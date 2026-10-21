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
#include "rayneo/core/latch_controller.h"
#include "rayneo/core/fov_tuner.h"
#include "rayneo/core/supersample_policy.h"
#include "rayneo/core/presets.h"
#include "rayneo/core/settings.h"
#include "rayneo/platform/guided_calibration.h"
#include "rayneo/platform/nimbo_win.h"

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002  // Windows 10 1803+
#endif
#include "rayneo/platform/rayneo_engine.h"
#include "rayneo/platform/rayneo_layouts.h"
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
    std::optional<bool> side_by_side;  // nil: both halves when the glasses are in 3D mode, mono in --windowed
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
    float zoom = 1.0f;                     // every panel this much larger (shortcuts), as left last time
    bool grow_by_gaze = true;              // glasses' pinned mode: the panel looked at takes over (else mouse only)
    bool vehicle_mode = false;             // a quarter of the head's heading followed (core::scale_heading)
    bool flat_wall = true;                 // every panel in one plane, edge to edge (core::wall_grid_layout)
    int background = 0;                    // around the panels (win::RenderOptions::background)
    float background_brightness = 0.6f;
    std::string background_image;          // UTF-8

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
    if (c.zoom > 0) o.zoom = std::clamp(c.zoom, 0.5f, 3.0f);
    o.grow_by_gaze = c.grow_by_gaze;
    o.vehicle_mode = c.vehicle_mode;
    o.flat_wall = c.flat_wall;
    o.background = std::clamp(c.background, 0, 5);
    o.background_brightness = std::clamp(c.background_brightness, 0.0f, 1.0f);
    o.background_image.assign(c.background_image, strnlen(c.background_image, sizeof(c.background_image)));
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
    float panels_pitch_rad = 0.0f;  // the panels lifted to the last recentre's elevation
    float gyro_noise_degs = 0.0f;   // gyro white noise at rest (deg/s per axis)
    bool gyro_noisy = false;        // far above normal: a poor cable or USB port
    float temp_c = 0.0f;            // the IMU's temperature (C)
    bool worn = true;               // on a head (false after 30 s lying still)
    core::Vec3 bias;                // gyro bias in use (deg/s)
};

class PoseSource {
public:
    virtual ~PoseSource() = default;
    virtual void recenter() = 0;
    virtual std::optional<PoseSample> latest() = 0;
    virtual void nudge(float, float) {}
    virtual void set_glasses_mode(int) {}  // the glasses' own display mode (IMU only)
    virtual bool reconnect(std::string*) { return false; }  // IMU only: reopen the glasses' HID device
    virtual bool stats(rayneo_stats*) { return false; }  // IMU transport counters (IMU only)
    virtual void set_glasses_brightness(int) {}
    virtual bool dump_recent(const char*) { return false; }  // IMU only: the last 30 s of reports
};

class ImuPose final : public PoseSource {
public:
    explicit ImuPose(float dead_zone_deg) {
        rayneo_config cfg = rayneo_default_config();
        if (dead_zone_deg != 0) cfg.dead_zone_deg = dead_zone_deg;  // < 0: world-locked (see rayneo_config)
        engine_ = rayneo_engine_create(&cfg);
        // The gyro bias per sensor temperature, kept between runs.
        wchar_t base[MAX_PATH];
        if (const DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH); len > 0 && len < MAX_PATH) {
            const std::wstring path = std::wstring(base) + L"\\Nimbo\\rayneo_biasmap.json";
            const int n = WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, nullptr, 0, nullptr, nullptr);
            std::string utf8(n > 0 ? static_cast<size_t>(n) : 1, '\0');
            WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, utf8.data(), n, nullptr, nullptr);
            rayneo_engine_set_bias_map_path(engine_, utf8.c_str());
        }
    }
    bool dump_recent(const char* path) override { return rayneo_engine_dump_recent(engine_, path); }
    ~ImuPose() override {
        rayneo_engine_stop(engine_);
        rayneo_engine_destroy(engine_);
    }
    bool load_calibration(const std::string& path) {
        std::optional<core::Quat> q;
        if (std::ifstream f(std::filesystem::u8path(path)); f) {
            std::stringstream ss;
            ss << f.rdbuf();
            q = core::calibration_from_json(ss.str());
        }
        const bool identity = q && std::fabs(q->w) > 0.9999f;
        const core::Quat use = q && !identity ? *q : core::kRayNeoGtMounting.normalized();
        rayneo_engine_set_sensor_to_head(engine_, use.w, use.x, use.y, use.z);
        if (!q || identity) logln("calibration: %s, the RayNeo GT's usual mounting instead", q ? "identity" : "none");
        return q && !identity;
    }
    bool start(std::string* error) {
        if (rayneo_engine_start(engine_) == RAYNEO_OK) return true;
        *error = rayneo_engine_last_error(engine_);
        return false;
    }
    void recenter() override { rayneo_engine_recenter(engine_); }
    void set_glasses_mode(int mode) override { rayneo_engine_set_glasses_mode(engine_, mode); }
    bool reconnect(std::string* error) override {
        rayneo_engine_stop(engine_);
        return start(error);
    }
    bool stats(rayneo_stats* out) override { return rayneo_engine_stats(engine_, out); }
    void set_glasses_brightness(int level) override { rayneo_engine_set_glasses_brightness(engine_, level); }
    std::optional<PoseSample> latest() override {
        rayneo_pose p;
        if (!rayneo_engine_latest_pose(engine_, &p)) return std::nullopt;
        return PoseSample{{p.qw, p.qx, p.qy, p.qz}, {p.wx, p.wy, p.wz}, p.host_time_ns, p.tracking, p.panels_pitch_rad,
                          p.gyro_noise_degs, p.gyro_noisy, p.temp_c, p.worn, {p.bias_x, p.bias_y, p.bias_z}};
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

// The space at real size (the normal mode), tunable within their ranges (as on macOS).
constexpr float kPredictionMs = 16.0f;    // motion to photon along the gyro's angular velocity (14..20)
constexpr float kOpticalVFovDeg = 23.0f;  // the RayNeo GT's optics (~46 deg diagonal): panels move as fast as the room
constexpr float kOpticalVFovMin = 22.2f, kOpticalVFovMax = 23.8f;

float load_optical_fov() {
    DWORD v = 0, size = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Nimbo", L"OpticalVFov100", RRF_RT_REG_DWORD, nullptr, &v, &size) !=
        ERROR_SUCCESS) {
        return kOpticalVFovDeg;
    }
    return std::clamp(static_cast<float>(v) / 100.0f, kOpticalVFovMin, kOpticalVFovMax);
}

void save_optical_fov(float fov) {
    const DWORD v = static_cast<DWORD>(std::lround(fov * 100.0f));
    RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\Nimbo", L"OpticalVFov100", REG_DWORD, &v, sizeof(v));
}

// ---------------------------------------------------------------------------------------
// Server
// ---------------------------------------------------------------------------------------

constexpr UINT_PTR kRepinTimer = 1;  // recentre in the glasses' pinned mode: back to pinned

enum HotKey {
    kRecenter = 1, kQuit, kFovDown, kFovUp, kLeft, kRight, kUp, kDown, kJump, kFocus, kCinema,
    kZoomIn, kZoomOut, kZoomReset, kCodeMode, kZoomInPad, kZoomOutPad, kOverview, kVehicle, kOptics
};


struct LatencyStats {
    int frames = 0, idle = 0;
    double pose_age_sum = 0, pose_age_max = 0;  // sample -> encode (ms)
    double pred_sum = 0;                        // horizon actually extrapolated (ms)
    int presented_n = 0;
    double presented_sum = 0, presented_max = 0;  // sample -> scanout (DXGI frame statistics)
    double target_err_sum = 0;                    // scanout - predicted target (ms)
    // As on macOS: frames shown a refresh (or more) after their target, split by the head moving
    // or still when drawn; the scanout rhythm; the pose read to vsync as applied (late latching).
    int missed = 0, missed_moving = 0, missed_still = 0, missed_latch = 0;
    int late_switches = 0;          // on time <-> a refresh late (each one a step of the panels)
    int gpu_n = 0;
    double gpu_sum = 0;             // GPU time per frame (ms, timestamp queries)
    int composed = 0, overlay = 0, not_composed = 0, failed = 0;  // DXGI_FRAME_PRESENTATION_MODE of the stats read
    double cpu_sum = 0, cpu_max = 0;  // wake-up to Present (ms)
    int interval_n = 0;
    double interval_sum = 0, interval_sq = 0, interval_max = 0;  // ms
    int latch_n = 0;
    double latch_sum = 0;  // ms
    // With the head moving (> 1 deg/s): frame-to-frame change of the drawn pose's step (deg).
    int moving_n = 0;
    double moving_sq = 0;
    bool tracking = false;
    // With the head still (< 1 deg/s): frame-to-frame change of the pose drawn, i.e. how much
    // the panels wobble in the room (deg rms), as on macOS.
    int still_n = 0;
    double still_sq = 0;
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
    bool dump_recent_imu(const char* path) { return pose_ && pose_->dump_recent(path); }
    std::string last_line;  // server thread: last log line (why start failed)

private:
    bool create_window(const RECT& rect);
    bool create_swap_chain();
    bool create_back_buffer_view();
    uint64_t estimate_vsync(uint64_t now);
    // How much later than its target a frame reaches the glasses (ms, smoothed): the pose is
    // predicted for that real moment, or the panels lag and jerk while turning (as on macOS).
    double present_late_ms_ = 0.0;
    // Late latching: pose read this long before the vsync; grows only on a refresh the frame's
    // CPU work (wake-up to Present, measured) missed (core/latch_controller.h).
    core::LatchState latch_ = core::latch_start(1000.0f / 120);
    core::Quat last_drawn_{};
    bool have_last_drawn_ = false;
    rayneo_stats last_imu_{};  // counters at the previous stats line
    bool frame();
    void collect_presented();
    void periodic();
    void handle_display_change();
    // The glasses in their 3D mode are a double-width display (3840x1080): the image in both halves.
    bool auto_side_by_side() const { return !opt_.windowed && height_ > 0 && width_ > 3 * height_; }
    uint64_t glasses_lost_ns_ = 0;  // their display went away at (0: present)
    // Wallpaper copy to the new monitors, retried while they do not take it yet.
    RECT wallpaper_from_{};
    std::vector<RECT> wallpaper_to_;
    int wallpaper_tries_ = 0;
    uint64_t wallpaper_next_ns_ = 0;
    bool setup_virtual_displays();
    const win::OutputInfo* find_output(const std::wstring& gdi_name) const;
    void refresh_rects();
    void follow_gaze(uint64_t now);
    // The cursor over the monitor panels, read just before each frame (floating windows'
    // captures draw their own).
    void update_cursor();
    // Glasses' pinned mode: the panel looked at for 0.6 s (or the mouse taken to) fills their
    // image, as on macOS.
    void update_grow(uint64_t now);
    int grow_candidate_ = -2;
    uint64_t grow_since_ns_ = 0;
    int grow_mouse_panel_ = -1;
    void apply_pinned(const Options& o);
    void jump_cursor_to_gaze();
    void toggle_focus(bool cinema);
    int focus_mode_ = 0;  // 0 all, 1 focus, 2 cinema
    // Zoom (Ctrl+Alt++ / -, 0; code mode Ctrl+Alt+K = 170 %): every panel larger and the
    // arrangement with it, like walking up to the screens. For the run.
    void set_zoom(float z, const std::wstring& message = {}, bool save = true);
    void zoom_step(int direction);
    // Ctrl+Alt+A: every panel at once, seen from afar (pinned: all fitted in the glasses' image;
    // real size: zoomed out until they fit). Again: back as it was.
    void toggle_overview();
    bool overview_ = false;
    float zoom_before_overview_ = 1.0f;
    int grow_before_overview_ = -1;
    float zoom_ = 1.0f;
    bool code_mode_ = false;
    bool overlay_shown_ = false;  // start-up message on the glasses until tracking starts
    // A short message on the glasses ("Vista de todas las pantallas"), on the start-up card;
    // two in a row share it. Waits for the start-up message to end.
    void flash(const std::wstring& line, double seconds = 2.5);
public:
    void show_message(const std::wstring& text) { flash(text); }
private:
    std::vector<std::wstring> flash_lines_, pending_flash_;
    uint64_t flash_at_ns_ = 0, flash_until_ns_ = 0;
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
    bool no_virtual_ = false;  // asked for virtual monitors, but the driver did not answer
    bool parsec() const { return backend_ && std::string(backend_->name()) == "Parsec VDD"; }
    // Parsec VDD: monitors added or removed while running (the layout changed), no restart.
    bool reconfigure_displays(const Options& o);
    bool start_captures(std::string* error);
    std::vector<const win::OutputInfo*> wait_virtual_outputs(int n, const std::vector<std::wstring>& keep_order);
    HANDLE frame_timer_ = nullptr;  // high-resolution waitable timer (the wait before latching the pose)
    uint64_t imu_lost_ns_ = 0, imu_retry_ns_ = 0;  // IMU watchdog
    bool driver_needs_setup_ = false;  // driver off and no task to switch it on
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
    // A monitor of its own per floating window (GDI name, empty: none), from the driver's pool
    // and placed where the window floats, so the cursor reaches it as on macOS.
    std::vector<std::wstring> floating_monitors_;
    void assign_floating_monitors();
    std::optional<DISPLAYCONFIG_TOPOLOGY_ID> saved_topology_;  // Win+P mode before we extended
    bool driver_switched_on_ = false;   // we switched the Virtual Display Driver on: off again on exit
    uint64_t arranged_ns_ = 0;          // our own arrangement: its display-change messages are ours
    void rearrange_displays();
    // The glasses' native mode is a temporary one (CDS_FULLSCREEN): applying the saved display
    // settings (placing a monitor) puts them back at 60 Hz. Set it again after such a change.
    void restore_glasses_mode();
    void keep_floating_windows();
    std::vector<const win::OutputInfo*> virtual_panel_outputs() const;

