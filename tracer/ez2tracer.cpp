// ez2tracer.dll — separate full-execution tracer for EZ2B.exe (EZ2AC).
//
// Traces EVERY game .text instruction the process executes, from an "arm" address onward,
// using the trap flag (single-step) + a hardware breakpoint to skip library internals at full
// speed. Output is a compact BINARY stream of 4-byte EIP RVAs (decode with decode_trace.py).
//
// WHY binary + armed + capped (the honest ceiling):
//   Single-stepping is ~thousands× slower than native, so the game's frame-time WATCHDOG (it uses
//   GetTickCount/QPC/timeGetTime) will likely kill the process during a long trace. That is the real
//   limit — NOT file size. Mitigations baked in: (1) tracing is ARMED at a target function so the
//   game runs full-speed until you reach the interesting code; (2) only GAME .text is single-stepped,
//   library calls run free (HW bp on their return); (3) a hard instruction CAP stops the trace and
//   lets the game keep running; (4) binary output + buffered WriteFile so the trace survives a kill.
//   ponytail: if even the armed window trips the watchdog, the lazier-correct tool is DynamoRIO/Intel
//   Pin (JIT code-cache, ~50× not ~thousands×) — this DLL is the no-extra-tool version you asked for.
//
// Config via environment variables (set before launching the game):
//   EZ2TRACE_ARM=4448fe   hex RVA where tracing begins (default 0x4448fe, the category-change path)
//   EZ2TRACE_CAP=5000000  max game instructions to record, then stop (default 5,000,000)
//   EZ2TRACE_OUT=...      output path (default ez2trace.bin next to the exe)
//
// Build: python tracer/build_tracer.py   (or: g++ -O2 -shared -static -o ez2tracer.dll ez2tracer.cpp)

#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static uintptr_t g_base, g_textLo, g_textHi;
static uintptr_t g_armRva = 0x4448fe;
static unsigned long long g_cap = 40000000ULL, g_count = 0; // inject→abort can be long; raise via EZ2TRACE_CAP
static HANDLE g_out = INVALID_HANDLE_VALUE;
static BYTE   g_origByte = 0;        // original byte under the arm int3
static bool   g_armed = false;       // int3 currently planted
static volatile LONG g_tracing = 0;
static bool   g_stopped = false;
static bool   g_prevInText = false;    // was the previously-stepped instruction in game .text?

// 256 KiB write buffer of 4-byte RVAs, flushed via WriteFile every ~64K instructions so a hard
// watchdog kill (TerminateProcess, which bypasses our ExitProcess flush) still leaves data on disk.
static uint32_t g_buf[64 * 1024];
static unsigned g_bufN = 0;

static inline bool inText(uintptr_t a) { return a >= g_textLo && a < g_textHi; }

// --- Clock virtualization (EZ2TRACE_CLOCK) ---------------------------------------------------------
// timeGetTime is EZ2B's frame clock (30+ call sites). Single-stepping makes wall time jump, which
// blows up every dt-based path (and any time-based watchdog / timing anti-debug). We can't delete it
// (the game would freeze) — instead we IAT-hook it and, WHILE TRACING, return a clock scaled down by
// EZ2TRACE_CLOCK so the game perceives ~normal time. One hook covers all calls. Off (0/1) = untouched.
typedef DWORD (WINAPI *timeGetTime_t)(void);
static timeGetTime_t g_realTGT = nullptr;
static DWORD g_clockScale = 1000;       // ON by default (no command line to set EZ2TRACE_CLOCK); only
                                        // scales WHILE tracing, so normal play is unaffected. 0/1 = off.
static DWORD g_clkRealStart = 0, g_clkFakeBase = 0;
static bool  g_clkArmed = false;

static DWORD WINAPI HookTimeGetTime(void) {
    DWORD real = g_realTGT ? g_realTGT() : 0;
    if (g_tracing && g_clockScale > 1) {
        if (!g_clkArmed) { g_clkRealStart = real; g_clkFakeBase = real; g_clkArmed = true; } // continuous at arm
        return g_clkFakeBase + (real - g_clkRealStart) / g_clockScale;   // then advance slowly
    }
    return real;
}

