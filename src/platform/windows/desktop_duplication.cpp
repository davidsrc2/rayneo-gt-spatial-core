#include "desktop_duplication.h"

#include <chrono>
#include <cstdio>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace rayneo::win {

DesktopDuplication::~DesktopDuplication() { stop(); }

bool DesktopDuplication::start(IDXGIAdapter* adapter, IDXGIOutput1* output, ID3D11Device* render_device,
                               std::string* error) {
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                        D3D_FEATURE_LEVEL_10_0};
    const HRESULT hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                         levels, 4, D3D11_SDK_VERSION, &device_, nullptr, &context_);
    if (FAILED(hr)) {
        if (error != nullptr) *error = "capture device creation failed";
        return false;
    }
    render_device_ = render_device;
    output_ = output;
    // The render device on another adapter: no GPU sharing between them, a CPU copy instead.
    {
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> render_adapter;
        DXGI_ADAPTER_DESC a{}, b{};
        if (SUCCEEDED(render_device->QueryInterface(IID_PPV_ARGS(&dxgi))) && SUCCEEDED(dxgi->GetAdapter(&render_adapter)) &&
            SUCCEEDED(render_adapter->GetDesc(&a)) && SUCCEEDED(adapter->GetDesc(&b))) {
            cross_ = a.AdapterLuid.LowPart != b.AdapterLuid.LowPart || a.AdapterLuid.HighPart != b.AdapterLuid.HighPart;
        }
    }
    // The first duplication is opened here so the caller learns about failures (wrong
    // adapter, secure desktop); later ones (mode change, UAC prompt) reopen on the thread.
    if (!open(error)) return false;
    running_ = true;
    thread_ = std::thread([this] { run(); });
    return true;
}

void DesktopDuplication::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
    dup_.Reset();
}

bool DesktopDuplication::open(std::string* error) {
    dup_.Reset();
    const HRESULT hr = output_->DuplicateOutput(device_.Get(), &dup_);
    if (FAILED(hr)) {
        if (error != nullptr) {
            char buf[160];
            snprintf(buf, sizeof(buf), "DuplicateOutput failed (0x%08lx)%s", static_cast<unsigned long>(hr),
                     hr == DXGI_ERROR_UNSUPPORTED ? ": the capture device is not on this output's GPU" : "");
            *error = buf;
        }
        return false;
    }
    return true;
}

FrameSource::Frame DesktopDuplication::latest() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (cross_) {
        // Uploaded into a texture of the render device (keyed mutex too, so the renderer's
        // AcquireSync / ReleaseSync work the same).
        if (width_ > 0 && height_ > 0) {
            D3D11_TEXTURE2D_DESC have{};
            if (render_texture_) render_texture_->GetDesc(&have);
            if (!render_texture_ || have.Width != static_cast<UINT>(width_) || have.Height != static_cast<UINT>(height_)) {
                D3D11_TEXTURE2D_DESC d{};
                d.Width = static_cast<UINT>(width_);
                d.Height = static_cast<UINT>(height_);
                d.MipLevels = 1;
                d.ArraySize = 1;
                d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                d.SampleDesc.Count = 1;
                d.Usage = D3D11_USAGE_DEFAULT;
                d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                d.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
                render_texture_.Reset();
                render_mutex_.Reset();
                if (SUCCEEDED(render_device_->CreateTexture2D(&d, nullptr, &render_texture_))) render_texture_.As(&render_mutex_);
                cpu_dirty_ = true;
            }
            if (cpu_dirty_ && render_mutex_ && cpu_frame_.size() >= static_cast<size_t>(width_) * height_ * 4 &&
                render_mutex_->AcquireSync(0, 0) == S_OK) {
                ComPtr<ID3D11DeviceContext> ctx;
                render_device_->GetImmediateContext(&ctx);
                ctx->UpdateSubresource(render_texture_.Get(), 0, nullptr, cpu_frame_.data(), static_cast<UINT>(width_) * 4, 0);
                render_mutex_->ReleaseSync(0);
                cpu_dirty_ = false;
            }
        }
        Frame f;
        if (render_mutex_) {
            f.texture = render_texture_;
            f.mutex = render_mutex_;
        }
        f.generation = generation_.load(std::memory_order_acquire);
        f.width = width_;
        f.height = height_;
        f.pointer = pointer_;
        return f;
    }
    if (shared_handle_ != opened_handle_) {
        render_texture_.Reset();
        render_mutex_.Reset();
        if (SUCCEEDED(render_device_->OpenSharedResource(shared_handle_, IID_PPV_ARGS(&render_texture_)))) {
            render_texture_.As(&render_mutex_);
        }
        opened_handle_ = shared_handle_;
    }
    Frame f;
    if (render_mutex_) {
        f.texture = render_texture_;
        f.mutex = render_mutex_;
    }
    f.generation = generation_.load(std::memory_order_acquire);
    f.width = width_;
    f.height = height_;
    f.pointer = pointer_;
    return f;
}

