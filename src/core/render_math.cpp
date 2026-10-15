#include "rayneo/core/render_math.h"

#include <algorithm>
#include <cmath>

namespace rayneo::core {
namespace {

Vec3 render_to_core(const Vec3& r) { return {r.z, -r.x, r.y}; }

}  // namespace

Mat4 Mat4::operator*(const Mat4& b) const {
    Mat4 out;
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            float s = 0.0f;
            for (int k = 0; k < 4; ++k) s += at(r, k) * b.at(k, c);
            out.at(r, c) = s;
        }
    }
    return out;
}

Vec3 Mat4::transform_point(const Vec3& p) const {
    return {at(0, 0) * p.x + at(0, 1) * p.y + at(0, 2) * p.z + at(0, 3),
            at(1, 0) * p.x + at(1, 1) * p.y + at(1, 2) * p.z + at(1, 3),
            at(2, 0) * p.x + at(2, 1) * p.y + at(2, 2) * p.z + at(2, 3)};
}

Vec3 core_to_render(const Vec3& v) { return {-v.y, v.z, v.x}; }

Mat4 render_rotation(const Quat& head_rel) {
    const Quat q = head_rel.normalized();
    const Vec3 basis[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    Mat4 m;
    for (int c = 0; c < 3; ++c) {
        // Column c = image of render basis vector c.
        const Vec3 col = core_to_render(q.rotate(render_to_core(basis[c])));
        m.at(0, c) = col.x;
        m.at(1, c) = col.y;
        m.at(2, c) = col.z;
    }
    return m;
}

Mat4 view_matrix(const Quat& head_rel, float eye_offset_m) {
    // Head at the origin: v_eye = R^T * v_world - eye_offset.
    const Mat4 r = render_rotation(head_rel);
    Mat4 v;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) v.at(i, j) = r.at(j, i);
    }
    v.at(0, 3) = -eye_offset_m;
    return v;
}

Mat4 perspective_lh(float fovy_rad, float aspect, float near_z, float far_z) {
    const float ys = 1.0f / std::tan(fovy_rad * 0.5f);
    const float xs = ys / aspect;
    const float zs = far_z / (far_z - near_z);
    Mat4 p;
    p.at(0, 0) = xs;
    p.at(1, 1) = ys;
    p.at(2, 2) = zs;
    p.at(2, 3) = -near_z * zs;
    p.at(3, 2) = 1.0f;
    p.at(3, 3) = 0.0f;
    return p;
}

Quat yaw_only(const Quat& q) {
    // Heading of the forward axis on the horizon. Looking straight up or down the heading is
    // undefined: the up axis' horizontal direction (where the top of the head points) stands in.
    Vec3 f = q.rotate({1.0f, 0.0f, 0.0f});
    if (f.x * f.x + f.y * f.y < 1e-6f) {
        const Vec3 u = q.rotate({0.0f, 0.0f, 1.0f});
        f = {-u.x * f.z, -u.y * f.z, 0.0f};
        if (f.x * f.x + f.y * f.y < 1e-12f) return Quat{};
    }
    const float half = 0.5f * std::atan2(f.y, f.x);
    return Quat{std::cos(half), 0.0f, 0.0f, std::sin(half)};
}

Quat pitch_only(const Quat& q) {
    const Vec3 f = q.rotate({1.0f, 0.0f, 0.0f});
    const float elevation = std::asin(std::fmax(-1.0f, std::fmin(1.0f, f.z)));
    return Quat::from_axis_angle({0.0f, -1.0f, 0.0f}, elevation);  // looking up = about -Y
}

Quat track_axes(const Quat& q, TrackingAxes axes) {
    switch (axes) {
        case TrackingAxes::Horizontal: return yaw_only(q);
        case TrackingAxes::Vertical: return pitch_only(q);
        case TrackingAxes::Both: break;
    }
    return q;
}

Quat predict_orientation(const Quat& q, const Vec3& omega_rads, float dt_s) {
    const float angle = omega_rads.norm() * dt_s;
    if (angle < 1e-9f) return q;
    return (Quat::from_axis_angle(omega_rads, angle) * q).normalized();
}

