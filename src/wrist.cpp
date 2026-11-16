#include "wrist.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <string>

#include "config.h"
#include "log.h"
#include "mathx.h"

namespace bl2vr {

using namespace mathx;

namespace {

// Published by the game thread: x, y, width, height (16 bits each), 0 = HUD on screen.
std::atomic<uint64_t> g_hud_rect{0};

// Render-thread state.
IDirect3DTexture9* g_texture = nullptr;  // HUD target, same size as the back buffer
IDirect3DSurface9* g_surface = nullptr;
IDirect3DSurface9* g_depth = nullptr;    // Scaleform masks need a stencil of the same size
UINT g_width = 0, g_height = 0;
IDirect3DStateBlock9* g_state = nullptr;

bool g_redirecting = false;              // the HUD movie is drawing into g_active
bool g_internal = false;                 // our own SetRenderTarget calls pass straight through
IDirect3DSurface9* g_scratch = nullptr;  // where the second eye's HUD pass goes
IDirect3DSurface9* g_active = nullptr;   // g_surface or g_scratch while redirecting
IDirect3DSurface9* g_saved_target = nullptr;  // eye image the HUD would have drawn into
IDirect3DSurface9* g_saved_depth = nullptr;
bool g_has_content = false;              // g_texture holds a HUD
bool g_clear_pending = true;             // the next capture starts a new HUD
bool g_pass_captured = false;            // this eye image captured the HUD
bool g_frame_has_hud = false;            // an earlier eye image of this frame captured it

struct Rect {
    int x, y, width, height;
};

Rect hud_rect() {
    const uint64_t packed = g_hud_rect.load(std::memory_order_relaxed);
    return {static_cast<int>(packed & 0xFFFF), static_cast<int>((packed >> 16) & 0xFFFF),
            static_cast<int>((packed >> 32) & 0xFFFF), static_cast<int>(packed >> 48)};
}

template <class T>
void release(T*& p) {
    if (p != nullptr) {
        p->Release();
        p = nullptr;
    }
}

void release_targets() {
    release(g_scratch);
    release(g_surface);
    release(g_texture);
    release(g_depth);
    g_width = g_height = 0;
    g_has_content = false;
}

bool ensure_targets(IDirect3DDevice9* device, UINT width, UINT height) {
    if (g_texture != nullptr && g_width == width && g_height == height) {
        return true;
    }
    release_targets();
    if (FAILED(device->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                                     D3DPOOL_DEFAULT, &g_texture, nullptr)) ||
        FAILED(g_texture->GetSurfaceLevel(0, &g_surface)) ||
        FAILED(device->CreateRenderTarget(width, height, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE,
                                          0, FALSE, &g_scratch, nullptr)) ||
        FAILED(device->CreateDepthStencilSurface(width, height, D3DFMT_D24S8,
                                                 D3DMULTISAMPLE_NONE, 0, FALSE, &g_depth,
                                                 nullptr))) {
        LOG("ERROR: wrist HUD target %ux%u could not be created", width, height);
        release_targets();
        return false;
    }
    g_width = width;
    g_height = height;
    LOG("Wrist HUD target %ux%u", width, height);
    return true;
}

void begin_redirect(IDirect3DDevice9* device) {
    IDirect3DSurface9* target = nullptr;
    IDirect3DSurface9* back_buffer = nullptr;
    device->GetRenderTarget(0, &target);
    device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back_buffer);
    const bool on_eye = target != nullptr && target == back_buffer;
    release(back_buffer);
    D3DSURFACE_DESC desc{};
    if (!on_eye || FAILED(target->GetDesc(&desc)) ||
        !ensure_targets(device, desc.Width, desc.Height)) {
        release(target);
        return;
    }
    g_saved_target = target;
    device->GetDepthStencilSurface(&g_saved_depth);
    // The first eye image of a frame captures the HUD. Scaleform does not redraw the movie
    // in the second one (it only sets the viewport), so that pass goes to a scratch target
    // and both eyes share the first capture.
    g_active = g_frame_has_hud ? g_scratch : g_surface;
    g_internal = true;
    device->SetRenderTarget(0, g_active);
    device->SetDepthStencilSurface(g_depth);
    g_internal = false;
    if (g_active == g_surface && g_clear_pending) {
        // Scaleform may set the HUD viewport more than once per eye image; only the first
        // time starts a new HUD, later passes add to it.
        g_clear_pending = false;
        D3DVIEWPORT9 full{0, 0, g_width, g_height, 0.0f, 1.0f};
        device->SetViewport(&full);  // Clear only touches the viewport
        device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0,
                      1.0f, 0);
    }
    // Accumulate coverage in alpha so the texture ends up premultiplied.
    device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_SRCBLENDALPHA, D3DBLEND_ONE);
    device->SetRenderState(D3DRS_DESTBLENDALPHA, D3DBLEND_INVSRCALPHA);
    g_redirecting = true;
    g_pass_captured = g_pass_captured || g_active == g_surface;
    g_has_content = true;
}

