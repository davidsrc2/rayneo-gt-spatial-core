#pragma once

// AI 3D on Windows: a depth map per video frame from Depth Anything V2 Small (ONNX, fp16
// weights) through ONNX Runtime with DirectML, the counterpart of apps/nimbo/DepthEngine.swift.
// onnxruntime.dll and DirectML.dll are loaded at run time from next to nimbo_server.dll: without
// them Nimbo runs as before, only without AI 3D. The model is downloaded once (50 MB) to
// %LOCALAPPDATA%\Nimbo\Models.
//
// One inference at a time on a worker thread of its own. The input is the frame already shrunk
// on the GPU to input_size() (BGRA, tightly packed). On a slow GPU (integrated) the input size
// steps down until an inference takes under ~45 ms.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace rayneo::win {

struct DepthInput {
    uint64_t seq = 0;
    int width = 0, height = 0;
    std::vector<uint8_t> bgra;  // width * height * 4
};

struct DepthOutput {
    uint64_t seq = 0;
    int width = 0, height = 0;
    std::vector<float> depth;   // larger = nearer, unnormalised
    float low = 0, high = 1;    // 2 % and 98 % percentiles (inside the video)
    bool scene_cut = false;     // a new shot: the smoothing starts afresh
    float video[4] = {0, 0, 1, 1};  // where the video is in the frame, 0..1 (x0, y0, x1, y1): flat outside
};

// Where the video is in the captured frame (apps/nimbo/DepthEngine.swift, VideoArea): video
// changes in every frame almost everywhere, a page around it not at all; columns and rows of a
// coarse grid that keep changing make the box. Kept while nothing moves; cuts are ignored.
class VideoArea {
public:
    static constexpr int kColumns = 64, kRows = 48;
    void reset();
    // grid: kColumns x kRows luma; returns the box, 0..1.
    const float* update(const std::vector<uint8_t>& grid, bool cut);

private:
    std::vector<uint8_t> previous_;
    std::vector<float> energy_ = std::vector<float>(kColumns * kRows, 0.0f);
    int moving_ = 0;
    float rect_[4] = {0, 0, 1, 1};
};

class DepthEngine {
public:
    using Log = std::function<void(const std::string&)>;
    enum class State { Idle, Loading, Ready, Failed };

    struct Stats {
        int frames = 0;
        int cuts = 0;
        double prep_ms = 0;   // summed
        double infer_ms = 0;  // summed
    };

    DepthEngine() = default;
    ~DepthEngine();
    DepthEngine(const DepthEngine&) = delete;
    DepthEngine& operator=(const DepthEngine&) = delete;

    // Loads on the worker thread (download, ONNX Runtime, session): returns at once.
    // model_path empty: the default one (downloaded when missing).
    void start(Log log, std::string model_path = {});
    State state() const { return state_.load(); }
    std::string error() const;

    // What submit expects; changes when the engine steps down to a smaller size.
    void input_size(int* width, int* height) const;
    // Ready and nothing in flight: a submit would be taken.
    bool idle() const;
    // false: busy, not ready, or the wrong size.
    bool submit(DepthInput in);
    // The latest finished map, once.
    bool take(DepthOutput* out);
    // The next frame starts a new shot (another panel, mode change).
    void reset_scene();
    Stats take_stats();
    int input_width() const { return width_.load(); }

    static std::string default_model_path();

private:
    void run(std::string model_path);
    bool load(const std::string& path);
    bool create_session(int width, int height);
    void release_session();
    bool infer(const DepthInput& in, DepthOutput* out, double* infer_ms);
    bool scene_cut(const DepthInput& in);
    VideoArea video_area_;  // worker thread
    void fail(const std::string& message);

    Log log_;
    std::atomic<State> state_{State::Idle};
    std::atomic<int> width_{518}, height_{392};
    int size_index_ = 0;  // worker: into the input sizes

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool quit_ = false;                 // guarded by mutex_
    bool has_job_ = false, busy_ = false;
    DepthInput job_;                    // guarded by mutex_
    bool has_result_ = false;
    DepthOutput result_;                // guarded by mutex_
    Stats stats_;                       // guarded by mutex_
    std::string error_;                 // guarded by mutex_
    bool reset_scene_ = true;           // guarded by mutex_
    std::thread thread_;

    // Worker thread.
    void* library_ = nullptr;           // onnxruntime
    const void* api_ = nullptr;         // const OrtApi*
    void* env_ = nullptr;               // OrtEnv*
    void* session_ = nullptr;           // OrtSession*
    void* memory_ = nullptr;            // OrtMemoryInfo*
    std::string path_;
    std::vector<float> tensor_;
    std::vector<uint8_t> previous_thumb_;
    float diff_average_ = 0;
    double slow_ms_sum_ = 0;
    int slow_n_ = 0;
};

}  // namespace rayneo::win
