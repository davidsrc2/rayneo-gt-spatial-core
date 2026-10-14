#include "rayneo/core/presets.h"

#include <algorithm>

namespace rayneo::core {

const std::vector<LayoutPreset>& layout_presets() {
    static const std::vector<LayoutPreset> presets = {
        {"curve", {{0, 0}}, 0.7f, 46.0f, 1.0f},
        {"dual", {{0, 0}, {1, 0}}, 0.0f, 38.0f, 1.0f},
        {"triple", {{0, 0}, {1, 0}, {2, 0}}, 0.35f, 36.0f, 0.0f},
    };
    return presets;
}

const std::vector<Profile>& profiles() {
    static const std::vector<Profile> list = {
        {"code", "dual", 1.6f, 2, -1.0f, -1.0f},
        {"cinema", "curve", 2.6f, 0, 0.6f, 52.0f},
    };
    return list;
}

std::vector<GridCell> normalized_cells(const std::vector<GridCell>& cells) {
    if (cells.empty()) return {};
    int c0 = cells[0].col, r0 = cells[0].row;
    for (const auto& c : cells) {
        c0 = std::min(c0, c.col);
        r0 = std::min(r0, c.row);
    }
    std::vector<GridCell> out;
    out.reserve(cells.size());
    for (const auto& c : cells) out.push_back({c.col - c0, c.row - r0});
    return out;
}

const LayoutPreset* find_layout_preset(const std::vector<GridCell>& cells) {
    const auto n = normalized_cells(cells);
    for (const auto& p : layout_presets()) {
        if (p.cells.size() != n.size()) continue;
        if (std::equal(n.begin(), n.end(), p.cells.begin(),
                       [](const GridCell& a, const GridCell& b) { return a.col == b.col && a.row == b.row; })) {
            return &p;
        }
    }
    return nullptr;
}

const LayoutPreset* layout_preset(std::string_view id) {
    for (const auto& p : layout_presets()) {
        if (p.id == id) return &p;
    }
    return nullptr;
}

}  // namespace rayneo::core
