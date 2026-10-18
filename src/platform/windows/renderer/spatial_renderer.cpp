#include "spatial_renderer.h"

#include <d3dcompiler.h>
#include <wincodec.h>

#include <algorithm>
#include <chrono>
#include <cfloat>
#include <cmath>
#include <cstring>

#include "rayneo/core/pose_stabilizer.h"
#include "rn_shaders_hlsl.h"  // generated: kRNShaderSourceHlsl

using Microsoft::WRL::ComPtr;

namespace rayneo::win {

namespace {

constexpr float kDegToRad = 0.017453292519943295f;

struct SceneUniforms {
    float view_projection[16];
};
struct PanelUniforms {
    float brightness;
    float pad[3];
};
struct DistortUniforms {
    float k1, k2, aspect, pad;
};
struct OverlayUniforms {
    float rect[4];
};
struct StereoUniforms {
    float brightness, eye, strength, convergence, edge, pad[3];
    float rect[4];  // where the video is, 0..1: x0, y0, x1, y1
};
struct DepthBlendUniforms {
    float low, high, alpha, lod;
};

// Mirrors BackgroundUniforms in Shaders.hlsl.
struct BackgroundUniforms {
    float right[4], up[4], forward[4];
    float tan_x, tan_y, time, brightness;
    int style;
    float image_aspect;
    float pad[2];
};

constexpr float kDepthEdge = 0.05f;  // AI 3D: side margins where the image returns to the panel's plane

bool compile(const char* entry, const char* target, ComPtr<ID3DBlob>* out, std::string* error,
             const D3D_SHADER_MACRO* defines = nullptr) {
    ComPtr<ID3DBlob> errors;
    const HRESULT hr = D3DCompile(kRNShaderSourceHlsl, std::strlen(kRNShaderSourceHlsl), "Shaders.hlsl", defines,
                                  nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, out->GetAddressOf(),
                                  &errors);
    if (FAILED(hr)) {
        if (error != nullptr) {
            *error = std::string("shader ") + entry + ": " +
                     (errors ? static_cast<const char*>(errors->GetBufferPointer()) : "compile failed");
        }
        return false;
    }
    return true;
}

ComPtr<ID3D11Buffer> constant_buffer(ID3D11Device* device, UINT size) {
    D3D11_BUFFER_DESC d{};
    d.ByteWidth = size;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ComPtr<ID3D11Buffer> b;
    device->CreateBuffer(&d, nullptr, &b);
    return b;
}

ComPtr<ID3D11ShaderResourceView> image(ID3D11Device* device, int w, int h, const uint32_t* bgra, DXGI_FORMAT format) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = static_cast<UINT>(w);
    d.Height = static_cast<UINT>(h);
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = format;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_IMMUTABLE;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init{bgra, static_cast<UINT>(w * 4), 0};
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11ShaderResourceView> srv;
    if (SUCCEEDED(device->CreateTexture2D(&d, &init, &tex))) device->CreateShaderResourceView(tex.Get(), nullptr, &srv);
    return srv;
}

}  // namespace

bool SpatialRenderer::init(ID3D11Device* device, std::string* error) {
    device_ = device;
    device_->GetImmediateContext(&ctx_);

    ComPtr<ID3DBlob> vs, ps;
    if (!compile("screen_vertex", "vs_4_0", &vs, error)) return false;
    device_->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &screen_vs_);
    const D3D11_INPUT_ELEMENT_DESC elements[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    static_assert(sizeof(core::ScreenVertex) == 20, "5 tightly packed floats");
    if (FAILED(device_->CreateInputLayout(elements, 2, vs->GetBufferPointer(), vs->GetBufferSize(), &layout_))) {
        if (error != nullptr) *error = "CreateInputLayout failed";
        return false;
    }
    if (!compile("fullscreen_vertex", "vs_4_0", &vs, error)) return false;
    device_->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &fullscreen_vs_);
    if (!compile("overlay_vertex", "vs_4_0", &vs, error)) return false;
    device_->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &overlay_vs_);
    if (!compile("screen_fragment", "ps_4_0", &ps, error)) return false;
    device_->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &screen_ps_);
    if (!compile("distort_fragment", "ps_4_0", &ps, error)) return false;
    device_->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &distort_ps_);
    if (!compile("overlay_fragment", "ps_4_0", &ps, error)) return false;
    device_->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &overlay_ps_);
    if (!compile("cursor_fragment", "ps_4_0", &ps, error)) return false;
    device_->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &cursor_ps_);
    if (!compile("stereo_screen_fragment", "ps_4_0", &ps, error)) return false;
    device_->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &stereo_ps_);
    if (!compile("depth_blend_fragment", "ps_4_0", &ps, error)) return false;
    device_->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &depth_blend_ps_);
    if (!compile("downscale_fragment", "ps_4_0", &ps, error)) return false;
    device_->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &downscale_ps_);

    scene_cb_ = constant_buffer(device_.Get(), sizeof(SceneUniforms));
    panel_cb_ = constant_buffer(device_.Get(), sizeof(PanelUniforms));
    distort_cb_ = constant_buffer(device_.Get(), sizeof(DistortUniforms));
    overlay_cb_ = constant_buffer(device_.Get(), sizeof(OverlayUniforms));
    stereo_cb_ = constant_buffer(device_.Get(), sizeof(StereoUniforms));
    depth_blend_cb_ = constant_buffer(device_.Get(), sizeof(DepthBlendUniforms));

    D3D11_SAMPLER_DESC s{};
    s.Filter = D3D11_FILTER_ANISOTROPIC;  // trilinear + anisotropic: crisp, stable text
    s.MaxAnisotropy = 16;
    s.AddressU = s.AddressV = s.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;  // clamp to zero
    s.ComparisonFunc = D3D11_COMPARISON_NEVER;
    s.MaxLOD = FLT_MAX;
    device_->CreateSamplerState(&s, &sampler_);
    // Depth maps and the shrinking for the depth model: trilinear, edges clamped.
    D3D11_SAMPLER_DESC ds{};
    ds.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    ds.AddressU = ds.AddressV = ds.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    ds.ComparisonFunc = D3D11_COMPARISON_NEVER;
    ds.MaxLOD = FLT_MAX;
    device_->CreateSamplerState(&ds, &depth_sampler_);
    // The space at real size: bicubic panels (optional: without them, the plain shader).
    std::string sharp_error;
    const D3D_SHADER_MACRO sharp_defines[] = {{"NIMBO_SHARP", "1"}, {nullptr, nullptr}};
    if (compile("screen_sharp_fragment", "ps_5_0", &ps, &sharp_error, sharp_defines)) {
        device_->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &sharp_ps_);
    } else {
        OutputDebugStringA(("nimbo: sharp shader: " + sharp_error + "\n").c_str());
    }
    // Background: optional.
    std::string bg_error;
    const D3D_SHADER_MACRO bg_defines[] = {{"NIMBO_BACKGROUND", "1"}, {nullptr, nullptr}};
    if (compile("background_fragment", "ps_4_0", &ps, &bg_error, bg_defines)) {
        device_->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &background_ps_);
        background_cb_ = constant_buffer(device_.Get(), sizeof(BackgroundUniforms));
        D3D11_SAMPLER_DESC ms = ds;
        ms.AddressU = D3D11_TEXTURE_ADDRESS_MIRROR;
        ms.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        device_->CreateSamplerState(&ms, &mirror_sampler_);
    } else {
        OutputDebugStringA(("nimbo: background shader: " + bg_error + "\n").c_str());
    }

    D3D11_RASTERIZER_DESC r{};
    r.FillMode = D3D11_FILL_SOLID;
    r.CullMode = D3D11_CULL_NONE;
    r.DepthClipEnable = TRUE;
    device_->CreateRasterizerState(&r, &raster_);

    D3D11_BLEND_DESC b{};
    auto& rt = b.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D11_BLEND_SRC_ALPHA;
    rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOp = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D11_BLEND_ZERO;
    rt.DestBlendAlpha = D3D11_BLEND_ONE;
    rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    device_->CreateBlendState(&b, &blend_alpha_);
    // src * (1 - dst) + dst * (1 - src): white inverts the desktop, black leaves it.
    rt.SrcBlend = D3D11_BLEND_INV_DEST_COLOR;
    rt.DestBlend = D3D11_BLEND_INV_SRC_COLOR;
    device_->CreateBlendState(&b, &blend_invert_);

    const uint32_t grey = 0xFF16181Cu;  // BGRA {28, 24, 22}: dark panel until the first frame
    placeholder_ = image(device_.Get(), 1, 1, &grey, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
    if (!sampler_ || !depth_sampler_ || !raster_ || !blend_alpha_ || !blend_invert_ || !placeholder_ || !scene_cb_ ||
        !stereo_cb_ || !depth_blend_cb_) {
        if (error != nullptr) *error = "D3D11 state creation failed";
        return false;
    }
    return true;
}

