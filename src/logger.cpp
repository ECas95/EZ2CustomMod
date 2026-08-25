#include "logger.h"
#include "config.h"
#include <windows.h>
#include <cstdio>
#include <mutex>

static FILE* g_log = nullptr;
static std::mutex g_logMutex;
static void (*g_logSink)(const char*) = nullptr;

void SetLogSink(void (*sink)(const char* line)) { g_logSink = sink; }

void LogInit() {
    if (!Config().enableLog) return;
    std::string dir = JoinPath(GetExeDirectory(), "logs");
    CreateDirectoryA(dir.c_str(), nullptr);
    std::string path = JoinPath(dir, "EZ2CustomMod.log");
    fopen_s(&g_log, path.c_str(), "wb");
    if (g_log) {
        SYSTEMTIME st{};
        GetLocalTime(&st);
        fprintf(g_log, "===== EZ2CustomMod start %04d-%02d-%02d %02d:%02d:%02d =====\n",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        fflush(g_log);
    }
}

void LogClose() {
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_log) {
        fprintf(g_log, "===== EZ2CustomMod end =====\n");
        fclose(g_log);
        g_log = nullptr;
    }
}

void LogV(const char* prefix, const char* fmt, va_list ap) {
    if (!g_log && !g_logSink) return;
    char msg[4096] = {};
    vsnprintf(msg, sizeof(msg) - 1, fmt ? fmt : "(null)", ap);
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_log) {
        fprintf(g_log, "%s%s\n", prefix ? prefix : "", msg);
        // Flushing every line costs a syscall per log line (~20k lines during a scan, plus every
        // traced file open in-game). Batch: flush at most every 250ms — EXCEPT crash/exit lines,
        // which must hit the disk before the process dies (they are the whole point of the log).
        static DWORD s_lastFlush = 0, s_flushUntil = 0;
        DWORD now = GetTickCount();
        if (strstr(msg, "CRASH") || strstr(msg, "EXIT") || strstr(msg, "[throw]") || strstr(msg, "abort"))
            s_flushUntil = now + 2000; // crash dumps span several follow-up lines (stack walk) — flush them all
        if (now < s_flushUntil || now - s_lastFlush > 250) { fflush(g_log); s_lastFlush = now; }
    }
    if (g_logSink) {
        char full[4160] = {};
        snprintf(full, sizeof(full) - 1, "%s%s", prefix ? prefix : "", msg);
        g_logSink(full);
    }
}

void LogLine(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    LogV("", fmt, ap);
    va_end(ap);
}

void LogRaw(const std::string& s) {
    if (!g_log) return;
    std::lock_guard<std::mutex> lock(g_logMutex);
    fwrite(s.data(), 1, s.size(), g_log);
    fputc('\n', g_log);
    fflush(g_log);
}
