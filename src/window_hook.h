#pragma once
#include <windows.h>

void InstallWindowApiHooks();
void ReleaseStartupGate();
void ShutdownWindowHook();
void CaptureGameWindow(HWND hwnd);
void CaptureExistingProcessWindow();