void SpatialRenderer::set_panel_count(int count) { count_ = std::clamp(count, 1, kMaxPanels); }

void SpatialRenderer::set_cursor(int panel, bool visible, int x, int y) {
    if (panel < 0 || panel >= kMaxPanels) return;
    Panel& p = panels_[panel];
    if (!visible && !p.cursor_visible) return;
    if (visible == p.cursor_visible && x == p.cursor_x && y == p.cursor_y) return;
    p.cursor_visible = visible;
    p.cursor_x = x;
    p.cursor_y = y;
    cursor_generation_ += 1;
}

void SpatialRenderer::set_source(int panel, FrameSource* source) {
    if (panel < 0 || panel >= kMaxPanels) return;
    panels_[panel].source = source;
    panels_[panel].composed_generation = ~0ull;
}

// The panel arc as seen at the current distance (closer looks bigger, like a real monitor).
float SpatialRenderer::panel_arc() const {
    const float zoom = zoom_override_ > 0.0f ? zoom_override_ : options.zoom;
    return core::arc_at_distance(options.arc_deg * kDegToRad, options.distance_m) * std::clamp(zoom, 0.2f, 4.0f);
}

float SpatialRenderer::vertical_arc(float aspect) const {
    return core::panel_vertical_arc(panel_arc(), aspect, true);
}

// Monitors first (grid or hand-placed), then the floating windows.
std::vector<core::PanelPlacement> SpatialRenderer::placements(float aspect) const {
    const int floats = std::min(static_cast<int>(floating_.size()), std::max(0, count_ - 1));
    const int monitors = count_ - floats;
    // Placed at the editor's panel size: angles follow the arc actually shown (distance,
    // pinned fitting), so the arrangement keeps its shape.
    const float placed = (options.layout_arc_deg > 0.0f ? options.layout_arc_deg : options.arc_deg) * kDegToRad;
    const float k = placed > 0.0f ? panel_arc() / placed : 1.0f;
    std::vector<core::PanelPlacement> out;
    if (static_cast<int>(poses_.size()) == monitors) {
        out = core::placements_from_poses(poses_, k);
    } else {
        std::vector<core::GridCell> cells = cells_;
        if (static_cast<int>(cells.size()) != monitors) {
            cells.clear();
            for (int i = 0; i < monitors; ++i) cells.push_back({i, 0});
        }
        out = core::grid_layout(cells, panel_arc(), vertical_arc(aspect), options.gap_deg * kDegToRad);
    }
    const std::vector<core::PanelPose> fl(floating_.begin(), floating_.begin() + floats);
    for (const auto& pl : core::placements_from_poses(fl, k)) out.push_back(pl);
    if (floats > 0) {  // windows never over the monitors, whatever their shape
        std::vector<float> w, h;
        for (size_t i = 0; i < out.size(); ++i) {
            w.push_back(panel_arc() * out[i].scale);
            h.push_back(core::panel_vertical_arc(w.back(), panels_[std::min<size_t>(i, kMaxPanels - 1)].aspect, true));
        }
        core::separate_floating(out, w, h, static_cast<size_t>(monitors), options.gap_deg * kDegToRad);
    }
    if (!options.static_head) core::lift_placements(out, options.panels_pitch_rad);  // the recentre's elevation
    // Cinema: the focused panel straight ahead, large (the others are not drawn), but never
    // larger than the glasses' image (at real size a panel nearly fills it already).
    if (options.cinema && options.focus_panel >= 0 && options.focus_panel < static_cast<int>(out.size())) {
        const float a = panels_[std::min(options.focus_panel, kMaxPanels - 1)].aspect;
        const float fw = core::horizontal_fov(options.fov_y_deg * kDegToRad, options.eye_aspect);
        const float fh = options.fov_y_deg * kDegToRad;
        // Fitted at zoom 1; the zoom (Ctrl+Alt++ / -) then scales the fitted size, as on macOS.
        const float zoom = std::clamp(zoom_override_ > 0.0f ? zoom_override_ : options.zoom, 0.2f, 4.0f);
        const float arc = panel_arc() / zoom;
        const float fit = 0.95f * std::min(fw / arc, fh / core::panel_vertical_arc(arc, a, true));
        out[static_cast<size_t>(options.focus_panel)] =
            core::PanelPlacement{options.cinema_yaw_rad, options.cinema_pitch_rad, std::min(1.6f, fit)};
    }
    if (options.static_head) fit_in_glasses_image(out);
    return out;
}

namespace {
constexpr double kGrowSeconds = 0.5;   // glasses' pinned mode: pan and zoom to another highlighted panel
constexpr float kGrowDim = 0.45f;      // the panels around the highlighted one

uint64_t renderer_now_ns() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}
}  // namespace

float SpatialRenderer::overview_zoom() const {
    zoom_override_ = 1.0f;  // placements at zoom 1
    const auto pls = placements(panels_[0].aspect);
    const float arc = panel_arc();
    zoom_override_ = 0.0f;
    float half_w = 0, half_h = 0;
    for (size_t i = 0; i < pls.size() && i < static_cast<size_t>(kMaxPanels); ++i) {
        const float w = arc * pls[i].scale, h = core::panel_vertical_arc(w, panels_[i].aspect, true);
        // Both sides of straight ahead: the zoom scales about it.
        half_w = std::max(half_w, std::fabs(pls[i].yaw_rad) + w / 2);
        half_h = std::max(half_h, std::fabs(pls[i].pitch_rad) + h / 2);
    }
    if (half_w <= 0 || half_h <= 0) return 1.0f;
    const float fw = core::horizontal_fov(options.fov_y_deg * kDegToRad, options.eye_aspect);
    const float fh = options.fov_y_deg * kDegToRad;
    return std::clamp(0.92f * std::min(fw / (2 * half_w), fh / (2 * half_h)), 0.2f, 1.0f);
}

