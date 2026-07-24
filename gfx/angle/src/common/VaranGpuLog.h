// Varan (GPU diag): file logger for the ANGLE D3D11 instrumentation.
//
// There is NO DebugView / debugger on ARM32 Windows RT, so OutputDebugStringA captures nothing on
// the device. Like the JIT fault reporter (%TEMP%\varan-jit-fault.log), append the GPU diagnostic
// lines to a FILE the RT sandbox can write: %TEMP%\varan-gpu.log. Read it off the device after a run.
//
// Raw Win32 only (no CRT alloc), self-contained, greppable ("VaranGpuLog"), trivially revertible.
#ifndef VARAN_GPU_LOG_H
#define VARAN_GPU_LOG_H

#include <windows.h>

static inline void VaranGpuLog(const char* s)
{
    char path[MAX_PATH];
    DWORD n = GetTempPathA(MAX_PATH, path);
    if (n == 0 || n >= MAX_PATH - 16)
        return;
    const char* fn = "varan-gpu.log";
    DWORD i = 0;
    for (; fn[i] && n + i < MAX_PATH - 1; i++)
        path[n + i] = fn[i];
    path[n + i] = '\0';

    HANDLE h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;
    DWORD len = 0;
    while (s[len])
        len++;
    DWORD wrote = 0;
    WriteFile(h, s, len, &wrote, NULL);
    CloseHandle(h);
}

#endif  // VARAN_GPU_LOG_H
