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
// `neck_up_m` / `neck_forward_m`: where the eyes sit relative to the neck's pivot (the head
// turns about the neck, not about the eyes). With them the eyes move a few centimetres as the
// head turns and near panels shift against the real room as real objects do; without, on
// see-through glasses the panels seemed to follow the head a little (> 1 deg in a 30 deg turn).
Mat4 view_matrix(const Quat& head_rel, float eye_offset_m, float neck_up_m = 0.0f, float neck_forward_m = 0.0f);

// Eye position from the neck's pivot (render frame: up, forward), metres: the pivot 8 cm below
// and 7 cm behind the point between the eyes.
inline constexpr float kNeckUpM = 0.08f;
inline constexpr float kNeckForwardM = 0.07f;

// Left-handed perspective with Metal/D3D clip depth [0, 1].
Mat4 perspective_lh(float fovy_rad, float aspect, float near_z, float far_z);

// The space at real size's prediction horizon (ms): the tuned fixed motion-to-photon time plus
// how much later than their target the frames are really shown (the measured lateness, clamped
// to [0, one period]): a streak a refresh late is then predicted for, instead of the panels
// sliding back and forth while the head turns. Plus the sample's age when the frame read it
// (0-2.2 ms at 457 Hz, clamped to [0, one period]): every frame then shows the head a fixed
// time after its own pose read, not after a sample on the IMU's own grid (that grid against
// 120 Hz put the drawn poses up to 1.1 ms off their frames' times: an uneven step while turning).
// The queue's own latency needs no term: the display link's target moves with it, and the
// lateness is measured against that target (two frames queued added 1.4 ms on time, not a
// period: logs of 17/18-10-2026).
float fixed_prediction_horizon_ms(float fixed_ms, float present_late_ms, float frame_period_ms, float sample_age_ms = 0.0f);

// Constant-rate extrapolation of a world-frame-delta orientation: exp(omega*dt) * q.
// omega is in the same frame as q's rotation axes (rad/s).
Quat predict_orientation(const Quat& q, const Vec3& omega_rads, float dt_s);

// The angular velocity (frame of q's rotation axes) without its part about the head's own
// forward axis (roll): the prediction then extrapolates turning and nodding only (PredXY),
// the noisiest and least useful axis left out.
Vec3 without_roll(const Quat& head, const Vec3& omega_rads);

// The head's turn about the vertical axis only (the heading of its forward axis): panels
// follow looking left and right, while nodding and tilting leave them where they are, so
// vertical head motion can never shake them.
Quat yaw_only(const Quat& q);

// The head's nod only (elevation of its forward axis): for stacked panels, turning left and
// right leaves them where they are.
Quat pitch_only(const Quat& q);

// Which head rotations move the panels.
// NoRoll: turning and looking up / down move the panels as in Both, but tilting the head
// sideways does not tilt them: they stay level with the eyes (Both keeps them level with the
// room, so a tilted head sees them askew).
enum class TrackingAxes { Both = 0, Horizontal = 1, Vertical = 2, NoRoll = 3 };
Quat track_axes(const Quat& q, TrackingAxes axes);

// Vehicle mode: of the head's turn about the vertical (its heading), only `k` moves the panels
// (pitch and roll stay 1:1), so a car turning carries the panels along partly instead of
// leaving them behind. kVehicleHeadingScale: a quarter of the heading is followed.
inline constexpr float kVehicleHeadingScale = 0.25f;
Quat scale_heading(const Quat& head, float k);

// Inside of a vertical cylinder section centred on the viewer, in render space.
struct ScreenVertex {
    float px, py, pz;
    float u, v;
};
struct ScreenMesh {
    std::vector<ScreenVertex> vertices;
    std::vector<uint16_t> indices;  // triangle list
};
// radius_m: viewing distance; arc_rad: horizontal angular width (> 0), centred on
// center_rad (0 = straight ahead, positive = to the right); height_m: screen height,
// centred on center_y_m. u runs left -> right, v top -> bottom (Metal texture origin).
ScreenMesh curved_screen_mesh(float radius_m, float arc_rad, float height_m, int segments,
                              float center_rad = 0.0f, float center_y_m = 0.0f);

// Flat panel facing the viewer, tangent to the circle of radius_m at center_rad, so it
// spans exactly arc_rad as seen from the origin (width 2 r tan(arc/2)). Adjacent flat
// panels with gap 0 share their edge, like monitors angled towards the user.
ScreenMesh flat_screen_mesh(float radius_m, float arc_rad, float height_m, int segments,
                            float center_rad = 0.0f, float center_y_m = 0.0f);

// Panel with adjustable curvature, as a sheet of fixed width 2 r tan(arc/2) bent around a
// vertical axis: curvature 0 = flat (flat_screen_mesh), 1 = a cylinder centred on the viewer
// (every column at distance r). In between, the bending radius is r / curvature and the
// panel centre stays at distance r, so distance and curvature are independent controls.
ScreenMesh bent_screen_mesh(float radius_m, float arc_rad, float height_m, int segments, float curvature,
                            float center_rad = 0.0f, float center_y_m = 0.0f);

// Physical width of a panel spanning arc_rad at radius_m (arc length if curved).
float panel_width(float radius_m, float arc_rad, bool flat);

// Horizontal row of `count` panels of equal arc separated by gap_rad, centred on straight
// ahead. Returns each panel's centre angle (render frame: negative = left), left to right.
std::vector<float> panel_centers(int count, float panel_arc_rad, float gap_rad);

