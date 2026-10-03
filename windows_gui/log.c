// SPDX-FileCopyrightText: 2026 XBAB Tech, LLC
// SPDX-License-Identifier: MIT
//
// MousePlotter Windows GUI logger.
//
// Records reports from one mouse, named by clicking with it, so a second
// pointing device cannot interleave reports into the capture. Given an elevated
// token it also opens a real-time ETW session and stamps each report with a
// kernel timestamp -- the moment the driver serviced the interrupt that
// carried it, or failing that the moment the USB transfer completed -- which
// has far less jitter than the userspace WM_INPUT stamp. ETW carries no X/Y,
// so the deltas still come from Raw Input; the two streams are paired after
// the recording (see etw.c). The manifest asks for elevation at launch, but a
// user who has no administrator rights is never prompted and the app still
// works, falling back to Raw Input only with a 3-column CSV. ETW exports also
// identify the timestamp source for each sample, including any Raw Input
// fallback.
//
// Requires Windows 10 or later (Universal CRT, CM_Get_Device_Interface_Property).
// Build with build.bat or the Makefile.

#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <timeapi.h>
#include <stdint.h>
#include <stdlib.h>
#include <wchar.h>

#include "resource.h"
#include "etw.h"

// Storage: linked list of fixed-size chunks.
#define CHUNK_CAP 131072

struct sample { int dx, dy; int64_t t; }; // t = QPC ticks at WM_INPUT dispatch
struct chunk  { struct sample data[CHUNK_CAP]; size_t sz; struct chunk *next; };

enum start_src { SRC_NONE, SRC_SPACE, SRC_LBUTTON };

static struct {
    int recording;
    enum start_src src;
    struct chunk *head, *tail;
    size_t count;
    int64_t total_dx, total_dy;
    int64_t qpf;              // QueryPerformanceFrequency (ticks/sec)
    int have_abs;            // last absolute position valid (RDP / tablet path)
    LONG last_abs_x, last_abs_y;
    HANDLE power_req;
    int timer_period_set;
    int cursor_hidden;
    int saving;
    int storage_failed;

    HANDLE device;           // the mouse being tested; other devices are ignored
    int    have_device;      // one has been picked (device itself may be NULL)
    wchar_t device_name[56]; // what to call it on screen

    int admin;               // process is elevated: kernel timestamps possible
    int kernel_active;       // ETW session running for the current recording
    int stopping;            // draining/pairing at the end of a kernel recording
    struct etw_pairing pairing; // sample timestamps (valid when pair_ok)
    int      pair_ok;

    wchar_t result[512];     // how the recording turned out; survives a save
    wchar_t msg[512];        // outcome of the last save, or an error
} G;

static HWND  g_hwnd;
static HFONT g_font;
static int   g_dpi = 96;
#define DP(x) MulDiv((x), g_dpi, 96)

static int64_t qpc_now(void) {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return c.QuadPart;
}

static int is_elevated(void) {
    HANDLE tok;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return 0;
    TOKEN_ELEVATION el;
    DWORD sz = 0;
    int r = GetTokenInformation(tok, TokenElevation, &el, sizeof el, &sz) &&
            el.TokenIsElevated;
    CloseHandle(tok);
    return r;
}

// One call into hid.dll, declared here rather than including hidsdi.h, which
// drags a good deal of the DDK along with it.
BOOLEAN WINAPI HidD_GetProductString(HANDLE dev, PVOID buf, ULONG len);

static void copy_clipped(wchar_t *out, size_t cap, const wchar_t *s) {
    if ((size_t)lstrlenW(s) < cap) { lstrcpynW(out, s, (int)cap); return; }
    lstrcpynW(out, s, (int)cap - 3);   // lstrcpynW's count includes the NUL
    lstrcatW(out, L"...");
}

