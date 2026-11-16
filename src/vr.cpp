#include "vr.h"

#include <windows.h>
#include <intrin.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <MinHook.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_set>

#include "config.h"
#include "d3d.h"
#include "hud.h"
#include "gamepad.h"
#include "log.h"
#include "mathx.h"
#include "stereo.h"
#include "ue3.h"
#include "xr.h"

using Microsoft::WRL::ComPtr;
using namespace bl2vr::mathx;

namespace bl2vr {

namespace {

// ---- engine entry points ----
// FViewport::Draw(UBOOL bShouldPresent); thiscall, hooked as fastcall with a dummy edx.
using DrawFn = void(__fastcall*)(void* viewport, void* edx, BOOL should_present);
DrawFn g_draw_original = nullptr;
// UGameEngine::Tick: GameViewport->Viewport->Draw(TRUE) — the only call site we need.
constexpr const char* DRAW_CALL_SIG = "8B 8F 98 04 00 00 8B 49 44 85 C9 74 ?? 6A 01 E8";

bool g_initialized = false;
bool g_engine_ok = false;
ComPtr<IDXGIAdapter> g_adapter;

// APlayerController view point virtual used by ULocalPlayer::CalcSceneView:
//   PC->GetPlayerViewPoint(FVector& Location, FRotator& Rotation, UBOOL bForRendering)
// Its vtable offset is read from the call site inside CalcSceneView.
constexpr const char* VIEWPOINT_CALL_SIG =
    "8B 4E 40 8B 01 8B 53 0C 8B 80 ?? ?? ?? ?? 6A 01 57 52 FF D0";
using ViewPointFn = void(__fastcall*)(ue3::UObject* pc, void* edx, ue3::FVector* location,
                                      ue3::FRotator* rotation, BOOL for_rendering);
ViewPointFn g_viewpoint_original = nullptr;
uint32_t g_viewpoint_vtable_offset = 0;

// AWillowPlayerPawn's first-person arm update (thiscall, no arguments). It asks the controller
// for the view point (call site below), then writes the arms component's absolute Translation /
// Rotation and refreshes its transform with the function called at its end.
constexpr const char* ARMS_UPDATE_SIG =
    "53 8B DC 83 EC 08 83 E4 F0 83 C4 04 55 8B 6B 04 89 6C 24 04 8B EC 81 EC ?? ?? ?? ?? A1 ?? "
    "?? ?? ?? 33 C5 89 45 FC 56 8B F1 8B 86 ?? ?? ?? ?? 57 50 E8 ?? ?? ?? ?? D9 46 60 D9 9E";
constexpr const char* ARMS_VIEWPOINT_CALL_SIG =
    "6A 01 8D 4D A0 F3 0F 11 45 E0 F3 0F 10 45 C8 F3 0F 58 45 E4 51 8D 55 F0 52 8B CF F3 0F "
    "11 45 E4 FF D0";
// End of the arm update: `mov eax,[esi+Arms]; ...; mov ecx,[esi+Arms]; call UpdateTransform`.
constexpr const char* ARMS_TRANSFORM_SIG =
    "F3 0F 7E 45 A0 66 0F D6 80 E8 01 00 00 8B 4D A8 89 88 F0 01 00 00 8B 8E ?? ?? ?? ?? E8";
using ArmsUpdateFn = void(__fastcall*)(ue3::UObject* pawn, void* edx);
using UpdateTransformFn = void(__fastcall*)(ue3::UObject* component, void* edx);
ArmsUpdateFn g_arms_update_original = nullptr;
UpdateTransformFn g_update_transform = nullptr;
uintptr_t g_arms_return = 0;          // return address of the view point call inside it
uint32_t g_off_arms = 0;              // pawn -> arms component
constexpr uint32_t kOffComponentTranslation = 0x1DC;  // PrimitiveComponent.Translation
constexpr uint32_t kOffComponentRotation = 0x1E8;     // PrimitiveComponent.Rotation
ue3::FVector g_arms_eye{};            // game eye position seen by the last arm update
bool g_arms_eye_valid = false;

// APawn::GetWeaponStartTraceLocation(AWeapon*) virtual: where instant-hit shots start. Its
// vtable offset is read from the script thunk behind UFunction WillowPawn.GetWeaponStartTraceLocation.
using TraceStartFn = ue3::FVector*(__fastcall*)(ue3::UObject* pawn, void* edx,
                                                ue3::FVector* out, ue3::UObject* weapon);
TraceStartFn g_trace_start_original = nullptr;
bool g_trace_start_tried = false;
int32_t g_off_pawn = -1;
ue3::UObject* g_local_pawn = nullptr;
ue3::UObject* g_local_pc = nullptr;

// Arm rig placement. The weapon's offset inside the rig (hand axes, Unreal units) is measured
// after every placement from the weapon mesh so the grip, not the rig origin, lands in the hand.
ue3::UObject* g_weapon_mesh = nullptr;  // held weapon's first-person mesh (this frame)
Vec3 g_grip_offset{};
bool g_grip_valid = false;
int32_t g_off_weapon = -1;
int32_t g_off_first_person_mesh = -1;
int32_t g_off_local_to_world = -1;
ue3::BoolRef g_suppress_crosshair;

// ---- engine objects / reflection offsets (resolved lazily) ----
ue3::UObject* g_viewport_client = nullptr;
int32_t g_name_get_fov = -1;
int32_t g_name_post_render = -1;
int32_t g_off_game_players = -1;
int32_t g_off_actor = -1;
int32_t g_off_rotation = -1;
bool g_framerate_uncapped = false;

struct TPOV {
    ue3::FVector location;
    ue3::FRotator rotation;
    float fov;
};

// ---- per-frame state (game thread) ----
int g_eye = -1;               // eye being drawn, -1 outside our Draw pair
float g_fov_degrees = 90.0f;  // FOV reported to the engine while drawing an eye
bool g_theater = false;       // user toggle: flat virtual screen instead of stereo
ue3::UObject* g_frame_pc = nullptr;  // controller whose view the current pair renders
xr::FrameInfo g_frame_xr;            // head pose of the current pair
xr::ControllerInput g_frame_input;   // controllers of the current pair

// Head tracking reference (OpenXR LOCAL space).
bool g_recenter_pending = true;
float g_ref_yaw = 0.0f;
Vec3 g_ref_position{};

// Head aim bookkeeping (Unreal rotator units).
bool g_aim_valid = false;
bool g_player_view = false;  // the player (not a cinematic) controls the camera
float g_body_yaw = 0.0f;
ue3::FRotator g_last_written{};

// Resolution management.
int g_setres_attempts = 0;
DWORD g_setres_time = 0;

bool key_pressed(int vk, bool& was_down) {
    const bool down = vk != 0 && (GetAsyncKeyState(vk) & 0x8000) != 0;
    const bool pressed = down && !was_down;
    was_down = down;
    return pressed;
}

bool game_has_focus() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

Vec3 to_vec(const XrVector3f& v) {
    return {v.x, v.y, v.z};
}

XrPosef screen_pose(float distance) {
    // Quad placed `distance` metres in front of the recentred head, facing it.
    const Vec3 forward = yaw_xr({0.0f, 0.0f, -distance}, g_ref_yaw);
    XrPosef pose{};
    pose.position = {g_ref_position.x + forward.x, g_ref_position.y, g_ref_position.z + forward.z};
    pose.orientation = {0.0f, std::sin(g_ref_yaw * 0.5f), 0.0f, std::cos(g_ref_yaw * 0.5f)};
    return pose;
}

XrQuaternionf yaw_pitch_quat(float yaw_degrees, float pitch_degrees) {
    const float yaw = yaw_degrees * kPi / 180.0f;
    const float pitch = pitch_degrees * kPi / 180.0f;
    // Yaw about +Y, then pitch about +X.
    const XrQuaternionf qy{0, std::sin(yaw / 2), 0, std::cos(yaw / 2)};
    const XrQuaternionf qx{std::sin(pitch / 2), 0, 0, std::cos(pitch / 2)};
    return {qy.w * qx.x, qy.y * qx.w, -qy.y * qx.x, qy.w * qx.w};
}

// Quest 3-like optics and a right controller in front of the chest, with configurable
// orientations. Used without a headset (FakeHeadset=1).
void fake_views(xr::FrameInfo& frame, xr::ControllerInput& input) {
    frame.views_valid = true;
    frame.should_render = true;
    const XrQuaternionf q = yaw_pitch_quat(config().fake_yaw, config().fake_pitch);
    for (int eye = 0; eye < 2; ++eye) {
        XrView& v = frame.views[eye];
        v.type = XR_TYPE_VIEW;
        v.pose.orientation = q;
        const Vec3 p = rotate(q, {eye == 0 ? -0.032f : 0.032f, 0.0f, 0.0f});
        v.pose.position = {p.x, p.y, p.z};
        const float outer = 0.9425f, inner = 0.6981f;
        v.fov = {eye == 0 ? -outer : -inner, eye == 0 ? inner : outer, 0.7679f, -0.8727f};
    }
    input = xr::ControllerInput{};
    input.active = true;
    xr::HandInput& right = input.hand[xr::kHandRight];
    right.pose_valid = true;
    right.aim.orientation = yaw_pitch_quat(config().fake_hand_yaw, config().fake_hand_pitch);
    right.aim.position = {0.20f, -0.35f, -0.35f};
    if (config().fake_fire) {
        right.trigger = (GetTickCount() / 400) % 2 != 0 ? 1.0f : 0.0f;
    }
}

void __fastcall viewpoint_hook(ue3::UObject* pc, void* edx, ue3::FVector* location,
                               ue3::FRotator* rotation, BOOL for_rendering);

// Detours the controller's view point virtual (shared by every controller of that class).
void hook_viewpoint(ue3::UObject* pc) {
    if (g_viewpoint_original != nullptr || g_viewpoint_vtable_offset == 0) {
        return;
    }
    void* target = (*reinterpret_cast<void***>(pc))[g_viewpoint_vtable_offset / 4];
    if (MH_CreateHook(target, reinterpret_cast<void*>(&viewpoint_hook),
                      reinterpret_cast<void**>(&g_viewpoint_original)) != MH_OK ||
        MH_EnableHook(target) != MH_OK) {
        LOG("ERROR: failed to hook the view point of %s", ue3::full_name(pc).c_str());
        g_viewpoint_vtable_offset = 0;
        return;
    }
    LOG("View point hooked: exe+%06X", static_cast<unsigned>(reinterpret_cast<uintptr_t>(target) -
                                                             ue3::module_base()));
}

// First-person arms and weapons are drawn with the controller's "foreground" FOV (fixed, so the
// gun keeps its size whatever the FOV slider says). In VR it must match the eye projection, or
// the gun is magnified, pushed off screen and at the wrong depth.
using ForegroundFovFn = float(__fastcall*)(ue3::UObject* pc, void* edx);
ForegroundFovFn g_foreground_fov_original = nullptr;

float __fastcall foreground_fov_hook(ue3::UObject* pc, void* edx) {
    const float fov = g_foreground_fov_original(pc, edx);
    return g_eye >= 0 ? g_fov_degrees : fov;
}

// Vtable offset of the C++ virtual behind a native UFunction, read from its script thunk
// (`mov eax,[reg+offset]; ...; call eax`). 0 if not recognised.
uint32_t thunk_vtable_offset(ue3::UFunction* func) {
    const auto* thunk = reinterpret_cast<const uint8_t*>(ue3::field<uintptr_t>(func, 0xA8));
    for (int i = 0; i < 0x80; ++i) {
        if (thunk[i] == 0x8B && (thunk[i + 1] & 0xF8) == 0x80) {
            const uint32_t offset = *reinterpret_cast<const uint32_t*>(thunk + i + 2);
            return offset <= 0x2000 ? offset : 0;
        }
    }
    return 0;
}

void hook_foreground_fov(ue3::UObject* pc) {
    static bool tried = false;
    if (tried) {
        return;
    }
    tried = true;
    ue3::UFunction* func = ue3::find_function(pc, "GetFOVAngleForeground");
    const uint32_t offset = func != nullptr ? thunk_vtable_offset(func) : 0;
    if (offset == 0) {
        LOG("GetFOVAngleForeground not found: the gun keeps the flat-screen projection");
        return;
    }
    void* target = (*reinterpret_cast<void***>(pc))[offset / 4];
    if (MH_CreateHook(target, reinterpret_cast<void*>(&foreground_fov_hook),
                      reinterpret_cast<void**>(&g_foreground_fov_original)) != MH_OK ||
        MH_EnableHook(target) != MH_OK) {
        LOG("ERROR: failed to hook GetFOVAngleForeground");
        return;
    }
    LOG("Foreground FOV hooked (vtable 0x%X)", offset);
}

ue3::FVector* __fastcall trace_start_hook(ue3::UObject* pawn, void* edx, ue3::FVector* out,
                                          ue3::UObject* weapon);

// Detours the pawn's GetWeaponStartTraceLocation virtual (vtable offset from its script thunk).
void hook_trace_start(ue3::UObject* pawn) {
    if (g_trace_start_tried) {
        return;
    }
    g_trace_start_tried = true;
    ue3::UFunction* func = ue3::find_function(pawn, "GetWeaponStartTraceLocation");
    if (func == nullptr) {
        LOG("GetWeaponStartTraceLocation not found; shots start at the eyes");
        return;
    }
    const uint32_t offset = thunk_vtable_offset(func);
    if (offset == 0) {
        LOG("Trace start vtable offset not found; shots start at the eyes");
        return;
    }
    void* target = (*reinterpret_cast<void***>(pawn))[offset / 4];
    if (MH_CreateHook(target, reinterpret_cast<void*>(&trace_start_hook),
                      reinterpret_cast<void**>(&g_trace_start_original)) != MH_OK ||
        MH_EnableHook(target) != MH_OK) {
        g_trace_start_original = nullptr;
        LOG("ERROR: failed to hook GetWeaponStartTraceLocation");
        return;
    }
    LOG("Trace start hooked (vtable 0x%X)", offset);
}

// Resolves GameEngine.GamePlayers[0].Actor, the local PlayerController.
ue3::UObject* resolve_player() {
    static DWORD last_report = 0;
    auto fail = [](const char* why) -> ue3::UObject* {
        if (GetTickCount() - last_report > 10000) {
            last_report = GetTickCount();
            LOG("No local player: %s", why);
        }
        return nullptr;
    };
    if (!ue3::is_live(g_viewport_client)) {
        g_viewport_client = nullptr;
        return fail("viewport client gone");
    }
    // GamePlayers lives on UGameEngine, the viewport client's outer.
    ue3::UObject* engine = ue3::outer_of(g_viewport_client);
    if (g_off_game_players < 0 && engine != nullptr) {
        g_off_game_players = ue3::property_offset(ue3::class_of(engine), "GamePlayers");
    }
    if (g_off_game_players < 0) {
        return fail("GamePlayers property missing");
    }
    const auto& players = ue3::field<ue3::TArray<ue3::UObject*>>(engine, g_off_game_players);
    if (players.count <= 0 || players.data[0] == nullptr) {
        return fail("no local player");
    }
    ue3::UObject* local_player = players.data[0];
    if (g_off_actor < 0) {
        g_off_actor = ue3::property_offset(ue3::class_of(local_player), "Actor");
    }
    ue3::UObject* pc = g_off_actor >= 0 ? ue3::field<ue3::UObject*>(local_player, g_off_actor)
                                        : nullptr;
    if (!ue3::is_live(pc)) {
        return fail("no player controller");
    }
    if (g_off_rotation < 0) {
        g_off_rotation = ue3::property_offset(ue3::class_of(pc), "Rotation");
        LOG("Offsets: GamePlayers=%d Actor=%d Rotation=%d", g_off_game_players, g_off_actor,
            g_off_rotation);
    }
    if (g_off_rotation < 0) {
        return fail("Rotation property missing");
    }
    hook_viewpoint(pc);
    if (ue3::is_a(pc, "WillowPlayerController")) {
        hook_foreground_fov(pc);
    }
    g_local_pc = pc;
    if (g_off_pawn < 0) {
        g_off_pawn = ue3::property_offset(ue3::class_of(pc), "Pawn");
    }
    ue3::UObject* pawn = g_off_pawn >= 0 ? ue3::field<ue3::UObject*>(pc, g_off_pawn) : nullptr;
    g_local_pawn = ue3::is_live(pawn) ? pawn : nullptr;
    if (g_local_pawn != nullptr && ue3::is_a(g_local_pawn, "WillowPlayerPawn")) {
        hook_trace_start(g_local_pawn);
    }
    return g_viewpoint_original != nullptr ? pc : nullptr;
}

void uncap_framerate() {
    if (g_framerate_uncapped || !config().uncap_framerate) {
        return;
    }
    g_framerate_uncapped = true;
    ue3::UObject* engine = ue3::outer_of(g_viewport_client);
    ue3::UProperty* prop = engine ? ue3::find_property(ue3::class_of(engine), "bSmoothFrameRate")
                                  : nullptr;
    if (prop == nullptr) {
        LOG("bSmoothFrameRate not found");
        return;
    }
    const int32_t offset = ue3::field<int32_t>(prop, ue3::off::property_offset);
    const uint32_t mask = ue3::field<uint32_t>(prop, ue3::off::bool_property_mask);
    ue3::field<uint32_t>(engine, offset) &= ~mask;
    LOG("Frame rate smoothing disabled on %s", ue3::full_name(engine).c_str());
}

// Asks the engine for a back buffer whose aspect matches the symmetric eye frustum.
void manage_resolution(ue3::UObject* pc, const xr::FrameInfo& frame, float tan_x, float tan_y) {
    int width = config().render_width;
    int height = config().render_height;
    if (width <= 0 || height <= 0) {
        // Same pixel density as the runtime's recommendation, widened to the symmetric frustum.
        const XrFovf& fov = frame.views[0].fov;
        const float covered = std::tan(fov.angleRight) - std::tan(fov.angleLeft);
        const float base = xr::available() ? static_cast<float>(xr::recommended_width()) : 1680.0f;
        width = static_cast<int>(base * (2.0f * tan_x) / covered * config().resolution_scale);
        width = std::clamp(width, 640, 4096);
        height = static_cast<int>(width * tan_y / tan_x);
    }
    // UE3 clamps windowed resolutions to the monitor's work area; ask for the largest size with
    // the frustum's aspect that fits instead of having the engine squash it.
    if (HWND window = d3d_window()) {
        MONITORINFO monitor{sizeof(monitor)};
        GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor);
        RECT frame_rect{0, 0, 0, 0};
        AdjustWindowRectEx(&frame_rect, static_cast<DWORD>(GetWindowLongW(window, GWL_STYLE)), FALSE,
                           static_cast<DWORD>(GetWindowLongW(window, GWL_EXSTYLE)));
        const int max_width = (monitor.rcWork.right - monitor.rcWork.left) -
                              (frame_rect.right - frame_rect.left);
        const int max_height = (monitor.rcWork.bottom - monitor.rcWork.top) -
                               (frame_rect.bottom - frame_rect.top);
        const float fit = std::min({1.0f, static_cast<float>(max_width) / width,
                                    static_cast<float>(max_height) / height});
        width = static_cast<int>(width * fit);
        height = static_cast<int>(height * fit);
    }
    width &= ~1;
    height &= ~1;
    unsigned bb_width = 0, bb_height = 0;
    stereo_backbuffer_size(&bb_width, &bb_height);
    if (bb_width == 0 || (std::abs(static_cast<int>(bb_width) - width) <= 8 &&
                          std::abs(static_cast<int>(bb_height) - height) <= 8)) {
        return;
    }
    const DWORD now = GetTickCount();
    if (g_setres_attempts >= 3 || now - g_setres_time < 5000) {
        return;
    }
    ++g_setres_attempts;
    g_setres_time = now;
    wchar_t command[64];
    swprintf_s(command, L"setres %dx%dw", width, height);
    LOG("Back buffer %ux%u, requesting %dx%d", bb_width, bb_height, width, height);
    ue3::console_command(pc, command);
}

void handle_debug_triggers() {
    if (!config().trace) {
        return;
    }
    static DWORD last = 0;
    const DWORD now = GetTickCount();
    if (now - last < 1000) {
        return;
    }
    last = now;
    if (IDirect3DDevice9* device = d3d9_device()) {
        static void* last_vtable = nullptr;
        void** vtable = *reinterpret_cast<void***>(device);
        if (vtable != last_vtable) {
            last_vtable = vtable;
            LOG("Device vtable now %p (Present slot %p)", (void*)vtable, vtable[17]);
        }
    }
    const std::wstring dump = config().dir + L"bl2vr_dump.txt";
    if (GetFileAttributesW(dump.c_str()) != INVALID_FILE_ATTRIBUTES) {
        DeleteFileW(dump.c_str());
        stereo_request_dump();
    }
    const std::wstring trace = config().dir + L"bl2vr_trace.txt";
    if (GetFileAttributesW(trace.c_str()) != INVALID_FILE_ATTRIBUTES) {
        DeleteFileW(trace.c_str());
        d3d_request_trace(3);
    }
    // bl2vr_objs.txt: first line = class name; logs every live instance (debugging aid).
    const std::wstring objs = config().dir + L"bl2vr_objs.txt";
    if (FILE* f = _wfopen(objs.c_str(), L"r")) {
        char name[128] = {};
        fgets(name, sizeof(name), f);
        fclose(f);
        DeleteFileW(objs.c_str());
        name[strcspn(name, "\r\n")] = 0;
        // "fn:Text" lists functions whose path contains Text, "path:Text" objects whose path
        // contains Text; anything else is a class name.
        const bool functions = strncmp(name, "fn:", 3) == 0;
        const bool by_path = strncmp(name, "path:", 5) == 0;
        for (int32_t i = 0; i < ue3::object_count(); ++i) {
            ue3::UObject* obj = ue3::object_at(i);
            if (obj == nullptr) {
                continue;
            }
            const bool match =
                functions ? (ue3::object_name(ue3::class_of(obj)) == "Function" &&
                             ue3::full_name(obj).find(name + 3) != std::string::npos)
                : by_path ? ue3::full_name(obj).find(name + 5) != std::string::npos
                          : ue3::is_a(obj, name);
            if (match) {
                LOG("OBJ %p %s", (void*)obj, ue3::full_name(obj).c_str());
                if (functions) {
                    LOG("    flags=0x%08X native=%u func=exe+%06X params:",
                        ue3::field<uint32_t>(obj, 0x8C), ue3::field<uint16_t>(obj, 0x90),
                        static_cast<unsigned>(ue3::field<uintptr_t>(obj, 0xA8) -
                                              ue3::module_base()));
                    for (ue3::UObject* p = ue3::field<ue3::UObject*>(obj, ue3::off::struct_children);
                         p != nullptr; p = ue3::field<ue3::UObject*>(p, ue3::off::field_next)) {
                        LOG("      %s %s @%d", ue3::object_name(ue3::class_of(p)).c_str(),
                            ue3::object_name(p).c_str(),
                            ue3::field<int32_t>(p, ue3::off::property_offset));
                    }
                    continue;
                }
                if (ue3::is_default_object(obj)) {
                    continue;
                }
                // Bool properties help find state flags (menus, input capture...).
                for (ue3::UStruct* s = ue3::class_of(obj); s != nullptr;
                     s = ue3::field<ue3::UStruct*>(s, ue3::off::struct_super)) {
                    for (ue3::UObject* p = ue3::field<ue3::UObject*>(s, ue3::off::struct_children);
                         p != nullptr; p = ue3::field<ue3::UObject*>(p, ue3::off::field_next)) {
                        const std::string kind = ue3::object_name(ue3::class_of(p));
                        const int32_t offset = ue3::field<int32_t>(p, ue3::off::property_offset);
                        if (kind == "BoolProperty") {
                            const uint32_t mask = ue3::field<uint32_t>(p, ue3::off::bool_property_mask);
                            LOG("    %s = %d", ue3::object_name(p).c_str(),
                                (ue3::field<uint32_t>(obj, offset) & mask) != 0);
                        } else if (kind == "ObjectProperty" || kind == "ComponentProperty") {
                            ue3::UObject* target = ue3::field<ue3::UObject*>(obj, offset);
                            if (target != nullptr) {
                                LOG("    %s -> %s", ue3::object_name(p).c_str(),
                                    ue3::full_name(target).c_str());
                            }
                        } else if (kind == "FloatProperty") {
                            LOG("    %s = %g", ue3::object_name(p).c_str(),
                                ue3::field<float>(obj, offset));
                        } else if (kind == "StructProperty") {
                            const int32_t size =
                                ue3::field<int32_t>(p, ue3::off::property_element_size);
                            const auto& v = ue3::field<ue3::FVector>(obj, offset);
                            LOG("    %s [%d bytes] = (%g, %g, %g) @%d", ue3::object_name(p).c_str(),
                                size, v.x, v.y, v.z, offset);
                        }
                    }
                }
            }
        }
    }
}

void run_command_file(ue3::UObject* pc) {
    if (!config().trace) {
        return;
    }
    const std::wstring path = config().dir + L"bl2vr_cmd.txt";
    FILE* f = _wfopen(path.c_str(), L"r");
    if (f == nullptr) {
        return;
    }
    wchar_t line[512];
    while (fgetws(line, 512, f) != nullptr) {
        std::wstring cmd = line;
        while (!cmd.empty() && (cmd.back() == L'\n' || cmd.back() == L'\r')) {
            cmd.pop_back();
        }
        if (!cmd.empty()) {
            ue3::console_command(pc, cmd.c_str());
        }
    }
    fclose(f);
    DeleteFileW(path.c_str());
}

// Head orientation of `view` relative to the recentred reference, as Unreal axes.
void relative_axes(const XrView& view, Vec3* forward, Vec3* right, Vec3* up) {
    const XrQuaternionf& q = view.pose.orientation;
    *forward = xr_to_ue(yaw_xr(rotate(q, {0, 0, -1}), -g_ref_yaw));
    *right = xr_to_ue(yaw_xr(rotate(q, {1, 0, 0}), -g_ref_yaw));
    *up = xr_to_ue(yaw_xr(rotate(q, {0, 1, 0}), -g_ref_yaw));
}

void recenter(const xr::FrameInfo& frame) {
    const XrView& v = frame.views[0];
    g_ref_yaw = xr_yaw(v.pose.orientation);
    g_ref_position = (to_vec(frame.views[0].pose.position) + to_vec(frame.views[1].pose.position)) *
                     0.5f;
    if (config().fake_xr) {
        g_ref_yaw = 0.0f;  // keep the configured fake orientation visible
        g_ref_position = {};
    }
    g_aim_valid = false;  // keep the current aim direction after recentering
    LOG("Recentred: yaw %.1f deg, head at (%.2f, %.2f, %.2f)", g_ref_yaw * 180.0f / kPi,
        g_ref_position.x, g_ref_position.y, g_ref_position.z);
}

Vec3 relative_forward(const XrQuaternionf& q) {
    return xr_to_ue(yaw_xr(rotate(q, {0, 0, -1}), -g_ref_yaw));
}

float yaw_units(Vec3 forward) {
    return std::atan2(forward.y, forward.x) * kRadToUnits;
}

// Body rotation (Unreal units) for this frame from the right stick: smooth or snap turning.
float stick_turn(float x) {
    static LARGE_INTEGER last{};
    static bool snapped = false;
    LARGE_INTEGER now, frequency;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    const float dt = last.QuadPart == 0
                         ? 0.0f
                         : std::min(0.1f, static_cast<float>(now.QuadPart - last.QuadPart) /
                                              static_cast<float>(frequency.QuadPart));
    last = now;
    const float degrees_to_units = 65536.0f / 360.0f;
    if (config().snap_turn > 0.0f) {
        if (std::abs(x) < 0.3f) {
            snapped = false;
        } else if (std::abs(x) > 0.7f && !snapped) {
            snapped = true;
            return (x > 0.0f ? 1.0f : -1.0f) * config().snap_turn * degrees_to_units;
        }
        return 0.0f;
    }
    if (std::abs(x) < 0.2f) {
        return 0.0f;
    }
    const float scaled = (std::abs(x) - 0.2f) / 0.8f * (x > 0.0f ? 1.0f : -1.0f);
    return scaled * config().turn_speed * dt * degrees_to_units;
}

// Turns the player controller towards the aiming device (head or right controller). Runs once
// per frame before the left eye. Returns the left-stick rotation (radians) that makes walking
// follow the head while the hand aims elsewhere.
float update_aim(ue3::UObject* pc, const xr::FrameInfo& frame, const xr::ControllerInput& input) {
    auto& rotation = ue3::field<ue3::FRotator>(pc, g_off_rotation);
    // Cinematics and other cameras that do not follow the controller keep their own yaw.
    if (config().aim_mode == 0 || !g_player_view) {
        g_aim_valid = false;
        return 0.0f;
    }
    const float head_yaw = yaw_units(relative_forward(frame.views[0].pose.orientation));
    const xr::HandInput& hand = input.hand[xr::kHandRight];
    const bool by_hand = config().aim_mode == 2 && input.active && hand.pose_valid;
    const Vec3 forward =
        relative_forward(by_hand ? hand.aim.orientation : frame.views[0].pose.orientation);
    const float aim_yaw = yaw_units(forward);
    const float aim_pitch =
        std::atan2(forward.z, std::sqrt(forward.x * forward.x + forward.y * forward.y)) * kRadToUnits;
    if (!g_aim_valid) {
        g_body_yaw = static_cast<float>(rotation.yaw) - head_yaw;
        g_aim_valid = true;
    } else if (input.active) {
        // With motion controllers the right stick turns the body here and the game never sees
        // it, so recoil and other game-side aim changes cannot spin the player's view.
        g_body_yaw += stick_turn(input.hand[xr::kHandRight].stick.x);
    } else {
        // Stick / mouse turning since our last write rotates the body.
        g_body_yaw += static_cast<float>(wrap_units(rotation.yaw - g_last_written.yaw));
    }
    g_body_yaw = std::fmod(g_body_yaw, 65536.0f);
    ue3::FRotator aim;
    aim.pitch = static_cast<int32_t>(std::lround(std::clamp(aim_pitch, -16000.0f, 16000.0f))) & 0xFFFF;
    aim.yaw = static_cast<int32_t>(std::lround(g_body_yaw + aim_yaw)) & 0xFFFF;
    aim.roll = 0;
    rotation = aim;
    g_last_written = aim;
    return wrap_units(static_cast<int32_t>(std::lround(head_yaw - aim_yaw))) * kUnitsToRad;
}

TPOV eye_pov(const TPOV& game_pov, const xr::FrameInfo& frame, int eye, float base_yaw_units) {
    const float base_yaw = base_yaw_units * kUnitsToRad;
    const XrView& view = frame.views[eye];
    Vec3 forward, right, up;
    relative_axes(view, &forward, &right, &up);

    Vec3 offset;
    if (config().positional) {
        offset = to_vec(view.pose.position) - g_ref_position;
    } else {
        // Keep only the eye separation around the head centre.
        const Vec3 centre =
            (to_vec(frame.views[0].pose.position) + to_vec(frame.views[1].pose.position)) * 0.5f;
        offset = to_vec(view.pose.position) - centre;
    }
    const Vec3 local = xr_to_ue(yaw_xr(offset, -g_ref_yaw)) * config().world_scale;
    const Vec3 world = yaw_ue(local, base_yaw);

    TPOV pov = game_pov;
    pov.location.x += world.x;
    pov.location.y += world.y;
    pov.location.z += world.z;
    const Rotator r = axes_to_rotator(yaw_ue(forward, base_yaw), yaw_ue(right, base_yaw),
                                      yaw_ue(up, base_yaw));
    pov.rotation = {r.pitch, r.yaw, r.roll};
    pov.fov = g_fov_degrees;
    return pov;
}

struct HandPose {
    ue3::FVector location;
    ue3::FRotator rotation;
    Vec3 forward, right, up;  // world axes
};

// World pose of the right controller, placed relative to the game's eye position `eye` the same
// way eye_pov places the cameras. False unless the right hand is aiming.
bool hand_world(const ue3::FVector& eye, HandPose* out) {
    const xr::HandInput& hand = g_frame_input.hand[xr::kHandRight];
    if (config().aim_mode != 2 || !g_frame_input.active || !hand.pose_valid || !g_aim_valid ||
        !g_player_view) {
        return false;
    }
    const float base_yaw = g_body_yaw * kUnitsToRad;
    const Vec3 local =
        xr_to_ue(yaw_xr(to_vec(hand.aim.position) - g_ref_position, -g_ref_yaw)) * config().world_scale;
    const Vec3 world = yaw_ue(local, base_yaw);
    const XrQuaternionf& q = hand.aim.orientation;
    out->forward = yaw_ue(xr_to_ue(yaw_xr(rotate(q, {0, 0, -1}), -g_ref_yaw)), base_yaw);
    out->right = yaw_ue(xr_to_ue(yaw_xr(rotate(q, {1, 0, 0}), -g_ref_yaw)), base_yaw);
    out->up = yaw_ue(xr_to_ue(yaw_xr(rotate(q, {0, 1, 0}), -g_ref_yaw)), base_yaw);
    out->location = {eye.x + world.x, eye.y + world.y, eye.z + world.z};
    const Rotator r = axes_to_rotator(out->forward, out->right, out->up);
    out->rotation = {r.pitch, r.yaw, r.roll};
    return true;
}

ue3::FVector* __fastcall trace_start_hook(ue3::UObject* pawn, void* edx, ue3::FVector* out,
                                          ue3::UObject* weapon) {
    ue3::FVector* start = g_trace_start_original(pawn, edx, out, weapon);
    HandPose hand;
    if (pawn == g_local_pawn && hand_world(*start, &hand)) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            LOG("Shots start at the controller (%.0f, %.0f, %.0f) instead of (%.0f, %.0f, %.0f)",
                hand.location.x, hand.location.y, hand.location.z, start->x, start->y, start->z);
        }
        *start = hand.location;  // shots leave from the controller, not the eyes
    }
    return start;
}

