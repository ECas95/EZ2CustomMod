#include "patch.h"
#include "logger.h"
#include <cstring>

uintptr_t ExeBase() {
    return reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
}

void* Rva(uintptr_t rva) {
    return reinterpret_cast<void*>(ExeBase() + rva);
}

bool WriteMemory(void* dst, const void* src, size_t len) {
    DWORD oldProtect = 0;
    if (!VirtualProtect(dst, len, PAGE_EXECUTE_READWRITE, &oldProtect)) return false;
    memcpy(dst, src, len);
    FlushInstructionCache(GetCurrentProcess(), dst, len);
    DWORD tmp = 0;
    VirtualProtect(dst, len, oldProtect, &tmp);
    return true;
}

bool InstallJmp(void* target, void* destination, JumpPatch& out, size_t overwriteLen) {
    if (!target || !destination || overwriteLen < 5 || overwriteLen > sizeof(out.original)) return false;
    out.target = target;
    out.length = overwriteLen;
    memcpy(out.original, target, overwriteLen);

    unsigned char patch[16] = {};
    patch[0] = 0xE9;
    intptr_t rel = reinterpret_cast<intptr_t>(destination) - reinterpret_cast<intptr_t>(target) - 5;
    *reinterpret_cast<int32_t*>(&patch[1]) = static_cast<int32_t>(rel);
    for (size_t i = 5; i < overwriteLen; ++i) patch[i] = 0x90;

    if (!WriteMemory(target, patch, overwriteLen)) {
        LogLine("[patch] failed jmp target=%p destination=%p", target, destination);
        return false;
    }
    out.installed = true;
    LogLine("[patch] jmp installed target=%p destination=%p len=%u", target, destination, (unsigned)overwriteLen);
    return true;
}

bool RemoveJmp(JumpPatch& patch) {
    if (!patch.installed || !patch.target || patch.length == 0) return false;
    bool ok = WriteMemory(patch.target, patch.original, patch.length);
    patch.installed = false;
    return ok;
}
