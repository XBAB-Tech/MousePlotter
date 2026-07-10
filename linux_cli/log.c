// SPDX-FileCopyrightText: 2026 XBAB Tech, LLC
// SPDX-License-Identifier: MIT

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <grp.h>
#include <limits.h>
#include <inttypes.h>
#include <pwd.h>
#include <stdint.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

// Storage: linked list of fixed-size chunks.
// Overflow (>CHUNK_CAP samples) is rare, so we just malloc a new chunk; no copying.
#define CHUNK_CAP 131072
#define BATCH     64

struct sample { int dx, dy; int64_t t_ev, t_user; };
struct chunk  { struct sample data[CHUNK_CAP]; size_t sz; struct chunk *next; };

// HTML report template halves, embedded verbatim by blob.S. A report file is
// head + the exact CSV byte stream + tail: the CSV lands inside a
// <script type="application/csv"> data block that the inlined web app plots
// on load. Built by tools/build_report_template.py.
extern const char report_head[], report_head_end[];
extern const char report_tail[], report_tail_end[];

static volatile sig_atomic_t g_intr = 0;
static void on_signal(int sig) { (void)sig; g_intr = 1; }

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
        fprintf(stderr, "Governor: performance on %zu polic%s (restored on exit)\n",
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
        fprintf(stderr, "Select device:\n\n");
        for (size_t i = 0; i < n; i++)
            fprintf(stderr, "  %zu) %s\n", i + 1, paths[i]);
        fprintf(stderr, "\nEnter number [1]: ");
        fflush(stderr);
        char line[32];
        if (fgets(line, sizeof line, stdin) && line[0] != '\n') {
            size_t choice = (size_t)atoi(line);
            if (choice >= 1 && choice <= n)
                sel = choice - 1;
        }
    } else {
        fprintf(stderr, "Device: %s\n", paths[0]);
    }
    snprintf(out, sz, "%s", paths[sel]);
    free(paths); free(reals);
    if (has_id)   globfree(&g_id);
    if (has_path) globfree(&g_path);
    return 0;
}