// After the game positions the first-person arms, move them (and the weapon they hold) to the
// right controller: rig rotation = hand rotation, rig origin shifted so the weapon's grip lands
// in the hand.
void __fastcall arms_update_hook(ue3::UObject* pawn, void* edx) {
    g_arms_eye_valid = false;
    g_arms_update_original(pawn, edx);
    HandPose hand;
    if (pawn != g_local_pawn || !g_arms_eye_valid || g_update_transform == nullptr ||
        !hand_world(g_arms_eye, &hand)) {
        return;
    }
    ue3::UObject* arms = ue3::field<ue3::UObject*>(pawn, g_off_arms);
    if (arms == nullptr) {
        return;
    }
    const Vec3 grip = g_grip_valid ? g_grip_offset : Vec3{0.0f, 0.0f, 0.0f};
    const Vec3 shift = hand.forward * (config().gun_forward - grip.x) +
                       hand.right * (config().gun_right - grip.y) +
                       hand.up * (config().gun_up - grip.z);
    const ue3::FVector root{hand.location.x + shift.x, hand.location.y + shift.y,
                            hand.location.z + shift.z};
    ue3::field<ue3::FVector>(arms, kOffComponentTranslation) = root;
    ue3::field<ue3::FRotator>(arms, kOffComponentRotation) = hand.rotation;
    g_update_transform(arms, nullptr);

    // The transform update carried the attached weapon along: measure where it ended up in
    // the rig (hand axes) so the next placement puts the weapon itself in the hand.
    if (g_weapon_mesh == nullptr || !ue3::is_live(g_weapon_mesh)) {
        return;
    }
    const float* w = &ue3::field<float>(g_weapon_mesh, g_off_local_to_world);
    const Vec3 d = Vec3{w[12] - root.x, w[13] - root.y, w[14] - root.z};
    const Vec3 local{dot(d, hand.forward), dot(d, hand.right), dot(d, hand.up)};
    if (config().trace) {
        static DWORD last = 0;
        if (GetTickCount() - last > 3000) {
            last = GetTickCount();
            const Vec3 miss{w[12] - hand.location.x, w[13] - hand.location.y,
                            w[14] - hand.location.z};
            LOG("Grip: (%.1f %.1f %.1f), weapon %.1f uu from the hand", local.x, local.y, local.z,
                std::sqrt(dot(miss, miss)));
        }
    }
    if (std::abs(local.x) > 200.0f || std::abs(local.y) > 200.0f || std::abs(local.z) > 200.0f) {
        return;  // weapon not attached to the arms (switching, holstered...)
    }
    if (!g_grip_valid) {
        g_grip_offset = local;
        g_grip_valid = true;
        LOG("Weapon grip in arm rig: (%.1f, %.1f, %.1f)", local.x, local.y, local.z);
    } else {
        g_grip_offset = g_grip_offset * 0.9f + local * 0.1f;
    }
}

