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
    std::vector<PanelPose> panels;  // "panels": hand-placed position and size per monitor; empty = the grid
    int text_size = 3;           // "textSize": index into the app's text-size steps (0 = small text)
    float distance_m = 1.2f;     // "distance": the wall's plane (flat wall: no gap, no tilt, no curvature)
    float gap_deg = 0.0f;        // "gap": edge to edge
    float curvature = 0.0f;      // "curvature": 0 flat .. 1 cylinder
    float size_deg = 38.0f;      // "size": panel width
    bool auto_start = false;     // "autoStart": start when the glasses connect

    float fov_y_deg = 24.5f;     // "fovY"
    float dead_zone_deg = 1.0f;  // "deadZone"
    bool gaze_cursor = true;     // "gazeCursor"
    bool dim_others = false;     // "dimOthers" (off: on see-through optics a dimmed panel looks cut off)
    bool gather_windows = true;  // "gatherWindows"
    int tracking = 1;            // "tracking": head rotations that move the panels, TrackingAxes
                                 // (0 both, 1 left/right only, 2 up/down only)
    int glasses_mode = 0;        // "glassesMode": the glasses' own mode set by Nimbo: 0 follow, 1 pinned
                                 // (they anchor the image), 2 steady (reads the earlier "glassesPinned")
    int brightness = 0;          // "brightness": glasses' brightness 1..8 set by Nimbo; 0 = leave as is
    float tilt = 0.0f;           // "tilt": panels above / below face you (1) .. stand upright (0)
    bool anchor_at_rest = false;  // "anchorAtRest": panels frozen while the head holds still
    bool mac_screen = true;      // "macScreen": the computer's own screen is panel 1 (laptop lid open)
    float zoom = 1.0f;           // "zoom": every panel this much larger (shortcuts), as left last time
    bool grow_by_gaze = true;    // "growByGaze": glasses' pinned mode, the panel looked at takes over (else mouse only)
    int background = 0;          // "background": around the panels, 0 none, 1 rain, 2 stars, 3 sunset, 4 aurora, 5 picture
    std::string background_image;  // "backgroundImage": style 5's image file (UTF-8)
    float background_brightness = 0.6f;  // "backgroundBrightness": 0..1
    float k1 = 0.0f;             // "k1"
    float k2 = 0.0f;             // "k2"
    bool onboarding_done = false;  // "onboardingDone"
    bool vehicle_mode = false;   // "vehicleMode": a quarter of the heading followed (core::kVehicleHeadingScale)
    bool flat_wall = true;       // "flatWall": every panel in one plane, edge to edge (core::wall_grid_layout)
};

std::string settings_to_json(const Settings& s);

// Missing or malformed keys keep their defaults; unknown keys are ignored. nullopt only when
// the text is not a JSON object at all.
std::optional<Settings> settings_from_json(std::string_view json);

}  // namespace rayneo::core
