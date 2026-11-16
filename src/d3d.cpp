#include "d3d.h"

#include <windows.h>
#include <d3d9on12.h>
#include <wrl/client.h>

#include <MinHook.h>

#include <atomic>

#include "config.h"
#include "log.h"
#include "proxy.h"
#include "stereo.h"
#include "ue3.h"
#include "vr.h"

using Microsoft::WRL::ComPtr;

namespace bl2vr {

namespace {

ComPtr<ID3D12Device> g_d3d12;
ComPtr<ID3D12CommandQueue> g_queue;
IDirect3DDevice9* g_device = nullptr;
HWND g_window = nullptr;
bool g_9on12 = false;
std::atomic<int> g_trace_frames{0};

using CreateDeviceFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD,
                                                   D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND,
                                              const RGNDATA*);
using ResetFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
using SceneFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*);
using SetRenderTargetFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD,
                                                      IDirect3DSurface9*);

CreateDeviceFn g_create_device = nullptr;
PresentFn g_present = nullptr;
ResetFn g_reset = nullptr;
SceneFn g_begin_scene = nullptr;
SceneFn g_end_scene = nullptr;
SetRenderTargetFn g_set_render_target = nullptr;

// Detours the function behind a COM vtable slot. Inline detours survive the Steam overlay (and
// anything else) rewriting the device's per-instance vtable after us, which silently bypassed
// plain vtable patches. Returns the trampoline, or nullptr if that function is already hooked.
void* hook_method(void* object, int index, void* hook) {
    MH_Initialize();  // no-op when the engine hooks already initialised MinHook
    void* target = (*reinterpret_cast<void***>(object))[index];
    void* original = nullptr;
    const MH_STATUS status = MH_CreateHook(target, hook, &original);
    if (status == MH_ERROR_ALREADY_CREATED) {
        return nullptr;
    }
    if (status != MH_OK || MH_EnableHook(target) != MH_OK) {
        LOG("Failed to hook vtable slot %d (%p): %d", index, target, status);
        return nullptr;
    }
    return original;
}

void force_present_params(D3DPRESENT_PARAMETERS* pp) {
    if (pp == nullptr) {
        return;
    }
    if (stereo_wants_unthrottled_present()) {
        // VR frame pacing comes from xrWaitFrame; the desktop mirror must never block on vsync.
        pp->PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    }
    if (g_9on12 && pp->Windowed && pp->SwapEffect == D3DSWAPEFFECT_COPY) {
        // D3D9On12 leaves the window black with UE3's COPY swap chain.
        pp->SwapEffect = D3DSWAPEFFECT_DISCARD;
    }
}

