#pragma once
#include <d3d9.h>

#include "stereo.h"

// VR HUD. In VR the HUD movie renders with a viewport rectangle no other movie uses; when
// Scaleform sets that viewport the render thread swaps the render target for an off-screen
// texture, so the HUD lands there instead of in the eye images. Before each eye image is
// captured, pieces of that texture are rearranged onto two panels:
//   - wrist: health/shield, ammo/grenades, XP, minimap and missions, on the back of the left
//     wrist like a watch (visible while it faces you);
//   - view: the centre of the HUD (enemy health, use/pick-up prompts, messages) floating in
//     front of the eyes.
namespace bl2vr {

enum class HudLayout { wrist, view };
// Height / width of a panel's layout.
float hud_layout_aspect(HudLayout layout);
// Width of a panel's layout as a fraction of the HUD's own width (for sizing the view panel
// like the flat-screen HUD).
float hud_layout_hud_fraction(HudLayout layout);

// ---- game thread ----
// Viewport the HUD movie renders with while it is shown on the panels (width 0 = on screen).
void wrist_set_hud_rect(int x, int y, int width, int height);
// ---- D3D9 hooks (render thread) ----
void wrist_on_present();
void wrist_on_set_viewport(IDirect3DDevice9* device, const D3DVIEWPORT9* viewport);
// Return the surface the call must really bind.
IDirect3DSurface9* wrist_on_set_render_target(DWORD index, IDirect3DSurface9* surface);
IDirect3DSurface9* wrist_on_set_depth_stencil(IDirect3DSurface9* surface);
void wrist_on_end_scene(IDirect3DDevice9* device);
void wrist_on_device_lost();
// Draws both panels into the current render target: the captured image of eye `eye`
// (0 left, 1 right), called at Present once the eye images are known.
void wrist_draw(IDirect3DDevice9* device, const StereoFrame& frame, int eye, UINT width,
                UINT height);
// Draws the sight (frame.reticle) into the current render target, the image of eye `eye`.
void reticle_draw(IDirect3DDevice9* device, const StereoFrame& frame, int eye, UINT width,
                  UINT height);
// Debug: writes the HUD texture (with alpha) to bl2vr_hud.tga.
void wrist_dump(IDirect3DDevice9* device);

}  // namespace bl2vr
