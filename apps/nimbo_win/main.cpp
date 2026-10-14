// nimbo_win: the Windows spatial server. Captures an existing display with DXGI Desktop
// Duplication and renders it as a panel fixed in space on the RayNeo GT, half-SBS, with
// Direct3D 11 (W2 of docs/WINDOWS_PLAN.md; the counterpart of `Nimbo --displays 0`).
//
//   nimbo_win                     glasses display, IMU tracking
//   nimbo_win --windowed --sim    no glasses: a window on the main screen, simulated head
//   nimbo_win --help

#include <windows.h>

#include <d3d11_4.h>
#include <dxgi1_3.h>
#include <timeapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "desktop_duplication.h"
#include "displays.h"
#include "rayneo/core/head_calibration.h"
#include "rayneo/platform/rayneo_engine.h"
#include "spatial_renderer.h"
#include "virtual_displays.h"
#include "window_gatherer.h"

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

using Microsoft::WRL::ComPtr;
namespace core = rayneo::core;
namespace win = rayneo::win;

namespace {

// ---------------------------------------------------------------------------------------
// Options (same names as the macOS CLI where they mean the same thing)
// ---------------------------------------------------------------------------------------

struct Options {
    std::optional<bool> side_by_side;  // nil: SBS on the glasses, mono in --windowed
    float ipd_mm = 63;
    float distance_m = 2.0f;
    std::optional<float> arc_deg;  // nil: 38 per virtual panel, 50 for single-display capture
    float gap_deg = 1;
    float curvature = 0;
    float fov_y_deg = 24.5f;
    bool fov_from_cli = false;
    float dead_zone_deg = 1.0f;
    float k1 = 0, k2 = 0;
    std::string source = "auto";
    std::string calibration_path = "calibration.json";
    bool calibration_required = false;
    bool refresh120 = true;
    bool windowed = false;
    bool sim = false;
    float sim_sweep_deg = 0;
    double duration_s = 0;

    int displays = 2;                      // virtual monitors; 0 = show an existing display
    std::vector<core::GridCell> layout;    // grid cell per monitor; empty = one row
    int vd_width = 1920, vd_height = 1080;  // a mode the driver offers (vdd_settings.xml)
    int vd_scale = 150;                    // Windows scaling of the virtual monitors, percent
    bool fit = false;                      // size panels so all of them are visible at once
    bool gather_windows = true;            // move windows off the glasses display into panel 1
    bool gaze_cursor = true;               // cursor jumps to the panel you look at
    float dim_others = 1;                  // brightness of the panels you are not looking at
    bool restore_only = false;             // --restore: undo a crashed run's display changes

    float panel_arc_deg() const { return arc_deg.value_or(displays > 0 ? 38.0f : 50.0f); }
};

const char* kUsage = R"(usage: nimbo_win [options]
layout
  --displays N         virtual monitors around you (2; needs the Virtual Display Driver);
                       0 = show an existing display
  --layout C,R;C,R...  grid cell per monitor (row 0 = top), e.g. "0,0;0,1" = two stacked
  --vd-size WxH        virtual monitor resolution, one the driver offers (1920x1080)
  --vd-scale PCT       Windows scaling of the virtual monitors (150: large, sharp text)
  --fit                all panels visible at once (each at a fraction of full resolution)
  --arc DEG            horizontal size of each panel (38; 50 with --displays 0)
  --gap DEG            space between panels (1)
  --no-gather          do not move windows from the glasses display to panel 1
  --no-gaze-cursor     keep the cursor where it is when you look at another panel
  --dim B              brightness of the panels you are not looking at, 0..1 (1 = off)
  --curved             curved panels (default: flat monitors angled towards you)
  --curvature K        0 flat .. 1 fully curved around you
  --distance M         panel distance in metres (2.0)
optics
  --mono / --sbs       single view (glasses 2D) / half side-by-side (glasses 3D)
  --ipd MM             interpupillary distance (63)
  --fov DEG            per-eye vertical FOV of the optics (24.5, or the value tuned with
                       Ctrl+Alt+- / Ctrl+Alt+= which is remembered)
  --k1 K --k2 K        barrel pre-distortion (0 = off)
tracking
  --calib FILE         sensor_to_head calibration (default ./calibration.json if present)
  --dead-zone DEG      stabilizer dead zone (1.0): smaller follows sooner, larger is stiller
  --sim                no glasses: simulated head (Ctrl+Alt+arrows to look around)
  --sim-sweep DEG      sim: automatic left/right sweep of +-DEG
display
  --windowed           render in a window on the main screen instead of the glasses
  --source S           --displays 0 only: auto | glasses | primary | <index from the list>
  --60hz               keep the glasses' current refresh rate (default: 120 Hz)
  --duration S         exit after S seconds
  --restore            undo the display changes of a run that crashed, then exit
hotkeys: Ctrl+Alt+C recenter, Ctrl+Alt+Q quit,
         Ctrl+Alt+- / Ctrl+Alt+= field of view (tune until the panel stays put when turning),
         Ctrl+Alt+arrows look around (--sim)
)";

// "A<sep>B" with two integers, nothing else.
bool parse_pair(const char* text, char sep, int* a, int* b) {
    char* end = nullptr;
    const long x = std::strtol(text, &end, 10);
    if (end == text || *end != sep) return false;
    const char* rest = end + 1;
    const long y = std::strtol(rest, &end, 10);
    if (end == rest || *end != '\0') return false;
    *a = static_cast<int>(x);
    *b = static_cast<int>(y);
    return true;
}

