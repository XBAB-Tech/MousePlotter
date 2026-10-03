// SPDX-FileCopyrightText: 2026 XBAB Tech, LLC
// SPDX-License-Identifier: MIT

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <grp.h>
#include <limits.h>
#include <inttypes.h>
#include <poll.h>
#include <pwd.h>
#include <stdint.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/input.h>
#include <linux/kd.h>
#include <linux/major.h>
#include <linux/vt.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

// Storage: a linked list of fixed-size chunks, kept for the session and reused
// by each recording, plus a spare that prepare_spare() readies between reads.
#define CHUNK_CAP 131072
#define BATCH     64

struct sample { int dx, dy; int64_t t_ev, t_user; };
struct chunk  { struct sample data[CHUNK_CAP]; size_t sz; struct chunk *next; };

enum start_source  { START_NONE, START_SPACE, START_CLICK };

struct capture {
    const char *name; // the mouse's, for the plot title
    enum start_source source;
    struct chunk *head, *tail, *spare;
    size_t spare_ready; // bytes of spare faulted in
    size_t count;
    int64_t total_dx, total_dy;
    int cur_dx, cur_dy;
    int discard_report;
    int fd;
    int grabbed;
    int grab_warned;
    int storage_failed;
    int mlock_warned;
};

// HTML report template halves, embedded verbatim by blob.S. A report file is
// head + the exact CSV byte stream + tail: the CSV lands inside a
// <script type="application/csv"> data block that the inlined web app plots
// on load. Built by tools/build_report_template.py.
extern const char report_head[], report_head_end[];
extern const char report_tail[], report_tail_end[];

static volatile sig_atomic_t g_intr = 0;
static void on_signal(int sig) { (void)sig; g_intr = 1; }

// What Esc does when idle: back to the mouse list, unless a DEVICE was given.
static const char *g_esc_action = "quit";

static struct termios g_term_saved;
static int g_term_saved_valid;
static int g_term_raw;

static int terminal_raw(void) {
    if (!isatty(STDIN_FILENO)) return 0;
    if (!g_term_saved_valid) {
        if (tcgetattr(STDIN_FILENO, &g_term_saved) != 0) return -1;
        g_term_saved_valid = 1;
    }
    struct termios raw = g_term_saved;
    raw.c_lflag &= (tcflag_t)~(ICANON | ECHO);
    raw.c_iflag &= (tcflag_t)~(ICRNL | IXON);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) return -1;
    g_term_raw = 1;
    return 0;
}

static void terminal_restore(void) {
    if (!g_term_raw) return;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_term_saved);
    g_term_raw = 0;
}

// Governor changes are sticky (unlike the PM QoS fd), so save and restore them.
static glob_t gov_glob;
static int    gov_globbed;
static char (*gov_saved)[64];

static void set_performance_governor(void) {
    if (glob("/sys/devices/system/cpu/cpufreq/policy*/scaling_governor", 0, NULL, &gov_glob) != 0)
        return; // no cpufreq support
    gov_globbed = 1;
    gov_saved = calloc(gov_glob.gl_pathc, sizeof *gov_saved);
    if (!gov_saved) return;
    size_t changed = 0;
    int warned = 0;
    for (size_t i = 0; i < gov_glob.gl_pathc; i++) {
        FILE *f = fopen(gov_glob.gl_pathv[i], "r");
        if (!f) continue;
        if (fscanf(f, "%63s", gov_saved[i]) != 1) gov_saved[i][0] = '\0';
        fclose(f);
        if (!gov_saved[i][0] || strcmp(gov_saved[i], "performance") == 0) {
            gov_saved[i][0] = '\0'; // nothing to change or restore
            continue;
        }
        f = fopen(gov_glob.gl_pathv[i], "w");
        if (!f) {
            if (!warned++)
                fprintf(stderr, "[warn] set governor: %s (run as root to set performance governor)\n",
                        strerror(errno));
            gov_saved[i][0] = '\0';
            continue;
        }
        fputs("performance", f);
        fclose(f);
        changed++;
    }
    if (changed)
        fprintf(stderr, "Governor: performance on %zu polic%s "
                        "(restored on exit)\n",
                changed, changed == 1 ? "y" : "ies");
}