bool DesktopDuplication::pointer_shape(uint64_t have, PointerShape* out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (shape_.generation == have) return false;
    *out = shape_;
    return true;
}

void DesktopDuplication::run() {
    while (running_.load()) {
        if (dup_ == nullptr) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            open(nullptr);
            continue;
        }
        DXGI_OUTDUPL_FRAME_INFO info{};
        ComPtr<IDXGIResource> resource;
        const HRESULT hr = dup_->AcquireNextFrame(100, &info, &resource);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) continue;
        if (FAILED(hr)) {  // DXGI_ERROR_ACCESS_LOST: mode change, desktop switch, UAC prompt
            dup_.Reset();
            continue;
        }

        bool changed = false;
        if (info.LastPresentTime.QuadPart != 0) {
            ComPtr<ID3D11Texture2D> tex;
            if (SUCCEEDED(resource.As(&tex)) && cross_) {
                D3D11_TEXTURE2D_DESC d{};
                tex->GetDesc(&d);
                read_back(tex.Get(), d);
                changed = true;
                frames_.fetch_add(1, std::memory_order_relaxed);
            } else if (tex) {
                D3D11_TEXTURE2D_DESC d{};
                tex->GetDesc(&d);
                if (latest_ == nullptr || width_ != static_cast<int>(d.Width) || height_ != static_cast<int>(d.Height)) {
                    D3D11_TEXTURE2D_DESC cd{};
                    cd.Width = d.Width;
                    cd.Height = d.Height;
                    cd.MipLevels = 1;
                    cd.ArraySize = 1;
                    cd.Format = d.Format;  // B8G8R8A8_UNORM for DuplicateOutput
                    cd.SampleDesc.Count = 1;
                    cd.Usage = D3D11_USAGE_DEFAULT;
                    cd.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
                    cd.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
                    ComPtr<ID3D11Texture2D> shared;
                    ComPtr<IDXGIResource> res;
                    HANDLE handle = nullptr;
                    if (SUCCEEDED(device_->CreateTexture2D(&cd, nullptr, &shared)) && SUCCEEDED(shared.As(&res)) &&
                        SUCCEEDED(res->GetSharedHandle(&handle))) {
                        std::lock_guard<std::mutex> lock(mutex_);
                        latest_ = shared;
                        latest_.As(&latest_mutex_);
                        shared_handle_ = handle;
                        width_ = static_cast<int>(d.Width);
                        height_ = static_cast<int>(d.Height);
                    }
                }
                // The render side holds the mutex only for one GPU copy.
                if (latest_mutex_ && latest_mutex_->AcquireSync(0, 100) == S_OK) {
                    context_->CopyResource(latest_.Get(), tex.Get());
                    latest_mutex_->ReleaseSync(0);
                    changed = true;
                    frames_.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
        if (info.LastMouseUpdateTime.QuadPart != 0) {
            std::lock_guard<std::mutex> lock(mutex_);
            pointer_.visible = info.PointerPosition.Visible != FALSE;
            pointer_.x = info.PointerPosition.Position.x;
            pointer_.y = info.PointerPosition.Position.y;
        }
        // The renderer draws the cursor itself at the glasses' rate (GetCursorPos): a move or a
        // new shape is not a new desktop frame to copy and mipmap.
        if (info.PointerShapeBufferSize > 0) read_shape(info);
        dup_->ReleaseFrame();
        if (changed) generation_.fetch_add(1, std::memory_order_acq_rel);
    }
}

void DesktopDuplication::read_back(ID3D11Texture2D* tex, const D3D11_TEXTURE2D_DESC& d) {
    D3D11_TEXTURE2D_DESC have{};
    if (staging_) staging_->GetDesc(&have);
    if (!staging_ || have.Width != d.Width || have.Height != d.Height) {
        D3D11_TEXTURE2D_DESC s{};
        s.Width = d.Width;
        s.Height = d.Height;
        s.MipLevels = 1;
        s.ArraySize = 1;
        s.Format = d.Format;
        s.SampleDesc.Count = 1;
        s.Usage = D3D11_USAGE_STAGING;
        s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        staging_.Reset();
        if (FAILED(device_->CreateTexture2D(&s, nullptr, &staging_))) return;
    }
    context_->CopyResource(staging_.Get(), tex);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(context_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &m))) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const size_t row = static_cast<size_t>(d.Width) * 4;
        cpu_frame_.resize(row * d.Height);
        for (UINT y = 0; y < d.Height; ++y) {
            std::memcpy(cpu_frame_.data() + y * row, static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch, row);
        }
        width_ = static_cast<int>(d.Width);
        height_ = static_cast<int>(d.Height);
        cpu_dirty_ = true;
    }
    context_->Unmap(staging_.Get(), 0);
}