std::optional<Options> parse(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto value = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        auto number = [&](float* out) {
            const char* v = value();
            if (v == nullptr) return false;
            char* end = nullptr;
            *out = std::strtof(v, &end);
            return end != v && *end == '\0';
        };
        float f = 0;
        if (a == "--displays" && number(&f) && f >= 0 && f <= 8) o.displays = static_cast<int>(f);
        else if (a == "--layout") {
            const char* v = value();
            if (v == nullptr) return std::nullopt;
            o.layout.clear();
            std::stringstream ss(v);
            std::string cell;
            while (std::getline(ss, cell, ';')) {
                core::GridCell c;
                if (!parse_pair(cell.c_str(), ',', &c.col, &c.row)) return std::nullopt;
                o.layout.push_back(c);
            }
            if (o.layout.empty() || o.layout.size() > 8) return std::nullopt;
            o.displays = static_cast<int>(o.layout.size());
        }
        else if (a == "--vd-size") {
            const char* v = value();
            if (v == nullptr || !parse_pair(v, 'x', &o.vd_width, &o.vd_height) || o.vd_width < 640 ||
                o.vd_height < 480) {
                return std::nullopt;
            }
        }
        else if (a == "--vd-scale" && number(&f)) o.vd_scale = static_cast<int>(f);
        else if (a == "--fit") o.fit = true;
        else if (a == "--no-gather") o.gather_windows = false;
        else if (a == "--no-gaze-cursor") o.gaze_cursor = false;
        else if (a == "--dim" && number(&f)) o.dim_others = std::fmax(0.0f, std::fmin(1.0f, f));
        else if (a == "--restore") o.restore_only = true;
        else if (a == "--arc" && number(&f)) o.arc_deg = f;
        else if (a == "--gap" && number(&f)) o.gap_deg = f;
        else if (a == "--curved") o.curvature = 1;
        else if (a == "--curvature" && number(&f) && f >= 0 && f <= 1) o.curvature = f;
        else if (a == "--distance" && number(&f)) o.distance_m = f;
        else if (a == "--mono") o.side_by_side = false;
        else if (a == "--sbs") o.side_by_side = true;
        else if (a == "--ipd" && number(&f)) o.ipd_mm = f;
        else if (a == "--fov" && number(&f)) { o.fov_y_deg = f; o.fov_from_cli = true; }
        else if (a == "--k1" && number(&f)) o.k1 = f;
        else if (a == "--k2" && number(&f)) o.k2 = f;
        else if (a == "--dead-zone" && number(&f)) o.dead_zone_deg = f;
        else if (a == "--calib") {
            const char* v = value();
            if (v == nullptr) return std::nullopt;
            o.calibration_path = v;
            o.calibration_required = true;
        }
        else if (a == "--sim") o.sim = true;
        else if (a == "--sim-sweep" && number(&f)) { o.sim = true; o.sim_sweep_deg = f; }
        else if (a == "--windowed") o.windowed = true;
        else if (a == "--source") {
            const char* v = value();
            if (v == nullptr) return std::nullopt;
            o.source = v;
        }
        else if (a == "--60hz") o.refresh120 = false;
        else if (a == "--duration" && number(&f)) o.duration_s = f;
        else return std::nullopt;
    }
    return o;
}

void logln(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::vprintf(fmt, args);
    va_end(args);
    std::printf("\n");
    std::fflush(stdout);
}

// ---------------------------------------------------------------------------------------
// Clocks: the engine stamps poses with std::chrono::steady_clock; DXGI frame statistics use
// QPC. Map one onto the other once (on MSVC both are QPC, the offset is ~0).
// ---------------------------------------------------------------------------------------

uint64_t steady_ns() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

struct QpcClock {
    double ns_per_tick = 0;
    int64_t offset_ns = 0;  // steady - qpc

    void init() {
        LARGE_INTEGER f, now;
        QueryPerformanceFrequency(&f);
        ns_per_tick = 1e9 / static_cast<double>(f.QuadPart);
        QueryPerformanceCounter(&now);
        offset_ns = static_cast<int64_t>(steady_ns()) - static_cast<int64_t>(static_cast<double>(now.QuadPart) * ns_per_tick);
    }
    uint64_t to_steady(int64_t qpc) const {
        return static_cast<uint64_t>(static_cast<int64_t>(static_cast<double>(qpc) * ns_per_tick) + offset_ns);
    }
};

// ---------------------------------------------------------------------------------------
// Head pose: the glasses' IMU through the C ABI engine, or a simulator.
// ---------------------------------------------------------------------------------------

struct PoseSample {
    core::Quat orientation;  // core frame (X fwd, Y left, Z up), head since recenter
    core::Vec3 angular_velocity;
    uint64_t host_ns = 0;
    bool tracking = false;
};

class PoseSource {
public:
    virtual ~PoseSource() = default;
    virtual void recenter() = 0;
    virtual std::optional<PoseSample> latest() = 0;
    virtual void nudge(float, float) {}
};

class ImuPose final : public PoseSource {
public:
    explicit ImuPose(float dead_zone_deg) {
        rayneo_config cfg = rayneo_default_config();
        if (dead_zone_deg > 0) cfg.dead_zone_deg = dead_zone_deg;
        engine_ = rayneo_engine_create(&cfg);
    }
    ~ImuPose() override {
        rayneo_engine_stop(engine_);
        rayneo_engine_destroy(engine_);
    }
    bool load_calibration(const std::string& path) {
        std::ifstream f(path);
        if (!f) return false;
        std::stringstream ss;
        ss << f.rdbuf();
        const auto q = core::calibration_from_json(ss.str());
        if (!q) return false;
        rayneo_engine_set_sensor_to_head(engine_, q->w, q->x, q->y, q->z);
        return true;
    }
    bool start(std::string* error) {
        if (rayneo_engine_start(engine_) == RAYNEO_OK) return true;
        *error = rayneo_engine_last_error(engine_);
        return false;
    }
    void recenter() override { rayneo_engine_recenter(engine_); }
    std::optional<PoseSample> latest() override {
        rayneo_pose p;
        if (!rayneo_engine_latest_pose(engine_, &p)) return std::nullopt;
        return PoseSample{{p.qw, p.qx, p.qy, p.qz}, {p.wx, p.wy, p.wz}, p.host_time_ns, p.tracking};
    }

private:
    rayneo_engine* engine_ = nullptr;
};

