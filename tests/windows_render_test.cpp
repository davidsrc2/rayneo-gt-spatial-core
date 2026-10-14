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
ComPtr<ID3D11InfoQueue> info;

void dump_messages() {
    if (!info) return;
    for (UINT64 i = 0; i < info->GetNumStoredMessages(); ++i) {
        SIZE_T len = 0;
        info->GetMessage(i, nullptr, &len);
        std::string buf(len, '\0');
        auto* msg = reinterpret_cast<D3D11_MESSAGE*>(buf.data());
        if (SUCCEEDED(info->GetMessage(i, msg, &len))) std::printf("  d3d: %.*s\n", static_cast<int>(msg->DescriptionByteLength), msg->pDescription);
    }
    info->ClearStoredMessages();
}

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

// SEH: a crash inside the driver or debug layer prints its code and the layer's messages.
bool guarded_render(rayneo::win::SpatialRenderer& r, ID3D11RenderTargetView* rtv, int w, int h,
                    const rayneo::core::Quat& q) {
    __try {
        r.render(rtv, w, h, q, rayneo::core::Vec3{}, 0, 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        std::printf("FAIL render raised exception 0x%08lx\n", GetExceptionCode());
        return false;
    }
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // keep the output if it crashes
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    // Debug layer when installed (Graphics Tools): its messages explain a failing check.
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                                 D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_DEBUG, nullptr, 0,
                                 D3D11_SDK_VERSION, &device, nullptr, &ctx)) &&
        FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                 D3D11_SDK_VERSION, &device, nullptr, &ctx))) {
        std::printf("WARP device unavailable: skipped\n");
        return 0;
    }

    device.As(&info);
    std::printf("debug layer: %s\n", info ? "on" : "off");

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
    d.Format = DXGI_FORMAT_B8G8R8A8_TYPELESS;  // a swap chain buffer takes an sRGB view as is; a texture must be typeless
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
    dump_messages();
    if (!guarded_render(renderer, rtv.Get(), w, h, rayneo::core::Quat{})) {
        dump_messages();
        return 1;
    }

    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
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
    int lit = 0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) lit += pixel(x, y) != 0;
    }
    std::printf("centre 0x%06x, %d of %d pixels lit\n", pixel(w / 4, h / 2), lit, w * h);
    dump_messages();
    ctx->Unmap(staging.Get(), 0);

    // A head turned 90 degrees away leaves the panel out of view.
    const rayneo::core::Quat away = rayneo::core::Quat::from_axis_angle({0, 0, 1}, 1.5707963f);
    if (!guarded_render(renderer, rtv.Get(), w, h, away)) {
        dump_messages();
        return 1;
    }
    ctx->CopyResource(staging.Get(), target.Get());
    ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m);
    check(pixel(w / 4, h / 2) == 0, "head turned away: panel out of view");
    ctx->Unmap(staging.Get(), 0);

    return failures == 0 ? 0 : 1;
}