static void restore_governor(void) {
    if (!gov_globbed) return;
    for (size_t i = 0; gov_saved && i < gov_glob.gl_pathc; i++) {
        if (!gov_saved[i][0]) continue;
        FILE *f = fopen(gov_glob.gl_pathv[i], "w");
        if (!f) continue;
        fputs(gov_saved[i], f);
        fclose(f);
    }
    free(gov_saved);
    globfree(&gov_glob);
}

// Latency tuning is attempted once and kept for the interactive session, so a
// missing permission or facility is reported only during startup.
static int tuning_begin(void) {
    set_performance_governor();
    // A zero latency target leaves only the POLL idle state usable. Systems
    // that choose the CPU idle states themselves set this to keep their choice.
    if (getenv("MOUSEPLOTTER_NO_PM_QOS"))
        return -1;
    int qos_fd = open("/dev/cpu_dma_latency", O_WRONLY | O_CLOEXEC);
    if (qos_fd >= 0) {
        int32_t target = 0;
        if (write(qos_fd, &target, sizeof target) == sizeof target)
            return qos_fd;
        fprintf(stderr, "[warn] cpu_dma_latency write: %s\n", strerror(errno));
        close(qos_fd);
    } else {
        fprintf(stderr, "[warn] open /dev/cpu_dma_latency: %s "
                        "(run as root to limit C-state latency)\n", strerror(errno));
    }
    return -1;
}

static void tuning_end(int qos_fd) {
    if (qos_fd >= 0) close(qos_fd);
    restore_governor();
}

static int64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void print_udev_rule(const char *dev) {
    char real[PATH_MAX];
    if (!realpath(dev, real)) return;
    const char *base = strrchr(real, '/');
    if (!base) return;
    base++;

    char vpath[PATH_MAX], ppath[PATH_MAX];
    snprintf(vpath, sizeof vpath, "/sys/class/input/%s/device/id/vendor", base);
    snprintf(ppath, sizeof ppath, "/sys/class/input/%s/device/id/product", base);

    char vendor[16] = "", product[16] = "";
    FILE *f;
    if ((f = fopen(vpath, "r"))) { if (fscanf(f, "%15s", vendor)  != 1) vendor[0]  = '\0'; fclose(f); }
    if ((f = fopen(ppath, "r"))) { if (fscanf(f, "%15s", product) != 1) product[0] = '\0'; fclose(f); }

    fputs("Try sudo or add a udev rule.\n", stderr);
    if (vendor[0] && product[0]) {
        fprintf(stderr,
            "Add a udev rule to /etc/udev/rules.d/70-mouse.rules:\n"
            "  SUBSYSTEM==\"input\", ATTRS{idVendor}==\"%s\", ATTRS{idProduct}==\"%s\", TAG+=\"uaccess\"\n"
            "Then reload: sudo udevadm control --reload && sudo udevadm trigger\n",
            vendor, product);
    }
}

