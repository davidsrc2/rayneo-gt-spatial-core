#pragma once

// Direct3D 11 renderer for the spatial desktop: N panels around the viewer, the Windows
// counterpart of RNSpatialRenderer.mm. Same core render math, same two passes (panels into
// per-eye textures, then barrel pre-distortion into the back buffer, half-SBS or mono).

#include <d3d11.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "desktop_duplication.h"
#include "rayneo/core/frame_skip.h"
#include "rayneo/core/panel_dimmer.h"
#include "rayneo/core/render_math.h"

namespace rayneo::win {

struct RenderOptions {
    bool side_by_side = false;   // the glasses in their 3D mode (double width): the same image in both halves
    float distance_m = 2.0f;     // panel radius
    float arc_deg = 50.0f;       // horizontal extent of each panel
    float gap_deg = 1.0f;        // space between panels
    float curvature = 0.0f;      // 0 = flat monitors angled towards you, 1 = cylinder
    float tilt = 1.0f;
    bool neck_model = false;     // eyes ahead of the neck's pivot (core::view_matrix)           // panels above / below: 1 face you, 0 upright (core::place_pitched)
    float fov_y_deg = 24.5f;     // per-eye vertical FOV of the optics
    float eye_aspect = 16.0f / 9.0f;
    float k1 = 0.0f;             // barrel pre-distortion, 0 = off
    float k2 = 0.0f;
    float max_prediction_ms = 50.0f;
    float fixed_prediction_ms = 0.0f;  // > 0: always this far ahead (space at real size), not the measured horizon
    // With fixed_prediction_ms: how late frames reach the display (ms, measured) and the refresh
    // period, added to it (core::fixed_prediction_horizon_ms). Set before each frame.
    float present_late_ms = 0.0f;
    float frame_period_ms = 1000.0f / 60;
    float sample_age_ms = 0.0f;  // the pose's age when the frame read it
    int supersample = 1;               // > 1: eyes drawn this much larger, mipmapped down (space at real size)
    float dim_others = 1.0f;     // brightness of the panels you are not looking at, 1 = off
    core::TrackingAxes tracking_axes = core::TrackingAxes::Both;  // head rotations that move the panels
    bool static_head = false;    // glasses in their own pinned mode: they anchor the image, not us
    float layout_arc_deg = 0.0f; // panel size the hand-placed poses refer to (0 = arc_deg)
    int focus_panel = -1;        // focus mode: only this panel is drawn (-1 = all)
    bool cinema = false;         // with focus_panel: centred straight ahead and large
    float cinema_yaw_rad = 0.0f;   // where the cinema panel is centred (yaw right, pitch up)
    float cinema_pitch_rad = 0.0f;
    float panels_pitch_rad = 0.0f;  // the arrangement lifted to the recentre's elevation (up > 0)
    bool vehicle_mode = false;      // a quarter of the head's heading followed (core::scale_heading)
    // Wall: flat panels at distance_m, each square to the eye (as RealWarp's flat
    // screens), edge to edge as one block curving towards you (no gap, curvature or tilt; core::wall_grid_layout / wall_panel_mesh), fixed in
    // the room on every axis, as macOS.
    bool flat_wall = true;
    int key_panel = -1;  // the panel that gets the keyboard (foreground window), framed; -1 none
    float zoom = 1.0f;           // every panel this much larger (and the arrangement with them), 1 = off
    // Around the panels, fixed in the room: 0 none (the room seen through), 1 rain, 2 stars,
    // 3 sunset, 4 aurora, 5 the picture in background_image (UTF-8 path; loaded when it changes).
    int background = 0;
    float background_brightness = 0.6f;
    std::string background_image;
};

// Render thread only, except where noted.
class SpatialRenderer {
public:
    static constexpr int kMaxPanels = 8;

    bool init(ID3D11Device* device, std::string* error);

    RenderOptions options;

