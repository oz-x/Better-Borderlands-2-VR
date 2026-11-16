#include "gamepad.h"

#include <windows.h>
#include <Xinput.h>

#include <MinHook.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>

#include "log.h"

namespace bl2vr {

namespace {

using GetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
using SetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_VIBRATION*);
GetStateFn g_get_state = nullptr;
SetStateFn g_set_state = nullptr;

std::mutex g_mutex;
bool g_active = false;  // VR controllers are driving user 0
XINPUT_GAMEPAD g_pad{};
DWORD g_packet = 0;
WORD g_rumble[2] = {0, 0};

SHORT axis(float v) {
    return static_cast<SHORT>(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f));
}

BYTE trigger(float v) {
    return static_cast<BYTE>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
}

DWORD WINAPI hook_get_state(DWORD user, XINPUT_STATE* state) {
    if (user == 0 && state != nullptr) {
        std::lock_guard lock(g_mutex);
        if (g_active) {
            state->dwPacketNumber = g_packet;
            state->Gamepad = g_pad;
            return ERROR_SUCCESS;
        }
    }
    return g_get_state(user, state);
}

DWORD WINAPI hook_set_state(DWORD user, XINPUT_VIBRATION* vibration) {
    if (user == 0 && vibration != nullptr) {
        std::lock_guard lock(g_mutex);
        g_rumble[0] = vibration->wLeftMotorSpeed;
        g_rumble[1] = vibration->wRightMotorSpeed;
        if (g_active) {
            return ERROR_SUCCESS;  // no physical pad behind the virtual one
        }
    }
    return g_set_state(user, vibration);
}

bool hook(HMODULE module, int ordinal, void* detour, void** original) {
    void* target = reinterpret_cast<void*>(GetProcAddress(module, MAKEINTRESOURCEA(ordinal)));
    return target != nullptr && MH_CreateHook(target, detour, original) == MH_OK &&
           MH_EnableHook(target) == MH_OK;
}

}  // namespace

bool gamepad_install() {
    // Borderlands 2 imports XInputGetState (#2) and XInputSetState (#3) from xinput1_3.dll.
    HMODULE module = GetModuleHandleW(L"xinput1_3.dll");
    if (module == nullptr) {
        module = LoadLibraryW(L"xinput1_3.dll");
    }
    const bool ok = module != nullptr &&
                    hook(module, 2, reinterpret_cast<void*>(&hook_get_state),
                         reinterpret_cast<void**>(&g_get_state)) &&
                    hook(module, 3, reinterpret_cast<void*>(&hook_set_state),
                         reinterpret_cast<void**>(&g_set_state));
    LOG(ok ? "Virtual gamepad ready" : "ERROR: could not hook XInput");
    return ok;
}

void gamepad_publish(const xr::ControllerInput& input, float move_rotation, bool in_menu) {
    XINPUT_GAMEPAD pad{};
    const bool active = input.active;
    if (active) {
        const xr::HandInput& left = input.hand[xr::kHandLeft];
        const xr::HandInput& right = input.hand[xr::kHandRight];

        // Left stick: walk relative to the head. XInput: X = right, Y = forward.
        const float c = std::cos(move_rotation), s = std::sin(move_rotation);
        const float forward = left.stick.y, strafe = left.stick.x;
        pad.sThumbLX = axis(forward * s + strafe * c);
        pad.sThumbLY = axis(forward * c - strafe * s);
        // Right stick: in gameplay the mod turns the body itself (see update_aim), so the game
        // only gets the horizontal axis in menus. Vertical look comes from the hand, so that
        // axis is free: up = D-pad up (quick weapon select); down opens the backpack in
        // gameplay (vr.cpp calls ShowStatusMenu_Inventory) and is B (close) in menus.
        pad.sThumbRX = in_menu ? axis(right.stick.x) : 0;
        const bool vertical = std::abs(right.stick.y) > 0.7f && std::abs(right.stick.x) < 0.5f;
        if (vertical && right.stick.y > 0.0f) {
            pad.wButtons |= XINPUT_GAMEPAD_DPAD_UP;
        }
        if (vertical && right.stick.y < 0.0f && in_menu) {
            pad.wButtons |= XINPUT_GAMEPAD_B;
        }
        pad.bLeftTrigger = trigger(left.trigger);    // aim down sights
        pad.bRightTrigger = trigger(right.trigger);  // fire
        if (left.grip > 0.6f) pad.wButtons |= XINPUT_GAMEPAD_LEFT_SHOULDER;   // action skill
        if (right.grip > 0.6f) pad.wButtons |= XINPUT_GAMEPAD_RIGHT_SHOULDER; // grenade
        if (right.primary) pad.wButtons |= XINPUT_GAMEPAD_A;                  // jump
        if (right.secondary) pad.wButtons |= XINPUT_GAMEPAD_B;                // crouch
        if (left.primary) pad.wButtons |= XINPUT_GAMEPAD_X;                   // reload / use
        if (left.secondary) pad.wButtons |= XINPUT_GAMEPAD_Y;                 // swap weapon
        if (left.stick_click) pad.wButtons |= XINPUT_GAMEPAD_LEFT_THUMB;      // sprint
        if (right.stick_click) pad.wButtons |= XINPUT_GAMEPAD_RIGHT_THUMB;    // melee
        if (left.menu) pad.wButtons |= XINPUT_GAMEPAD_START;                  // pause
    }
    std::lock_guard lock(g_mutex);
    if (active != g_active || memcmp(&pad, &g_pad, sizeof(pad)) != 0) {
        ++g_packet;
    }
    g_active = active;
    g_pad = pad;
}

void gamepad_rumble(float* left, float* right) {
    std::lock_guard lock(g_mutex);
    *left = g_rumble[0] / 65535.0f;
    *right = g_rumble[1] / 65535.0f;
}

}  // namespace bl2vr