class SimPose final : public PoseSource {
public:
    explicit SimPose(float sweep_deg) : sweep_deg_(sweep_deg), t0_(steady_ns()) {}
    void recenter() override {
        yaw_ = 0;
        pitch_ = 0;
    }
    void nudge(float yaw, float pitch) override {
        yaw_ = std::fmax(-170.0f, std::fmin(170.0f, yaw_.load() + yaw));
        pitch_ = std::fmax(-80.0f, std::fmin(80.0f, pitch_.load() + pitch));
    }
    std::optional<PoseSample> latest() override {
        constexpr float d2r = 0.017453292519943295f;
        float yaw = yaw_;
        float rate = 0;
        if (sweep_deg_ > 0) {
            const double w = 2 * 3.141592653589793 / 12.0;
            const double t = static_cast<double>(steady_ns() - t0_) * 1e-9;
            yaw += sweep_deg_ * static_cast<float>(std::sin(w * t));
            rate = sweep_deg_ * static_cast<float>(w * std::cos(w * t)) * d2r;
        }
        const core::Quat q_yaw = core::Quat::from_axis_angle({0, 0, 1}, yaw * d2r);       // positive = left
        const core::Quat q_pitch = core::Quat::from_axis_angle({0, -1, 0}, pitch_ * d2r);  // look up = about -Y
        return PoseSample{q_yaw * q_pitch, {0, 0, rate}, steady_ns(), true};
    }

private:
    std::atomic<float> yaw_{0}, pitch_{0};
    float sweep_deg_;
    uint64_t t0_;
};

// ---------------------------------------------------------------------------------------
// Persisted settings (the FOV tuned with the hotkeys), HKCU\Software\Nimbo.
// ---------------------------------------------------------------------------------------

std::optional<float> load_fov() {
    DWORD v = 0, size = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Nimbo", L"FovY100", RRF_RT_REG_DWORD, nullptr, &v, &size) !=
        ERROR_SUCCESS) {
        return std::nullopt;
    }
    return static_cast<float>(v) / 100.0f;
}

void save_fov(float fov) {
    const DWORD v = static_cast<DWORD>(std::lround(fov * 100.0f));
    RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\Nimbo", L"FovY100", REG_DWORD, &v, sizeof(v));
}

// ---------------------------------------------------------------------------------------
// Server
// ---------------------------------------------------------------------------------------

enum HotKey { kRecenter = 1, kQuit, kFovDown, kFovUp, kLeft, kRight, kUp, kDown };

std::atomic<bool> g_quit{false};
HWND g_hwnd = nullptr;

struct LatencyStats {
    int frames = 0, idle = 0;
    double pose_age_sum = 0, pose_age_max = 0;  // sample -> encode (ms)
    double pred_sum = 0;                        // horizon actually extrapolated (ms)
    int presented_n = 0;
    double presented_sum = 0, presented_max = 0;  // sample -> scanout (DXGI frame statistics)
    double target_err_sum = 0;                    // scanout - predicted target (ms)
    bool tracking = false;
};

class Server {
public:
    explicit Server(Options o) : opt_(std::move(o)) {}
    ~Server() { shutdown(); }

    bool start();
    void run();
    void on_hotkey(int id);
    void on_display_change() { display_changed_ = true; }

private:
    bool create_window(const RECT& rect);
    bool create_swap_chain();
    bool create_back_buffer_view();
    uint64_t estimate_target(uint64_t now);
    bool frame();
    void collect_presented();
    void periodic();
    void handle_display_change();
    void adjust_fov(float delta);
    bool setup_virtual_displays();
    const win::OutputInfo* find_output(const std::wstring& gdi_name) const;
    void refresh_rects();
    void follow_gaze(uint64_t now);
    void shutdown();

    Options opt_;
    QpcClock qpc_;
    std::vector<win::OutputInfo> outputs_;
    std::wstring glasses_gdi_;  // empty in --windowed
    bool mode_changed_ = false;
    int refresh_hz_ = 60;

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<IDXGISwapChain2> swap_chain_;
    ComPtr<ID3D11RenderTargetView> back_rtv_;
    ComPtr<IDXGIOutput> vblank_output_;
    HANDLE waitable_ = nullptr;
    int width_ = 0, height_ = 0;

    std::unique_ptr<PoseSource> pose_;
    bool imu_ = false;
    std::vector<std::unique_ptr<win::DesktopDuplication>> captures_;
    win::SpatialRenderer renderer_;

    // Virtual monitors (W3).
    bool virtual_ = false;
    win::DisplayArrangement arrangement_;
    std::vector<std::wstring> panel_names_;  // GDI name per panel, left -> right
    std::vector<RECT> panel_rects_;          // desktop rectangle per panel
    RECT glasses_rect_{};
    POINT last_cursor_{};
    uint64_t last_cursor_move_ns_ = 0, next_gaze_ns_ = 0;
    int last_gaze_panel_ = -2;
    bool redraw_ = true;
    bool display_changed_ = false;

    struct Sent {
        uint64_t sample_ns = 0, target_ns = 0;
    };
    Sent sent_[128];
    UINT last_stats_present_ = 0;

    LatencyStats stats_;
    uint64_t next_stats_ns_ = 0, start_ns_ = 0, last_pose_ns_ = 0;
    uint64_t last_capture_frames_ = 0;
    int stale_pose_s_ = 0, calib_s_ = 0;
    bool shut_ = false;
};

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* server = reinterpret_cast<Server*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_HOTKEY:
        if (server != nullptr) server->on_hotkey(static_cast<int>(wp));
        return 0;
    case WM_DISPLAYCHANGE:
        if (server != nullptr) server->on_display_change();
        return 0;
    case WM_CLOSE:
        g_quit = true;
        return 0;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

BOOL WINAPI console_handler(DWORD type) {
    // Ctrl+C / closing the console: stop cleanly so stream-off is sent and the mode restored.
    g_quit = true;
    if (g_hwnd != nullptr) PostMessageW(g_hwnd, WM_NULL, 0, 0);
    // Closing the console ends the process when this returns: give the main loop time to clean up.
    if (type != CTRL_C_EVENT && type != CTRL_BREAK_EVENT) Sleep(2000);
    return TRUE;
}

