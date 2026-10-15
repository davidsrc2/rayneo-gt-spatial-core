#include "window_capture.h"

#include <dwmapi.h>
#include <psapi.h>

#pragma warning(push, 0)
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#pragma warning(pop)

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>

using Microsoft::WRL::ComPtr;
namespace wgc = winrt::Windows::Graphics::Capture;
namespace wgd = winrt::Windows::Graphics::DirectX;

namespace rayneo::win {

namespace {

std::string utf8(const wchar_t* w, int n) {
    if (n <= 0) return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, w, n, nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, n, s.data(), len, nullptr, nullptr);
    return s;
}

std::string process_name(DWORD pid) {
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (p == nullptr) return {};
    wchar_t path[MAX_PATH];
    DWORD n = MAX_PATH;
    std::string out;
    if (QueryFullProcessImageNameW(p, 0, path, &n)) {
        std::wstring s(path, n);
        const size_t slash = s.find_last_of(L"\\/");
        if (slash != std::wstring::npos) s = s.substr(slash + 1);
        const size_t dot = s.find_last_of(L'.');
        if (dot != std::wstring::npos) s = s.substr(0, dot);
        out = utf8(s.c_str(), static_cast<int>(s.size()));
    }
    CloseHandle(p);
    return out;
}

BOOL CALLBACK collect(HWND hwnd, LPARAM lp) {
    auto* out = reinterpret_cast<std::vector<WindowInfo>*>(lp);
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd) || GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;
    if ((GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) != 0) return TRUE;
    BOOL cloaked = FALSE;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) return TRUE;
    RECT r{};
    if (!GetWindowRect(hwnd, &r) || r.right - r.left < 200 || r.bottom - r.top < 120) return TRUE;
    wchar_t title[256];
    const int n = GetWindowTextW(hwnd, title, 256);
    if (n <= 0) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId()) return TRUE;
    out->push_back({hwnd, utf8(title, n), process_name(pid)});
    return TRUE;
}

}  // namespace

std::vector<WindowInfo> list_windows() {
    std::vector<WindowInfo> out;
    EnumWindows(collect, reinterpret_cast<LPARAM>(&out));
    return out;
}

struct WindowCapture::Impl {
    ComPtr<ID3D11Device> device;  // capture device
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11Device> render_device;
    wgd::Direct3D11::IDirect3DDevice rt_device{nullptr};
    wgc::GraphicsCaptureItem item{nullptr};
    wgc::Direct3D11CaptureFramePool pool{nullptr};
    wgc::GraphicsCaptureSession session{nullptr};
    winrt::event_token token{};
    winrt::Windows::Graphics::SizeInt32 size{};

    std::mutex frame_mutex;  // FrameArrived: one frame at a time on the capture context
    std::mutex mutex;        // shared texture handle and size
    ComPtr<ID3D11Texture2D> shared;
    ComPtr<IDXGIKeyedMutex> shared_mutex;
    HANDLE shared_handle = nullptr;
    int width = 0, height = 0;

    HANDLE opened = nullptr;  // render thread
    ComPtr<ID3D11Texture2D> render_texture;
    ComPtr<IDXGIKeyedMutex> render_mutex;

    std::atomic<uint64_t> generation{0};
    std::atomic<uint64_t> frames{0};

    void on_frame();
};

WindowCapture::WindowCapture() : impl_(std::make_unique<Impl>()) {}

WindowCapture::~WindowCapture() { stop(); }

