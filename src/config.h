#pragma once
#include <string>

namespace bl2vr {

struct Config {
    bool vr_enabled = true;        // master switch
    float world_scale = 100.0f;    // Unreal units per metre (BL2 eye height 150 uu = 1.5 m)
    int aim_mode = 2;              // 0 = stick/mouse, 1 = head, 2 = right controller
    float turn_speed = 180.0f;     // smooth turning with the right stick, degrees per second
    float snap_turn = 0.0f;        // > 0: snap turning by this many degrees instead
    // Weapon grip offset from the controller, in Unreal units (forward, right, up), so the gun
    // model sits in the hand.
    float gun_forward = 0.0f;
    float gun_right = 0.0f;
    float gun_up = 0.0f;
    bool positional = true;        // apply 6DoF head position
    int render_width = 0;          // 0 = derive from the runtime's recommended size
    int render_height = 0;
    float resolution_scale = 1.0f; // multiplier on the recommended per-eye size
    bool uncap_framerate = true;   // disable BL2's 62 fps smoothing
    int recenter_key = 0x75;       // VK_F6
    int toggle_vr_key = 0x76;      // VK_F7 (stereo / theater screen)
    bool mirror = true;            // show the right eye on the desktop window
    float hud_scale = 0.6f;        // fraction of each eye the HUD occupies (1 = full screen)
    float hud_distance = 2.0f;     // metres the HUD appears in front of you (0 = infinity)
    bool auto_theater = true;      // show menus on the theater screen
    // Debug
    bool use_9on12 = true;
    bool fake_xr = false;          // stereo pipeline without a headset (eye dumps)
    bool trace = false;            // allow bl2vr_trace.txt trigger
    float fake_yaw = 0.0f;         // fake headset orientation, degrees (positive = left)
    float fake_pitch = 0.0f;       // degrees (positive = up)
    float fake_hand_yaw = 0.0f;    // fake right controller orientation, degrees
    float fake_hand_pitch = 0.0f;
    bool fake_fire = false;        // pulse the fake right trigger
    std::wstring dir;              // Binaries/Win32 directory, with trailing backslash
};

const Config& config();
void config_load();

}  // namespace bl2vr
