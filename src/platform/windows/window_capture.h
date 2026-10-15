#pragma once

// Capture of a single window with Windows.Graphics.Capture (the floating windows: a video
// above the monitors...), the counterpart of ScreenCaptureKit's desktopIndependentWindow.
// Works while the window is covered. Frames reach the renderer like DesktopDuplication's:
// a capture device of its own and a keyed-mutex shared texture.

#include <windows.h>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "desktop_duplication.h"

namespace rayneo::win {

struct WindowInfo {
    HWND hwnd = nullptr;
    std::string title;  // UTF-8
    std::string app;    // executable name without extension
};

// Windows worth floating: visible, titled, not tiny, not cloaked, not tool windows, not ours.
std::vector<WindowInfo> list_windows();

class WindowCapture final : public FrameSource {
public:
    WindowCapture();
    ~WindowCapture() override;
    WindowCapture(const WindowCapture&) = delete;
    WindowCapture& operator=(const WindowCapture&) = delete;

    // `adapter` is the GPU of `render_device` (frames are shared between the two devices).
    bool start(IDXGIAdapter* adapter, HWND window, ID3D11Device* render_device, std::string* error);
    void stop();

    Frame latest() override;
    uint64_t generation() const override;
    bool pointer_shape(uint64_t, PointerShape*) override { return false; }  // drawn by the capture
    uint64_t frames() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace rayneo::win