// What to call the picked mouse. Raw Input knows only the device interface
// path, so ask the HID stack for the product string behind it -- a real name
// like "G502 HERO Gaming Mouse" -- and fall back to the vendor and product ids
// the path itself spells out.
static void device_label(HANDLE dev, wchar_t *out, size_t cap) {
    out[0] = 0;
    wchar_t path[512];
    UINT n = (UINT)(sizeof path / sizeof *path);
    if (GetRawInputDeviceInfoW(dev, RIDI_DEVICENAME, path, &n) == (UINT)-1) {
        lstrcpynW(out, L"unidentified device", (int)cap);
        return;
    }

    // A mouse is already open exclusively for the system's own use, so ask for
    // no access at all; that is enough to read its strings and is the only open
    // that succeeds.
    HANDLE h = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        wchar_t product[128];
        product[0] = 0;
        if (HidD_GetProductString(h, product, sizeof product) && product[0])
            copy_clipped(out, cap, product);
        CloseHandle(h);
    }
    if (out[0]) return;

    // e.g. \\?\HID#VID_046D&PID_C08B&... -- keep the ids, drop the plumbing.
    const wchar_t *v = wcsstr(path, L"VID_"), *p = wcsstr(path, L"PID_");
    if (v && p && lstrlenW(v) > 8 && lstrlenW(p) > 8) {
        wchar_t vid[5] = {0}, pid[5] = {0};
        for (int i = 0; i < 4; i++) { vid[i] = v[4 + i]; pid[i] = p[4 + i]; }
        wsprintfW(out, L"VID %s PID %s", vid, pid);
    } else {
        lstrcpynW(out, L"unnamed device", (int)cap);
    }
}

// Raw Input carries no cursor position, so both the device picker and the
// click-to-record gesture ask the cursor where it is.
static int cursor_in_client(HWND hwnd) {
    POINT p;
    GetCursorPos(&p);
    RECT cr;
    GetClientRect(hwnd, &cr);
    POINT tl = {0, 0};
    ClientToScreen(hwnd, &tl);
    return p.x >= tl.x && p.y >= tl.y &&
           p.x < tl.x + cr.right && p.y < tl.y + cr.bottom;
}

static BOOL register_raw_mouse(DWORD flags) {
    RAWINPUTDEVICE rid = {0};
    rid.usUsagePage = 0x01;
    rid.usUsage = 0x02;
    rid.dwFlags = flags | RIDEV_DEVNOTIFY;
    rid.hwndTarget = g_hwnd;
    return RegisterRawInputDevices(&rid, 1, sizeof rid);
}

// --------------------------------------------------------------------------
// Sample storage
// --------------------------------------------------------------------------
static void free_chunks(void) {
    struct chunk *c = G.head;
    while (c) {
        struct chunk *n = c->next;
        VirtualFree(c, 0, MEM_RELEASE);
        c = n;
    }
    G.head = G.tail = NULL;
    G.count = 0;
    G.total_dx = G.total_dy = 0;
}

static struct chunk *new_chunk(void) {
    struct chunk *c = VirtualAlloc(NULL, sizeof *c, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!c) return NULL;
    // Fault every page in now rather than one at a time while storing reports.
    for (size_t i = 0; i < sizeof *c; i += 4096)
        ((volatile BYTE *)c)[i] = 0;
    return c;
}

static int append_sample(int dx, int dy, int64_t t) {
    if (G.tail->sz == CHUNK_CAP) {
        struct chunk *n = new_chunk();
        if (!n) { G.storage_failed = 1; return 0; }
        G.tail->next = n;
        G.tail = n;
    }
    G.tail->data[G.tail->sz++] = (struct sample){ dx, dy, t };
    G.count++;
    G.total_dx += dx;
    G.total_dy += dy;
    return 1;
}

static void free_pairing(void) {
    etw_free_pairing(&G.pairing);
    G.pair_ok = 0;
}

