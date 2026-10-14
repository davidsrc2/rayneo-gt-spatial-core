#pragma once

#include <cstddef>
#include <string_view>
#include <vector>

#include "rayneo/core/render_math.h"

namespace rayneo::core {

// One-click layouts and usage profiles, shared by every app (macOS, Windows) so they offer
// the same choices. Titles are UI strings and stay in each app, keyed by id.
// Ids are string literals (NUL-terminated), so the C ABI can hand them out as-is.
struct LayoutPreset {
    std::string_view id;
    std::vector<GridCell> cells;  // normalized: top-left cell is (0, 0)
    float curvature = 0.0f;       // 0 flat .. 1 cylinder
    float arc_deg = 38.0f;        // panel width
    float gap_deg = 1.0f;
};

struct Profile {
    std::string_view id;
    std::string_view layout_id;
    float distance_m = 2.0f;
    int text_size = 1;           // index into the app's text-size steps
    float curvature = -1.0f;     // < 0: keep the layout's
    float arc_deg = -1.0f;       // < 0: keep the layout's
};

const std::vector<LayoutPreset>& layout_presets();
const std::vector<Profile>& profiles();

// Shifts cells so the top-left of the arrangement is (0, 0).
std::vector<GridCell> normalized_cells(const std::vector<GridCell>& cells);

// Preset whose cells match `cells` (after normalizing), or nullptr.
const LayoutPreset* find_layout_preset(const std::vector<GridCell>& cells);
const LayoutPreset* layout_preset(std::string_view id);

}  // namespace rayneo::core
