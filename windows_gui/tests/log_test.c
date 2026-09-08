// SPDX-FileCopyrightText: 2026 XBAB Tech, LLC
// SPDX-License-Identifier: MIT
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#undef NDEBUG
#include <assert.h>

static int fail_alloc, write_mode;
static char output[4096];
static size_t output_size;
static LPVOID WINAPI test_alloc(LPVOID p, SIZE_T n, DWORD type, DWORD protection) {
    if (fail_alloc) { fail_alloc = 0; return NULL; }
    return VirtualAlloc(p, n, type, protection);
}
static BOOL WINAPI test_write(HANDLE h, LPCVOID p, DWORD n, LPDWORD wrote,
                              LPOVERLAPPED overlap) {
    (void)h; (void)overlap;
    *wrote = 0;
    if (write_mode == 1) { SetLastError(ERROR_DISK_FULL); return FALSE; }
    if (write_mode == 2) return TRUE; // no forward progress
    if (n > 3) n = 3; // exercise partial writes on every call
    assert(output_size + n < sizeof output);
    memcpy(output + output_size, p, n);
    output_size += n;
    output[output_size] = 0;
    *wrote = n;
    return TRUE;
}
#define VirtualAlloc test_alloc
#define WriteFile test_write
#include "../log.c"
#undef VirtualAlloc
#undef WriteFile

int main(void) {
    G.head = G.tail = new_chunk();
    assert(G.head);
    G.qpf = 1000000;
    assert(append_sample(1, 2, 1000));
    assert(append_sample(3, 4, 1125));
    assert(append_sample(5, 6, 1250));
    G.pairing.times = malloc(3 * sizeof *G.pairing.times);
    assert(G.pairing.times);
    G.pairing.times[0] = (struct etw_time){900, ETW_INTERRUPT};
    G.pairing.times[1] = (struct etw_time){1120, ETW_COMPLETION};
    G.pairing.times[2] = (struct etw_time){1250, ETW_RAW};
    G.pairing.interrupts = G.pairing.completions = G.pairing.raw = 1;
    G.pair_ok = 1;
    assert(write_csv_file((HANDLE)1));
    assert(strstr(output, "timestampSource\r\n"));
    assert(strstr(output, "1,2,0.000000,0.100000,interrupt\r\n"));
    assert(strstr(output, "3,4,0.220000,0.225000,completion\r\n"));
    assert(strstr(output, "5,6,0.350000,0.350000,raw\r\n"));

    // Failed writes must propagate, without changing the captured data.
    write_mode = 1;
    assert(!write_csv_file((HANDLE)1));
    assert(G.count == 3 && G.total_dx == 9 && G.total_dy == 12);
    write_mode = 2;
    assert(!write_all((HANDLE)1, "abc", 3));

    // A failed new recording must preserve the old samples and pairing.
    struct chunk *old = G.head;
    struct etw_time *old_times = G.pairing.times;
    fail_alloc = 1;
    start_recording(SRC_SPACE);
    assert(G.head == old && G.count == 3 && G.pair_ok);
    assert(G.pairing.times == old_times && !G.recording);

    G.tail->sz = CHUNK_CAP;
    fail_alloc = 1;
    assert(!append_sample(7, 8, 1400));
    assert(G.storage_failed && G.count == 3);
    G.tail->sz = 3;
    free_pairing();
    output_size = 0;
    write_mode = 0;
    assert(write_csv_file((HANDLE)1));
    assert(strstr(output, "1,2,0.000000\r\n"));
    assert(!strstr(output, "timestampSource"));

    wchar_t small[4];
    assert(create_stamped(small, 4, L"", L"html") == INVALID_HANDLE_VALUE);
    assert(GetLastError() == ERROR_FILENAME_EXCED_RANGE);
    free_chunks();
    assert(!G.head && !G.tail && G.count == 0);
    puts("GUI storage/export regression tests passed.");
    return 0;
}