void SpatialRenderer::set_grow_panel(int panel) {
    if (panel == grow_panel_) return;
    grow_from_ = grow_panel_;
    grow_panel_ = panel;
    grow_start_ns_ = renderer_now_ns();
    grow_generation_ += 1;
}

bool SpatialRenderer::grow_animating() const {
    return grow_start_ns_ != 0 &&
           static_cast<double>(renderer_now_ns() - grow_start_ns_) * 1e-9 < kGrowSeconds + 0.05;  // and a frame at the end
}

// A panel fills the glasses' image: drawn flat, like the computer's own screen (as on macOS).
bool SpatialRenderer::filling_image() const {
    return options.static_head && (options.focus_panel >= 0 || grow_panel_ >= 0);
}

// Glasses in their own pinned mode anchor the image we send and let the eyes roam over it:
// nothing outside it can ever be seen. With a panel highlighted (the one you look at, or
// focus / cinema) that panel fills the whole image; the arrangement keeps its shape, so the
// others lie just beyond the edges, where a look picks them. With none, the whole arrangement
// is fitted in the image. Changing the highlighted panel pans and zooms there (macOS's
// arrangeInGlassesImage).
void SpatialRenderer::fit_in_glasses_image(std::vector<core::PanelPlacement>& out) const {
    if (options.focus_panel >= 0) {
        fill_glasses_image(out, options.focus_panel);
        return;
    }
    auto target = out;
    fill_glasses_image(target, grow_panel_);
    const double t = grow_start_ns_ == 0
                         ? 1.0
                         : std::min(1.0, static_cast<double>(renderer_now_ns() - grow_start_ns_) * 1e-9 / kGrowSeconds);
    if (t >= 1.0 || grow_from_ == grow_panel_) {
        out = target;
        return;
    }
    fill_glasses_image(out, grow_from_);
    // The pan eases in and out (cubic); the size settles with a slight overshoot, as if the
    // panel stepped forward.
    const float u = static_cast<float>(t);
    const float pan = u < 0.5f ? 4.0f * u * u * u : 1.0f - std::pow(-2.0f * u + 2.0f, 3.0f) / 2.0f;
    constexpr float c1 = 0.9f, c3 = c1 + 1.0f;  // ease-out-back, ~4 % overshoot
    const float grow = 1.0f + c3 * std::pow(u - 1.0f, 3.0f) + c1 * std::pow(u - 1.0f, 2.0f);
    for (size_t i = 0; i < out.size(); ++i) {
        out[i].yaw_rad += (target[i].yaw_rad - out[i].yaw_rad) * pan;
        out[i].pitch_rad += (target[i].pitch_rad - out[i].pitch_rad) * pan;
        out[i].scale += (target[i].scale - out[i].scale) * grow;
    }
}

// `panel` centred and scaled to fill the glasses' image (the others keep their place around
// it); -1 or out of range: everything fitted, zoomed by distance and size (core::pinned_zoom).
void SpatialRenderer::fill_glasses_image(std::vector<core::PanelPlacement>& out, int panel) const {
    const float arc = panel_arc();
    const bool one = panel >= 0 && static_cast<size_t>(panel) < out.size();
    float y0 = 1e9f, y1 = -1e9f, p0 = 1e9f, p1 = -1e9f;
    for (size_t i = 0; i < out.size() && i < static_cast<size_t>(kMaxPanels); ++i) {
        if (one && static_cast<int>(i) != panel) continue;
        const float w = arc * out[i].scale, h = core::panel_vertical_arc(w, panels_[i].aspect, true);
        y0 = std::min(y0, out[i].yaw_rad - w / 2);
        y1 = std::max(y1, out[i].yaw_rad + w / 2);
        p0 = std::min(p0, out[i].pitch_rad - h / 2);
        p1 = std::max(p1, out[i].pitch_rad + h / 2);
    }
    if (y1 <= y0 || p1 <= p0) return;
    const float fw = core::horizontal_fov(options.fov_y_deg * kDegToRad, options.eye_aspect);
    const float fh = options.fov_y_deg * kDegToRad;
    const float room = one ? 1.0f : 0.94f * core::pinned_zoom(arc);
    const float s = room * std::min(fw / (y1 - y0), fh / (p1 - p0));
    const float yc = 0.5f * (y0 + y1), pc = 0.5f * (p0 + p1);
    for (auto& pl : out) {
        pl.yaw_rad = (pl.yaw_rad - yc) * s;
        pl.pitch_rad = (pl.pitch_rad - pc) * s;
        pl.scale *= s;
    }
}

void SpatialRenderer::set_overlay_text(const std::wstring& text) {
    overlay_.Reset();
    overlay_generation_ += 1;
    if (text.empty()) return;
    // Dark card with white text, drawn with GDI into a 32-bit DIB (black would be see-through
    // on the glasses).
    constexpr int w = 1400, h = 360;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bmp == nullptr || bits == nullptr) {
        DeleteDC(dc);
        return;
    }
    HGDIOBJ old = SelectObject(dc, bmp);
    RECT all{0, 0, w, h};
    HBRUSH bg = CreateSolidBrush(RGB(33, 38, 41));
    FillRect(dc, &all, bg);
    DeleteObject(bg);
    SetBkMode(dc, TRANSPARENT);
    const size_t nl = text.find(L'\n');
    const std::wstring lines[2] = {text.substr(0, nl), nl == std::wstring::npos ? L"" : text.substr(nl + 1)};
    for (int i = 0; i < 2; ++i) {
        HFONT font = CreateFontW(i == 0 ? 76 : 50, 0, 0, 0, i == 0 ? FW_SEMIBOLD : FW_NORMAL, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                 DEFAULT_PITCH, L"Segoe UI");
        HGDIOBJ oldFont = SelectObject(dc, font);
        SetTextColor(dc, i == 0 ? RGB(255, 255, 255) : RGB(190, 196, 200));
        RECT r{0, i == 0 ? 70 : 200, w, i == 0 ? 170 : 280};
        DrawTextW(dc, lines[i].c_str(), static_cast<int>(lines[i].size()), &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, oldFont);
        DeleteObject(font);
    }
    GdiFlush();
    auto* px = static_cast<uint32_t*>(bits);
    for (int k = 0; k < w * h; ++k) px[k] |= 0xFF000000u;  // GDI leaves alpha undefined
    overlay_ = image(device_.Get(), w, h, px, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
    SelectObject(dc, old);
    DeleteObject(bmp);
    DeleteDC(dc);
    if (!overlay_vertices_) {
        // 20° wide card at 2 m, 4° below straight ahead, head-locked.
        core::ScreenMesh m =
            core::flat_screen_mesh(2.0f, 20.0f * kDegToRad, core::panel_width(2.0f, 20.0f * kDegToRad, true) * h / w, 1);
        core::pitch_mesh(m, -4.0f * kDegToRad);
        D3D11_BUFFER_DESC d{};
        d.Usage = D3D11_USAGE_IMMUTABLE;
        d.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        d.ByteWidth = static_cast<UINT>(m.vertices.size() * sizeof(core::ScreenVertex));
        D3D11_SUBRESOURCE_DATA init{m.vertices.data(), 0, 0};
        device_->CreateBuffer(&d, &init, &overlay_vertices_);
        d.BindFlags = D3D11_BIND_INDEX_BUFFER;
        d.ByteWidth = static_cast<UINT>(m.indices.size() * sizeof(uint16_t));
        init.pSysMem = m.indices.data();
        device_->CreateBuffer(&d, &init, &overlay_indices_);
        overlay_index_count_ = static_cast<UINT>(m.indices.size());
    }
}