// Lists the event-mouse devices and waits for a button press on one of them,
// or its number on the keyboard. Returns 0 with the chosen device's path in
// out, 1 on Esc or an interrupt, or -1 if there are no mice.
static int choose_device(char *out, size_t sz) {
    glob_t g;
    int found = glob("/dev/input/by-id/*-event-mouse", 0, NULL, &g) == 0;
    if (glob("/dev/input/by-path/*-event-mouse", found ? GLOB_APPEND : 0, NULL, &g) == 0)
        found = 1;
    if (!found) {
        fputs("No mice found in /dev/input/by-id/ or /dev/input/by-path/.\n", stderr);
        return -1;
    }

    int result = -1;
    char **paths = malloc(g.gl_pathc * sizeof *paths);
    dev_t *ids = malloc(g.gl_pathc * sizeof *ids);
    struct pollfd *pfds = malloc((g.gl_pathc + 1) * sizeof *pfds);
    if (!paths || !ids || !pfds) {
        perror("malloc");
        goto done;
    }

    // A mouse usually has a by-id and a by-path name: list it once, under the first.
    size_t n = 0;
    for (size_t i = 0; i < g.gl_pathc; i++) {
        struct stat st;
        if (stat(g.gl_pathv[i], &st) != 0) continue;
        size_t j = 0;
        while (j < n && ids[j] != st.st_rdev) j++;
        if (j == n) {
            ids[n] = st.st_rdev;
            paths[n++] = g.gl_pathv[i];
        }
    }

    fputs("\nMice:\n", stderr);
    for (size_t i = 0; i < n; i++)
        fprintf(stderr, "  %zu) %s\n", i + 1, paths[i]);
    fputs("Click with the mouse to test or press its number (Enter: 1), Esc to quit.\n",
          stderr);

    // Mice that can't be opened here can still be chosen by number.
    pfds[0] = (struct pollfd){ .fd = STDIN_FILENO, .events = POLLIN };
    for (size_t i = 0; i < n; i++)
        pfds[i + 1] = (struct pollfd){
            .fd = open(paths[i], O_RDONLY | O_NONBLOCK | O_CLOEXEC),
            .events = POLLIN,
        };

    size_t sel = 0, typed = 0;
    result = 2; // still waiting
    while (result == 2) {
        if (poll(pfds, n + 1, -1) < 0) {
            if (g_intr || errno != EINTR) result = 1;
            continue;
        }
        if (pfds[0].revents) {
            unsigned char key;
            if (read(STDIN_FILENO, &key, 1) != 1 || key == 27) {
                result = 1;
            } else if (key == '\r' || key == '\n') {
                sel = typed ? typed - 1 : 0;
                result = 0;
            } else if (key >= '0' && key <= '9') {
                // Pick as soon as another digit can't make a valid number.
                typed = typed * 10 + (size_t)(key - '0');
                if (typed == 0 || typed > n) {
                    typed = 0;
                } else if (typed * 10 > n) {
                    sel = typed - 1;
                    result = 0;
                }
            }
        }
        for (size_t i = 0; i < n && result == 2; i++) {
            struct pollfd *p = &pfds[i + 1];
            if (!p->revents) continue;
            struct input_event ev[BATCH];
            ssize_t got;
            while ((got = read(p->fd, ev, sizeof ev)) > 0)
                for (size_t k = 0; k < (size_t)got / sizeof *ev; k++)
                    if (ev[k].type == EV_KEY && ev[k].value == 1 &&
                        ev[k].code >= BTN_MOUSE && ev[k].code < BTN_JOYSTICK) {
                        sel = i;
                        result = 0;
                    }
            if (got < 0 && errno != EAGAIN) {
                close(p->fd); // unplugged: stop watching it
                p->fd = -1;
            }
        }
    }
    if (result == 0)
        snprintf(out, sz, "%s", paths[sel]);
    for (size_t i = 1; i <= n; i++)
        if (pfds[i].fd >= 0) close(pfds[i].fd);

done:
    free(paths);
    free(ids);
    free(pfds);
    globfree(&g);
    return result;
}

// --------------------------------------------------------------------------
// Recording control and evdev input
// --------------------------------------------------------------------------

static void free_chunks(struct capture *cap) {
    struct chunk *c = cap->head;
    while (c) {
        struct chunk *next = c->next;
        free(c);
        c = next;
    }
    free(cap->spare);
}

// Faults in and locks the spare chunk's next page. Called once per loop pass,
// after the batch is timestamped, it readies the next chunk long before the
// current one fills, so a recording never stops for allocation or page faults.
static void prepare_spare(struct capture *cap) {
    if (!cap->spare) {
        cap->spare = malloc(sizeof *cap->spare);
        cap->spare_ready = 0;
        if (!cap->spare) return;
    }
    size_t left = sizeof *cap->spare - cap->spare_ready;
    if (!left) return;
    size_t len = left < 4096 ? left : 4096;
    char *page = (char *)cap->spare + cap->spare_ready;
    if (mlock(page, len) != 0) { // mlock also faults the page in
        if (!cap->mlock_warned++)
            fprintf(stderr, "[warn] mlock: %s "
                            "(recording continues unlocked; raise ulimit -l)\n",
                    strerror(errno));
        memset(page, 0, len);
    }
    cap->spare_ready += len;
}

