#include "spatial_renderer.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <cfloat>
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

bool compile(const char* entry, const char* target, ComPtr<ID3DBlob>* out, std::string* error) {
    ComPtr<ID3DBlob> errors;
    const HRESULT hr = D3DCompile(kRNShaderSourceHlsl, std::strlen(kRNShaderSourceHlsl), "Shaders.hlsl", nullptr,
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

    scene_cb_ = constant_buffer(device_.Get(), sizeof(SceneUniforms));
    panel_cb_ = constant_buffer(device_.Get(), sizeof(PanelUniforms));
    distort_cb_ = constant_buffer(device_.Get(), sizeof(DistortUniforms));
    overlay_cb_ = constant_buffer(device_.Get(), sizeof(OverlayUniforms));

    D3D11_SAMPLER_DESC s{};
    s.Filter = D3D11_FILTER_ANISOTROPIC;  // trilinear + anisotropic: crisp, stable text
    s.MaxAnisotropy = 16;
    s.AddressU = s.AddressV = s.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;  // clamp to zero
    s.ComparisonFunc = D3D11_COMPARISON_NEVER;
    s.MaxLOD = FLT_MAX;
    device_->CreateSamplerState(&s, &sampler_);

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
    if (!sampler_ || !raster_ || !blend_alpha_ || !blend_invert_ || !placeholder_ || !scene_cb_) {
        if (error != nullptr) *error = "D3D11 state creation failed";
        return false;
    }
    return true;
}

void SpatialRenderer::set_panel_count(int count) { count_ = std::clamp(count, 1, kMaxPanels); }

void SpatialRenderer::set_source(int panel, FrameSource* source) {
    if (panel < 0 || panel >= kMaxPanels) return;
    panels_[panel].source = source;
    panels_[panel].composed_generation = ~0ull;
}

// The panel arc as seen at the current distance (closer looks bigger, like a real monitor).
float SpatialRenderer::panel_arc() const {
    return core::arc_at_distance(options.arc_deg * kDegToRad, options.distance_m);
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
    // Cinema: the focused panel straight ahead, large (the others are not drawn).
    if (options.cinema && options.focus_panel >= 0 && options.focus_panel < static_cast<int>(out.size())) {
        out[static_cast<size_t>(options.focus_panel)] = core::PanelPlacement{0.0f, 0.0f, 1.6f};
    }
    if (options.static_head) fit_in_glasses_image(out);
    return out;
}

// Glasses in their own pinned mode anchor the image we send and let the eyes roam over it:
// nothing outside it can ever be seen. The whole arrangement (monitors and floating windows,
// both axes; only the focused panel in focus / cinema) is centred and scaled to fit in it.
// Distance and size zoom along the whole of their range, never past the image (see
// core::pinned_zoom): the largest and closest fills it.
void SpatialRenderer::fit_in_glasses_image(std::vector<core::PanelPlacement>& out) const {
    const float arc = panel_arc();
    float y0 = 1e9f, y1 = -1e9f, p0 = 1e9f, p1 = -1e9f;
    for (size_t i = 0; i < out.size() && i < static_cast<size_t>(kMaxPanels); ++i) {
        if (options.focus_panel >= 0 && static_cast<int>(i) != options.focus_panel) continue;
        const float w = arc * out[i].scale, h = core::panel_vertical_arc(w, panels_[i].aspect, true);
        y0 = std::min(y0, out[i].yaw_rad - w / 2);
        y1 = std::max(y1, out[i].yaw_rad + w / 2);
        p0 = std::min(p0, out[i].pitch_rad - h / 2);
        p1 = std::max(p1, out[i].pitch_rad + h / 2);
    }
    if (y1 <= y0 || p1 <= p0) return;
    const float fw = core::horizontal_fov(options.fov_y_deg * kDegToRad, options.eye_aspect);
    const float fh = options.fov_y_deg * kDegToRad;
    const float zoom = core::pinned_zoom(arc);
    const float s = 0.94f * std::min(fw * zoom / (y1 - y0), fh * zoom / (p1 - p0));
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
    in.source_generation = generation + overlay_generation_;
    in.tracking = tracking;
    in.animating = dimmer_.animating();
    in.forced = forced;
    return skipper_.skip(in);
}

void SpatialRenderer::rebuild_mesh(Panel& p, const core::PanelPlacement& pl) {
    const float k = std::clamp(options.curvature, 0.0f, 1.0f);
    if (p.vertices && p.aspect == p.mesh_aspect && options.distance_m == p.mesh_distance &&
        options.arc_deg == p.mesh_arc && pl.yaw_rad == p.mesh_center && pl.pitch_rad == p.mesh_pitch &&
        k == p.mesh_curvature && pl.scale == p.mesh_scale) {
        return;
    }
    const float arc = panel_arc() * pl.scale;
    const float width = core::panel_width(options.distance_m, arc, true);  // a sheet: bending keeps its width
    core::ScreenMesh mesh =
        core::bent_screen_mesh(options.distance_m, arc, width / p.aspect, k > 0.0f ? 64 : 1, k, pl.yaw_rad);
    core::pitch_mesh(mesh, pl.pitch_rad);

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
}

void SpatialRenderer::compose(Panel& p) {
    if (p.source == nullptr) return;
    const FrameSource::Frame f = p.source->latest();
    if (!f.texture || f.generation == p.composed_generation) return;
    // The capture thread is writing it: keep the previous composite, retry next frame.
    if (f.mutex->AcquireSync(0, 0) != S_OK) return;

    if (!p.composite || p.width != f.width || p.height != f.height) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = static_cast<UINT>(f.width);
        d.Height = static_cast<UINT>(f.height);
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
        if (FAILED(device_->CreateTexture2D(&d, nullptr, &p.composite))) {
            f.mutex->ReleaseSync(0);
            return;
        }
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = static_cast<UINT>(-1);
        device_->CreateShaderResourceView(p.composite.Get(), &sd, &p.composite_srv);
        D3D11_RENDER_TARGET_VIEW_DESC rd{};
        rd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        device_->CreateRenderTargetView(p.composite.Get(), &rd, &p.composite_rtv);
        p.width = f.width;
        p.height = f.height;
        p.aspect = f.height > 0 ? static_cast<float>(f.width) / static_cast<float>(f.height) : 16.0f / 9.0f;
    }
    ctx_->CopySubresourceRegion(p.composite.Get(), 0, 0, 0, 0, f.texture.Get(), 0, nullptr);
    f.mutex->ReleaseSync(0);
    p.composed_generation = f.generation;

    PointerShape shape;
    if (p.source->pointer_shape(p.shape_generation, &shape) && shape.width > 0 && shape.height > 0) {
        p.shape_generation = shape.generation;
        p.cursor_w = shape.width;
        p.cursor_h = shape.height;
        p.cursor_color = image(device_.Get(), shape.width, shape.height, shape.color.data(), DXGI_FORMAT_B8G8R8A8_UNORM);
        const bool inverts = std::any_of(shape.invert.begin(), shape.invert.end(), [](uint32_t v) { return v != 0; });
        p.cursor_invert = inverts ? image(device_.Get(), shape.width, shape.height, shape.invert.data(),
                                          DXGI_FORMAT_B8G8R8A8_UNORM)
                                  : nullptr;
    }
    if (f.pointer.visible && p.cursor_color && p.composite_rtv) {
        const OverlayUniforms ou{{static_cast<float>(f.pointer.x) / p.width, static_cast<float>(f.pointer.y) / p.height,
                                  static_cast<float>(f.pointer.x + p.cursor_w) / p.width,
                                  static_cast<float>(f.pointer.y + p.cursor_h) / p.height}};
        ctx_->UpdateSubresource(overlay_cb_.Get(), 0, nullptr, &ou, 0, 0);
        ID3D11RenderTargetView* rtv = p.composite_rtv.Get();
        ctx_->OMSetRenderTargets(1, &rtv, nullptr);
        const D3D11_VIEWPORT vp{0, 0, static_cast<float>(p.width), static_cast<float>(p.height), 0, 1};
        ctx_->RSSetViewports(1, &vp);
        ctx_->RSSetState(raster_.Get());
        ctx_->IASetInputLayout(nullptr);
        ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        ctx_->VSSetShader(overlay_vs_.Get(), nullptr, 0);
        ctx_->VSSetConstantBuffers(3, 1, overlay_cb_.GetAddressOf());
        ctx_->PSSetShader(overlay_ps_.Get(), nullptr, 0);
        ctx_->PSSetSamplers(0, 1, sampler_.GetAddressOf());
        ctx_->OMSetBlendState(blend_alpha_.Get(), nullptr, 0xFFFFFFFF);
        ctx_->PSSetShaderResources(0, 1, p.cursor_color.GetAddressOf());
        ctx_->Draw(4, 0);
        if (p.cursor_invert) {
            ctx_->OMSetBlendState(blend_invert_.Get(), nullptr, 0xFFFFFFFF);
            ctx_->PSSetShaderResources(0, 1, p.cursor_invert.GetAddressOf());
            ctx_->Draw(4, 0);
        }
        ctx_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
        ctx_->OMSetRenderTargets(0, nullptr, nullptr);
    }
    ctx_->GenerateMips(p.composite_srv.Get());
}

