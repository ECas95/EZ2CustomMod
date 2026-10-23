#include "progress_popup.h"
#include "logger.h"
#include <windows.h>
#include <commctrl.h>
#include <mutex>
#include <string>
#include <deque>

static HWND g_wnd = nullptr;
static HWND g_bar = nullptr;
static HWND g_edit = nullptr;
static HANDLE g_thread = nullptr;
static DWORD g_threadId = 0;

// Real progress, set per-song by StartupProgress from the scan thread; read by WM_TIMER.
static std::mutex g_qMutex;
static std::deque<std::string> g_queue;
static int g_done = 0, g_total = 0;

void StartupProgress(int done, int total, const char* songLabel) {
    std::lock_guard<std::mutex> lock(g_qMutex);
    g_done = done; g_total = total;
    if (songLabel && *songLabel) {
        char line[300];
        wsprintfA(line, "[%d/%d] %s", done, total, songLabel);
        if (g_queue.size() > 4000) g_queue.pop_front(); // bound memory
        g_queue.emplace_back(line);
    }
}

static void FlushQueue() {
    std::deque<std::string> local;
    {
        std::lock_guard<std::mutex> lock(g_qMutex);
        local.swap(g_queue);
    }
    if (local.empty() || !g_edit) return;
    std::string chunk;
    for (auto& s : local) { chunk += s; chunk += "\r\n"; }
    int len = GetWindowTextLengthA(g_edit);
    SendMessageA(g_edit, EM_SETSEL, len, len);
    SendMessageA(g_edit, EM_REPLACESEL, FALSE, (LPARAM)chunk.c_str());
    SendMessageA(g_edit, EM_SCROLLCARET, 0, 0);
}

// Posted by CloseStartupPopup from another thread to tear the window down on
// the popup's own thread (DestroyWindow must run where the window was created).
#define WM_POPUP_CLOSE (WM_APP + 1)

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_TIMER: {
            FlushQueue();
            int done, total;
            { std::lock_guard<std::mutex> lock(g_qMutex); done = g_done; total = g_total; }
            if (g_bar && total > 0) {
                int pct = (int)((long long)done * 100 / total);
                if (pct > 100) pct = 100;
                SendMessageA(g_bar, PBM_SETPOS, pct, 0);
                char title[160];
                wsprintfA(title, "EZ2CustomMod - %d%%  (%d/%d songs)", pct, done, total);
                SetWindowTextA(h, title);
            }
            return 0;
        }
        case WM_POPUP_CLOSE:
            FlushQueue();        // drain remaining lines
            KillTimer(h, 1);
            DestroyWindow(h);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

static DWORD WINAPI PopupThread(LPVOID) {
    // Force the message queue to exist now so a PostThreadMessage from
    // CloseStartupPopup can never be dropped, even if the scan finishes fast.
    MSG seed;
    PeekMessageA(&seed, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_PROGRESS_CLASS };
    InitCommonControlsEx(&icc);

    WNDCLASSA wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "EZ2CustomModPopup";
    wc.hCursor = LoadCursorA(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassA(&wc);

    const int W = 560, H = 380;
    int sx = GetSystemMetrics(SM_CXSCREEN), sy = GetSystemMetrics(SM_CYSCREEN);
    g_wnd = CreateWindowExA(WS_EX_TOPMOST, "EZ2CustomModPopup",
        "EZ2CustomMod - Preparing custom songs...",
        WS_POPUP | WS_CAPTION | WS_VISIBLE,
        (sx - W) / 2, (sy - H) / 2, W, H,
        nullptr, nullptr, wc.hInstance, nullptr);

    g_bar = CreateWindowExA(0, PROGRESS_CLASSA, nullptr,
        WS_CHILD | WS_VISIBLE,
        12, 12, W - 36, 22, g_wnd, nullptr, wc.hInstance, nullptr);
    SendMessageA(g_bar, PBM_SETRANGE32, 0, 100);
    SendMessageA(g_bar, PBM_SETSTEP, 1, 0);

    g_edit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", nullptr,
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
        12, 44, W - 36, H - 92, g_wnd, nullptr, wc.hInstance, nullptr);
    HFONT font = (HFONT)GetStockObject(ANSI_FIXED_FONT);
    SendMessageA(g_edit, WM_SETFONT, (WPARAM)font, TRUE);
    SendMessageA(g_edit, EM_SETLIMITTEXT, 0, 0); // 0 = max; default ~30KB made the log stop following

    SetTimer(g_wnd, 1, 80, nullptr);

    MSG msg;
    while (GetMessageA(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    g_wnd = g_bar = g_edit = nullptr;
    return 0;
}

void ShowStartupPopup() {
    if (g_thread) return;
    // ponytail: no generic log sink — the popup shows ONLY per-song progress (StartupProgress),
    // so it can't flood/overflow the edit. Full detail still goes to the log file.
    g_thread = CreateThread(nullptr, 0, PopupThread, nullptr, 0, &g_threadId);
}

void CloseStartupPopup() {
    if (g_threadId) PostThreadMessageA(g_threadId, WM_POPUP_CLOSE, 0, 0);
    if (g_thread) {
        // Fallback: if the thread's window vanished before the posted message
        // landed, the message is lost; bound the wait and force-quit its loop.
        if (WaitForSingleObject(g_thread, 4000) == WAIT_TIMEOUT && g_threadId) {
            PostThreadMessageA(g_threadId, WM_QUIT, 0, 0);
            WaitForSingleObject(g_thread, 1000);
        }
        CloseHandle(g_thread);
        g_thread = nullptr;
        g_threadId = 0;
    }
}
