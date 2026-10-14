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
    float fov_y_deg = 24.5f;     // per-eye vertical FOV of the optics
    float eye_aspect = 16.0f / 9.0f;
    float k1 = 0.0f;             // barrel pre-distortion, 0 = off
    float k2 = 0.0f;
    float max_prediction_ms = 50.0f;
    float dim_others = 1.0f;     // brightness of the panels you are not looking at, 1 = off
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
    // The capture feeding a panel (not owned; must outlive the renderer's use).
    void set_source(int panel, DesktopDuplication* source);

    float fit_arc_deg(int columns) const;
    int panel_at_gaze(const core::Quat& head) const;

    // True when the frame about to be drawn equals the one on screen: skip it.
    bool can_skip_frame(const core::Quat& orientation, const core::Vec3& angular_velocity, bool tracking, bool forced);
    void invalidate_skipped_frame() { skipper_.invalidate(); }

    // Predicts the pose to target_ns (steady clock), renders both eyes into `target`
    // (an sRGB view of the back buffer) and returns the prediction horizon applied, in ms.
    float render(ID3D11RenderTargetView* target, int width, int height, const core::Quat& orientation,
                 const core::Vec3& angular_velocity, uint64_t sample_ns, uint64_t target_ns);

private:
    struct Panel {
        DesktopDuplication* source = nullptr;
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
        float brightness = 1.0f;
    };

    std::vector<core::PanelPlacement> placements(float aspect) const;
    void rebuild_mesh(Panel& p, const core::PanelPlacement& pl);
    void compose(Panel& p);
    void ensure_eye_textures(int width, int height);
    float vertical_arc(float aspect) const;

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> screen_vs_, fullscreen_vs_, overlay_vs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> screen_ps_, distort_ps_, overlay_ps_;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> layout_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> scene_cb_, panel_cb_, distort_cb_, overlay_cb_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> raster_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blend_alpha_, blend_invert_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> placeholder_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> eye_tex_[2];
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> eye_rtv_[2];
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> eye_srv_[2];
    int eye_w_ = 0, eye_h_ = 0;

    Panel panels_[kMaxPanels];
    int count_ = 1;
    std::vector<core::GridCell> cells_;
    core::PanelDimmer dimmer_;
    core::FrameSkipper skipper_;
    uint64_t last_frame_ns_ = 0;
};

}  // namespace rayneo::win