// Returns the spare chunk, fully faulted in, or NULL if out of memory.
static struct chunk *take_spare(struct capture *cap) {
    do prepare_spare(cap);
    while (cap->spare && cap->spare_ready < sizeof *cap->spare);
    struct chunk *c = cap->spare;
    cap->spare = NULL;
    if (c) c->next = NULL;
    return c;
}

// The device is grabbed only while recording, so between recordings the
// cursor stays usable, e.g. in the report viewer.
static void set_grab(struct capture *cap, int on) {
    if (cap->grabbed == on) return;
    // EVIOCGRAB reads the argument value itself: any non-NULL pointer grabs,
    // so ungrabbing must pass NULL, not a pointer to zero.
    int ok = ioctl(cap->fd, EVIOCGRAB, on ? (void *)1 : NULL) == 0;
    if (on && !ok) {
        if (!cap->grab_warned++)
            fprintf(stderr, "[warn] EVIOCGRAB: %s (cursor will remain active)\n",
                    strerror(errno));
        return;
    }
    cap->grabbed = on;
}

// Clicks start a recording only on a Linux text console that is on screen.
// Between recordings the device is ungrabbed, so under a display server the
// click would also reach the desktop, which then never sees the release
// (grabbed mid-recording) and keeps the button held. The same check ignores
// clicks while a report viewer has taken over the console's screen.
static int console_in_front(void) {
    int mode;
    struct vt_stat vs;
    struct stat st;
    if (ioctl(STDIN_FILENO, KDGETMODE, &mode) != 0 || mode != KD_TEXT) return 0;
    if (ioctl(STDIN_FILENO, VT_GETSTATE, &vs) != 0) return 0;
    if (fstat(STDIN_FILENO, &st) != 0 || major(st.st_rdev) != TTY_MAJOR) return 0;
    return minor(st.st_rdev) == vs.v_active;
}

static void show_ready(void) {
    fprintf(stderr, "Press Space %sto record, Esc to %s.\n",
            console_in_front() ? "or click and hold " : "", g_esc_action);
}

static void show_actions(const struct capture *cap) {
    if (cap->count > 0)
        fputs("Press H to save and view HTML, C to save CSV.\n", stderr);
    show_ready();
}

static void show_result(const struct capture *cap) {
    fprintf(stderr, "Events:   %zu\n"
                    "Total X:  %" PRId64 "\n"
                    "Total Y:  %" PRId64 "\n",
            cap->count, cap->total_dx, cap->total_dy);
    if (cap->storage_failed)
        fputs("Recording stopped: out of memory. Captured samples retained.\n",
              stderr);
    else if (cap->count == 0)
        fputs("No samples captured.\n", stderr);
    show_actions(cap);
}

static void start_recording(struct capture *cap, enum start_source source) {
    if (cap->source != START_NONE) return;
    // Overwrite the previous recording in the already faulted-in chunks.
    cap->tail = cap->head;
    cap->head->sz = 0;
    cap->count = 0;
    cap->total_dx = cap->total_dy = 0;
    cap->cur_dx = cap->cur_dy = 0;
    cap->storage_failed = 0;
    cap->discard_report = source == START_CLICK;
    set_grab(cap, 1);
    cap->source = source;
    fputs("Recording\n", stderr);
}

static void stop_recording(struct capture *cap) {
    if (cap->source == START_NONE) return;
    cap->source = START_NONE;
    set_grab(cap, 0);
    cap->cur_dx = cap->cur_dy = 0;
    show_result(cap);
}

