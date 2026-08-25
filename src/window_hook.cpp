#include "window_hook.h"
#include "iat.h"
#include "config.h"
#include "logger.h"

static HWND g_hwnd = nullptr;
static WNDPROC g_oldWndProc = nullptr;
static bool g_borderless = false;
static RECT g_oldRect{};
static DWORD g_oldStyle = 0;
static DWORD g_oldExStyle = 0;
static volatile LONG g_startupReady = 0;

using CreateWindowExA_t = HWND (WINAPI*)(DWORD,LPCSTR,LPCSTR,DWORD,int,int,int,int,HWND,HMENU,HINSTANCE,LPVOID);
static CreateWindowExA_t RealCreateWindowExA = nullptr;

static void ToggleBorderless() {
    if (!g_hwnd) {
        LogLine("[video] Alt+Enter ignored: no HWND captured");
        return;
    }
    if (!g_borderless) {
        g_oldStyle = GetWindowLongA(g_hwnd, GWL_STYLE);
        g_oldExStyle = GetWindowLongA(g_hwnd, GWL_EXSTYLE);
        GetWindowRect(g_hwnd, &g_oldRect);
        HMONITOR mon = MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoA(mon, &mi);
        SetWindowLongA(g_hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowLongA(g_hwnd, GWL_EXSTYLE, g_oldExStyle & ~(WS_EX_CLIENTEDGE | WS_EX_WINDOWEDGE));
        SetWindowPos(g_hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        g_borderless = true;
        LogLine("[video] borderless on hwnd=%p", g_hwnd);
    } else {
        SetWindowLongA(g_hwnd, GWL_STYLE, g_oldStyle);
        SetWindowLongA(g_hwnd, GWL_EXSTYLE, g_oldExStyle);
        SetWindowPos(g_hwnd, nullptr, g_oldRect.left, g_oldRect.top,
                     g_oldRect.right - g_oldRect.left,
                     g_oldRect.bottom - g_oldRect.top,
                     SWP_NOZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        g_borderless = false;
        LogLine("[video] borderless off hwnd=%p", g_hwnd);
    }
}

static LRESULT CALLBACK ModWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (Config().altEnter && msg == WM_SYSKEYDOWN && wp == VK_RETURN && (lp & (1 << 29))) {
        ToggleBorderless();
        return 0;
    }
    return CallWindowProcA(g_oldWndProc, hwnd, msg, wp, lp);
}

void CaptureGameWindow(HWND hwnd) {
    if (!hwnd || g_hwnd) return;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId()) return;

    g_hwnd = hwnd;
    g_oldWndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrA(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(ModWndProc)));
    LogLine("[window] captured hwnd=%p oldWndProc=%p title/class captured", hwnd, g_oldWndProc);
}

static BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId()) return TRUE;
    if (GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;
    if (!IsWindowVisible(hwnd)) return TRUE;

    char title[256] = {};
    char cls[256] = {};
    GetWindowTextA(hwnd, title, sizeof(title));
    GetClassNameA(hwnd, cls, sizeof(cls));
    LogLine("[window] existing candidate hwnd=%p class='%s' title='%s'", hwnd, cls, title);
    CaptureGameWindow(hwnd);
    return FALSE;
}

void CaptureExistingProcessWindow() {
    if (g_hwnd) return;
    EnumWindows(EnumWindowsProc, 0);
    if (!g_hwnd) LogLine("[window] no existing top-level visible window found for this process");
}

static HWND WINAPI HookCreateWindowExA(DWORD exStyle, LPCSTR className, LPCSTR windowName, DWORD style,
                                       int x, int y, int w, int h, HWND parent, HMENU menu,
                                       HINSTANCE inst, LPVOID param) {
    if (InterlockedCompareExchange(&g_startupReady, 0, 0) == 0) {
        LogLine("[startup] game window waiting for BMS cache and memory hooks");
        DWORD started = GetTickCount();
        // The game must NOT start until ScanBMSData finishes, else it loads incomplete custom data and
        // crashes (and a half-built cache then fails on the next launch too). First cache build scales
        // with pack size (~6 min per 3k songs; ~35k on XP-era hardware can take hours) — 6h covers it;
        // the popup shows live progress so a genuine hang is visible and killable.
        while (InterlockedCompareExchange(&g_startupReady, 0, 0) == 0 && GetTickCount() - started < 21600000) Sleep(10);
        LogLine("[startup] game window released ready=%d waitMs=%lu",
                InterlockedCompareExchange(&g_startupReady, 0, 0) ? 1 : 0,
                static_cast<unsigned long>(GetTickCount() - started));
    }
    HWND hwnd = RealCreateWindowExA(exStyle, className, windowName, style, x, y, w, h, parent, menu, inst, param);
    if (hwnd && !parent) {
        LogLine("[window] CreateWindowExA class='%s' title='%s' style=0x%08lx ex=0x%08lx size=%dx%d hwnd=%p",
                className ? className : "", windowName ? windowName : "", style, exStyle, w, h, hwnd);
        CaptureGameWindow(hwnd);
    }
    return hwnd;
}

void ReleaseStartupGate() { InterlockedExchange(&g_startupReady, 1); }

void InstallWindowApiHooks() {
    bool ok = HookIAT("USER32.dll", "CreateWindowExA", reinterpret_cast<void*>(HookCreateWindowExA), reinterpret_cast<void**>(&RealCreateWindowExA));
    LogLine("[window] IAT hook CreateWindowExA ok=%d real=%p", ok ? 1 : 0, RealCreateWindowExA);
}

void ShutdownWindowHook() {
    ReleaseStartupGate();
    if (g_hwnd && g_oldWndProc) {
        SetWindowLongPtrA(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_oldWndProc));
    }
    g_hwnd = nullptr;
    g_oldWndProc = nullptr;
}
