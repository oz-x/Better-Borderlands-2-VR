#include "stereo.h"

#include <windows.h>
#include <initguid.h>
#include <d3d9on12.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "config.h"
#include "d3d.h"
#include "log.h"
#include "wrist.h"

using Microsoft::WRL::ComPtr;

namespace bl2vr {

namespace {

std::mutex g_queue_mutex;
HANDLE g_idle_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);  // auto-reset
std::deque<StereoFrame> g_frames;
int g_in_flight = 0;    // queued or being submitted; guarded by g_queue_mutex
std::atomic<bool> g_stalled{false};  // last wait timed out

ComPtr<IDirect3DDevice9On12> g_on12;

// Copies of the back buffer taken at the end of each Draw. A stereo pair produces two
// back-buffer EndScenes (left: Draw without Present, right: Draw with Present).
constexpr int kSlots = 2;
IDirect3DTexture9* g_snap_texture[kSlots] = {};
IDirect3DSurface9* g_snap_surface[kSlots] = {};
UINT g_snap_width = 0;
UINT g_snap_height = 0;
D3DFORMAT g_snap_format = D3DFMT_UNKNOWN;
int g_snap_count = 0;

std::atomic<unsigned> g_bb_width{0};
std::atomic<unsigned> g_bb_height{0};
std::atomic<bool> g_dump_requested{false};

void release_snapshots() {
    for (int i = 0; i < kSlots; ++i) {
        if (g_snap_surface[i] != nullptr) {
            g_snap_surface[i]->Release();
            g_snap_surface[i] = nullptr;
        }
        if (g_snap_texture[i] != nullptr) {
            g_snap_texture[i]->Release();
            g_snap_texture[i] = nullptr;
        }
    }
    g_snap_width = g_snap_height = 0;
    g_snap_count = 0;
}

bool ensure_snapshots(IDirect3DDevice9* device, const D3DSURFACE_DESC& desc) {
    if (g_snap_texture[0] != nullptr && g_snap_width == desc.Width &&
        g_snap_height == desc.Height && g_snap_format == desc.Format) {
        return true;
    }
    release_snapshots();
    for (int i = 0; i < kSlots; ++i) {
        HRESULT hr = device->CreateTexture(desc.Width, desc.Height, 1, D3DUSAGE_RENDERTARGET,
                                           desc.Format, D3DPOOL_DEFAULT, &g_snap_texture[i],
                                           nullptr);
        if (FAILED(hr)) {
            LOG("CreateTexture for eye snapshot failed 0x%08lx", hr);
            release_snapshots();
            return false;
        }
        g_snap_texture[i]->GetSurfaceLevel(0, &g_snap_surface[i]);
    }
    g_snap_width = desc.Width;
    g_snap_height = desc.Height;
    g_snap_format = desc.Format;
    g_bb_width = desc.Width;
    g_bb_height = desc.Height;
    LOG("Eye snapshot textures %ux%u fmt=%d", desc.Width, desc.Height, desc.Format);
    return true;
}

bool pop_frame(StereoFrame& out) {
    std::lock_guard lock(g_queue_mutex);
    if (g_frames.empty()) {
        return false;
    }
    out = g_frames.front();
    g_frames.pop_front();
    g_stalled = false;
    return true;
}

void frame_done() {
    {
        std::lock_guard lock(g_queue_mutex);
        if (g_in_flight > 0) {
            --g_in_flight;
        }
    }
    SetEvent(g_idle_event);
}

// ---- debug: write eye images as BMP files ----
void dump_surface(IDirect3DDevice9* device, IDirect3DSurface9* surface, const wchar_t* name) {
    IDirect3DSurface9* sysmem = nullptr;
    if (FAILED(device->CreateOffscreenPlainSurface(g_snap_width, g_snap_height, g_snap_format,
                                                   D3DPOOL_SYSTEMMEM, &sysmem, nullptr))) {
        return;
    }
    if (SUCCEEDED(device->GetRenderTargetData(surface, sysmem))) {
        D3DLOCKED_RECT locked;
        if (SUCCEEDED(sysmem->LockRect(&locked, nullptr, D3DLOCK_READONLY))) {
            const std::wstring path = config().dir + name;
            FILE* f = _wfopen(path.c_str(), L"wb");
            if (f != nullptr) {
                const uint32_t row = g_snap_width * 4;
                BITMAPFILEHEADER fh{};
                BITMAPINFOHEADER ih{};
                fh.bfType = 0x4D42;
                fh.bfOffBits = sizeof(fh) + sizeof(ih);
                fh.bfSize = fh.bfOffBits + row * g_snap_height;
                ih.biSize = sizeof(ih);
                ih.biWidth = static_cast<LONG>(g_snap_width);
                ih.biHeight = -static_cast<LONG>(g_snap_height);
                ih.biPlanes = 1;
                ih.biBitCount = 32;
                fwrite(&fh, sizeof(fh), 1, f);
                fwrite(&ih, sizeof(ih), 1, f);
                for (UINT y = 0; y < g_snap_height; ++y) {
                    fwrite(static_cast<uint8_t*>(locked.pBits) + y * locked.Pitch, row, 1, f);
                }
                fclose(f);
                LOG("Dumped %ls", name);
            }
            sysmem->UnlockRect();
        }
    }
    sysmem->Release();
}

// Hands the snapshot textures of one frame to OpenXR.
void submit_xr(const StereoFrame& frame, int left_slot, int right_slot) {
    ID3D12CommandQueue* queue = d3d12_queue();
    ComPtr<ID3D12Resource> left;
    ComPtr<ID3D12Resource> right;
    HRESULT hr = g_on12->UnwrapUnderlyingResource(g_snap_texture[left_slot], queue,
                                                  IID_PPV_ARGS(&left));
    if (SUCCEEDED(hr) && right_slot != left_slot) {
        hr = g_on12->UnwrapUnderlyingResource(g_snap_texture[right_slot], queue,
                                              IID_PPV_ARGS(&right));
        if (FAILED(hr)) {
            UINT64 none = 0;
            g_on12->ReturnUnderlyingResource(g_snap_texture[left_slot], 0, &none, nullptr);
        }
    }
    if (FAILED(hr)) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            LOG("UnwrapUnderlyingResource failed 0x%08lx", hr);
        }
        xr::end_frame_empty(frame.xr);
        return;
    }

    uint64_t fence_value = 0;
    if (right) {
        const float tan_half_y = frame.tan_half_x * g_snap_height / static_cast<float>(g_snap_width);
        fence_value = xr::end_frame_stereo(frame.xr, left.Get(), right.Get(), g_snap_width,
                                           g_snap_height, frame.tan_half_x, tan_half_y);
    } else {
        fence_value = xr::end_frame_screen(frame.xr, left.Get(), g_snap_width, g_snap_height,
                                           frame.screen_pose, frame.screen_width);
    }
    ID3D12Fence* fence = xr::copy_fence();
    UINT64 value = fence_value;
    const UINT sync = fence_value != 0 ? 1 : 0;
    g_on12->ReturnUnderlyingResource(g_snap_texture[left_slot], sync, &value, &fence);
    if (right) {
        g_on12->ReturnUnderlyingResource(g_snap_texture[right_slot], sync, &value, &fence);
    }
}

