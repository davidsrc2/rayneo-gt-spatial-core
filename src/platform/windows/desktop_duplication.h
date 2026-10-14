#pragma once

// DXGI Desktop Duplication of one output (the ScreenCaptureKit equivalent). A capture thread
// copies every new desktop frame into a texture of its own; the renderer reads the latest
// one from the render thread. Both share one D3D11 device with multithread protection on.

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace rayneo::win {

// Desktop Duplication hands the pointer over separately from the frame. Its shape comes
// split in two BGRA images: `color` is alpha blended, `invert` marks the pixels that XOR
// with the desktop (text I-beam, monochrome cursors).
struct PointerShape {
    uint64_t generation = 0;  // bumps when the shape changes
    int width = 0;
    int height = 0;
    std::vector<uint32_t> color;
    std::vector<uint32_t> invert;
};

struct PointerState {
    bool visible = false;
    int x = 0;  // top-left of the shape, in output pixels
    int y = 0;
    uint64_t shape_generation = 0;
};

class DesktopDuplication {
public:
    struct Frame {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;  // B8G8R8A8_UNORM, output size
        uint64_t generation = 0;  // bumps on every new frame or pointer change
        int width = 0;
        int height = 0;
        PointerState pointer;
    };

    DesktopDuplication() = default;
    ~DesktopDuplication();
    DesktopDuplication(const DesktopDuplication&) = delete;
    DesktopDuplication& operator=(const DesktopDuplication&) = delete;

    // `device` must be on the adapter that owns `output`.
    bool start(ID3D11Device* device, IDXGIOutput1* output, std::string* error);
    void stop();

    Frame latest();
    uint64_t generation() const { return generation_.load(std::memory_order_acquire); }
    // Copies the shape if its generation differs from `have`; returns false otherwise.
    bool pointer_shape(uint64_t have, PointerShape* out);
    uint64_t frames() const { return frames_.load(std::memory_order_relaxed); }

private:
    void run();
    bool open(std::string* error);
    void read_shape(const DXGI_OUTDUPL_FRAME_INFO& info);

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGIOutput1> output_;
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> dup_;  // capture thread

    std::mutex mutex_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> latest_;  // guarded by mutex_
    int width_ = 0, height_ = 0;                      // guarded by mutex_
    PointerState pointer_;                            // guarded by mutex_
    PointerShape shape_;                              // guarded by mutex_
    std::vector<uint8_t> shape_buffer_;               // capture thread

    std::atomic<uint64_t> generation_{0};
    std::atomic<uint64_t> frames_{0};
    std::atomic<bool> running_{false};
    std::thread thread_;
};

}  // namespace rayneo::win