    struct Sent {
        uint64_t sample_ns = 0, target_ns = 0;
        bool still = false;
        float cpu_ms = 0;   // wake-up (pose read) to Present returning
        float gpu_ms = -1;  // its GPU time once the queries are read back (-1: not yet)
    };
    // GPU time per frame: timestamp queries around the frame's draws, read back a few frames
    // later without waiting on the GPU.
    struct GpuQuery {
        ComPtr<ID3D11Query> disjoint, begin, end;
        UINT present = 0;
        bool pending = false;
    };
    GpuQuery gpu_q_[4];
    int gpu_q_next_ = 0;
    bool gpu_q_ok_ = false;
    void collect_gpu_times();
    ComPtr<IDXGISwapChainMedia> swap_media_;  // how each frame was presented (composed or not)
    bool last_late_ = false;
    bool gyro_noise_warned_ = false;  // said once a run (as macOS)
    bool paused_off_ = false;         // glasses taken off: captures stopped until they move
    std::vector<core::Vec3> bias_history_;  // a gyro bias a second, the last minute (diagnostics)
    int key_panel_tick_ = 0;
    void update_key_panel();
    core::SupersamplePolicy supersample_policy_;  // 1 while frames go a refresh late, as macOS
    // Ctrl+Alt+L: the optics assistant (core/fov_tuner.h), as macOS.
    std::optional<core::FovTuner> optics_;
    void toggle_optics();
    void optics_answer(bool against_head);
    void finish_optics(bool early);
    std::wstring optics_text() const;
    uint64_t last_motion_ns_ = 0;  // the last frame drawn with the head moving
    uint64_t last_shown_ns_ = 0;  // scanout of the last frame statistics collected
    UINT last_shown_count_ = 0;
    core::Quat last_predicted_{};
    core::Vec3 last_step_{};
    bool have_last_predicted_ = false, have_last_step_ = false;
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
    case WM_APP + 5: {  // nimbo_show_message: lp = new std::wstring
        std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring*>(lp));
        if (server != nullptr && text) server->show_message(*text);
        return 0;
    }
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

    // Windows duplicating the glasses (Win+P > Duplicate) or showing only one screen: the
    // laptop's screen is not a display of its own and new virtual monitors would duplicate too.
    // Extend for the session; shutdown() puts your mode back.
    if (!opt_.windowed && opt_.displays > 0) {
        const auto topology = win::current_topology();
        if (topology && *topology != DISPLAYCONFIG_TOPOLOGY_EXTEND) {
            if (win::set_topology(DISPLAYCONFIG_TOPOLOGY_EXTEND)) {
                saved_topology_ = *topology;
                logln("displays: Windows was %s, extended for this session (restored on exit)",
                      *topology == DISPLAYCONFIG_TOPOLOGY_CLONE ? "duplicating" : "on one screen");
                Sleep(2000);
                outputs_ = win::enumerate_outputs();
                for (const auto& o : outputs_) {
                    logln("  %s %dx%d at %ld,%ld%s%s", win::narrow(o.gdi_name).c_str(), o.width(), o.height(),
                          o.desktop.left, o.desktop.top, o.primary ? " [primary]" : "", o.glasses ? " [glasses]" : "");
                }
                glasses = win::find_glasses(outputs_);
                if (glasses == nullptr) {
                    logln("glasses display lost after extending the desktop");
                    return false;
                }
                glasses_gdi_ = glasses->gdi_name;
            }
        }
    }

    // The computer's own screen as panel 1: the primary display that is neither the glasses nor
    // a virtual monitor. With a single panel, that screen alone, no virtual monitors.
    if (opt_.laptop_screen && !opt_.windowed && opt_.displays > 0) {
        for (const auto& o : outputs_) {
            if (o.primary && !o.glasses && !win::is_virtual_panel(o)) laptop_gdi_ = o.gdi_name;
        }
        if (laptop_gdi_.empty()) {  // the glasses are primary: then your only other screen
            int others = 0;
            for (const auto& o : outputs_) {
                if (!o.glasses && !win::is_virtual_panel(o)) {
                    laptop_gdi_ = o.gdi_name;
                    ++others;
                }
            }
            if (others != 1) laptop_gdi_.clear();
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
    // The driver is kept off while Nimbo is closed (its monitors slowed the PC): switch it on.
    if (opt_.displays > 0 && !opt_.windowed && !win::parsec_vdd_available() && win::virtual_display_driver_installed() &&
        win::VirtualDisplayDriver::live_monitors() == 0) {
        driver_switched_on_ = win::virtual_driver_on(15000);
        driver_needs_setup_ = !driver_switched_on_;
        logln("virtual monitors: driver %s", driver_switched_on_ ? "switched on" : "off and no task to switch it on (Preparar pantallas virtuales)");
    }
    if (opt_.displays > 0) backend_ = win::find_virtual_display_backend();
    if (opt_.displays > 0 && !backend_) {
        no_virtual_ = true;
        if (win::virtual_display_driver_installed()) {
            logln("virtual monitors: the Virtual Display Driver is installed but does not answer on its control pipe"
                  " (update to 25.7.23 or newer, or restart Windows); showing an existing display instead");
        } else {
            logln("virtual monitors: no virtual display driver, showing an existing display instead"
                  " (install Parsec VDD: builds.parsec.app/vdd/parsec-vdd-0.45.0.0.exe)");
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
        // The glasses' mode change can take a virtual monitor off the desktop again (Windows
        // re-applies its own idea of the layout): put it back and lay everything out again.
        if (virtual_) {
            bool missing = false;
            for (const auto& name : panel_names_) missing = missing || find_output(name) == nullptr;
            if (missing) {
                const int n = win::attach_detached_virtual(opt_.vd_width, opt_.vd_height);
                Sleep(500);
                outputs_ = win::enumerate_outputs();
                rearrange_displays();
                glasses = find_output(glasses_gdi_);
                if (glasses == nullptr) {
                    logln("glasses display lost after restoring the virtual monitors");
                    return false;
                }
                int found = 0;
                for (const auto& name : panel_names_) found += find_output(name) != nullptr ? 1 : 0;
                logln("virtual monitors: %d dropped by the glasses' mode change, re-attached (%d of %zu panels there)", n,
                      found, panel_names_.size());
            }
        }
    } else if (const auto m = win::current_mode(L"")) {
        refresh_hz_ = m->hz;  // primary display
    }

    // Hall-of-mirrors guard: the glasses' own output is never a panel (it shows the render).
    if (virtual_ && !glasses_gdi_.empty()) {
        const auto it = std::remove(panel_names_.begin(), panel_names_.end(), glasses_gdi_);
        if (it != panel_names_.end()) {
            logln("capture: the glasses' display was among the panels, left out");
            panel_names_.erase(it, panel_names_.end());
        }
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
        if (calibrated) logln("calibration: %s", opt_.calibration_path.c_str());
        std::string error;
        // Right after the displays change the glasses can miss the stream-on: a few tries.
        bool streaming = false;
        for (int attempt = 1; attempt <= 4 && !streaming; ++attempt) {
            streaming = imu->start(&error);
            if (!streaming && attempt < 4) {
                logln("imu: %s, trying again (%d/3)", error.c_str(), attempt);
                Sleep(1500);
            }
        }
        if (!streaming) {
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
    // Without virtual monitors too (driver not answering): the glasses' surface is still a
    // screen of the desktop, beside yours, where windows and the cursor would be head-locked.
    if (glasses != nullptr && !glasses->mirrored() && sources.front()->gdi_name != glasses_gdi_) {
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
    ro.side_by_side = opt_.side_by_side.value_or(auto_side_by_side());
    ro.distance_m = opt_.distance_m;
    ro.arc_deg = opt_.panel_arc_deg();
    ro.gap_deg = opt_.gap_deg;
    ro.curvature = opt_.curvature;
    ro.fov_y_deg = opt_.fov_y_deg;
    if (!opt_.fov_from_cli) {
        if (const auto saved = load_fov()) ro.fov_y_deg = *saved;
    }
    // The space at real size: the optics' field of view and a fixed prediction.
    const bool normal_mode = opt_.real_size && !opt_.glasses_pinned;
    if (normal_mode) ro.fov_y_deg = load_optical_fov();
    ro.fixed_prediction_ms = normal_mode ? kPredictionMs : 0.0f;
    ro.supersample = normal_mode ? 2 : 1;  // eyes drawn twice as large, mipmapped down
    ro.k1 = opt_.k1;
    ro.k2 = opt_.k2;
    ro.dim_others = opt_.dim_others;
    ro.tracking_axes = static_cast<core::TrackingAxes>(opt_.tracking_axes);
    zoom_ = opt_.zoom;
    code_mode_ = std::fabs(zoom_ - 1.7f) < 0.01f;
    ro.zoom = zoom_;
    ro.background = opt_.background;
    ro.background_brightness = opt_.background_brightness;
    ro.background_image = opt_.background_image;
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
                          : opt_.real_size ? "space at real size: full head rotation, panels fixed in the room"
                                           : kTracking[opt_.tracking_axes]);
    logln("render: %s, %d %s panel(s) of %.1f deg gap %.1f deg at %.1f m, fov %.1f deg, k1 %.3f k2 %.3f",
          ro.side_by_side ? "both halves (glasses in 3D mode)" : "mono", panels, ro.curvature > 0 ? "curved" : "flat", ro.arc_deg, ro.gap_deg,
          ro.distance_m, ro.fov_y_deg, ro.k1, ro.k2);

    render_adapter_ = first->adapter;
    if (!start_captures(&error)) {
        logln("capture: %s", error.c_str());
        return false;
    }
    refresh_rects();
    if (overlay_shown_) renderer_.set_overlay_text(L"Nimbo se está preparando\nDeja las gafas quietas unos segundos");
    monitor_panels_ = panels;
    start_floating(opt_.floating);
    if (no_virtual_) {
        flash(driver_needs_setup_ ? L"Sin monitores virtuales: driver apagado\nInstala Parsec VDD (ver Ajustes > Diagnóstico)"
                                  : L"Sin monitores virtuales: solo tu pantalla\nInstala Parsec VDD (ver Ajustes > Diagnóstico)",
              8.0);
    }

    const UINT mods = MOD_CONTROL | MOD_ALT | MOD_NOREPEAT;
    struct Key {
        int id;
        UINT vk;
        const char* name;
        UINT extra = 0;  // more modifiers (Shift)
    };
    // VK_OEM_PLUS / VK_OEM_MINUS are the "+" / "-" keys in every layout (Spanish too).
    std::vector<Key> keys = {{kRecenter, 'C', "Ctrl+Alt+C"}, {kQuit, 'Q', "Ctrl+Alt+Q"},
                             {kFovDown, VK_OEM_MINUS, "Ctrl+Alt+Shift+-", MOD_SHIFT},
                             {kFovUp, VK_OEM_PLUS, "Ctrl+Alt+Shift++", MOD_SHIFT},
                             {kZoomIn, VK_OEM_PLUS, "Ctrl+Alt++"}, {kZoomOut, VK_OEM_MINUS, "Ctrl+Alt+-"},
                             {kZoomInPad, VK_ADD, "Ctrl+Alt+Num+"}, {kZoomOutPad, VK_SUBTRACT, "Ctrl+Alt+Num-"},
                             {kZoomReset, '0', "Ctrl+Alt+0"}, {kCodeMode, 'K', "Ctrl+Alt+K"},
                             {kOverview, 'A', "Ctrl+Alt+A"},
                             {kJump, 'J', "Ctrl+Alt+J"}, {kFocus, 'O', "Ctrl+Alt+O"}, {kCinema, 'P', "Ctrl+Alt+P"},
                             {kVehicle, 'V', "Ctrl+Alt+V"},
                             {kOptics, 'L', "Ctrl+Alt+L"}};  // F and H are the app's (background, shortcuts window)
    if (opt_.sim) {
        keys.push_back({kLeft, VK_LEFT, "Ctrl+Alt+Left"});
        keys.push_back({kRight, VK_RIGHT, "Ctrl+Alt+Right"});
        keys.push_back({kUp, VK_UP, "Ctrl+Alt+Up"});
        keys.push_back({kDown, VK_DOWN, "Ctrl+Alt+Down"});
    }
    if (opt_.hotkeys) {
        for (const auto& k : keys) {
            if (!RegisterHotKey(hwnd_, k.id, mods | k.extra, k.vk)) logln("hotkey %s: in use by another app", k.name);
        }
        logln("hotkeys: Ctrl+Alt+C recenter, Ctrl+Alt+Q quit, Ctrl+Alt++ / - zoom, Ctrl+Alt+Shift++ / - field of view");
    }
    {
        std::lock_guard<std::mutex> lock(shared_mutex_);
        status_.panel_count = panels;
        status_.fov_y_deg = renderer_.options.fov_y_deg;
        status_.zoom = zoom_;
    }

    frame_timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
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
    swap_media_.Reset();
    if (FAILED(swap_chain_.As(&swap_media_))) logln("surface: presentation mode not reported (no IDXGISwapChainMedia)");
    if (!gpu_q_ok_) {
        gpu_q_ok_ = true;
        D3D11_QUERY_DESC qd{};
        for (auto& q : gpu_q_) {
            qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
            gpu_q_ok_ = gpu_q_ok_ && SUCCEEDED(device_->CreateQuery(&qd, &q.disjoint));
            qd.Query = D3D11_QUERY_TIMESTAMP;
            gpu_q_ok_ = gpu_q_ok_ && SUCCEEDED(device_->CreateQuery(&qd, &q.begin)) &&
                        SUCCEEDED(device_->CreateQuery(&qd, &q.end));
        }
        if (!gpu_q_ok_) logln("gpu: timestamp queries unavailable (no GPU time per frame)");
    }
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

void Server::collect_gpu_times() {
    for (auto& q : gpu_q_) {
        if (!q.pending) continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
        if (ctx_->GetData(q.disjoint.Get(), &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        UINT64 t0 = 0, t1 = 0;
        if (ctx_->GetData(q.begin.Get(), &t0, sizeof(t0), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            ctx_->GetData(q.end.Get(), &t1, sizeof(t1), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) {
            continue;
        }
        q.pending = false;
        if (dj.Disjoint || dj.Frequency == 0 || t1 < t0) continue;  // clock changed: no reading
        const double ms = static_cast<double>(t1 - t0) * 1000.0 / static_cast<double>(dj.Frequency);
        sent_[q.present % 128].gpu_ms = static_cast<float>(ms);
        stats_.gpu_sum += ms;
        stats_.gpu_n += 1;
    }
}

void Server::collect_presented() {
    DXGI_FRAME_STATISTICS fs{};
    if (FAILED(swap_chain_->GetFrameStatistics(&fs)) || fs.PresentCount == last_stats_present_ ||
        fs.SyncQPCTime.QuadPart == 0) {
        return;
    }
    last_stats_present_ = fs.PresentCount;
    // Composed by DWM or flipped straight to the display (RealWarp checks Independent_Flip).
    DXGI_FRAME_STATISTICS_MEDIA fm{};
    if (swap_media_ && SUCCEEDED(swap_media_->GetFrameStatisticsMedia(&fm))) {
        switch (fm.CompositionMode) {
        case DXGI_FRAME_PRESENTATION_MODE_COMPOSED: stats_.composed += 1; break;
        case DXGI_FRAME_PRESENTATION_MODE_OVERLAY: stats_.overlay += 1; break;
        case DXGI_FRAME_PRESENTATION_MODE_NONE: stats_.not_composed += 1; break;
        default: stats_.failed += 1; break;
        }
    }
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
    if (late > 500.0 / std::max(1, refresh_hz_)) {
        stats_.missed += 1;
        (s.still ? stats_.missed_still : stats_.missed_moving) += 1;
    }
    // Consecutive presents only: a gap of skipped (idle) frames is not the rhythm.
    if (last_shown_ns_ != 0 && fs.PresentCount == last_shown_count_ + 1 && shown > last_shown_ns_) {
        const double gap = static_cast<double>(shown - last_shown_ns_) / 1e6;
        if (gap < 50.0) {
            stats_.interval_n += 1;
            stats_.interval_sum += gap;
            stats_.interval_sq += gap * gap;
            stats_.interval_max = std::max(stats_.interval_max, gap);
        }
    }
    last_shown_ns_ = shown;
    last_shown_count_ = fs.PresentCount;
    // Late latching: a refresh missed because the frame's work (CPU to Present, then its GPU
    // time when read back; as macOS, which uses the GPU's end) ran past the margin reads the
    // pose earlier from now on (a refresh lost to the compositor does not); a long run on time
    // narrows the margin again.
    const float period = 1000.0f / static_cast<float>(std::max(1, refresh_hz_));
    const bool missed = late > 0.5 * period;
    if (missed != last_late_) stats_.late_switches += 1;
    last_late_ = missed;
    const float work = s.cpu_ms + std::max(0.0f, s.gpu_ms);
    if (missed && core::latch_caused_miss(latch_, work)) stats_.missed_latch += 1;
    latch_ = core::latch_presented(latch_, missed, work, period);
    // Followed within a few frames, as on macOS (late in streaks: at 0.03 half a streak went by first).
    present_late_ms_ += (std::clamp(late, -10.0, 40.0) - present_late_ms_) * 0.15;
}

// Renders and presents one frame. False when it was skipped (nothing changed).
bool Server::frame() {
    collect_gpu_times();
    collect_presented();
    const uint64_t now = steady_ns();
    // Predicted for when the frame really reaches the glasses (vsync + measured lateness);
    // the lateness itself is measured against the vsync, as on macOS: measured against the
    // corrected target it fed back on itself and settled at half the real value.
    const uint64_t vsync = estimate_vsync(now);
    const uint64_t target = vsync + static_cast<uint64_t>(std::clamp(present_late_ms_, 0.0, 25.0) * 1e6);
    // Late latching, as on macOS: the swap chain wakes us about a refresh before the frame is
    // shown; the pose is read just before the vsync (minus an adaptive margin) instead, so less
    // of the head's motion has to be predicted.
    const uint64_t margin = static_cast<uint64_t>(static_cast<double>(latch_.margin_ms) * 1e6);
    const uint64_t wake = vsync > margin ? vsync - margin : 0;
    if (const uint64_t t = steady_ns(); wake > t + 300000 && wake - t < 20000000) {
        const uint64_t ms = (wake - t) / 1000000;
        if (frame_timer_ != nullptr && wake - t > 1000000) {
            // Up to 0.5 ms before the deadline on a high-resolution timer, the rest spinning.
            LARGE_INTEGER due;
            due.QuadPart = -static_cast<LONGLONG>((wake - t - 500000) / 100);
            if (SetWaitableTimer(frame_timer_, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(frame_timer_, 25);
        } else if (ms >= 2) {
            Sleep(static_cast<DWORD>(ms - 1));  // 1 ms timer resolution (timeBeginPeriod)
        }
        while (steady_ns() < wake) YieldProcessor();
    }
    update_cursor();
    const uint64_t read_ns = steady_ns();
    const auto p = pose_->latest();
    float prediction = 0;
    GpuQuery* timed = nullptr;
    if (!p) {
        renderer_.invalidate_skipped_frame();
        renderer_.render(back_rtv_.Get(), width_, height_, core::Quat{}, core::Vec3{}, 0, 0);
    } else {
        const bool forced = redraw_;
        redraw_ = false;
        stats_.tracking = p->tracking;
        // As macOS: frames drawn on for 0.5 s after the head stops (fewer on-time <-> late switches).
        const uint64_t t = steady_ns();
        if (p->angular_velocity.norm() >= 0.0175f) last_motion_ns_ = t;
        if (t - std::min(t, last_motion_ns_) < 500000000ull) {
            renderer_.invalidate_skipped_frame();
        } else if (renderer_.can_skip_frame(p->orientation, p->angular_velocity, p->tracking, forced)) {
            stats_.idle += 1;
            return false;
        }
        renderer_.options.panels_pitch_rad = p->panels_pitch_rad;
        renderer_.options.present_late_ms = static_cast<float>(present_late_ms_);  // the fixed prediction follows it too
        renderer_.options.frame_period_ms = 1000.0f / static_cast<float>(std::max(1, refresh_hz_));
        renderer_.options.sample_age_ms = read_ns > p->host_ns ? static_cast<float>(read_ns - p->host_ns) / 1e6f : 0.0f;
        timed = gpu_q_ok_ && !gpu_q_[gpu_q_next_].pending ? &gpu_q_[gpu_q_next_] : nullptr;
        if (timed) {
            ctx_->Begin(timed->disjoint.Get());
            ctx_->End(timed->begin.Get());
        }
        prediction = renderer_.render(back_rtv_.Get(), width_, height_, p->orientation, p->angular_velocity,
                                      p->host_ns, target);
        if (timed) {
            ctx_->End(timed->end.Get());
            ctx_->End(timed->disjoint.Get());
        }
    }
    const HRESULT hr = swap_chain_->Present(1, 0);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        logln("gpu device lost (0x%08lx): stopping", static_cast<unsigned long>(device_->GetDeviceRemovedReason()));
        request_stop("GPU device lost");
        return true;
    }
    UINT id = 0;
    const bool still = p && p->angular_velocity.norm() < 0.0175f;
    const float cpu_ms = static_cast<float>(steady_ns() - read_ns) / 1e6f;
    if (p && SUCCEEDED(swap_chain_->GetLastPresentCount(&id))) {
        sent_[id % 128] = {p->host_ns, vsync, still, cpu_ms, -1.0f};
        if (timed) {
            timed->present = id;
            timed->pending = true;
            gpu_q_next_ = (gpu_q_next_ + 1) % 4;
        }
    }
    if (p) {
        // Still head: how much the drawn pose changes frame to frame (the panels' wobble).
        if (p->angular_velocity.norm() < 0.0175f && have_last_drawn_) {
            // From the relative rotation's vector part: acos of a dot product near 1 rounds small
            // steps to 0 in float (the log read 0 while the panels trembled).
            const core::Quat r = last_drawn_.conjugate() * p->orientation;
            const double v = std::sqrt(static_cast<double>(r.x) * r.x + static_cast<double>(r.y) * r.y +
                                       static_cast<double>(r.z) * r.z);
            const double step = 2.0 * std::asin(std::fmin(1.0, v)) * 57.29577951308232;
            if (step < 1.0) {
                stats_.still_n += 1;
                stats_.still_sq += step * step;
            }
        }
        last_drawn_ = p->orientation;
        have_last_drawn_ = true;
        // The pose as drawn (the renderer's prediction redone along the gyro, its horizon), as on macOS.
        const core::Quat drawn = core::predict_orientation(p->orientation, p->angular_velocity, prediction * 1e-3f);
        if (have_last_predicted_) {
            core::Quat r = last_predicted_.conjugate() * drawn;
            if (r.w < 0) r = core::Quat{-r.w, -r.x, -r.y, -r.z};
            const core::Vec3 step{2.0f * r.x * 57.29578f, 2.0f * r.y * 57.29578f, 2.0f * r.z * 57.29578f};
            if (!still && have_last_step_) {
                const double j = (step - last_step_).norm();
                if (j < 2.0) {
                    stats_.moving_n += 1;
                    stats_.moving_sq += j * j;
                }
            }
            last_step_ = step;
            have_last_step_ = true;
        }
        last_predicted_ = drawn;
        have_last_predicted_ = true;
        if (vsync > read_ns) {
            stats_.latch_n += 1;
            stats_.latch_sum += static_cast<double>(vsync - read_ns) / 1e6;
            stats_.cpu_sum += cpu_ms;
            stats_.cpu_max = std::max(stats_.cpu_max, static_cast<double>(cpu_ms));
        }
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
        update_grow(now);
        const auto p = pose_->latest();
        const int gaze = p && p->tracking ? renderer_.panel_at_gaze(p->orientation) : -1;
        std::lock_guard<std::mutex> lock(shared_mutex_);
        status_.gaze_panel = gaze;
    }
    if (now < next_stats_ns_) return;
    next_stats_ns_ += 1000000000ull;
    if (next_stats_ns_ < now) next_stats_ns_ = now + 1000000000ull;
    if (glasses_lost_ns_ != 0) handle_display_change();  // waiting for the glasses' display to come back
    if (wallpaper_tries_ > 0 && now >= wallpaper_next_ns_) {
        const int done = win::copy_wallpaper(wallpaper_from_, wallpaper_to_);
        wallpaper_tries_ = done >= static_cast<int>(wallpaper_to_.size()) ? 0 : wallpaper_tries_ - 1;
        wallpaper_next_ns_ = now + 2000000000ull;
        logln("wallpaper: copied to %d of %zu monitor(s) (again)", done, wallpaper_to_.size());
    }
    if (flash_until_ns_ != 0 && now >= flash_until_ns_ && !overlay_shown_) {
        renderer_.set_overlay_text(optics_text());  // the optics assistant's card, if open
        flash_until_ns_ = 0;
        redraw_ = true;
    }

    keep_floating_windows();

    // IMU watchdog: after 0.5 s without samples the engine holds the pose and sends stream-on
    // again (core/stream_watchdog.h); still nothing at 1.5 s (a USB hiccup, the cable moved) and
    // the HID device is reopened silently; after 10 s without it (unplugged) Nimbo stops,
    // restoring the displays.
    if (imu_) {
        const auto p = pose_->latest();
        const bool stale = !p || now - std::min(now, p->host_ns) > 1500000000ull;
        if (!stale) {
            if (imu_lost_ns_ != 0) {
                logln("imu: data again after %.1f s", static_cast<double>(now - imu_lost_ns_) / 1e9);
                imu_lost_ns_ = 0;
            }
        } else if (now >= imu_retry_ns_) {
            if (imu_lost_ns_ == 0) {
                imu_lost_ns_ = now;
                logln("imu: no data for 1.5 s (stream-on did not bring it back), reconnecting");
            }
            if (now - imu_lost_ns_ > 10000000000ull) {
                logln("glasses IMU gone for 10 s (unplugged?): stopping");
                request_stop("glasses disconnected");
            } else {
                std::string error;
                if (pose_->reconnect(&error)) {
                    logln("imu: reconnected (head tracking starts again in a few seconds)");
                    flash(L"Gafas reconectadas\nDeja la cabeza quieta un momento", 4.0);
                } else {
                    logln("imu: reconnect failed: %s", error.c_str());
                }
                imu_retry_ns_ = now + 2000000000ull;  // the new stream gets a moment to start
            }
        }
    }

    // A gyro far noisier than normal at rest: the bias is learned badly and the panels drift.
    if (imu_ && !gyro_noise_warned_) {
        if (const auto g = pose_->latest(); g && g->gyro_noisy) {
            gyro_noise_warned_ = true;
            logln("imu: gyro noise %.2f deg/s at rest, far above normal (0.05-0.16): try another cable or USB port",
                  g->gyro_noise_degs);
            flash(L"Mucho ruido en el sensor de las gafas\nPrueba otro cable o puerto USB", 6.0);
        }
    }

    // Glasses taken off (30 s lying still, core/wear_detector.h): the monitors' captures stop
    // and a message stays on the glasses, as macOS; the first motion starts them again.
    if (imu_) {
        if (const auto g = pose_->latest()) {
            if (!g->worn && !paused_off_) {
                paused_off_ = true;
                logln("glasses off (30 s lying still): captures paused until they move");
                for (auto& c : captures_) c->stop();
                renderer_.set_overlay_text(L"Nimbo en pausa\nPonte las gafas para seguir");
                redraw_ = true;
            } else if (g->worn && paused_off_) {
                paused_off_ = false;
                logln("glasses on again: captures restarting");
                renderer_.set_overlay_text(L"");
                redraw_ = true;
                std::string error;
                if (!start_captures(&error)) logln("capture: %s", error.c_str());
            }
        }
    }

    const LatencyStats s = stats_;
    stats_ = LatencyStats{};
    if (s.tracking && s.frames > 0 && opt_.real_size && !opt_.glasses_pinned) {  // the space at real size draws at 2
        const int ss = supersample_policy_.update(s.missed, static_cast<double>(now) / 1e9, 2);
        if (ss != renderer_.options.supersample) {
            renderer_.options.supersample = ss;
            redraw_ = true;
            logln(ss < 2 ? "render: frames a refresh late (%d missed): supersample 1 for a while" : "render: supersample %d again",
                  ss < 2 ? s.missed : ss);
        }
    }
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
        // Glasses that start cold (sensor under 28 C): the bias moves while they warm up, as macOS.
        if (const auto c = pose_ ? pose_->latest() : std::nullopt; imu_ && c && c->temp_c > 0.0f && c->temp_c < 28.0f) {
            logln("imu: glasses cold (%.1f C): tracking settles as they warm up (2-3 minutes)", c->temp_c);
            pending_flash_.push_back(L"Gafas fr\u00edas: la imagen se estabiliza\nen 2-3 minutos, al calentarse");
        }
        redraw_ = true;
        const auto waiting = std::move(pending_flash_);
        pending_flash_.clear();
        for (const auto& line : waiting) flash(line);
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
        status_.missed_per_s = static_cast<float>(s.missed);
        status_.gpu_ms = s.gpu_n > 0 ? static_cast<float>(s.gpu_sum / s.gpu_n) : 0.0f;
        status_.supersample = renderer_.options.supersample;
        if (const auto g = imu_ && pose_ ? pose_->latest() : std::nullopt) {
            status_.gyro_noise = g->gyro_noise_degs;
            status_.temp_c = g->temp_c;
            status_.worn = g->worn ? 1 : 0;
            bias_history_.push_back(g->bias);
            if (bias_history_.size() > 60) bias_history_.erase(bias_history_.begin());
            status_.bias_change = (g->bias - bias_history_.front()).norm();
        }
    }
    // IMU health over the last second: samples received (457 expected), lost, dropped.
    std::string imu;
    rayneo_stats st{};
    if (pose_ && pose_->stats(&st)) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), " | imu %llu Hz (missed %llu, dropped %llu)",
                      static_cast<unsigned long long>(st.imu_samples - last_imu_.imu_samples),
                      static_cast<unsigned long long>(st.missed_samples - last_imu_.missed_samples),
                      static_cast<unsigned long long>(st.reports_dropped - last_imu_.reports_dropped));
        imu = buf;
        if (st.stream_resends > last_imu_.stream_resends) {
            logln("imu: no samples for 0.5 s; pose held, stream-on sent again (%llu acknowledged so far)",
                  static_cast<unsigned long long>(st.stream_acks));
        }
        if (st.stalls_recovered > last_imu_.stalls_recovered) {
            logln("imu: samples again after %.0f ms (stream reconnected, nothing restarted)", st.last_stall_ms);
        }
        last_imu_ = st;
        std::lock_guard<std::mutex> lock(shared_mutex_);
        status_.stalls = static_cast<int>(st.stalls_recovered);
    }
    std::string wobble;
    if (s.still_n > 30) {
        char buf[48];
        std::snprintf(buf, sizeof(buf), " | still wobble %.4f deg/frame", std::sqrt(s.still_sq / s.still_n));
        wobble = buf;
    }
    if (s.moving_n > 30) {
        char buf[48];
        std::snprintf(buf, sizeof(buf), " | moving jitter %.3f deg", std::sqrt(s.moving_sq / s.moving_n));
        wobble += buf;
    }
    // GPU time and how the frames were presented ("direct": DXGI_FRAME_PRESENTATION_MODE_NONE,
    // not composed by DWM; see docs/WINDOWS_PRESENT.md).
    std::string present;
    {
        char buf[160];
        std::snprintf(buf, sizeof(buf), " | gpu %.1f ms | present composed %d, overlay %d, direct %d%s",
                      s.gpu_n > 0 ? s.gpu_sum / s.gpu_n : 0.0, s.composed, s.overlay, s.not_composed,
                      s.failed > 0 ? (", failed " + std::to_string(s.failed)).c_str() : "");
        present = buf;
    }
    const double in_n = std::max(1, s.interval_n);
    const double in_mean = s.interval_sum / in_n;
    const double in_std = std::sqrt(std::max(0.0, s.interval_sq / in_n - in_mean * in_mean));
    // Same fields and order as macOS (scripts/log_stats.py reads both).
    logln("%s render %3d fps%s | capture %3llu fps (%zu src) | pose age %.1f/%.1f ms | sample->scanout %.1f/%.1f ms"
        " | predicted %.1f ms (late %.1f) | frame interval %.1f+-%.1f max %.1f ms | missed %d (moving %d, still %d,"
        " latch %d, switches %d) | latch %.1f ms (applied %.1f, cpu %.1f/%.1f)%s%s%s",
        s.tracking ? "TRACK" : "CALIB", s.frames, s.idle > 0 ? (" (" + std::to_string(s.idle) + " idle)").c_str() : "",
        static_cast<unsigned long long>(cap_fps), captures_.size(), s.pose_age_sum / n, s.pose_age_max, s.presented_sum / pn,
        s.presented_max, s.pred_sum / n, s.target_err_sum / pn, in_mean, in_std, s.interval_max, s.missed, s.missed_moving,
        s.missed_still, s.missed_latch, s.late_switches, static_cast<double>(latch_.margin_ms),
        s.latch_n > 0 ? s.latch_sum / s.latch_n : 0.0, s.latch_n > 0 ? s.cpu_sum / s.latch_n : 0.0, s.cpu_max, imu.c_str(),
        wobble.c_str(), present.c_str());
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
        // Switching the glasses between 2D and 3D takes their display away for a moment (and
        // brings it back at another size, maybe under another name) while the IMU keeps going:
        // wait for it (periodic() looks again). Unplugged, the IMU stops and Nimbo stops.
        for (const auto& o : outputs_) {
            if (o.glasses) glasses = &o;
        }
        if (glasses == nullptr) {
            if (glasses_lost_ns_ == 0) {
                glasses_lost_ns_ = steady_ns();
                logln("glasses display gone (switching 2D / 3D?): waiting for it");
            } else if (steady_ns() - glasses_lost_ns_ > 10000000000ull) {
                logln("glasses display disconnected: stopping");
                request_stop("glasses disconnected");
            }
            return;
        }
        glasses_gdi_ = glasses->gdi_name;
        logln("glasses display back as %s", win::narrow(glasses_gdi_).c_str());
    }
    if (glasses_lost_ns_ != 0) {
        glasses_lost_ns_ = 0;
        if (const auto m = win::set_native_mode(glasses_gdi_, opt_.refresh120 ? 120 : 60)) {
            mode_changed_ = true;
            Sleep(300);
            outputs_ = win::enumerate_outputs();
            glasses = find_output(glasses_gdi_);
            if (glasses == nullptr) return;
            logln("glasses mode: %dx%d native", m->width, m->height);
        }
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
        // The glasses in their 3D mode (double width): the same image in both halves.
        renderer_.options.side_by_side = opt_.side_by_side.value_or(auto_side_by_side());
        logln("surface: %dx%d px, %s", width_, height_,
              renderer_.options.side_by_side ? "both halves (glasses in 3D mode)" : "mono (glasses in 2D)");
        renderer_.invalidate_skipped_frame();
        redraw_ = true;
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
    ro.vehicle_mode = o.vehicle_mode && !o.glasses_pinned;
    ro.flat_wall = o.flat_wall;
    ro.neck_model = !o.glasses_pinned && o.real_size;  // the eyes turn about the neck (core::view_matrix)
    // Space at real size (glasses in follow): every panel nearly as large as the glasses' own
    // image, fixed in the room on both axes. It fits both ways (a 16:10 laptop screen is taller
    // than the image): 94 % of what fits. The arc is set so the distance scaling cancels out.
    if (!o.glasses_pinned && o.real_size) {
        constexpr float kDegToRad = 0.017453292519943295f;
        ro.tracking_axes = core::TrackingAxes::Both;  // full rotation: fixed in the room, level with the floor
        const float fov_y = ro.fov_y_deg * kDegToRad;
        float tallest = 16.0f / 9.0f;
        for (const auto& name : panel_names_) {
            if (const auto* out = find_output(name); out != nullptr && out->height() > 0) {
                tallest = std::min(tallest, static_cast<float>(out->width()) / static_cast<float>(out->height()));
            }
        }
        const float wide = 2.0f * std::atan(std::tan(fov_y / 2) * (16.0f / 9.0f));
        const float tall = 2.0f * std::atan(std::tan(fov_y / 2) * tallest);
        // Flat: exactly the image (1:1 pixels looking straight at it, the sharpest). Curved: 94 %,
        // its edges come closer and would leave the image.
        const float fw = (ro.curvature > 0.01f ? 0.94f : 1.0f) * std::min(wide, tall);
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
        // Space at real size: the cinema screen centred where you look now, evenly framed.
        const auto gaze = pose_->latest();
        if (cinema && opt_.real_size && !opt_.glasses_pinned && gaze && gaze->tracking) {
            const core::PanelPlacement g = core::gaze_placement(gaze->orientation);
            ro.cinema_yaw_rad = g.yaw_rad;
            ro.cinema_pitch_rad = g.pitch_rad;
        } else {
            ro.cinema_yaw_rad = ro.cinema_pitch_rad = 0.0f;
        }
        logln("focus: %s on panel %d", cinema ? "cinema" : "focus", ro.focus_panel + 1);
        // The only panel shown: the cursor there (centre), unless it already is.
        const int i = ro.focus_panel;
        POINT c{};
        if (i >= 0 && i < static_cast<int>(panel_rects_.size()) && GetCursorPos(&c) &&
            !PtInRect(&panel_rects_[static_cast<size_t>(i)], c)) {
            const RECT& r = panel_rects_[static_cast<size_t>(i)];
            SetCursorPos((r.left + r.right) / 2, (r.top + r.bottom) / 2);
            last_cursor_ = {(r.left + r.right) / 2, (r.top + r.bottom) / 2};
            last_gaze_panel_ = i;
            logln("cursor: to panel %d", i + 1);
        }
    }
    redraw_ = true;
}

void Server::update_grow(uint64_t now) {
    if (!opt_.glasses_pinned || focus_mode_ != 0 || overview_ || renderer_.panel_count() < 2) {
        if (!opt_.glasses_pinned && renderer_.grow_panel() != -1) renderer_.set_grow_panel(-1);
        grow_candidate_ = -2;
        return;
    }
    const auto p = pose_->latest();
    if (!p || !p->tracking) return;
    const int current = renderer_.grow_panel();
    // The mouse taken to another monitor: that one, at once.
    POINT c{};
    if (GetCursorPos(&c)) {
        int under = -1;
        for (size_t i = 0; i < panel_rects_.size(); ++i) {
            const RECT& r = panel_rects_[i];
            if (c.x >= r.left && c.x < r.right && c.y >= r.top && c.y < r.bottom) under = static_cast<int>(i);
        }
        if (under != grow_mouse_panel_) {
            grow_mouse_panel_ = under;
            if (under >= 0 && under != current && now - last_cursor_move_ns_ < 500000000ull) {
                renderer_.set_grow_panel(under);
                logln("grow: panel %d (mouse)", under + 1);
                redraw_ = true;
                return;
            }
        }
    }
    if (!opt_.grow_by_gaze) {  // mouse only (Settings)
        grow_candidate_ = -2;
        return;
    }
    const int panel = renderer_.panel_at_gaze(p->orientation);
    if (panel < 0 || panel == current) {
        grow_candidate_ = -2;
        return;
    }
    if (panel != grow_candidate_) {
        grow_candidate_ = panel;
        grow_since_ns_ = now;
    } else if (now - grow_since_ns_ >= 600000000ull) {
        renderer_.set_grow_panel(panel);
        logln("grow: panel %d (gaze)", panel + 1);
        redraw_ = true;
    }
}

void Server::set_zoom(float z, const std::wstring& message, bool save) {
    zoom_ = std::clamp(z, 0.2f, 3.0f);
    renderer_.options.zoom = zoom_;
    renderer_.invalidate_skipped_frame();
    redraw_ = true;
    logln("zoom: %.0f %%", zoom_ * 100.0f);
    if (save) {
        std::lock_guard<std::mutex> lock(shared_mutex_);
        status_.zoom = zoom_;  // the app saves it: the same zoom next time
    }
    flash(message.empty() ? L"Zoom " + std::to_wstring(static_cast<int>(std::lround(zoom_ * 100.0f))) + L" %" : message);
}

void Server::zoom_step(int direction) {
    float z = zoom_ * (direction > 0 ? 1.15f : 1.0f / 1.15f);
    if (std::fabs(z - 1.0f) < 0.07f) z = 1.0f;  // lands on 100 % on the way
    code_mode_ = false;
    overview_ = false;
    set_zoom(z);
}

void Server::toggle_overview() {
    overview_ = !overview_;
    if (overview_) {
        if (opt_.glasses_pinned) {
            grow_before_overview_ = renderer_.grow_panel();
            renderer_.set_grow_panel(-1);
            flash(L"Vista de todas las pantallas");
        } else {
            zoom_before_overview_ = zoom_;
            set_zoom(renderer_.overview_zoom(), L"Vista de todas las pantallas", false);
        }
    } else if (opt_.glasses_pinned) {
        renderer_.set_grow_panel(grow_before_overview_);
        flash(L"Vuelta a la pantalla en uso");
    } else {
        set_zoom(zoom_before_overview_, L"Vuelta al zoom de antes", false);
    }
    logln("overview: %s", overview_ ? "on" : "off");
    redraw_ = true;
}

void Server::flash(const std::wstring& line, double seconds) {
    if (overlay_shown_) {
        pending_flash_.push_back(line);
        if (pending_flash_.size() > 2) pending_flash_.erase(pending_flash_.begin());
        return;
    }
    const uint64_t now = steady_ns();
    if (now - flash_at_ns_ < 600000000ull) {
        flash_lines_.push_back(line);
        if (flash_lines_.size() > 2) flash_lines_.erase(flash_lines_.begin());
    } else {
        flash_lines_ = {line};
    }
    flash_at_ns_ = now;
    flash_until_ns_ = now + static_cast<uint64_t>(seconds * 1e9);
    renderer_.set_overlay_text(flash_lines_.size() > 1 ? flash_lines_[0] + L"\n" + flash_lines_[1] : flash_lines_[0]);
    renderer_.invalidate_skipped_frame();
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
        if (IsIconic(f.hwnd)) ShowWindow(f.hwnd, SW_RESTORE);  // a minimised window draws nothing
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
    assign_floating_monitors();
}

// One Desktop Duplication per panel (panel_names_ order), on the render GPU.
bool Server::start_captures(std::string* error) {
    for (auto& c : captures_) c->stop();
    captures_.clear();
    DXGI_ADAPTER_DESC render_desc{};
    render_adapter_->GetDesc(&render_desc);
    for (size_t i = 0; i < panel_names_.size() && i < static_cast<size_t>(win::SpatialRenderer::kMaxPanels); ++i) {
        const win::OutputInfo* src = find_output(panel_names_[i]);
        if (src == nullptr) {
            if (error != nullptr) *error = win::narrow(panel_names_[i]) + " is gone";
            return false;
        }
        DXGI_ADAPTER_DESC desc{};
        src->adapter->GetDesc(&desc);
        if (desc.AdapterLuid.LowPart != render_desc.AdapterLuid.LowPart ||
            desc.AdapterLuid.HighPart != render_desc.AdapterLuid.HighPart) {
            logln("capture: %s is on another GPU (%s): copied through the CPU", win::narrow(src->gdi_name).c_str(),
                  win::narrow(src->adapter_name).c_str());
        }
        auto cap = std::make_unique<win::DesktopDuplication>();
        if (!cap->start(src->adapter.Get(), src->output.Get(), device_.Get(), error)) return false;
        renderer_.set_source(static_cast<int>(i), cap.get());
        captures_.push_back(std::move(cap));
        logln("capture: panel %zu <- %s %dx%d", i + 1, win::narrow(src->gdi_name).c_str(), src->width(), src->height());
    }
    return true;
}

// The driver's displays on the desktop, `n` of them, those in keep_order first (same order).
std::vector<const win::OutputInfo*> Server::wait_virtual_outputs(int n, const std::vector<std::wstring>& keep_order) {
    std::vector<const win::OutputInfo*> out;
    for (int waited = 0; waited <= 6000; waited += 250) {
        outputs_ = win::enumerate_outputs();
        out.clear();
        for (const auto& name : keep_order) {
            if (const auto* o = find_output(name); o != nullptr && win::is_virtual_panel(*o)) out.push_back(o);
        }
        std::vector<const win::OutputInfo*> fresh;
        for (const auto& o : outputs_) {
            if (o.glasses || !win::is_virtual_panel(o)) continue;
            if (std::find(keep_order.begin(), keep_order.end(), o.gdi_name) == keep_order.end()) fresh.push_back(&o);
        }
        std::sort(fresh.begin(), fresh.end(),
                  [](const win::OutputInfo* a, const win::OutputInfo* b) { return a->gdi_name < b->gdi_name; });
        for (const auto* o : fresh) out.push_back(o);
        if (static_cast<int>(out.size()) >= n) break;
        if (waited >= 1000 && waited % 1000 == 0) win::attach_detached_virtual(opt_.vd_width, opt_.vd_height, n);
        Sleep(250);
    }
    if (static_cast<int>(out.size()) > n) out.resize(static_cast<size_t>(n));
    return out;
}

bool Server::reconfigure_displays(const Options& o) {
    if (!virtual_ || !parsec() || opt_.windowed) return false;
    const int n = std::clamp(o.displays - (laptop_gdi_.empty() ? 0 : 1), 1, win::SpatialRenderer::kMaxPanels - 1);
    logln("virtual monitors: %d -> %d, without restarting", static_cast<int>(panel_names_.size()), o.displays);
    for (auto& c : window_captures_) c->stop();
    window_captures_.clear();
    for (auto& c : captures_) c->stop();
    captures_.clear();
    std::string error;
    if (!backend_->set_monitors(n, opt_.vd_width, opt_.vd_height, &error)) logln("virtual monitors: %s", error.c_str());
    std::vector<std::wstring> keep;
    for (const auto& name : panel_names_) {
        if (name != laptop_gdi_) keep.push_back(name);
    }
    const auto panels = wait_virtual_outputs(n, keep);
    panel_names_.clear();
    if (!laptop_gdi_.empty()) panel_names_.push_back(laptop_gdi_);
    for (const auto* p : panels) panel_names_.push_back(p->gdi_name);
    opt_.displays = o.displays;
    opt_.layout = o.layout;
    const auto* glasses = glasses_gdi_.empty() ? nullptr : find_output(glasses_gdi_);
    const auto* laptop = laptop_gdi_.empty() ? nullptr : find_output(laptop_gdi_);
    arranged_ns_ = steady_ns();
    if (!arrangement_.arrange(panels, opt_.layout, glasses, outputs_, opt_.vd_width, opt_.vd_height, &error, laptop)) {
        logln("virtual monitors: %s", error.c_str());
    }
    Sleep(500);
    outputs_ = win::enumerate_outputs();
    for (const auto& name : panel_names_) {
        if (name != laptop_gdi_) win::set_display_scale(name, opt_.vd_scale, &error);
    }
    restore_glasses_mode();
    if (!start_captures(&error)) {
        logln("capture: %s", error.c_str());
        request_stop("capture: " + error);
        return true;
    }
    monitor_panels_ = static_cast<int>(captures_.size());
    renderer_.set_panel_count(monitor_panels_);
    renderer_.set_grid(static_cast<int>(opt_.layout.size()) == monitor_panels_ ? opt_.layout : std::vector<core::GridCell>{});
    refresh_rects();
    start_floating(opt_.floating);
    last_gaze_panel_ = -2;
    renderer_.invalidate_skipped_frame();
    redraw_ = true;
    {
        std::lock_guard<std::mutex> lock(shared_mutex_);
        status_.panel_count = monitor_panels_;
    }
    flash(L"Pantallas: " + std::to_wstring(monitor_panels_));
    return true;
}

// Each floating window onto a spare monitor of the Virtual Display Driver's pool (no reload),
// that monitor laid out where the window floats: above the monitors it touches their top edge,
// so the cursor goes up into it. The window is maximised on it and stays captured as itself
// (its panel shows the window, the cursor over it included).
void Server::assign_floating_monitors() {
    // Monitors that were floating windows' and are no longer: off the desktop (Windows moves
    // any window left there back to a monitor).
    for (const auto& name : floating_monitors_) {
        if (!name.empty()) win::attach_display(name, 0, 0, 0, 0);  // zero size: detached
    }
    floating_monitors_.assign(floating_hwnds_.size(), std::wstring());
    refresh_rects();
    if (floating_hwnds_.empty() || !virtual_ || opt_.windowed || !backend_ ||
        (std::string(backend_->name()) != "Virtual Display Driver" && !parsec()) || panel_rects_.empty()) {
        return;
    }
    // Parsec VDD has no pool: one more display per floating window, made now.
    if (parsec()) {
        const int monitors = static_cast<int>(panel_names_.size()) - (laptop_gdi_.empty() ? 0 : 1);
        std::string error;
        if (!backend_->set_monitors(monitors + static_cast<int>(floating_hwnds_.size()), opt_.vd_width, opt_.vd_height, &error)) {
            logln("window: %s", error.c_str());
        }
        for (int waited = 0; waited < 4000 && win::spare_virtual_monitors(panel_names_).size() < floating_hwnds_.size();
             waited += 250) {
            Sleep(250);
        }
    }
    const auto pls = renderer_.current_placements();
    const float arc = renderer_.shown_arc_rad();
    const RECT r0 = panel_rects_[0];
    if (pls.empty() || arc <= 0.0f || r0.right <= r0.left) return;
    const float ppr = static_cast<float>(r0.right - r0.left) / (arc * pls[0].scale);  // pixels per radian
    RECT all = r0;  // the monitors' bounding box
    for (const RECT& r : panel_rects_) {
        all.left = std::min(all.left, r.left);
        all.top = std::min(all.top, r.top);
        all.right = std::max(all.right, r.right);
        all.bottom = std::max(all.bottom, r.bottom);
    }
    std::vector<std::wstring> used = panel_names_;
    std::vector<RECT> placed;
    const int w = opt_.vd_width, h = opt_.vd_height;
    arranged_ns_ = steady_ns();
    for (size_t i = 0; i < floating_hwnds_.size(); ++i) {
        const size_t k = static_cast<size_t>(monitor_panels_) + i;
        if (k >= pls.size()) break;
        const auto spare = win::spare_virtual_monitors(used);
        if (spare.empty()) {
            logln("window: no spare virtual monitor left for floating window %zu (the cursor will not reach it)", i + 1);
            continue;
        }
        const float cx = 0.5f * static_cast<float>(r0.left + r0.right) + (pls[k].yaw_rad - pls[0].yaw_rad) * ppr;
        const float cy = 0.5f * static_cast<float>(r0.top + r0.bottom) - (pls[k].pitch_rad - pls[0].pitch_rad) * ppr;
        // Touching the monitors on the side where it floats, overlapping them along that edge.
        int x = static_cast<int>(cx) - w / 2, y = static_cast<int>(cy) - h / 2;
        const auto clamp_x = [&] { x = std::clamp(x, static_cast<int>(all.left) - w + 64, static_cast<int>(all.right) - 64); };
        const auto clamp_y = [&] { y = std::clamp(y, static_cast<int>(all.top) - h + 64, static_cast<int>(all.bottom) - 64); };
        if (cy < all.top) {
            y = all.top - h;
            clamp_x();
        } else if (cy > all.bottom) {
            y = all.bottom;
            clamp_x();
        } else if (cx < all.left) {
            x = all.left - w;
            clamp_y();
        } else {
            x = all.right;
            clamp_y();
        }
        // Clear of the floating monitors placed before: further out on the same side.
        for (bool clash = true; clash;) {
            clash = false;
            for (const RECT& r : placed) {
                if (x < r.right && x + w > r.left && y < r.bottom && y + h > r.top) {
                    if (cy < all.top) y = r.top - h;
                    else if (cy > all.bottom) y = r.bottom;
                    else if (cx < all.left) x = r.left - w;
                    else x = r.right;
                    clash = true;
                }
            }
        }
        const std::wstring& name = spare.front();
        if (!win::attach_display(name, x, y, w, h)) {
            logln("window: could not place a monitor for floating window %zu", i + 1);
            continue;
        }
        used.push_back(name);
        placed.push_back(RECT{x, y, x + w, y + h});
        floating_monitors_[i] = name;
        const HWND hwnd = floating_hwnds_[i];
        ShowWindow(hwnd, SW_RESTORE);
        SetWindowPos(hwnd, nullptr, x + 40, y + 40, w - 80, h - 80, SWP_NOZORDER | SWP_NOACTIVATE);
        ShowWindow(hwnd, SW_MAXIMIZE);  // fills its monitor
        logln("window: floating window %zu on %s at %d,%d", i + 1, win::narrow(name).c_str(), x, y);
    }
    Sleep(300);
    outputs_ = win::enumerate_outputs();
    restore_glasses_mode();  // placing the monitors applied the saved settings: glasses at 60 Hz
    refresh_rects();
}

void Server::refresh_rects() {
    panel_rects_.clear();
    for (const auto& name : panel_names_) {
        const auto* o = find_output(name);
        panel_rects_.push_back(o != nullptr ? o->desktop : RECT{});
    }
    // Floating windows' monitors after the monitors, in panel order: the gaze cursor goes there
    // too. Only when every floating window has one (else the indexes would not line up).
    const bool all_floating = !floating_monitors_.empty() &&
        std::none_of(floating_monitors_.begin(), floating_monitors_.end(), [](const std::wstring& n) { return n.empty(); });
    if (all_floating && static_cast<int>(panel_rects_.size()) == monitor_panels_) {
        for (const auto& name : floating_monitors_) {
            const auto* o = find_output(name);
            panel_rects_.push_back(o != nullptr ? o->desktop : RECT{});
        }
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
    int n = opt_.displays - (laptop_gdi_.empty() ? 0 : 1);  // the laptop screen is one of them
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
    // Parsec VDD: each virtual monitor at the computer's own resolution and, with the laptop
    // screen as panel 1, its scaling too, so a window dragged across keeps its size.
    if (parsec()) {
        const win::OutputInfo* own = laptop_gdi_.empty() ? nullptr : find_output(laptop_gdi_);
        for (const auto& o : outputs_) {
            if (own == nullptr && o.primary && !o.glasses && !win::is_virtual_panel(o)) own = &o;
        }
        if (own != nullptr && own->width() > 0 && own->height() > 0) {
            opt_.vd_width = own->width();
            opt_.vd_height = own->height();
            if (!laptop_gdi_.empty()) {
                if (const auto pct = win::get_display_scale(laptop_gdi_)) opt_.vd_scale = *pct;
            }
            logln("virtual monitors: like %s, %dx%d at %d%%", win::narrow(own->gdi_name).c_str(), opt_.vd_width,
                  opt_.vd_height, opt_.vd_scale);
        }
    }
    if (!backend_->set_monitors(n, opt_.vd_width, opt_.vd_height, &error)) {
        // More panels than the driver has, and it will not make more until Windows restarts:
        // start with the ones it has rather than not at all.
        const int live = std::string(backend_->name()) == "Virtual Display Driver" ? win::VirtualDisplayDriver::live_monitors() : 0;
        if (live <= 0) {
            logln("virtual monitors: %s", error.c_str());
            return false;
        }
        logln("virtual monitors: %d asked, only %d available until Windows restarts (%s)", n, live, error.c_str());
        n = live;
        opt_.layout.resize(std::min(opt_.layout.size(), static_cast<size_t>(n + (laptop_gdi_.empty() ? 0 : 1))));
    }
    virtual_ = true;  // from here on shutdown() restores the arrangement
    std::vector<const win::OutputInfo*> panels;
    bool extended = false;
    for (int waited = 0; waited < 15000; waited += 250) {  // the driver adds its monitors
        Sleep(250);
        // With the glasses as the only active display (laptop lid closed, "second screen only"),
        // Windows adds the new monitors switched off or duplicating the glasses: extend the
        // desktop (Win+P > Extend), once.
        if (!extended && waited >= 2000) {
            extended = true;
            if (!saved_topology_) saved_topology_ = win::current_topology();  // put back on exit
            const bool ok = win::set_topology(DISPLAYCONFIG_TOPOLOGY_EXTEND);
            logln("virtual monitors: not there yet, desktop set to Extend (%s)", ok ? "ok" : "failed");
        }
        // Created by the driver but left off the desktop: attach them ourselves.
        if (waited >= 3000 && waited % 1000 == 0) {
            if (const int n_attached = win::attach_detached_virtual(opt_.vd_width, opt_.vd_height, n - static_cast<int>(panels.size()));
                n_attached > 0) {
                logln("virtual monitors: %d were off the desktop, attached", n_attached);
            }
        }
        outputs_ = win::enumerate_outputs();
        panels.clear();
        for (const auto& o : outputs_) {
            const bool is_new = std::find(before.begin(), before.end(), o.gdi_name) == before.end();
            // After Extend a screen of yours may switch on too: only the drivers' monitors then.
            if (!o.glasses && (win::is_virtual_panel(o) || (is_new && !extended))) panels.push_back(&o);
        }
        if (static_cast<int>(panels.size()) >= n) {
            // The Virtual Display Driver reloads its adapter after a count change: a monitor
            // seen mid-reload (the one it had before) vanishes a moment later. Keep them only
            // if they are still there after a pause.
            std::vector<std::wstring> seen;
            for (const auto* p : panels) seen.push_back(p->gdi_name);
            Sleep(1500);
            outputs_ = win::enumerate_outputs();
            int still = 0;
            for (const auto& name : seen) still += find_output(name) != nullptr ? 1 : 0;
            if (still == static_cast<int>(seen.size())) {
                panels.clear();
                for (const auto& name : seen) panels.push_back(find_output(name));
                break;
            }
            logln("virtual monitors: %d of %zu gone again (driver reloading), waiting", static_cast<int>(seen.size()) - still,
                  seen.size());
            panels.clear();
        }
    }
    if (static_cast<int>(panels.size()) < n) {
        logln("virtual monitors: %zu of %d appeared (are they off in Settings > Display?)", panels.size(), n);
        logln("virtual monitors: the driver says: %s", backend_->state().substr(0, 600).c_str());
        logln("display devices Windows knows:");
        for (const auto& line : win::describe_display_devices()) logln("  %s", line.c_str());
        if (panels.empty()) return false;
    }
    std::sort(panels.begin(), panels.end(),
              [](const win::OutputInfo* a, const win::OutputInfo* b) { return a->gdi_name < b->gdi_name; });
    panels.resize(std::min<size_t>(panels.size(), static_cast<size_t>(n)));
    panel_names_.clear();
    if (!laptop_gdi_.empty()) panel_names_.push_back(laptop_gdi_);
    for (const auto* p : panels) panel_names_.push_back(p->gdi_name);

    // Spare monitors of the driver's pool are not panels: off the desktop.
    if (const int spare = win::detach_unused_virtual(outputs_, panel_names_); spare > 0) {
        logln("virtual monitors: %d spare one(s) kept off the desktop", spare);
        Sleep(500);
        outputs_ = win::enumerate_outputs();
        panels.clear();
        for (const auto& name : panel_names_) {
            if (const auto* o = find_output(name); o != nullptr && name != laptop_gdi_) panels.push_back(o);
        }
    }
    const win::OutputInfo* glasses = glasses_gdi_.empty() ? nullptr : find_output(glasses_gdi_);
    const win::OutputInfo* laptop = laptop_gdi_.empty() ? nullptr : find_output(laptop_gdi_);
    arranged_ns_ = steady_ns();
    bool arranged = arrangement_.arrange(panels, opt_.layout, opt_.windowed ? nullptr : glasses, outputs_, opt_.vd_width,
                                         opt_.vd_height, &error, laptop);
    if (!arranged && parsec() && (opt_.vd_width != 1920 || opt_.vd_height != 1080)) {
        logln("virtual monitors: %dx%d not offered by the driver, 1920x1080 instead", opt_.vd_width, opt_.vd_height);
        opt_.vd_width = 1920;
        opt_.vd_height = 1080;
        arranged = arrangement_.arrange(panels, opt_.layout, opt_.windowed ? nullptr : glasses, outputs_, opt_.vd_width,
                                        opt_.vd_height, &error, laptop);
    }
    if (!arranged) {
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
        const int done = win::copy_wallpaper(own->desktop, fresh);
        logln("wallpaper: copied to %d of %zu monitor(s)", done, fresh.size());
        // Monitors just attached may not take it yet (black, half drawn): again in a moment.
        if (done < static_cast<int>(fresh.size())) {
            wallpaper_from_ = own->desktop;
            wallpaper_to_ = fresh;
            wallpaper_tries_ = 3;
            wallpaper_next_ns_ = steady_ns() + 2000000000ull;
        }
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
    restore_glasses_mode();
    logln("displays: arrangement restored");
}

// Once a second: a floating window that left its monitor (opened from the taskbar on another
// screen, dragged) goes back and fills it again; else the cursor climbs into an empty monitor.
void Server::keep_floating_windows() {
    for (size_t i = 0; i < floating_monitors_.size() && i < floating_hwnds_.size(); ++i) {
        if (floating_monitors_[i].empty()) continue;
        const auto* mon = find_output(floating_monitors_[i]);
        const HWND hwnd = floating_hwnds_[i];
        RECT w{};
        if (mon == nullptr || !IsWindow(hwnd) || !GetWindowRect(hwnd, &w)) continue;
        const POINT c{(w.left + w.right) / 2, (w.top + w.bottom) / 2};
        const RECT& m = mon->desktop;
        if (!IsIconic(hwnd) && IsZoomed(hwnd) && c.x >= m.left && c.x < m.right && c.y >= m.top && c.y < m.bottom) continue;
        ShowWindow(hwnd, SW_RESTORE);
        SetWindowPos(hwnd, nullptr, m.left + 40, m.top + 40, (m.right - m.left) - 80, (m.bottom - m.top) - 80,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        ShowWindow(hwnd, SW_MAXIMIZE);
        logln("window: floating window %zu back on its monitor", i + 1);
    }
}

void Server::restore_glasses_mode() {
    if (!mode_changed_ || glasses_gdi_.empty()) return;
    const int want = opt_.refresh120 ? 120 : 60;
    const auto now = win::current_mode(glasses_gdi_);
    if (now && now->hz == want) return;
    if (const auto m = win::set_native_mode(glasses_gdi_, want)) {
        Sleep(300);
        outputs_ = win::enumerate_outputs();
        if (const auto cur = win::current_mode(glasses_gdi_)) refresh_hz_ = cur->hz;
        logln("glasses mode: back to %dx%d @%d Hz (a display change had reset it)", m->width, m->height, refresh_hz_);
    }
}

// The framed panel, as macOS: the one under the cursor (where you are working), every gaze
// tick. With the cursor on no panel, the panel that gets the keyboard (the foreground window:
// its own floating panel, or the panel of the monitor it is on), every 8th tick (~4 Hz).
// That tick also brings back a foreground window that ended up on the glasses' surface
// (invisible there: it shows the render), onto the panel under the cursor, else panel 1.
void Server::update_key_panel() {
    int panel = -1;
    const bool several = !opt_.glasses_pinned && monitor_panels_ + static_cast<int>(floating_hwnds_.size()) > 1;
    POINT cur{};
    const bool have_cursor = GetCursorPos(&cur) != FALSE;
    int cursor_panel = -1;
    for (size_t i = 0; have_cursor && i < panel_rects_.size(); ++i) {
        if (PtInRect(&panel_rects_[i], cur)) cursor_panel = static_cast<int>(i);
    }
    if (several) panel = cursor_panel;
    const bool tick = ++key_panel_tick_ % 8 == 0;
    HWND fg = tick ? GetForegroundWindow() : nullptr;
    if (fg == hwnd_) fg = nullptr;
    if (fg != nullptr && !opt_.windowed && !IsRectEmpty(&glasses_rect_) && !panel_rects_.empty()) {
        RECT r{};
        bool mirrored = false;  // the glasses sharing a panel's area: nothing is lost there
        for (const RECT& pr : panel_rects_) {
            RECT both{};
            if (IntersectRect(&both, &pr, &glasses_rect_)) mirrored = true;
        }
        if (!mirrored && GetWindowRect(fg, &r)) {
            const POINT centre{(r.left + r.right) / 2, (r.top + r.bottom) / 2};
            const RECT& to = panel_rects_[static_cast<size_t>(std::max(cursor_panel, 0))];
            if (PtInRect(&glasses_rect_, centre) && !IsRectEmpty(&to)) {
                const int moved = win::gather_windows(glasses_rect_, to, hwnd_);
                logln("windows: %d window(s) off the glasses display to panel %d", moved, std::max(cursor_panel, 0) + 1);
            }
        }
    }
    if (several && panel < 0) {
        if (!tick) return;
        if (fg != nullptr) {
            const auto it = std::find(floating_hwnds_.begin(), floating_hwnds_.end(), fg);
            if (it != floating_hwnds_.end()) {
                panel = monitor_panels_ + static_cast<int>(it - floating_hwnds_.begin());
            } else if (HMONITOR mon = MonitorFromWindow(fg, MONITOR_DEFAULTTONULL); mon != nullptr) {
                MONITORINFOEXW mi{};
                mi.cbSize = sizeof(mi);
                if (GetMonitorInfoW(mon, &mi)) {
                    for (size_t i = 0; i < panel_names_.size(); ++i) {
                        if (panel_names_[i] == mi.szDevice) panel = static_cast<int>(i);
                    }
                }
            }
        }
    }
    if (renderer_.options.key_panel != panel) {
        renderer_.options.key_panel = panel;
        redraw_ = true;
    }
}

// The cursor follows the panel you look at, keeping its relative position, once the mouse
// has rested for a moment (the mouse wins). On the glasses surface it would be a second,
// head-locked cursor: send it to the panel in view.
void Server::follow_gaze(uint64_t now) {
    update_key_panel();
    // Captures of panels out of the glasses' view slow down (their frames would wait anyway).
    const uint32_t visible = renderer_.visible_panel_mask();
    for (size_t i = 0; i < captures_.size(); ++i) captures_[i]->set_in_view((visible >> i & 1u) != 0);
    if (opt_.windowed || panel_rects_.empty()) return;
    // Mirrored glasses share a panel's area: there the cursor belongs where it is.
    for (const RECT& r : panel_rects_) {
        RECT both{};
        if (IntersectRect(&both, &r, &glasses_rect_)) return;
    }
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
    if (!virtual_) return;
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

void Server::update_cursor() {
    CURSORINFO ci{sizeof(ci)};
    const bool showing = GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING) != 0;
    const POINT c = ci.ptScreenPos;
    const int n = std::min(monitor_panels_, static_cast<int>(panel_rects_.size()));
    for (int i = 0; i < n; ++i) {
        const RECT& r = panel_rects_[static_cast<size_t>(i)];
        renderer_.set_cursor(i, showing && PtInRect(&r, c) != FALSE, c.x - r.left, c.y - r.top);
    }
}

void Server::adjust_fov(float delta) {
    auto& ro = renderer_.options;
    if (opt_.real_size && !opt_.glasses_pinned) {  // the optics' field of view, within its range
        ro.fov_y_deg = std::clamp(ro.fov_y_deg + delta * 0.2f, kOpticalVFovMin, kOpticalVFovMax);
        save_optical_fov(ro.fov_y_deg);
    } else {
        ro.fov_y_deg = std::fmax(10.0f, std::fmin(45.0f, ro.fov_y_deg + delta));
        save_fov(ro.fov_y_deg);
    }
    redraw_ = true;
    {
        std::lock_guard<std::mutex> lock(shared_mutex_);
        status_.fov_y_deg = ro.fov_y_deg;
    }
    logln("fov: %.1f deg (saved)", ro.fov_y_deg);
}

void Server::toggle_optics() {
    if (!opt_.real_size || opt_.glasses_pinned) {
        flash(L"Ajuste de la \u00f3ptica: solo en el espacio a tama\u00f1o real");
        return;
    }
    if (optics_) {
        finish_optics(true);
        return;
    }
    core::FovTuner t;
    t.start(renderer_.options.fov_y_deg, kOpticalVFovMin, kOpticalVFovMax);
    optics_ = t;
    logln("optics: assistant started at %.2f deg", t.fov);
    renderer_.set_overlay_text(optics_text());
    redraw_ = true;
}

std::wstring Server::optics_text() const {
    if (!optics_) return L"";
    wchar_t buf[320];
    std::swprintf(buf, 320,
                  L"Ajuste de la \u00f3ptica \u00b7 %.2f\u00b0\nGira la cabeza despacio mirando el borde de una pantalla\n"
                  L"Se mueve CON la cabeza: Ctrl+Alt+Shift+-   \u00b7   EN CONTRA: Ctrl+Alt+Shift++\nQuieta en su sitio: Ctrl+Alt+L",
                  optics_->fov);
    return buf;
}

void Server::optics_answer(bool against_head) {
    const float fov = optics_->answer(against_head);
    renderer_.options.fov_y_deg = fov;
    save_optical_fov(fov);
    redraw_ = true;
    if (optics_->done) {
        finish_optics(false);
    } else {
        renderer_.set_overlay_text(optics_text());
    }
}

void Server::finish_optics(bool early) {
    const float fov = optics_->fov;
    optics_.reset();
    renderer_.options.fov_y_deg = fov;
    save_optical_fov(fov);
    {
        std::lock_guard<std::mutex> lock(shared_mutex_);
        status_.fov_y_deg = fov;
    }
    renderer_.set_overlay_text(L"");
    redraw_ = true;
    logln("optics: %s at %.2f deg (saved)", early ? "set by the wearer" : "found", fov);
    wchar_t buf[64];
    std::swprintf(buf, 64, L"\u00d3ptica ajustada: %.2f\u00b0", fov);
    flash(buf, 3.0);
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
    case kFovDown:
        if (optics_) optics_answer(false);
        else adjust_fov(-0.5f);
        break;
    case kFovUp:
        if (optics_) optics_answer(true);
        else adjust_fov(0.5f);
        break;
    case kOptics: toggle_optics(); break;
    case kLeft: pose_->nudge(step, 0); break;
    case kRight: pose_->nudge(-step, 0); break;
    case kUp: pose_->nudge(0, step); break;
    case kDown: pose_->nudge(0, -step); break;
    case kJump: jump_cursor_to_gaze(); break;
    case kFocus: toggle_focus(false); break;
    case kCinema: toggle_focus(true); break;
    case kZoomIn:
    case kZoomInPad: zoom_step(1); break;
    case kZoomOut:
    case kZoomOutPad: zoom_step(-1); break;
    case kZoomReset: set_zoom(1.0f, L"Zoom 100 %"); break;
    case kOverview: toggle_overview(); break;
    case kVehicle:  // vehicle mode on / off at once (a quarter of turns followed), as macOS
        if (opt_.glasses_pinned) {
            flash(L"Modo veh\u00edculo: no disponible\ncon las gafas en modo anclado");
            break;
        }
        opt_.vehicle_mode = !opt_.vehicle_mode;
        renderer_.options.vehicle_mode = opt_.vehicle_mode;
        redraw_ = true;
        logln("vehicle mode: %s", opt_.vehicle_mode ? "on (a quarter of turns followed)" : "off");
        flash(opt_.vehicle_mode ? L"Modo veh\u00edculo activado\nLas pantallas acompa\u00f1an los giros"
                                : L"Modo veh\u00edculo desactivado");
        break;
    case kCodeMode:
        overview_ = false;
        code_mode_ = !code_mode_;
        set_zoom(code_mode_ ? 1.7f : 1.0f, code_mode_ ? L"Modo código activado" : L"Modo código desactivado");
        break;
    default: break;
    }
}

void Server::run() {
    // The frame loop above the app's other threads, as the IMU threads (late latching wakes it
    // a millisecond or two before the vsync: it must get the CPU then).
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
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
    if (frame_timer_ != nullptr) {
        CloseHandle(frame_timer_);
        frame_timer_ = nullptr;
    }
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
    // Its monitors only exist while Nimbo runs (they slowed the PC otherwise).
    const bool parsec = backend_ && std::string(backend_->name()) == "Parsec VDD";
    backend_.reset();  // Parsec VDD: its displays end with the keepalive
    if (!parsec && win::virtual_display_driver_installed() && win::virtual_driver_off()) {
        logln("virtual monitors: driver switched off");
    }
    if (saved_topology_) {
        Sleep(500);  // the monitors gone first
        logln("displays: Windows back to %s", win::set_topology(*saved_topology_) ? "your Win+P mode" : "? (set it with Win+P)");
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
    // Monitors added or removed (Parsec VDD): rebuilt in place, before the rest is applied.
    bool layout_changed = o->displays != opt_.displays || o->layout.size() != opt_.layout.size();
    for (size_t i = 0; !layout_changed && i < o->layout.size(); ++i) {
        layout_changed = o->layout[i].col != opt_.layout[i].col || o->layout[i].row != opt_.layout[i].row;
    }
    if (layout_changed) reconfigure_displays(*o);
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
    const bool normal_mode = o->real_size && !o->glasses_pinned;
    if (normal_mode) {
        ro.fov_y_deg = load_optical_fov();
    } else if (o->fov_from_cli) {
        ro.fov_y_deg = o->fov_y_deg;
    } else if (opt_.real_size && !opt_.glasses_pinned) {  // leaving the space at real size: the pinned mode's own
        ro.fov_y_deg = load_fov().value_or(o->fov_y_deg);
    }
    ro.fixed_prediction_ms = normal_mode ? kPredictionMs : 0.0f;
    ro.supersample = normal_mode ? 2 : 1;
    ro.side_by_side = o->side_by_side.value_or(auto_side_by_side());
    ro.k1 = o->k1;
    ro.k2 = o->k2;
    ro.dim_others = o->dim_others;
    ro.tracking_axes = static_cast<core::TrackingAxes>(o->tracking_axes);
    ro.background = o->background;
    ro.background_brightness = o->background_brightness;
    ro.background_image = o->background_image;
    opt_.background = o->background;
    opt_.background_brightness = o->background_brightness;
    opt_.background_image = o->background_image;
    if (o->glasses_mode >= 0 && o->glasses_mode != opt_.glasses_mode) pose_->set_glasses_mode(o->glasses_mode);
    if (o->brightness > 0 && o->brightness != opt_.brightness) {
        pose_->set_glasses_brightness(o->brightness);
        logln("glasses: brightness %d sent", o->brightness);
    }
    opt_.glasses_mode = o->glasses_mode;
    opt_.brightness = o->brightness;
    opt_.recenter_glasses = o->recenter_glasses;
    opt_.glasses_pinned = o->glasses_pinned;
    opt_.tilt = o->tilt;
    opt_.real_size = o->real_size;
    apply_pinned(*o);  // after the width above: it overrides it
    opt_.gaze_cursor = o->gaze_cursor;
    opt_.gather_windows = o->gather_windows;
    opt_.grow_by_gaze = o->grow_by_gaze;
    opt_.vehicle_mode = o->vehicle_mode;
    opt_.flat_wall = o->flat_wall;
    renderer_.options.flat_wall = o->flat_wall;
    renderer_.options.vehicle_mode = o->vehicle_mode && !o->glasses_pinned;
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
    s.zoom = c.zoom > 0 ? c.zoom : 1.0f;
    s.grow_by_gaze = c.grow_by_gaze;
    s.vehicle_mode = c.vehicle_mode;
    s.flat_wall = c.flat_wall;
    s.background = std::clamp(c.background, 0, 5);
    s.background_brightness = std::clamp(c.background_brightness, 0.0f, 1.0f);
    s.background_image.assign(c.background_image, strnlen(c.background_image, sizeof(c.background_image)));
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
    c.zoom = s.zoom;
    c.grow_by_gaze = s.grow_by_gaze;
    c.vehicle_mode = s.vehicle_mode;
    c.flat_wall = s.flat_wall;
    c.background = s.background;
    c.background_brightness = s.background_brightness;
    copy_str(c.background_image, sizeof(c.background_image), s.background_image);
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
    o.gap_deg = 0.0f;     // flat wall: edge to edge
    o.distance_m = 1.2f;
    o.gather_windows = true;
    o.gaze_cursor = true;
    o.dim_others = 1.0f;
    o.side_by_side = -1;
    copy_str(o.calibration_path, sizeof(o.calibration_path), "calibration.json");
    o.dead_zone_deg = 1.0f;
    copy_str(o.source, sizeof(o.source), "auto");
    o.refresh120 = true;
    o.hotkeys = true;
    o.tracking_axes = 1;
    o.glasses_mode = -1;
    o.tilt = 0.0f;
    o.flat_wall = true;
    o.zoom = 1.0f;
    o.grow_by_gaze = true;
    o.background_brightness = 0.6f;
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

void nimbo_show_message(nimbo_server* s, const char* text) {
    if (s == nullptr || text == nullptr) return;
    const int n = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (n <= 1) return;
    auto* w = new std::wstring(static_cast<size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, w->data(), n);
    std::lock_guard<std::mutex> lock(s->mutex);
    if (s->server) {
        s->server->post(WM_APP + 5, 0, reinterpret_cast<LPARAM>(w));  // freed by the server thread
    } else {
        delete w;
    }
}

void nimbo_toggle_focus(nimbo_server* s, bool cinema) {
    std::lock_guard<std::mutex> lock(s->mutex);
    if (s->server) s->server->post(WM_APP + 3, cinema ? 1 : 0);
}

bool nimbo_dump_recent_imu(nimbo_server* s, const char* path_utf8) {
    if (s == nullptr || path_utf8 == nullptr) return false;
    std::lock_guard<std::mutex> lock(s->mutex);
    return s->server && s->server->dump_recent_imu(path_utf8);
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

// Instant (the app asks from its UI thread): no waiting for a reloading driver's pipe.
bool nimbo_displays_hot(void) { return win::parsec_vdd_available(); }

bool nimbo_virtual_driver_available(void) {
    return win::parsec_vdd_available() || win::find_virtual_display_backend(false) != nullptr;
}

bool nimbo_restore_displays(void) {
    DpiScope dpi;
    const bool restored = restore_from_backup();
    // A run that ended abruptly left the Virtual Display Driver on: its monitors (the pool of 6)
    // slow the PC down while Nimbo is not running. Off again (Nimbo switches it on when it starts).
    bool driver = false;
    if (!g_server_running.load() && win::virtual_display_driver_installed() &&
        win::VirtualDisplayDriver::live_monitors() > 0) {
        driver = win::virtual_driver_off();
    }
    return restored || driver;
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
    // The wall (flatWall, not in the glasses' pinned mode) is the block of cells, edge to edge:
    // a hand placement is kept for when it is off, as on macOS.
    const bool wall = s.flat_wall && s.glasses_mode != 1;
    const bool placed = !wall && !s.panels.empty() && s.panels.size() == cells.size();
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
    o.side_by_side = -1;  // the image in both halves exactly when the glasses are in their 3D mode
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
    o.dead_zone_deg = s.anchor_at_rest ? -2.0f : -1.0f;  // see rayneo_config: -1 - hold (1 deg; 2 up / down)
    o.tilt = s.tilt;
    o.laptop_screen = s.mac_screen;
    o.zoom = s.zoom;
    o.grow_by_gaze = s.grow_by_gaze;
    o.vehicle_mode = s.vehicle_mode;
    o.flat_wall = s.flat_wall;
    o.background = s.background;
    o.background_brightness = s.background_brightness;
    copy_str(o.background_image, sizeof(o.background_image), s.background_image);
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

// The arrangement editing of rayneo_layouts.h (core/presets.h), shared with macOS.
static_assert(NIMBO_MAX_PANELS == RAYNEO_MAX_PRESET_CELLS);

bool nimbo_next_free_cell(const int* cols, const int* rows, int count, int* col, int* row) {
    return rayneo_next_free_cell(cols, rows, count, col, row);
}

bool nimbo_move_cell(int* cols, int* rows, int count, int index, int col, int row) {
    return rayneo_move_cell(cols, rows, count, index, col, row);
}

bool nimbo_pose_overlaps(const float* yaw, const float* pitch, const float* scale, int count, int index, float size_deg,
                         float aspect) {
    return rayneo_pose_overlaps(yaw, pitch, scale, count, index, size_deg, aspect);
}

void nimbo_pose_beside(const float* yaw, const float* pitch, const float* scale, int count, float size_deg, float aspect,
                       float gap_deg, float* out_yaw, float* out_pitch) {
    rayneo_pose_beside(yaw, pitch, scale, count, size_deg, aspect, gap_deg, out_yaw, out_pitch);
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
