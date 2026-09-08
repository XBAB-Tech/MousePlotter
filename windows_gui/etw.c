// SPDX-FileCopyrightText: 2026 XBAB Tech, LLC
// SPDX-License-Identifier: MIT
//
// UCX completions identify reports; USBXHCI interrupt/DPC events supply earlier
// timestamps. All times are untouched event-header QPC ticks, not callback times.
// An interrupt timestamp still includes interrupt delivery latency. Reports
// serviced by one interrupt have unresolved individual bus arrival times.
// See ../tools/etw_research/ETW_summary.md for schema and measurement notes.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <evntrace.h>
#include <evntcons.h>
#include <cfgmgr32.h>
#include <initguid.h>
#include <devpkey.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <math.h>
#include "etw.h"

#ifndef EVENT_FILTER_TYPE_EVENT_ID
#define EVENT_FILTER_TYPE_EVENT_ID 0x80000200
#endif

static const GUID UCX_GUID =
    {0x36da592d,0xe43a,0x4e28,{0xaf,0x6f,0x4b,0xc5,0x7c,0x5a,0x11,0xe8}};
static const GUID XHCI_GUID =
    {0x30e1d284,0x5d88,0x459c,{0x83,0xfd,0x63,0x45,0xb3,0x9b,0x19,0xec}};
static const GUID HUB_GUID =
    {0xac52ad17,0xcc01,0x4f85,{0x8d,0xf5,0x4d,0xce,0x43,0x33,0xc9,0x9b}};

static wchar_t g_name[] = L"MousePlotterKernelTS";
static HANDLE g_owner, g_thread;
static TRACEHANDLE g_session, g_consumer = INVALID_PROCESSTRACE_HANDLE;
static int g_stop_failed, g_capture_failed;

// Set before starting the consumer. PDO identity distinguishes even identical
// mice; MI identifies the HID interface of a composite device.
static wchar_t g_pdo[256];
static int g_interface;

// Only the consumer writes capture state. The GUI reads it after joining.
#define KCHUNK 65536
struct kev { int64_t t_cmp, t_int; uint64_t device, pipe; };
struct kchunk { struct kev d[KCHUNK]; size_t sz; struct kchunk *next; };
static struct kchunk *k_head, *k_tail;
static size_t k_count;
static uint64_t g_usb;
static unsigned g_endpoint;
static struct { uint64_t device, pipe; unsigned address; } g_pipes[256];
static size_t g_npipes;

// ISR state is keyed by controller AND interrupter, independent of CPU. DPC
// state is per CPU: only completions inside that controller's DPC may use it.
// Consuming the pending ISR at DPC start prevents stale reuse by a later DPC.
// Real-time ETW orders events within a CPU's buffer, not across CPUs, so an
// ISR delivered after its migrated DPC is either ambiguous or rejected by the
// monotonic check in pair_stopped; the report then keeps its completion time.
struct interrupt { uint64_t controller; unsigned number; int64_t t; int pending; };
static struct interrupt g_interrupts[256];
static size_t g_ninterrupts;
struct dpc { uint64_t controller; int64_t t; };
static struct dpc g_dpc[4096];

static void k_free(void) {
    while (k_head) {
        struct kchunk *next = k_head->next;
        free(k_head);
        k_head = next;
    }
    k_tail = NULL;
    k_count = 0;
}

static void k_append(struct kev e) {
    if (g_capture_failed) return;
    if (!k_tail || k_tail->sz == KCHUNK) {
        // Off the measurement path: no need to lock these pages.
        struct kchunk *c = malloc(sizeof *c);
        if (!c) { g_capture_failed = 1; return; }
        c->sz = 0;
        c->next = NULL;
        if (k_tail) k_tail->next = c; else k_head = c;
        k_tail = c;
    }
    k_tail->d[k_tail->sz++] = e;
    k_count++;
}

