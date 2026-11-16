#include "hud.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

#include "config.h"
#include "log.h"
#include "ue3.h"
#include "wrist.h"

namespace bl2vr {

namespace {

std::vector<ue3::UObject*> g_movies;  // every live GFxMoviePlayer
DWORD g_last_scan = 0;
bool g_resolved = false;
ue3::BoolRef g_movie_is_open;
ue3::UFunction* g_set_viewport = nullptr;
int32_t g_arg_x = -1, g_arg_y = -1, g_arg_width = -1, g_arg_height = -1;
uint16_t g_args_size = 0;
std::string g_menu_name;  // last reported menu, for the log

// BL2 screens that are menus but set neither bFullscreenMovie nor bPauseGameWhileActive
// (input/focus flags do not tell them apart from on-screen notifications).
constexpr const char* kMenuClasses[] = {
    "StatusMenuExGFxMovie",         // backpack, map, missions, skills, challenges
    "QuestAcceptGFxMovie",          // mission accept / mission complete and rewards
    "VendingMachineExGFxMovie",     // vendors
    "TwoPanelInterfaceGFxMovie",    // bank, stash, Claptrap's storage
    "FastTravelStationGFxMovie",
    "VehicleSpawnStationGFxMovie",
    "TradingGFxMovie",
    "CustomizationGFxMovie",
    "ItemInspectionGFxMovie",
};

// Menu facts live on BL2's own movie classes, so they are resolved once per class.
struct MenuFlags {
    ue3::BoolRef fullscreen;  // bFullscreenMovie (pause, ...)
    ue3::BoolRef pauses;      // bPauseGameWhileActive
    bool listed = false;      // one of kMenuClasses (or derived from one)
};
std::unordered_map<const ue3::UClass*, MenuFlags> g_menu_flags;

const MenuFlags& menu_flags(const ue3::UObject* movie) {
    const ue3::UClass* cls = ue3::class_of(movie);
    auto it = g_menu_flags.find(cls);
    if (it == g_menu_flags.end()) {
        MenuFlags flags{ue3::find_bool(cls, "bFullscreenMovie"),
                        ue3::find_bool(cls, "bPauseGameWhileActive")};
        for (const char* name : kMenuClasses) {
            flags.listed = flags.listed || ue3::is_a(movie, name);
        }
        it = g_menu_flags.emplace(cls, flags).first;
    }
    return it->second;
}

// An open movie that is a menu (shown on the theater screen), not part of the HUD.
bool is_menu(const ue3::UObject* movie) {
    const MenuFlags& flags = menu_flags(movie);
    return flags.listed || flags.fullscreen.get(movie) || flags.pauses.get(movie);
}

void resolve(const ue3::UObject* movie) {
    g_resolved = true;
    g_movie_is_open = ue3::find_bool(ue3::class_of(movie), "bMovieIsOpen");
    g_set_viewport = ue3::find_function(movie, "SetViewport");
    if (g_set_viewport != nullptr) {
        g_arg_x = ue3::property_offset(g_set_viewport, "X");
        g_arg_y = ue3::property_offset(g_set_viewport, "Y");
        g_arg_width = ue3::property_offset(g_set_viewport, "Width");
        g_arg_height = ue3::property_offset(g_set_viewport, "Height");
        g_args_size = ue3::field<uint16_t>(g_set_viewport, ue3::off::function_params_size);
    }
    LOG("GFx: bMovieIsOpen@%d SetViewport=%p (%d,%d,%d,%d)", g_movie_is_open.offset,
        (void*)g_set_viewport, g_arg_x, g_arg_y, g_arg_width, g_arg_height);
}

ue3::UClass* g_movie_class = nullptr;
ue3::UClass* g_hud_class = nullptr;

bool is_hud(const ue3::UObject* movie) {
    return g_hud_class != nullptr && ue3::is_a(movie, g_hud_class);
}

void set_viewport(ue3::UObject* movie, int x, int y, int width, int height) {
    if (g_set_viewport == nullptr || g_arg_x < 0 || g_arg_y < 0 || g_arg_width < 0 ||
        g_arg_height < 0) {
        return;
    }
    uint8_t args[64] = {};
    if (g_args_size > sizeof(args)) {
        return;
    }
    ue3::field<int32_t>(args, g_arg_x) = x;
    ue3::field<int32_t>(args, g_arg_y) = y;
    ue3::field<int32_t>(args, g_arg_width) = width;
    ue3::field<int32_t>(args, g_arg_height) = height;
    ue3::call_function(movie, g_set_viewport, args);
}

bool g_wrist = false;  // the HUD belongs on the left wrist

}  // namespace

void hud_movie_started(ue3::UObject* object) {
    if (g_movie_class != nullptr && ue3::is_a(object, g_movie_class) &&
        std::find(g_movies.begin(), g_movies.end(), object) == g_movies.end()) {
        g_movies.push_back(object);
        if (!g_resolved) {
            resolve(object);
        }
    }
}

void hud_update() {
    // Drop movies that were destroyed since the last scan.
    std::erase_if(g_movies, [](ue3::UObject* m) { return !ue3::is_live(m); });
    const DWORD now = GetTickCount();
    if (now - g_last_scan < 2000) {
        return;
    }
    g_last_scan = now;
    if (g_movie_class == nullptr) {
        g_movie_class = ue3::find_class("GFxMoviePlayer");
        g_hud_class = ue3::find_class("WillowHUDGFxMovie");
        if (g_movie_class == nullptr) {
            return;
        }
    }
    g_movies.clear();
    for (int32_t i = 0; i < ue3::object_count(); ++i) {
        ue3::UObject* obj = ue3::object_at(i);
        if (obj != nullptr && ue3::is_a(obj, g_movie_class) && !ue3::is_default_object(obj)) {
            g_movies.push_back(obj);
        }
    }
    if (!g_resolved && !g_movies.empty()) {
        resolve(g_movies.front());
    }
}

bool hud_menu_open() {
    for (ue3::UObject* movie : g_movies) {
        if (is_hud(movie) || !g_movie_is_open.get(movie)) {
            continue;
        }
        if (is_menu(movie)) {
            const std::string name = ue3::object_name(ue3::class_of(movie));
            if (name != g_menu_name) {
                g_menu_name = name;
                LOG("Menu open: %s", name.c_str());
            }
            return true;
        }
    }
    if (!g_menu_name.empty()) {
        LOG("Menu closed: %s", g_menu_name.c_str());
        g_menu_name.clear();
    }
    return false;
}

void hud_layout(int eye, unsigned width, unsigned height, float tan_half_x) {
    if (width == 0 || height == 0) {
        return;
    }
    const float scale = eye < 0 ? 1.0f : config().hud_scale;
    const int hud_width = static_cast<int>(width * scale);
    const int hud_height = static_cast<int>(height * scale);
    int x = (static_cast<int>(width) - hud_width) / 2;
    const int y = (static_cast<int>(height) - hud_height) / 2;
    if (eye >= 0 && config().hud_distance > 0.0f) {
        // Parallax of a point straight ahead at hud_distance for an eye 32 mm off centre.
        const float shift = 0.032f / config().hud_distance / tan_half_x * (width * 0.5f);
        x += static_cast<int>(std::lround(eye == 0 ? shift : -shift));
    }
    // Menus are authored for 16:9 and crop their sides in the near-square VR frame;
    // letterbox them into a 16:9 band so everything stays visible.
    const int band = std::min(static_cast<int>(height), static_cast<int>(width) * 9 / 16);
    for (ue3::UObject* movie : g_movies) {
        if (!g_movie_is_open.get(movie)) {
            continue;
        }
        if (is_hud(movie)) {
            if (g_wrist && eye >= 0) {
                // A 16:9 viewport no other movie uses: the render thread recognises it and
                // sends the HUD into the wrist texture (see wrist.cpp).
                const int wrist_width = static_cast<int>(width) - 4;
                const int wrist_height = std::min(static_cast<int>(height) - 4, wrist_width * 9 / 16);
                const int wrist_y = (static_cast<int>(height) - wrist_height) / 2 + 1;
                set_viewport(movie, 2, wrist_y, wrist_width, wrist_height);
                wrist_set_hud_rect(2, wrist_y, wrist_width, wrist_height);
            } else {
                set_viewport(movie, x, y, hud_width, hud_height);
            }
        } else if (is_menu(movie)) {
            set_viewport(movie, 0, (static_cast<int>(height) - band) / 2, static_cast<int>(width),
                         band);
        }
    }
}

bool hud_set_wrist(bool on) {
    bool hud_open = false;
    for (ue3::UObject* movie : g_movies) {
        hud_open = hud_open || (is_hud(movie) && g_movie_is_open.get(movie));
    }
    if (on != g_wrist) {
        g_wrist = on;
        LOG("HUD %s", on ? "on the left wrist" : "on the screen");
    }
    if (!on) {
        wrist_set_hud_rect(0, 0, 0, 0);
    }
    return on && hud_open;
}

}  // namespace bl2vr
