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

// --- Editing an arrangement (both apps' layout editors) -------------------------------------

// The most monitors a grid holds and its extent, as both apps allow.
inline constexpr int kMaxMonitors = 6;
inline constexpr int kMaxGridColumns = 4;
inline constexpr int kMaxGridRows = 3;

// Every cell unique, within kMaxGridColumns x kMaxGridRows, and every monitor reaching the
// others edge to edge (the OS arrangement needs it, and so does the wall).
bool valid_arrangement(const std::vector<GridCell>& cells);

// Where an added monitor goes: right of a row first (top row first), then below a column, so it
// joins the block edge to edge. False when the arrangement is full.
bool next_free_cell(const std::vector<GridCell>& cells, GridCell* out);

// Monitor `index` moved to `target`, swapping with the one already there. False, and `cells`
// untouched, when the result is not a valid arrangement.
bool move_cell(std::vector<GridCell>& cells, size_t index, GridCell target);

// Hand placement (PanelPose degrees; a panel `arc_deg` wide at scale 1, `aspect` width / height).
// Whether panel `index` overlaps any other by more than `tolerance_deg` on both axes.
bool pose_overlaps(const std::vector<PanelPose>& poses, size_t index, float arc_deg, float aspect,
                   float tolerance_deg = 0.25f);
// A monitor added to hand-placed ones: right of the rightmost panel of the row nearest the
// horizon, `gap_deg` apart, its centre at that panel's height. Straight ahead when none.
PanelPose pose_beside(const std::vector<PanelPose>& poses, float arc_deg, float aspect, float gap_deg);

}  // namespace rayneo::core