bool Server::start() {
    qpc_.init();
    outputs_ = win::enumerate_outputs();
    logln("displays:");
    for (size_t i = 0; i < outputs_.size(); ++i) {
        const auto& o = outputs_[i];
        std::string names;
        for (const auto& m : o.monitors) names += (names.empty() ? "" : " + ") + win::narrow(m);
        logln("  [%zu] %s %dx%d at %ld,%ld '%s' on %s%s%s", i, win::narrow(o.gdi_name).c_str(), o.width(), o.height(),
            o.desktop.left, o.desktop.top, names.c_str(), win::narrow(o.adapter_name).c_str(),
            o.primary ? " [primary]" : "", o.glasses ? " [glasses]" : "");
    }

    const win::OutputInfo* glasses = win::find_glasses(outputs_);
    if (!opt_.windowed) {
        if (glasses == nullptr) {
            logln("RayNeo GT display (SmartGlasses) not found. Without the glasses: --windowed --sim");
            return false;
        }
        glasses_gdi_ = glasses->gdi_name;
    }

    // Virtual monitors first: arranging displays resets temporary modes such as the glasses'
    // native mode below.
    if (opt_.displays > 0 && !win::VirtualDisplayDriver::available()) {
        logln("virtual monitors: the Virtual Display Driver is not installed, showing an existing display instead"
              " (install it to get --displays: see docs/WINDOWS_PLAN.md, W3)");
        opt_.displays = 0;
    }
    if (opt_.displays > 0 && !setup_virtual_displays()) return false;

    glasses = glasses_gdi_.empty() ? nullptr : find_output(glasses_gdi_);
    if (glasses != nullptr) {
        if (glasses->mirrored()) {
            logln("glasses: duplicating another display. Use Extend (Win+P) for the native mode and your own desktop");
        } else if (const auto m = win::set_native_mode(glasses_gdi_, opt_.refresh120 ? 120 : 60)) {
            logln("glasses mode: %dx%d @%d Hz (your setting is restored on exit)", m->width, m->height, m->hz);
            mode_changed_ = true;
            Sleep(1000);  // let the reconfiguration settle
            outputs_ = win::enumerate_outputs();
            glasses = find_output(glasses_gdi_);
            if (glasses == nullptr) {
                logln("glasses display lost after the mode change");
                return false;
            }
        } else {
            logln("glasses mode: unchanged");
        }
        if (const auto m = win::current_mode(glasses_gdi_)) refresh_hz_ = m->hz;
    } else if (const auto m = win::current_mode(L"")) {
        refresh_hz_ = m->hz;  // primary display
    }

    // Sources: the virtual monitors, or the one existing display that the panel shows.
    std::vector<const win::OutputInfo*> sources;
    if (virtual_) {
        for (const auto& name : panel_names_) {
            if (const auto* o = find_output(name)) sources.push_back(o);
        }
    } else {
        const win::OutputInfo* source = nullptr;
        if (opt_.source == "glasses") {
            source = glasses;
        } else if (opt_.source == "primary") {
            for (const auto& o : outputs_) {
                if (o.primary) source = &o;
            }
        } else if (opt_.source == "auto") {
            // Extended desktop: the primary display (or the first one that is not the glasses).
            // Mirrored: the glasses output itself; the render window is excluded from capture,
            // so the desktop behind it is what gets captured.
            for (const auto& o : outputs_) {
                if (!o.glasses && (source == nullptr || o.primary)) source = &o;
            }
            if (source == nullptr) source = glasses;
        } else {
            char* end = nullptr;
            const unsigned long i = std::strtoul(opt_.source.c_str(), &end, 10);
            if (end != opt_.source.c_str() && *end == '\0' && i < outputs_.size()) source = &outputs_[i];
        }
        if (source == nullptr) {
            logln("bad --source %s", opt_.source.c_str());
            return false;
        }
        sources.push_back(source);
        panel_names_ = {source->gdi_name};
    }
    if (sources.empty()) {
        logln("no display to show");
        return false;
    }

    // Render and present on the GPU that owns the captured outputs (each capture's own device
    // must be there, and frames are shared with this one). A glasses output on another GPU
    // costs a copy.
    const win::OutputInfo* first = sources.front();
    DXGI_ADAPTER_DESC1 first_desc{};
    first->adapter->GetDesc1(&first_desc);
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                        D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL level{};
    HRESULT hr = D3D11CreateDevice(first->adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 4, D3D11_SDK_VERSION, &device_, &level,
                                   &ctx_);
    if (FAILED(hr)) {
        logln("D3D11CreateDevice failed (0x%08lx)", static_cast<unsigned long>(hr));
        return false;
    }
    logln("gpu: %s (feature level %x)", win::narrow(first->adapter_name).c_str(), static_cast<unsigned>(level));

    // Pose first: the IMU needs ~6 s of rest, let it start while the rest comes up.
    if (opt_.sim) {
        pose_ = std::make_unique<SimPose>(opt_.sim_sweep_deg);
        logln("tracking: simulated head%s", opt_.sim_sweep_deg > 0 ? " (sweep)" : " (Ctrl+Alt+arrows)");
    } else {
        auto imu = std::make_unique<ImuPose>(opt_.dead_zone_deg);
        const bool calibrated = imu->load_calibration(opt_.calibration_path);
        if (!calibrated && opt_.calibration_required) {
            logln("invalid or missing calibration: %s", opt_.calibration_path.c_str());
            return false;
        }
        logln("calibration: %s", calibrated ? opt_.calibration_path.c_str() : "none (identity)");
        std::string error;
        if (!imu->start(&error)) {
            logln("imu: %s", error.c_str());
            return false;
        }
        logln("imu: streaming, leave the glasses still on the table ~6 s until TRACK");
        pose_ = std::move(imu);
        imu_ = true;
    }

    // Windows left on the glasses surface would be invisible (it shows the render): bring
    // them to panel 1, and the cursor with them.
    if (virtual_ && glasses != nullptr) {
        const RECT target = sources.front()->desktop;
        if (opt_.gather_windows) {
            logln("windows: moved %d window(s) from the glasses display to panel 1",
                  win::gather_windows(glasses->desktop, target, nullptr));
        }
        SetCursorPos((target.left + target.right) / 2, (target.top + target.bottom) / 2);
    }

    RECT rect{80, 80, 80 + 1280, 80 + 720};
    if (!opt_.windowed) rect = glasses->desktop;
    if (!create_window(rect) || !create_swap_chain()) return false;

    std::string error;
    if (!renderer_.init(device_.Get(), &error)) {
        logln("renderer: %s", error.c_str());
        return false;
    }
    const int panels = static_cast<int>(sources.size());
    auto& ro = renderer_.options;
    ro.side_by_side = opt_.side_by_side.value_or(!opt_.windowed);
    ro.ipd_m = opt_.ipd_mm / 1000.0f;
    ro.distance_m = opt_.distance_m;
    ro.arc_deg = opt_.panel_arc_deg();
    ro.gap_deg = opt_.gap_deg;
    ro.curvature = opt_.curvature;
    ro.fov_y_deg = opt_.fov_y_deg;
    if (!opt_.fov_from_cli) {
        if (const auto saved = load_fov()) ro.fov_y_deg = *saved;
    }
    ro.k1 = opt_.k1;
    ro.k2 = opt_.k2;
    ro.dim_others = opt_.dim_others;
    renderer_.set_panel_count(panels);
    if (static_cast<int>(opt_.layout.size()) == panels) renderer_.set_grid(opt_.layout);
    if (opt_.fit && !opt_.arc_deg) {
        int columns = panels;
        if (!opt_.layout.empty()) {
            std::vector<int> cols;
            for (const auto& c : opt_.layout) {
                if (std::find(cols.begin(), cols.end(), c.col) == cols.end()) cols.push_back(c.col);
            }
            columns = static_cast<int>(cols.size());
        }
        ro.arc_deg = renderer_.fit_arc_deg(columns);
    }
    logln("render: %s, %d %s panel(s) of %.1f deg gap %.1f deg at %.1f m, ipd %.0f mm, fov %.1f deg, k1 %.3f k2 %.3f",
          ro.side_by_side ? "half-SBS" : "mono", panels, ro.curvature > 0 ? "curved" : "flat", ro.arc_deg, ro.gap_deg,
          ro.distance_m, opt_.ipd_mm, ro.fov_y_deg, ro.k1, ro.k2);

    for (int i = 0; i < panels; ++i) {
        const win::OutputInfo* src = sources[static_cast<size_t>(i)];
        DXGI_ADAPTER_DESC1 desc{};
        src->adapter->GetDesc1(&desc);
        if (desc.AdapterLuid.LowPart != first_desc.AdapterLuid.LowPart ||
            desc.AdapterLuid.HighPart != first_desc.AdapterLuid.HighPart) {
            logln("capture: %s is on another GPU (%s): not supported yet", win::narrow(src->gdi_name).c_str(),
                  win::narrow(src->adapter_name).c_str());
            return false;
        }
        auto cap = std::make_unique<win::DesktopDuplication>();
        if (!cap->start(src->adapter.Get(), src->output.Get(), device_.Get(), &error)) {
            logln("capture: %s", error.c_str());
            return false;
        }
        renderer_.set_source(i, cap.get());
        captures_.push_back(std::move(cap));
        logln("capture: panel %d <- %s %dx%d", i + 1, win::narrow(src->gdi_name).c_str(), src->width(), src->height());
    }
    refresh_rects();

    const UINT mods = MOD_CONTROL | MOD_ALT | MOD_NOREPEAT;
    struct Key {
        int id;
        UINT vk;
        const char* name;
    };
    std::vector<Key> keys = {{kRecenter, 'C', "Ctrl+Alt+C"}, {kQuit, 'Q', "Ctrl+Alt+Q"},
                             {kFovDown, VK_OEM_MINUS, "Ctrl+Alt+-"}, {kFovUp, VK_OEM_PLUS, "Ctrl+Alt+="}};
    if (opt_.sim) {
        keys.push_back({kLeft, VK_LEFT, "Ctrl+Alt+Left"});
        keys.push_back({kRight, VK_RIGHT, "Ctrl+Alt+Right"});
        keys.push_back({kUp, VK_UP, "Ctrl+Alt+Up"});
        keys.push_back({kDown, VK_DOWN, "Ctrl+Alt+Down"});
    }
    for (const auto& k : keys) {
        if (!RegisterHotKey(g_hwnd, k.id, mods, k.vk)) logln("hotkey %s: in use by another app", k.name);
    }
    logln("hotkeys: Ctrl+Alt+C recenter, Ctrl+Alt+Q quit, Ctrl+Alt+- / = field of view");

    start_ns_ = steady_ns();
    next_stats_ns_ = start_ns_ + 1000000000ull;
    return true;
}

