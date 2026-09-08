// SPDX-FileCopyrightText: 2026 XBAB Tech, LLC
// SPDX-License-Identifier: MIT
// Offline regression tests. No tracing session or administrator rights needed.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <evntrace.h>
#include <stdlib.h>
#include <stdio.h>
#undef NDEBUG
#include <assert.h>

static int fail_alloc;
static void *test_malloc(size_t n) {
    if (fail_alloc) { fail_alloc = 0; return NULL; }
    return malloc(n);
}
static ULONG stop_status, events_lost;
static ULONG WINAPI test_control(TRACEHANDLE h, LPCWSTR name,
                                 PEVENT_TRACE_PROPERTIES p, ULONG code) {
    (void)h; (void)name; (void)code;
    p->EventsLost = events_lost;
    return stop_status;
}
#define malloc test_malloc
#define ControlTraceW test_control
#include "../etw.c"
#undef malloc
#undef ControlTraceW

#define N 200
static int64_t U[N];
static struct etw_pairing P;

static void reset(void) {
    etw_free_pairing(&P);
    k_free();
    g_capture_failed = g_stop_failed = 0;
    g_usb = 7;
    g_endpoint = 0x81;
    g_npipes = 1;
    g_pipes[0].device = 7;
    g_pipes[0].pipe = 11;
    g_pipes[0].address = 0x81;
    g_ninterrupts = 0;
    memset(g_dpc, 0, sizeof g_dpc);
    stop_status = events_lost = 0;
}
static int64_t completion(int i) { return 1000 + 125 * i; }
static void reports(void) {
    for (int i = 0; i < N; i++) {
        U[i] = completion(i) + 10;
        k_append((struct kev){completion(i), completion(i) - 5, 7, 11});
    }
}
static void assert_all_paired(void) {
    assert(etw_pair(U, N, &P, 1000000) == 0);
    assert(P.interrupts == N && !P.raw && !P.completions && !P.unmatched);
    for (int i = 0; i < N; i++) assert(P.times[i].t == completion(i) - 5);
}

static void pairing_tests(void) {
    reset(); reports();
    // Faster-looking unrelated traffic must not compete with the selected mouse.
    for (int i = 0; i < N; i++) k_append((struct kev){U[i] - 1, 0, 8, 12});
    assert_all_paired();

    reset(); reports();
    g_usb = 8;
    assert(etw_pair(U, N, &P, 1000000) == -1);
    reset(); reports(); g_usb = 0;
    assert(etw_pair(U, N, &P, 1000000) == -1);

    // Catch-up after stalls at the start, middle, and end; well beyond 64 reports.
    for (int start = 0; start <= 120; start += 60) {
        reset(); reports();
        for (int i = start; i < start + 80; i++)
            U[i] = completion(start + 79) + 10 + i - start;
        assert_all_paired();
    }

    // A backlog may drain across more than one completion interval.
    reset(); reports();
    for (int i = 50; i < 55; i++) U[i] += 150;
    U[55] = U[54] + 1;
    assert_all_paired();

    // Repeated stalls must preserve every original timestamp, not just produce
    // a smooth-looking series. A fixed seed makes failures reproducible.
    unsigned random = 1;
    for (int trial = 0; trial < 200; trial++) {
        reset(); reports();
        for (int i = 10; i < N; i++) {
            random = random * 1664525u + 1013904223u;
            if (i < 150 && random % 20 == 0) U[i] += 2000;
            if (U[i] <= U[i - 1]) U[i] = U[i - 1] + 1;
        }
        assert_all_paired();
    }

    // One missing user report: preserve all remaining samples and resynchronize.
    reset(); reports();
    memmove(&U[50], &U[51], (N - 51) * sizeof *U);
    assert(etw_pair(U, N - 1, &P, 1000000) == 0);
    assert(P.unmatched == 1 && P.raw < 6);
    for (int i = 60; i < N - 1; i++) assert(P.times[i].t == completion(i + 1) - 5);
    for (int i = 1; i < N - 1; i++) assert(P.times[i].t >= P.times[i - 1].t);

    // Missing completion: no invented timestamp or duplicate movement row.
    reset(); reports();
    memmove(&k_head->d[50], &k_head->d[51], (N - 51) * sizeof(struct kev));
    k_head->sz--; k_count--;
    assert(etw_pair(U, N, &P, 1000000) == 0);
    assert(P.raw > 0 && P.raw < 6);
    for (int i = 1; i < N; i++) assert(P.times[i].t >= P.times[i - 1].t);
    for (int i = 60; i < N; i++) assert(P.times[i].t == completion(i) - 5);

    reset(); reports(); g_stop_failed = 1;
    assert(etw_pair(U, N, &P, 1000000) == -1);
    reset(); reports(); g_capture_failed = 1;
    assert(etw_pair(U, N, &P, 1000000) == -1);
    reset(); fail_alloc = 1; k_append((struct kev){1,0,7,11});
    assert(g_capture_failed && k_count == 0);

    // Interrupt timestamps must not be sorted independently of their reports.
    reset(); reports(); k_head->d[50].t_int = 1;
    assert(etw_pair(U, N, &P, 1000000) == 0);
    assert(P.times[50].source == ETW_COMPLETION);
    assert(P.times[50].t == completion(50));
    reset(); reports(); k_head->d[50].t_int = k_head->d[49].t_int;
    assert(etw_pair(U, N, &P, 1000000) == 0 && P.shared == 1);
}

