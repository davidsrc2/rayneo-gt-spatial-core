// Windows renderer smoke test on WARP (software D3D11, available on CI runners): the HLSL
// compiles, the pipeline builds, and a frame looking straight ahead puts the panel in the
// middle of both eyes and leaves the corners black (see-through).

#include <windows.h>

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdio>
#include <string>

#include "spatial_renderer.h"

using Microsoft::WRL::ComPtr;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

}  // namespace

int main() {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                 D3D11_SDK_VERSION, &device, nullptr, &ctx))) {
        std::printf("WARP device unavailable: skipped\n");
        return 0;
    }

    rayneo::win::SpatialRenderer renderer;
    std::string error;
    const bool init = renderer.init(device.Get(), &error);
    check(init, "renderer init (shaders compile)");
    if (!init) {
        std::printf("  %s\n", error.c_str());
        return 1;
    }

    constexpr int w = 640, h = 180;  // half-SBS: two 320x180 eyes
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w;
    d.Height = h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> target;
    device->CreateTexture2D(&d, nullptr, &target);
    D3D11_RENDER_TARGET_VIEW_DESC rd{};
    rd.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;  // as the swap chain view
    rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11RenderTargetView> rtv;
    device->CreateRenderTargetView(target.Get(), &rd, &rtv);

    renderer.options.side_by_side = true;
    renderer.options.arc_deg = 30;  // smaller than the ~42 deg horizontal FOV: corners stay empty
    renderer.render(rtv.Get(), w, h, rayneo::core::Quat{}, rayneo::core::Vec3{}, 0, 0);

    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    device->CreateTexture2D(&d, nullptr, &staging);
    ctx->CopyResource(staging.Get(), target.Get());
    D3D11_MAPPED_SUBRESOURCE m{};
    check(SUCCEEDED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m)), "read back");
    auto pixel = [&](int x, int y) {
        return *reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(m.pData) + y * m.RowPitch + x * 4) &
               0x00FFFFFFu;
    };
    // The placeholder panel (dark grey, no capture yet) covers each eye's centre.
    check(pixel(w / 4, h / 2) != 0, "left eye: panel in the centre");
    check(pixel(3 * w / 4, h / 2) != 0, "right eye: panel in the centre");
    check(pixel(0, 0) == 0 && pixel(w - 1, h - 1) == 0, "corners black (see-through)");
    std::printf("centre 0x%06x\n", pixel(w / 4, h / 2));
    ctx->Unmap(staging.Get(), 0);

    // A head turned 90 degrees away leaves the panel out of view.
    const rayneo::core::Quat away = rayneo::core::Quat::from_axis_angle({0, 0, 1}, 1.5707963f);
    renderer.render(rtv.Get(), w, h, away, rayneo::core::Vec3{}, 0, 0);
    ctx->CopyResource(staging.Get(), target.Get());
    ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m);
    check(pixel(w / 4, h / 2) == 0, "head turned away: panel out of view");
    ctx->Unmap(staging.Get(), 0);

    return failures == 0 ? 0 : 1;
}