bool Server::create_window(const RECT& rect) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW (narrow in non-UNICODE builds)
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = L"NimboRender";
    RegisterClassExW(&wc);

    RECT r = rect;
    DWORD style = WS_POPUP;
    DWORD ex = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;  // above everything, no taskbar entry
    if (opt_.windowed) {
        style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
        ex = 0;
        AdjustWindowRectEx(&r, style, FALSE, ex);
    }
    g_hwnd = CreateWindowExW(ex, wc.lpszClassName, L"Nimbo (vista previa)", style, r.left, r.top, r.right - r.left,
                             r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
    if (g_hwnd == nullptr) {
        logln("CreateWindowEx failed (%lu)", GetLastError());
        return false;
    }
    SetWindowLongPtrW(g_hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    // Invisible to capture: no feedback loop when the captured display is the glasses one,
    // and screenshots / screen sharing see the desktop instead of the SBS image.
    if (!SetWindowDisplayAffinity(g_hwnd, WDA_EXCLUDEFROMCAPTURE)) {
        logln("capture exclusion unavailable (Windows 10 2004 or newer): keep --source off the glasses display");
    }
    ShowWindow(g_hwnd, opt_.windowed ? SW_SHOW : SW_SHOWNOACTIVATE);
    RECT client{};
    GetClientRect(g_hwnd, &client);
    width_ = client.right - client.left;
    height_ = client.bottom - client.top;
    return true;
}

bool Server::create_swap_chain() {
    ComPtr<IDXGIDevice> dxgi_device;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    device_.As(&dxgi_device);
    dxgi_device->GetAdapter(&adapter);
    adapter->GetParent(IID_PPV_ARGS(&factory));

    // Flip model with a waitable object and one frame of latency: the loop wakes when the
    // next frame can be queued and renders with the freshest pose.
    DXGI_SWAP_CHAIN_DESC1 d{};
    d.Width = static_cast<UINT>(width_);
    d.Height = static_cast<UINT>(height_);
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;  // the view is sRGB (linear-light filtering)
    d.SampleDesc.Count = 1;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.BufferCount = 2;
    d.Scaling = DXGI_SCALING_STRETCH;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    d.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    d.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    ComPtr<IDXGISwapChain1> sc1;
    const HRESULT hr = factory->CreateSwapChainForHwnd(device_.Get(), g_hwnd, &d, nullptr, nullptr, &sc1);
    if (FAILED(hr) || FAILED(sc1.As(&swap_chain_))) {
        logln("CreateSwapChainForHwnd failed (0x%08lx)", static_cast<unsigned long>(hr));
        return false;
    }
    factory->MakeWindowAssociation(g_hwnd, DXGI_MWA_NO_ALT_ENTER);
    swap_chain_->SetMaximumFrameLatency(1);
    waitable_ = swap_chain_->GetFrameLatencyWaitableObject();
    swap_chain_->GetContainingOutput(&vblank_output_);
    logln("surface: %dx%d px, flip model, target %d Hz", width_, height_, refresh_hz_);
    return create_back_buffer_view();
}

bool Server::create_back_buffer_view() {
    back_rtv_.Reset();
    ComPtr<ID3D11Texture2D> back;
    if (FAILED(swap_chain_->GetBuffer(0, IID_PPV_ARGS(&back)))) return false;
    D3D11_RENDER_TARGET_VIEW_DESC rd{};
    rd.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    return SUCCEEDED(device_->CreateRenderTargetView(back.Get(), &rd, &back_rtv_));
}

// Scanout time of the frame about to be queued: the next vblank after the render budget,
// from the last vblank DXGI reported (or one refresh from now until it reports one).
uint64_t Server::estimate_target(uint64_t now) {
    const double period = 1e9 / std::max(1, refresh_hz_);
    DXGI_FRAME_STATISTICS fs{};
    if (SUCCEEDED(swap_chain_->GetFrameStatistics(&fs)) && fs.SyncQPCTime.QuadPart != 0) {
        const uint64_t vsync = qpc_.to_steady(fs.SyncQPCTime.QuadPart);
        const uint64_t ready = now + 2000000;  // ~2 ms to render and queue
        if (ready > vsync) {
            const double n = std::ceil(static_cast<double>(ready - vsync) / period);
            return vsync + static_cast<uint64_t>(n * period);
        }
    }
    return now + static_cast<uint64_t>(period);
}

void Server::collect_presented() {
    DXGI_FRAME_STATISTICS fs{};
    if (FAILED(swap_chain_->GetFrameStatistics(&fs)) || fs.PresentCount == last_stats_present_ ||
        fs.SyncQPCTime.QuadPart == 0) {
        return;
    }
    last_stats_present_ = fs.PresentCount;
    const Sent& s = sent_[fs.PresentCount % 128];
    if (s.sample_ns == 0) return;
    const uint64_t shown = qpc_.to_steady(fs.SyncQPCTime.QuadPart);
    if (shown < s.sample_ns) return;
    const double latency = static_cast<double>(shown - s.sample_ns) / 1e6;
    stats_.presented_n += 1;
    stats_.presented_sum += latency;
    stats_.presented_max = std::max(stats_.presented_max, latency);
    stats_.target_err_sum += (static_cast<double>(shown) - static_cast<double>(s.target_ns)) / 1e6;
}

// Renders and presents one frame. False when it was skipped (nothing changed).
bool Server::frame() {
    collect_presented();
    const uint64_t now = steady_ns();
    const uint64_t target = estimate_target(now);
    const auto p = pose_->latest();
    float prediction = 0;
    if (!p) {
        renderer_.invalidate_skipped_frame();
        renderer_.render(back_rtv_.Get(), width_, height_, core::Quat{}, core::Vec3{}, 0, 0);
    } else {
        const bool forced = redraw_;
        redraw_ = false;
        stats_.tracking = p->tracking;
        if (renderer_.can_skip_frame(p->orientation, p->angular_velocity, p->tracking, forced)) {
            stats_.idle += 1;
            return false;
        }
        prediction = renderer_.render(back_rtv_.Get(), width_, height_, p->orientation, p->angular_velocity,
                                      p->host_ns, target);
    }
    const HRESULT hr = swap_chain_->Present(1, 0);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        logln("gpu device lost (0x%08lx): stopping", static_cast<unsigned long>(device_->GetDeviceRemovedReason()));
        g_quit = true;
        return true;
    }
    UINT id = 0;
    if (p && SUCCEEDED(swap_chain_->GetLastPresentCount(&id))) sent_[id % 128] = {p->host_ns, target};
    if (p) {
        const double age = static_cast<double>(static_cast<int64_t>(steady_ns() - p->host_ns)) / 1e6;
        stats_.frames += 1;
        stats_.pose_age_sum += age;
        stats_.pose_age_max = std::max(stats_.pose_age_max, age);
        stats_.pred_sum += prediction;
    }
    return true;
}

