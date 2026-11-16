#pragma once
#include <windows.h>
#include <d3d12.h>

#include <cstdint>

#ifndef XR_USE_PLATFORM_WIN32
#define XR_USE_PLATFORM_WIN32
#endif
#ifndef XR_USE_GRAPHICS_API_D3D12
#define XR_USE_GRAPHICS_API_D3D12
#endif
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

namespace bl2vr::xr {

struct FrameInfo {
    uint64_t id = 0;
    XrTime display_time = 0;
    bool should_render = false;
    bool views_valid = false;
    XrView views[2]{};
};

// Instance + system. Returns false when no runtime/headset is available.
bool init_instance();
bool available();
bool adapter_luid(LUID* out);

bool create_session(ID3D12Device* device, ID3D12CommandQueue* queue);
bool session_running();
// True once after the runtime recentred its LOCAL space (headset recenter gesture).
bool consume_recenter_request();

// Per-eye size recommended by the runtime.
uint32_t recommended_width();
uint32_t recommended_height();

// ---- game thread ----
void poll_events();
// Waits for pacing, begins the frame and locates both eyes. Returns false when no frame began.
bool begin_frame(FrameInfo& out);

// ---- render thread ----
// Every function below ends the frame begun by begin_frame. Copies are recorded on the queue
// passed to create_session; the return value is the copy fence value (0 = nothing submitted).
// Source resources must be B8G8R8A8 textures in a state promotable from COMMON.
uint64_t end_frame_stereo(const FrameInfo& frame, ID3D12Resource* left, ID3D12Resource* right,
                          uint32_t width, uint32_t height, float tan_half_x, float tan_half_y);
uint64_t end_frame_screen(const FrameInfo& frame, ID3D12Resource* image, uint32_t width,
                          uint32_t height, const XrPosef& pose, float width_metres);
void end_frame_empty(const FrameInfo& frame);
ID3D12Fence* copy_fence();

// ---- motion controllers (xr_input.cpp) ----
enum Hand { kHandLeft = 0, kHandRight = 1 };

struct HandInput {
    bool pose_valid = false;
    XrPosef aim{};             // aim pose in the LOCAL space
    float trigger = 0.0f;
    float grip = 0.0f;
    XrVector2f stick{};
    bool stick_click = false;
    bool primary = false;      // A (right) / X (left)
    bool secondary = false;    // B (right) / Y (left)
    bool menu = false;         // left hand only
};

struct ControllerInput {
    bool active = false;       // actions synced while the session had focus
    HandInput hand[2];
};

// Creates the action set; called by create_session.
bool input_init(XrInstance instance, XrSession session, XrSpace local);
// Game thread, after begin_frame: syncs actions and locates both hands at `time`.
void input_sync(XrTime time, ControllerInput& out);
// Vibrates a controller (amplitude 0..1, 0 stops). Game thread.
void haptic(Hand hand, float amplitude);

}  // namespace bl2vr::xr
