#include "depth_engine.h"

#include <onnxruntime_c_api.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#include <urlmon.h>
#else
#include <dlfcn.h>
#endif

namespace rayneo::win {

namespace {

constexpr const char* kModelUrl =
    "https://huggingface.co/onnx-community/depth-anything-v2-small/resolve/main/onnx/model_fp16.onnx";
constexpr const char* kModelFile = "depth_anything_v2_small_fp16.onnx";
// Multiples of 14 (the model's patch size), about the aspect of a film frame.
constexpr int kSizes[][2] = {{518, 392}, {392, 294}, {308, 224}};
constexpr int kSizeCount = sizeof(kSizes) / sizeof(kSizes[0]);
constexpr double kSlowMs = 45.0;  // above this on average, step down to the next size
constexpr int kSlowFrames = 20;

// The first entry of OrtDmlApi (dml_provider_factory.h, which needs DirectML.h): enough to
// append the DirectML execution provider by adapter index.
struct DmlApi {
    OrtStatus*(ORT_API_CALL* append_dml)(OrtSessionOptions* options, int device_id) NO_EXCEPTION;
};

const OrtApi* ort(const void* api) { return static_cast<const OrtApi*>(api); }

double ms_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

#ifdef _WIN32
std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string narrow_path(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// The folder of the module holding this code (nimbo_server.dll): the ONNX Runtime and
// DirectML DLLs are shipped next to it.
std::wstring module_dir() {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&module_dir), &self);
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(self, path, MAX_PATH);
    std::wstring p(path);
    const size_t slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : p.substr(0, slash);
}
#endif

bool file_exists(const std::string& path) {
#ifdef _WIN32
    return GetFileAttributesW(widen(path).c_str()) != INVALID_FILE_ATTRIBUTES;
#else
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        std::fclose(f);
        return true;
    }
    return false;
#endif
}

}  // namespace

DepthEngine::~DepthEngine() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        quit_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    release_session();
    if (api_ != nullptr) {
        if (memory_ != nullptr) ort(api_)->ReleaseMemoryInfo(static_cast<OrtMemoryInfo*>(memory_));
        if (env_ != nullptr) ort(api_)->ReleaseEnv(static_cast<OrtEnv*>(env_));
    }
    // The library stays loaded: ONNX Runtime does not support being unloaded and reloaded.
}

std::string DepthEngine::default_model_path() {
#ifdef _WIN32
    PWSTR base = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base))) dir = base;
    CoTaskMemFree(base);
    dir += L"\\Nimbo\\Models";
    return narrow_path(dir) + "\\" + kModelFile;
#else
    const char* home = std::getenv("HOME");
    return std::string(home != nullptr ? home : ".") + "/" + kModelFile;
#endif
}

void DepthEngine::start(Log log, std::string model_path) {
    if (state_.load() != State::Idle) return;
    log_ = std::move(log);
    state_ = State::Loading;
    if (model_path.empty()) model_path = default_model_path();
    thread_ = std::thread([this, model_path] { run(model_path); });
}

std::string DepthEngine::error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return error_;
}

void DepthEngine::input_size(int* width, int* height) const {
    *width = width_.load();
    *height = height_.load();
}

bool DepthEngine::idle() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_.load() == State::Ready && !has_job_ && !busy_;
}

bool DepthEngine::submit(DepthInput in) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_.load() != State::Ready || has_job_ || busy_) return false;
        if (in.width != width_.load() || in.height != height_.load()) return false;
        if (in.bgra.size() < static_cast<size_t>(in.width) * static_cast<size_t>(in.height) * 4) return false;
        job_ = std::move(in);
        has_job_ = true;
    }
    cv_.notify_all();
    return true;
}

bool DepthEngine::take(DepthOutput* out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!has_result_) return false;
    *out = std::move(result_);
    has_result_ = false;
    return true;
}

void DepthEngine::reset_scene() {
    std::lock_guard<std::mutex> lock(mutex_);
    reset_scene_ = true;
}

DepthEngine::Stats DepthEngine::take_stats() {
    std::lock_guard<std::mutex> lock(mutex_);
    const Stats s = stats_;
    stats_ = Stats{};
    return s;
}

void DepthEngine::fail(const std::string& message) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        error_ = message;
    }
    state_ = State::Failed;
    if (log_) log_("3d: " + message);
}