// Finds the held weapon's first-person mesh for the arm hook's grip calibration, and hides the
// screen-centre crosshair, which no longer matches where a hand-held gun points.
void update_weapon() {
    g_weapon_mesh = nullptr;
    if (g_local_pawn == nullptr || !ue3::is_live(g_local_pawn)) {
        return;
    }
    if (g_off_weapon < 0) {
        g_off_weapon = ue3::property_offset(ue3::class_of(g_local_pawn), "Weapon");
    }
    ue3::UObject* weapon =
        g_off_weapon >= 0 ? ue3::field<ue3::UObject*>(g_local_pawn, g_off_weapon) : nullptr;
    if (!ue3::is_live(weapon) || !ue3::is_a(weapon, "WillowWeapon")) {
        return;
    }
    if (g_off_first_person_mesh < 0) {
        g_off_first_person_mesh = ue3::property_offset(ue3::class_of(weapon), "FirstPersonMesh");
        g_suppress_crosshair = ue3::find_bool(ue3::class_of(weapon), "bSuppressCrosshair");
    }
    if (config().aim_mode == 2 && g_aim_valid) {
        g_suppress_crosshair.set(weapon, true);
    }
    ue3::UObject* mesh = g_off_first_person_mesh >= 0
                             ? ue3::field<ue3::UObject*>(weapon, g_off_first_person_mesh)
                             : nullptr;
    if (mesh == nullptr) {
        return;
    }
    if (g_off_local_to_world == -1) {
        g_off_local_to_world = ue3::property_offset(ue3::class_of(mesh), "_LocalToWorld");
        LOG("Weapon mesh LocalToWorld @%d", g_off_local_to_world);
        if (g_off_local_to_world < 0) {
            g_off_local_to_world = -2;  // not exposed: keep the configured offset only
        }
    }
    if (g_off_local_to_world >= 0) {
        g_weapon_mesh = mesh;
    }
}