// --------------------------------------------------------------------------
// Keep the input thread responsive without elevating the ETW drain thread.
// --------------------------------------------------------------------------
static void tuning_begin(void) {
    // Priority 15 in a normal process; the ETW consumer remains priority 7.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    if (timeBeginPeriod(1) == TIMERR_NOERROR) G.timer_period_set = 1;

    REASON_CONTEXT rc = {0};
    rc.Version = POWER_REQUEST_CONTEXT_VERSION;
    rc.Flags = POWER_REQUEST_CONTEXT_SIMPLE_STRING;
    rc.Reason.SimpleReasonString = L"MousePlotter recording";
    G.power_req = PowerCreateRequest(&rc);
    if (G.power_req == INVALID_HANDLE_VALUE) {
        G.power_req = NULL;
    } else {
        PowerSetRequest(G.power_req, PowerRequestExecutionRequired);
        PowerSetRequest(G.power_req, PowerRequestSystemRequired);
    }
}

static void tuning_end(void) {
    if (G.power_req) {
        PowerClearRequest(G.power_req, PowerRequestExecutionRequired);
        PowerClearRequest(G.power_req, PowerRequestSystemRequired);
        CloseHandle(G.power_req);
        G.power_req = NULL;
    }
    if (G.timer_period_set) {
        timeEndPeriod(1);
        G.timer_period_set = 0;
    }
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
}

// --------------------------------------------------------------------------
// CSV export. Without kernel timestamps the columns are the MouseTester ones,
// xCount,yCount,Time(ms). ETW files also carry userTime and timestampSource.
// The web app plots eventTime and accepts the additional diagnostic columns.
// --------------------------------------------------------------------------
static int write_all(HANDLE h, const void *buf, int len) {
    const BYTE *p = buf;
    while (len > 0) {
        DWORD wrote = 0;
        if (!WriteFile(h, p, (DWORD)len, &wrote, NULL) || !wrote) return 0;
        p += wrote;
        len -= (int)wrote;
    }
    return 1;
}

// QPC tick delta -> whole ms plus 6 fractional digits (wsprintf has no float).
static void ticks_ms(int64_t d, int64_t q, int *ms, int *frac6) {
    if (d < 0) d = 0;
    int64_t sec = d / q;
    int64_t ns = (d % q) * 1000000000LL / q;
    *ms = (int)(sec * 1000 + ns / 1000000);
    *frac6 = (int)(ns % 1000000);
}

// Name the most-used clock in the CSV/HTML plot title. Per-row source labels
// retain the details of any fallback; ties favor the earlier kernel source.
// The plot title line, from the mouse's name. The device supplies the name:
// keep control characters out of the CSV and "<" out of the HTML report's
// <script> block.
static int write_title(HANDLE h) {
    char name[3 * 56]; // device_name in UTF-8
    if (WideCharToMultiByte(CP_UTF8, 0, G.device_name, -1, name, sizeof name,
                            NULL, NULL) == 0)
        lstrcpyA(name, "unnamed device");
    for (char *p = name; *p; p++)
        if ((unsigned char)*p < ' ' || *p == '<') *p = '?';
    char line[sizeof name + 32];
    int n = wsprintfA(line, "%s (MousePlotter Windows)\r\n", name);
    return write_all(h, line, n);
}

static int write_csv_regular(HANDLE h) {
    static const char header[] = "800\r\nxCount,yCount,Time (ms)\r\n";
    if (!write_title(h) || !write_all(h, header, sizeof header - 1)) return 0;

    int64_t t0 = G.head->data[0].t, q = G.qpf;
    char row[64];
    for (struct chunk *c = G.head; c; c = c->next)
        for (size_t j = 0; j < c->sz; j++) {
            int ms, fr;
            ticks_ms(c->data[j].t - t0, q, &ms, &fr);
            int len = wsprintfA(row, "%d,%d,%d.%06d\r\n",
                                c->data[j].dx, c->data[j].dy, ms, fr);
            if (!write_all(h, row, len)) return 0;
        }
    return 1;
}