void end_redirect(IDirect3DDevice9* device) {
    if (!g_redirecting) {
        return;
    }
    g_redirecting = false;
    g_internal = true;
    device->SetRenderTarget(0, g_saved_target);
    device->SetDepthStencilSurface(g_saved_depth);
    g_internal = false;
    device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    release(g_saved_target);
    release(g_saved_depth);
}

// ---- drawing ----

Vec3 to_vec(const XrVector3f& v) {
    return {v.x, v.y, v.z};
}

Vec3 cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

Vec3 normalize(Vec3 v) {
    const float length = std::sqrt(dot(v, v));
    return length > 0.0f ? v * (1.0f / length) : v;
}

XrQuaternionf conjugate(const XrQuaternionf& q) {
    return {-q.x, -q.y, -q.z, q.w};
}

struct Vertex {
    float x, y, z, rhw;
    float u, v;
};
constexpr DWORD kFvf = D3DFVF_XYZRHW | D3DFVF_TEX1;

// One eye's camera: the pose and symmetric frustum its image was rendered with.
struct EyeCamera {
    Vec3 position;
    XrQuaternionf inverse;  // LOCAL -> eye space
    float tan_x, tan_y;
    float width, height;
};

// Projects a LOCAL-space point. False when it is behind (or too close to) the eye.
bool project(const EyeCamera& eye, Vec3 point, float u, float v, Vertex* out) {
    const Vec3 p = rotate(eye.inverse, point - eye.position);
    const float depth = -p.z;  // OpenXR eyes look down -Z
    if (depth < 0.05f) {
        return false;
    }
    const float sx = p.x / depth / eye.tan_x;
    const float sy = p.y / depth / eye.tan_y;
    out->x = (sx * 0.5f + 0.5f) * eye.width - 0.5f;
    out->y = (0.5f - sy * 0.5f) * eye.height - 0.5f;
    out->z = 0.0f;
    out->rhw = 1.0f / depth;
    out->u = u;
    out->v = v;
    return true;
}

// The HUD as measured in a 1324x744 viewport; any 16:9 viewport scales the same way.
constexpr float kRefWidth = 1324.0f;
constexpr float kRefHeight = 744.0f;

// A piece of the HUD copied 1:1 onto a layout canvas (reference HUD pixels).
struct Piece {
    float x0, y0, x1, y1;  // in the HUD
    float x, y;            // top-left on the canvas
};
struct Layout {
    float width, height;   // canvas
    const Piece* pieces;
    int count;
};

// Wrist (reference HUD pixels, measured in game): minimap with the mission and objectives
// beside it, then action skill / shield / health next to grenades / ammo, then level and
// XP. The boxes leave room for elements that appear later (shield, action skill, more
// objective lines).
constexpr Piece kWristPieces[] = {
    {985, 45, 1105, 178, 47, 0},      // minimap
    {930, 176, 1110, 300, 177, 0},    // mission name and objectives (right-aligned)
    {220, 555, 405, 712, 10, 140},    // action skill, shield, health
    {915, 620, 1100, 712, 210, 205},  // grenades, ammo
    {460, 648, 865, 697, 0, 305},     // level, XP
};
constexpr Layout kWristLayout{405, 355, kWristPieces, 5};

// View: the centre of the HUD (enemy health and name, use / pick-up prompts, messages).
constexpr Piece kViewPieces[] = {
    {452, 182, 872, 562, 0, 0},
};
constexpr Layout kViewLayout{420, 380, kViewPieces, 1};

const Layout& layout_of(HudLayout layout) {
    return layout == HudLayout::wrist ? kWristLayout : kViewLayout;
}