static int select_mouse(HANDLE mouse) {
    wchar_t path[512], id[MAX_DEVICE_ID_LEN];
    UINT n = sizeof path / sizeof *path;
    if (!mouse || GetRawInputDeviceInfoW(mouse, RIDI_DEVICENAME, path, &n) == (UINT)-1)
        return -1;
    DEVPROPTYPE type;
    ULONG bytes = sizeof id;
    if (CM_Get_Device_Interface_PropertyW(path, &DEVPKEY_Device_InstanceId,
            &type, (BYTE *)id, &bytes, 0) != CR_SUCCESS || type != DEVPROP_TYPE_STRING)
        return -1;
    DEVINST node;
    if (CM_Locate_DevNodeW(&node, id, CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) return -1;
    g_interface = -1;
    g_pdo[0] = 0;
    for (unsigned depth = 0; depth < 16; depth++) {
        if (CM_Get_Device_IDW(node, id, MAX_DEVICE_ID_LEN, 0) != CR_SUCCESS) break;
        // Do not walk through Bluetooth to its USB radio and call that a mouse.
        if (_wcsnicmp(id, L"HID\\", 4) && _wcsnicmp(id, L"USB\\", 4)) break;
        if (!_wcsnicmp(id, L"USB\\VID_", 8)) {
            const wchar_t *mi = wcsstr(id, L"&MI_");
            if (mi) {
                wchar_t *end;
                unsigned long number = wcstoul(mi + 4, &end, 16);
                if (end != mi + 6 || number > 255) return -1;
                g_interface = (int)number;
            } else {
                bytes = sizeof g_pdo;
                ULONG reg_type;
                return CM_Get_DevNode_Registry_PropertyW(node,
                    CM_DRP_PHYSICAL_DEVICE_OBJECT_NAME, &reg_type, g_pdo, &bytes, 0)
                    == CR_SUCCESS && reg_type == REG_SZ && g_pdo[0] ? 0 : -1;
            }
        }
        DEVINST parent;
        if (CM_Get_Parent(&parent, node, 0) != CR_SUCCESS) break;
        node = parent;
    }
    return -1;
}

// Known manifest versions only; ETW packs fields without C struct alignment.
// Unknown versions are ignored, so they cannot silently change field meaning.
struct payload { const BYTE *p; size_t n; unsigned pointer; int ok; };
static const BYTE *take(struct payload *p, size_t n) {
    if (!p->ok || n > p->n) { p->ok = 0; return NULL; }
    const BYTE *v = p->p;
    p->p += n;
    p->n -= n;
    return v;
}
static uint64_t number(struct payload *p, size_t n) {
    uint64_t v = 0;
    const BYTE *b = take(p, n);
    if (b) memcpy(&v, b, n);
    return v;
}
static int string(struct payload *p, wchar_t *out, size_t cap) {
    size_t n = 0;
    while (p->ok) {
        wchar_t ch = (wchar_t)number(p, 2);
        if (!p->ok) break;
        if (out) {
            if (n >= cap) { p->ok = 0; break; }
            out[n++] = ch;
        }
        if (!ch) return 1;
    }
    return 0;
}

// Choose exactly one interrupt IN endpoint on the selected HID interface.
static unsigned descriptor_endpoint(const BYTE *b, size_t n, int interface) {
    unsigned endpoint = 0;
    int selected = 0;
    for (size_t i = 0; i < n;) {
        if (n - i < 2 || b[i] < 2 || b[i] > n - i) return 0;
        unsigned len = b[i], type = b[i + 1];
        if (type == 4) {
            if (len < 9) return 0;
            selected = b[i + 5] == 3 && b[i + 3] == 0 &&
                       (interface < 0 || b[i + 2] == interface);
        } else if (type == 5 && selected) {
            if (len < 7) return 0;
            unsigned address = b[i + 2];
            if ((address & 0x80) && (b[i + 3] & 3) == 3) {
                if (endpoint) return 0;
                endpoint = address;
            }
        }
        i += len;
    }
    return endpoint;
}

static void hub_event(struct payload p) {
    number(&p, p.pointer); // hub
    uint64_t device = number(&p, p.pointer);
    number(&p, 4); // port
    string(&p, NULL, 0); // description
    string(&p, NULL, 0); // interface path
    take(&p, 18); // USB_DEVICE_DESCRIPTOR
    size_t n = (size_t)number(&p, 2);
    const BYTE *config = take(&p, n);
    wchar_t pdo[256];
    if (!string(&p, pdo, sizeof pdo / sizeof *pdo) || _wcsicmp(pdo, g_pdo)) return;
    unsigned endpoint = descriptor_endpoint(config, n, g_interface);
    if (!device || !endpoint || (g_usb && g_usb != device)) {
        g_capture_failed = 1;
        return;
    }
    g_usb = device;
    g_endpoint = endpoint;
}

static void pipe_event(struct payload p) {
    uint64_t device = number(&p, p.pointer);
    number(&p, p.pointer); // endpoint object
    uint64_t pipe = number(&p, p.pointer);
    const BYTE *b = take(&p, 7);
    if (!b || b[0] < 7 || b[1] != 5 || !(b[2] & 0x80) || (b[3] & 3) != 3) return;
    size_t i = 0;
    while (i < g_npipes && g_pipes[i].pipe != pipe) i++;
    if (i == sizeof g_pipes / sizeof *g_pipes) { g_capture_failed = 1; return; }
    if (i == g_npipes) g_npipes++;
    g_pipes[i].device = device;
    g_pipes[i].pipe = pipe;
    g_pipes[i].address = b[2];
}

static void xhci_event(struct payload p, USHORT id, unsigned cpu, int64_t t) {
    if (id != 42 && cpu < sizeof g_dpc / sizeof *g_dpc)
        g_dpc[cpu] = (struct dpc){0};
    if (id == 44) return;
    uint64_t controller = number(&p, p.pointer);
    unsigned intr = (unsigned)number(&p, 4);
    if (!p.ok || !controller) return;
    size_t i = 0;
    while (i < g_ninterrupts && (g_interrupts[i].controller != controller ||
                                g_interrupts[i].number != intr)) i++;
    if (i == sizeof g_interrupts / sizeof *g_interrupts) return;
    if (i == g_ninterrupts) {
        g_ninterrupts++;
        g_interrupts[i] = (struct interrupt){controller, intr, 0, 0};
    }
    struct interrupt *irq = &g_interrupts[i];
    if (id == 42) {
        // Multiple interrupts before the DPC make attribution ambiguous; every
        // report it services then falls back to its completion time.
        irq->pending = irq->pending ? 2 : 1;
        irq->t = t;
    } else if (id == 43) {
        // Same-CPU ETW order is guaranteed by its buffer. If a DPC migrates,
        // late ISR delivery makes this ambiguous or fails the monotonic check.
        if (cpu < sizeof g_dpc / sizeof *g_dpc)
            g_dpc[cpu] = (struct dpc){controller,
                irq->pending == 1 && irq->t <= t ? irq->t : 0};
        irq->pending = 0;
    }
}

static void WINAPI on_event(PEVENT_RECORD ev) {
    USHORT id = ev->EventHeader.EventDescriptor.Id;
    UCHAR version = ev->EventHeader.EventDescriptor.Version;
    int64_t t = ev->EventHeader.TimeStamp.QuadPart;
    unsigned cpu = (ev->EventHeader.Flags & EVENT_HEADER_FLAG_PROCESSOR_INDEX)
        ? ev->BufferContext.ProcessorIndex : ev->BufferContext.ProcessorNumber;
    struct payload p = {(const BYTE *)ev->UserData, ev->UserDataLength,
        ev->EventHeader.Flags & EVENT_HEADER_FLAG_32_BIT_HEADER ? 4u : 8u,
        ev->UserData != NULL};
    if (!memcmp(&ev->EventHeader.ProviderId, &HUB_GUID, sizeof(GUID))) {
        if (id == 6 && version <= 2) hub_event(p);
    } else if (!memcmp(&ev->EventHeader.ProviderId, &XHCI_GUID, sizeof(GUID))) {
        // Earlier versions lack controller identity; use UCX on those builds.
        if (id >= 42 && id <= 44) {
            if (version == 2) xhci_event(p, id, cpu, t);
            else {
                g_ninterrupts = 0;
                if (cpu < sizeof g_dpc / sizeof *g_dpc)
                    g_dpc[cpu] = (struct dpc){0};
            }
        }
    } else if (!memcmp(&ev->EventHeader.ProviderId, &UCX_GUID, sizeof(GUID))) {
        if (id == 6 && version == 0) { pipe_event(p); return; }
        if (id != 27 || version > 1) return;
        uint64_t controller = number(&p, p.pointer);
        uint64_t device = number(&p, p.pointer);
        uint64_t pipe = number(&p, p.pointer);
        if (g_usb && device != g_usb) return;
        take(&p, 2 * p.pointer); // IRP and URB
        number(&p, 2); // URB header length
        unsigned function = (unsigned)number(&p, 2);
        unsigned status = (unsigned)number(&p, 4);
        take(&p, 3 * p.pointer); // device, flags, pipe
        unsigned flags = (unsigned)number(&p, 4);
        unsigned length = (unsigned)number(&p, 4);
        take(&p, 11 * p.pointer); // buffer, MDL, reserved pointers
        unsigned nt_status = (unsigned)number(&p, 4);
        if (!p.ok) { g_capture_failed = 1; return; }
        if (function != 9 || status || (nt_status & 0x80000000u) ||
            !(flags & 1) || !length) return;
        int64_t anchor = 0;
        if (cpu < sizeof g_dpc / sizeof *g_dpc &&
            g_dpc[cpu].controller == controller && g_dpc[cpu].t <= t)
            anchor = g_dpc[cpu].t;
        k_append((struct kev){t, anchor, device, pipe});
    }
}

struct props { EVENT_TRACE_PROPERTIES p; wchar_t name[128]; };
static void fill_props(struct props *pb) {
    memset(pb, 0, sizeof *pb);
    pb->p.Wnode.BufferSize = sizeof *pb;
    pb->p.Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    pb->p.Wnode.ClientContext = 1;
    pb->p.LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
    pb->p.LoggerNameOffset = offsetof(struct props, name);
    pb->p.BufferSize = 64;
    pb->p.MinimumBuffers = 64;
    pb->p.MaximumBuffers = 256;
    pb->p.FlushTimer = 1;
}

static ULONG enable_ids(const GUID *provider, ULONGLONG keywords,
                        const USHORT *ids, USHORT count) {
    // Private definition: mingw supplies the descriptor but not its payload.
    struct { BOOLEAN include; UCHAR reserved; USHORT count, ids[3]; } filter = {0};
    if (count > 3) return ERROR_INVALID_PARAMETER;
    filter.include = TRUE;
    filter.count = count;
    memcpy(filter.ids, ids, (size_t)count * sizeof *ids);
    EVENT_FILTER_DESCRIPTOR fd = {0};
    fd.Ptr = (ULONGLONG)(ULONG_PTR)&filter;
    fd.Size = 4u + 2u * count;
    fd.Type = EVENT_FILTER_TYPE_EVENT_ID;
    ENABLE_TRACE_PARAMETERS ep = {0};
    ep.Version = ENABLE_TRACE_PARAMETERS_VERSION_2;
    ep.EnableFilterDesc = &fd;
    ep.FilterDescCount = 1;
    ULONG st = EnableTraceEx2(g_session, provider, EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                              TRACE_LEVEL_INFORMATION, keywords, 0, 0, &ep);
    if (st == ERROR_INVALID_PARAMETER || st == ERROR_NOT_SUPPORTED)
        st = EnableTraceEx2(g_session, provider, EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                            TRACE_LEVEL_INFORMATION, keywords, 0, 0, NULL);
    return st;
}

static DWORD WINAPI consumer(LPVOID unused) {
    (void)unused;
    TRACEHANDLE h = g_consumer; // etw_stop may invalidate the global early
    return ProcessTrace(&h, 1, NULL, NULL);
}

void etw_stop(void) {
    struct props pb;
    if (g_session) {
        fill_props(&pb);
        ULONG st = ControlTraceW(g_session, NULL, &pb.p, EVENT_TRACE_CONTROL_STOP);
        if (st != ERROR_SUCCESS || pb.p.EventsLost || pb.p.RealTimeBuffersLost ||
            pb.p.LogBuffersLost) g_stop_failed = 1;
        if (st != ERROR_SUCCESS && g_consumer != INVALID_PROCESSTRACE_HANDLE) {
            CloseTrace(g_consumer); // cancel processing if the session cannot stop
            g_consumer = INVALID_PROCESSTRACE_HANDLE;
        }
    }
    if (g_thread) {
        // Storage must remain owned by the consumer until it actually exits.
        if (WaitForSingleObject(g_thread, INFINITE) != WAIT_OBJECT_0) {
            g_stop_failed = 1;
            return;
        }
        DWORD st;
        if (!GetExitCodeThread(g_thread, &st) || st != ERROR_SUCCESS) g_stop_failed = 1;
        CloseHandle(g_thread);
        g_thread = NULL;
    }
    if (g_consumer != INVALID_PROCESSTRACE_HANDLE) CloseTrace(g_consumer);
    g_consumer = INVALID_PROCESSTRACE_HANDLE;
    g_session = 0;
    if (g_owner) {
        ReleaseMutex(g_owner);
        CloseHandle(g_owner);
        g_owner = NULL;
    }
}

int etw_start(HANDLE mouse) {
    etw_stop();
    if (g_thread) return -1;
    k_free();
    if (select_mouse(mouse)) return -1;
    g_stop_failed = g_capture_failed = 0;
    g_usb = g_endpoint = 0;
    g_npipes = g_ninterrupts = 0;
    memset(g_dpc, 0, sizeof g_dpc);

    // Only the owner may stop a leftover session. Another live instance keeps
    // its recording; an abandoned mutex lets us recover after a crash.
    g_owner = CreateMutexW(NULL, FALSE, L"Global\\MousePlotterKernelTSOwner");
    if (!g_owner) return -1;
    DWORD wait = WaitForSingleObject(g_owner, 0);
    if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) {
        CloseHandle(g_owner);
        g_owner = NULL;
        return -1;
    }
    struct props pb;
    fill_props(&pb);
    ControlTraceW(0, g_name, &pb.p, EVENT_TRACE_CONTROL_STOP);
    fill_props(&pb);
    if (StartTraceW(&g_session, g_name, &pb.p) != ERROR_SUCCESS) {
        g_session = 0;
        goto fail;
    }
    EVENT_TRACE_LOGFILEW lf = {0};
    lf.LoggerName = g_name;
    lf.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD |
                          PROCESS_TRACE_MODE_RAW_TIMESTAMP;
    lf.EventRecordCallback = on_event;
    g_consumer = OpenTraceW(&lf);
    if (g_consumer == INVALID_PROCESSTRACE_HANDLE) goto fail;
    g_thread = CreateThread(NULL, 0, consumer, NULL, 0, NULL);
    if (!g_thread) goto fail;
    SetThreadPriority(g_thread, THREAD_PRIORITY_BELOW_NORMAL);

    static const USHORT hub_ids[] = {6}, ucx_ids[] = {6,27}, xhci_ids[] = {42,43,44};
    if (enable_ids(&HUB_GUID, 0x8000, hub_ids, 1) != ERROR_SUCCESS ||
        enable_ids(&UCX_GUID, 0x8040, ucx_ids, 2) != ERROR_SUCCESS) goto fail;
    enable_ids(&XHCI_GUID, 0x20, xhci_ids, 3); // optional; UCX remains usable
    return 0;
fail:
    etw_stop();
    return -1;
}

