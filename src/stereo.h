#pragma once
#include <d3d9.h>

#include "xr.h"

// Render-thread side of the VR pipeline: captures each eye from the D3D9 back buffer and hands
// it to OpenXR. The game thread queues one StereoFrame per FViewport::Draw pair.
namespace bl2vr {

enum class FrameMode {
    stereo,   // two Draw calls (left without Present, right with Present)
    theater,  // one Draw call shown on a virtual screen
};

// A flat HUD panel of a stereo frame, in the OpenXR LOCAL space (metres).
struct HudPanel {
    bool visible = false;
    XrVector3f centre{};
    XrVector3f right{};     // half width along the panel's +X (text right)
    XrVector3f up{};        // half height along the panel's +Y (text up); normal = right x up
};

struct StereoFrame {
    FrameMode mode = FrameMode::stereo;
    bool xr_frame = false;  // xr::begin_frame succeeded and xr::end_frame is owed
    xr::FrameInfo xr;
    float tan_half_x = 1.0f;  // symmetric frustum the game rendered with
    XrPosef screen_pose{};    // theater screen placement (LOCAL space)
    float screen_width = 3.0f;
    // HUD pieces drawn into the eye images (see wrist.h): vitals on the left wrist, contextual
    // prompts in front of the eyes.
    HudPanel wrist;
    HudPanel view;
    XrVector3f eye_position[2]{};  // where each eye image was rendered from
};

// ---- game thread ----
void stereo_queue_frame(const StereoFrame& frame);
// Blocks until every queued frame has been presented (or the timeout expires).
bool stereo_wait_idle(unsigned timeout_ms);
// Size of the last captured back buffer (0 before the first frame).
void stereo_backbuffer_size(unsigned* width, unsigned* height);
void stereo_request_dump();

// ---- render thread (D3D9 hooks) ----
void stereo_on_device_created(IDirect3DDevice9* device);
void stereo_on_device_lost();
void stereo_on_begin_scene(IDirect3DDevice9* device);
void stereo_on_end_scene(IDirect3DDevice9* device);
// Returns false when the real Present must be skipped.
bool stereo_on_present(IDirect3DDevice9* device);
bool stereo_wants_unthrottled_present();

}  // namespace bl2vr