void DepthEngine::run(std::string model_path) {
    if (!load(model_path)) return;
    state_ = State::Ready;
    for (;;) {
        DepthInput in;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return quit_ || has_job_; });
            if (quit_) return;
            in = std::move(job_);
            has_job_ = false;
            busy_ = true;
        }
        const auto t0 = std::chrono::steady_clock::now();
        DepthOutput out;
        out.seq = in.seq;
        out.scene_cut = scene_cut(in);
        {
            std::vector<uint8_t> grid(VideoArea::kColumns * VideoArea::kRows);
            for (int gy = 0; gy < VideoArea::kRows; ++gy) {
                const int y = gy * in.height / VideoArea::kRows + in.height / (2 * VideoArea::kRows);
                for (int gx = 0; gx < VideoArea::kColumns; ++gx) {
                    const int x = gx * in.width / VideoArea::kColumns + in.width / (2 * VideoArea::kColumns);
                    const uint8_t* px = &in.bgra[(static_cast<size_t>(y) * static_cast<size_t>(in.width) + static_cast<size_t>(x)) * 4];
                    grid[static_cast<size_t>(gy * VideoArea::kColumns + gx)] =
                        static_cast<uint8_t>((px[2] * 54 + px[1] * 183 + px[0] * 19) >> 8);
                }
            }
            const float* v = video_area_.update(grid, out.scene_cut);
            for (int i = 0; i < 4; ++i) out.video[i] = v[i];
        }
        double infer_ms = 0;
        const bool ok = infer(in, &out, &infer_ms);
        const double total_ms = ms_since(t0);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            busy_ = false;
            if (ok) {
                result_ = std::move(out);
                has_result_ = true;
                stats_.frames += 1;
                if (result_.scene_cut) stats_.cuts += 1;
                stats_.infer_ms += infer_ms;
                stats_.prep_ms += total_ms - infer_ms;
            }
        }
        if (!ok) continue;
        // An integrated GPU may be too slow at full size: smaller, a little less detail.
        slow_ms_sum_ += infer_ms;
        if (++slow_n_ >= kSlowFrames) {
            const double mean = slow_ms_sum_ / slow_n_;
            slow_ms_sum_ = 0;
            slow_n_ = 0;
            if (mean > kSlowMs && size_index_ + 1 < kSizeCount) {
                ++size_index_;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    busy_ = true;  // no submits while the session is rebuilt
                }
                if (log_) {
                    char buf[128];
                    std::snprintf(buf, sizeof(buf), "3d: %.0f ms per frame, input down to %dx%d", mean,
                                  kSizes[size_index_][0], kSizes[size_index_][1]);
                    log_(buf);
                }
                const bool rebuilt = create_session(kSizes[size_index_][0], kSizes[size_index_][1]);
                std::lock_guard<std::mutex> lock(mutex_);
                busy_ = false;
                if (!rebuilt) {
                    error_ = "could not rebuild the session";
                    state_ = State::Failed;
                    return;
                }
            }
        }
    }
}

bool DepthEngine::load(const std::string& path) {
    path_ = path;
#ifdef _WIN32
    const std::wstring dir = module_dir();
    const std::wstring dll = dir + L"\\onnxruntime.dll";
    // Altered search path: its own imports (DirectML.dll) come from the same folder before System32's.
    HMODULE lib = LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (lib == nullptr) {
        fail("onnxruntime.dll not found next to nimbo_server.dll");
        return false;
    }
    library_ = lib;
    using GetApiBase = const OrtApiBase*(ORT_API_CALL*)();
    auto get_base = reinterpret_cast<GetApiBase>(reinterpret_cast<void*>(GetProcAddress(lib, "OrtGetApiBase")));
#else
    void* lib = dlopen("libonnxruntime.dylib", RTLD_NOW);
    if (lib == nullptr) {
        fail("libonnxruntime not found");
        return false;
    }
    library_ = lib;
    using GetApiBase = const OrtApiBase* (*)();
    auto get_base = reinterpret_cast<GetApiBase>(dlsym(lib, "OrtGetApiBase"));
#endif
    if (get_base == nullptr || get_base()->GetApi(ORT_API_VERSION) == nullptr) {
        fail("onnxruntime is too old (needs API " + std::to_string(ORT_API_VERSION) + ")");
        return false;
    }
    api_ = get_base()->GetApi(ORT_API_VERSION);

    if (!file_exists(path)) {
#ifdef _WIN32
        if (log_) log_("3d: downloading the depth model (50 MB)");
        const std::wstring wpath = widen(path);
        const std::wstring folder = wpath.substr(0, wpath.find_last_of(L'\\'));
        SHCreateDirectoryExW(nullptr, folder.c_str(), nullptr);
        const std::wstring part = wpath + L".part";
        const HRESULT hr = URLDownloadToFileW(nullptr, widen(kModelUrl).c_str(), part.c_str(), 0, nullptr);
        if (FAILED(hr) || !MoveFileExW(part.c_str(), wpath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            DeleteFileW(part.c_str());
            char buf[96];
            std::snprintf(buf, sizeof(buf), "model download failed (0x%08lx)", static_cast<unsigned long>(hr));
            fail(buf);
            return false;
        }
#else
        fail("model missing: " + path);
        return false;
#endif
    }

    const OrtApi* a = ort(api_);
    OrtEnv* env = nullptr;
    if (OrtStatus* st = a->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "nimbo", &env)) {
        fail(std::string("CreateEnv: ") + a->GetErrorMessage(st));
        a->ReleaseStatus(st);
        return false;
    }
    env_ = env;
    OrtMemoryInfo* mem = nullptr;
    if (OrtStatus* st = a->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &mem)) {
        fail(std::string("CreateCpuMemoryInfo: ") + a->GetErrorMessage(st));
        a->ReleaseStatus(st);
        return false;
    }
    memory_ = mem;
    const auto t0 = std::chrono::steady_clock::now();
    if (!create_session(kSizes[0][0], kSizes[0][1])) return false;
    if (log_) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "3d: model ready in %.1f s (DirectML), input %dx%d", ms_since(t0) / 1000.0,
                      width_.load(), height_.load());
        log_(buf);
    }
    return true;
}