static int cmp_event(const void *a, const void *b) {
    int64_t x = ((const struct kev *)a)->t_cmp, y = ((const struct kev *)b)->t_cmp;
    return (x > y) - (x < y);
}

// Fill only complete, causal, equal-count spans between reliable boundaries.
// A dispatch stall can span any number of reports; there is no pending queue.
// Unequal spans cannot tell dropped reports from coalesced input: leave them raw.
static int match_span(const struct kev *K, const int64_t *U, size_t *map,
                       size_t k, size_t u, size_t count) {
    for (size_t i = 0; i < count; i++) if (K[k + i].t_cmp > U[u + i]) return 0;
    for (size_t i = 0; i < count; i++) map[u + i] = k + i;
    return 1;
}

static size_t match_samples(const struct kev *K, size_t nk, const int64_t *U,
                            size_t nu, size_t *map) {
    for (size_t i = 0; i < nu; i++) map[i] = SIZE_MAX;
    size_t next = 0, prev = SIZE_MAX, prev2 = SIZE_MAX;
    size_t ak = 0, au = 0, unmatched = 0;
    size_t first_k = 0, first_u = 0;
    int have_anchor = 0;
    for (size_t u = 0; u < nu; u++) {
        while (next < nk && K[next].t_cmp <= U[u]) next++;
        size_t k = next ? next - 1 : SIZE_MAX;
        // The middle of three consecutive one-to-one intervals is a boundary.
        // Batched user delivery fails this condition until the queue clears.
        if (u >= 2 && prev2 != SIZE_MAX && prev == prev2 + 1 && k == prev + 1) {
            size_t bu = u - 1, bk = prev;
            if (!have_anchor) {
                first_k = bk;
                first_u = bu;
                if (bk >= bu) match_span(K, U, map, bk - bu, 0, bu);
            } else {
                size_t nc = bk - ak - 1, ns = bu - au - 1;
                if (nc == ns) match_span(K, U, map, ak + 1, au + 1, ns);
                else if (nc > ns) unmatched += nc - ns;
            }
            map[bu] = bk;
            ak = bk;
            au = bu;
            have_anchor = 1;
        }
        prev2 = prev;
        prev = k;
    }
    if (have_anchor) {
        // Prefer uninterrupted FIFO order when the outer boundaries agree.
        // A sustained delivery delay can look like several clean intervals
        // shifted by one report; local boundaries must not override that order.
        if (ak - first_k == au - first_u &&
            match_span(K, U, map, first_k, first_u, au - first_u + 1))
            unmatched = 0;
        size_t nc = next - ak - 1, ns = nu - au - 1;
        if (nc == ns) match_span(K, U, map, ak + 1, au + 1, ns);
        else if (nc > ns) unmatched += nc - ns;
    }
    return unmatched;
}

