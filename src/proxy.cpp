// d3d9.dll proxy: every export except Direct3DCreate9 jumps straight into the system d3d9.dll.
#include <windows.h>

#include "proxy.h"

namespace bl2vr {

namespace {
HMODULE g_system_d3d9 = nullptr;
}

HMODULE system_d3d9() {
    if (g_system_d3d9 == nullptr) {
        wchar_t path[MAX_PATH];
        GetSystemDirectoryW(path, MAX_PATH);  // SysWOW64 for this 32-bit process
        wcscat_s(path, L"\\d3d9.dll");
        g_system_d3d9 = LoadLibraryW(path);
    }
    return g_system_d3d9;
}

FARPROC system_proc(const char* name) {
    HMODULE module = system_d3d9();
    return module == nullptr ? nullptr : GetProcAddress(module, name);
}

}  // namespace bl2vr

#define BL2VR_FORWARD(name)                                              \
    static FARPROC g_real_##name = nullptr;                              \
    extern "C" __declspec(naked) void proxy_##name() {                   \
        __asm { jmp [g_real_##name] }                                    \
    }

BL2VR_FORWARD(Direct3DCreate9Ex)
BL2VR_FORWARD(Direct3DCreate9On12)
BL2VR_FORWARD(Direct3DCreate9On12Ex)
BL2VR_FORWARD(D3DPERF_BeginEvent)
BL2VR_FORWARD(D3DPERF_EndEvent)
BL2VR_FORWARD(D3DPERF_GetStatus)
BL2VR_FORWARD(D3DPERF_QueryRepeatFrame)
BL2VR_FORWARD(D3DPERF_SetMarker)
BL2VR_FORWARD(D3DPERF_SetOptions)
BL2VR_FORWARD(D3DPERF_SetRegion)
BL2VR_FORWARD(DebugSetLevel)
BL2VR_FORWARD(DebugSetMute)
BL2VR_FORWARD(Direct3D9EnableMaximizedWindowedModeShim)
BL2VR_FORWARD(Direct3DShaderValidatorCreate9)
BL2VR_FORWARD(PSGPError)
BL2VR_FORWARD(PSGPSampleTexture)

namespace bl2vr {

void proxy_resolve_exports() {
#define BL2VR_RESOLVE(name) g_real_##name = system_proc(#name);
    BL2VR_RESOLVE(Direct3DCreate9Ex)
    BL2VR_RESOLVE(Direct3DCreate9On12)
    BL2VR_RESOLVE(Direct3DCreate9On12Ex)
    BL2VR_RESOLVE(D3DPERF_BeginEvent)
    BL2VR_RESOLVE(D3DPERF_EndEvent)
    BL2VR_RESOLVE(D3DPERF_GetStatus)
    BL2VR_RESOLVE(D3DPERF_QueryRepeatFrame)
    BL2VR_RESOLVE(D3DPERF_SetMarker)
    BL2VR_RESOLVE(D3DPERF_SetOptions)
    BL2VR_RESOLVE(D3DPERF_SetRegion)
    BL2VR_RESOLVE(DebugSetLevel)
    BL2VR_RESOLVE(DebugSetMute)
    BL2VR_RESOLVE(Direct3D9EnableMaximizedWindowedModeShim)
    BL2VR_RESOLVE(Direct3DShaderValidatorCreate9)
    BL2VR_RESOLVE(PSGPError)
    BL2VR_RESOLVE(PSGPSampleTexture)
#undef BL2VR_RESOLVE
}

}  // namespace bl2vr