static void put(BYTE *b, size_t at, uint64_t v, size_t n) { memcpy(b + at, &v, n); }
static void event(const GUID *guid, unsigned id, unsigned version, unsigned cpu,
                  int64_t t, BYTE *b, size_t n, unsigned pointer) {
    EVENT_RECORD e = {0};
    e.EventHeader.ProviderId = *guid;
    e.EventHeader.EventDescriptor.Id = (USHORT)id;
    e.EventHeader.EventDescriptor.Version = (UCHAR)version;
    e.EventHeader.TimeStamp.QuadPart = t;
    e.EventHeader.Flags = EVENT_HEADER_FLAG_PROCESSOR_INDEX |
        (pointer == 4 ? EVENT_HEADER_FLAG_32_BIT_HEADER : EVENT_HEADER_FLAG_64_BIT_HEADER);
    e.BufferContext.ProcessorIndex = (USHORT)cpu;
    e.UserData = b;
    e.UserDataLength = (USHORT)n;
    on_event(&e);
}
static void irq(unsigned id, unsigned cpu, uint64_t controller, unsigned intr, int64_t t) {
    BYTE b[20] = {0};
    put(b, 0, controller, 8); put(b, 8, intr, 4);
    event(&XHCI_GUID, id, 2, cpu, t, b, sizeof b, 8);
}
static void complete(unsigned cpu, uint64_t controller, int64_t t, unsigned pointer) {
    BYTE b[172] = {0};
    put(b, 0, controller, pointer);
    put(b, pointer, 7, pointer);
    put(b, 2 * pointer, 11, pointer);
    put(b, 5 * pointer + 2, 9, 2); // URB function
    put(b, 8 * pointer + 8, 1, 4); // IN
    put(b, 8 * pointer + 12, 6, 4); // bytes transferred
    event(&UCX_GUID, 27, 1, cpu, t, b, 19 * pointer + 20, pointer);
}
static void correlation_tests(void) {
    reset();
    irq(42, 0, 1, 0, 100);
    irq(42, 0, 2, 0, 101); // unrelated controller, same CPU and interrupter number
    irq(43, 1, 1, 0, 102); // DPC migrated CPUs
    complete(1, 1, 103, 8);
    assert(k_head->d[0].t_int == 100);
    complete(1, 2, 104, 8); // wrong controller inside that DPC
    assert(k_head->d[1].t_int == 0);
    irq(44, 1, 1, 0, 105);
    complete(1, 1, 106, 8); // outside the DPC
    assert(k_head->d[2].t_int == 0);
    irq(43, 1, 1, 0, 10000); // stale ISR must not be reused
    complete(1, 1, 10001, 8);
    assert(k_head->d[3].t_int == 0);

    reset();
    irq(42, 0, 1, 0, 100);
    irq(42, 0, 1, 1, 101); // different interrupter
    irq(43, 0, 1, 0, 102);
    irq(42, 0, 1, 1, 103); // ISR preempts DPC; must not overwrite its anchor
    complete(0, 1, 104, 4);
    assert(k_count == 1 && k_head->d[0].t_int == 100);

    reset();
    irq(42, 0, 1, 0, 100);
    irq(43, 0, 1, 0, 10000); // a real long DPC dispatch delay must stay measurable
    complete(0, 1, 10001, 8);
    assert(k_head->d[0].t_int == 100);

    reset();
    irq(42, 0, 1, 0, 100); irq(42, 0, 1, 0, 101);
    irq(43, 0, 1, 0, 102); complete(0, 1, 103, 8);
    assert(k_head->d[0].t_int == 0); // ambiguous multiple interrupts
}

