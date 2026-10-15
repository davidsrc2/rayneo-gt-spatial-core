#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "rayneo/core/render_math.h"

namespace rayneo::core {

// Every user-facing setting of the Nimbo apps, shared by macOS and Windows so both keep the
// same settings file (settings.json). Keys match the macOS app's UserDefaults keys.
struct Settings {
    std::vector<GridCell> cells = {{0, 0}, {1, 0}};  // "layoutCells": one cell per virtual monitor
    int text_size = 3;           // "textSize": index into the app's text-size steps (0 = small text)
    float distance_m = 2.0f;     // "distance"
    float gap_deg = 1.0f;        // "gap"
    float curvature = 0.0f;      // "curvature": 0 flat .. 1 cylinder
    float size_deg = 38.0f;      // "size": panel width
    bool auto_start = false;     // "autoStart": start when the glasses connect

    float fov_y_deg = 24.5f;     // "fovY"
    float ipd_mm = 63.0f;        // "ipd"
    bool stereo = false;         // "stereo": half-SBS (glasses in 3D mode)
    float dead_zone_deg = 1.0f;  // "deadZone"
    bool gaze_cursor = true;     // "gazeCursor"
    bool dim_others = false;     // "dimOthers" (off: on see-through optics a dimmed panel looks cut off)
    bool gather_windows = true;  // "gatherWindows"
    int tracking = 1;            // "tracking": head rotations that move the panels, TrackingAxes
                                 // (0 both, 1 left/right only, 2 up/down only)
    float k1 = 0.0f;             // "k1"
    float k2 = 0.0f;             // "k2"
    bool onboarding_done = false;  // "onboardingDone"
};

std::string settings_to_json(const Settings& s);

// Missing or malformed keys keep their defaults; unknown keys are ignored. nullopt only when
// the text is not a JSON object at all.
std::optional<Settings> settings_from_json(std::string_view json);

}  // namespace rayneo::core
