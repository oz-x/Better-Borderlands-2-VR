#pragma once

#include "ue3.h"

// Scaleform (GFx) movie handling for VR: sends the HUD to the VR panels (wrist and view, see
// wrist.h) and reports when a menu that captures input is open (shown on the theater screen).
namespace bl2vr {

// Game thread. Refreshes the list of live movies (cheap, rescans every couple of seconds).
void hud_update();
// Game thread, after any object's Start event: picks up movies the moment they open.
void hud_movie_started(ue3::UObject* object);
// True while a menu movie (pause, inventory, vendors...) wants input.
bool hud_menu_open();
// Places HUD movies for an eye (-1 = full screen, used for theater / flat frames).
void hud_layout(int eye, unsigned width, unsigned height, float tan_half_x);
// Sends the HUD to the VR panels (on = true, see wrist.h) or back onto the screen. Returns
// true while the HUD is open and on the panels.
bool hud_set_wrist(bool on);

}  // namespace bl2vr