static void descriptor_tests(void) {
    BYTE b[] = {9,2,41,0,2,1,0,0x80,50,
                9,4,0,0,1,3,1,2,0, 7,5,0x81,3,8,0,1,
                9,4,1,0,1,3,1,1,0, 7,5,0x82,3,8,0,1};
    assert(descriptor_endpoint(b, sizeof b, 0) == 0x81);
    assert(descriptor_endpoint(b, sizeof b, 1) == 0x82);
    assert(descriptor_endpoint(b, sizeof b, -1) == 0);
    assert(descriptor_endpoint(b, sizeof b - 1, 1) == 0);
    b[9] = 0;
    assert(descriptor_endpoint(b, sizeof b, 0) == 0);
}

static void metadata_tests(void) {
    const BYTE config[] = {9,2,25,0,1,1,0,0x80,50,
                          9,4,0,0,1,3,1,2,0, 7,5,0x81,3,8,0,1};
    for (unsigned width = 4; width <= 8; width += 4) {
        reset();
        g_usb = g_endpoint = g_npipes = 0;
        g_interface = 0;
        wcscpy(g_pdo, L"\\Device\\USBPDO-test");
        BYTE hub[256] = {0};
        put(hub, width, 7, width);
        size_t at = 2 * width + 4 + 4 + 18; // two empty strings, device descriptor
        put(hub, at, sizeof config, 2); at += 2;
        memcpy(hub + at, config, sizeof config); at += sizeof config;
        size_t pdo_bytes = (wcslen(g_pdo) + 1) * sizeof(wchar_t);
        memcpy(hub + at, g_pdo, pdo_bytes); at += pdo_bytes;
        event(&HUB_GUID, 6, 2, 0, 100, hub, at - 1, width); // truncated string
        assert(g_usb == 0);
        event(&HUB_GUID, 6, 2, 0, 100, hub, at, width);
        assert(g_usb == 7 && g_endpoint == 0x81 && !g_capture_failed);

        BYTE pipe[32] = {0};
        put(pipe, 0, 7, width); put(pipe, 2 * width, 11, width);
        memcpy(pipe + 3 * width, config + 18, 7);
        event(&UCX_GUID, 6, 0, 0, 101, pipe, 3 * width + 7, width);
        assert(g_npipes == 1 && g_pipes[0].pipe == 11 && g_pipes[0].address == 0x81);
        complete(0, 1, 102, width);
        assert(k_count == 1 && k_head->d[0].device == 7 && k_head->d[0].pipe == 11);
    }
    reset();
    irq(42, 0, 1, 0, 100);
    BYTE unknown[12] = {0};
    event(&XHCI_GUID, 42, 3, 0, 101, unknown, sizeof unknown, 8);
    irq(43, 0, 1, 0, 102); complete(0, 1, 103, 8);
    assert(k_head->d[0].t_int == 0);
}

static DWORD WINAPI delayed_consumer(LPVOID unused) {
    (void)unused;
    Sleep(3100); // the old stop incorrectly returned before this append
    k_append((struct kev){100,0,7,11});
    return ERROR_SUCCESS;
}
static void stop_tests(void) {
    reset();
    g_session = 1; // fake controller, real thread
    g_thread = CreateThread(NULL, 0, delayed_consumer, NULL, 0, NULL);
    assert(g_thread);
    etw_stop();
    assert(!g_thread && !g_session && k_count == 1 && !g_stop_failed);
    g_session = 1; events_lost = 1;
    etw_stop();
    assert(g_stop_failed);
}

int main(void) {
    pairing_tests();
    correlation_tests();
    descriptor_tests();
    metadata_tests();
    stop_tests();
    reset();
    puts("ETW regression tests passed.");
    return 0;
}
