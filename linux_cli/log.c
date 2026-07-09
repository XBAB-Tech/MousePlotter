// SPDX-FileCopyrightText: 2026 XBAB Tech, LLC
// SPDX-License-Identifier: MIT

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <inttypes.h>
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

int main(void) {
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
        goto cleanup;
    }
    fprintf(fp, "MousePlotter CLI logger\n800\nxCount,yCount,eventTime (ms),userTime (ms)\n");

    int64_t t0_ev   = head->data[0].t_ev;
    int64_t t0_user = head->data[0].t_user;
    for (struct chunk *c = head; c; c = c->next) {
        for (size_t j = 0; j < c->sz; j++) {
            int64_t de = c->data[j].t_ev   - t0_ev;
            int64_t du = c->data[j].t_user - t0_user;
            fprintf(fp, "%d,%d,%" PRId64 ".%06" PRId64 ",%" PRId64 ".%06" PRId64 "\n",
                c->data[j].dx, c->data[j].dy,
                de / 1000000, de % 1000000,
                du / 1000000, du % 1000000);
        }
    }
    fclose(fp);
    fprintf(stderr, "Saved %zu samples to %s\n", total, fname);

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
    return 0;
}