static int write_csv_paired(HANDLE h) {
    static const char header[] =
        "800\r\nxCount,yCount,eventTime (ms),userTime (ms),timestampSource\r\n";
    if (!write_title(h) || !write_all(h, header, sizeof header - 1)) return 0;

    const struct etw_time *times = G.pairing.times;
    int64_t q = G.qpf;
    // One QPC origin preserves the observed event-to-dispatch delay.
    int64_t t0 = times[0].t;

    static const char *sources[] = {"raw", "completion", "interrupt"};
    char row[128];
    size_t r = 0;
    for (struct chunk *c = G.head; c; c = c->next)
        for (size_t j = 0; j < c->sz; j++, r++) {
            const struct sample *s = &c->data[j];
            int kms, kfr, ums, ufr;
            ticks_ms(times[r].t - t0, q, &kms, &kfr);
            ticks_ms(s->t - t0, q, &ums, &ufr);
            int len = wsprintfA(row, "%d,%d,%d.%06d,%d.%06d,%s\r\n",
                                s->dx, s->dy, kms, kfr, ums, ufr, sources[times[r].source]);
            if (!write_all(h, row, len)) return 0;
        }
    return 1;
}

static int write_csv_file(HANDLE h) {
    return G.pair_ok ? write_csv_paired(h) : write_csv_regular(h);
}

static wchar_t *basename_w(wchar_t *s) {
    wchar_t *b = s;
    for (wchar_t *p = s; *p; p++)
        if (*p == L'\\' || *p == L'/') b = p + 1;
    return b;
}

static void exe_dir(wchar_t *dir, size_t cap) {
    DWORD n = GetModuleFileNameW(NULL, dir, (DWORD)cap);
    if (n == 0 || n >= cap) { dir[0] = 0; return; }
    *basename_w(dir) = 0;
}

// dir + the mouse's name and the time, NAME-YYYYMMDD-HHMMSS.ext, with the
// characters Windows doesn't allow in file names as "_"; returns -1 when it
// does not fit.
static int stamped_path(wchar_t *path, size_t cap, const wchar_t *dir, const wchar_t *ext) {
    wchar_t name[sizeof G.device_name / sizeof *G.device_name];
    lstrcpynW(name, G.device_name, (int)(sizeof name / sizeof *name));
    for (wchar_t *p = name; *p; p++)
        if (*p < L' ' || wcschr(L"/\\:*?\"<>|", *p)) *p = L'_';
    SYSTEMTIME st;
    GetLocalTime(&st);
    int n = _snwprintf(path, cap, L"%s%s-%04u%02u%02u-%02u%02u%02u.%s",
                       dir, name, st.wYear, st.wMonth, st.wDay,
                       st.wHour, st.wMinute, st.wSecond, ext);
    return n < 0 || (size_t)n >= cap ? -1 : 0;
}

static HANDLE create_stamped(wchar_t *path, size_t cap,
                             const wchar_t *dir, const wchar_t *ext) {
    if (stamped_path(path, cap, dir, ext)) {
        SetLastError(ERROR_FILENAME_EXCED_RANGE);
        return INVALID_HANDLE_VALUE;
    }
    return CreateFileW(path, GENERIC_WRITE, 0, NULL,
                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
}

static HANDLE create_output(wchar_t *path, size_t cap, const wchar_t *ext) {
    wchar_t dir[MAX_PATH];
    exe_dir(dir, MAX_PATH);
    HANDLE h = create_stamped(path, cap, dir, ext);
    if (h != INVALID_HANDLE_VALUE || GetLastError() != ERROR_ACCESS_DENIED)
        return h;
    wchar_t docs[MAX_PATH];
    if (SHGetFolderPathW(NULL, CSIDL_PERSONAL, NULL, SHGFP_TYPE_CURRENT, docs) != S_OK)
        return INVALID_HANDLE_VALUE;
    if ((size_t)lstrlenW(docs) + 2 > sizeof docs / sizeof *docs) {
        SetLastError(ERROR_FILENAME_EXCED_RANGE);
        return INVALID_HANDLE_VALUE;
    }
    lstrcatW(docs, L"\\");
    return create_stamped(path, cap, docs, ext);
}

static void save_csv(void) {
    wchar_t dir[MAX_PATH];
    exe_dir(dir, MAX_PATH);

    wchar_t path[MAX_PATH];
    stamped_path(path, MAX_PATH, L"", L"csv");

    OPENFILENAMEW ofn = {0};
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter = L"CSV files\0*.csv\0All files\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrInitialDir = dir[0] ? dir : NULL;
    ofn.lpstrDefExt = L"csv";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    G.saving = 1;
    BOOL ok = GetSaveFileNameW(&ofn);
    G.saving = 0;
    if (!ok) return; // cancelled: leave the previous result line alone

    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        wsprintfW(G.msg, L"Could not open file for writing.");
        return;
    }
    ok = write_csv_file(h);
    if (!CloseHandle(h)) ok = 0;
    if (!ok) {
        wsprintfW(G.msg, L"Write failed; the file may be incomplete. Recording retained.");
        return;
    }
    wsprintfW(G.msg, L"Data saved to %s", basename_w(path));
}

