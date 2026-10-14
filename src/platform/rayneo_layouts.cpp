#include "rayneo/platform/rayneo_layouts.h"

#include <string_view>

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

}  // extern "C"