void __fastcall draw_hook(void* viewport, void* edx, BOOL should_present) {
    static bool recenter_down = false, toggle_down = false;
    if (g_eye >= 0 || !config().vr_enabled || !g_engine_ok || g_viewport_client == nullptr) {
        g_draw_original(viewport, edx, should_present);
        return;
    }
    handle_debug_triggers();
    const bool focus = game_has_focus();
    if (key_pressed(config().recenter_key, recenter_down) && focus) {
        g_recenter_pending = true;
    }
    if (key_pressed(config().toggle_vr_key, toggle_down) && focus) {
        g_theater = !g_theater;
        LOG("Theater mode %s", g_theater ? "on" : "off");
    }

    // Whatever path this frame takes, the virtual gamepad gets this frame's controller state
    // (inactive when there is none, so no button can stay stuck down).
    xr::ControllerInput input;
    float move_rotation = 0.0f;
    bool in_menu = true;  // cleared on the stereo gameplay path below
    struct PublishOnExit {
        xr::ControllerInput& input;
        float& rotation;
        bool& menu;
        ~PublishOnExit() { gamepad_publish(input, rotation, menu); }
    } publish{input, move_rotation, in_menu};

    const bool use_xr = xr::available() && !config().fake_xr;
    if (use_xr) {
        xr::poll_events();
        if (xr::consume_recenter_request()) {
            g_recenter_pending = true;
        }
        if (!xr::session_running()) {
            g_draw_original(viewport, edx, should_present);
            return;
        }
    } else if (!config().fake_xr) {
        g_draw_original(viewport, edx, should_present);
        return;
    }

    stereo_wait_idle(100);
    StereoFrame frame;
    if (use_xr) {
        if (!xr::begin_frame(frame.xr)) {
            g_draw_original(viewport, edx, should_present);
            return;
        }
        frame.xr_frame = true;
        xr::input_sync(frame.xr.display_time, input);
        float rumble_left = 0.0f, rumble_right = 0.0f;
        gamepad_rumble(&rumble_left, &rumble_right);
        xr::haptic(xr::kHandLeft, rumble_left);
        xr::haptic(xr::kHandRight, rumble_right);
    } else {
        fake_views(frame.xr, input);
    }
    if (frame.xr.views_valid && g_recenter_pending) {
        g_recenter_pending = false;
        recenter(frame.xr);
    }
    frame.screen_pose = screen_pose(2.2f);
    frame.screen_width = 2.6f;

    ue3::UObject* pc = resolve_player();
    if (pc != nullptr) {
        uncap_framerate();
        run_command_file(pc);
        hud_update();
    }
    unsigned bb_width = 0, bb_height = 0;
    stereo_backbuffer_size(&bb_width, &bb_height);
    const bool menu = config().auto_theater && pc != nullptr && hud_menu_open();
    if (pc == nullptr || g_theater || menu || !frame.xr.views_valid) {
        frame.mode = FrameMode::theater;
        hud_layout(-1, bb_width, bb_height, 1.0f);
        stereo_queue_frame(frame);
        g_draw_original(viewport, edx, should_present);
        return;
    }

    // Symmetric frustum covering both eyes' asymmetric FOV.
    float tan_x = 0.0f, tan_y = 0.0f;
    for (const XrView& v : frame.xr.views) {
        tan_x = std::max({tan_x, std::tan(-v.fov.angleLeft), std::tan(v.fov.angleRight)});
        tan_y = std::max({tan_y, std::tan(-v.fov.angleDown), std::tan(v.fov.angleUp)});
    }
    frame.tan_half_x = tan_x;
    g_fov_degrees = 2.0f * std::atan(tan_x) * 180.0f / kPi;
    manage_resolution(pc, frame.xr, tan_x, tan_y);
    // The player controls the view unless a cinematic owns the camera (or there is no pawn).
    static const ue3::BoolRef cinematic = ue3::find_bool(ue3::class_of(pc), "bCinematicMode");
    g_player_view = g_local_pawn != nullptr && !cinematic.get(pc);
    move_rotation = update_aim(pc, frame.xr, input);
    in_menu = false;
    g_frame_input = input;
    update_weapon();
    if (config().trace) {
        static DWORD last_status = 0;
        if (GetTickCount() - last_status > 3000) {
            last_status = GetTickCount();
            LOG("Status: player_view=%d aim_valid=%d input=%d hand=%d pawn=%p grip=%d", g_player_view,
                g_aim_valid, input.active, input.hand[xr::kHandRight].pose_valid,
                (void*)g_local_pawn, g_grip_valid);
        }
    }

    // The view point hook swaps in each eye's pose while CalcSceneView runs inside Draw.
    frame.mode = FrameMode::stereo;
    stereo_queue_frame(frame);
    g_frame_pc = pc;
    g_frame_xr = frame.xr;
    if (d3d_tracing()) {
        LOG("TRACE Draw pair present=%d", should_present);
    }
    g_eye = 0;
    hud_layout(0, bb_width, bb_height, tan_x);
    g_draw_original(viewport, edx, FALSE);
    g_eye = 1;
    hud_layout(1, bb_width, bb_height, tan_x);
    g_draw_original(viewport, edx, should_present);
    g_eye = -1;
    g_frame_pc = nullptr;
}