static int append_sample(struct capture *cap, int64_t t_ev, int64_t t_user) {
    if (cap->tail->sz == CHUNK_CAP) {
        if (!cap->tail->next)
            cap->tail->next = take_spare(cap); // normally ready already
        if (!cap->tail->next) {
            cap->storage_failed = 1;
            return -1;
        }
        cap->tail = cap->tail->next;
        cap->tail->sz = 0;
    }
    cap->tail->data[cap->tail->sz++] = (struct sample){
        .dx = cap->cur_dx,
        .dy = cap->cur_dy,
        .t_ev = t_ev,
        .t_user = t_user,
    };
    cap->count++;
    cap->total_dx += cap->cur_dx;
    cap->total_dy += cap->cur_dy;
    return 0;
}

// CSV in the MousePlotter / MouseTester format the web app imports: plot
// title, DPI, column names, rows. It plots eventTime, or userTime in its user
// space timestamp view.
static int write_csv(FILE *fp, const struct capture *cap) {
    fprintf(fp, "%s (MousePlotter Linux)\n800\n"
                "xCount,yCount,eventTime (ms),userTime (ms)\n", cap->name);
    // One monotonic origin preserves the event-to-userspace dispatch delay.
    int64_t t0_ev = cap->head->data[0].t_ev;
    for (const struct chunk *c = cap->head;; c = c->next) {
        for (size_t j = 0; j < c->sz; j++) {
            int64_t de = c->data[j].t_ev   - t0_ev;
            int64_t du = c->data[j].t_user - t0_ev;
            fprintf(fp, "%d,%d,%" PRId64 ".%06" PRId64 ",%" PRId64 ".%06" PRId64 "\n",
                c->data[j].dx, c->data[j].dy,
                de / 1000000, de % 1000000,
                du / 1000000, du % 1000000);
        }
        if (c == cap->tail) break; // later chunks hold older recordings
    }
    return !ferror(fp);
}

// Under sudo this process runs as root, so everything it creates would come
// out root-owned and the invoking user couldn't delete their own logs.
static void chown_to_invoker(const char *path) {
    const char *su = getenv("SUDO_UID");
    const char *sg = getenv("SUDO_GID");
    if (geteuid() != 0 || !su || !sg) return;
    if (chown(path, (uid_t)strtoul(su, NULL, 10), (gid_t)strtoul(sg, NULL, 10)) != 0)
        fprintf(stderr, "[warn] chown %s: %s\n", path, strerror(errno));
}

// Open the report in the user's browser. Under sudo a root xdg-open either
// fails (DISPLAY/XDG_RUNTIME_DIR are not in the sudo env) or runs the browser
// as root, so the child drops back to the invoking user and rebuilds the two
// environment variables xdg-open needs. SIGCHLD is ignored, so no zombie.
static void open_report(const char *path) {
    signal(SIGCHLD, SIG_IGN);
    if (fork() != 0) return; // parent (or failed fork): nothing more to do

    // Detach from the terminal: fds 0-2 onto /dev/null so browser chatter
    // stays out of it, a new session so closing it can't HUP the browser.
    // The real stderr survives on a CLOEXEC fd, reachable by the exec-failed
    // warning below but never by the browser.
    int err_fd = fcntl(STDERR_FILENO, F_DUPFD_CLOEXEC, 3);
    setsid();
    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
        dup2(devnull, STDIN_FILENO);
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
        if (devnull > STDERR_FILENO) close(devnull);
    }

    if (geteuid() == 0) {
        // Root with no known non-root target (bare `su`, a root cron job,
        // SUDO_UID/GID stripped from the environment) has no safe user to
        // drop to. Refuse rather than ever exec the browser as root.
        const char *su = getenv("SUDO_UID");
        const char *sg = getenv("SUDO_GID");
        if (!su || !sg) {
            if (err_fd >= 0)
                dprintf(err_fd, "[warn] running as root with no SUDO_UID/SUDO_GID; "
                                "not opening the browser as root\n");
            _exit(127);
        }
        uid_t uid = (uid_t)strtoul(su, NULL, 10);
        gid_t gid = (gid_t)strtoul(sg, NULL, 10);
        const struct passwd *pw = getpwuid(uid);
        if (pw) initgroups(pw->pw_name, gid); // best effort, needs root
        if (setgid(gid) != 0 || setuid(uid) != 0)
            _exit(127); // never launch the browser as root
        // setuid() from euid 0 drops the real/effective/saved uid together,
        // so this is a permanent, unrecoverable drop -- not just for exec.
        if (geteuid() == 0 || getuid() == 0)
            _exit(127); // paranoia: refuse to proceed if root wasn't shed
        if (pw && pw->pw_dir) setenv("HOME", pw->pw_dir, 1);
        char runtime[64];
        snprintf(runtime, sizeof runtime, "/run/user/%u", (unsigned)uid);
        setenv("XDG_RUNTIME_DIR", runtime, 1);
    }
    execlp("xdg-open", "xdg-open", path, (char *)NULL);
    if (err_fd >= 0)
        dprintf(err_fd, "[warn] exec xdg-open: %s\n", strerror(errno));
    _exit(127);
}

