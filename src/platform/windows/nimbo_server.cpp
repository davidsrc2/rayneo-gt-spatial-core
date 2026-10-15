// nimbo.dll: the Windows spatial server behind the C ABI of rayneo/platform/nimbo_win.h.
// Captures the virtual monitors (or an existing display) with DXGI Desktop Duplication and
// renders them as panels fixed in space on the RayNeo GT with Direct3D 11 (W2/W3 of
// docs/WINDOWS_PLAN.md; the counterpart of SpatialServer.swift). The server runs on a
// thread of its own, which owns its window, hotkeys and display changes.

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
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <thread>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "desktop_duplication.h"
#include "displays.h"
#include "rayneo/core/head_calibration.h"
#include "rayneo/core/presets.h"
#include "rayneo/core/settings.h"
#include "rayneo/platform/guided_calibration.h"
#include "rayneo/platform/nimbo_win.h"
#include "rayneo/platform/rayneo_engine.h"
#include "spatial_renderer.h"
#include "virtual_displays.h"
#include "window_capture.h"
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
    bool hotkeys = true;
    int tracking_axes = 1;  // 0 both, 1 left/right, 2 up/down
    bool glasses_pinned = false;
    std::vector<core::PanelPose> poses;  // hand-placed monitors; empty = the grid
    int glasses_mode = -1;               // set the glasses' mode: 0 follow, 1 pinned, 2 steady; -1 leave
    int brightness = 0;                  // set the glasses' brightness 1..8; 0 leave
    bool recenter_glasses = false;       // Ctrl+Alt+C also recentres the glasses' pinned mode
    struct Floating {
        HWND hwnd = nullptr;
        core::PanelPose pose;
        bool placed = false;  // false: centred above the monitors
    };
    std::vector<Floating> floating;      // single windows as panels, after the monitors

    int displays = 2;                      // virtual monitors; 0 = show an existing display
    std::vector<core::GridCell> layout;    // grid cell per monitor; empty = one row
    int vd_width = 1920, vd_height = 1080;  // a mode the driver offers (vdd_settings.xml)
    int vd_scale = 150;                    // Windows scaling of the virtual monitors, percent
    bool fit = false;                      // size panels so all of them are visible at once
    bool gather_windows = true;            // move windows off the glasses display into panel 1
    bool gaze_cursor = true;               // cursor jumps to the panel you look at
    float dim_others = 1;                  // brightness of the panels you are not looking at
    float tilt = 1;                        // panels above / below: 1 face you .. 0 upright
    bool real_size = false;                // not pinned: every panel as large as the glasses' image, both axes
    bool laptop_screen = false;            // the computer's own (primary) screen is panel 1

    float panel_arc_deg() const { return arc_deg.value_or(displays > 0 ? 38.0f : 50.0f); }
};

Options from_c(const nimbo_options& c) {
    Options o;
    o.displays = std::clamp(c.displays, 0, NIMBO_MAX_PANELS);
    for (int i = 0; i < std::clamp(c.layout_count, 0, NIMBO_MAX_PANELS); ++i) {
        o.layout.push_back({c.layout_cols[i], c.layout_rows[i]});
    }
    if (!o.layout.empty()) o.displays = static_cast<int>(o.layout.size());
    o.vd_width = c.vd_width;
    o.vd_height = c.vd_height;
    o.vd_scale = c.vd_scale;
    if (c.arc_deg > 0) o.arc_deg = c.arc_deg;
    o.gap_deg = c.gap_deg;
    o.distance_m = c.distance_m;
    o.curvature = std::clamp(c.curvature, 0.0f, 1.0f);
    o.fit = c.fit;
    o.gather_windows = c.gather_windows;
    o.gaze_cursor = c.gaze_cursor;
    o.dim_others = std::clamp(c.dim_others, 0.0f, 1.0f);
    if (c.side_by_side >= 0) o.side_by_side = c.side_by_side != 0;
    o.ipd_mm = c.ipd_mm;
    if (c.fov_y_deg > 0) {
        o.fov_y_deg = c.fov_y_deg;
        o.fov_from_cli = true;
    }
    o.k1 = c.k1;
    o.k2 = c.k2;
    o.calibration_path = std::string(c.calibration_path, strnlen(c.calibration_path, sizeof(c.calibration_path)));
    o.calibration_required = c.calibration_required;
    o.dead_zone_deg = c.dead_zone_deg;
    o.sim = c.sim || c.sim_sweep_deg > 0;
    o.sim_sweep_deg = c.sim_sweep_deg;
    o.windowed = c.windowed;
    o.source = std::string(c.source, strnlen(c.source, sizeof(c.source)));
    if (o.source.empty()) o.source = "auto";
    o.refresh120 = c.refresh120;
    o.hotkeys = c.hotkeys;
    o.tracking_axes = std::clamp(c.tracking_axes, 0, 2);
    o.glasses_pinned = c.glasses_pinned;
    for (int i = 0; i < std::clamp(c.pose_count, 0, NIMBO_MAX_PANELS); ++i) {
        o.poses.push_back({c.pose_yaw[i], c.pose_pitch[i], c.pose_scale[i]});
    }
    o.glasses_mode = std::clamp(c.glasses_mode, -1, 2);
    o.brightness = std::clamp(c.brightness, 0, 8);
    o.recenter_glasses = c.recenter_glasses;
    for (int i = 0; i < std::clamp(c.floating_count, 0, NIMBO_MAX_PANELS); ++i) {
        Options::Floating f;
        f.hwnd = reinterpret_cast<HWND>(static_cast<uintptr_t>(c.floating_window[i]));
        f.placed = c.floating_scale[i] > 0.0f;
        f.pose = {c.floating_yaw[i], c.floating_pitch[i], f.placed ? c.floating_scale[i] : 1.0f};
        o.floating.push_back(f);
    }
    o.tilt = std::clamp(c.tilt, 0.0f, 1.0f);
    o.real_size = c.real_size;
    o.laptop_screen = c.laptop_screen;
    return o;
}

// Log sink (nimbo_set_log) shared by every server and the calibration.
std::mutex g_log_mutex;
nimbo_log_fn g_log_fn = nullptr;
void* g_log_ctx = nullptr;
thread_local std::string* t_last_line = nullptr;  // the running server's last line (start failures)

void logln(const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (t_last_line != nullptr) *t_last_line = buf;
    std::lock_guard<std::mutex> lock(g_log_mutex);
    if (g_log_fn != nullptr) {
        g_log_fn(g_log_ctx, buf);
    } else {
        std::printf("%s\n", buf);
        std::fflush(stdout);
    }
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
    virtual void set_glasses_mode(int) {}  // the glasses' own display mode (IMU only)
    virtual void set_glasses_brightness(int) {}
};