void __fastcall viewpoint_hook(ue3::UObject* pc, void* edx, ue3::FVector* location,
                               ue3::FRotator* rotation, BOOL for_rendering) {
    g_viewpoint_original(pc, edx, location, rotation, for_rendering);
    // The pawn's arm update asks for the view point (it happens during the tick and again while
    // drawing). Keep the game's answer, so both eyes see the same arms, and remember the eye
    // position: arms_update_hook places the arms at the controller relative to it.
    if (g_arms_return != 0 && _ReturnAddress() == reinterpret_cast<void*>(g_arms_return)) {
        if (pc == g_local_pc) {
            g_arms_eye = *location;
            g_arms_eye_valid = true;
        }
        return;
    }
    if (g_eye < 0 || pc != g_frame_pc) {
        return;
    }
    TPOV game{*location, *rotation, g_fov_degrees};
    const float base_yaw = (config().aim_mode != 0 && g_player_view && g_aim_valid)
                               ? g_body_yaw
                               : static_cast<float>(rotation->yaw);
    const TPOV eye = eye_pov(game, g_frame_xr, g_eye, base_yaw);
    *location = eye.location;
    *rotation = eye.rotation;
}

void on_process_event(ue3::UObject* obj, ue3::UFunction* func, void* params, bool post) {
    if (g_name_get_fov < 0) {
        return;  // names not resolved yet (engine still starting)
    }
    if (config().trace && !post) {
        static std::unordered_set<ue3::UFunction*> seen;
        if (seen.size() < 30000 && seen.insert(func).second) {
            LOG("PE-NEW %s on %s (eye %d)", ue3::full_name(func).c_str(),
                ue3::object_name(ue3::class_of(obj)).c_str(), g_eye);
        }
    }
    const int32_t name = ue3::name_of(func).index;
    if (post && g_eye >= 0 && name == g_name_get_fov) {
        static int32_t return_offset = ue3::property_offset(func, "ReturnValue");
        if (return_offset >= 0) {
            ue3::field<float>(params, return_offset) = g_fov_degrees;
        }
    } else if (!post && name == g_name_post_render && g_viewport_client != obj &&
               ue3::is_a(obj, "GameViewportClient")) {
        g_viewport_client = obj;
        LOG("Viewport client: %s", ue3::full_name(obj).c_str());
    }
}