// CSV in the MousePlotter / MouseTester format the web app imports; its parser
// reads columns 0-2, so a report plots eventTime and carries userTime unused.
static void write_csv(FILE *fp, const struct chunk *head) {
    fprintf(fp, "MousePlotter CLI logger\n800\nxCount,yCount,eventTime (ms),userTime (ms)\n");
    int64_t t0_ev   = head->data[0].t_ev;
    int64_t t0_user = head->data[0].t_user;
    for (const struct chunk *c = head; c; c = c->next) {
        for (size_t j = 0; j < c->sz; j++) {
            int64_t de = c->data[j].t_ev   - t0_ev;
            int64_t du = c->data[j].t_user - t0_user;
            fprintf(fp, "%d,%d,%" PRId64 ".%06" PRId64 ",%" PRId64 ".%06" PRId64 "\n",
                c->data[j].dx, c->data[j].dy,
                de / 1000000, de % 1000000,
                du / 1000000, du % 1000000);
        }
    }
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

int main(void) {
    char html_path[64] = ""; // set when a report should open after cleanup
    char dev_buf[PATH_MAX];
    if (choose_device(dev_buf, sizeof dev_buf) != 0) return 1;
    const char *dev = dev_buf;

    int fd = open(dev, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "open %s: %s\n", dev, strerror(errno));
        print_udev_rule(dev);
        return 1;
    }

    int clk = CLOCK_MONOTONIC;
    if (ioctl(fd, EVIOCSCLOCKID, &clk) < 0)
        fprintf(stderr, "[warn] EVIOCSCLOCKID: %s\n", strerror(errno));

    int one = 1;
    if (ioctl(fd, EVIOCGRAB, &one) < 0)
        fprintf(stderr, "[warn] EVIOCGRAB: %s\n", strerror(errno));

    if (mlockall(MCL_CURRENT | MCL_FUTURE) < 0)
        fprintf(stderr, "[warn] mlockall: %s\n", strerror(errno));

    // PM QoS: keep CPUs out of deep C-states while the fd is open (needs root).
    int qos_fd = open("/dev/cpu_dma_latency", O_WRONLY);
    if (qos_fd >= 0) {
        int32_t target = 0;
        if (write(qos_fd, &target, sizeof target) != sizeof target) {
            fprintf(stderr, "[warn] cpu_dma_latency write: %s\n", strerror(errno));
            close(qos_fd);
            qos_fd = -1;
        }
    } else {
        fprintf(stderr, "[warn] open /dev/cpu_dma_latency: %s (run as root to limit C-state latency)\n",
                strerror(errno));
    }

    // Catch Ctrl+C/SIGTERM (no SA_RESTART: read() returns EINTR) so governors get restored.
    struct sigaction sa = { .sa_handler = on_signal };
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    set_performance_governor();

    struct chunk *head = malloc(sizeof *head);
    if (!head) { perror("malloc"); return 1; }
    memset(head, 0, sizeof *head); // pre-fault all pages before recording starts
    struct chunk *tail = head;

    struct input_event evbuf[BATCH];
    int cur_dx = 0, cur_dy = 0, stop = 0;
    fputs("Recording... press any mouse button to stop, Ctrl+C to quit.\n", stderr);

    while (!stop) {
        ssize_t n = read(fd, evbuf, sizeof evbuf);
        int64_t t_read = now_ns(); // one timestamp per batch, not per event
        if (n < 0) {
            if (errno == EINTR && g_intr) break;
            perror("read");
            break;
        }
        size_t cnt = (size_t)n / sizeof *evbuf;
        for (size_t i = 0; i < cnt && !stop; i++) {
            struct input_event *e = &evbuf[i];
            if (e->type == EV_REL && e->code == REL_X) cur_dx += e->value;
            if (e->type == EV_REL && e->code == REL_Y) cur_dy += e->value;
            if (e->type == EV_SYN && e->code == SYN_REPORT) {
                if (tail->sz == CHUNK_CAP) {
                    tail->next = malloc(sizeof *tail);
                    if (!tail->next) { fputs("out of memory\n", stderr); stop = 1; break; }
                    memset(tail->next, 0, sizeof *tail);
                    tail = tail->next;
                }
                tail->data[tail->sz++] = (struct sample){
                    .dx     = cur_dx,
                    .dy     = cur_dy,
                    .t_ev   = (int64_t)e->time.tv_sec * 1000000000LL + (int64_t)e->time.tv_usec * 1000,
                    .t_user = t_read,
                };
                cur_dx = 0;
                cur_dy = 0;
            }
            if (e->type == EV_KEY) stop = 1;
        }
    }

    if (g_intr) {
        fputs("Interrupted, discarding samples.\n", stderr);
        goto cleanup;
    }

    size_t total = 0;
    for (struct chunk *c = head; c; c = c->next) total += c->sz;
    if (total == 0) {
        fputs("No samples captured.\n", stderr);
        goto cleanup;
    }

    int want_html = 0;
    int want_csv = !isatty(STDIN_FILENO); // non-interactive: always keep a CSV
    if (!want_csv) {
        fprintf(stderr, "Save %zu samples: [c] CSV  [h] HTML + open  [enter] skip: ", total);
        char line[32];
        if (fgets(line, sizeof line, stdin)) {
            if (line[0] == 'c' || line[0] == 'C') want_csv = 1;
            if (line[0] == 'h' || line[0] == 'H') want_html = 1;
        }
    }

    if (want_csv) {
        char fname[PATH_MAX] = "log.csv";
        if (isatty(STDIN_FILENO)) {
            fprintf(stderr, "Save CSV as [log.csv]: ");
            char line[PATH_MAX];
            if (fgets(line, sizeof line, stdin)) {
                line[strcspn(line, "\n")] = '\0';
                if (line[0]) snprintf(fname, sizeof fname, "%s", line);
            }
        }
        FILE *fp = fopen(fname, "w");
        if (!fp) {
            fprintf(stderr, "fopen %s: %s\n", fname, strerror(errno));
        } else {
            write_csv(fp, head);
            fclose(fp);
            chown_to_invoker(fname);
            fprintf(stderr, "Saved %zu samples to %s\n", total, fname);
        }
    }

    if (want_html) {
        char fname[64];
        time_t now = time(NULL);
        struct tm tmv;
        localtime_r(&now, &tmv);
        strftime(fname, sizeof fname, "MousePlotter-%Y%m%d-%H%M%S.html", &tmv);

        FILE *fp = fopen(fname, "w");
        if (!fp) {
            fprintf(stderr, "fopen %s: %s\n", fname, strerror(errno));
        } else {
            fwrite(report_head, 1, (size_t)(report_head_end - report_head), fp);
            write_csv(fp, head);
            fwrite(report_tail, 1, (size_t)(report_tail_end - report_tail), fp);
            fclose(fp);
            chown_to_invoker(fname);
            fprintf(stderr, "Saved %zu samples to %s\n", total, fname);
            snprintf(html_path, sizeof html_path, "%s", fname);
        }
    }

cleanup:
    while (head) {
        struct chunk *next = head->next;
        free(head);
        head = next;
    }
    int zero = 0;
    ioctl(fd, EVIOCGRAB, &zero);
    close(fd);
    if (qos_fd >= 0) close(qos_fd); // releases the PM QoS request
    restore_governor();
    // Launch the browser only now, with the grab released, the QoS request
    // dropped, and the governor restored, so the child inherits none of it.
    if (html_path[0]) open_report(html_path);
    return 0;
}
