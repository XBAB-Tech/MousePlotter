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
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

// Storage: linked list of fixed-size chunks.
// Overflow (>CHUNK_CAP samples) is rare, so we just malloc a new chunk; no copying.
#define CHUNK_CAP 131072
#define BATCH     64

struct sample { int dx, dy; int64_t t_ev, t_user; };
struct chunk  { struct sample data[CHUNK_CAP]; size_t sz; struct chunk *next; };

enum start_source  { START_NONE, START_SPACE, START_CLICK };

struct capture {
    enum start_source source;
    struct chunk *head, *tail;
    size_t count;
    int64_t total_dx, total_dy;
    int cur_dx, cur_dy;
    int discard_report;
    int grabbed;
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
    if (glob("/sys/devices/system/cpu/cpu[0-9]*/cpufreq/scaling_governor", 0, NULL, &gov_glob) != 0)
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
    gov_saved = NULL;
    gov_globbed = 0;
}

// Latency tuning is attempted once and kept for the interactive session, so a
// missing permission or facility is reported only during startup.
static int tuning_begin(void) {
    set_performance_governor();
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

static void tuning_end(int *qos_fd) {
    if (*qos_fd >= 0) {
        close(*qos_fd);
        *qos_fd = -1;
    }
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

static int choose_device(char *out, size_t sz) {
    glob_t g_id, g_path;
    int has_id   = (glob("/dev/input/by-id/*-event-mouse",   0, NULL, &g_id)   == 0);
    int has_path = (glob("/dev/input/by-path/*-event-mouse", 0, NULL, &g_path) == 0);

    if (!has_id && !has_path) {
        fputs("No mice found in /dev/input/by-id/ or /dev/input/by-path/.\n", stderr);
        return -1;
    }

    // Merge: all by-id entries, then by-path entries not already seen (by realpath).
    size_t cap = (has_id ? g_id.gl_pathc : 0) + (has_path ? g_path.gl_pathc : 0);
    char **paths        = malloc(cap * sizeof *paths);
    char (*reals)[PATH_MAX] = malloc(cap * sizeof *reals);
    if (!paths || !reals) { perror("malloc"); free(paths); free(reals); return -1; }
    size_t n = 0;

    if (has_id) {
        for (size_t i = 0; i < g_id.gl_pathc; i++) {
            paths[n] = g_id.gl_pathv[i];
            if (!realpath(g_id.gl_pathv[i], reals[n])) reals[n][0] = '\0';
            n++;
        }
    }
    if (has_path) {
        for (size_t i = 0; i < g_path.gl_pathc; i++) {
            char real[PATH_MAX];
            if (!realpath(g_path.gl_pathv[i], real)) real[0] = '\0';
            int dup = 0;
            for (size_t j = 0; j < n && !dup; j++)
                if (real[0] && strcmp(real, reals[j]) == 0) dup = 1;
            if (!dup) {
                paths[n] = g_path.gl_pathv[i];
                memcpy(reals[n], real, sizeof real);
                n++;
            }
        }
    }

    size_t sel = 0;
    if (n > 1) {
        fputs("Select device:\n", stderr);
        for (size_t i = 0; i < n; i++)
            fprintf(stderr, "  %zu) %s\n", i + 1, paths[i]);
        fputs("Enter number [1]: ", stderr);
        fflush(stderr);
        char line[32];
        if (fgets(line, sizeof line, stdin) && line[0] != '\n') {
            size_t choice = (size_t)atoi(line);
            if (choice >= 1 && choice <= n)
                sel = choice - 1;
        }
    }
    snprintf(out, sz, "%s", paths[sel]);
    free(paths); free(reals);
    if (has_id)   globfree(&g_id);
    if (has_path) globfree(&g_path);
    return 0;
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
    cap->head = cap->tail = NULL;
}

static struct chunk *new_chunk(struct capture *cap) {
    struct chunk *c = malloc(sizeof *c);
    if (!c) return NULL;
    memset(c, 0, sizeof *c); // pre-fault before the first sample arrives
    if (mlock(c, sizeof *c) != 0 && !cap->mlock_warned++)
        fprintf(stderr, "[warn] mlock chunk: %s "
                        "(recording continues unlocked; raise ulimit -l)\n",
                strerror(errno));
    return c;
}

static void show_ready(void) {
    fputs("Press Space or click and hold to record, Esc to quit.\n", stderr);
}

static void show_device(const char *mouse) {
    fprintf(stderr, "Mouse:  %s\n", mouse);
    fputs("Source: evdev kernel timestamps\n", stderr);
    show_ready();
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

static int reset_capture(struct capture *cap) {
    // Allocate first so a failed restart leaves the previous recording intact.
    struct chunk *first = new_chunk(cap);
    if (!first) {
        fputs("Out of memory. Previous recording retained.\n", stderr);
        return -1;
    }
    free_chunks(cap);
    cap->head = cap->tail = first;
    cap->source = START_NONE;
    cap->count = 0;
    cap->total_dx = cap->total_dy = 0;
    cap->cur_dx = cap->cur_dy = 0;
    cap->discard_report = 0;
    cap->storage_failed = 0;
    return 0;
}

static int start_recording(struct capture *cap, enum start_source source) {
    if (cap->source != START_NONE) return 0;
    if ((cap->count || !cap->head) && reset_capture(cap) != 0) return -1;
    cap->head->sz = 0;
    cap->head->next = NULL;
    cap->tail = cap->head;
    cap->count = 0;
    cap->total_dx = cap->total_dy = 0;
    cap->cur_dx = cap->cur_dy = 0;
    cap->discard_report = source == START_CLICK;
    cap->storage_failed = 0;
    cap->source = source;
    fputs("Recording\n", stderr);
    return 0;
}

static void stop_recording(struct capture *cap) {
    if (cap->source == START_NONE) return;
    cap->source = START_NONE;
    cap->cur_dx = cap->cur_dy = 0;
    show_result(cap);
}

static int append_sample(struct capture *cap, int64_t t_ev, int64_t t_user) {
    if (cap->tail->sz == CHUNK_CAP) {
        struct chunk *next = new_chunk(cap);
        if (!next) {
            cap->storage_failed = 1;
            return -1;
        }
        cap->tail->next = next;
        cap->tail = next;
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

// CSV in the MousePlotter / MouseTester format the web app imports; its parser
// reads columns 0-2, so a report plots eventTime and carries userTime unused.
static int write_csv(FILE *fp, const struct chunk *head) {
    fprintf(fp, "MousePlotter Linux logger (evdev)\n800\n"
                "xCount,yCount,eventTime (ms),userTime (ms)\n");
    // One monotonic origin preserves the event-to-userspace dispatch delay.
    int64_t t0_ev = head->data[0].t_ev;
    for (const struct chunk *c = head; c; c = c->next) {
        for (size_t j = 0; j < c->sz; j++) {
            int64_t de = c->data[j].t_ev   - t0_ev;
            int64_t du = c->data[j].t_user - t0_ev;
            fprintf(fp, "%d,%d,%" PRId64 ".%06" PRId64 ",%" PRId64 ".%06" PRId64 "\n",
                c->data[j].dx, c->data[j].dy,
                de / 1000000, de % 1000000,
                du / 1000000, du % 1000000);
        }
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

static int stamped_path(char *path, size_t path_size, const char *extension) {
    time_t now = time(NULL);
    struct tm tmv;
    if (!localtime_r(&now, &tmv)) return 0;
    char stem[48];
    if (!strftime(stem, sizeof stem, "MousePlotter-%Y%m%d-%H%M%S", &tmv)) return 0;
    int n = snprintf(path, path_size, "%s.%s", stem, extension);
    return n >= 0 && (size_t)n < path_size;
}

static int save_capture(const struct capture *cap, int html, int device_fd,
                        char *path, size_t path_size) {
    if (!stamped_path(path, path_size, html ? "html" : "csv")) {
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
        while (read(device_fd, discard, sizeof discard) > 0) {}
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
    if (ok && !write_csv(fp, cap->head)) ok = 0;
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
            "With no DEVICE, choose from the detected event-mouse devices.\n"
            "Controls: Space starts/stops; left-click and hold records.\n"
            "          Esc stops a recording or exits when idle; Ctrl+C exits.\n",
            argv0);
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

    // Avoid stdio read-ahead before the UI switches to one-byte terminal reads.
    setvbuf(stdin, NULL, _IONBF, 0);

    char dev_buf[PATH_MAX];
    if (device_arg) {
        if (snprintf(dev_buf, sizeof dev_buf, "%s", device_arg) >= (int)sizeof dev_buf) {
            fputs("Device path is too long.\n", stderr);
            return 2;
        }
    } else if (choose_device(dev_buf, sizeof dev_buf) != 0) {
        return 1;
    }
    const char *dev = dev_buf;

    // Keep the selected device open for the entire UI session. O_NONBLOCK lets
    // poll establish exact ready/recording boundaries without a read race;
    // O_CLOEXEC prevents a launched report browser from inheriting the fd.
    int fd = open(dev, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "open %s: %s\n", dev, strerror(errno));
        print_udev_rule(dev);
        return 1;
    }

    int one = 1;
    int grabbed = ioctl(fd, EVIOCGRAB, &one) == 0;
    if (!grabbed)
        fprintf(stderr, "[warn] EVIOCGRAB: %s (cursor will remain active)\n",
                strerror(errno));

    char mouse_name[256];
    if (ioctl(fd, EVIOCGNAME(sizeof mouse_name), mouse_name) < 0 || !mouse_name[0])
        snprintf(mouse_name, sizeof mouse_name, "%.*s",
                 (int)sizeof mouse_name - 1, dev);

    int clk = CLOCK_MONOTONIC;
    if (ioctl(fd, EVIOCSCLOCKID, &clk) < 0)
        fprintf(stderr, "[warn] EVIOCSCLOCKID: %s\n", strerror(errno));

    // Allocate and pre-fault the first chunk before mlockall so MCL_CURRENT
    // covers it. MCL_FUTURE is deliberately absent: it would charge every
    // overflow chunk against RLIMIT_MEMLOCK (8 MiB unprivileged, ~2 chunks)
    // and kill long recordings; overflow chunks are mlock()ed best-effort.
    struct chunk *head = malloc(sizeof *head);
    if (!head) {
        perror("malloc");
        if (grabbed) {
            int zero = 0;
            ioctl(fd, EVIOCGRAB, &zero);
        }
        close(fd);
        return 1;
    }
    memset(head, 0, sizeof *head);
    struct capture cap = {
        .head = head,
        .tail = head,
        .grabbed = grabbed,
    };

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
        goto cleanup;
    }
    show_device(mouse_name);

    int stdin_open = 1;
    int quit = 0;
    struct input_event evbuf[BATCH];
    while (!quit && !g_intr) {
        struct pollfd pfds[2] = {
            { .fd = fd, .events = POLLIN },
            { .fd = stdin_open ? STDIN_FILENO : -1, .events = POLLIN },
        };
        int ready = poll(pfds, 2, -1);
        if (ready < 0) {
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
                        if (e->value == 1 && cap.source == START_NONE)
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
                } else if (key == 3) {
                    quit = 1;
                } else if (key == ' ') {
                    if (cap.source == START_NONE)
                        start_recording(&cap, START_SPACE);
                    else if (cap.source == START_SPACE)
                        stop_recording(&cap);
                } else if ((key == 'c' || key == 'C') &&
                           cap.source == START_NONE && cap.count) {
                    char path[PATH_MAX];
                    save_capture(&cap, 0, fd, path, sizeof path);
                    show_actions(&cap);
                } else if ((key == 'h' || key == 'H') &&
                           cap.source == START_NONE && cap.count) {
                    char path[PATH_MAX];
                    if (save_capture(&cap, 1, fd, path, sizeof path)) {
                        open_report(path);
                    }
                    show_actions(&cap);
                }
            }
        }
    }

cleanup:
    terminal_restore();
    if (g_intr) fputs("\nInterrupted; exiting.\n", stderr);
    if (cap.grabbed) {
        int zero = 0;
        ioctl(fd, EVIOCGRAB, &zero);
    }
    tuning_end(&qos_fd);
    free_chunks(&cap);
    close(fd);
    return status;
}