ScreenMesh curved_screen_mesh(float radius_m, float arc_rad, float height_m, int segments, float center_rad,
                              float center_y_m) {
    ScreenMesh mesh;
    if (segments < 1 || arc_rad <= 0.0f) return mesh;
    mesh.vertices.reserve(static_cast<size_t>(segments + 1) * 2);
    const float half_h = height_m * 0.5f;
    for (int i = 0; i <= segments; ++i) {
        const float u = static_cast<float>(i) / static_cast<float>(segments);
        const float theta = center_rad + (u - 0.5f) * arc_rad;  // negative = left
        const float x = radius_m * std::sin(theta);
        const float z = radius_m * std::cos(theta);
        mesh.vertices.push_back({x, center_y_m + half_h, z, u, 0.0f});
        mesh.vertices.push_back({x, center_y_m - half_h, z, u, 1.0f});
    }
    for (int i = 0; i < segments; ++i) {
        const auto top = static_cast<uint16_t>(i * 2);
        const auto bottom = static_cast<uint16_t>(top + 1);
        const auto next_top = static_cast<uint16_t>(top + 2);
        const auto next_bottom = static_cast<uint16_t>(top + 3);
        mesh.indices.insert(mesh.indices.end(), {top, next_top, bottom, bottom, next_top, next_bottom});
    }
    return mesh;
}

ScreenMesh flat_screen_mesh(float radius_m, float arc_rad, float height_m, int segments, float center_rad,
                            float center_y_m) {
    ScreenMesh mesh;
    if (segments < 1 || arc_rad <= 0.0f || arc_rad >= 3.0f) return mesh;
    mesh.vertices.reserve(static_cast<size_t>(segments + 1) * 2);
    const float half_w = radius_m * std::tan(arc_rad * 0.5f);
    const float half_h = height_m * 0.5f;
    const float dx = std::sin(center_rad), dz = std::cos(center_rad);  // towards the panel
    for (int i = 0; i <= segments; ++i) {
        const float u = static_cast<float>(i) / static_cast<float>(segments);
        const float t = (u - 0.5f) * 2.0f * half_w;  // along the panel, left -> right
        const float x = radius_m * dx + t * dz;
        const float z = radius_m * dz - t * dx;
        mesh.vertices.push_back({x, center_y_m + half_h, z, u, 0.0f});
        mesh.vertices.push_back({x, center_y_m - half_h, z, u, 1.0f});
    }
    for (int i = 0; i < segments; ++i) {
        const auto top = static_cast<uint16_t>(i * 2);
        const auto bottom = static_cast<uint16_t>(top + 1);
        const auto next_top = static_cast<uint16_t>(top + 2);
        const auto next_bottom = static_cast<uint16_t>(top + 3);
        mesh.indices.insert(mesh.indices.end(), {top, next_top, bottom, bottom, next_top, next_bottom});
    }
    return mesh;
}

ScreenMesh bent_screen_mesh(float radius_m, float arc_rad, float height_m, int segments, float curvature,
                            float center_rad, float center_y_m) {
    if (curvature <= 1e-4f) return flat_screen_mesh(radius_m, arc_rad, height_m, segments, center_rad, center_y_m);
    ScreenMesh mesh;
    if (segments < 1 || arc_rad <= 0.0f || arc_rad >= 3.0f) return mesh;
    const float k = std::fmin(curvature, 1.0f);
    const float width = 2.0f * radius_m * std::tan(arc_rad * 0.5f);
    const float bend = radius_m / k;  // bending radius; its axis sits at distance r - bend
    const float half_h = height_m * 0.5f;
    const float cs = std::cos(center_rad), sn = std::sin(center_rad);
    mesh.vertices.reserve(static_cast<size_t>(segments + 1) * 2);
    for (int i = 0; i <= segments; ++i) {
        const float u = static_cast<float>(i) / static_cast<float>(segments);
        const float phi = (u - 0.5f) * width / bend;  // arc length along the sheet / bend radius
        const float lx = bend * std::sin(phi);
        const float lz = (radius_m - bend) + bend * std::cos(phi);
        // Rotate the straight-ahead panel to its centre direction (yaw, render Y axis).
        const float x = lx * cs + lz * sn;
        const float z = -lx * sn + lz * cs;
        mesh.vertices.push_back({x, center_y_m + half_h, z, u, 0.0f});
        mesh.vertices.push_back({x, center_y_m - half_h, z, u, 1.0f});
    }
    for (int i = 0; i < segments; ++i) {
        const auto top = static_cast<uint16_t>(i * 2);
        const auto bottom = static_cast<uint16_t>(top + 1);
        const auto next_top = static_cast<uint16_t>(top + 2);
        const auto next_bottom = static_cast<uint16_t>(top + 3);
        mesh.indices.insert(mesh.indices.end(), {top, next_top, bottom, bottom, next_top, next_bottom});
    }
    return mesh;
}

float panel_width(float radius_m, float arc_rad, bool flat) {
    return flat ? 2.0f * radius_m * std::tan(arc_rad * 0.5f) : radius_m * arc_rad;
}

std::vector<float> panel_centers(int count, float panel_arc_rad, float gap_rad) {
    std::vector<float> out;
    if (count <= 0) return out;
    out.reserve(static_cast<size_t>(count));
    const float step = panel_arc_rad + gap_rad;
    for (int i = 0; i < count; ++i) out.push_back((static_cast<float>(i) - 0.5f * static_cast<float>(count - 1)) * step);
    return out;
}