// Locates and hooks the first-person arm update (optional: without it the weapon stays at the
// eyes but aiming and shooting from the hand still work).
void install_arms_hook() {
    const uintptr_t update = ue3::find_pattern(ARMS_UPDATE_SIG);
    const uintptr_t call = ue3::find_pattern(ARMS_VIEWPOINT_CALL_SIG);
    const uintptr_t tail = ue3::find_pattern(ARMS_TRANSFORM_SIG);
    if (update == 0 || call == 0 || tail == 0) {
        LOG("Arm update not found (%d/%d/%d): the weapon stays at the eyes", update != 0,
            call != 0, tail != 0);
        return;
    }
    g_arms_return = call + 0x22;  // the pattern ends with `call eax`
    g_off_arms = *reinterpret_cast<const uint32_t*>(tail + 24);
    const uintptr_t call_end = tail + 33;  // E8 rel32 ends the pattern + 4 bytes
    g_update_transform =
        reinterpret_cast<UpdateTransformFn>(call_end + *reinterpret_cast<const int32_t*>(tail + 29));
    if (MH_CreateHook(reinterpret_cast<void*>(update), reinterpret_cast<void*>(&arms_update_hook),
                      reinterpret_cast<void**>(&g_arms_update_original)) != MH_OK ||
        MH_EnableHook(reinterpret_cast<void*>(update)) != MH_OK) {
        LOG("ERROR: failed to hook the arm update");
        g_update_transform = nullptr;
        return;
    }
    LOG("Arm update hooked at exe+%06X (arms @0x%X, transform update exe+%06X)",
        static_cast<unsigned>(update - ue3::module_base()), g_off_arms,
        static_cast<unsigned>(reinterpret_cast<uintptr_t>(g_update_transform) - ue3::module_base()));
}