// Draws the VR HUD panels (wrist.h) into both captured eye images, each with its own camera.
void draw_hud_panels(IDirect3DDevice9* device, const StereoFrame& frame, int left_slot,
                     int right_slot) {
    if (!frame.wrist.visible && !frame.view.visible) {
        return;
    }
    IDirect3DSurface9* target = nullptr;
    IDirect3DSurface9* depth = nullptr;
    device->GetRenderTarget(0, &target);
    device->GetDepthStencilSurface(&depth);
    device->SetDepthStencilSurface(nullptr);  // panels ignore depth
    if (SUCCEEDED(device->BeginScene())) {
        const int slots[2] = {left_slot, right_slot};
        for (int eye = 0; eye < 2; ++eye) {
            device->SetRenderTarget(0, g_snap_surface[slots[eye]]);
            wrist_draw(device, frame, eye, g_snap_width, g_snap_height);
        }
        device->EndScene();
    }
    device->SetRenderTarget(0, target);
    device->SetDepthStencilSurface(depth);
    if (target != nullptr) {
        target->Release();
    }
    if (depth != nullptr) {
        depth->Release();
    }
}

}  // namespace

void stereo_queue_frame(const StereoFrame& frame) {
    std::lock_guard lock(g_queue_mutex);
    g_frames.push_back(frame);
    ++g_in_flight;
}

