// SPDX-FileCopyrightText: 2026 XBAB Tech, LLC
// SPDX-License-Identifier: MIT
//
// MousePlotter Windows GUI logger.
//
// Why a GUI and not a CLI (unlike linux_cli/log.c): on recent Windows the only
// way to get raw, un-coalesced, one-sample-per-report mouse motion is the Raw
// Input API (WM_INPUT / RAWMOUSE), and Raw Input requires a window with a
// message pump. A console application receives only coalesced pointer motion,
// merged toward the display/poll cadence. The Linux tool can instead read the
// evdev device node directly, so it needs no window.
//
// One honest caveat vs. Linux: Windows does not expose a kernel/hardware
// timestamp for raw mouse reports, so the best available stamp is
// QueryPerformanceCounter() taken as we dequeue WM_INPUT. That is analogous to
// the Linux tool's "userTime" (it includes OS scheduling jitter), not its
// kernel "eventTime". The real-time tuning below exists to keep that jitter low.
//
// Requires the Universal CRT: in-box on Windows 10 and 11, and available on
// Windows 7 / 8.1 through the KB2999226 update. Build with build.bat or the
// Makefile.

#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601 // Windows 7 floor
#endif
#ifndef WINVER
#define WINVER 0x0601
#endif

#include <windows.h>
#include <commdlg.h>
#include <timeapi.h>
#include <powrprof.h>
#include <stddef.h>
#include <stdint.h>

#include "resource.h"

// Storage: linked list of fixed-size chunks (mirrors linux_cli). Overflow past
// CHUNK_CAP is rare; we just allocate a new chunk, with no copying.
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
    int saving;              // Save dialog open: ignore input during its modal loop
    int cursor_hidden;       // cursor currently confined + hidden for a recording
    wchar_t msg[512];        // last status message (save result, etc.)
} G;

static HWND  g_hwnd;
static HFONT g_font;
static int   g_dpi = 96;      // system DPI; all layout is scaled by g_dpi / 96
#define DP(x) MulDiv((x), g_dpi, 96)

static int64_t qpc_now(void) {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return c.QuadPart;
}

static BOOL register_raw_mouse(DWORD flags) {
    RAWINPUTDEVICE rid = {0};
    rid.usUsagePage = 0x01; // generic desktop controls
    rid.usUsage = 0x02;     // mouse
    rid.dwFlags = flags;
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
        VirtualUnlock(c, sizeof *c);
        VirtualFree(c, 0, MEM_RELEASE);
        c = n;
    }
    G.head = G.tail = NULL;
}

static struct chunk *new_chunk(void) {
    // VirtualAlloc hands back zeroed pages (so sz/next start at 0); VirtualLock
    // faults them all resident now to avoid page-fault latency mid-record.
    struct chunk *c = VirtualAlloc(NULL, sizeof *c, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!c) return NULL;
    VirtualLock(c, sizeof *c);
    return c;
}

static void append_sample(int dx, int dy, int64_t t) {
    if (G.tail->sz == CHUNK_CAP) {
        struct chunk *n = new_chunk();
        if (!n) return; // out of memory: drop the sample rather than crash
        G.tail->next = n;
        G.tail = n;
    }
    G.tail->data[G.tail->sz++] = (struct sample){ dx, dy, t };
    G.count++;
    G.total_dx += dx;
    G.total_dy += dy;
}

// --------------------------------------------------------------------------
// Real-time tuning (the Windows analogue of linux_cli's governor / PM QoS /
// mlockall). Applied only for the duration of a recording session.
// --------------------------------------------------------------------------
static void tuning_begin(void) {
    if (!SetPriorityClass(GetCurrentProcess(), REALTIME_PRIORITY_CLASS))
        SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
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
        // Keep the system awake and executing while we record. This is the
        // closest documented user-mode lever to Linux's /dev/cpu_dma_latency;
        // deeper C-state control still needs a High-Performance power plan / BIOS.
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
    SetPriorityClass(GetCurrentProcess(), NORMAL_PRIORITY_CLASS);
}

// --------------------------------------------------------------------------
// CSV export (MousePlotter / MouseTester format). The web app's parser reads
// columns 0,1,2 -> xCount, yCount, Time (ms); see public/app.js parseCsv().
// --------------------------------------------------------------------------
static void write_all(HANDLE h, const void *buf, int len) {
    DWORD wrote;
    WriteFile(h, buf, (DWORD)len, &wrote, NULL);
}