void DesktopDuplication::read_shape(const DXGI_OUTDUPL_FRAME_INFO& info) {
    shape_buffer_.resize(info.PointerShapeBufferSize);
    UINT size = 0;
    DXGI_OUTDUPL_POINTER_SHAPE_INFO si{};
    if (FAILED(dup_->GetFramePointerShape(info.PointerShapeBufferSize, shape_buffer_.data(), &size, &si))) return;

    PointerShape s;
    const bool mono = si.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME;
    s.width = static_cast<int>(si.Width);
    s.height = static_cast<int>(mono ? si.Height / 2 : si.Height);
    s.hot_x = si.HotSpot.x;
    s.hot_y = si.HotSpot.y;
    s.color.assign(static_cast<size_t>(s.width) * s.height, 0);
    s.invert.assign(s.color.size(), 0);
    const uint8_t* buf = shape_buffer_.data();
    for (int y = 0; y < s.height; ++y) {
        for (int x = 0; x < s.width; ++x) {
            const size_t i = static_cast<size_t>(y) * s.width + x;
            if (mono) {
                // AND mask on top, XOR mask below: (0,0) black, (0,1) white, (1,0) clear, (1,1) invert.
                const uint8_t bit = static_cast<uint8_t>(0x80 >> (x % 8));
                const bool a = (buf[y * si.Pitch + x / 8] & bit) != 0;
                const bool xr = (buf[(y + s.height) * si.Pitch + x / 8] & bit) != 0;
                if (!a) s.color[i] = xr ? 0xFFFFFFFFu : 0xFF000000u;
                else if (xr) s.invert[i] = 0xFFFFFFFFu;
            } else {
                const uint32_t px = *reinterpret_cast<const uint32_t*>(buf + y * si.Pitch + x * 4);
                if (si.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR) {
                    s.color[i] = px;
                } else {  // masked colour: alpha 0 replaces, alpha 0xFF XORs the RGB
                    if ((px >> 24) == 0) s.color[i] = px | 0xFF000000u;
                    else s.invert[i] = px | 0xFF000000u;
                }
            }
        }
    }
    std::lock_guard<std::mutex> lock(mutex_);
    s.generation = shape_.generation + 1;
    shape_ = std::move(s);
    pointer_.shape_generation = shape_.generation;
}

}  // namespace rayneo::win