void SpatialRenderer::ensure_eye_textures(int width, int height) {
    if (eye_tex_[0] && eye_w_ == width && eye_h_ == height) return;
    D3D11_TEXTURE2D_DESC d{};
    d.Width = static_cast<UINT>(width);
    d.Height = static_cast<UINT>(height);
    d.MipLevels = 1;
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
    core::Quat head = core::predict_orientation(orientation, angular_velocity, horizon_s);
    head = core::track_axes(head, options.tracking_axes);
    if (options.static_head) {  // the glasses' pinned mode moves the whole image: the panels stay put in it
        head = core::Quat{};
        horizon_s = 0.0f;
    }

    for (int i = 0; i < count_; ++i) compose(panels_[i]);
    // One layout for all panels (they share a size); per-panel aspect only shapes its mesh.
    const auto pls = placements(panels_[0].aspect);
    for (int i = 0; i < count_; ++i) rebuild_mesh(panels_[i], pls[static_cast<size_t>(i)]);

    // Panels you are not looking at fade to dim_others (core::PanelDimmer, shared with macOS).
    int gaze = -1;
    if (count_ > 1 && options.dim_others < 1.0f) {
        gaze = core::panel_at_gaze(head, pls, panel_arc(), vertical_arc(panels_[0].aspect),
                                   options.gap_deg * kDegToRad);
    }
    const float dt = last_frame_ns_ > 0 && target_ns > last_frame_ns_
                         ? std::min(0.1f, static_cast<float>(target_ns - last_frame_ns_) * 1e-9f)
                         : 0.0f;
    last_frame_ns_ = target_ns;
    dimmer_.update(static_cast<size_t>(count_), gaze, options.dim_others, dt);
    for (int i = 0; i < count_; ++i) panels_[i].brightness = dimmer_.brightness(static_cast<size_t>(i));

    ensure_eye_textures(width, height);
    const int eyes = options.side_by_side ? 2 : 1;
    const core::Mat4 proj = core::perspective_lh(options.fov_y_deg * kDegToRad, options.eye_aspect, 0.05f, 100.0f);
    const float black[4] = {0, 0, 0, 1};  // black = see-through

    ctx_->RSSetState(raster_.Get());
    ctx_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    ctx_->IASetInputLayout(layout_.Get());
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx_->VSSetShader(screen_vs_.Get(), nullptr, 0);
    ctx_->VSSetConstantBuffers(0, 1, scene_cb_.GetAddressOf());
    ctx_->PSSetShader(screen_ps_.Get(), nullptr, 0);
    ctx_->PSSetConstantBuffers(1, 1, panel_cb_.GetAddressOf());
    ctx_->PSSetSamplers(0, 1, sampler_.GetAddressOf());
    const D3D11_VIEWPORT eye_vp{0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
    for (int eye = 0; eye < eyes; ++eye) {
        ID3D11RenderTargetView* rtv = eye_rtv_[eye].Get();
        ctx_->OMSetRenderTargets(1, &rtv, nullptr);
        ctx_->RSSetViewports(1, &eye_vp);
        ctx_->ClearRenderTargetView(rtv, black);
        const float offset = options.side_by_side ? (eye == 0 ? -0.5f : 0.5f) * options.ipd_m : 0.0f;
        SceneUniforms su;
        std::memcpy(su.view_projection, (proj * core::view_matrix(head, offset)).m, sizeof(su.view_projection));
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
            ctx_->DrawIndexed(p.index_count, 0, 0);
        }
        if (overlay_ && overlay_vertices_) {
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

}  // namespace rayneo::win