// Title, DPI (hard-coded 800 like linux_cli; change it in the web UI on import),
// the column header, then one "dx,dy,ms" row per report (ms relative to the
// first sample). Byte-for-byte the format the web app itself exports. wsprintfA
// has no floating-point support, so the time is built from integer math (whole
// ms + 6 fractional digits).
static void write_csv_file(HANDLE h) {
    static const char header[] =
        "MousePlotter Windows logger\r\n800\r\nxCount,yCount,Time (ms)\r\n";
    write_all(h, header, (int)(sizeof header - 1));

    int64_t t0 = G.head->data[0].t;
    int64_t q = G.qpf;
    char row[64];
    for (struct chunk *c = G.head; c; c = c->next) {
        for (size_t j = 0; j < c->sz; j++) {
            int64_t d = c->data[j].t - t0;             // ticks since first sample
            int64_t sec = d / q;
            int64_t ns = (d % q) * 1000000000LL / q;   // ns within the second
            int ms_int = (int)(sec * 1000 + ns / 1000000);
            int frac6 = (int)(ns % 1000000);
            int n = wsprintfA(row, "%d,%d,%d.%06d\r\n",
                              c->data[j].dx, c->data[j].dy, ms_int, frac6);
            write_all(h, row, n);
        }
    }
}

static const wchar_t *basename_w(const wchar_t *s) {
    const wchar_t *b = s;
    for (const wchar_t *p = s; *p; p++)
        if (*p == L'\\' || *p == L'/') b = p + 1;
    return b;
}

static void do_save_csv(void) {
    wchar_t path[MAX_PATH] = L"log.csv";
    OPENFILENAMEW ofn = {0};
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter = L"CSV files\0*.csv\0All files\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"csv";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&ofn)) {
        wsprintfW(G.msg, L"Press S to save.");
        return;
    }

    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        wsprintfW(G.msg, L"Could not open file for writing.");
        return;
    }
    write_csv_file(h);
    CloseHandle(h);
    wsprintfW(G.msg, L"Saved %u samples to %s",
              (unsigned)G.count, basename_w(path));
}

// Wrap the modal dialog so raw input (including clicks inside the dialog itself)
// is ignored for its whole duration, no matter which branch do_save_csv() takes.
static void save_csv(void) {
    G.saving = 1;
    do_save_csv();
    G.saving = 0;
}

// --------------------------------------------------------------------------
// Recording control
// --------------------------------------------------------------------------

// Confine the cursor to a 1x1 rect at its current spot and hide it, so physical
// motion generates no WM_MOUSE* traffic in other windows (a jitter source) while
// we record. Raw Input still reports the full per-report deltas regardless of
// where the cursor is, so the pinned cursor costs us no data.
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
    free_chunks();
    G.head = G.tail = new_chunk();
    if (!G.head) {
        wsprintfW(G.msg, L"Out of memory.");
        return;
    }
    G.count = 0;
    G.total_dx = G.total_dy = 0;
    G.have_abs = 0;
    G.msg[0] = 0;
    G.src = src;
    G.recording = 1;
    register_raw_mouse(RIDEV_NOLEGACY);
    cursor_capture();
    tuning_begin();
    // Paint the recording state once, synchronously, here at the boundary. We do
    // NOT repaint during the session: any paint work would land on this same
    // thread as WM_INPUT and show up as a periodic pattern in the timestamps.
    InvalidateRect(g_hwnd, NULL, FALSE);
    UpdateWindow(g_hwnd);
}

static void stop_recording(void) {
    if (!G.recording) return;
    G.recording = 0;
    G.src = SRC_NONE;
    register_raw_mouse(0);
    cursor_release(); // restore the pointer before any Save dialog appears
    tuning_end();
    G.msg[0] = 0;
    InvalidateRect(g_hwnd, NULL, FALSE);
    UpdateWindow(g_hwnd); // show the final counts before any modal Save dialog
    if (G.count == 0)
        wsprintfW(G.msg, L"No samples captured.");
    else
        save_csv();
    InvalidateRect(g_hwnd, NULL, FALSE);
}

