#include "rayneo/core/presets.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

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

bool valid_arrangement(const std::vector<GridCell>& cells) {
    if (cells.empty()) return true;
    if (static_cast<int>(cells.size()) > kMaxMonitors) return false;
    int c0 = cells[0].col, c1 = c0, r0 = cells[0].row, r1 = r0;
    for (const auto& c : cells) {
        c0 = std::min(c0, c.col);
        c1 = std::max(c1, c.col);
        r0 = std::min(r0, c.row);
        r1 = std::max(r1, c.row);
    }
    if (c1 - c0 + 1 > kMaxGridColumns || r1 - r0 + 1 > kMaxGridRows) return false;
    auto same = [](const GridCell& a, const GridCell& b) { return a.col == b.col && a.row == b.row; };
    for (size_t i = 0; i < cells.size(); ++i) {
        for (size_t j = i + 1; j < cells.size(); ++j) {
            if (same(cells[i], cells[j])) return false;
        }
    }
    // Connected: every cell reached from the first through edge neighbours.
    std::vector<bool> seen(cells.size(), false);
    std::vector<size_t> stack{0};
    seen[0] = true;
    size_t reached = 1;
    while (!stack.empty()) {
        const GridCell c = cells[stack.back()];
        stack.pop_back();
        for (size_t j = 0; j < cells.size(); ++j) {
            const int d = std::abs(cells[j].col - c.col) + std::abs(cells[j].row - c.row);
            if (!seen[j] && d == 1) {
                seen[j] = true;
                ++reached;
                stack.push_back(j);
            }
        }
    }
    return reached == cells.size();
}

bool next_free_cell(const std::vector<GridCell>& cells, GridCell* out) {
    if (cells.empty()) {
        *out = {0, 0};
        return true;
    }
    std::vector<int> rows, cols;
    for (const auto& c : cells) {
        rows.push_back(c.row);
        cols.push_back(c.col);
    }
    std::sort(rows.begin(), rows.end());
    rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
    std::sort(cols.begin(), cols.end());
    cols.erase(std::unique(cols.begin(), cols.end()), cols.end());
    std::vector<GridCell> candidates;
    for (int r : rows) {
        int right = cols.front();
        for (const auto& c : cells) {
            if (c.row == r) right = std::max(right, c.col);
        }
        candidates.push_back({right + 1, r});
    }
    for (int col : cols) {
        int below = rows.front();
        for (const auto& c : cells) {
            if (c.col == col) below = std::max(below, c.row);
        }
        candidates.push_back({col, below + 1});
    }
    for (const auto& p : candidates) {
        std::vector<GridCell> next = cells;
        next.push_back(p);
        if (valid_arrangement(next)) {
            *out = p;
            return true;
        }
    }
    return false;
}

bool move_cell(std::vector<GridCell>& cells, size_t index, GridCell target) {
    if (index >= cells.size()) return false;
    std::vector<GridCell> next = cells;
    for (auto& c : next) {
        if (c.col == target.col && c.row == target.row) c = cells[index];  // swap
    }
    next[index] = target;
    if (!valid_arrangement(next)) return false;
    cells = std::move(next);
    return true;
}

namespace {
float pose_vertical_deg(float arc_deg, float aspect) {
    constexpr float kHalfDegToRad = 0.00872664626f;
    return 2.0f * std::atan(std::tan(arc_deg * kHalfDegToRad) / aspect) / (2.0f * kHalfDegToRad);
}
}  // namespace

bool pose_overlaps(const std::vector<PanelPose>& poses, size_t index, float arc_deg, float aspect, float tolerance_deg) {
    if (index >= poses.size()) return false;
    const PanelPose& p = poses[index];
    const float hw = arc_deg * p.scale / 2, hh = pose_vertical_deg(arc_deg * p.scale, aspect) / 2;
    for (size_t j = 0; j < poses.size(); ++j) {
        if (j == index) continue;
        const PanelPose& o = poses[j];
        const float ow = arc_deg * o.scale / 2, oh = pose_vertical_deg(arc_deg * o.scale, aspect) / 2;
        const float dx = (hw + ow) - std::fabs(p.yaw_deg - o.yaw_deg);
        const float dy = (hh + oh) - std::fabs(p.pitch_deg - o.pitch_deg);
        if (dx > tolerance_deg && dy > tolerance_deg) return true;
    }
    return false;
}

PanelPose pose_beside(const std::vector<PanelPose>& poses, float arc_deg, float aspect, float gap_deg) {
    if (poses.empty()) return {};
    // The row nearest the horizon: the panels within half a panel's height of the lowest |pitch|.
    float row = poses[0].pitch_deg;
    for (const auto& p : poses) {
        if (std::fabs(p.pitch_deg) < std::fabs(row)) row = p.pitch_deg;
    }
    const float half_h = pose_vertical_deg(arc_deg, aspect) / 2;
    const PanelPose* right = nullptr;
    for (const auto& p : poses) {
        if (std::fabs(p.pitch_deg - row) > half_h) continue;
        if (right == nullptr || p.yaw_deg + arc_deg * p.scale / 2 > right->yaw_deg + arc_deg * right->scale / 2) right = &p;
    }
    PanelPose out{right->yaw_deg + arc_deg * right->scale / 2 + gap_deg + arc_deg / 2, right->pitch_deg, 1.0f};
    // Clear of any panel already there (one placed by hand beyond the row): further right.
    std::vector<PanelPose> all = poses;
    all.push_back(out);
    for (int k = 0; k < kMaxMonitors * 2 && pose_overlaps(all, all.size() - 1, arc_deg, aspect, 0.0f); ++k) {
        all.back().yaw_deg += arc_deg + gap_deg;
    }
    return all.back();
}

}  // namespace rayneo::core