void DepthEngine::release_session() {
    if (session_ != nullptr && api_ != nullptr) ort(api_)->ReleaseSession(static_cast<OrtSession*>(session_));
    session_ = nullptr;
}

// A session with every dimension fixed (DirectML compiles the graph once for that shape).
bool DepthEngine::create_session(int width, int height) {
    const OrtApi* a = ort(api_);
    release_session();
    OrtSessionOptions* so = nullptr;
    auto check = [&](OrtStatus* st, const char* what) {
        if (st == nullptr) return true;
        fail(std::string(what) + ": " + a->GetErrorMessage(st));
        a->ReleaseStatus(st);
        return false;
    };
    if (!check(a->CreateSessionOptions(&so), "CreateSessionOptions")) return false;
    bool ok = check(a->SetSessionGraphOptimizationLevel(so, ORT_ENABLE_ALL), "optimization level") &&
              check(a->AddFreeDimensionOverrideByName(so, "batch_size", 1), "batch_size") &&
              check(a->AddFreeDimensionOverrideByName(so, "height", height), "height") &&
              check(a->AddFreeDimensionOverrideByName(so, "width", width), "width");
#ifdef _WIN32
    // DirectML needs these two; adapter 0 is the default one, the GPU driving the desktop.
    ok = ok && check(a->DisableMemPattern(so), "DisableMemPattern") &&
         check(a->SetSessionExecutionMode(so, ORT_SEQUENTIAL), "execution mode");
    const void* dml = nullptr;
    ok = ok && check(a->GetExecutionProviderApi("DML", ORT_API_VERSION, &dml), "DirectML provider") && dml != nullptr &&
         check(static_cast<const DmlApi*>(dml)->append_dml(so, 0), "DirectML (DirectML.dll next to onnxruntime.dll?)");
    OrtSession* session = nullptr;
    ok = ok && check(a->CreateSession(static_cast<OrtEnv*>(env_), widen(path_).c_str(), so, &session), "model");
#else
    OrtSession* session = nullptr;
    ok = ok && check(a->CreateSession(static_cast<OrtEnv*>(env_), path_.c_str(), so, &session), "model");
#endif
    a->ReleaseSessionOptions(so);
    if (!ok) return false;
    session_ = session;
    width_ = width;
    height_ = height;
    tensor_.assign(static_cast<size_t>(3 * width * height), 0.0f);
    return true;
}