// --------------------------------------------------------------------------
// Painting. Only ever runs at the start/stop boundaries, never during a
// recording (see start_recording), so it cannot perturb the sample timing.
// Double-buffered so the boundary repaints do not flicker.
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

    if (G.recording) {
        SetTextColor(dc, RGB(200, 30, 30));
        line_out(dc, pad, &y, L"Recording");
        return;
    }

    SetTextColor(dc, RGB(20, 20, 20));
    wchar_t line[128];
    line_out(dc, pad, &y, L"Ready. Press SPACE or click and hold to record.");
    y += DP(6);
    wsprintfW(line, L"Events:   %u", (unsigned)G.count);
    line_out(dc, pad, &y, line);
    wsprintfW(line, L"Total X:  %ld", (long)G.total_dx);
    line_out(dc, pad, &y, line);
    wsprintfW(line, L"Total Y:  %ld", (long)G.total_dy);
    line_out(dc, pad, &y, line);
    if (G.msg[0]) {
        y += DP(6);
        line_out(dc, pad, &y, G.msg);
    }
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
        if (G.saving) break; // dialog's modal loop still pumps us; ignore input
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

        int dx, dy;
        if (m->usFlags & MOUSE_MOVE_ABSOLUTE) {
            // Absolute coordinates (RDP / tablet / VM): derive per-report deltas.
            dx = G.have_abs ? (int)(m->lLastX - G.last_abs_x) : 0;
            dy = G.have_abs ? (int)(m->lLastY - G.last_abs_y) : 0;
            G.last_abs_x = m->lLastX;
            G.last_abs_y = m->lLastY;
            G.have_abs = 1;
        } else {
            dx = m->lLastX;
            dy = m->lLastY;
        }
        USHORT btn = m->usButtonFlags;

        if (!G.recording) {
            // Start on a left-button press that lands inside our client area,
            // so clicking the title bar / close button never begins a session.
            if (btn & RI_MOUSE_LEFT_BUTTON_DOWN) {
                POINT p;
                GetCursorPos(&p);
                RECT cr;
                GetClientRect(hwnd, &cr);
                POINT tl = {0, 0};
                ClientToScreen(hwnd, &tl);
                if (p.x >= tl.x && p.y >= tl.y &&
                    p.x < tl.x + cr.right && p.y < tl.y + cr.bottom)
                    start_recording(SRC_LBUTTON); // the press report is not a sample
            }
            break;
        }

        // The button-release report ends the session but is not itself a sample,
        // so the first and last rows are motion, not the press/release.
        if (G.src == SRC_LBUTTON && (btn & RI_MOUSE_LEFT_BUTTON_UP))
            stop_recording();
        else
            append_sample(dx, dy, t);
        break; // WM_INPUT still needs DefWindowProc for cleanup
    }

    case WM_KEYDOWN:
        if (G.saving) return 0;                    // dialog open: ignore keys
        if (wp == VK_SPACE && !(lp & (1 << 30))) { // bit 30 set => auto-repeat
            // Space toggles: press to start, press again to stop. A mouse
            // click-and-hold session (SRC_LBUTTON) is left for the button to end.
            if (!G.recording)
                start_recording(SRC_SPACE);
            else if (G.src == SRC_SPACE)
                stop_recording();
        } else if (wp == 'S' && !G.recording && G.count > 0) {
            save_csv(); // re-open the dialog for the samples still in memory
            InvalidateRect(hwnd, NULL, FALSE);
        } else if (wp == VK_ESCAPE) {
            DestroyWindow(hwnd);
        }
        return 0;

    case WM_SETFOCUS:
        if (G.recording) cursor_capture(); // re-confine after returning to us
        return 0;

    case WM_KILLFOCUS:
        cursor_release(); // never leave the pointer stuck hidden/confined
        return 0;

    case WM_ERASEBKGND:
        return 1; // fully repainted in WM_PAINT; skip default erase (no flicker)

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
        if (G.recording) tuning_end();
        cursor_release();
        free_chunks();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrevInstance, PWSTR pCmdLine, int nCmdShow) {
    (void)hPrevInstance; (void)pCmdLine; (void)nCmdShow;

    // Render at true device pixels so text stays sharp on scaled displays, then
    // read the system DPI so the window and fonts are scaled up to match.
    // app.manifest already declares us system-DPI-aware, which is what actually
    // takes effect; this call is the fallback for a build without the resources
    // and is a harmless no-op otherwise.
    SetProcessDPIAware();
    HDC screen = GetDC(NULL);
    g_dpi = GetDeviceCaps(screen, LOGPIXELSX);
    ReleaseDC(NULL, screen);
    if (g_dpi < 96) g_dpi = 96;

    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    G.qpf = f.QuadPart;

    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"MousePlotterWin";
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    // Title bar and Alt-Tab want different sizes out of the icon group; asking
    // for each by name beats letting Windows stretch one. Both are NULL, and the
    // default icon appears, if this was built without log.rc.
    wc.hIcon = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
        GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    wc.hIconSm = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    RegisterClassExW(&wc);

    // Non-resizable window (no maximize / thick border), sized in DPI-scaled px.
    DWORD styleflags = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT wr = {0, 0, DP(430), DP(190)};
    AdjustWindowRect(&wr, styleflags, FALSE);
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"MousePlotter Windows logger",
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