bool install_draw_hook() {
    const uintptr_t match = ue3::find_pattern(DRAW_CALL_SIG);
    if (match == 0) {
        LOG("ERROR: FViewport::Draw call site not found");
        return false;
    }
    const uintptr_t call_end = match + 20;  // E8 rel32 ends the pattern
    const uintptr_t draw = call_end + *reinterpret_cast<const int32_t*>(match + 16);
    LOG("FViewport::Draw at exe+%06X", static_cast<unsigned>(draw - ue3::module_base()));

    const uintptr_t viewpoint_call = ue3::find_pattern(VIEWPOINT_CALL_SIG);
    if (viewpoint_call == 0) {
        LOG("ERROR: CalcSceneView view point call not found");
        return false;
    }
    g_viewpoint_vtable_offset = *reinterpret_cast<const uint32_t*>(viewpoint_call + 10);
    LOG("View point vtable offset 0x%X", g_viewpoint_vtable_offset);
    install_arms_hook();
    if (MH_CreateHook(reinterpret_cast<void*>(draw), reinterpret_cast<void*>(&draw_hook),
                      reinterpret_cast<void**>(&g_draw_original)) != MH_OK ||
        MH_EnableHook(reinterpret_cast<void*>(draw)) != MH_OK) {
        LOG("ERROR: failed to hook FViewport::Draw");
        return false;
    }
    return true;
}