// Panel the head is looking at: nearest centre to the view direction's yaw, or -1 when the
// view is more than half a step (arc + gap) beyond the outermost panels.
int panel_at_gaze(const Quat& head_rel, const std::vector<float>& centers, float panel_arc_rad, float gap_rad);

// --- Grid layouts (monitors side by side and/or stacked) ------------------------------------

struct GridCell {
    int col = 0;
    int row = 0;  // 0 = top
};

// Direction of a panel's centre in the render frame: yaw positive = right, pitch positive = up.
// The panel is the yawed panel rotated about the render X axis by pitch, so rows behave like
// a cylinder tilted up or down and every panel faces the viewer.
struct PanelPlacement {
    float yaw_rad = 0.0f;
    float pitch_rad = 0.0f;
    float scale = 1.0f;  // panel size relative to the common panel arc
};

// A panel placed by hand (Nimbo's layout editor), in degrees, at the 2 m reference with the
// common panel arc: yaw positive = right, pitch positive = up. Renderers scale the angles
// with the arc actually shown (distance, pinned fitting) so the arrangement keeps its shape.
struct PanelPose {
    float yaw_deg = 0.0f;
    float pitch_deg = 0.0f;
    float scale = 1.0f;
};

// Hand-placed panels as placements, angles multiplied by `angle_scale`.
std::vector<PanelPlacement> placements_from_poses(const std::vector<PanelPose>& poses, float angle_scale = 1.0f);

// Glasses' pinned mode: the share of the glasses' image the fitted arrangement fills, from
// the panel arc shown at the current size and distance. Every setting changes it and none
// overflows the image: the largest and closest (60 deg at 1.2 m) fills it, a 38 deg panel at
// 2 m about 81 %, the smallest and farthest (28 deg at 3 m) about 68 %.
float pinned_zoom(float shown_arc_rad);

// Floating panels (from `first_floating` on) moved out of the panels before them: along the
// axis that frees them soonest, away from the one they overlap, `gap_rad` clear. A window
// placed just above the monitors stays just above them whatever their shape (the Mac's
// 16:10 screen is taller than the 16:9 the editor assumed; overlapping, the two flickered).
// `widths_rad` / `heights_rad`: each panel's extent at its scale.
void separate_floating(std::vector<PanelPlacement>& panels, const std::vector<float>& widths_rad,
                       const std::vector<float>& heights_rad, size_t first_floating, float gap_rad);

// Grid cells for the OS display arrangement (so the cursor crosses to the neighbour you see):
// panels grouped into columns by yaw and rows by pitch, in panel order, every cell unique.
std::vector<GridCell> cells_from_poses(const std::vector<PanelPose>& poses, float column_deg, float row_deg);

// Angular height of a panel of horizontal arc `arc_rad` and width/height `aspect`.
float panel_vertical_arc(float arc_rad, float aspect, bool flat);

// Placements for `cells`, centred on the bounding box of the grid.
std::vector<PanelPlacement> grid_layout(const std::vector<GridCell>& cells, float arc_rad, float vertical_arc_rad,
                                        float gap_rad);

// Rotates a mesh about the render X axis: pitch > 0 tilts it up.
void pitch_mesh(ScreenMesh& mesh, float pitch_rad);

// A panel built straight ahead at `distance_m` moved up or down to `pitch_rad`. `tilt` 1 faces
// it to the viewer (rows like a tilted cylinder), 0 keeps it upright like a wall of monitors,
// in between leans part of the way (it turns about its own centre).
void place_pitched(ScreenMesh& mesh, float pitch_rad, float tilt, float distance_m);

// Panel whose cell (panel plus half the gap on each side) contains the view direction, or -1.
// The whole arrangement lifted (pitch_rad > 0) or lowered to an elevation: every panel's pitch
// moved by it (the recentre's elevation, Pose::panels_pitch_rad).
void lift_placements(std::vector<PanelPlacement>& panels, float pitch_rad);

// Where the head looks, as a panel placement (yaw right, pitch up; scale 1).
PanelPlacement gaze_placement(const Quat& head_rel);

int panel_at_gaze(const Quat& head_rel, const std::vector<PanelPlacement>& panels, float arc_rad,
                  float vertical_arc_rad, float gap_rad);

// Horizontal FOV from the vertical FOV and the perceived per-eye aspect.
float horizontal_fov(float fovy_rad, float aspect);

// Panel arc so that `count` panels plus their gaps fill `fov_rad` (minus a margin on each
// side): every panel visible at once, at the cost of angular resolution.
float fit_panel_arc(float fov_rad, int count, float gap_rad, float margin_rad);

// Angular width of a panel that spans `arc_rad` at `reference_m` once moved to `distance_m`:
// closer looks bigger, farther smaller, like a real monitor. Its own inverse with the two
// distances swapped.
float arc_at_distance(float arc_rad, float distance_m, float reference_m = 2.0f);

// Arc width (rad) that shows a source `pixels_wide` at `pixels_per_degree` angular density.
float arc_for_pixels(float pixels_wide, float pixels_per_degree);

// Radial lens pre-distortion in normalised eye coordinates (centre 0, half-height 1):
// p' = p * (1 + k1 r^2 + k2 r^4). Positive k pre-compensates pincushion optics.
struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;
};
Vec2 barrel_distort(const Vec2& p, float k1, float k2);

}  // namespace rayneo::core