static int pair_stopped(const int64_t *U, size_t n, struct etw_pairing *out, int64_t qpf) {
    memset(out, 0, sizeof *out);
    if (g_thread || g_session || g_stop_failed || g_capture_failed || !g_usb ||
        !g_endpoint || !n || qpf <= 0) return -1;
    for (size_t i = 1; i < n; i++) if (U[i] < U[i - 1]) return -1;
    uint64_t pipe = 0;
    for (size_t i = 0; i < g_npipes; i++) {
        if (g_pipes[i].device != g_usb || g_pipes[i].address != g_endpoint) continue;
        if (pipe && pipe != g_pipes[i].pipe) return -1;
        pipe = g_pipes[i].pipe;
    }
    if (!pipe || !k_count) return -1;
    struct kev *K = malloc(k_count * sizeof *K);
    size_t *map = malloc(n * sizeof *map);
    struct etw_time *times = malloc(n * sizeof *times);
    if (!K || !map || !times) { free(K); free(map); free(times); return -1; }
    size_t nk = 0;
    for (struct kchunk *c = k_head; c; c = c->next)
        for (size_t i = 0; i < c->sz; i++)
            if (c->d[i].device == g_usb && c->d[i].pipe == pipe) K[nk++] = c->d[i];
    qsort(K, nk, sizeof *K, cmp_event); // completion order, never interrupt order
    out->unmatched = match_samples(K, nk, U, n, map);
    double mean = 0, m2 = 0;
    size_t matched = 0;
    int64_t last = 0, last_interrupt = 0;
    for (size_t i = 0; i < n; i++) {
        times[i] = (struct etw_time){U[i], ETW_RAW};
        if (map[i] != SIZE_MAX) {
            const struct kev *e = &K[map[i]];
            if (e->t_int > 0 && e->t_int <= e->t_cmp && e->t_int >= last) {
                times[i] = (struct etw_time){e->t_int, ETW_INTERRUPT};
                out->interrupts++;
                if (e->t_int == last_interrupt) out->shared++;
                last_interrupt = e->t_int;
            } else if (e->t_cmp >= last) {
                times[i] = (struct etw_time){e->t_cmp, ETW_COMPLETION};
                out->completions++;
            }
        }
        last = times[i].t;
        if (times[i].source == ETW_RAW) out->raw++;
        else {
            double d = (double)(U[i] - last) * 1e6 / (double)qpf;
            double delta = d - mean;
            mean += delta / (double)++matched;
            m2 += delta * (d - mean);
        }
    }
    free(K);
    free(map);
    if (!matched) { free(times); memset(out, 0, sizeof *out); return -1; }
    out->times = times;
    out->lag_us = mean;
    out->resid_us = sqrt(m2 / (double)matched);
    return 0;
}

int etw_pair(const int64_t *U, size_t n, struct etw_pairing *out, int64_t qpf) {
    int result = pair_stopped(U, n, out, qpf);
    if (!g_thread && !g_session) k_free();
    return result;
}

void etw_free_pairing(struct etw_pairing *p) {
    free(p->times);
    memset(p, 0, sizeof *p);
}
