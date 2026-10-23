// Guards InstallGateDetour -- the rewrite that lifts the category ceiling past an imm8.
//
// Each ceiling on the category index in EZ2B.exe is `cmp r32,imm8` + jcc. The detour replaces
// the pair with a jmp to a stub that compares against an imm32, takes the same jcc to the same
// target, and jumps back. That is machine code written by hand, so the check is to run it: a
// tiny function with the native shape is built in executable memory, detoured, and CALLED with
// values on both sides of the old and new bounds.
//
//   g++ -std=c++17 -Wall -Wextra -Werror -static -o gate_detour_test tests/gate_detour_test.cpp
//   ./gate_detour_test
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstring>

static int g_fail = 0;
static void check(bool ok, const char* what) {
    printf("  %-66s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++g_fail;
}

// ---- the trace_hooks.cpp environment the function relies on -------------------------------------
static unsigned char* g_base = nullptr;                          // stands in for the exe image
static void* Rva(uintptr_t rva) { return g_base + rva; }
static bool WriteMemory(void* dst, const void* src, size_t len) { std::memcpy(dst, src, len); return true; }
static void* AllocExec(size_t size) {
    return VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
}
static void EmitU8(unsigned char*& p, unsigned char v) { *p++ = v; }
static void EmitU32(unsigned char*& p, uint32_t v) { std::memcpy(p, &v, 4); p += 4; }
static void EmitRel32(unsigned char*& p, void* fromNextInstruction, void* to) {
    intptr_t rel = reinterpret_cast<intptr_t>(to) - reinterpret_cast<intptr_t>(fromNextInstruction);
    EmitU32(p, static_cast<uint32_t>(rel));
}

// ---- copied verbatim from trace_hooks.cpp -------------------------------------------------------

struct GateDetour {
    uintptr_t rva;                 // of the cmp
    unsigned char expected;        // the native imm8
    int32_t  bound;                // the imm32 to compare against, set before install
    unsigned char original[9];
    size_t   length;               // 5 or 9, from the site
    void*    stub;
};

static bool InstallGateDetour(GateDetour& g) {
    unsigned char* site = static_cast<unsigned char*>(Rva(g.rva));
    if (site[0] != 0x83 || (site[1] & 0xF8) != 0xF8 || site[2] != g.expected) return false;
    unsigned char cc; int32_t rel;
    if ((site[3] & 0xF0) == 0x70) { cc = site[3] & 0x0F; rel = static_cast<signed char>(site[4]); g.length = 5; }
    else if (site[3] == 0x0F && (site[4] & 0xF0) == 0x80) { cc = site[4] & 0x0F; std::memcpy(&rel, site + 5, 4); g.length = 9; }
    else return false;
    unsigned char* stub = static_cast<unsigned char*>(AllocExec(24));
    if (!stub) return false;
    unsigned char* p = stub;
    EmitU8(p, 0x81); EmitU8(p, site[1]); EmitU32(p, static_cast<uint32_t>(g.bound));   // cmp r32, imm32
    EmitU8(p, 0x0F); EmitU8(p, 0x80 | cc); EmitRel32(p, p + 4, site + g.length + rel); // jcc near, same target
    EmitU8(p, 0xE9); EmitRel32(p, p + 4, site + g.length);                              // jmp back
    unsigned char patch[9] = {0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
    unsigned char* q = patch;
    EmitU8(q, 0xE9); EmitRel32(q, site + 5, stub);
    std::memcpy(g.original, site, g.length);
    if (!WriteMemory(site, patch, g.length)) { VirtualFree(stub, 0, MEM_RELEASE); return false; }
    g.stub = stub;
    return true;
}

static void RemoveGateDetour(GateDetour& g) {
    if (!g.stub) return;
    WriteMemory(Rva(g.rva), g.original, g.length);
    VirtualFree(g.stub, 0, MEM_RELEASE);
    g.stub = nullptr;
}

// ---- a function shaped like the game's gates ---------------------------------------------------
//
//   mov  r32,[esp+4]          ; the category index, as the argument
//   cmp  r32,imm8             ; <-- the site
//   jcc  taken                ;     short (7x) or near (0F 8x)
//   mov  eax,0 / ret          ; fall through
// taken:
//   mov  eax,1 / ret
//
// Returns 1 when the jcc was taken, so the caller sees exactly which way the gate went.
typedef int (__cdecl *GateFn)(int);

static uintptr_t BuildGate(uintptr_t at, unsigned char modrm, unsigned char imm8, unsigned char cc, bool nearJcc) {
    unsigned char* p = g_base + at;
    // mov r32,[esp+4]: 8B /r with modrm for [esp+disp8] = 0x44 | reg<<3, SIB 0x24, disp 4
    const unsigned char reg = modrm & 7;
    EmitU8(p, 0x8B); EmitU8(p, 0x44 | (reg << 3)); EmitU8(p, 0x24); EmitU8(p, 0x04);
    const uintptr_t site = p - g_base;
    EmitU8(p, 0x83); EmitU8(p, modrm); EmitU8(p, imm8);
    if (nearJcc) { EmitU8(p, 0x0F); EmitU8(p, 0x80 | cc); EmitU32(p, 6); }
    else         { EmitU8(p, 0x70 | cc); EmitU8(p, 6); }
    EmitU8(p, 0xB8); EmitU32(p, 0); EmitU8(p, 0xC3);           // mov eax,0 ; ret   (6 bytes)
    EmitU8(p, 0xB8); EmitU32(p, 1); EmitU8(p, 0xC3);           // mov eax,1 ; ret
    return site;
}

int main() {
    printf("gate detour\n");
    g_base = static_cast<unsigned char*>(AllocExec(4096));
    check(g_base != nullptr, "executable scratch for the fake gates");
    if (!g_base) return 1;

    // The pager's forward gate: cmp eax,0x2e / jge wrap.  Native: 46 wraps, 45 steps.
    {
        const uintptr_t site = BuildGate(0x000, 0xF8, 0x2e, 0xD /*jge*/, false);
        GateFn f = reinterpret_cast<GateFn>(g_base + 0x000);
        check(f(45) == 0 && f(46) == 1, "native shape: 46 takes the jump, 45 falls through");
        GateDetour g = {site, 0x2e, 200, {}, 0, nullptr};
        check(InstallGateDetour(g), "the short-jcc site is accepted");
        check(g.length == 5, "and measured as 5 bytes");
        check(f(46) == 0 && f(199) == 0, "after: 46 and 199 fall through");
        check(f(200) == 1 && f(300) == 1, "200 and 300 take the jump -- the imm32 is live");
        check(g_base[site + 0] == 0xE9, "the site now starts with a jmp");
        RemoveGateDetour(g);
        check(f(45) == 0 && f(46) == 1 && g.stub == nullptr, "removal restores the native bytes and behaviour");
    }

    // The renderer's loop bound: cmp edi,0x2f / jl loop, with a NEAR jcc.  Native: <47 loops.
    {
        const uintptr_t site = BuildGate(0x100, 0xFF, 0x2f, 0xC /*jl*/, true);
        GateFn f = reinterpret_cast<GateFn>(g_base + 0x100);
        check(f(46) == 1 && f(47) == 0, "native near-jcc shape: 46 loops, 47 exits");
        GateDetour g = {site, 0x2f, 256, {}, 0, nullptr};
        check(InstallGateDetour(g), "the near-jcc site is accepted");
        check(g.length == 9, "and measured as 9 bytes");
        check(f(47) == 1 && f(255) == 1 && f(256) == 0, "after: 47..255 loop, 256 exits");
        check(g_base[site + 5] == 0x90 && g_base[site + 8] == 0x90, "the leftover bytes are nops");
        RemoveGateDetour(g);
        check(f(46) == 1 && f(47) == 0, "removal restores it");
    }

    // The sortstyle branch: cmp ecx,0x2e / jg skip.
    {
        const uintptr_t site = BuildGate(0x200, 0xF9, 0x2e, 0xF /*jg*/, false);
        GateFn f = reinterpret_cast<GateFn>(g_base + 0x200);
        check(f(46) == 0 && f(47) == 1, "native ecx shape: 47 is skipped");
        GateDetour g = {site, 0x2e, 255, {}, 0, nullptr};
        check(InstallGateDetour(g), "the ecx site is accepted");
        check(f(47) == 0 && f(255) == 0 && f(256) == 1, "after: up to 255 pass, 256 is skipped");
        RemoveGateDetour(g);
    }

    // What it must refuse: a wrong imm8, and a site that is not cmp+jcc at all.
    {
        const uintptr_t site = BuildGate(0x300, 0xF8, 0x12, 0xF, false);
        GateDetour wrong = {site, 0x2e, 255, {}, 0, nullptr};
        check(!InstallGateDetour(wrong) && g_base[site] == 0x83, "a different imm8 is refused, bytes untouched");
        g_base[site + 3] = 0xB8;                                   // not a jcc after the cmp
        GateDetour notJcc = {site, 0x12, 255, {}, 0, nullptr};
        check(!InstallGateDetour(notJcc) && g_base[site] == 0x83, "cmp without a jcc is refused");
    }

    VirtualFree(g_base, 0, MEM_RELEASE);
    printf("%s\n", g_fail ? "FAILED" : "all ok");
    return g_fail ? 1 : 0;
}