bool WindowCapture::start(IDXGIAdapter* adapter, HWND window, ID3D11Device* render_device, std::string* error) {
    auto fail = [&](const char* why) {
        if (error != nullptr) *error = why;
        return false;
    };
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (const winrt::hresult_error&) {
        // Already initialised on this thread (any model works for a free-threaded frame pool).
    }
    try {
        if (!wgc::GraphicsCaptureSession::IsSupported()) return fail("window capture needs Windows 10 1903 or newer");
        Impl& m = *impl_;
        if (FAILED(D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr,
                                     0, D3D11_SDK_VERSION, &m.device, nullptr, &m.ctx))) {
            return fail("capture device creation failed");
        }
        m.render_device = render_device;
        ComPtr<IDXGIDevice> dxgi;
        m.device.As(&dxgi);
        winrt::com_ptr<::IInspectable> inspectable;
        winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(), inspectable.put()));
        m.rt_device = inspectable.as<wgd::Direct3D11::IDirect3DDevice>();

        auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        winrt::check_hresult(interop->CreateForWindow(window, winrt::guid_of<wgc::GraphicsCaptureItem>(),
                                                      winrt::put_abi(m.item)));
        m.size = m.item.Size();
        m.pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(m.rt_device, wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                                                                    2, m.size);
        m.token = m.pool.FrameArrived([this](auto&&, auto&&) { impl_->on_frame(); });
        m.session = m.pool.CreateCaptureSession(m.item);
        try {
            m.session.IsBorderRequired(false);  // Windows 11: no yellow border around the window
        } catch (...) {
        }
        try {
            m.session.IsCursorCaptureEnabled(true);
        } catch (...) {
        }
        m.session.StartCapture();
        return true;
    } catch (const winrt::hresult_error& e) {
        if (error != nullptr) {
            char buf[96];
            snprintf(buf, sizeof(buf), "window capture failed (0x%08lx)", static_cast<unsigned long>(e.code().value));
            *error = buf;
        }
        return false;
    }
}

void WindowCapture::stop() {
    Impl& m = *impl_;
    try {
        if (m.session) m.session.Close();
        if (m.pool) {
            m.pool.FrameArrived(m.token);
            m.pool.Close();
        }
    } catch (...) {
    }
    m.session = nullptr;
    m.pool = nullptr;
    m.item = nullptr;
}

void WindowCapture::Impl::on_frame() {
    std::lock_guard<std::mutex> frame_lock(frame_mutex);
    auto frame = pool.TryGetNextFrame();
    if (!frame) return;
    const auto content = frame.ContentSize();
    auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    ComPtr<ID3D11Texture2D> tex;
    if (FAILED(access->GetInterface(IID_PPV_ARGS(&tex)))) return;
    if (content.Width != size.Width || content.Height != size.Height) {  // window resized
        size = content;
        pool.Recreate(rt_device, wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
    }
    const int w = std::max(1, content.Width), h = std::max(1, content.Height);
    if (!shared || width != w || height != h) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = static_cast<UINT>(w);
        d.Height = static_cast<UINT>(h);
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        d.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
        ComPtr<ID3D11Texture2D> t;
        ComPtr<IDXGIResource> res;
        HANDLE handle = nullptr;
        if (FAILED(device->CreateTexture2D(&d, nullptr, &t)) || FAILED(t.As(&res)) || FAILED(res->GetSharedHandle(&handle))) {
            return;
        }
        std::lock_guard<std::mutex> lock(mutex);
        shared = t;
        shared.As(&shared_mutex);
        shared_handle = handle;
        width = w;
        height = h;
    }
    if (shared_mutex->AcquireSync(0, 50) != S_OK) return;
    D3D11_TEXTURE2D_DESC src{};
    tex->GetDesc(&src);
    const D3D11_BOX box{0, 0, 0, std::min<UINT>(src.Width, static_cast<UINT>(w)), std::min<UINT>(src.Height, static_cast<UINT>(h)), 1};
    ctx->CopySubresourceRegion(shared.Get(), 0, 0, 0, 0, tex.Get(), 0, &box);
    shared_mutex->ReleaseSync(0);
    frames.fetch_add(1, std::memory_order_relaxed);
    generation.fetch_add(1, std::memory_order_acq_rel);
}

FrameSource::Frame WindowCapture::latest() {
    Impl& m = *impl_;
    std::lock_guard<std::mutex> lock(m.mutex);
    if (m.shared_handle != m.opened) {
        m.render_texture.Reset();
        m.render_mutex.Reset();
        if (m.shared_handle != nullptr &&
            SUCCEEDED(m.render_device->OpenSharedResource(m.shared_handle, IID_PPV_ARGS(&m.render_texture)))) {
            m.render_texture.As(&m.render_mutex);
        }
        m.opened = m.shared_handle;
    }
    Frame f;
    if (m.render_mutex) {
        f.texture = m.render_texture;
        f.mutex = m.render_mutex;
    }
    f.generation = m.generation.load(std::memory_order_acquire);
    f.width = m.width;
    f.height = m.height;
    return f;
}

uint64_t WindowCapture::generation() const { return impl_->generation.load(std::memory_order_acquire); }

uint64_t WindowCapture::frames() const { return impl_->frames.load(std::memory_order_relaxed); }

}  // namespace rayneo::win
