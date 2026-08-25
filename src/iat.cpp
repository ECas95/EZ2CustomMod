#include "iat.h"
#include "logger.h"
#include <winnt.h>
#include <cstring>

bool HookIAT(const char* importedModule, const char* importedName, void* hook, void** originalOut) {
    return HookModuleIAT(GetModuleHandleA(nullptr), importedModule, importedName, hook, originalOut);
}

bool HookModuleIAT(HMODULE mod, const char* importedModule, const char* importedName, void* hook, void** originalOut) {
    if (!mod) return false;
    auto base = reinterpret_cast<unsigned char*>(mod);
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return false;

    auto imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
    for (; imp->Name; ++imp) {
        const char* modName = reinterpret_cast<const char*>(base + imp->Name);
        if (_stricmp(modName, importedModule) != 0) continue;

        auto thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
        auto origThunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->OriginalFirstThunk);
        for (; thunk->u1.Function; ++thunk, ++origThunk) {
            if (origThunk->u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;
            auto byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + origThunk->u1.AddressOfData);
            const char* name = reinterpret_cast<const char*>(byName->Name);
            if (strcmp(name, importedName) != 0) continue;

            void** slot = reinterpret_cast<void**>(&thunk->u1.Function);
            if (originalOut) *originalOut = *slot;
            DWORD oldProtect = 0;
            if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect)) return false;
            *slot = hook;
            DWORD tmp = 0;
            VirtualProtect(slot, sizeof(void*), oldProtect, &tmp);
            LogLine("[iat] %s!%s hooked slot=%p hook=%p", importedModule, importedName, slot, hook);
            return true;
        }
    }
    LogLine("[iat] not found %s!%s", importedModule, importedName);
    return false;
}
