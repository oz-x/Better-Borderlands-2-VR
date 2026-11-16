#pragma once

// Scaleform (GFx) movie handling for VR: keeps the HUD inside the comfortable centre of each
// eye with a small parallax so it reads as floating in front of the player, and reports when a
// menu that captures input is open (shown on the theater screen instead).
namespace bl2vr {

// Game thread. Refreshes the list of live movies (cheap, rescans every couple of seconds).
void hud_update();
// True while a menu movie (pause, inventory, vendors...) wants input.
bool hud_menu_open();
// Places HUD movies for an eye (-1 = full screen, used for theater / flat frames).
void hud_layout(int eye, unsigned width, unsigned height, float tan_half_x);

}  // namespace bl2vr