void draw_piece(IDirect3DDevice9* device, const EyeCamera& eye, const HudPanel& panel,
                const Layout& layout, const Piece& piece, const Rect& hud) {
    const Vec3 centre = to_vec(panel.centre);
    const Vec3 right = to_vec(panel.right);
    const Vec3 up = to_vec(panel.up);
    // Canvas point -> LOCAL space.
    auto corner = [&](float x, float y) {
        return centre + right * (2.0f * x / layout.width - 1.0f) +
               up * (1.0f - 2.0f * y / layout.height);
    };
    // HUD point -> texture coordinates.
    auto u = [&](float x) { return (hud.x + x / kRefWidth * hud.width) / g_width; };
    auto v = [&](float y) { return (hud.y + y / kRefHeight * hud.height) / g_height; };
    const float x1 = piece.x + (piece.x1 - piece.x0);
    const float y1 = piece.y + (piece.y1 - piece.y0);
    Vertex quad[4];
    if (!project(eye, corner(piece.x, piece.y), u(piece.x0), v(piece.y0), &quad[0]) ||
        !project(eye, corner(x1, piece.y), u(piece.x1), v(piece.y0), &quad[1]) ||
        !project(eye, corner(piece.x, y1), u(piece.x0), v(piece.y1), &quad[2]) ||
        !project(eye, corner(x1, y1), u(piece.x1), v(piece.y1), &quad[3])) {
        return;
    }
    const HRESULT hr = device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(Vertex));
    static int failures = 0;
    if (FAILED(hr) && failures < 5) {
        ++failures;
        LOG("Wrist HUD draw failed 0x%08lx at (%.0f, %.0f)", hr, quad[0].x, quad[0].y);
    }
}

// Draws a panel with every piece of its layout, faded by `opacity`.
void draw_panel(IDirect3DDevice9* device, const EyeCamera& eye, const HudPanel& panel,
                HudLayout layout, float opacity, const Rect& hud) {
    if (!panel.visible || opacity <= 0.0f) {
        return;
    }
    const DWORD fade = static_cast<DWORD>(opacity * 255.0f + 0.5f);
    device->SetRenderState(D3DRS_TEXTUREFACTOR, D3DCOLOR_ARGB(fade, fade, fade, fade));
    const Layout& l = layout_of(layout);
    for (int i = 0; i < l.count; ++i) {
        draw_piece(device, eye, panel, l, l.pieces[i], hud);
    }
}

}  // namespace

float hud_layout_aspect(HudLayout layout) {
    const Layout& l = layout_of(layout);
    return l.height / l.width;
}

float hud_layout_hud_fraction(HudLayout layout) {
    return layout_of(layout).width / kRefWidth;
}

void wrist_set_hud_rect(int x, int y, int width, int height) {
    const uint64_t packed =
        width <= 0 || height <= 0
            ? 0
            : (static_cast<uint64_t>(x & 0xFFFF) | (static_cast<uint64_t>(y & 0xFFFF) << 16) |
               (static_cast<uint64_t>(width & 0xFFFF) << 32) |
               (static_cast<uint64_t>(height & 0xFFFF) << 48));
    g_hud_rect.store(packed, std::memory_order_relaxed);
}

void wrist_on_set_viewport(IDirect3DDevice9* device, const D3DVIEWPORT9* viewport) {
    if (g_internal || viewport == nullptr) {
        return;
    }
    const Rect hud = hud_rect();
    const bool is_hud = hud.width > 0 && static_cast<int>(viewport->X) == hud.x &&
                        static_cast<int>(viewport->Y) == hud.y &&
                        static_cast<int>(viewport->Width) == hud.width &&
                        static_cast<int>(viewport->Height) == hud.height;
    if (is_hud && !g_redirecting) {
        begin_redirect(device);
    } else if (!is_hud && g_redirecting) {
        // Another viewport on our target: the next movie starts. Viewports set while a
        // Scaleform filter target is bound belong to the HUD and are left alone.
        IDirect3DSurface9* target = nullptr;
        device->GetRenderTarget(0, &target);
        const bool ours = target == g_active;
        release(target);
        if (ours) {
            end_redirect(device);
        }
    }
}

IDirect3DSurface9* wrist_on_set_render_target(DWORD index, IDirect3DSurface9* surface) {
    if (g_internal || !g_redirecting || index != 0) {
        return surface;
    }
    return surface == g_saved_target ? g_active : surface;
}

IDirect3DSurface9* wrist_on_set_depth_stencil(IDirect3DSurface9* surface) {
    if (g_internal || !g_redirecting) {
        return surface;
    }
    return surface == g_saved_depth ? g_depth : surface;
}

void wrist_on_end_scene(IDirect3DDevice9* device) {
    end_redirect(device);
    if (g_pass_captured) {
        g_pass_captured = false;
        g_frame_has_hud = true;  // later eye images of this frame reuse this capture
    }
}