void Server::periodic() {
    if (display_changed_) handle_display_change();
    const uint64_t now = steady_ns();
    if (opt_.duration_s > 0 && static_cast<double>(now - start_ns_) * 1e-9 >= opt_.duration_s) g_quit = true;
    if (now >= next_gaze_ns_) {
        next_gaze_ns_ = now + 33000000ull;  // ~30 Hz
        follow_gaze(now);
    }
    if (now < next_stats_ns_) return;
    next_stats_ns_ += 1000000000ull;
    if (next_stats_ns_ < now) next_stats_ns_ = now + 1000000000ull;

    // Unplugged glasses: the IMU stops. Quit, which restores the display mode.
    if (imu_) {
        const auto p = pose_->latest();
        const uint64_t t = p ? p->host_ns : 0;
        stale_pose_s_ = t == last_pose_ns_ ? stale_pose_s_ + 1 : 0;
        last_pose_ns_ = t;
        if (stale_pose_s_ >= 2) {
            logln("glasses IMU stopped (unplugged?): stopping");
            g_quit = true;
        }
    }

    const LatencyStats s = stats_;
    stats_ = LatencyStats{};
    stats_.tracking = s.tracking;
    uint64_t cap = 0;
    for (const auto& c : captures_) cap += c->frames();
    const uint64_t cap_fps = cap - last_capture_frames_;
    last_capture_frames_ = cap;
    if (!s.tracking && imu_ && ++calib_s_ % 3 == 1) {
        logln("CALIB: head tracking starts after ~6 s with the glasses still on the table"
            " (or ~14 s if you are already wearing them)");
    }
    const double n = std::max(1, s.frames);
    const double pn = std::max(1, s.presented_n);
    logln("%s render %3d fps%s | capture %3llu fps (%zu src) | pose age %.1f/%.1f ms | sample->scanout %.1f/%.1f ms"
        " (target err %+.1f) | predicted %.1f ms",
        s.tracking ? "TRACK" : "CALIB", s.frames, s.idle > 0 ? (" (" + std::to_string(s.idle) + " idle)").c_str() : "",
        static_cast<unsigned long long>(cap_fps), captures_.size(), s.pose_age_sum / n, s.pose_age_max, s.presented_sum / pn,
        s.presented_max, s.target_err_sum / pn, s.pred_sum / n);
}

