#pragma once
#include <d3d9.h>
#include <d3d12.h>

namespace bl2vr {

// True when the game's D3D9 device runs on top of our D3D12 device (needed for VR output).
bool d3d_is_9on12();
ID3D12Device* d3d12_device();
ID3D12CommandQueue* d3d12_queue();
IDirect3DDevice9* d3d9_device();
// Window the game's device presents to (nullptr before CreateDevice).
HWND d3d_window();

// Requests a trace of the D3D call sequence for the next few frames.
void d3d_request_trace(int frames);
bool d3d_tracing();

}  // namespace bl2vr