// Normalised like the model was trained (ImageNet mean / std, RGB planes), run, and the
// 2-98 % range of the result.
bool DepthEngine::infer(const DepthInput& in, DepthOutput* out, double* infer_ms) {
    const OrtApi* a = ort(api_);
    const int w = in.width, h = in.height;
    const size_t plane = static_cast<size_t>(w) * static_cast<size_t>(h);
    if (session_ == nullptr || tensor_.size() != 3 * plane) return false;
    constexpr float kMean[3] = {0.485f, 0.456f, 0.406f};
    constexpr float kStd[3] = {0.229f, 0.224f, 0.225f};
    float scale[3], bias[3];
    for (int c = 0; c < 3; ++c) {
        scale[c] = 1.0f / (255.0f * kStd[c]);
        bias[c] = -kMean[c] / kStd[c];
    }
    float* r = tensor_.data();
    float* g = r + plane;
    float* b = g + plane;
    const uint8_t* px = in.bgra.data();
    for (size_t i = 0; i < plane; ++i, px += 4) {
        b[i] = px[0] * scale[2] + bias[2];
        g[i] = px[1] * scale[1] + bias[1];
        r[i] = px[2] * scale[0] + bias[0];
    }
    const int64_t shape[4] = {1, 3, h, w};
    OrtValue* input = nullptr;
    OrtValue* output = nullptr;
    auto check = [&](OrtStatus* st, const char* what) {
        if (st == nullptr) return true;
        if (log_) log_(std::string("3d: ") + what + ": " + a->GetErrorMessage(st));
        a->ReleaseStatus(st);
        return false;
    };
    if (!check(a->CreateTensorWithDataAsOrtValue(static_cast<OrtMemoryInfo*>(memory_), tensor_.data(),
                                                 tensor_.size() * sizeof(float), shape, 4,
                                                 ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &input),
               "input")) {
        return false;
    }
    const char* in_names[] = {"pixel_values"};
    const char* out_names[] = {"predicted_depth"};
    const auto t0 = std::chrono::steady_clock::now();
    const bool ran = check(a->Run(static_cast<OrtSession*>(session_), nullptr, in_names, &input, 1, out_names, 1, &output),
                           "inference");
    *infer_ms = ms_since(t0);
    a->ReleaseValue(input);
    if (!ran) return false;
    float* data = nullptr;
    OrtTensorTypeAndShapeInfo* info = nullptr;
    size_t dims_n = 0;
    int64_t dims[4] = {};
    bool ok = check(a->GetTensorMutableData(output, reinterpret_cast<void**>(&data)), "output") &&
              check(a->GetTensorTypeAndShape(output, &info), "output shape") &&
              check(a->GetDimensionsCount(info, &dims_n), "output dims");
    if (ok && dims_n >= 2 && dims_n <= 4) ok = check(a->GetDimensions(info, dims, dims_n), "output dims");
    if (info != nullptr) a->ReleaseTensorTypeAndShapeInfo(info);
    if (!ok || dims_n < 2) {
        a->ReleaseValue(output);
        return false;
    }
    out->height = static_cast<int>(dims[dims_n - 2]);
    out->width = static_cast<int>(dims[dims_n - 1]);
    const size_t n = static_cast<size_t>(out->width) * static_cast<size_t>(out->height);
    out->depth.assign(data, data + n);
    a->ReleaseValue(output);

    // Percentiles from a histogram between the extremes, inside the video only (the page around
    // it is flat anyway): a few odd pixels do not squash the rest.
    const int xs = std::clamp(static_cast<int>(out->video[0] * out->width), 0, out->width - 1);
    const int xe = std::clamp(static_cast<int>(out->video[2] * out->width), xs + 1, out->width);
    const int ys = std::clamp(static_cast<int>(out->video[1] * out->height), 0, out->height - 1);
    const int ye = std::clamp(static_cast<int>(out->video[3] * out->height), ys + 1, out->height);
    auto at = [&](int x, int y) { return out->depth[static_cast<size_t>(y) * static_cast<size_t>(out->width) + static_cast<size_t>(x)]; };
    float lo = INFINITY, hi = -INFINITY;
    for (int y = ys; y < ye; ++y) {
        for (int x = xs; x < xe; ++x) {
            lo = std::min(lo, at(x, y));
            hi = std::max(hi, at(x, y));
        }
    }
    if (!(hi > lo)) {
        out->low = std::isfinite(lo) ? lo : 0.0f;
        out->high = out->low + 1.0f;
        return true;
    }
    constexpr int kBins = 1024;
    int histogram[kBins] = {};
    const float k = (kBins - 1) / (hi - lo);
    for (int y = ys; y < ye; ++y) {
        for (int x = xs; x < xe; ++x) histogram[static_cast<int>((at(x, y) - lo) * k)] += 1;
    }
    const size_t count = static_cast<size_t>(xe - xs) * static_cast<size_t>(ye - ys);
    size_t acc = 0;
    int p2 = 0, p98 = kBins - 1;
    bool found2 = false;
    for (int i = 0; i < kBins; ++i) {
        acc += static_cast<size_t>(histogram[i]);
        if (!found2 && acc >= count * 2 / 100) {
            p2 = i;
            found2 = true;
        }
        if (acc >= count * 98 / 100) {
            p98 = i;
            break;
        }
    }
    out->low = lo + static_cast<float>(p2) / k;
    out->high = std::max(lo + static_cast<float>(p98 + 1) / k, out->low + 1e-3f);
    return true;
}

