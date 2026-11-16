#pragma once
#include "xr.h"

// Presents the VR motion controllers to Borderlands 2 as an Xbox gamepad (XInput user 0), so
// every game action works with the game's own gamepad bindings, in single player and co-op.
namespace bl2vr {

bool gamepad_install();
// Game thread, once per frame. `move_rotation` (radians, Unreal yaw sense) rotates the left
// stick so that walking follows the head instead of the aiming hand. Outside menus the right
// stick is not forwarded: the mod turns the body itself.
void gamepad_publish(const xr::ControllerInput& input, float move_rotation, bool in_menu);
// Game rumble requested through XInputSetState, 0..1 per motor (left, right).
void gamepad_rumble(float* left, float* right);

}  // namespace bl2vr