float SpatialRenderer::fit_arc_deg(int columns) const {
    const float hfov = core::horizontal_fov(options.fov_y_deg * kDegToRad, options.eye_aspect);
    return core::fit_panel_arc(hfov, columns, options.gap_deg * kDegToRad, 1.0f * kDegToRad) / kDegToRad;
}

int SpatialRenderer::panel_at_gaze(const core::Quat& head) const {
    if (options.focus_panel >= 0 && options.focus_panel < count_) return options.focus_panel;
    const float aspect = panels_[0].aspect;
    return core::panel_at_gaze(core::track_axes(head, options.tracking_axes), placements(aspect), panel_arc(), vertical_arc(aspect),
                               options.gap_deg * kDegToRad);
}

bool SpatialRenderer::can_skip_frame(const core::Quat& orientation, const core::Vec3& angular_velocity, bool tracking,
                                     bool forced) {
    uint64_t generation = 0;
    for (int i = 0; i < count_; ++i) {
        if (panels_[i].source != nullptr) generation += panels_[i].source->generation();
    }
    core::FrameSkipper::Input in;
    in.orientation = options.static_head ? core::Quat{} : orientation;
    in.angular_speed_rads = options.static_head ? 0.0f : angular_velocity.norm();
    in.source_generation = generation + overlay_generation_ + depth_generation_ + grow_generation_ + cursor_generation_;
    in.tracking = tracking;
    const bool moving_background = background_ps_ && (options.background == 1 || options.background == 2 || options.background == 4);
    in.animating = dimmer_.animating() || depth_pending() || grow_animating() || moving_background;  // depth on its way, a pan / zoom, rain
    in.forced = forced;
    return skipper_.skip(in);
}

void SpatialRenderer::rebuild_mesh(Panel& p, const core::PanelPlacement& pl) {
    const float k = filling_image() ? 0.0f : std::clamp(options.curvature, 0.0f, 1.0f);
    if (p.vertices && p.aspect == p.mesh_aspect && options.distance_m == p.mesh_distance &&
        options.arc_deg == p.mesh_arc && pl.yaw_rad == p.mesh_center && pl.pitch_rad == p.mesh_pitch &&
        k == p.mesh_curvature && pl.scale == p.mesh_scale && options.tilt == p.mesh_tilt && options.zoom == p.mesh_zoom) {
        return;
    }
    const float arc = panel_arc() * pl.scale;
    const float width = core::panel_width(options.distance_m, arc, true);  // a sheet: bending keeps its width
    core::ScreenMesh mesh =
        core::bent_screen_mesh(options.distance_m, arc, width / p.aspect, k > 0.0f ? 64 : 1, k, pl.yaw_rad);
    core::place_pitched(mesh, pl.pitch_rad, options.tilt, options.distance_m);

    D3D11_BUFFER_DESC d{};
    d.Usage = D3D11_USAGE_IMMUTABLE;
    d.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    d.ByteWidth = static_cast<UINT>(mesh.vertices.size() * sizeof(core::ScreenVertex));
    D3D11_SUBRESOURCE_DATA init{mesh.vertices.data(), 0, 0};
    p.vertices.Reset();
    device_->CreateBuffer(&d, &init, &p.vertices);
    d.BindFlags = D3D11_BIND_INDEX_BUFFER;
    d.ByteWidth = static_cast<UINT>(mesh.indices.size() * sizeof(uint16_t));
    init.pSysMem = mesh.indices.data();
    p.indices.Reset();
    device_->CreateBuffer(&d, &init, &p.indices);
    p.index_count = static_cast<UINT>(mesh.indices.size());
    p.mesh_aspect = p.aspect;
    p.mesh_distance = options.distance_m;
    p.mesh_arc = options.arc_deg;
    p.mesh_center = pl.yaw_rad;
    p.mesh_pitch = pl.pitch_rad;
    p.mesh_curvature = k;
    p.mesh_scale = pl.scale;
    p.mesh_tilt = options.tilt;
    p.mesh_zoom = options.zoom;
}

void SpatialRenderer::compose(Panel& p) {
    if (p.source == nullptr) return;
    const FrameSource::Frame f = p.source->latest();
    if (!f.texture || f.generation == p.composed_generation) return;
    // The capture thread is writing it: keep the previous composite, retry next frame.
    if (f.mutex->AcquireSync(0, 0) != S_OK) return;
    const bool copied = compose_copy(p, f.texture.Get(), f.width, f.height);
    f.mutex->ReleaseSync(0);
    if (!copied) return;
    p.composed_generation = f.generation;
    compose_finish(p);
}

bool SpatialRenderer::compose_copy(Panel& p, ID3D11Texture2D* frame, int width, int height) {
    if (!p.composite || p.width != width || p.height != height) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = static_cast<UINT>(width);
        d.Height = static_cast<UINT>(height);
        d.MipLevels = 0;  // full chain
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_TYPELESS;  // UNORM for the cursor pass, sRGB for sampling
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        d.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
        p.composite.Reset();
        p.composite_srv.Reset();
        p.composite_rtv.Reset();
        if (FAILED(device_->CreateTexture2D(&d, nullptr, &p.composite))) return false;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = static_cast<UINT>(-1);
        device_->CreateShaderResourceView(p.composite.Get(), &sd, &p.composite_srv);
        D3D11_RENDER_TARGET_VIEW_DESC rd{};
        rd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        device_->CreateRenderTargetView(p.composite.Get(), &rd, &p.composite_rtv);
        p.width = width;
        p.height = height;
        p.aspect = height > 0 ? static_cast<float>(width) / static_cast<float>(height) : 16.0f / 9.0f;
    }
    ctx_->CopySubresourceRegion(p.composite.Get(), 0, 0, 0, 0, frame, 0, nullptr);
    return true;
}

void SpatialRenderer::compose_finish(Panel& p) { ctx_->GenerateMips(p.composite_srv.Get()); }