// Default file name: the mouse's name and the time, NAME-YYYYMMDD-HHMMSS.ext.
// Characters Windows or Linux don't allow in file names become "_", and a long
// name is cut after 64 bytes, at a character boundary.
static int stamped_path(char *path, size_t path_size, const char *name,
                        const char *extension) {
    char stem[80];
    size_t n = 0;
    for (const char *p = name; *p && n < sizeof stem - 1 &&
                               (n < 64 || ((unsigned char)*p & 0xC0) == 0x80); p++)
        stem[n++] = strchr("/\\:*?\"<>|", *p) ? '_' : *p;
    stem[n] = '\0';
    time_t now = time(NULL);
    struct tm tmv;
    char when[16];
    if (!localtime_r(&now, &tmv) || !strftime(when, sizeof when, "%Y%m%d-%H%M%S", &tmv))
        return 0;
    int len = snprintf(path, path_size, "%s-%s.%s", stem, when, extension);
    return len >= 0 && (size_t)len < path_size;
}

static int save_capture(const struct capture *cap, int html, char *path, size_t path_size) {
    if (!stamped_path(path, path_size, cap->name, html ? "html" : "csv")) {
        fputs("Could not create output filename. Recording retained.\n", stderr);
        return 0;
    }

    if (!html && isatty(STDIN_FILENO)) {
        terminal_restore();
        fprintf(stderr, "Save CSV as [%s]: ", path);
        fflush(stderr);
        char line[PATH_MAX];
        char *got_line = fgets(line, sizeof line, stdin);
        if (terminal_raw() != 0)
            fprintf(stderr, "[warn] could not restore raw terminal input: %s\n",
                    strerror(errno));
        struct input_event discard[BATCH];
        while (read(cap->fd, discard, sizeof discard) > 0) {}
        if (!got_line || g_intr) return 0;
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] && snprintf(path, path_size, "%s", line) >= (int)path_size) {
            fputs("Output path is too long. Recording retained.\n", stderr);
            return 0;
        }
    }

    FILE *fp = fopen(path, "w");
    if (!fp) {
        fprintf(stderr, "Could not open %s for writing: %s. Recording retained.\n",
                path, strerror(errno));
        return 0;
    }
    int ok = 1;
    size_t head_size = (size_t)(report_head_end - report_head);
    size_t tail_size = (size_t)(report_tail_end - report_tail);
    if (html && fwrite(report_head, 1, head_size, fp) != head_size) ok = 0;
    if (ok && !write_csv(fp, cap)) ok = 0;
    if (html && ok && fwrite(report_tail, 1, tail_size, fp) != tail_size) ok = 0;
    if (fclose(fp) != 0) ok = 0;
    if (!ok) {
        fprintf(stderr, "Write failed for %s; the file may be incomplete. "
                        "Recording retained.\n", path);
        return 0;
    }
    chown_to_invoker(path);
    fprintf(stderr, "Data saved to %s\n", path);
    return 1;
}