HRESULT STDMETHODCALLTYPE hook_present(IDirect3DDevice9* device, const RECT* src, const RECT* dst,
                                       HWND window, const RGNDATA* dirty) {
    static bool first = true;
    if (first) {
        first = false;
        LOG("First Present on thread %lu, window override %p", GetCurrentThreadId(),
            (void*)window);
    }
    if (g_trace_frames.load() > 0) {
        LOG("TRACE Present");
        if (g_trace_frames.fetch_sub(1) == 1) {
            LOG("TRACE end");
        }
    }
    if (!stereo_on_present(device)) {
        return D3D_OK;  // frame consumed by VR, nothing to show on the desktop
    }
    const HRESULT hr = g_present(device, src, dst, window, dirty);
    static int reported = 0;
    if (hr != D3D_OK && reported < 5) {
        ++reported;
        LOG("Present -> 0x%08lx", hr);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hook_reset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* pp) {
    LOG("Device Reset %ux%u windowed=%d", pp->BackBufferWidth, pp->BackBufferHeight, pp->Windowed);
    stereo_on_device_lost();
    force_present_params(pp);
    HRESULT hr = g_reset(device, pp);
    LOG("Reset -> 0x%08lx", hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE hook_begin_scene(IDirect3DDevice9* device) {
    if (g_trace_frames.load() > 0) {
        LOG("TRACE BeginScene");
    }
    stereo_on_begin_scene(device);
    return g_begin_scene(device);
}

HRESULT STDMETHODCALLTYPE hook_end_scene(IDirect3DDevice9* device) {
    if (g_trace_frames.load() > 0) {
        IDirect3DSurface9* rt = nullptr;
        device->GetRenderTarget(0, &rt);
        D3DSURFACE_DESC desc{};
        if (rt != nullptr) {
            rt->GetDesc(&desc);
        }
        IDirect3DSurface9* bb = nullptr;
        device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
        LOG("TRACE EndScene rt=%p (%ux%u fmt=%d) backbuffer=%p", (void*)rt, desc.Width,
            desc.Height, desc.Format, (void*)bb);
        if (rt != nullptr) {
            rt->Release();
        }
        if (bb != nullptr) {
            bb->Release();
        }
    }
    stereo_on_end_scene(device);
    return g_end_scene(device);
}

HRESULT STDMETHODCALLTYPE hook_set_render_target(IDirect3DDevice9* device, DWORD index,
                                                 IDirect3DSurface9* surface) {
    if (g_trace_frames.load() > 0 && index == 0) {
        D3DSURFACE_DESC desc{};
        if (surface != nullptr) {
            surface->GetDesc(&desc);
        }
        LOG("TRACE SetRenderTarget0 %p (%ux%u fmt=%d)", (void*)surface, desc.Width, desc.Height,
            desc.Format);
    }
    return g_set_render_target(device, index, surface);
}

HRESULT STDMETHODCALLTYPE hook_create_device(IDirect3D9* d3d, UINT adapter, D3DDEVTYPE type,
                                             HWND window, DWORD flags, D3DPRESENT_PARAMETERS* pp,
                                             IDirect3DDevice9** out) {
    LOG("CreateDevice adapter=%u flags=0x%lx %ux%u windowed=%d fmt=%d interval=0x%x swap=%d "
        "focus=%p devicewnd=%p",
        adapter, flags, pp->BackBufferWidth, pp->BackBufferHeight, pp->Windowed,
        pp->BackBufferFormat, pp->PresentationInterval, pp->SwapEffect, (void*)window,
        (void*)pp->hDeviceWindow);
    force_present_params(pp);
    HRESULT hr = g_create_device(d3d, adapter, type, window, flags, pp, out);
    LOG("CreateDevice -> 0x%08lx", hr);
    if (SUCCEEDED(hr) && out != nullptr && *out != nullptr) {
        g_device = *out;
        g_window = pp->hDeviceWindow != nullptr ? pp->hDeviceWindow : window;
        // IDirect3DDevice9 vtable slots.
        if (void* p = hook_method(g_device, 16, (void*)&hook_reset)) g_reset = (ResetFn)p;
        if (void* p = hook_method(g_device, 17, (void*)&hook_present)) g_present = (PresentFn)p;
        if (void* p = hook_method(g_device, 37, (void*)&hook_set_render_target))
            g_set_render_target = (SetRenderTargetFn)p;
        if (void* p = hook_method(g_device, 41, (void*)&hook_begin_scene))
            g_begin_scene = (SceneFn)p;
        if (void* p = hook_method(g_device, 42, (void*)&hook_end_scene)) g_end_scene = (SceneFn)p;
        stereo_on_device_created(g_device);
    }
    return hr;
}

bool create_d3d12(IDXGIAdapter* adapter) {
    using CreateDeviceFn12 = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
    HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
    auto create = d3d12 ? (CreateDeviceFn12)GetProcAddress(d3d12, "D3D12CreateDevice") : nullptr;
    if (create == nullptr) {
        LOG("d3d12.dll unavailable");
        return false;
    }
    HRESULT hr = create(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_d3d12));
    if (FAILED(hr)) {
        LOG("D3D12CreateDevice failed 0x%08lx", hr);
        return false;
    }
    D3D12_COMMAND_QUEUE_DESC desc{};
    desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = g_d3d12->CreateCommandQueue(&desc, IID_PPV_ARGS(&g_queue));
    if (FAILED(hr)) {
        LOG("CreateCommandQueue failed 0x%08lx", hr);
        g_d3d12.Reset();
        return false;
    }
    return true;
}

}  // namespace

bool d3d_is_9on12() {
    return g_9on12;
}
ID3D12Device* d3d12_device() {
    return g_d3d12.Get();
}
ID3D12CommandQueue* d3d12_queue() {
    return g_queue.Get();
}
IDirect3DDevice9* d3d9_device() {
    return g_device;
}
HWND d3d_window() {
    return g_window;
}
void d3d_request_trace(int frames) {
    g_trace_frames = frames;
}
bool d3d_tracing() {
    return g_trace_frames.load() > 0;
}

}  // namespace bl2vr

using namespace bl2vr;

extern "C" IDirect3D9* WINAPI proxy_Direct3DCreate9(UINT sdk_version) {
    vr_early_init();  // engine hooks + OpenXR instance (picks the adapter)
    LOG("Direct3DCreate9(%u)", sdk_version);

    IDirect3D9* d3d = nullptr;
    const bool want_vr = config().vr_enabled && config().use_9on12 &&
                         (xr::available() || config().fake_xr);
    if (want_vr && (g_d3d12 || create_d3d12(vr_adapter()))) {
        using Create9On12Fn = IDirect3D9*(WINAPI*)(UINT, D3D9ON12_ARGS*, UINT);
        auto create = (Create9On12Fn)system_proc("Direct3DCreate9On12");
        if (create != nullptr) {
            D3D9ON12_ARGS args{};
            args.Enable9On12 = TRUE;
            args.pD3D12Device = g_d3d12.Get();
            args.ppD3D12Queues[0] = g_queue.Get();
            args.NumQueues = 1;
            d3d = create(sdk_version, &args, 1);
            LOG("Direct3DCreate9On12 -> %p", (void*)d3d);
            if (d3d != nullptr && !g_9on12) {
                g_9on12 = true;
                vr_on_d3d12_ready();
            }
        }
    }
    if (d3d == nullptr) {
        using Create9Fn = IDirect3D9*(WINAPI*)(UINT);
        auto create = (Create9Fn)system_proc("Direct3DCreate9");
        d3d = create != nullptr ? create(sdk_version) : nullptr;
        LOG("Using native D3D9 (VR output unavailable)");
    }
    if (d3d != nullptr) {
        if (void* p = hook_method(d3d, 16, (void*)&hook_create_device)) {
            g_create_device = (CreateDeviceFn)p;
        }
    }
    return d3d;
}