// The capture's pointer shape (Desktop Duplication hands it over apart), sRGB like the panels.
void SpatialRenderer::update_cursor_shape(Panel& p) {
    PointerShape shape;
    if (p.source == nullptr || !p.source->pointer_shape(p.shape_generation, &shape) || shape.width <= 0 ||
        shape.height <= 0) {
        return;
    }
    p.shape_generation = shape.generation;
    p.cursor_w = shape.width;
    p.cursor_h = shape.height;
    p.cursor_hot_x = shape.hot_x;
    p.cursor_hot_y = shape.hot_y;
    p.cursor_color = image(device_.Get(), shape.width, shape.height, shape.color.data(), DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
    const bool inverts = std::any_of(shape.invert.begin(), shape.invert.end(), [](uint32_t v) { return v != 0; });
    p.cursor_invert =
        inverts ? image(device_.Get(), shape.width, shape.height, shape.invert.data(), DXGI_FORMAT_B8G8R8A8_UNORM) : nullptr;
    if (p.cursor_visible) cursor_generation_ += 1;
}

// Over the panel just drawn (its mesh and PanelUniforms still bound).
void SpatialRenderer::draw_cursor(const Panel& p, ID3D11PixelShader* panel_ps) {
    if (!p.cursor_visible || !p.cursor_color || p.width <= 0 || p.height <= 0) return;
    const float x0 = static_cast<float>(p.cursor_x - p.cursor_hot_x), y0 = static_cast<float>(p.cursor_y - p.cursor_hot_y);
    const OverlayUniforms ou{{x0 / p.width, y0 / p.height, (x0 + p.cursor_w) / p.width, (y0 + p.cursor_h) / p.height}};
    ctx_->UpdateSubresource(overlay_cb_.Get(), 0, nullptr, &ou, 0, 0);
    ctx_->PSSetConstantBuffers(3, 1, overlay_cb_.GetAddressOf());
    ctx_->PSSetShader(cursor_ps_.Get(), nullptr, 0);
    ctx_->OMSetBlendState(blend_alpha_.Get(), nullptr, 0xFFFFFFFF);
    ctx_->PSSetShaderResources(0, 1, p.cursor_color.GetAddressOf());
    ctx_->DrawIndexed(p.index_count, 0, 0);
    if (p.cursor_invert) {
        ctx_->OMSetBlendState(blend_invert_.Get(), nullptr, 0xFFFFFFFF);
        ctx_->PSSetShaderResources(0, 1, p.cursor_invert.GetAddressOf());
        ctx_->DrawIndexed(p.index_count, 0, 0);
    }
    ctx_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    ctx_->PSSetShader(panel_ps, nullptr, 0);
}

// ---------------------------------------------------------------------------------------
// AI 3D
// ---------------------------------------------------------------------------------------

bool SpatialRenderer::depth_pending() const {
    if (depth_active_ < 0) return false;
    for (const auto& s : depth_slots_) {
        if (s.state != DepthSlot::State::Free) return true;
    }
    return false;
}

// Another panel or none: the frames in flight belong to the old one.
void SpatialRenderer::depth_reset() {
    for (auto& s : depth_slots_) {
        s.state = DepthSlot::State::Free;
        s.input = DepthInput{};
    }
    depth_valid_ = false;
    depth_video_[0] = depth_video_[1] = 0.0f;
    depth_video_[2] = depth_video_[3] = 1.0f;
    depth_captured_generation_ = ~0ull;
    if (depth_ != nullptr) {
        DepthOutput stale;
        depth_->take(&stale);
        depth_->reset_scene();
    }
}

bool SpatialRenderer::ensure_depth_slot(DepthSlot& s, int width, int height, int small_w, int small_h) {
    if (!s.color || s.width != width || s.height != height) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = static_cast<UINT>(width);
        d.Height = static_cast<UINT>(height);
        d.MipLevels = 0;  // full chain: shrunk to the model's size without aliasing
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_TYPELESS;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        d.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
        s.color.Reset();
        s.color_srv.Reset();
        if (FAILED(device_->CreateTexture2D(&d, nullptr, &s.color))) return false;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = static_cast<UINT>(-1);
        if (FAILED(device_->CreateShaderResourceView(s.color.Get(), &sd, &s.color_srv))) return false;
        s.width = width;
        s.height = height;
    }
    if (!s.staging || s.small_w != small_w || s.small_h != small_h) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = static_cast<UINT>(small_w);
        d.Height = static_cast<UINT>(small_h);
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_STAGING;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        s.staging.Reset();
        if (FAILED(device_->CreateTexture2D(&d, nullptr, &s.staging))) return false;
        s.small_w = small_w;
        s.small_h = small_h;
    }
    if (!depth_small_ || depth_small_w_ != small_w || depth_small_h_ != small_h) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = static_cast<UINT>(small_w);
        d.Height = static_cast<UINT>(small_h);
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_RENDER_TARGET;
        depth_small_.Reset();
        depth_small_rtv_.Reset();
        if (FAILED(device_->CreateTexture2D(&d, nullptr, &depth_small_))) return false;
        if (FAILED(device_->CreateRenderTargetView(depth_small_.Get(), nullptr, &depth_small_rtv_))) return false;
        depth_small_w_ = small_w;
        depth_small_h_ = small_h;
    }
    return true;
}

// A new frame of the depth panel into a free slot, shrunk to the model's input and copied for
// the CPU (read back on a later frame, without waiting for the GPU). One read-back at a time.
void SpatialRenderer::depth_capture(Panel& p) {
    if (p.source == nullptr) return;
    DepthSlot* slot = nullptr;
    for (auto& s : depth_slots_) {
        if (s.state == DepthSlot::State::Reading) return;
        if (s.state == DepthSlot::State::Free && slot == nullptr) slot = &s;
    }
    if (slot == nullptr) return;
    const FrameSource::Frame f = p.source->latest();
    if (!f.texture || f.generation == depth_captured_generation_ || f.width <= 0 || f.height <= 0) return;
    int sw = 0, sh = 0;
    depth_->input_size(&sw, &sh);
    if (!ensure_depth_slot(*slot, f.width, f.height, sw, sh)) return;
    if (f.mutex->AcquireSync(0, 0) != S_OK) return;
    ctx_->CopySubresourceRegion(slot->color.Get(), 0, 0, 0, 0, f.texture.Get(), 0, nullptr);
    f.mutex->ReleaseSync(0);
    depth_captured_generation_ = f.generation;
    ctx_->GenerateMips(slot->color_srv.Get());

    const DepthBlendUniforms u{0, 0, 0, std::max(0.0f, std::log2(static_cast<float>(f.width) / static_cast<float>(sw)))};
    ctx_->UpdateSubresource(depth_blend_cb_.Get(), 0, nullptr, &u, 0, 0);
    ID3D11RenderTargetView* rtv = depth_small_rtv_.Get();
    ctx_->OMSetRenderTargets(1, &rtv, nullptr);
    const D3D11_VIEWPORT vp{0, 0, static_cast<float>(sw), static_cast<float>(sh), 0, 1};
    ctx_->RSSetViewports(1, &vp);
    ctx_->RSSetState(raster_.Get());
    ctx_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    ctx_->IASetInputLayout(nullptr);
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx_->VSSetShader(fullscreen_vs_.Get(), nullptr, 0);
    ctx_->PSSetShader(downscale_ps_.Get(), nullptr, 0);
    ctx_->PSSetConstantBuffers(5, 1, depth_blend_cb_.GetAddressOf());
    ctx_->PSSetSamplers(1, 1, depth_sampler_.GetAddressOf());
    ctx_->PSSetShaderResources(0, 1, slot->color_srv.GetAddressOf());
    ctx_->Draw(3, 0);
    ID3D11ShaderResourceView* none = nullptr;
    ctx_->PSSetShaderResources(0, 1, &none);
    ctx_->OMSetRenderTargets(0, nullptr, nullptr);
    ctx_->CopyResource(slot->staging.Get(), depth_small_.Get());

    slot->pointer = f.pointer;
    slot->seq = ++depth_seq_;
    slot->state = DepthSlot::State::Reading;
}