class ImuPose final : public PoseSource {
public:
    explicit ImuPose(float dead_zone_deg) {
        rayneo_config cfg = rayneo_default_config();
        if (dead_zone_deg != 0) cfg.dead_zone_deg = dead_zone_deg;  // < 0: world-locked (see rayneo_config)
        engine_ = rayneo_engine_create(&cfg);
    }
    ~ImuPose() override {
        rayneo_engine_stop(engine_);
        rayneo_engine_destroy(engine_);
    }
    bool load_calibration(const std::string& path) {
        std::ifstream f(std::filesystem::u8path(path));
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
    void set_glasses_mode(int mode) override { rayneo_engine_set_glasses_mode(engine_, mode); }
    void set_glasses_brightness(int level) override { rayneo_engine_set_glasses_brightness(engine_, level); }
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

constexpr UINT_PTR kRepinTimer = 1;  // recentre in the glasses' pinned mode: back to pinned

enum HotKey { kRecenter = 1, kQuit, kFovDown, kFovUp, kLeft, kRight, kUp, kDown, kJump, kFocus, kCinema };


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

    // Server thread: start, then run until stopped, then shutdown.
    bool start();
    void run();
    void shutdown();
    void on_hotkey(int id);
    void on_display_change() { display_changed_ = true; }
    void adjust_fov(float delta);
    void repin() {
        pose_->set_glasses_mode(1);
        // Pinned: nothing of ours moves (the image is fitted), but the head's zero must match
        // the new anchor so the gaze finds the panel you look at.
        if (opt_.glasses_pinned) {
            pose_->recenter();
            last_gaze_panel_ = -2;
        }
    }

    // Any thread.
    void request_stop(const std::string& reason);
    void request_apply(const Options& o);
    void post(UINT msg, WPARAM wp = 0, LPARAM lp = 0);
    nimbo_status status();
    std::string last_line;  // server thread: last log line (why start failed)

private:
    bool create_window(const RECT& rect);
    bool create_swap_chain();
    bool create_back_buffer_view();
    uint64_t estimate_target(uint64_t now);
    uint64_t estimate_vsync(uint64_t now);
    // How much later than its target a frame reaches the glasses (ms, smoothed): the pose is
    // predicted for that real moment, or the panels lag and jerk while turning (as on macOS).
    double present_late_ms_ = 0.0;
    bool frame();
    void collect_presented();
    void periodic();
    void handle_display_change();
    bool setup_virtual_displays();
    const win::OutputInfo* find_output(const std::wstring& gdi_name) const;
    void refresh_rects();
    void follow_gaze(uint64_t now);
    void apply_pinned(const Options& o);
    void jump_cursor_to_gaze();
    void toggle_focus(bool cinema);
    int focus_mode_ = 0;  // 0 all, 1 focus, 2 cinema
    bool overlay_shown_ = false;  // start-up message on the glasses until tracking starts
    void start_floating(std::vector<Options::Floating> specs);
    core::PanelPose default_floating_pose(int index) const;
    void apply_pending();
    void set_state(nimbo_state st);

    Options opt_;
    std::atomic<bool> quit_{false};
    HWND hwnd_ = nullptr;
    std::mutex shared_mutex_;           // guards the fields below, shared with other threads
    std::optional<Options> pending_;    // nimbo_apply, picked up by the server thread
    nimbo_status status_{};
    std::string stop_reason_;
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
    std::vector<std::unique_ptr<win::WindowCapture>> window_captures_;  // floating windows
    std::vector<HWND> floating_hwnds_;
    int monitor_panels_ = 1;
    ComPtr<IDXGIAdapter> render_adapter_;
    win::SpatialRenderer renderer_;

    // Virtual monitors (W3).
    bool virtual_ = false;
    std::unique_ptr<win::VirtualDisplayBackend> backend_;
    win::DisplayArrangement arrangement_;
    std::vector<std::wstring> panel_names_;  // GDI name per panel, left -> right
    std::vector<RECT> panel_rects_;          // desktop rectangle per panel
    RECT glasses_rect_{};
    POINT last_cursor_{};
    uint64_t last_cursor_move_ns_ = 0, next_gaze_ns_ = 0;
    uint64_t last_other_input_ns_ = 0;     // clicks and keys (input that did not move the cursor)
    DWORD last_input_tick_ = 0;
    int last_gaze_panel_ = -2;
    int gaze_candidate_ = -2;              // panel looked at, not yet followed
    uint64_t gaze_candidate_ns_ = 0;
    bool redraw_ = true;
    bool display_changed_ = false;
    std::wstring laptop_gdi_;           // the computer's own screen as panel 1 (empty: all virtual)
    uint64_t arranged_ns_ = 0;          // our own arrangement: its display-change messages are ours
    void rearrange_displays();
    std::vector<const win::OutputInfo*> virtual_panel_outputs() const;

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
        if (server != nullptr) server->request_stop("window closed");
        return 0;
    case WM_APP + 1:  // nimbo_recenter
        if (server != nullptr) server->on_hotkey(kRecenter);
        return 0;
    case WM_TIMER:
        if (wp == kRepinTimer && server != nullptr) server->repin();
        KillTimer(hwnd, wp);
        return 0;
    case WM_APP + 3:  // nimbo_toggle_focus: wp = cinema
        if (server != nullptr) server->on_hotkey(wp != 0 ? kCinema : kFocus);
        return 0;
    case WM_APP + 2:  // nimbo_adjust_fov: lp = delta in hundredths of a degree
        if (server != nullptr) server->adjust_fov(static_cast<float>(static_cast<int>(lp)) / 100.0f);
        return 0;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

// Windows 11 throttles processes that are not in front (EcoQoS: slower cores, coarse timers,
// ignored timeBeginPeriod). Once another app is clicked the IMU and render threads would run
// late and the panels tremble (the macOS App Nap problem): opt out while the server runs.
void disable_power_throttling() {
#ifndef PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION
#define PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION 0x4
#endif
    PROCESS_POWER_THROTTLING_STATE st{};
    st.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    st.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
    st.StateMask = 0;  // controlled and off: full speed, timer resolution honoured
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &st, sizeof(st));
}

bool Server::start() {
    set_state(NIMBO_STARTING);
    disable_power_throttling();
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

    // The computer's own screen as panel 1: the primary display that is neither the glasses nor
    // a virtual monitor. With a single panel, that screen alone, no virtual monitors.
    if (opt_.laptop_screen && !opt_.windowed && opt_.displays > 0) {
        for (const auto& o : outputs_) {
            if (o.primary && !o.glasses && !win::is_virtual_panel(o)) laptop_gdi_ = o.gdi_name;
        }
        if (laptop_gdi_.empty()) {
            logln("laptop screen: no primary display of its own (lid closed?), all panels virtual");
        } else if (opt_.displays == 1) {
            opt_.displays = 0;
            opt_.source = "primary";
            laptop_gdi_.clear();
            logln("laptop screen: the only panel");
        } else {
            logln("laptop screen: %s is panel 1", win::narrow(laptop_gdi_).c_str());
        }
    }

    // Virtual monitors first: arranging displays resets temporary modes such as the glasses'
    // native mode below.
    if (opt_.displays > 0) backend_ = win::find_virtual_display_backend();
    if (opt_.displays > 0 && !backend_) {
        if (win::virtual_display_driver_installed()) {
            logln("virtual monitors: the Virtual Display Driver is installed but does not answer on its control pipe"
                  " (update to 25.7.23 or newer, or restart Windows); showing an existing display instead");
        } else {
            logln("virtual monitors: the Virtual Display Driver is not installed, showing an existing display instead"
                  " (install it to get --displays: see docs/WINDOWS_PLAN.md, W3)");
        }
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
        overlay_shown_ = true;  // shown on the glasses once the renderer exists
        pose_ = std::move(imu);
        imu_ = true;
        if (opt_.glasses_mode >= 0) {
            pose_->set_glasses_mode(opt_.glasses_mode);  // pinned: centred in front of you
            static const char* kModes[] = {"follow", "pinned", "steady"};
            logln("glasses: %s mode", kModes[opt_.glasses_mode]);
        }
        if (opt_.brightness > 0) {
            pose_->set_glasses_brightness(opt_.brightness);
            logln("glasses: brightness %d", opt_.brightness);
        }
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
    ro.tracking_axes = static_cast<core::TrackingAxes>(opt_.tracking_axes);
    renderer_.set_panel_count(panels);
    if (static_cast<int>(opt_.layout.size()) == panels) renderer_.set_grid(opt_.layout);
    ro.layout_arc_deg = opt_.panel_arc_deg();
    renderer_.set_poses(opt_.poses);
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
    // The glasses' pinned mode anchors the whole image and lets the eyes roam over it: moving
    // the panels with the head as well would push them out of it. They stay put, fitted inside.
    apply_pinned(opt_);
    static const char* kTracking[] = {"full head rotation", "left/right turn only (panels never move vertically)",
                                      "up/down only (panels never move sideways)"};
    logln("tracking: %s", opt_.glasses_pinned ? "by the glasses (pinned mode); panels fitted inside their image"
                                              : kTracking[opt_.tracking_axes]);
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
    if (overlay_shown_) renderer_.set_overlay_text(L"Nimbo se está preparando\nDeja las gafas quietas unos segundos");
    monitor_panels_ = panels;
    render_adapter_ = first->adapter;
    start_floating(opt_.floating);

    const UINT mods = MOD_CONTROL | MOD_ALT | MOD_NOREPEAT;
    struct Key {
        int id;
        UINT vk;
        const char* name;
    };
    std::vector<Key> keys = {{kRecenter, 'C', "Ctrl+Alt+C"}, {kQuit, 'Q', "Ctrl+Alt+Q"},
                             {kFovDown, VK_OEM_MINUS, "Ctrl+Alt+-"}, {kFovUp, VK_OEM_PLUS, "Ctrl+Alt+="},
                             {kJump, 'J', "Ctrl+Alt+J"}, {kFocus, 'O', "Ctrl+Alt+O"}, {kCinema, 'P', "Ctrl+Alt+P"}};
    if (opt_.sim) {
        keys.push_back({kLeft, VK_LEFT, "Ctrl+Alt+Left"});
        keys.push_back({kRight, VK_RIGHT, "Ctrl+Alt+Right"});
        keys.push_back({kUp, VK_UP, "Ctrl+Alt+Up"});
        keys.push_back({kDown, VK_DOWN, "Ctrl+Alt+Down"});
    }
    if (opt_.hotkeys) {
        for (const auto& k : keys) {
            if (!RegisterHotKey(hwnd_, k.id, mods, k.vk)) logln("hotkey %s: in use by another app", k.name);
        }
        logln("hotkeys: Ctrl+Alt+C recenter, Ctrl+Alt+Q quit, Ctrl+Alt+- / = field of view");
    }
    {
        std::lock_guard<std::mutex> lock(shared_mutex_);
        status_.panel_count = panels;
        status_.fov_y_deg = renderer_.options.fov_y_deg;
    }

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
    hwnd_ = CreateWindowExW(ex, wc.lpszClassName, L"Nimbo (vista previa)", style, r.left, r.top, r.right - r.left,
                             r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
    if (hwnd_ == nullptr) {
        logln("CreateWindowEx failed (%lu)", GetLastError());
        return false;
    }
    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    // Invisible to capture: no feedback loop when the captured display is the glasses one,
    // and screenshots / screen sharing see the desktop instead of the SBS image.
    if (!SetWindowDisplayAffinity(hwnd_, WDA_EXCLUDEFROMCAPTURE)) {
        logln("capture exclusion unavailable (Windows 10 2004 or newer): keep --source off the glasses display");
    }
    ShowWindow(hwnd_, opt_.windowed ? SW_SHOW : SW_SHOWNOACTIVATE);
    RECT client{};
    GetClientRect(hwnd_, &client);
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
    const HRESULT hr = factory->CreateSwapChainForHwnd(device_.Get(), hwnd_, &d, nullptr, nullptr, &sc1);
    if (FAILED(hr) || FAILED(sc1.As(&swap_chain_))) {
        logln("CreateSwapChainForHwnd failed (0x%08lx)", static_cast<unsigned long>(hr));
        return false;
    }
    factory->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);
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
    const auto late = static_cast<uint64_t>(std::clamp(present_late_ms_, 0.0, 25.0) * 1e6);
    return estimate_vsync(now) + late;
}

uint64_t Server::estimate_vsync(uint64_t now) {
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
    const double late = (static_cast<double>(shown) - static_cast<double>(s.target_ns)) / 1e6;
    stats_.target_err_sum += late;
    present_late_ms_ += (std::clamp(late, -10.0, 40.0) - present_late_ms_) * 0.03;
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
        request_stop("GPU device lost");
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
    apply_pending();
    if (now >= next_gaze_ns_) {
        next_gaze_ns_ = now + 33000000ull;  // ~30 Hz
        follow_gaze(now);
        const auto p = pose_->latest();
        const int gaze = p && p->tracking ? renderer_.panel_at_gaze(p->orientation) : -1;
        std::lock_guard<std::mutex> lock(shared_mutex_);
        status_.gaze_panel = gaze;
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
            request_stop("glasses disconnected");
        }
    }

    const LatencyStats s = stats_;
    stats_ = LatencyStats{};
    stats_.tracking = s.tracking;
    uint64_t cap = 0;
    for (const auto& c : captures_) cap += c->frames();
    for (const auto& c : window_captures_) cap += c->frames();
    const uint64_t cap_fps = cap - last_capture_frames_;
    last_capture_frames_ = cap;
    if (overlay_shown_ && s.tracking) {
        renderer_.set_overlay_text(L"");
        overlay_shown_ = false;
        logln("ready: tracking");
        redraw_ = true;
    } else if (overlay_shown_) {
        renderer_.set_overlay_text(L"Nimbo se está preparando\nDeja las gafas quietas · " +
                                   std::to_wstring(std::max(1, 6 - calib_s_)) + L" s");
    }
    if (!s.tracking && imu_ && ++calib_s_ % 3 == 1) {
        logln("CALIB: head tracking starts after ~6 s with the glasses still on the table"
            " (or ~14 s if you are already wearing them)");
    }
    const double n = std::max(1, s.frames);
    const double pn = std::max(1, s.presented_n);
    set_state(s.tracking ? NIMBO_TRACKING : NIMBO_CALIBRATING);
    {
        std::lock_guard<std::mutex> lock(shared_mutex_);
        status_.render_fps = static_cast<float>(s.frames);
        status_.capture_fps = static_cast<float>(cap_fps);
        status_.latency_ms = static_cast<float>(s.presented_sum / pn);
    }
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
        request_stop("glasses disconnected");
        return;
    }
    if (const auto m = win::current_mode(glasses_gdi_)) refresh_hz_ = m->hz;
    // Panels or the glasses' surface moved, and not by us: lay them out our way again.
    if (virtual_ && steady_ns() - arranged_ns_ > 2000000000ull && !panel_rects_.empty()) {
        bool moved = glasses->desktop.left != glasses_rect_.left || glasses->desktop.top != glasses_rect_.top;
        for (size_t i = 0; i < panel_names_.size() && i < panel_rects_.size(); ++i) {
            const auto* o = find_output(panel_names_[i]);
            if (o != nullptr && (o->desktop.left != panel_rects_[i].left || o->desktop.top != panel_rects_[i].top)) moved = true;
        }
        if (moved) {
            rearrange_displays();
            glasses = find_output(glasses_gdi_);
            if (glasses == nullptr) return;
        }
    }
    const RECT r = glasses->desktop;
    SetWindowPos(hwnd_, HWND_TOPMOST, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOACTIVATE);
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

void Server::apply_pinned(const Options& o) {
    auto& ro = renderer_.options;
    ro.static_head = o.glasses_pinned;  // the renderer fits everything in the glasses' image
    // Which panel you look at: the image is anchored on both axes, so a window above is
    // reached by looking up.
    if (o.glasses_pinned) ro.tracking_axes = core::TrackingAxes::Both;
    ro.tilt = o.tilt;
    // Space at real size (glasses in follow): every panel nearly as large as the glasses' own
    // image, fixed in the room on both axes. It fits both ways (a 16:10 laptop screen is taller
    // than the image): 94 % of what fits. The arc is set so the distance scaling cancels out.
    if (!o.glasses_pinned && o.real_size) {
        constexpr float kDegToRad = 0.017453292519943295f;
        ro.tracking_axes = core::TrackingAxes::Both;
        const float fov_y = ro.fov_y_deg * kDegToRad;
        float tallest = 16.0f / 9.0f;
        for (const auto& name : panel_names_) {
            if (const auto* out = find_output(name); out != nullptr && out->height() > 0) {
                tallest = std::min(tallest, static_cast<float>(out->width()) / static_cast<float>(out->height()));
            }
        }
        const float wide = 2.0f * std::atan(std::tan(fov_y / 2) * (16.0f / 9.0f));
        const float tall = 2.0f * std::atan(std::tan(fov_y / 2) * tallest);
        const float fw = 0.94f * std::min(wide, tall);
        ro.arc_deg = 2.0f * std::atan(std::tan(fw / 2) * ro.distance_m / 2.0f) / kDegToRad;
    }
}

// Ctrl+Alt+O focus mode: only the panel you look at stays; Ctrl+Alt+P cinema: that one centred
// straight ahead and large. The same key brings everything back; the other switches mode.
void Server::toggle_focus(bool cinema) {
    auto& ro = renderer_.options;
    const int wanted = cinema ? 2 : 1;
    if (focus_mode_ == wanted) {
        focus_mode_ = 0;
        ro.focus_panel = -1;
        ro.cinema = false;
        logln("focus: all panels");
    } else {
        if (focus_mode_ == 0) {
            const auto p = pose_->latest();
            const int gaze = p ? renderer_.panel_at_gaze(p->orientation) : -1;
            ro.focus_panel = gaze >= 0 ? gaze : 0;
        }
        focus_mode_ = wanted;
        ro.cinema = cinema;
        logln("focus: %s on panel %d", cinema ? "cinema" : "focus", ro.focus_panel + 1);
    }
    redraw_ = true;
}

// Ctrl+Alt+J: the cursor jumps to the centre of the monitor you are looking at.
void Server::jump_cursor_to_gaze() {
    const auto p = pose_->latest();
    if (!p || !p->tracking) return;
    const int i = renderer_.panel_at_gaze(p->orientation);
    // A floating window: its real window to the front with the cursor on it (pause the video,
    // type in the chat).
    if (i >= monitor_panels_ && i - monitor_panels_ < static_cast<int>(floating_hwnds_.size())) {
        HWND w = floating_hwnds_[static_cast<size_t>(i - monitor_panels_)];
        if (IsIconic(w)) ShowWindow(w, SW_RESTORE);
        SetForegroundWindow(w);  // allowed: this process just received the hotkey
        RECT r{};
        if (GetWindowRect(w, &r)) SetCursorPos((r.left + r.right) / 2, (r.top + r.bottom) / 2);
        logln("window: focus panel %d", i + 1);
        return;
    }
    if (i < 0 || i >= static_cast<int>(panel_rects_.size())) {
        logln("cursor: jump, no monitor in view");
        return;
    }
    const RECT& r = panel_rects_[static_cast<size_t>(i)];
    SetCursorPos((r.left + r.right) / 2, (r.top + r.bottom) / 2);
    last_cursor_ = {(r.left + r.right) / 2, (r.top + r.bottom) / 2};
    last_gaze_panel_ = i;
    logln("cursor: jump to panel %d", i + 1);
}

// Where a new floating window goes: centred above the monitors, one monitor wide.
core::PanelPose Server::default_floating_pose(int index) const {
    const float size = opt_.panel_arc_deg(), gap = opt_.gap_deg;
    const float v = 2.0f * std::atan(std::tan(size * 0.00872664626f) / (16.0f / 9.0f)) / 0.0174532925f;
    float top = v / 2;
    if (!opt_.poses.empty()) {
        top = -1e9f;
        for (const auto& p : opt_.poses) top = std::max(top, p.pitch_deg + v * p.scale / 2);
    } else if (!opt_.layout.empty()) {
        int r0 = opt_.layout.front().row, r1 = r0;
        for (const auto& c : opt_.layout) {
            r0 = std::min(r0, c.row);
            r1 = std::max(r1, c.row);
        }
        top = static_cast<float>(r1 - r0) * (v + gap) / 2 + v / 2;
    }
    return {0.0f, top + gap + v / 2 + static_cast<float>(index) * (v + gap), 1.0f};
}

// (Re)starts the floating windows' captures and makes them panels after the monitors.
void Server::start_floating(std::vector<Options::Floating> specs) {
    for (auto& c : window_captures_) c->stop();
    window_captures_.clear();
    floating_hwnds_.clear();
    std::vector<core::PanelPose> poses;
    std::string error;
    for (auto& f : specs) {
        if (monitor_panels_ + static_cast<int>(poses.size()) >= win::SpatialRenderer::kMaxPanels) break;
        auto cap = std::make_unique<win::WindowCapture>();
        if (!cap->start(render_adapter_.Get(), f.hwnd, device_.Get(), &error)) {
            logln("window: %s", error.c_str());
            continue;
        }
        if (!f.placed) f.pose = default_floating_pose(static_cast<int>(poses.size()));
        const int panel = monitor_panels_ + static_cast<int>(poses.size());
        renderer_.set_source(panel, cap.get());
        wchar_t title[128];
        const int n = GetWindowTextW(f.hwnd, title, 128);
        logln("window: panel %d <- %s", panel + 1, win::narrow(std::wstring(title, static_cast<size_t>(std::max(0, n)))).c_str());
        poses.push_back(f.pose);
        floating_hwnds_.push_back(f.hwnd);
        window_captures_.push_back(std::move(cap));
    }
    renderer_.set_floating(poses);
    renderer_.set_panel_count(monitor_panels_ + static_cast<int>(poses.size()));
    redraw_ = true;
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
    const int n = opt_.displays - (laptop_gdi_.empty() ? 0 : 1);  // the laptop screen is one of them
    std::vector<std::wstring> before;  // displays that are not Nimbo's
    for (const auto& o : outputs_) before.push_back(o.gdi_name);
    logln("virtual monitors: %s", backend_->name());
    // Next to the laptop's screen, the virtual monitors take its shape (16:10 on many laptops;
    // at 16:9 the same width drew them shorter, as if cut). Only virtual-display-rs takes any
    // resolution; the Virtual Display Driver offers the modes of its vdd_settings.xml.
    if (const auto* laptop = laptop_gdi_.empty() ? nullptr : find_output(laptop_gdi_);
        laptop != nullptr && laptop->width() > 0 && std::string(backend_->name()) == "virtual-display-rs") {
        opt_.vd_height = (opt_.vd_width * laptop->height() / laptop->width()) & ~1;
    }
    if (!backend_->set_monitors(n, opt_.vd_width, opt_.vd_height, &error)) {
        logln("virtual monitors: %s", error.c_str());
        return false;
    }
    virtual_ = true;  // from here on shutdown() restores the arrangement
    std::vector<const win::OutputInfo*> panels;
    for (int waited = 0; waited < 15000; waited += 250) {  // the driver adds its monitors
        Sleep(250);
        outputs_ = win::enumerate_outputs();
        panels.clear();
        for (const auto& o : outputs_) {
            const bool is_new = std::find(before.begin(), before.end(), o.gdi_name) == before.end();
            if (!o.glasses && (win::is_virtual_panel(o) || is_new)) panels.push_back(&o);
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
    if (!laptop_gdi_.empty()) panel_names_.push_back(laptop_gdi_);
    for (const auto* p : panels) panel_names_.push_back(p->gdi_name);

    const win::OutputInfo* glasses = glasses_gdi_.empty() ? nullptr : find_output(glasses_gdi_);
    const win::OutputInfo* laptop = laptop_gdi_.empty() ? nullptr : find_output(laptop_gdi_);
    arranged_ns_ = steady_ns();
    if (!arrangement_.arrange(panels, opt_.layout, opt_.windowed ? nullptr : glasses, outputs_, opt_.vd_width,
                              opt_.vd_height, &error, laptop)) {
        logln("virtual monitors: %s (is %dx%d in the driver's vdd_settings.xml?)", error.c_str(), opt_.vd_width,
              opt_.vd_height);
        return false;
    }
    Sleep(1000);  // let the arrangement settle
    outputs_ = win::enumerate_outputs();
    std::string line;
    for (const auto& name : panel_names_) {
        if (name == laptop_gdi_) continue;  // your own screen keeps its scaling
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
    // New monitors may come up with Windows' default wallpaper: give them yours.
    std::vector<RECT> fresh;
    for (const auto* p : virtual_panel_outputs()) fresh.push_back(p->desktop);
    const auto* own = laptop_gdi_.empty() ? nullptr : find_output(laptop_gdi_);
    for (const auto& o : outputs_) {
        if (own == nullptr && o.primary && !o.glasses && !win::is_virtual_panel(o)) own = &o;
    }
    if (own != nullptr && !fresh.empty()) {
        logln("wallpaper: copied to %d monitor(s)", win::copy_wallpaper(own->desktop, fresh));
    }
    return true;
}

// Nimbo's virtual monitors (the panels except the laptop screen), from the current outputs.
std::vector<const win::OutputInfo*> Server::virtual_panel_outputs() const {
    std::vector<const win::OutputInfo*> out;
    for (const auto& name : panel_names_) {
        if (name == laptop_gdi_) continue;
        if (const auto* o = find_output(name)) out.push_back(o);
    }
    return out;
}

// Windows lays the displays out its own way when one comes or goes (or a driver reloads):
// the glasses' surface beside a panel, the panels apart. Ours again; our own changes are
// ignored for 2 s.
void Server::rearrange_displays() {
    if (!virtual_ || opt_.windowed) return;
    std::string error;
    const auto* glasses = glasses_gdi_.empty() ? nullptr : find_output(glasses_gdi_);
    const auto* laptop = laptop_gdi_.empty() ? nullptr : find_output(laptop_gdi_);
    arranged_ns_ = steady_ns();
    if (!arrangement_.arrange(virtual_panel_outputs(), opt_.layout, glasses, outputs_, opt_.vd_width, opt_.vd_height,
                              &error, laptop)) {
        logln("displays: could not restore the arrangement: %s", error.c_str());
        return;
    }
    Sleep(500);
    outputs_ = win::enumerate_outputs();
    logln("displays: arrangement restored");
}

// The cursor follows the panel you look at, keeping its relative position, once the mouse
// has rested for a moment (the mouse wins). On the glasses surface it would be a second,
// head-locked cursor: send it to the panel in view.
void Server::follow_gaze(uint64_t now) {
    if (!virtual_ || opt_.windowed || panel_rects_.empty()) return;
    POINT cur{};
    if (!GetCursorPos(&cur)) return;
    const bool moved = cur.x != last_cursor_.x || cur.y != last_cursor_.y;
    if (moved) {
        last_cursor_ = cur;
        last_cursor_move_ns_ = now;
    }
    LASTINPUTINFO li{sizeof(li), 0};
    if (GetLastInputInfo(&li) && li.dwTime != last_input_tick_) {
        last_input_tick_ = li.dwTime;
        if (!moved) last_other_input_ns_ = now;  // a click or a key: the user is working here
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
    // Pinned: the glasses pan over the image, so the head direction does not say which panel is in view.
    if (!opt_.gaze_cursor || focus_mode_ != 0 || panel_rects_.size() < 2 || !p || !p->tracking) return;
    if (gaze == last_gaze_panel_) {
        gaze_candidate_ = gaze;
        return;
    }
    // A glance or the edge between two panels is not a decision: the new panel must hold the
    // gaze for a moment. The mouse and the keyboard win: right after a click, while typing or
    // moving the mouse, the cursor stays where the user is working.
    if (gaze != gaze_candidate_) {
        gaze_candidate_ = gaze;
        gaze_candidate_ns_ = now;
        return;
    }
    if (now - gaze_candidate_ns_ < 800000000ull) return;
    if (now - last_cursor_move_ns_ < 2000000000ull || now - last_other_input_ns_ < 4000000000ull) return;
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
    {
        std::lock_guard<std::mutex> lock(shared_mutex_);
        status_.fov_y_deg = ro.fov_y_deg;
    }
    logln("fov: %.1f deg (saved)", ro.fov_y_deg);
}

void Server::on_hotkey(int id) {
    constexpr float step = 10;
    switch (id) {
    case kRecenter:
        // Glasses in their own pinned mode hold an anchor too. Setting pinned while in pinned does
        // nothing; a mode change recentres it (both axes): follow, then pinned again a moment later.
        if (opt_.recenter_glasses || opt_.glasses_pinned) {
            pose_->set_glasses_mode(0);
            SetTimer(hwnd_, kRepinTimer, 300, nullptr);
            logln("recenter: glasses' pinned anchor (follow -> pinned)");
        }
        if (opt_.glasses_pinned) break;
        pose_->recenter();
        last_gaze_panel_ = -2;  // re-evaluate: the cursor follows to the panel in front
        logln("recenter");
        break;
    case kQuit:
        logln("stopped (Ctrl+Alt+Q)");
        request_stop("stopped (Ctrl+Alt+Q)");
        break;
    // Field-of-view tuning with the glasses on: if the panel drifts WITH the head when
    // turning, the render FOV is too large (Ctrl+Alt+-); against it, too small (Ctrl+Alt+=).
    case kFovDown: adjust_fov(-0.5f); break;
    case kFovUp: adjust_fov(0.5f); break;
    case kLeft: pose_->nudge(step, 0); break;
    case kRight: pose_->nudge(-step, 0); break;
    case kUp: pose_->nudge(0, step); break;
    case kDown: pose_->nudge(0, -step); break;
    case kJump: jump_cursor_to_gaze(); break;
    case kFocus: toggle_focus(false); break;
    case kCinema: toggle_focus(true); break;
    default: break;
    }
}

void Server::run() {
    bool presented = true;
    while (!quit_.load()) {
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
        if (quit_.load()) break;
        presented = frame();
        periodic();
    }
}

void Server::shutdown() {
    if (shut_) return;
    shut_ = true;
    for (int id = kRecenter; id <= kCinema; ++id) UnregisterHotKey(hwnd_, id);
    for (auto& c : window_captures_) c->stop();
    window_captures_.clear();
    for (auto& c : captures_) c->stop();
    captures_.clear();
    // The space at real size put the glasses in follow: back to pinned, how they are used
    // without Nimbo (the command goes out with the next IMU read).
    if (imu_ && pose_ && opt_.real_size && !opt_.glasses_pinned && opt_.glasses_mode == 0) {
        pose_->set_glasses_mode(1);
        Sleep(150);
    }
    pose_.reset();  // stream-off
    if (hwnd_ != nullptr) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
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
        if (backend_) backend_->remove_monitors();  // VDD keeps one, detached above
    }
    std::lock_guard<std::mutex> lock(shared_mutex_);
    status_.state = NIMBO_STOPPED;
    status_.gaze_panel = -1;
    status_.render_fps = status_.capture_fps = status_.latency_ms = 0;
    std::snprintf(status_.message, sizeof(status_.message), "%s", stop_reason_.c_str());
}

void Server::request_stop(const std::string& reason) {
    {
        std::lock_guard<std::mutex> lock(shared_mutex_);
        if (stop_reason_.empty()) stop_reason_ = reason;
    }
    quit_ = true;
    post(WM_NULL);
}

void Server::request_apply(const Options& o) {
    {
        std::lock_guard<std::mutex> lock(shared_mutex_);
        pending_ = o;
    }
    post(WM_NULL);
}

void Server::post(UINT msg, WPARAM wp, LPARAM lp) {
    if (hwnd_ != nullptr) PostMessageW(hwnd_, msg, wp, lp);
}

nimbo_status Server::status() {
    std::lock_guard<std::mutex> lock(shared_mutex_);
    return status_;
}

void Server::set_state(nimbo_state st) {
    std::lock_guard<std::mutex> lock(shared_mutex_);
    status_.state = st;
}

// What nimbo_apply can change without rebuilding monitors or captures.
void Server::apply_pending() {
    std::optional<Options> o;
    {
        std::lock_guard<std::mutex> lock(shared_mutex_);
        o.swap(pending_);
    }
    if (!o) return;
    auto& ro = renderer_.options;
    ro.distance_m = o->distance_m;
    ro.gap_deg = o->gap_deg;
    ro.curvature = o->curvature;
    if (o->arc_deg) ro.arc_deg = *o->arc_deg;
    ro.layout_arc_deg = o->panel_arc_deg();
    renderer_.set_poses(o->poses);
    {
        std::vector<HWND> hwnds;
        for (const auto& f : o->floating) hwnds.push_back(f.hwnd);
        if (hwnds != floating_hwnds_) {
            opt_.floating = o->floating;
            start_floating(o->floating);
        } else {
            std::vector<core::PanelPose> poses;
            for (size_t i = 0; i < o->floating.size(); ++i) {
                poses.push_back(o->floating[i].placed ? o->floating[i].pose : default_floating_pose(static_cast<int>(i)));
            }
            renderer_.set_floating(poses);
        }
    }
    if (o->fov_from_cli) ro.fov_y_deg = o->fov_y_deg;
    ro.ipd_m = o->ipd_mm / 1000.0f;
    ro.side_by_side = o->side_by_side.value_or(!opt_.windowed);
    ro.k1 = o->k1;
    ro.k2 = o->k2;
    ro.dim_others = o->dim_others;
    ro.tracking_axes = static_cast<core::TrackingAxes>(o->tracking_axes);
    if (o->glasses_mode >= 0 && o->glasses_mode != opt_.glasses_mode) pose_->set_glasses_mode(o->glasses_mode);
    if (o->brightness > 0 && o->brightness != opt_.brightness) pose_->set_glasses_brightness(o->brightness);
    opt_.glasses_mode = o->glasses_mode;
    opt_.brightness = o->brightness;
    opt_.recenter_glasses = o->recenter_glasses;
    opt_.glasses_pinned = o->glasses_pinned;
    opt_.tilt = o->tilt;
    opt_.real_size = o->real_size;
    apply_pinned(*o);  // after the width above: it overrides it
    opt_.gaze_cursor = o->gaze_cursor;
    opt_.gather_windows = o->gather_windows;
    redraw_ = true;
    std::lock_guard<std::mutex> lock(shared_mutex_);
    status_.fov_y_deg = ro.fov_y_deg;
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
    if (auto backend = win::find_virtual_display_backend()) backend->remove_monitors();
    logln("displays: restored from the backup of a previous run");
    return true;
}

// ---------------------------------------------------------------------------------------
// C ABI
// ---------------------------------------------------------------------------------------

std::atomic<bool> g_server_running{false};  // one server per process: displays and hotkeys are global

// DisplayConfig, DXGI and window rectangles in physical pixels on the calling thread.
struct DpiScope {
    DPI_AWARENESS_CONTEXT old = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ~DpiScope() {
        if (old != nullptr) SetThreadDpiAwarenessContext(old);
    }
};

void copy_str(char* dst, size_t size, const std::string& src) {
    if (size == 0) return;
    std::snprintf(dst, size, "%s", src.c_str());
}

core::Settings to_core(const nimbo_settings& c) {
    core::Settings s;
    s.cells.clear();
    for (int i = 0; i < std::clamp(c.cell_count, 0, NIMBO_MAX_PANELS); ++i) s.cells.push_back({c.cols[i], c.rows[i]});
    if (s.cells.empty()) s.cells = core::Settings{}.cells;
    s.text_size = c.text_size;
    s.distance_m = c.distance_m;
    s.gap_deg = c.gap_deg;
    s.curvature = c.curvature;
    s.size_deg = c.size_deg;
    s.auto_start = c.auto_start;
    s.fov_y_deg = c.fov_y_deg;
    s.ipd_mm = c.ipd_mm;
    s.stereo = c.stereo;
    s.dead_zone_deg = c.dead_zone_deg;
    s.gaze_cursor = c.gaze_cursor;
    s.dim_others = c.dim_others;
    s.gather_windows = c.gather_windows;
    s.k1 = c.k1;
    s.k2 = c.k2;
    s.onboarding_done = c.onboarding_done;
    s.tracking = std::clamp(c.tracking, 0, 2);
    s.glasses_mode = std::clamp(c.glasses_mode, 0, 2);
    s.brightness = std::clamp(c.brightness, 0, 8);
    s.tilt = std::clamp(c.tilt, 0.0f, 1.0f);
    s.anchor_at_rest = c.anchor_at_rest;
    s.mac_screen = c.laptop_screen;
    for (int i = 0; i < std::clamp(c.pose_count, 0, NIMBO_MAX_PANELS); ++i) {
        s.panels.push_back({c.pose_yaw[i], c.pose_pitch[i], c.pose_scale[i]});
    }
    return s;
}

nimbo_settings to_c(const core::Settings& s) {
    nimbo_settings c{};
    c.cell_count = static_cast<int>(std::min<size_t>(s.cells.size(), NIMBO_MAX_PANELS));
    for (int i = 0; i < c.cell_count; ++i) {
        c.cols[i] = s.cells[static_cast<size_t>(i)].col;
        c.rows[i] = s.cells[static_cast<size_t>(i)].row;
    }
    c.text_size = s.text_size;
    c.distance_m = s.distance_m;
    c.gap_deg = s.gap_deg;
    c.curvature = s.curvature;
    c.size_deg = s.size_deg;
    c.auto_start = s.auto_start;
    c.fov_y_deg = s.fov_y_deg;
    c.ipd_mm = s.ipd_mm;
    c.stereo = s.stereo;
    c.dead_zone_deg = s.dead_zone_deg;
    c.gaze_cursor = s.gaze_cursor;
    c.dim_others = s.dim_others;
    c.gather_windows = s.gather_windows;
    c.k1 = s.k1;
    c.k2 = s.k2;
    c.onboarding_done = s.onboarding_done;
    c.tracking = s.tracking;
    c.glasses_mode = s.glasses_mode;
    c.brightness = s.brightness;
    c.tilt = s.tilt;
    c.anchor_at_rest = s.anchor_at_rest;
    c.laptop_screen = s.mac_screen;
    c.pose_count = static_cast<int>(std::min<size_t>(s.panels.size(), NIMBO_MAX_PANELS));
    for (int i = 0; i < c.pose_count; ++i) {
        c.pose_yaw[i] = s.panels[static_cast<size_t>(i)].yaw_deg;
        c.pose_pitch[i] = s.panels[static_cast<size_t>(i)].pitch_deg;
        c.pose_scale[i] = s.panels[static_cast<size_t>(i)].scale;
    }
    return c;
}

}  // namespace

struct nimbo_server {
    std::mutex mutex;  // serialises start / stop / destroy
    std::unique_ptr<Server> server;
    std::thread thread;
    std::atomic<bool> finished{false};  // the server thread ended (stopped on its own or by nimbo_stop)
    nimbo_status last{};                // after the server stopped
};

struct nimbo_calibration {
    std::atomic<bool> cancel{false};
};

extern "C" {

nimbo_options nimbo_default_options(void) {
    nimbo_options o{};
    o.displays = 2;
    o.vd_width = 1920;
    o.vd_height = 1080;
    o.vd_scale = 150;
    o.gap_deg = 1.0f;
    o.distance_m = 2.0f;
    o.gather_windows = true;
    o.gaze_cursor = true;
    o.dim_others = 1.0f;
    o.side_by_side = -1;
    o.ipd_mm = 63.0f;
    copy_str(o.calibration_path, sizeof(o.calibration_path), "calibration.json");
    o.dead_zone_deg = 1.0f;
    copy_str(o.source, sizeof(o.source), "auto");
    o.refresh120 = true;
    o.hotkeys = true;
    o.tracking_axes = 1;
    o.glasses_mode = -1;
    o.tilt = 1.0f;
    return o;
}

nimbo_server* nimbo_create(void) { return new nimbo_server(); }

void nimbo_destroy(nimbo_server* s) {
    if (s == nullptr) return;
    nimbo_stop(s);
    delete s;
}

bool nimbo_start(nimbo_server* s, const nimbo_options* options) {
    std::lock_guard<std::mutex> lock(s->mutex);
    if (s->server && !s->finished) return true;
    if (s->server) {  // stopped on its own (glasses unplugged, Ctrl+Alt+Q): reap it
        s->thread.join();
        s->server.reset();
    }
    if (g_server_running.exchange(true)) {
        s->last = nimbo_status{};
        copy_str(s->last.message, sizeof(s->last.message), "another Nimbo server is already running");
        return false;
    }
    s->server = std::make_unique<Server>(from_c(*options));
    Server* srv = s->server.get();
    s->finished = false;
    std::promise<bool> started;
    std::future<bool> result = started.get_future();
    s->thread = std::thread([s, srv, &started] {
        SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        timeBeginPeriod(1);
        t_last_line = &srv->last_line;
        const bool ok = srv->start();
        if (!ok) srv->request_stop(srv->last_line);  // the reason start failed
        started.set_value(ok);
        if (ok) srv->run();
        srv->shutdown();
        t_last_line = nullptr;
        timeEndPeriod(1);
        g_server_running = false;
        s->finished = true;
    });
    if (result.get()) return true;
    s->thread.join();
    s->last = srv->status();
    s->server.reset();
    return false;
}

void nimbo_stop(nimbo_server* s) {
    std::lock_guard<std::mutex> lock(s->mutex);
    if (!s->server) return;
    s->server->request_stop("");
    if (s->thread.joinable()) s->thread.join();
    s->last = s->server->status();
    s->server.reset();
}

void nimbo_apply(nimbo_server* s, const nimbo_options* options) {
    std::lock_guard<std::mutex> lock(s->mutex);
    if (s->server) s->server->request_apply(from_c(*options));
}

void nimbo_recenter(nimbo_server* s) {
    std::lock_guard<std::mutex> lock(s->mutex);
    if (s->server) s->server->post(WM_APP + 1);
}

void nimbo_adjust_fov(nimbo_server* s, float delta_deg) {
    std::lock_guard<std::mutex> lock(s->mutex);
    if (s->server) s->server->post(WM_APP + 2, 0, static_cast<LPARAM>(std::lround(delta_deg * 100.0f)));
}

void nimbo_toggle_focus(nimbo_server* s, bool cinema) {
    std::lock_guard<std::mutex> lock(s->mutex);
    if (s->server) s->server->post(WM_APP + 3, cinema ? 1 : 0);
}

void nimbo_get_status(nimbo_server* s, nimbo_status* out) {
    std::lock_guard<std::mutex> lock(s->mutex);
    *out = s->server ? s->server->status() : s->last;
    // A server that stopped on its own (glasses unplugged, Ctrl+Alt+Q) reports STOPPED with
    // its reason; nimbo_stop then only joins its thread.
}

void nimbo_set_log(nimbo_log_fn fn, void* ctx) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    g_log_fn = fn;
    g_log_ctx = ctx;
}

bool nimbo_glasses_connected(void) {
    DpiScope dpi;
    return win::find_glasses(win::enumerate_outputs()) != nullptr;
}

bool nimbo_virtual_driver_available(void) { return win::find_virtual_display_backend() != nullptr; }

bool nimbo_restore_displays(void) {
    DpiScope dpi;
    return restore_from_backup();
}

nimbo_settings nimbo_default_settings(void) { return to_c(core::Settings{}); }

bool nimbo_load_settings(const char* path, nimbo_settings* out) {
    *out = nimbo_default_settings();
    std::ifstream f(std::filesystem::u8path(path));
    if (!f) return true;  // first run
    std::stringstream ss;
    ss << f.rdbuf();
    const auto s = core::settings_from_json(ss.str());
    if (!s) return false;
    *out = to_c(*s);
    return true;
}

bool nimbo_save_settings(const char* path, const nimbo_settings* settings) {
    const std::filesystem::path p = std::filesystem::u8path(path);
    std::error_code ec;
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::trunc);
    f << core::settings_to_json(to_core(*settings));
    return static_cast<bool>(f);
}

void nimbo_options_from_settings(const nimbo_settings* settings, nimbo_options* out) {
    // Text size -> Windows scaling of the virtual monitors; the macOS steps (1920 down to
    // 1024 points wide) are the same text sizes as 100..200 % at 1920x1080.
    static constexpr int kScale[] = {100, 125, 150, 175, 200};
    const core::Settings s = to_core(*settings);
    std::vector<core::GridCell> cells = core::normalized_cells(s.cells);
    const bool placed = !s.panels.empty() && s.panels.size() == cells.size();
    if (placed) {
        // The OS arrangement follows where the monitors were placed.
        const float vertical = 2.0f * std::atan(std::tan(s.size_deg * 0.00872664626f) / (16.0f / 9.0f)) / 0.0174532925f;
        cells = core::cells_from_poses(s.panels, s.size_deg, vertical);
    }
    nimbo_options o = *out;
    o.displays = static_cast<int>(cells.size());
    o.layout_count = o.displays;
    for (int i = 0; i < o.layout_count; ++i) {
        o.layout_cols[i] = cells[static_cast<size_t>(i)].col;
        o.layout_rows[i] = cells[static_cast<size_t>(i)].row;
    }
    o.vd_scale = kScale[std::clamp(s.text_size, 0, 4)];
    o.distance_m = s.distance_m;
    o.gap_deg = s.gap_deg;
    o.curvature = s.curvature;
    o.arc_deg = s.size_deg;
    o.fov_y_deg = s.fov_y_deg;
    o.ipd_mm = s.ipd_mm;
    o.side_by_side = s.stereo ? 1 : 0;
    o.dead_zone_deg = s.dead_zone_deg;
    o.gaze_cursor = s.gaze_cursor;
    o.dim_others = s.dim_others ? 0.55f : 1.0f;  // linear light: ~80 % perceived brightness
    o.gather_windows = s.gather_windows;
    o.tracking_axes = s.tracking;
    // The app has one way of seeing the panels, as on macOS: the space at real size. Nimbo puts
    // the glasses in follow (and back in pinned when it stops), every panel as large as their
    // image, fixed in the room; the stabilizer world-locked, frozen at rest with the anchor.
    o.glasses_mode = 0;
    o.glasses_pinned = false;
    o.recenter_glasses = false;
    o.real_size = true;
    o.dead_zone_deg = s.anchor_at_rest ? -1.3f : -1.0f;  // see rayneo_config: -1 - hold
    o.tilt = s.tilt;
    o.laptop_screen = s.mac_screen;
    o.brightness = s.brightness;
    o.pose_count = placed ? static_cast<int>(s.panels.size()) : 0;
    for (int i = 0; i < o.pose_count; ++i) {
        o.pose_yaw[i] = s.panels[static_cast<size_t>(i)].yaw_deg;
        o.pose_pitch[i] = s.panels[static_cast<size_t>(i)].pitch_deg;
        o.pose_scale[i] = s.panels[static_cast<size_t>(i)].scale;
    }
    o.k1 = s.k1;
    o.k2 = s.k2;
    *out = o;
}

int nimbo_list_windows(nimbo_window* out, int max) {
    int n = 0;
    for (const auto& w : win::list_windows()) {
        if (n >= max) break;
        out[n] = nimbo_window{};
        out[n].hwnd = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(w.hwnd));
        copy_str(out[n].title, sizeof(out[n].title), w.title);
        copy_str(out[n].app, sizeof(out[n].app), w.app);
        ++n;
    }
    return n;
}

int nimbo_layout_count(void) { return static_cast<int>(core::layout_presets().size()); }

bool nimbo_layout_at(int index, nimbo_layout* out) {
    const auto& all = core::layout_presets();
    if (index < 0 || index >= static_cast<int>(all.size())) return false;
    const auto& p = all[static_cast<size_t>(index)];
    *out = nimbo_layout{};
    out->id = p.id.data();  // string literal: NUL-terminated, static
    out->cell_count = static_cast<int>(std::min<size_t>(p.cells.size(), NIMBO_MAX_PANELS));
    for (int i = 0; i < out->cell_count; ++i) {
        out->cols[i] = p.cells[static_cast<size_t>(i)].col;
        out->rows[i] = p.cells[static_cast<size_t>(i)].row;
    }
    out->curvature = p.curvature;
    out->size_deg = p.arc_deg;
    out->gap_deg = p.gap_deg;
    return true;
}

int nimbo_profile_count(void) { return static_cast<int>(core::profiles().size()); }

bool nimbo_profile_at(int index, nimbo_profile* out) {
    const auto& all = core::profiles();
    if (index < 0 || index >= static_cast<int>(all.size())) return false;
    const auto& p = all[static_cast<size_t>(index)];
    out->id = p.id.data();
    out->layout_id = p.layout_id.data();
    out->distance_m = p.distance_m;
    out->text_size = p.text_size;
    out->curvature = p.curvature;
    out->size_deg = p.arc_deg;
    return true;
}

nimbo_calibration* nimbo_calibration_create(void) { return new nimbo_calibration(); }

void nimbo_calibration_destroy(nimbo_calibration* c) { delete c; }

void nimbo_calibration_cancel(nimbo_calibration* c) { c->cancel = true; }

int nimbo_calibration_run(nimbo_calibration* c, const char* out_path, nimbo_calib_fn fn, void* ctx, char* error,
                          int error_size) {
    auto fail = [&](int code, const std::string& why) {
        if (error != nullptr && error_size > 0) copy_str(error, static_cast<size_t>(error_size), why);
        logln("calibration: %s", why.c_str());
        return code;
    };
    if (g_server_running.load()) return fail(1, "stop Nimbo first: the calibration needs the glasses' IMU");
    c->cancel = false;
    const auto outcome = rayneo::platform::GuidedCalibration().run(c->cancel, [&](const rayneo::platform::CalibStatus& st) {
        if (fn == nullptr) return;
        nimbo_calib_status cs{};
        cs.step = static_cast<nimbo_calib_step>(st.step);
        cs.returning = st.returning;
        cs.hint = st.hint;
        cs.rate_degs = st.rate_degs;
        cs.rotation_deg = st.rotation_deg;
        cs.bias_ok = st.bias_ok;
        cs.captured_deg = st.captured_deg;
        fn(ctx, &cs);
    });
    if (!outcome.completed) return fail(1, outcome.error);
    const auto& r = outcome.result;
    if (!r.ok) return fail(2, r.error);
    const std::filesystem::path p = std::filesystem::u8path(out_path);
    std::error_code ec;
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::trunc);
    f << core::calibration_to_json(r.sensor_to_head);
    if (!f) return fail(1, std::string("cannot write ") + out_path);
    logln("calibration: saved (yaw axis vs gravity %.1f deg)", r.yaw_axis_error_deg);
    return 0;
}

}  // extern "C"
