#pragma once

// DXGI Desktop Duplication of one output (the ScreenCaptureKit equivalent). A capture thread
// with a D3D11 device of its own copies every new desktop frame into a shared texture; the
// render device opens it and copies from it under a keyed mutex. Separate devices matter:
// AcquireNextFrame holds its device's lock while it waits, which would stall the render loop.

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <array>
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
    int hot_x = 0;  // the point GetCursorPos reports, inside the shape
    int hot_y = 0;
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
        uint64_t generation = 0;  // bumps on every new desktop frame (the cursor is drawn live, apart)
        int width = 0;
        int height = 0;
        // partial: only `dirty` changed since the generation passed to latest(); else all of it.
        bool partial = false;
        D3D11_BOX dirty{};
        PointerState pointer;     // windows: never visible (the capture draws the cursor itself)
    };

    virtual ~FrameSource() = default;
    // Render thread. `since`: the generation the caller last copied (~0: none), for Frame::dirty.
    virtual Frame latest(uint64_t since) = 0;
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

    // `adapter` must own `output`; frames are handed to `render_device`: shared on the GPU when
    // it is the same adapter, else copied through the CPU (laptops with two GPUs).
    bool start(IDXGIAdapter* adapter, IDXGIOutput1* output, ID3D11Device* render_device, std::string* error);
    void stop();

    Frame latest(uint64_t since) override;  // render thread
    uint64_t generation() const override { return generation_.load(std::memory_order_acquire); }
    bool pointer_shape(uint64_t have, PointerShape* out) override;
    uint64_t frames() const { return frames_.load(std::memory_order_relaxed); }
    // Out of the glasses' view: at most kHiddenFps frames copied (as macOS). Any thread.
    void set_in_view(bool in_view) { in_view_.store(in_view, std::memory_order_relaxed); }
    static constexpr int kHiddenFps = 8;

private:
    void run();
    bool open(std::string* error);
    void read_shape(const DXGI_OUTDUPL_FRAME_INFO& info);
    // The frame's changed areas (dirty rects and moves' destinations) into rects_, clamped to
    // the image; false when the whole image must be taken (no metadata, rotated output).
    bool read_changes(const DXGI_OUTDUPL_FRAME_INFO& info, UINT width, UINT height);
    // Copies rects_ (or all of it) from `src` into `dst` on the capture device.
    void copy_changes(ID3D11Texture2D* dst, ID3D11Texture2D* src, bool all);
    void publish(bool all);  // bumps the generation, its changed area into history_
    void changes_since(uint64_t since, Frame* f) const;

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

    // Only the areas a desktop frame changed are copied (typing: a few lines, not 33 MB of a 4K
    // desktop twice per frame). The bounding box of each generation's changes is kept for the
    // last kHistory generations; a caller further behind takes the whole frame.
    static constexpr uint64_t kHistory = 32;
    struct Change {
        uint64_t generation = 0;
        bool all = true;
        D3D11_BOX box{};
    };
    std::array<Change, kHistory> history_{};          // guarded by mutex_
    std::vector<uint8_t> meta_;                       // capture thread: move and dirty rects
    std::vector<RECT> rects_;                         // capture thread: this frame's changes
    D3D11_BOX changed_{};                             // capture thread: their bounding box
    bool full_next_ = true;                           // capture thread: the next frame copied whole
    bool rotated_ = false;                            // capture thread: rects in rotated space

    // Another GPU renders: each frame read back here (staging) and uploaded on the render side.
    bool cross_ = false;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging_;  // capture device
    std::vector<uint8_t> cpu_frame_;                   // guarded by mutex_, width_ * 4 per row
    bool cpu_dirty_ = false;                           // guarded by mutex_
    bool cpu_all_ = true;                              // guarded by mutex_: upload all of it
    D3D11_BOX cpu_box_{};                              // guarded by mutex_: else this area
    // false: nothing read (the frame's changes are lost: the next one is taken whole).
    bool read_back(ID3D11Texture2D* tex, const D3D11_TEXTURE2D_DESC& d, bool& all);  // `all` set when it had to be

    std::atomic<uint64_t> generation_{0};
    std::atomic<uint64_t> frames_{0};
    std::atomic<bool> running_{false};
    std::atomic<bool> in_view_{true};
    std::thread thread_;
};

}  // namespace rayneo::win
