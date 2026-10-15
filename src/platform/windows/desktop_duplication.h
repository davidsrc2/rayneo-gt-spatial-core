#pragma once

// DXGI Desktop Duplication of one output (the ScreenCaptureKit equivalent). A capture thread
// with a D3D11 device of its own copies every new desktop frame into a shared texture; the
// render device opens it and copies from it under a keyed mutex. Separate devices matter:
// AcquireNextFrame holds its device's lock while it waits, which would stall the render loop.

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

// What the renderer reads from: the latest frame of a display (DesktopDuplication) or of a
// single window (WindowCapture), handed over in a keyed-mutex shared texture.
class FrameSource {
public:
    struct Frame {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;  // render device, B8G8R8A8_UNORM
        Microsoft::WRL::ComPtr<IDXGIKeyedMutex> mutex;    // hold (key 0) while reading `texture`
        uint64_t generation = 0;  // bumps on every new frame or pointer change
        int width = 0;
        int height = 0;
        PointerState pointer;     // windows: never visible (the capture draws the cursor itself)
    };

    virtual ~FrameSource() = default;
    virtual Frame latest() = 0;  // render thread
    virtual uint64_t generation() const = 0;
    // Copies the pointer shape if its generation differs from `have`; false otherwise.
    virtual bool pointer_shape(uint64_t have, PointerShape* out) = 0;
};

class DesktopDuplication final : public FrameSource {
public:

    DesktopDuplication() = default;
    ~DesktopDuplication();
    DesktopDuplication(const DesktopDuplication&) = delete;
    DesktopDuplication& operator=(const DesktopDuplication&) = delete;

    // `adapter` must own `output`; frames are handed to `render_device` (same adapter).
    bool start(IDXGIAdapter* adapter, IDXGIOutput1* output, ID3D11Device* render_device, std::string* error);
    void stop();

    Frame latest() override;  // render thread
    uint64_t generation() const override { return generation_.load(std::memory_order_acquire); }
    bool pointer_shape(uint64_t have, PointerShape* out) override;
    uint64_t frames() const { return frames_.load(std::memory_order_relaxed); }

private:
    void run();
    bool open(std::string* error);
    void read_shape(const DXGI_OUTDUPL_FRAME_INFO& info);

    Microsoft::WRL::ComPtr<ID3D11Device> device_;  // capture thread's own device
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID3D11Device> render_device_;
    Microsoft::WRL::ComPtr<IDXGIOutput1> output_;
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> dup_;  // capture thread

    std::mutex mutex_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> latest_;  // capture device; shared, keyed mutex
    Microsoft::WRL::ComPtr<IDXGIKeyedMutex> latest_mutex_;
    HANDLE shared_handle_ = nullptr;                  // guarded by mutex_
    HANDLE opened_handle_ = nullptr;                  // render thread
    Microsoft::WRL::ComPtr<ID3D11Texture2D> render_texture_;  // render thread
    Microsoft::WRL::ComPtr<IDXGIKeyedMutex> render_mutex_;    // render thread
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
