#pragma once
#include <dxgi.h>

// Game-thread side of the VR pipeline.
namespace bl2vr {

// Runs once from Direct3DCreate9: config, log, engine hooks, OpenXR instance.
void vr_early_init();
// Adapter the headset runtime requires (nullptr = default).
IDXGIAdapter* vr_adapter();
// Called once the D3D12 device that backs the game's D3D9 device exists.
void vr_on_d3d12_ready();

}  // namespace bl2vr