static const void *load_blob(int id, DWORD *len) {
    HRSRC r = FindResourceW(NULL, MAKEINTRESOURCEW(id), (LPCWSTR)RT_RCDATA);
    HGLOBAL h = r ? LoadResource(NULL, r) : NULL;
    if (!h) return NULL;
    *len = SizeofResource(NULL, r);
    return LockResource(h);
}

static void save_html_and_open(void) {
    DWORD head_len, tail_len;
    const void *head = load_blob(IDR_REPORT_HEAD, &head_len);
    const void *tail = load_blob(IDR_REPORT_TAIL, &tail_len);
    if (!head || !tail) {
        wsprintfW(G.msg, L"Report template resources are missing.");
        return;
    }

    wchar_t path[MAX_PATH + 64];
    HANDLE h = create_output(path, MAX_PATH + 64, L"html");
    if (h == INVALID_HANDLE_VALUE) {
        wsprintfW(G.msg, L"Could not open file for writing.");
        return;
    }
    int ok = write_all(h, head, (int)head_len) && write_csv_file(h) &&
             write_all(h, tail, (int)tail_len);
    if (!CloseHandle(h)) ok = 0;
    if (!ok) {
        wsprintfW(G.msg, L"Write failed; the file may be incomplete. Recording retained.");
        return;
    }
    wsprintfW(G.msg, L"Data saved to %s", basename_w(path));
    ShellExecuteW(NULL, L"open", path, NULL, NULL, SW_SHOWNORMAL);
}

// --------------------------------------------------------------------------
// Recording control
// --------------------------------------------------------------------------
static void cursor_capture(void) {
    if (G.cursor_hidden) return;
    POINT p;
    GetCursorPos(&p);
    RECT pin = { p.x, p.y, p.x + 1, p.y + 1 };
    ClipCursor(&pin);
    ShowCursor(FALSE);
    G.cursor_hidden = 1;
}

static void cursor_release(void) {
    if (!G.cursor_hidden) return;
    ClipCursor(NULL);
    ShowCursor(TRUE);
    G.cursor_hidden = 0;
}

static void start_recording(enum start_src src) {
    if (G.recording) return;
    struct chunk *first = new_chunk();
    if (!first) {
        wsprintfW(G.msg, L"Out of memory.");
        return;
    }
    if (!register_raw_mouse(RIDEV_NOLEGACY)) {
        VirtualFree(first, 0, MEM_RELEASE);
        wsprintfW(G.msg, L"Could not register mouse input.");
        return;
    }
    free_chunks();
    free_pairing();
    G.head = G.tail = first;
    G.have_abs = 0;
    G.storage_failed = 0;
    G.result[0] = 0;
    G.msg[0] = 0;
    G.src = src;
    G.recording = 1;
    // The trace comes up before any recorded WM_INPUT is dispatched. Reports
    // that queue meanwhile are still recorded, but with bunched dequeue times
    // and no completions, so they pair as raw.
    G.kernel_active = G.admin && (etw_start(G.device) == 0);
    cursor_capture();
    tuning_begin();
    InvalidateRect(g_hwnd, NULL, FALSE);
    UpdateWindow(g_hwnd);
}

