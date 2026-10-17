#pragma once

// Direct3D 11 renderer for the spatial desktop: N panels around the viewer, the Windows
// counterpart of RNSpatialRenderer.mm. Same core render math, same two passes (panels into
// per-eye textures, then barrel pre-distortion into the back buffer, half-SBS or mono).

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "depth_engine.h"
#include "desktop_duplication.h"
#include "rayneo/core/frame_skip.h"
#include "rayneo/core/panel_dimmer.h"
#include "rayneo/core/render_math.h"

namespace rayneo::win {

struct RenderOptions {
    bool side_by_side = true;    // half-SBS for the glasses' 3D mode
    float ipd_m = 0.063f;
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
    float dim_others = 1.0f;     // brightness of the panels you are not looking at, 1 = off
    core::TrackingAxes tracking_axes = core::TrackingAxes::Both;  // head rotations that move the panels
    bool static_head = false;    // glasses in their own pinned mode: they anchor the image, not us
    float layout_arc_deg = 0.0f; // panel size the hand-placed poses refer to (0 = arc_deg)
    int focus_panel = -1;        // focus mode: only this panel is drawn (-1 = all)
    bool cinema = false;         // with focus_panel: centred straight ahead and large
    float zoom = 1.0f;           // every panel this much larger (and the arrangement with them), 1 = off
    // AI 3D: this panel's frames go through the depth engine and it is drawn in depth when side
    // by side (-1: none).
    int depth_panel = -1;
    float depth_strength = 0.03f;     // parallax between the eyes at the nearest depth, in panel widths
    float depth_convergence = 0.35f;   // depth (0 far .. 1 near) left on the panel's plane
    float depth_smoothing = 0.35f;     // weight of each new depth map against the previous ones
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
    // AI 3D (not owned; must outlive the renderer's use).
    void set_depth_engine(DepthEngine* engine) { depth_ = engine; }
    struct DepthStats {
        int shown = 0;    // frames shown with their depth
        int dropped = 0;  // read back but replaced by a newer one before the engine was free
    };
    DepthStats take_depth_stats() {
        const DepthStats s = depth_stats_;
        depth_stats_ = DepthStats{};
        return s;
    }

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

        Microsoft::WRL::ComPtr<ID3D11Buffer> vertices, indices;
        UINT index_count = 0;
        float mesh_aspect = 0, mesh_distance = 0, mesh_arc = 0, mesh_center = 0, mesh_pitch = 0;
        float mesh_curvature = -1.0f;
        float mesh_tilt = -1.0f;
        float mesh_scale = 0.0f;
        float mesh_zoom = 0.0f;
        float brightness = 1.0f;
    };

    std::vector<core::PanelPlacement> placements(float aspect) const;
    void rebuild_mesh(Panel& p, const core::PanelPlacement& pl);
    void compose(Panel& p);
    // compose in two steps: the frame copied into the composite, then the cursor and mipmaps.
    bool compose_copy(Panel& p, ID3D11Texture2D* frame, int width, int height);
    void compose_finish(Panel& p, const PointerState& pointer);

    // AI 3D. A frame of the depth panel goes: copied into a slot (with its pointer), shrunk on
    // the GPU and read back without waiting (Reading), handed to the engine (Inferring; while it
    // is busy, the newest read-back one waits: Ready), and shown with its depth when that comes.
    struct DepthSlot {
        enum class State { Free, Reading, Ready, Inferring } state = State::Free;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> color;         // full size, mipmapped (to shrink without aliasing)
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> color_srv;  // UNORM
        Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;       // the shrunk frame for the CPU
        int width = 0, height = 0;                             // full size
        int small_w = 0, small_h = 0;
        PointerState pointer;
        uint64_t seq = 0;
        DepthInput input;                                      // Ready: the bytes read back
    };
    static constexpr int kDepthSlots = 3;
    void depth_frame(Panel& p);
    void depth_capture(Panel& p);
    void depth_reset();
    bool depth_pending() const;
    bool ensure_depth_slot(DepthSlot& s, int width, int height, int small_w, int small_h);
    void blend_depth(const DepthOutput& out);
    void ensure_eye_textures(int width, int height);
    float vertical_arc(float aspect) const;
    void fit_in_glasses_image(std::vector<core::PanelPlacement>& out) const;
    void fill_glasses_image(std::vector<core::PanelPlacement>& out, int panel) const;
    bool grow_animating() const;
    bool filling_image() const;
    float panel_arc() const;

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> screen_vs_, fullscreen_vs_, overlay_vs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> screen_ps_, distort_ps_, overlay_ps_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> stereo_ps_, depth_blend_ps_, downscale_ps_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> stereo_cb_, depth_blend_cb_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> depth_sampler_;
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
    UINT overlay_index_count_ = 0;
    uint64_t overlay_generation_ = 0;  // a new message must be drawn even with a still head
    Microsoft::WRL::ComPtr<ID3D11Texture2D> eye_tex_[2];
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> eye_rtv_[2];
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> eye_srv_[2];
    int eye_w_ = 0, eye_h_ = 0;

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

    DepthEngine* depth_ = nullptr;
    DepthSlot depth_slots_[kDepthSlots];
    int depth_active_ = -1;                 // panel the slots and maps belong to
    uint64_t depth_seq_ = 0;
    uint64_t depth_captured_generation_ = ~0ull;
    uint64_t depth_generation_ = 0;         // bumps with every frame shown in depth: draw it
    Microsoft::WRL::ComPtr<ID3D11Texture2D> depth_small_;  // the shrunk frame (render target)
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> depth_small_rtv_;
    int depth_small_w_ = 0, depth_small_h_ = 0;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> depth_raw_;    // R32F, the engine's map as it came
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> depth_raw_srv_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> depth_smooth_[2];  // R16F, temporally smoothed (ping-pong)
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> depth_smooth_rtv_[2];
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> depth_smooth_srv_[2];
    int depth_map_w_ = 0, depth_map_h_ = 0;
    int depth_smooth_index_ = 0;
    bool depth_valid_ = false;              // a smoothed map exists for depth_active_
    float depth_video_[4] = {0, 0, 1, 1};   // where the video is in the frame: depth only there
    DepthStats depth_stats_;
};

}  // namespace rayneo::win