void wrist_on_present() {
    g_frame_has_hud = false;
    g_clear_pending = true;
}

void wrist_on_device_lost() {
    g_redirecting = false;
    release(g_saved_target);
    release(g_saved_depth);
    release_targets();
    release(g_state);
}

void wrist_draw(IDirect3DDevice9* device, const StereoFrame& frame, int eye_index, UINT width,
                UINT height) {
    const Rect hud = hud_rect();
    if ((!frame.wrist.visible && !frame.view.visible) || !g_has_content || hud.width == 0 ||
        width != g_width || height != g_height) {
        return;
    }
    const XrView& view = frame.xr.views[eye_index];
    EyeCamera eye{to_vec(frame.eye_position[eye_index]), conjugate(view.pose.orientation),
                  frame.tan_half_x, frame.tan_half_x * height / static_cast<float>(width),
                  static_cast<float>(width), static_cast<float>(height)};

    // The wrist panel only shows while it faces the eye (like looking at a watch).
    const HudPanel& wrist = frame.wrist;
    const Vec3 normal = normalize(cross(to_vec(wrist.right), to_vec(wrist.up)));
    const Vec3 to_eye = normalize(eye.position - to_vec(wrist.centre));
    const float wrist_opacity = std::clamp((dot(normal, to_eye) - 0.35f) / 0.25f, 0.0f, 1.0f);
    if (!frame.view.visible && (!wrist.visible || wrist_opacity <= 0.0f)) {
        return;
    }
    if (g_state == nullptr && FAILED(device->CreateStateBlock(D3DSBT_ALL, &g_state))) {
        return;
    }
    g_state->Capture();
    device->SetVertexShader(nullptr);
    device->SetPixelShader(nullptr);
    device->SetFVF(kFvf);
    D3DVIEWPORT9 viewport{0, 0, width, height, 0.0f, 1.0f};
    g_internal = true;
    device->SetViewport(&viewport);
    g_internal = false;
    device->SetRenderState(D3DRS_ZENABLE, FALSE);
    device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_LIGHTING, FALSE);
    device->SetRenderState(D3DRS_FOGENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);  // premultiplied texture
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_TFACTOR);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_TFACTOR);
    device->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
    device->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    device->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);

    device->SetTexture(0, g_texture);
    draw_panel(device, eye, frame.view, HudLayout::view, 1.0f, hud);
    draw_panel(device, eye, wrist, HudLayout::wrist, wrist_opacity, hud);
    device->SetTexture(0, nullptr);
    g_internal = true;
    g_state->Apply();
    g_internal = false;
}

void wrist_dump(IDirect3DDevice9* device) {
    if (g_texture == nullptr) {
        LOG("Wrist HUD dump: no texture");
        return;
    }
    IDirect3DSurface9* sysmem = nullptr;
    if (SUCCEEDED(device->CreateOffscreenPlainSurface(g_width, g_height, D3DFMT_A8R8G8B8,
                                                      D3DPOOL_SYSTEMMEM, &sysmem, nullptr)) &&
        SUCCEEDED(device->GetRenderTargetData(g_surface, sysmem))) {
        D3DLOCKED_RECT locked;
        if (SUCCEEDED(sysmem->LockRect(&locked, nullptr, D3DLOCK_READONLY))) {
            const std::wstring path = config().dir + L"bl2vr_hud.tga";
            if (FILE* f = _wfopen(path.c_str(), L"wb")) {
                // Uncompressed 32-bit true-colour TGA, top-left origin (BGRA rows).
                uint8_t header[18] = {};
                header[2] = 2;
                header[12] = g_width & 0xFF;
                header[13] = (g_width >> 8) & 0xFF;
                header[14] = g_height & 0xFF;
                header[15] = (g_height >> 8) & 0xFF;
                header[16] = 32;
                header[17] = 0x28;
                fwrite(header, sizeof(header), 1, f);
                for (UINT y = 0; y < g_height; ++y) {
                    fwrite(static_cast<uint8_t*>(locked.pBits) + y * locked.Pitch, g_width * 4,
                           1, f);
                }
                fclose(f);
                const Rect hud = hud_rect();
                LOG("Dumped bl2vr_hud.tga (HUD at %d,%d %dx%d)", hud.x, hud.y, hud.width,
                    hud.height);
            }
            sysmem->UnlockRect();
        }
    }
    release(sysmem);
}

}  // namespace bl2vr