static void usage(FILE *fp, const char *argv0) {
    fprintf(fp,
            "Usage: %s [-d DEVICE | --device DEVICE] [DEVICE]\n"
            "\n"
            "With no DEVICE, pick the mouse from a list by clicking with it or\n"
            "pressing its number.\n"
            "Controls: Space starts/stops. On a Linux text console, left-click\n"
            "          and hold also records. Esc stops a recording; when idle\n"
            "          it returns to the mouse list, where (or with DEVICE\n"
            "          given) it exits. Ctrl+C exits.\n"
            "\n"
            "Set MOUSEPLOTTER_NO_PM_QOS to skip the /dev/cpu_dma_latency request,\n"
            "which limits the CPU idle states to POLL while running.\n",
            argv0);
}

// Records from one mouse until Esc while idle, an interrupt or a device error.
// Returns nonzero on errors.
static int run_session(const char *dev) {
    // Keep the device open for the whole session. O_NONBLOCK lets poll
    // establish exact ready/recording boundaries without a read race;
    // O_CLOEXEC prevents a launched report browser from inheriting the fd.
    int fd = open(dev, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "open %s: %s\n", dev, strerror(errno));
        print_udev_rule(dev);
        return 1;
    }

    char mouse_name[256];
    if (ioctl(fd, EVIOCGNAME(sizeof mouse_name), mouse_name) < 0 || !mouse_name[0])
        snprintf(mouse_name, sizeof mouse_name, "%.*s",
                 (int)sizeof mouse_name - 1, dev);
    // The device supplies its name: keep control characters out of the
    // terminal and the CSV, and "<" out of the HTML report's <script> block.
    for (char *p = mouse_name; *p; p++)
        if ((unsigned char)*p < ' ' || *p == '<') *p = '?';

    int clk = CLOCK_MONOTONIC;
    if (ioctl(fd, EVIOCSCLOCKID, &clk) < 0)
        fprintf(stderr, "[warn] EVIOCSCLOCKID: %s\n", strerror(errno));

    struct capture cap = { .name = mouse_name, .fd = fd };
    cap.head = cap.tail = take_spare(&cap);
    int status = !cap.head;
    if (status) {
        fputs("Out of memory.\n", stderr);
    } else {
        fprintf(stderr, "Mouse:  %s\nSource: evdev kernel timestamps\n", mouse_name);
        show_ready();
    }

    int quit = status;
    int stdin_open = 1;
    struct input_event evbuf[BATCH];
    while (!quit && !g_intr) {
        struct pollfd pfds[2] = {
            { .fd = fd, .events = POLLIN },
            { .fd = stdin_open ? STDIN_FILENO : -1, .events = POLLIN },
        };
        if (poll(pfds, 2, -1) < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "poll: %s\n", strerror(errno));
            status = 1;
            break;
        }

        if (pfds[0].revents & (POLLIN | POLLERR | POLLHUP)) {
            // Drain evdev before handling a simultaneous SPACE key, so queued
            // pre-boundary motion is not assigned to the new recording.
            for (;;) {
                ssize_t n = read(fd, evbuf, sizeof evbuf);
                if (n < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    if (errno == EINTR && !g_intr) continue;
                    if (!g_intr) fprintf(stderr, "read device: %s\n", strerror(errno));
                    status = 1;
                    quit = 1;
                    break;
                }
                if (n == 0) {
                    fputs("Mouse device disconnected.\n", stderr);
                    status = 1;
                    quit = 1;
                    break;
                }
                int64_t t_read = now_ns();
                size_t count = (size_t)n / sizeof *evbuf;
                for (size_t i = 0; i < count; i++) {
                    struct input_event *e = &evbuf[i];
                    if (e->type == EV_KEY && e->code == BTN_LEFT) {
                        if (e->value == 1 && cap.source == START_NONE &&
                            console_in_front())
                            start_recording(&cap, START_CLICK);
                        else if (e->value == 0 && cap.source == START_CLICK) {
                            cap.discard_report = 1;
                            stop_recording(&cap);
                        }
                    }
                    if (cap.source != START_NONE && e->type == EV_REL) {
                        if (e->code == REL_X) cap.cur_dx += e->value;
                        if (e->code == REL_Y) cap.cur_dy += e->value;
                    }
                    if (e->type == EV_SYN && e->code == SYN_REPORT) {
                        if (cap.discard_report) {
                            cap.discard_report = 0;
                        } else if (cap.source != START_NONE) {
                            int64_t t_ev = (int64_t)e->time.tv_sec * 1000000000LL
                                         + (int64_t)e->time.tv_usec * 1000;
                            if (append_sample(&cap, t_ev, t_read) != 0) {
                                stop_recording(&cap);
                            }
                        }
                        cap.cur_dx = cap.cur_dy = 0;
                    }
                }
            }
        }
        prepare_spare(&cap);
        if (g_intr) break;

        if (stdin_open && pfds[1].revents & (POLLIN | POLLHUP)) {
            unsigned char key;
            ssize_t n = read(STDIN_FILENO, &key, 1);
            if (n < 0) {
                if (errno != EINTR) {
                    fprintf(stderr, "read stdin: %s\n", strerror(errno));
                    status = 1;
                    break;
                }
            } else if (n == 0) {
                stdin_open = 0;
            } else {
                if (key == 27) {
                    if (cap.source != START_NONE)
                        stop_recording(&cap);
                    else
                        quit = 1;
                } else if (key == ' ') {
                    if (cap.source == START_NONE)
                        start_recording(&cap, START_SPACE);
                    else if (cap.source == START_SPACE)
                        stop_recording(&cap);
                } else if ((key == 'c' || key == 'C') &&
                           cap.source == START_NONE && cap.count) {
                    char path[PATH_MAX];
                    save_capture(&cap, 0, path, sizeof path);
                    show_actions(&cap);
                } else if ((key == 'h' || key == 'H') &&
                           cap.source == START_NONE && cap.count) {
                    char path[PATH_MAX];
                    if (save_capture(&cap, 1, path, sizeof path)) {
                        open_report(path);
                    }
                    show_actions(&cap);
                }
            }
        }
    }

    set_grab(&cap, 0);
    free_chunks(&cap);
    close(fd);
    return status;
}

