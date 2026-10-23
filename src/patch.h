#pragma once
#include <windows.h>
#include <cstdint>

struct JumpPatch {
    void* target = nullptr;
    unsigned char original[16] = {};
    size_t length = 0;
    bool installed = false;
};

uintptr_t ExeBase();
void* Rva(uintptr_t rva);
bool WriteMemory(void* dst, const void* src, size_t len);
bool InstallJmp(void* target, void* destination, JumpPatch& out, size_t overwriteLen = 5);
bool RemoveJmp(JumpPatch& patch);
