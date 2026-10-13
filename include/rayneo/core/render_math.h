#pragma once

// Renderer-facing math. Still OS/GPU agnostic: plain floats, column-major matrices that
// can be memcpy'd into Metal (simd_float4x4) or Direct3D constant buffers.

#include <cstdint>
#include <vector>

#include "rayneo/core/types.h"

namespace rayneo::core {

// Column-major 4x4: element (row r, col c) is m[c * 4 + r].
struct Mat4 {
    float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

    float at(int r, int c) const { return m[c * 4 + r]; }
    float& at(int r, int c) { return m[c * 4 + r]; }
    Mat4 operator*(const Mat4& b) const;
    Vec3 transform_point(const Vec3& p) const;  // w = 1, no perspective divide
};

// Core frame: X forward, Y left, Z up, right-handed (earth Z-up).
// Render frame (Metal convention used here): X right, Y up, Z forward, left-handed.
// The mapping is a fixed axis permutation with det = -1 (handedness flip):
//   render = (-core.y, core.z, core.x)
Vec3 core_to_render(const Vec3& v);

// Head rotation (core frame, head -> level reference frame) as a render-frame rotation
// matrix: M * R * M^T. Proper rotation (det +1) in the left-handed render basis.
Mat4 render_rotation(const Quat& head_rel);

// World -> eye view matrix. eye_offset_m is the eye's lateral position in head space
// (render X, i.e. -IPD/2 for the left eye).
Mat4 view_matrix(const Quat& head_rel, float eye_offset_m);

// Left-handed perspective with Metal/D3D clip depth [0, 1].
Mat4 perspective_lh(float fovy_rad, float aspect, float near_z, float far_z);

// Constant-rate extrapolation of a world-frame-delta orientation: exp(omega*dt) * q.
// omega is in the same frame as q's rotation axes (rad/s).
Quat predict_orientation(const Quat& q, const Vec3& omega_rads, float dt_s);

// Inside of a vertical cylinder section centred on the viewer, in render space.
struct ScreenVertex {
    float px, py, pz;
    float u, v;
};
struct ScreenMesh {
    std::vector<ScreenVertex> vertices;
    std::vector<uint16_t> indices;  // triangle list
};
// radius_m: viewing distance; arc_rad: horizontal angular width (> 0), centred on +Z;
// height_m: screen height. u runs left -> right, v top -> bottom (Metal texture origin).
ScreenMesh curved_screen_mesh(float radius_m, float arc_rad, float height_m, int segments);

// Radial lens pre-distortion in normalised eye coordinates (centre 0, half-height 1):
// p' = p * (1 + k1 r^2 + k2 r^4). Positive k pre-compensates pincushion optics.
struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;
};
Vec2 barrel_distort(const Vec2& p, float k1, float k2);

}  // namespace rayneo::core