int panel_at_gaze(const Quat& head_rel, const std::vector<float>& centers, float panel_arc_rad, float gap_rad) {
    if (centers.empty()) return -1;
    const Vec3 fwd = render_rotation(head_rel).transform_point({0.0f, 0.0f, 1.0f});
    const float yaw = std::atan2(fwd.x, fwd.z);  // render: positive = right
    int best = 0;
    for (int i = 1; i < static_cast<int>(centers.size()); ++i) {
        if (std::fabs(yaw - centers[static_cast<size_t>(i)]) < std::fabs(yaw - centers[static_cast<size_t>(best)])) best = i;
    }
    const float half_step = 0.5f * (panel_arc_rad + gap_rad);
    return std::fabs(yaw - centers[static_cast<size_t>(best)]) <= half_step ? best : -1;
}

float panel_vertical_arc(float arc_rad, float aspect, bool flat) {
    if (aspect <= 0.0f) return 0.0f;
    // Flat: half-height r tan(arc/2) / aspect at distance r. Curved: height r arc / aspect.
    return flat ? 2.0f * std::atan(std::tan(arc_rad * 0.5f) / aspect) : 2.0f * std::atan(arc_rad / (2.0f * aspect));
}

std::vector<PanelPlacement> grid_layout(const std::vector<GridCell>& cells, float arc_rad, float vertical_arc_rad,
                                        float gap_rad) {
    std::vector<PanelPlacement> out;
    if (cells.empty()) return out;
    int min_c = cells[0].col, max_c = cells[0].col, min_r = cells[0].row, max_r = cells[0].row;
    for (const auto& c : cells) {
        min_c = std::min(min_c, c.col);
        max_c = std::max(max_c, c.col);
        min_r = std::min(min_r, c.row);
        max_r = std::max(max_r, c.row);
    }
    const float mid_c = 0.5f * static_cast<float>(min_c + max_c);
    const float mid_r = 0.5f * static_cast<float>(min_r + max_r);
    out.reserve(cells.size());
    for (const auto& c : cells) {
        out.push_back({(static_cast<float>(c.col) - mid_c) * (arc_rad + gap_rad),
                       (mid_r - static_cast<float>(c.row)) * (vertical_arc_rad + gap_rad)});
    }
    return out;
}

void pitch_mesh(ScreenMesh& mesh, float pitch_rad) {
    const float cp = std::cos(pitch_rad), sp = std::sin(pitch_rad);
    for (auto& v : mesh.vertices) {
        const float y = v.py * cp + v.pz * sp;
        const float z = -v.py * sp + v.pz * cp;
        v.py = y;
        v.pz = z;
    }
}

int panel_at_gaze(const Quat& head_rel, const std::vector<PanelPlacement>& panels, float arc_rad,
                  float vertical_arc_rad, float gap_rad) {
    const Vec3 f = render_rotation(head_rel).transform_point({0.0f, 0.0f, 1.0f});
    const float half_w = 0.5f * (arc_rad + gap_rad), half_h = 0.5f * (vertical_arc_rad + gap_rad);
    for (size_t i = 0; i < panels.size(); ++i) {
        // Undo the panel's pitch, then compare angles in its row.
        const float cp = std::cos(panels[i].pitch_rad), sp = std::sin(panels[i].pitch_rad);
        const float y = f.y * cp - f.z * sp;
        const float z = f.y * sp + f.z * cp;
        const float yaw = std::atan2(f.x, z) - panels[i].yaw_rad;
        const float pitch = std::atan2(y, std::sqrt(f.x * f.x + z * z));
        if (std::fabs(yaw) <= half_w && std::fabs(pitch) <= half_h) return static_cast<int>(i);
    }
    return -1;
}

float horizontal_fov(float fovy_rad, float aspect) {
    return 2.0f * std::atan(std::tan(fovy_rad * 0.5f) * aspect);
}

float fit_panel_arc(float fov_rad, int count, float gap_rad, float margin_rad) {
    if (count <= 0) return 0.0f;
    const float usable = fov_rad - 2.0f * margin_rad - static_cast<float>(count - 1) * gap_rad;
    return usable > 0.0f ? usable / static_cast<float>(count) : 0.0f;
}

float arc_for_pixels(float pixels_wide, float pixels_per_degree) {
    return pixels_per_degree > 0.0f ? (pixels_wide / pixels_per_degree) * 0.017453292519943295f : 0.0f;
}

Vec2 barrel_distort(const Vec2& p, float k1, float k2) {
    const float r2 = p.x * p.x + p.y * p.y;
    const float s = 1.0f + k1 * r2 + k2 * r2 * r2;
    return {p.x * s, p.y * s};
}

}  // namespace rayneo::core