// Keeps the render window on the glasses whatever Windows does with the layout.
void Server::handle_display_change() {
    display_changed_ = false;
    if (glasses_gdi_.empty()) return;
    outputs_ = win::enumerate_outputs();
    const win::OutputInfo* glasses = nullptr;
    for (const auto& o : outputs_) {
        if (o.gdi_name == glasses_gdi_ && o.glasses) glasses = &o;
    }
    if (glasses == nullptr) {
        logln("glasses display disconnected: stopping");
        g_quit = true;
        return;
    }
    if (const auto m = win::current_mode(glasses_gdi_)) refresh_hz_ = m->hz;
    const RECT r = glasses->desktop;
    SetWindowPos(g_hwnd, HWND_TOPMOST, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOACTIVATE);
    if (r.right - r.left != width_ || r.bottom - r.top != height_) {
        width_ = r.right - r.left;
        height_ = r.bottom - r.top;
        back_rtv_.Reset();
        ctx_->OMSetRenderTargets(0, nullptr, nullptr);
        swap_chain_->ResizeBuffers(0, static_cast<UINT>(width_), static_cast<UINT>(height_), DXGI_FORMAT_UNKNOWN,
                                   DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT);
        create_back_buffer_view();
        logln("surface: %dx%d px", width_, height_);
    }
    vblank_output_.Reset();
    swap_chain_->GetContainingOutput(&vblank_output_);
    refresh_rects();
    redraw_ = true;
}

const win::OutputInfo* Server::find_output(const std::wstring& gdi_name) const {
    for (const auto& o : outputs_) {
        if (o.gdi_name == gdi_name) return &o;
    }
    return nullptr;
}

void Server::refresh_rects() {
    panel_rects_.clear();
    for (const auto& name : panel_names_) {
        const auto* o = find_output(name);
        panel_rects_.push_back(o != nullptr ? o->desktop : RECT{});
    }
    const auto* g = glasses_gdi_.empty() ? nullptr : find_output(glasses_gdi_);
    glasses_rect_ = g != nullptr ? g->desktop : RECT{};
}

// Creates the virtual monitors and arranges them: a grid to the right of your displays,
// the glasses surface tucked off its bottom-right corner. Undone in shutdown().
bool Server::setup_virtual_displays() {
    std::string error;
    if (!arrangement_.save(outputs_, &error)) {
        logln("virtual monitors: %s", error.c_str());
        return false;
    }
    const int n = opt_.displays;
    if (!win::VirtualDisplayDriver::set_count(n, &error)) {
        logln("virtual monitors: %s", error.c_str());
        return false;
    }
    virtual_ = true;  // from here on shutdown() restores the arrangement
    std::vector<const win::OutputInfo*> panels;
    for (int waited = 0; waited < 15000; waited += 250) {  // the driver reloads its adapter
        Sleep(250);
        outputs_ = win::enumerate_outputs();
        panels.clear();
        for (const auto& o : outputs_) {
            if (win::is_virtual_panel(o)) panels.push_back(&o);
        }
        if (static_cast<int>(panels.size()) >= n) break;
    }
    if (static_cast<int>(panels.size()) < n) {
        logln("virtual monitors: %zu of %d appeared (are they off in Settings > Display?)", panels.size(), n);
        if (panels.empty()) return false;
    }
    std::sort(panels.begin(), panels.end(),
              [](const win::OutputInfo* a, const win::OutputInfo* b) { return a->gdi_name < b->gdi_name; });
    panels.resize(std::min<size_t>(panels.size(), static_cast<size_t>(n)));
    panel_names_.clear();
    for (const auto* p : panels) panel_names_.push_back(p->gdi_name);

    const win::OutputInfo* glasses = glasses_gdi_.empty() ? nullptr : find_output(glasses_gdi_);
    if (!arrangement_.arrange(panels, opt_.layout, opt_.windowed ? nullptr : glasses, outputs_, opt_.vd_width,
                              opt_.vd_height, &error)) {
        logln("virtual monitors: %s (is %dx%d in the driver's vdd_settings.xml?)", error.c_str(), opt_.vd_width,
              opt_.vd_height);
        return false;
    }
    Sleep(1000);  // let the arrangement settle
    outputs_ = win::enumerate_outputs();
    std::string line;
    for (const auto& name : panel_names_) {
        if (!win::set_display_scale(name, opt_.vd_scale, &error)) {
            logln("virtual monitors: scale %d%% on %s: %s", opt_.vd_scale, win::narrow(name).c_str(), error.c_str());
        }
        if (const auto* o = find_output(name)) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "  %s@%ld,%ld %dx%d", win::narrow(name).c_str(), o->desktop.left,
                          o->desktop.top, o->width(), o->height());
            line += buf;
        }
    }
    logln("virtual monitors:%s, scale %d%% (removed on exit)", line.c_str(), opt_.vd_scale);
    return true;
}