// After stopping, pair the kernel timestamps onto the recorded samples. Builds
// the user-time array from the chunks, asks etw.c for the aligned kernel times,
// and records whether the pairing is trustworthy.
static void pair_kernel(void) {
    G.pair_ok = 0;
    if (!G.kernel_active || G.count == 0) return;

    // The pairing keeps only its chosen times; the chunks retain the user times.
    int64_t *user_t = malloc(G.count * sizeof *user_t);
    if (!user_t) return;
    size_t i = 0;
    for (struct chunk *c = G.head; c; c = c->next)
        for (size_t j = 0; j < c->sz; j++)
            user_t[i++] = c->data[j].t;

    G.pair_ok = etw_pair(user_t, G.count, &G.pairing, G.qpf) == 0;
    free(user_t);
}

// How the recording turned out. Kept apart from G.msg so that saving a file
// cannot overwrite it; the "press C / H" hint is drawn separately again, so no
// two of the three ever share a line.
static void set_result_message(void) {
    if (G.storage_failed) {
        wsprintfW(G.result, L"Recording stopped: out of memory. Captured samples retained.");
        return;
    }
    if (G.count == 0) {
        wsprintfW(G.result, L"No samples captured.");
        return;
    }
    if (!G.admin)
        G.result[0] = 0;                // regular mode: nothing to report
    else if (!G.kernel_active)
        wsprintfW(G.result, L"ETW session did not start; saved user timestamps.");
    else if (!G.pair_ok)
        wsprintfW(G.result, L"Kernel pairing failed; saved user timestamps.");
    // Which event supplied the kernel times is on the status line above, so
    // this carries only the pairing result and stays inside the window.
    else if (!G.pairing.raw && !G.pairing.unmatched && !G.pairing.shared)
        wsprintfW(G.result, L"All samples paired; delay avg %u us, std %u us",
                  (unsigned)(G.pairing.lag_us + 0.5),
                  (unsigned)(G.pairing.resid_us + 0.5));
    else
        wsprintfW(G.result, L"Unpaired: %u samples, %u USB reports; shared IRQ: %u",
                  (unsigned)G.pairing.raw, (unsigned)G.pairing.unmatched,
                  (unsigned)G.pairing.shared);
}

static void stop_recording(void) {
    if (!G.recording) return;
    G.recording = 0;
    G.src = SRC_NONE;
    register_raw_mouse(0);
    cursor_release();
    tuning_end();
    if (G.kernel_active) {
        // Draining the session and pairing takes a moment on a long capture;
        // say so rather than leaving "Recording" on screen while it happens.
        G.stopping = 1;
        InvalidateRect(g_hwnd, NULL, FALSE);
        UpdateWindow(g_hwnd);
        etw_stop();
        pair_kernel();
        G.stopping = 0;
    }
    set_result_message();
    InvalidateRect(g_hwnd, NULL, FALSE);
}

// --------------------------------------------------------------------------
// Painting (start/stop boundaries only; never during a recording).
// --------------------------------------------------------------------------
static void line_out(HDC dc, int pad, int *y, const wchar_t *s) {
    TextOutW(dc, pad, *y, s, lstrlenW(s));
    *y += DP(24);
}

