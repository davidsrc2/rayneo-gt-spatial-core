#include "rayneo/platform/rayneo_layouts.h"

#include <algorithm>
#include <string_view>
#include <vector>

#include "rayneo/core/presets.h"

namespace core = rayneo::core;

// Preset ids are string literals (core/presets.cpp): data() is NUL-terminated and static.
static const char* c_str(std::string_view s) { return s.data(); }

extern "C" {

int rayneo_layout_preset_count(void) { return static_cast<int>(core::layout_presets().size()); }

bool rayneo_layout_preset_at(int index, rayneo_layout_preset* out) {
    const auto& all = core::layout_presets();
    if (out == nullptr || index < 0 || index >= static_cast<int>(all.size())) return false;
    const auto& p = all[static_cast<size_t>(index)];
    *out = {};
    out->id = c_str(p.id);
    for (const auto& c : p.cells) {
        if (out->cell_count >= RAYNEO_MAX_PRESET_CELLS) break;
        out->cols[out->cell_count] = c.col;
        out->rows[out->cell_count] = c.row;
        ++out->cell_count;
    }
    out->curvature = p.curvature;
    out->arc_deg = p.arc_deg;
    out->gap_deg = p.gap_deg;
    return true;
}

int rayneo_profile_count(void) { return static_cast<int>(core::profiles().size()); }

bool rayneo_profile_at(int index, rayneo_profile* out) {
    const auto& all = core::profiles();
    if (out == nullptr || index < 0 || index >= static_cast<int>(all.size())) return false;
    const auto& p = all[static_cast<size_t>(index)];
    *out = {c_str(p.id), c_str(p.layout_id), p.distance_m, p.text_size, p.curvature, p.arc_deg};
    return true;
}

static std::vector<core::GridCell> cells_of(const int* cols, const int* rows, int count) {
    std::vector<core::GridCell> cells;
    for (int i = 0; i < std::min(count, RAYNEO_MAX_PRESET_CELLS); ++i) cells.push_back({cols[i], rows[i]});
    return cells;
}

static std::vector<core::PanelPose> poses_of(const float* yaw, const float* pitch, const float* scale, int count) {
    std::vector<core::PanelPose> poses;
    for (int i = 0; i < std::min(count, RAYNEO_MAX_PRESET_CELLS); ++i) poses.push_back({yaw[i], pitch[i], scale[i]});
    return poses;
}

bool rayneo_next_free_cell(const int* cols, const int* rows, int count, int* col, int* row) {
    core::GridCell c{};
    if (!core::next_free_cell(cells_of(cols, rows, count), &c)) return false;
    *col = c.col;
    *row = c.row;
    return true;
}

bool rayneo_move_cell(int* cols, int* rows, int count, int index, int col, int row) {
    auto cells = cells_of(cols, rows, count);
    if (index < 0 || !core::move_cell(cells, static_cast<size_t>(index), {col, row})) return false;
    for (size_t i = 0; i < cells.size(); ++i) {
        cols[i] = cells[i].col;
        rows[i] = cells[i].row;
    }
    return true;
}

bool rayneo_pose_overlaps(const float* yaw, const float* pitch, const float* scale, int count, int index, float arc_deg,
                          float aspect) {
    return index >= 0 && core::pose_overlaps(poses_of(yaw, pitch, scale, count), static_cast<size_t>(index), arc_deg, aspect);
}

void rayneo_pose_beside(const float* yaw, const float* pitch, const float* scale, int count, float arc_deg, float aspect,
                        float gap_deg, float* out_yaw, float* out_pitch) {
    const core::PanelPose p = core::pose_beside(poses_of(yaw, pitch, scale, count), arc_deg, aspect, gap_deg);
    *out_yaw = p.yaw_deg;
    *out_pitch = p.pitch_deg;
}

}  // extern "C"