// The cursor follows the panel you look at, keeping its relative position, once the mouse
// has rested for a moment (the mouse wins). On the glasses surface it would be a second,
// head-locked cursor: send it to the panel in view.
void Server::follow_gaze(uint64_t now) {
    if (!virtual_ || opt_.windowed || panel_rects_.empty()) return;
    POINT cur{};
    if (!GetCursorPos(&cur)) return;
    if (cur.x != last_cursor_.x || cur.y != last_cursor_.y) {
        last_cursor_ = cur;
        last_cursor_move_ns_ = now;
    }
    const auto p = pose_->latest();
    const int gaze = p && p->tracking ? renderer_.panel_at_gaze(p->orientation) : -1;
    auto warp = [&](const RECT& to, float rx, float ry) {
        const POINT target{to.left + static_cast<LONG>(rx * static_cast<float>(to.right - to.left)),
                           to.top + static_cast<LONG>(ry * static_cast<float>(to.bottom - to.top))};
        SetCursorPos(target.x, target.y);
        last_cursor_ = target;
    };
    if (PtInRect(&glasses_rect_, cur)) {
        const int i = gaze >= 0 && gaze < static_cast<int>(panel_rects_.size()) ? gaze : 0;
        warp(panel_rects_[static_cast<size_t>(i)], 0.5f, 0.5f);
        return;
    }
    if (!opt_.gaze_cursor || panel_rects_.size() < 2 || !p || !p->tracking) return;
    if (gaze == last_gaze_panel_) return;
    if (now - last_cursor_move_ns_ < 1500000000ull) return;  // re-evaluated once the mouse rests
    last_gaze_panel_ = gaze;
    if (gaze < 0 || gaze >= static_cast<int>(panel_rects_.size())) return;
    const RECT& to = panel_rects_[static_cast<size_t>(gaze)];
    if (PtInRect(&to, cur)) return;
    float rx = 0.5f, ry = 0.5f;
    for (const RECT& from : panel_rects_) {
        if (PtInRect(&from, cur)) {
            rx = static_cast<float>(cur.x - from.left) / static_cast<float>(std::max(1L, from.right - from.left));
            ry = static_cast<float>(cur.y - from.top) / static_cast<float>(std::max(1L, from.bottom - from.top));
        }
    }
    warp(to, rx, ry);
}

void Server::adjust_fov(float delta) {
    auto& ro = renderer_.options;
    ro.fov_y_deg = std::fmax(10.0f, std::fmin(45.0f, ro.fov_y_deg + delta));
    save_fov(ro.fov_y_deg);
    redraw_ = true;
    logln("fov: %.1f deg (saved)", ro.fov_y_deg);
}

void Server::on_hotkey(int id) {
    constexpr float step = 10;
    switch (id) {
    case kRecenter:
        pose_->recenter();
        last_gaze_panel_ = -2;  // re-evaluate: the cursor follows to the panel in front
        logln("recenter");
        break;
    case kQuit:
        logln("stopped (Ctrl+Alt+Q)");
        g_quit = true;
        break;
    // Field-of-view tuning with the glasses on: if the panel drifts WITH the head when
    // turning, the render FOV is too large (Ctrl+Alt+-); against it, too small (Ctrl+Alt+=).
    case kFovDown: adjust_fov(-0.5f); break;
    case kFovUp: adjust_fov(0.5f); break;
    case kLeft: pose_->nudge(step, 0); break;
    case kRight: pose_->nudge(-step, 0); break;
    case kUp: pose_->nudge(0, step); break;
    case kDown: pose_->nudge(0, -step); break;
    default: break;
    }
}

void Server::run() {
    bool presented = true;
    while (!g_quit.load()) {
        if (presented) {
            // Wake when the swap chain can take the next frame, or on window messages.
            MsgWaitForMultipleObjectsEx(1, &waitable_, 100, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        } else if (vblank_output_) {
            // Last frame skipped: the waitable will not signal until we present again.
            vblank_output_->WaitForVBlank();
        } else {
            Sleep(4);
        }
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (g_quit.load()) break;
        presented = frame();
        periodic();
    }
}

void Server::shutdown() {
    if (shut_) return;
    shut_ = true;
    for (auto& c : captures_) c->stop();
    captures_.clear();
    pose_.reset();  // stream-off
    if (g_hwnd != nullptr) {
        DestroyWindow(g_hwnd);
        g_hwnd = nullptr;
    }
    if (mode_changed_) {
        win::restore_mode(glasses_gdi_);
        logln("glasses mode: restored");
    }
    if (virtual_) {
        std::string error;
        if (arrangement_.restore(win::enumerate_outputs(), &error)) {
            logln("displays: restored, virtual monitors off");
        } else {
            logln("displays: %s (run nimbo_win --restore)", error.c_str());
        }
        win::VirtualDisplayDriver::set_count(1, nullptr);  // the driver keeps one, detached
    }
}

// Undoes the display changes of a run that did not exit cleanly (crash, power loss).
bool restore_from_backup() {
    win::DisplayArrangement a;
    if (!a.load_backup()) return false;
    std::string error;
    if (!a.restore(win::enumerate_outputs(), &error)) {
        logln("displays: %s", error.c_str());
        return false;
    }
    win::VirtualDisplayDriver::set_count(1, nullptr);
    logln("displays: restored from the backup of a previous run");
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--help" || std::string(argv[i]) == "-h") {
            std::printf("%s", kUsage);
            return 0;
        }
    }
    const auto options = parse(argc, argv);
    if (!options) {
        std::printf("%s", kUsage);
        return 1;
    }

    // One server at a time: two would fight over the display mode and the hotkeys.
    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\NimboServer");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        std::printf("Nimbo is already running: quit it first (Ctrl+Alt+Q)\n");
        return 1;
    }

    // Physical pixels everywhere (output rectangles, window size, capture).
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    timeBeginPeriod(1);
    SetConsoleCtrlHandler(console_handler, TRUE);

    if (options->restore_only) {
        if (!restore_from_backup()) std::printf("nothing to restore\n");
        timeEndPeriod(1);
        return 0;
    }
    if (win::DisplayArrangement::backup_exists()) {
        logln("previous run did not exit cleanly");
        restore_from_backup();
    }

    int code = 0;
    {
        Server server(*options);
        if (server.start()) {
            server.run();
        } else {
            code = 1;
        }
    }
    timeEndPeriod(1);
    if (single != nullptr) CloseHandle(single);
    return code;
}