static void draw(HDC dc, RECT rc) {
    FillRect(dc, &rc, (HBRUSH)(COLOR_WINDOW + 1));
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, g_font);

    const int pad = DP(18);
    int y = pad;

    if (G.stopping) {
        SetTextColor(dc, RGB(20, 20, 20));
        line_out(dc, pad, &y, L"Collecting kernel timestamps...");
        return;
    }

    if (G.recording) {
        SetTextColor(dc, RGB(200, 30, 30));
        line_out(dc, pad, &y,
                 G.kernel_active ? L"Recording (kernel timestamps)" : L"Recording");
        return;
    }

    SetTextColor(dc, RGB(20, 20, 20));

    if (!G.have_device) {
        line_out(dc, pad, &y, L"Click here with the mouse you want to test.");
        return;
    }

    wchar_t line[128];
    line_out(dc, pad, &y,
             L"Press Space or click and hold to record, "
             L"Esc to re-select mouse.");

    y += DP(6);
    // Where the samples come from: which mouse, and which clock stamps it.
    wsprintfW(line, L"Mouse:  %s", G.device_name);
    line_out(dc, pad, &y, line);
    // Which ETW event supplies the kernel time is only settled once a recording
    // has been paired, so until then this says what the session will reach for
    // rather than claiming a source it has not used yet.
    line_out(dc, pad, &y,
             !G.admin                    ? L"Source: Raw Input - run as admin for kernel timestamps"
             : G.count > 0 && !G.pair_ok ? L"Source: Raw Input - kernel pairing failed"
             : !G.pair_ok                ? L"Source: ETW kernel timestamps"
             : G.pairing.raw              ? L"Source: ETW / Raw Input (mixed)"
             : G.pairing.interrupts && G.pairing.completions
                                          ? L"Source: ETW interrupt / completion (mixed)"
             : G.pairing.interrupts       ? L"Source: ETW xHCI interrupt"
                                         : L"Source: ETW USB completion");

    y += DP(6);
    wsprintfW(line, L"Events:   %u", (unsigned)G.count);
    line_out(dc, pad, &y, line);
    wsprintfW(line, L"Total X:  %ld", (long)G.total_dx);
    line_out(dc, pad, &y, line);
    wsprintfW(line, L"Total Y:  %ld", (long)G.total_dy);
    line_out(dc, pad, &y, line);

    // How the recording went, what can be done with it, then where it went.
    y += DP(6);
    if (G.result[0])
        line_out(dc, pad, &y, G.result);
    if (G.count > 0)
        line_out(dc, pad, &y, L"Press H to save and view HTML, C to save CSV.");
    if (G.msg[0])
        line_out(dc, pad, &y, G.msg);
}