// The engine's map, normalised to its 2-98 % range and blended into the smoothed one (or
// replacing it at a cut). R16F ping-pong.
void SpatialRenderer::blend_depth(const DepthOutput& out) {
    if (!depth_raw_ || depth_map_w_ != out.width || depth_map_h_ != out.height) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = static_cast<UINT>(out.width);
        d.Height = static_cast<UINT>(out.height);
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R32_FLOAT;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        depth_raw_.Reset();
        depth_raw_srv_.Reset();
        if (FAILED(device_->CreateTexture2D(&d, nullptr, &depth_raw_))) return;
        device_->CreateShaderResourceView(depth_raw_.Get(), nullptr, &depth_raw_srv_);
        d.Format = DXGI_FORMAT_R16_FLOAT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        const float zero[4] = {0, 0, 0, 0};
        for (int i = 0; i < 2; ++i) {
            depth_smooth_[i].Reset();
            depth_smooth_rtv_[i].Reset();
            depth_smooth_srv_[i].Reset();
            if (FAILED(device_->CreateTexture2D(&d, nullptr, &depth_smooth_[i]))) return;
            device_->CreateRenderTargetView(depth_smooth_[i].Get(), nullptr, &depth_smooth_rtv_[i]);
            device_->CreateShaderResourceView(depth_smooth_[i].Get(), nullptr, &depth_smooth_srv_[i]);
            ctx_->ClearRenderTargetView(depth_smooth_rtv_[i].Get(), zero);  // never NaN, even blended at alpha 1
        }
        depth_map_w_ = out.width;
        depth_map_h_ = out.height;
        depth_valid_ = false;
    }
    ctx_->UpdateSubresource(depth_raw_.Get(), 0, nullptr, out.depth.data(), static_cast<UINT>(out.width * 4), 0);
    const int next = 1 - depth_smooth_index_;
    const float alpha = out.scene_cut || !depth_valid_ ? 1.0f : std::clamp(options.depth_smoothing, 0.01f, 1.0f);
    const DepthBlendUniforms u{out.low, out.high, alpha, 0};
    ctx_->UpdateSubresource(depth_blend_cb_.Get(), 0, nullptr, &u, 0, 0);
    ID3D11RenderTargetView* rtv = depth_smooth_rtv_[next].Get();
    ctx_->OMSetRenderTargets(1, &rtv, nullptr);
    const D3D11_VIEWPORT vp{0, 0, static_cast<float>(out.width), static_cast<float>(out.height), 0, 1};
    ctx_->RSSetViewports(1, &vp);
    ctx_->RSSetState(raster_.Get());
    ctx_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    ctx_->IASetInputLayout(nullptr);
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx_->VSSetShader(fullscreen_vs_.Get(), nullptr, 0);
    ctx_->PSSetShader(depth_blend_ps_.Get(), nullptr, 0);
    ctx_->PSSetConstantBuffers(5, 1, depth_blend_cb_.GetAddressOf());
    ctx_->PSSetSamplers(1, 1, depth_sampler_.GetAddressOf());
    ID3D11ShaderResourceView* srvs[2] = {depth_raw_srv_.Get(), depth_smooth_srv_[depth_smooth_index_].Get()};
    ctx_->PSSetShaderResources(0, 2, srvs);
    ctx_->Draw(3, 0);
    ID3D11ShaderResourceView* none[2] = {nullptr, nullptr};
    ctx_->PSSetShaderResources(0, 2, none);
    ctx_->OMSetRenderTargets(0, nullptr, nullptr);
    depth_smooth_index_ = next;
    depth_valid_ = true;
}

// The depth panel's frame steps, once per rendered frame: a finished map shows its own frame,
// finished read-backs become ready, the newest ready one goes to the engine when it is free,
// and a new frame is taken.
void SpatialRenderer::depth_frame(Panel& p) {
    DepthOutput out;
    if (depth_->take(&out)) {
        for (auto& s : depth_slots_) {
            if (s.state != DepthSlot::State::Inferring || s.seq != out.seq) continue;
            blend_depth(out);
            for (int k = 0; k < 4; ++k) depth_video_[k] = out.video[k];
            if (compose_copy(p, s.color.Get(), s.width, s.height)) compose_finish(p);
            s.state = DepthSlot::State::Free;
            depth_generation_ += 1;
            depth_stats_.shown += 1;
            break;
        }
    }
    for (auto& s : depth_slots_) {
        if (s.state != DepthSlot::State::Reading) continue;
        D3D11_MAPPED_SUBRESOURCE m{};
        const HRESULT hr = ctx_->Map(s.staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) continue;
        if (FAILED(hr)) {
            s.state = DepthSlot::State::Free;
            continue;
        }
        s.input.seq = s.seq;
        s.input.width = s.small_w;
        s.input.height = s.small_h;
        const size_t row = static_cast<size_t>(s.small_w) * 4;
        s.input.bgra.resize(row * static_cast<size_t>(s.small_h));
        for (int y = 0; y < s.small_h; ++y) {
            std::memcpy(&s.input.bgra[row * static_cast<size_t>(y)], static_cast<const uint8_t*>(m.pData) + m.RowPitch * static_cast<size_t>(y), row);
        }
        ctx_->Unmap(s.staging.Get(), 0);
        s.state = DepthSlot::State::Ready;
    }
    // Only the newest ready frame is worth inferring.
    DepthSlot* newest = nullptr;
    for (auto& s : depth_slots_) {
        if (s.state == DepthSlot::State::Ready && (newest == nullptr || s.seq > newest->seq)) newest = &s;
    }
    for (auto& s : depth_slots_) {
        if (s.state == DepthSlot::State::Ready && &s != newest) {
            s.state = DepthSlot::State::Free;
            depth_stats_.dropped += 1;
        }
    }
    if (newest != nullptr && depth_->idle()) {
        // Refused: the engine stepped down to a smaller input meanwhile.
        newest->state = depth_->submit(std::move(newest->input)) ? DepthSlot::State::Inferring : DepthSlot::State::Free;
        newest->input = DepthInput{};
    }
    depth_capture(p);
}

void SpatialRenderer::ensure_eye_textures(int width, int height, bool mips) {
    if (eye_tex_[0] && eye_w_ == width && eye_h_ == height && eye_mips_ == mips) return;
    D3D11_TEXTURE2D_DESC d{};
    d.Width = static_cast<UINT>(width);
    d.Height = static_cast<UINT>(height);
    d.MipLevels = mips ? 0 : 1;  // supersampled: the full chain, generated each frame
    d.MiscFlags = mips ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    for (int i = 0; i < 2; ++i) {
        eye_tex_[i].Reset();
        eye_rtv_[i].Reset();
        eye_srv_[i].Reset();
        device_->CreateTexture2D(&d, nullptr, &eye_tex_[i]);
        device_->CreateRenderTargetView(eye_tex_[i].Get(), nullptr, &eye_rtv_[i]);
        device_->CreateShaderResourceView(eye_tex_[i].Get(), nullptr, &eye_srv_[i]);
    }
    eye_w_ = width;
    eye_h_ = height;
    eye_mips_ = mips;
}