// Minimal IAT hook: swap EZ2B.exe's import thunk for winmm!timeGetTime, keep the original to call.
static void hookTimeGetTimeIAT() {
    BYTE* mod = (BYTE*)g_base;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)mod;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(mod + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY imp = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!imp.VirtualAddress) return;
    for (IMAGE_IMPORT_DESCRIPTOR* d = (IMAGE_IMPORT_DESCRIPTOR*)(mod + imp.VirtualAddress);
         d->Name; ++d) {
        IMAGE_THUNK_DATA* oft = (IMAGE_THUNK_DATA*)(mod + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        IMAGE_THUNK_DATA* ft  = (IMAGE_THUNK_DATA*)(mod + d->FirstThunk);
        for (; oft->u1.AddressOfData; ++oft, ++ft) {
            if (oft->u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;
            IMAGE_IMPORT_BY_NAME* n = (IMAGE_IMPORT_BY_NAME*)(mod + oft->u1.AddressOfData);
            if (lstrcmpiA((char*)n->Name, "timeGetTime") != 0) continue;
            g_realTGT = (timeGetTime_t)(uintptr_t)ft->u1.Function;
            DWORD old;
            VirtualProtect(&ft->u1.Function, sizeof(void*), PAGE_READWRITE, &old);
            ft->u1.Function = (uintptr_t)&HookTimeGetTime;
            VirtualProtect(&ft->u1.Function, sizeof(void*), old, &old);
            return;
        }
    }
}

static void flushBuf() {
    if (g_bufN && g_out != INVALID_HANDLE_VALUE) {
        DWORD wrote = 0;
        WriteFile(g_out, g_buf, g_bufN * sizeof(uint32_t), &wrote, nullptr);
    }
    g_bufN = 0;
}
static inline void emit(uint32_t rva) {
    g_buf[g_bufN++] = rva;
    if (g_bufN == (sizeof(g_buf) / sizeof(g_buf[0]))) flushBuf();
}

// exec HW breakpoint on DR1, set/cleared in the faulting thread's context.
static void setDR1(CONTEXT* c, uintptr_t addr, bool on) {
    c->Dr1 = on ? addr : 0;
    if (on) c->Dr7 |= (1u << 2);     // L1 enable; RW1/LEN1 left 0 = execute, length 1
    else    c->Dr7 &= ~(1u << 2);
}

static void plantArm() {
    uintptr_t a = g_base + g_armRva;
    DWORD old;
    if (VirtualProtect((void*)a, 1, PAGE_EXECUTE_READWRITE, &old)) {
        g_origByte = *(volatile BYTE*)a;
        *(volatile BYTE*)a = 0xCC;    // int3 — thread-agnostic arm trigger
        VirtualProtect((void*)a, 1, old, &old);
        FlushInstructionCache(GetCurrentProcess(), (void*)a, 1);
        g_armed = true;
    }
}
static void removeArm() {
    if (!g_armed) return;
    uintptr_t a = g_base + g_armRva;
    DWORD old;
    if (VirtualProtect((void*)a, 1, PAGE_EXECUTE_READWRITE, &old)) {
        *(volatile BYTE*)a = g_origByte;
        VirtualProtect((void*)a, 1, old, &old);
        FlushInstructionCache(GetCurrentProcess(), (void*)a, 1);
    }
    g_armed = false;
}

static void stopTrace(CONTEXT* c) {
    if (g_stopped) return;
    g_stopped = true;
    InterlockedExchange(&g_tracing, 0);
    c->EFlags &= ~0x100u;            // clear TF
    setDR1(c, 0, false);
    flushBuf();
    if (g_out != INVALID_HANDLE_VALUE) { CloseHandle(g_out); g_out = INVALID_HANDLE_VALUE; }
}

static LONG CALLBACK Veh(EXCEPTION_POINTERS* ep) {
    EXCEPTION_RECORD* er = ep->ExceptionRecord;
    CONTEXT* c = ep->ContextRecord;
    DWORD code = er->ExceptionCode;
    uintptr_t pc = (uintptr_t)er->ExceptionAddress;

    // --- Arm: our int3 fired at the configured target RVA → begin tracing on THIS thread ---
    if (code == EXCEPTION_BREAKPOINT && g_armed && pc == g_base + g_armRva) {
        removeArm();                 // one-shot
        c->Eip = g_base + g_armRva;  // re-execute the original instruction
        g_prevInText = true;         // arm RVA is game .text
        c->EFlags |= 0x100u;         // TF: start single-stepping
        InterlockedExchange(&g_tracing, 1);
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    if (code == EXCEPTION_SINGLE_STEP && g_tracing && !g_stopped) {
        bool drHit = (c->Dr6 & 0xfull) != 0;
        c->Dr6 = 0;
        if (drHit) {                 // returned from a skipped library call → back in .text
            setDR1(c, 0, false);
        }
        if (inText(pc)) {
            emit((uint32_t)(pc - g_base));
            g_prevInText = true;
            if (++g_count >= g_cap) { stopTrace(c); return EXCEPTION_CONTINUE_EXECUTION; }
            c->EFlags |= 0x100u;     // keep stepping
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        // Not in game .text. Only skip a library CALL when we just LEFT .text (then [esp] is the
        // return address). If we started outside .text (armed mid-mod-code), slow-step until we
        // reach .text — using [esp] mid-function would set a garbage breakpoint and stall.
        if (g_prevInText) {
            uintptr_t ret = 0;
            if (!IsBadReadPtr((void*)c->Esp, 4)) ret = *(uintptr_t*)c->Esp;
            if (inText(ret)) { setDR1(c, ret, true); c->EFlags &= ~0x100u; } // run free to the return
            else c->EFlags |= 0x100u;                                        // tail-jmp → slow-step
        } else {
            c->EFlags |= 0x100u;     // started outside .text → slow-step until we enter it
        }
        g_prevInText = false;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    // A real fault (AV, etc.) hit while tracing → save the tail (the crash path) before the game dies.
    if (g_tracing && !g_stopped && code != EXCEPTION_SINGLE_STEP && code != EXCEPTION_BREAKPOINT) {
        emit(0xFFFFFFFFu); emit((uint32_t)(pc - g_base)); // marker + fault site
        flushBuf();
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static bool g_armFromEnv = false;
static void readEnvCfg() {
    char b[64];
    if (GetEnvironmentVariableA("EZ2TRACE_ARM", b, sizeof(b))) { g_armRva = strtoul(b, nullptr, 16); g_armFromEnv = true; }
    if (GetEnvironmentVariableA("EZ2TRACE_CAP", b, sizeof(b)))   g_cap        = strtoull(b, nullptr, 10);
    if (GetEnvironmentVariableA("EZ2TRACE_CLOCK", b, sizeof(b))) g_clockScale = strtoul(b, nullptr, 10);
    char out[MAX_PATH] = "ez2trace.bin";
    GetEnvironmentVariableA("EZ2TRACE_OUT", out, sizeof(out));
    g_out = CreateFileA(out, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, nullptr);
}

static void init() {
    g_base = (uintptr_t)GetModuleHandleA(nullptr);
    // EZ2B.exe .text is base+0x1000 .. ~base+0x87000 (matches the main mod's crash-logger range).
    g_textLo = g_base + 0x1000;
    g_textHi = g_base + 0x87000;
    readEnvCfg();
    if (g_clockScale > 1) hookTimeGetTimeIAT();  // virtualize the frame clock during tracing
    AddVectoredExceptionHandler(1, Veh);   // first: see single-steps before anything else
    if (g_armFromEnv) plantArm();          // fixed-RVA arm only if EZ2TRACE_ARM was set; else mod-driven
}

// Called by the mod at a known point in the load (after the song inject) — start tracing here, the
// closest spot before the >~2300 abort. Set the trap flag DIRECTLY on this thread (no int3 — a
// breakpoint + CONTINUE_EXECUTION had Eip ambiguity and propagated unhandled = exit 0x80000003). The
// next instruction single-steps → our VEH takes over. g_prevInText=false: we start in mod code.
extern "C" __declspec(dllexport) void Ez2TraceArm(void) {
    if (g_tracing || g_stopped) return;
    g_prevInText = false;
    InterlockedExchange(&g_tracing, 1);
    __asm__ __volatile__("pushf\n\torl $0x100, (%%esp)\n\tpopf" ::: "cc", "memory");
}
// Called by the mod's ExitProcess hook so the trace tail (the abort path) is saved — abort(3) exits via
// ExitProcess, not an exception, so the fault-flush path never runs.
extern "C" __declspec(dllexport) void Ez2TraceFlush(void) { flushBuf(); }

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) { DisableThreadLibraryCalls(h); init(); }
    else if (reason == DLL_PROCESS_DETACH) { removeArm(); flushBuf(); }
    return TRUE;
}

#ifdef SELFTEST
// ponytail check: the only non-trivial pure logic is the .text-range gate + buffer wrap. Verify both.
#include <assert.h>
int main() {
    g_base = 0x400000; g_textLo = g_base + 0x1000; g_textHi = g_base + 0x87000;
    assert(inText(0x4448fe) && !inText(0x400000) && !inText(0x500000) && !inText(0x76fb0000));
    g_out = INVALID_HANDLE_VALUE; g_bufN = 0;
    for (unsigned i = 0; i < (sizeof(g_buf)/sizeof(g_buf[0])); ++i) emit(i); // exactly fills → wraps to 0
    assert(g_bufN == 0);
    emit(123); assert(g_bufN == 1 && g_buf[0] == 123);
    printf("selftest ok\n"); return 0;
}
#endif