// Name indices are resolved once the engine has loaded its core packages.
DWORD WINAPI resolve_names_thread(LPVOID) {
    for (int attempt = 0; attempt < 600; ++attempt) {
        Sleep(100);
        if (!ue3::ready()) {
            continue;
        }
        const int32_t post_render = ue3::find_name_index("PostRender");
        const int32_t get_fov = ue3::find_name_index("GetFOVAngle");
        if (post_render >= 0 && get_fov >= 0) {
            g_name_post_render = post_render;
            g_name_get_fov = get_fov;  // written last: enables the observer
            LOG("Engine names resolved");
            return 0;
        }
    }
    LOG("ERROR: engine names never appeared");
    return 0;
}

}  // namespace

void vr_early_init() {
    if (g_initialized) {
        return;
    }
    g_initialized = true;
    config_load();
    log_init((config().dir + L"bl2vr.log").c_str());
    LOG("BL2 VR starting (enabled=%d fake=%d)", config().vr_enabled, config().fake_xr);
    if (!config().vr_enabled) {
        return;
    }
    g_engine_ok = ue3::init() && install_draw_hook();
    if (g_engine_ok) {
        gamepad_install();
    }
    if (!g_engine_ok) {
        return;
    }
    ue3::set_process_event_observer(&on_process_event);
    CloseHandle(CreateThread(nullptr, 0, &resolve_names_thread, nullptr, 0, nullptr));

    if (!config().fake_xr && !xr::init_instance()) {
        LOG("No OpenXR headset available: the game runs flat. Start Virtual Desktop / Quest Link "
            "before the game.");
        return;
    }
    LUID luid;
    if (!config().fake_xr && xr::adapter_luid(&luid)) {
        ComPtr<IDXGIFactory4> factory;
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
            factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&g_adapter));
        }
    }
}

IDXGIAdapter* vr_adapter() {
    return g_adapter.Get();
}

void vr_on_d3d12_ready() {
    if (xr::available() && !config().fake_xr) {
        xr::create_session(d3d12_device(), d3d12_queue());
    }
}

}  // namespace bl2vr