    void set_panel_count(int count);
    int panel_count() const { return count_; }
    // Grid cell per panel (row 0 = top); empty or mismatched count: one row, left -> right.
    void set_grid(std::vector<core::GridCell> cells) { cells_ = std::move(cells); }
    // Hand-placed position and size per panel (layout editor); empty or another count: the grid.
    void set_poses(std::vector<core::PanelPose> poses) { poses_ = std::move(poses); }
    // Floating windows: the last N panels (after the monitors), placed at these poses.
    void set_floating(std::vector<core::PanelPose> poses) { floating_ = std::move(poses); }
    // A message fixed in front of the eyes (start-up), two lines separated by '\n'; empty hides it.
    void set_overlay_text(const std::wstring& text);
    // The capture feeding a panel (not owned; must outlive the renderer's use).
    void set_source(int panel, FrameSource* source);
    // The cursor over a panel, where GetCursorPos puts it now (hotspot, in the panel's pixels):
    // drawn every frame, not baked into the capture, so it moves at the glasses' rate.
    void set_cursor(int panel, bool visible, int x, int y);
    // Panels the glasses show or are about to (bit per panel), from the last frame drawn: the
    // others keep their last composite and their captures slow down. Any thread.
    uint32_t visible_panel_mask() const { return visible_mask_.load(std::memory_order_relaxed); }

    float fit_arc_deg(int columns) const;
    int panel_at_gaze(const core::Quat& head) const;
    // Where the panels are drawn (monitors, then floating windows) and the common panel width
    // (radians), for laying the OS displays out to match.
    std::vector<core::PanelPlacement> current_placements() const { return placements(panels_[0].aspect); }
    float shown_arc_rad() const { return panel_arc(); }

    // Glasses in their pinned mode: this panel fills their image and the others lie just beyond
    // its edges; changing it pans and zooms there in half a second. -1: all fitted in the image.
    void set_grow_panel(int panel);
    // The zoom (<= 1) at which every panel fits in the glasses' image at once (an overview).
    float overview_zoom() const;
    int grow_panel() const { return grow_panel_; }

    // True when the frame about to be drawn equals the one on screen: skip it.
    bool can_skip_frame(const core::Quat& orientation, const core::Vec3& angular_velocity, bool tracking, bool forced);
    void invalidate_skipped_frame() { skipper_.invalidate(); }

    // Predicts the pose to target_ns (steady clock), renders both eyes into `target`
    // (an sRGB view of the back buffer) and returns the prediction horizon applied, in ms.
    float render(ID3D11RenderTargetView* target, int width, int height, const core::Quat& orientation,
                 const core::Vec3& angular_velocity, uint64_t sample_ns, uint64_t target_ns);

private:
    struct Panel {
        FrameSource* source = nullptr;
        uint64_t composed_generation = ~0ull;
        float aspect = 16.0f / 9.0f;

        // The capture is copied here once per change, the cursor drawn on top, then
        // mipmapped: panels are drawn at a fraction of the capture's size, and sampling the
        // full-size frame directly aliases (text shimmers at the slightest head motion).
        Microsoft::WRL::ComPtr<ID3D11Texture2D> composite;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> composite_srv;  // sRGB
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> composite_rtv;    // UNORM, mip 0
        int width = 0, height = 0;

        uint64_t shape_generation = 0;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> cursor_color, cursor_invert;
        int cursor_w = 0, cursor_h = 0;
        int cursor_hot_x = 0, cursor_hot_y = 0;
        bool cursor_visible = false;
        int cursor_x = 0, cursor_y = 0;  // hotspot, panel pixels

        Microsoft::WRL::ComPtr<ID3D11Buffer> vertices, indices;
        std::vector<core::ScreenVertex> mesh_vertices;  // CPU copy, for the in-view test
        UINT index_count = 0;
        float mesh_aspect = 0, mesh_distance = 0, mesh_arc = 0, mesh_center = 0, mesh_pitch = 0;
        float mesh_wall_pitch = 0;  // the wall's turn to the recentre's elevation
        float mesh_curvature = -1.0f;
        float mesh_tilt = -1.0f;
        float mesh_scale = 0.0f;
        float mesh_zoom = 0.0f;
        bool mesh_wall = false;
        float brightness = 1.0f;
    };