// --------------------------------------------------------------------------
// Window procedure
// --------------------------------------------------------------------------
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        g_font = CreateFontW(-DP(16), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        if (!g_font) g_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        return 0;

    case WM_INPUT: {
        if (G.saving) break;
        int64_t t = qpc_now();
        BYTE buf[sizeof(RAWINPUT) + 32];
        UINT size = sizeof buf;
        if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, buf, &size,
                            sizeof(RAWINPUTHEADER)) == (UINT)-1)
            break;
        RAWINPUT *ri = (RAWINPUT *)buf;
        if (ri->header.dwType != RIM_TYPEMOUSE)
            break;
        RAWMOUSE *m = &ri->data.mouse;
        USHORT btn = m->usButtonFlags;

        // Pick the mouse to test: the first click inside the window names it,
        // and nothing else is looked at until then. Recording one device keeps
        // a second mouse from interleaving reports into the sample stream,
        // which no kernel event sequence could then line up with.
        if (!G.have_device) {
            if ((btn & RI_MOUSE_LEFT_BUTTON_DOWN) && cursor_in_client(hwnd)) {
                G.device = ri->header.hDevice;
                G.have_device = 1;
                G.have_abs = 0;
                device_label(G.device, G.device_name, 56);
                InvalidateRect(hwnd, NULL, FALSE);
            }
            break;
        }
        if (ri->header.hDevice != G.device)
            break;

        int dx, dy;
        if (m->usFlags & MOUSE_MOVE_ABSOLUTE) {
            dx = G.have_abs ? (int)(m->lLastX - G.last_abs_x) : 0;
            dy = G.have_abs ? (int)(m->lLastY - G.last_abs_y) : 0;
            G.last_abs_x = m->lLastX;
            G.last_abs_y = m->lLastY;
            G.have_abs = 1;
        } else {
            dx = m->lLastX;
            dy = m->lLastY;
        }

        if (!G.recording) {
            if ((btn & RI_MOUSE_LEFT_BUTTON_DOWN) && cursor_in_client(hwnd))
                start_recording(SRC_LBUTTON);
            break;
        }

        if (G.src == SRC_LBUTTON && (btn & RI_MOUSE_LEFT_BUTTON_UP))
            stop_recording();
        else if (!append_sample(dx, dy, t))
            stop_recording();
        break;
    }

    case WM_KEYDOWN:
        if (G.saving) return 0;
        if (wp == VK_SPACE && !(lp & (1 << 30)) && G.have_device) {
            if (!G.recording)
                start_recording(SRC_SPACE);
            else if (G.src == SRC_SPACE)
                stop_recording();
        } else if (wp == 'C' && !G.recording && G.count > 0) {
            save_csv();
            InvalidateRect(hwnd, NULL, FALSE);
        } else if (wp == 'H' && !G.recording && G.count > 0) {
            save_html_and_open();
            InvalidateRect(hwnd, NULL, FALSE);
        } else if (wp == VK_ESCAPE) {
            // Escape only ever backs out: it stops a recording, then releases
            // the mouse. It never closes the window, so a second press at the
            // picker cannot throw away the capture that the first press kept.
            if (G.recording)
                stop_recording();
            else if (G.have_device) {
                G.have_device = 0;
                G.device = NULL;
                InvalidateRect(hwnd, NULL, FALSE);
            }
        }
        return 0;

    case WM_SETFOCUS:
        if (G.recording) cursor_capture();
        return 0;

    case WM_KILLFOCUS:
        // Foreground-only Raw Input stops on focus loss; do not leave ETW
        // collecting a gap that could be mistaken for missing mouse reports.
        if (G.recording) stop_recording();
        cursor_release();
        return 0;

    case WM_INPUT_DEVICE_CHANGE:
        if (wp == GIDC_REMOVAL && (HANDLE)lp == G.device) {
            if (G.recording) stop_recording();
            G.have_device = 0;
            G.device = NULL;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HBITMAP oldbmp = (HBITMAP)SelectObject(mem, bmp);
        draw(mem, rc);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldbmp);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_DESTROY:
        if (G.recording) { tuning_end(); if (G.kernel_active) etw_stop(); }
        cursor_release();
        free_chunks();
        free_pairing();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrevInstance, PWSTR pCmdLine, int nCmdShow) {
    (void)hPrevInstance; (void)pCmdLine; (void)nCmdShow;

    HDC screen = GetDC(NULL);
    g_dpi = GetDeviceCaps(screen, LOGPIXELSX);
    ReleaseDC(NULL, screen);
    if (g_dpi < 96) g_dpi = 96;

    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    G.qpf = f.QuadPart;
    G.admin = is_elevated();

    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"MousePlotterWin";
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hIcon = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
        GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    wc.hIconSm = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    RegisterClassExW(&wc);

    DWORD styleflags = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    // Fits the widest line (the Space/Esc prompt, 441 px) and, at full height,
    // nine lines with three gaps between them.
    RECT wr = {0, 0, DP(480), DP(260)};
    AdjustWindowRect(&wr, styleflags, FALSE);
    const wchar_t *title = G.admin ? L"MousePlotter Windows Logger (ETW)"
                                   : L"MousePlotter Windows logger";
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, title,
        styleflags, CW_USEDEFAULT, CW_USEDEFAULT,
        wr.right - wr.left, wr.bottom - wr.top, NULL, NULL, hInst, NULL);
    if (!g_hwnd) ExitProcess(1);

    if (!register_raw_mouse(0)) {
        MessageBoxW(g_hwnd, L"RegisterRawInputDevices failed.", L"MousePlotter", MB_ICONERROR);
        ExitProcess(1);
    }

    ShowWindow(g_hwnd, SW_SHOWNORMAL);
    UpdateWindow(g_hwnd);
    SetForegroundWindow(g_hwnd);
    SetFocus(g_hwnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    ExitProcess((UINT)msg.wParam);
}
