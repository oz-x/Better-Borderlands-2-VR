#include "config.h"

#include <windows.h>

#include <algorithm>
#include <cstdlib>

namespace bl2vr {

namespace {
Config g_config;

float read_float(const wchar_t* ini, const wchar_t* section, const wchar_t* key, float fallback) {
    wchar_t buf[64];
    GetPrivateProfileStringW(section, key, L"", buf, 64, ini);
    if (buf[0] == 0) {
        return fallback;
    }
    return static_cast<float>(_wtof(buf));
}

int read_int(const wchar_t* ini, const wchar_t* section, const wchar_t* key, int fallback) {
    return static_cast<int>(GetPrivateProfileIntW(section, key, fallback, ini));
}

}  // namespace

const Config& config() {
    return g_config;
}

void config_load() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dir = path;
    dir.resize(dir.find_last_of(L"\\/") + 1);
    g_config.dir = dir;

    const std::wstring ini_path = dir + L"bl2vr.ini";
    const wchar_t* ini = ini_path.c_str();
    Config& c = g_config;
    c.vr_enabled = read_int(ini, L"VR", L"Enabled", c.vr_enabled) != 0;
    c.world_scale = read_float(ini, L"VR", L"WorldScale", c.world_scale);
    c.aim_mode = std::clamp(read_int(ini, L"VR", L"AimMode", c.aim_mode), 0, 2);
    c.turn_speed = read_float(ini, L"VR", L"TurnSpeed", c.turn_speed);
    c.snap_turn = read_float(ini, L"VR", L"SnapTurn", c.snap_turn);
    c.gun_forward = read_float(ini, L"VR", L"GunOffsetForward", c.gun_forward);
    c.gun_right = read_float(ini, L"VR", L"GunOffsetRight", c.gun_right);
    c.gun_up = read_float(ini, L"VR", L"GunOffsetUp", c.gun_up);
    c.positional = read_int(ini, L"VR", L"PositionalTracking", c.positional) != 0;
    c.render_width = read_int(ini, L"VR", L"RenderWidth", c.render_width);
    c.render_height = read_int(ini, L"VR", L"RenderHeight", c.render_height);
    c.resolution_scale = read_float(ini, L"VR", L"ResolutionScale", c.resolution_scale);
    c.uncap_framerate = read_int(ini, L"VR", L"UncapFramerate", c.uncap_framerate) != 0;
    c.recenter_key = read_int(ini, L"Keys", L"Recenter", c.recenter_key);
    c.toggle_vr_key = read_int(ini, L"Keys", L"ToggleStereo", c.toggle_vr_key);
    c.mirror = read_int(ini, L"VR", L"DesktopMirror", c.mirror) != 0;
    c.hud_scale = read_float(ini, L"VR", L"HudScale", c.hud_scale);
    c.hud_distance = read_float(ini, L"VR", L"HudDistance", c.hud_distance);
    c.auto_theater = read_int(ini, L"VR", L"MenusOnScreen", c.auto_theater) != 0;
    c.use_9on12 = read_int(ini, L"Debug", L"Use9On12", c.use_9on12) != 0;
    c.fake_xr = read_int(ini, L"Debug", L"FakeHeadset", c.fake_xr) != 0;
    c.trace = read_int(ini, L"Debug", L"Trace", c.trace) != 0;
    c.fake_yaw = read_float(ini, L"Debug", L"FakeYaw", c.fake_yaw);
    c.fake_pitch = read_float(ini, L"Debug", L"FakePitch", c.fake_pitch);
    c.fake_hand_yaw = read_float(ini, L"Debug", L"FakeHandYaw", c.fake_hand_yaw);
    c.fake_hand_pitch = read_float(ini, L"Debug", L"FakeHandPitch", c.fake_hand_pitch);
    c.fake_fire = read_int(ini, L"Debug", L"FakeFire", c.fake_fire) != 0;
}

}  // namespace bl2vr