    std::vector<core::PanelPlacement> placements(float aspect) const;
    void rebuild_mesh(Panel& p, const core::PanelPlacement& pl, bool wall);
    bool wall_mode() const { return options.flat_wall && !options.static_head; }
    // The wall is a poster in the room (as RealWarp, anchored to gravity): turning and nodding
    // move it, so a brisk turn's bob or nod can not carry it up and down against the room. Its
    // roll is left out (NoRoll): a calibration a little off turned the panels about the gaze as
    // the head looked up, until they stood on end, and slanted the side ones on a turn.
    core::TrackingAxes tracking_axes() const { return wall_mode() ? core::TrackingAxes::NoRoll : options.tracking_axes; }
    void compose(Panel& p);
    // compose in two steps: the frame copied into the composite, then its mipmaps.
    bool compose_copy(Panel& p, ID3D11Texture2D* frame, int width, int height);
    void compose_finish(Panel& p);
    void update_cursor_shape(Panel& p);
    void draw_cursor(const Panel& p, ID3D11PixelShader* panel_ps);

    void ensure_eye_textures(int width, int height, bool mips);
    float vertical_arc(float aspect) const;
    void fit_in_glasses_image(std::vector<core::PanelPlacement>& out) const;
    void fill_glasses_image(std::vector<core::PanelPlacement>& out, int panel) const;
    bool grow_animating() const;
    bool filling_image() const;
    float panel_arc() const;

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> screen_vs_, fullscreen_vs_, overlay_vs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> screen_ps_, distort_ps_, overlay_ps_, cursor_ps_;
    uint64_t cursor_generation_ = 0;  // bumps when a drawn cursor moves, shows, hides or changes shape
    Microsoft::WRL::ComPtr<ID3D11InputLayout> layout_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> scene_cb_, panel_cb_, distort_cb_, overlay_cb_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> raster_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blend_alpha_, blend_invert_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> placeholder_;
    // Background (options.background): its shader is optional (compiled apart: a failure leaves
    // Nimbo without backgrounds, not without panels).
    Microsoft::WRL::ComPtr<ID3D11PixelShader> sharp_ps_;  // the space at real size: Catmull-Rom (optional, ps_5_0)
    Microsoft::WRL::ComPtr<ID3D11PixelShader> background_ps_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> background_cb_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> mirror_sampler_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> background_srv_;
    float background_aspect_ = 1.0f;
    std::string loaded_background_;
    void load_background(const std::string& path);
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> overlay_;  // start-up message, null = none
    Microsoft::WRL::ComPtr<ID3D11Buffer> overlay_vertices_, overlay_indices_;
    int overlay_mesh_height_ = 0;  // the card height (px) its mesh was built for
    UINT overlay_index_count_ = 0;
    uint64_t overlay_generation_ = 0;  // a new message must be drawn even with a still head
    Microsoft::WRL::ComPtr<ID3D11Texture2D> eye_tex_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> eye_rtv_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> eye_srv_;
    int eye_w_ = 0, eye_h_ = 0;
    bool eye_mips_ = false;

    Panel panels_[kMaxPanels];
    int count_ = 1;
    std::vector<core::GridCell> cells_;
    std::vector<core::PanelPose> poses_;
    std::vector<core::PanelPose> floating_;
    core::PanelDimmer dimmer_;
    core::FrameSkipper skipper_;
    uint64_t last_frame_ns_ = 0;

    mutable float zoom_override_ = 0.0f;     // > 0: panel_arc() uses this zoom (overview_zoom measures at 1)
    int grow_panel_ = -1, grow_from_ = -1;  // glasses' pinned mode: highlighted panel, the one before
    uint64_t grow_start_ns_ = 0;             // start of the pan / zoom between them (steady clock)
    uint64_t grow_generation_ = 0;           // bumps on every change: draw it
    std::atomic<uint32_t> visible_mask_{~0u};  // all in view until the first frame says otherwise
    uint64_t view_generation_ = 0;             // bumps when a panel comes into view: its waiting frame is drawn

};

}  // namespace rayneo::win