float SpatialRenderer::render(ID3D11RenderTargetView* target, int width, int height, const core::Quat& orientation,
                              const core::Vec3& angular_velocity, uint64_t sample_ns, uint64_t target_ns) {
    // Pose prediction to the photon time.
    float horizon_s = 0.0f;
    if (sample_ns > 0 && target_ns > sample_ns) {
        horizon_s = std::min(static_cast<float>(target_ns - sample_ns) * 1e-9f, options.max_prediction_ms * 1e-3f);
    }
    // Extrapolating while the head holds still would only amplify noise: ramp the prediction
    // in with angular speed.
    horizon_s *= core::prediction_gain(angular_velocity.norm(), core::kPredictStartRads, core::kPredictFullRads);
    // Space at real size: a fixed motion-to-photon time along the gyro's instantaneous angular
    // velocity (zero while the stationary lock holds), no speed ramp.
    core::Vec3 wp = angular_velocity;
    if (options.fixed_prediction_ms > 0.0f && !options.static_head) {
        horizon_s = core::fixed_prediction_horizon_ms(options.fixed_prediction_ms, options.present_late_ms,
                                                      options.frame_period_ms, options.sample_age_ms) * 1e-3f;
        wp = core::without_roll(orientation, angular_velocity);  // PredXY: turning and nodding
    }
    core::Quat head = core::predict_orientation(orientation, wp, horizon_s);
    head = core::track_axes(head, options.tracking_axes);
    // No horizon lock: the head's roll is compensated in full, so the panels stay level with the
    // floor when the head tilts (taking the roll out made them lean with the head). A diagonal
    // drift when looking up / down comes from the sensor mounting (calibration), not from here.
    if (options.static_head) {  // the glasses' pinned mode moves the whole image: the panels stay put in it
        head = core::Quat{};
        horizon_s = 0.0f;
    }

    // AI 3D: the depth panel shows only frames whose depth map has come.
    const int depth_panel = depth_ != nullptr && depth_->state() == DepthEngine::State::Ready &&
                                    options.depth_panel >= 0 && options.depth_panel < count_
                                ? options.depth_panel
                                : -1;
    if (depth_panel != depth_active_) {
        depth_reset();
        if (depth_active_ >= 0) panels_[depth_active_].composed_generation = ~0ull;  // back to its live frames
        depth_active_ = depth_panel;
    }
    for (int i = 0; i < count_; ++i) {
        if (i == depth_active_) {
            depth_frame(panels_[i]);
        } else {
            compose(panels_[i]);
        }
        update_cursor_shape(panels_[i]);
    }
    // One layout for all panels (they share a size); per-panel aspect only shapes its mesh.
    const auto pls = placements(panels_[0].aspect);
    for (int i = 0; i < count_; ++i) rebuild_mesh(panels_[i], pls[static_cast<size_t>(i)]);

    // Panels you are not looking at fade to dim_others (core::PanelDimmer, shared with macOS).
    int gaze = -1;
    float dim = options.dim_others;
    if (count_ > 1 && options.dim_others < 1.0f) {
        gaze = core::panel_at_gaze(head, pls, panel_arc(), vertical_arc(panels_[0].aspect),
                                   options.gap_deg * kDegToRad);
    }
    // Glasses' pinned mode with a highlighted panel: the ones around it step back.
    if (options.static_head && count_ > 1 && grow_panel_ >= 0) {
        gaze = grow_panel_;
        dim = std::min(dim, kGrowDim);
    }
    const float dt = last_frame_ns_ > 0 && target_ns > last_frame_ns_
                         ? std::min(0.1f, static_cast<float>(target_ns - last_frame_ns_) * 1e-9f)
                         : 0.0f;
    last_frame_ns_ = target_ns;
    dimmer_.update(static_cast<size_t>(count_), gaze, dim, dt);
    for (int i = 0; i < count_; ++i) panels_[i].brightness = dimmer_.brightness(static_cast<size_t>(i));

    // Supersampling (space at real size): the eyes drawn larger, mipmapped down in the last pass.
    const int ss = options.static_head ? 1 : std::clamp(options.supersample, 1, 2);
    ensure_eye_textures(width * ss, height * ss, ss > 1);
    const int eyes = options.side_by_side ? 2 : 1;
    const core::Mat4 proj = core::perspective_lh(options.fov_y_deg * kDegToRad, options.eye_aspect, 0.05f, 100.0f);
    const float black[4] = {0, 0, 0, 1};  // black = see-through

    ctx_->RSSetState(raster_.Get());
    ctx_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    ctx_->IASetInputLayout(layout_.Get());
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx_->VSSetShader(screen_vs_.Get(), nullptr, 0);
    ctx_->VSSetConstantBuffers(0, 1, scene_cb_.GetAddressOf());
    // The space at real size: bicubic panels; the glasses' pinned mode as before.
    ID3D11PixelShader* panel_ps = !options.static_head && sharp_ps_ ? sharp_ps_.Get() : screen_ps_.Get();
    ctx_->PSSetShader(panel_ps, nullptr, 0);
    ctx_->PSSetConstantBuffers(1, 1, panel_cb_.GetAddressOf());
    ctx_->PSSetSamplers(0, 1, sampler_.GetAddressOf());
    const D3D11_VIEWPORT eye_vp{0, 0, static_cast<float>(width * ss), static_cast<float>(height * ss), 0, 1};
    // Background: the eye's axes in the room (the view's rotation), the same for both eyes.
    if (options.background == 5 && options.background_image != loaded_background_) load_background(options.background_image);
    const bool draw_background = background_ps_ && options.background > 0 && options.background <= 5 &&
                                 (options.background != 5 || background_srv_);
    if (draw_background) {
        const core::Mat4 r = core::render_rotation(head);
        BackgroundUniforms bu{};
        for (int k = 0; k < 3; ++k) {
            bu.right[k] = r.at(k, 0);
            bu.up[k] = r.at(k, 1);
            bu.forward[k] = r.at(k, 2);
        }
        bu.tan_y = std::tan(options.fov_y_deg * kDegToRad * 0.5f);
        bu.tan_x = bu.tan_y * options.eye_aspect;
        bu.time = static_cast<float>(std::fmod(
            std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(), 3600.0));
        bu.brightness = std::clamp(options.background_brightness, 0.0f, 1.0f);
        bu.style = options.background;
        bu.image_aspect = background_aspect_;
        ctx_->UpdateSubresource(background_cb_.Get(), 0, nullptr, &bu, 0, 0);
    }
    for (int eye = 0; eye < eyes; ++eye) {
        ID3D11RenderTargetView* rtv = eye_rtv_[eye].Get();
        ctx_->OMSetRenderTargets(1, &rtv, nullptr);
        ctx_->RSSetViewports(1, &eye_vp);
        ctx_->ClearRenderTargetView(rtv, black);
        ctx_->PSSetShader(panel_ps, nullptr, 0);  // the previous eye ended with the overlay's
        if (draw_background) {
            ctx_->IASetInputLayout(nullptr);
            ctx_->VSSetShader(fullscreen_vs_.Get(), nullptr, 0);
            ctx_->PSSetShader(background_ps_.Get(), nullptr, 0);
            ctx_->PSSetConstantBuffers(6, 1, background_cb_.GetAddressOf());
            ID3D11ShaderResourceView* bg = background_srv_ ? background_srv_.Get() : placeholder_.Get();
            ctx_->PSSetShaderResources(2, 1, &bg);
            ctx_->PSSetSamplers(2, 1, mirror_sampler_.GetAddressOf());
            ctx_->Draw(3, 0);
            ctx_->IASetInputLayout(layout_.Get());
            ctx_->VSSetShader(screen_vs_.Get(), nullptr, 0);
            ctx_->PSSetShader(panel_ps, nullptr, 0);
        }
        const float offset = options.side_by_side ? (eye == 0 ? -0.5f : 0.5f) * options.ipd_m : 0.0f;
        SceneUniforms su;
        const float up = options.neck_model ? core::kNeckUpM : 0.0f, fwd = options.neck_model ? core::kNeckForwardM : 0.0f;
        std::memcpy(su.view_projection, (proj * core::view_matrix(head, offset, up, fwd)).m, sizeof(su.view_projection));
        ctx_->UpdateSubresource(scene_cb_.Get(), 0, nullptr, &su, 0, 0);
        for (int i = 0; i < count_; ++i) {
            if (options.focus_panel >= 0 && i != options.focus_panel) continue;  // focus / cinema
            const Panel& p = panels_[i];
            if (!p.vertices || !p.indices) continue;
            const PanelUniforms pu{p.brightness, {0, 0, 0}};
            ctx_->UpdateSubresource(panel_cb_.Get(), 0, nullptr, &pu, 0, 0);
            const UINT stride = sizeof(core::ScreenVertex), offset0 = 0;
            ctx_->IASetVertexBuffers(0, 1, p.vertices.GetAddressOf(), &stride, &offset0);
            ctx_->IASetIndexBuffer(p.indices.Get(), DXGI_FORMAT_R16_UINT, 0);
            ID3D11ShaderResourceView* srv = p.composite_srv ? p.composite_srv.Get() : placeholder_.Get();
            ctx_->PSSetShaderResources(0, 1, &srv);
            // AI 3D: only with one image per eye (in mono there is no depth to show).
            const bool deep = i == depth_active_ && depth_valid_ && options.side_by_side && p.composite_srv;
            if (deep) {
                const StereoUniforms st{p.brightness, eye == 0 ? -1.0f : 1.0f, options.depth_strength,
                                        std::clamp(options.depth_convergence, 0.0f, 1.0f), kDepthEdge, {0, 0, 0},
                                        {depth_video_[0], depth_video_[1], depth_video_[2], depth_video_[3]}};
                ctx_->UpdateSubresource(stereo_cb_.Get(), 0, nullptr, &st, 0, 0);
                ctx_->PSSetShader(stereo_ps_.Get(), nullptr, 0);
                ctx_->PSSetConstantBuffers(4, 1, stereo_cb_.GetAddressOf());
                ctx_->PSSetSamplers(1, 1, depth_sampler_.GetAddressOf());
                ctx_->PSSetShaderResources(1, 1, depth_smooth_srv_[depth_smooth_index_].GetAddressOf());
            }
            ctx_->DrawIndexed(p.index_count, 0, 0);
            if (deep) {
                ID3D11ShaderResourceView* none = nullptr;
                ctx_->PSSetShaderResources(1, 1, &none);
                ctx_->PSSetShader(panel_ps, nullptr, 0);
            }
            draw_cursor(p, panel_ps);
        }
        if (overlay_ && overlay_vertices_) {
            ctx_->PSSetShader(screen_ps_.Get(), nullptr, 0);
            // Head-locked: straight ahead whatever the pose.
            SceneUniforms ou;
            std::memcpy(ou.view_projection, (proj * core::view_matrix(core::Quat{}, offset)).m, sizeof(ou.view_projection));
            ctx_->UpdateSubresource(scene_cb_.Get(), 0, nullptr, &ou, 0, 0);
            const PanelUniforms pu{1.0f, {0, 0, 0}};
            ctx_->UpdateSubresource(panel_cb_.Get(), 0, nullptr, &pu, 0, 0);
            const UINT stride = sizeof(core::ScreenVertex), offset0 = 0;
            ctx_->IASetVertexBuffers(0, 1, overlay_vertices_.GetAddressOf(), &stride, &offset0);
            ctx_->IASetIndexBuffer(overlay_indices_.Get(), DXGI_FORMAT_R16_UINT, 0);
            ctx_->PSSetShaderResources(0, 1, overlay_.GetAddressOf());
            ctx_->DrawIndexed(overlay_index_count_, 0, 0);
        }
    }

    if (ss > 1) {
        ctx_->OMSetRenderTargets(0, nullptr, nullptr);  // unbound before their mips are made
        for (int eye = 0; eye < eyes; ++eye) ctx_->GenerateMips(eye_srv_[eye].Get());
    }
    ctx_->OMSetRenderTargets(1, &target, nullptr);
    ctx_->ClearRenderTargetView(target, black);
    const DistortUniforms du{options.k1, options.k2, options.eye_aspect, 0};
    ctx_->UpdateSubresource(distort_cb_.Get(), 0, nullptr, &du, 0, 0);
    ctx_->IASetInputLayout(nullptr);
    ctx_->VSSetShader(fullscreen_vs_.Get(), nullptr, 0);
    ctx_->PSSetShader(distort_ps_.Get(), nullptr, 0);
    ctx_->PSSetConstantBuffers(2, 1, distort_cb_.GetAddressOf());
    const float half = static_cast<float>(width) / static_cast<float>(eyes);
    for (int eye = 0; eye < eyes; ++eye) {
        const D3D11_VIEWPORT vp{half * static_cast<float>(eye), 0, half, static_cast<float>(height), 0, 1};
        ctx_->RSSetViewports(1, &vp);
        ctx_->PSSetShaderResources(0, 1, eye_srv_[eye].GetAddressOf());
        ctx_->Draw(3, 0);
    }
    // Unbind: next frame writes the eye textures and the composites again.
    ID3D11ShaderResourceView* none = nullptr;
    ctx_->PSSetShaderResources(0, 1, &none);
    ctx_->OMSetRenderTargets(0, nullptr, nullptr);
    return horizon_s * 1e3f;
}