// A new shot: the brightness of a coarse grid changes a lot from the frame before, or much
// more than it has been changing (a fast pan changes it steadily). As on macOS.
bool DepthEngine::scene_cut(const DepthInput& in) {
    constexpr int gw = 48, gh = 36;
    std::vector<uint8_t> thumb(gw * gh);
    for (int gy = 0; gy < gh; ++gy) {
        const int y = gy * in.height / gh + in.height / (2 * gh);
        for (int gx = 0; gx < gw; ++gx) {
            const int x = gx * in.width / gw + in.width / (2 * gw);
            const uint8_t* px = &in.bgra[(static_cast<size_t>(y) * static_cast<size_t>(in.width) + static_cast<size_t>(x)) * 4];
            thumb[static_cast<size_t>(gy * gw + gx)] = static_cast<uint8_t>((px[2] * 54 + px[1] * 183 + px[0] * 19) >> 8);
        }
    }
    bool reset;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        reset = reset_scene_;
        reset_scene_ = false;
    }
    if (reset || previous_thumb_.size() != thumb.size()) {
        previous_thumb_ = std::move(thumb);
        diff_average_ = 0;
        if (reset) video_area_.reset();
        return true;
    }
    int sum = 0;
    for (size_t i = 0; i < thumb.size(); ++i) sum += std::abs(static_cast<int>(thumb[i]) - static_cast<int>(previous_thumb_[i]));
    previous_thumb_ = std::move(thumb);
    const float diff = static_cast<float>(sum) / static_cast<float>(gw * gh * 255);
    const bool cut = diff > 0.2f || (diff > 0.08f && diff > 4 * diff_average_);
    if (!cut) diff_average_ += (diff - diff_average_) * 0.1f;
    return cut;
}

void VideoArea::reset() {
    previous_.clear();
    std::fill(energy_.begin(), energy_.end(), 0.0f);
    moving_ = 0;
    rect_[0] = rect_[1] = 0.0f;
    rect_[2] = rect_[3] = 1.0f;
}

const float* VideoArea::update(const std::vector<uint8_t>& grid, bool cut) {
    const size_t n = static_cast<size_t>(kColumns * kRows);
    if (grid.size() != n) return rect_;
    if (previous_.size() != n || cut) {
        previous_ = grid;
        return rect_;
    }
    std::vector<float> diff(n);
    float total = 0;
    for (size_t i = 0; i < n; ++i) {
        diff[i] = static_cast<float>(std::abs(static_cast<int>(grid[i]) - static_cast<int>(previous_[i]))) / 255.0f;
        total += diff[i];
    }
    previous_ = grid;
    if (total / static_cast<float>(n) < 0.0005f) return rect_;  // nothing moving: paused or a still page
    moving_ += 1;
    for (size_t i = 0; i < n; ++i) energy_[i] += (std::min(diff[i], 0.1f) - energy_[i]) * 0.05f;
    if (moving_ < 10) return rect_;  // half a second of motion before deciding
    int cols[kColumns] = {}, rows[kRows] = {};
    for (int y = 0; y < kRows; ++y) {
        for (int x = 0; x < kColumns; ++x) {
            if (energy_[static_cast<size_t>(y * kColumns + x)] > 0.0008f) {
                cols[x] += 1;
                rows[y] += 1;
            }
        }
    }
    const int col_min = std::max(2, kRows / 7), row_min = std::max(2, kColumns / 7);
    int x0 = -1, x1 = -1, y0 = -1, y1 = -1;
    for (int x = 0; x < kColumns; ++x) {
        if (cols[x] >= col_min) {
            if (x0 < 0) x0 = x;
            x1 = x;
        }
    }
    for (int y = 0; y < kRows; ++y) {
        if (rows[y] >= row_min) {
            if (y0 < 0) y0 = y;
            y1 = y;
        }
    }
    if (x0 < 0 || y0 < 0) return rect_;
    float t[4] = {static_cast<float>(x0) / kColumns, static_cast<float>(y0) / kRows, static_cast<float>(x1 + 1) / kColumns,
                  static_cast<float>(y1 + 1) / kRows};
    const float area = (t[2] - t[0]) * (t[3] - t[1]);
    if (area < 0.06f || area > 0.85f) {  // a spinner / the whole panel
        t[0] = t[1] = 0.0f;
        t[2] = t[3] = 1.0f;
    }
    for (int i = 0; i < 4; ++i) rect_[i] += (t[i] - rect_[i]) * 0.12f;  // glides there
    return rect_;
}

}  // namespace rayneo::win
