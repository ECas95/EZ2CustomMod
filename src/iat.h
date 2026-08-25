#pragma once
#include <windows.h>

bool HookIAT(const char* importedModule, const char* importedName, void* hook, void** originalOut);

// Same, but patches the IAT of an ARBITRARY loaded module (e.g. quartz.dll) instead of the exe.
// Used to redirect DirectShow's own CreateFileW without touching kernel32 globally.
bool HookModuleIAT(HMODULE mod, const char* importedModule, const char* importedName, void* hook, void** originalOut);