// The picture for background style 5 through WIC (any format Windows decodes), at most 4096 px
// on the long side, as sRGB. Unreadable: none (and not retried until the path changes).
void SpatialRenderer::load_background(const std::string& path) {
    loaded_background_ = path;
    background_srv_.Reset();
    if (path.empty()) return;
    const int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring wpath(n > 0 ? static_cast<size_t>(n) : 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wpath.data(), n);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);  // already initialised: harmless
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateDecoderFromFilename(wpath.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame))) {
        return;
    }
    UINT w = 0, h = 0;
    frame->GetSize(&w, &h);
    if (w == 0 || h == 0) return;
    ComPtr<IWICBitmapSource> source = frame;
    const UINT longest = std::max(w, h);
    if (longest > 4096) {
        ComPtr<IWICBitmapScaler> scaler;
        const UINT sw = std::max(1u, w * 4096 / longest), sh = std::max(1u, h * 4096 / longest);
        if (SUCCEEDED(factory->CreateBitmapScaler(&scaler)) &&
            SUCCEEDED(scaler->Initialize(frame.Get(), sw, sh, WICBitmapInterpolationModeFant))) {
            source = scaler;
            w = sw;
            h = sh;
        }
    }
    if (FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(source.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom))) {
        return;
    }
    std::vector<uint32_t> pixels(static_cast<size_t>(w) * h);
    if (FAILED(converter->CopyPixels(nullptr, w * 4, static_cast<UINT>(pixels.size() * 4),
                                     reinterpret_cast<BYTE*>(pixels.data())))) {
        return;
    }
    background_srv_ = image(device_.Get(), static_cast<int>(w), static_cast<int>(h), pixels.data(), DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
    background_aspect_ = static_cast<float>(w) / static_cast<float>(h);
}

}  // namespace rayneo::win