int main(int argc, char **argv) {
    const char *device_arg = NULL;
    if (argc == 2 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
        usage(stdout, argv[0]);
        return 0;
    }
    if (argc == 2 && (!strcmp(argv[1], "-d") || !strcmp(argv[1], "--device"))) {
        usage(stderr, argv[0]);
        return 2;
    }
    if (argc == 2) device_arg = argv[1];
    else if (argc == 3 && (!strcmp(argv[1], "-d") || !strcmp(argv[1], "--device")))
        device_arg = argv[2];
    else if (argc != 1) {
        usage(stderr, argv[0]);
        return 2;
    }

    // The CSV filename prompt reads a line with fgets: keep stdio from reading
    // ahead into the one-byte key reads that follow.
    setvbuf(stdin, NULL, _IONBF, 0);

    // Lock what is mapped now (code, libraries, stack). MCL_FUTURE is
    // deliberately absent: it would charge every chunk against RLIMIT_MEMLOCK
    // (8 MiB unprivileged, ~2 chunks) and kill long recordings; chunks are
    // mlock()ed best-effort instead.
    if (mlockall(MCL_CURRENT) < 0)
        fprintf(stderr, "[warn] mlockall: %s\n", strerror(errno));

    // Catch Ctrl+C/SIGTERM (no SA_RESTART: read() returns EINTR) so governors get restored.
    struct sigaction sa = { .sa_handler = on_signal };
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    int status = 0;
    int qos_fd = tuning_begin();
    if (terminal_raw() != 0 && isatty(STDIN_FILENO)) {
        fprintf(stderr, "Could not enable raw terminal input: %s\n", strerror(errno));
        status = 1;
    } else if (device_arg) {
        status = run_session(device_arg);
    } else {
        g_esc_action = "re-select mouse";
        char dev[PATH_MAX];
        int chosen = 0;
        while (!g_intr && (chosen = choose_device(dev, sizeof dev)) == 0)
            run_session(dev);
        status = chosen < 0;
    }

    terminal_restore();
    if (g_intr) fputs("\nInterrupted; exiting.\n", stderr);
    tuning_end(qos_fd);
    return status;
}