bool stereo_wait_idle(unsigned timeout_ms) {
    // std::condition_variable::wait_for never timed out inside the game (observed hang), so
    // this uses a plain Win32 event with a tick-count deadline.
    // While the engine draws without presenting (loading screens) do not stall every frame.
    const ULONGLONG deadline = GetTickCount64() + (g_stalled ? 5 : timeout_ms);
    for (;;) {
        {
            std::lock_guard lock(g_queue_mutex);
            if (g_in_flight == 0) {
                return true;
            }
        }
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline) {
            break;
        }
        WaitForSingleObject(g_idle_event, static_cast<DWORD>(deadline - now));
    }
    std::lock_guard lock(g_queue_mutex);
    // The render thread never presented these frames (loading, window minimised...).
    // xrBeginFrame on the game thread discards the frame they belonged to.
    if (!g_stalled) {
        LOG("Frames are not being presented; dropping %zu", g_frames.size());
    }
    g_stalled = true;
    g_frames.clear();
    g_in_flight = 0;
    return false;
}

void stereo_backbuffer_size(unsigned* width, unsigned* height) {
    *width = g_bb_width;
    *height = g_bb_height;
}

void stereo_request_dump() {
    g_dump_requested = true;
}

void stereo_on_device_created(IDirect3DDevice9* device) {
    g_on12.Reset();
    if (d3d_is_9on12() && FAILED(device->QueryInterface(IID_PPV_ARGS(&g_on12)))) {
        LOG("IDirect3DDevice9On12 unavailable");
    }
}

void stereo_on_device_lost() {
    // D3DPOOL_DEFAULT resources must be gone before IDirect3DDevice9::Reset.
    release_snapshots();
    wrist_on_device_lost();
}

void stereo_on_begin_scene(IDirect3DDevice9*) {}

void stereo_on_end_scene(IDirect3DDevice9* device) {
    IDirect3DSurface9* render_target = nullptr;
    IDirect3DSurface9* back_buffer = nullptr;
    device->GetRenderTarget(0, &render_target);
    device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back_buffer);
    if (render_target != nullptr && render_target == back_buffer) {
        D3DSURFACE_DESC desc;
        back_buffer->GetDesc(&desc);
        if (ensure_snapshots(device, desc)) {
            const int slot = g_snap_count % kSlots;
            device->StretchRect(back_buffer, nullptr, g_snap_surface[slot], nullptr, D3DTEXF_NONE);
            ++g_snap_count;
        }
    }
    if (render_target != nullptr) {
        render_target->Release();
    }
    if (back_buffer != nullptr) {
        back_buffer->Release();
    }
}

bool stereo_on_present(IDirect3DDevice9* device) {
    const int captured = g_snap_count;
    g_snap_count = 0;
    StereoFrame frame;
    if (!pop_frame(frame)) {
        return true;  // not a frame we drew (loading screens, startup)
    }
    const int right_slot = (captured - 1 + kSlots) % kSlots;
    const int left_slot = (captured - 2 + kSlots) % kSlots;
    const bool stereo = frame.mode == FrameMode::stereo && captured >= 2;

    // Frame-rate report every 10 s.
    static DWORD stats_start = GetTickCount();
    static int stats_frames = 0, stats_stereo = 0;
    ++stats_frames;
    stats_stereo += stereo ? 1 : 0;
    if (const DWORD elapsed = GetTickCount() - stats_start; elapsed >= 10000) {
        LOG("%.1f fps (%d%% stereo), eye %ux%u", stats_frames * 1000.0f / elapsed,
            stats_stereo * 100 / stats_frames, g_snap_width, g_snap_height);
        stats_start = GetTickCount();
        stats_frames = stats_stereo = 0;
    }

    // The engine may end more than two scenes on the back buffer per frame; only now are
    // the two eye images known, so the VR HUD panels are drawn into them here.
    if (stereo) {
        draw_hud_panels(device, frame, left_slot, right_slot);
    }

    if (frame.xr_frame) {
        if (g_on12 && captured >= 1) {
            submit_xr(frame, stereo ? left_slot : right_slot, right_slot);
        } else {
            xr::end_frame_empty(frame.xr);
        }
    }
    if (captured >= 1 && g_dump_requested.exchange(false)) {
        dump_surface(device, g_snap_surface[stereo ? left_slot : right_slot], L"bl2vr_left.bmp");
        dump_surface(device, g_snap_surface[right_slot], L"bl2vr_right.bmp");
        wrist_dump(device);
    }
    frame_done();
    return config().mirror || !frame.xr_frame;
}

bool stereo_wants_unthrottled_present() {
    return config().vr_enabled;
}

}  // namespace bl2vr
