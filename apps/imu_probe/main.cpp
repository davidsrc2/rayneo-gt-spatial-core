// CLI probe for the RayNeo GT.
//   imu_probe --list          enumerate 0x3941 HID interfaces
//   imu_probe --raw [secs]    bypass the engine: dump the descriptor and raw reports,
//                             validate framing and measure the device-clock rate
//   imu_probe --calibrate [out.json]
//                             guided look-around -> sensor_to_head quaternion
//   imu_probe [--calib file]  stream IMU and print the fused pose at 10 Hz
//                             (loads ./calibration.json when present)
// Ctrl+C to exit, 'stream-off' is sent on shutdown.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>

#include "rayneo/core/gt_protocol.h"
#include "rayneo/core/head_calibration.h"
#include "rayneo/core/imu_pipeline.h"
#include "rayneo/core/tick_monitor.h"
#include "rayneo/hal/hid_transport.h"
#include "rayneo/hal/imu_stream_reader.h"
#include "rayneo/platform/rayneo_engine.h"

namespace {

namespace core = rayneo::core;
namespace gt = rayneo::core::gt;
namespace hal = rayneo::hal;

constexpr const char* kDefaultCalibPath = "calibration.json";

std::atomic<bool> g_quit{false};
void on_signal(int) { g_quit.store(true); }

void hexdump(const char* label, const uint8_t* d, int n) {
    std::printf("%s len=%d:", label, n);
    for (int i = 0; i < n; ++i) std::printf("%s%02x", i % 16 == 0 ? "\n  " : " ", d[i]);
    std::printf("\n");
}

int run_list() {
    hal::hal_init();
    const auto devices = hal::enumerate(gt::kVendorId, 0);
    for (const auto& d : devices) std::printf("%s\n", hal::describe(d).c_str());
    hal::hal_shutdown();
    return devices.empty() ? 1 : 0;
}

int run_raw(double secs) {
    if (!hal::hal_init()) {
        std::fprintf(stderr, "hid_init failed\n");
        return 1;
    }
    const auto devices = hal::enumerate(gt::kVendorId, gt::kProductId);
    const hal::HidDeviceInfo* target = nullptr;
    for (const auto& d : devices) {
        if (d.usage_page == gt::kUsagePage) {
            target = &d;
            break;
        }
    }
    if (target == nullptr) {
        std::fprintf(stderr, "RayNeo GT (3941:AF50) not found\n");
        return 1;
    }
    auto t = hal::create_transport();
    if (!t->open(target->path)) {
        std::fprintf(stderr, "open failed: %s\n", t->last_error().c_str());
        return 1;
    }
    std::printf("opened %s\n", hal::describe(*target).c_str());

    uint8_t desc[4096];
    const int dn = t->report_descriptor(desc, sizeof(desc));
    if (dn > 0) {
        hexdump("report descriptor", desc, dn);
        // Short item 0x85 = Report ID. Absent -> unnumbered reports.
        bool numbered = false;
        for (int i = 0; i < dn;) {
            const uint8_t b = desc[i];
            if (b == 0xFE) {  // long item
                i += 3 + (i + 1 < dn ? desc[i + 1] : 0);
                continue;
            }
            const int sz = (b & 3) == 3 ? 4 : (b & 3);
            if ((b & 0xFC) == 0x84 && sz >= 1 && i + 1 < dn) {
                std::printf("  Report ID item: 0x%02x\n", desc[i + 1]);
                numbered = true;
            }
            i += 1 + sz;
        }
        std::printf("descriptor uses numbered reports: %s\n", numbered ? "YES" : "no");
    }

    const auto on = gt::build_command(gt::kCmdStreamOn);
    const int wn = t->write(on.data(), on.size());
    std::printf("stream-on write -> %d\n", wn);

    std::map<int, uint64_t> lengths;
    std::map<int, uint64_t> heads;  // first two bytes
    std::map<uint32_t, uint64_t> deltas;
    bool have_tick = false;
    uint32_t prev_tick = 0;
    rayneo::core::TickMonitor ticks;
    uint64_t total = 0, acks = 0, imu = 0, unknown = 0;
    int dumped = 0, unknown_dumped = 0;
    bool ack_seen = false;

    const auto t0 = std::chrono::steady_clock::now();
    const auto deadline = t0 + std::chrono::duration<double>(secs);
    uint8_t buf[65];
    while (!g_quit.load() && std::chrono::steady_clock::now() < deadline) {
        const int n = t->read(buf, sizeof(buf), 100);
        if (n < 0) {
            std::fprintf(stderr, "read error: %s\n", t->last_error().c_str());
            break;
        }
        if (n == 0) continue;
        ++total;
        ++lengths[n];
        ++heads[(buf[0] << 8) | (n > 1 ? buf[1] : 0)];
        const auto r = gt::decode_report(buf, static_cast<size_t>(n));
        if (r.kind == gt::ReportKind::Ack) {
            ++acks;
            if (r.ack_cmd == gt::kCmdStreamOn && !ack_seen) {
                ack_seen = true;
                const double ms =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                std::printf("ACK 99 c8 for stream-on after %.1f ms (report #%llu)\n", ms,
                            static_cast<unsigned long long>(total));
                hexdump("ack", buf, n);
            }
        } else if (r.kind == gt::ReportKind::Imu) {
            ++imu;
            ticks.update(r.imu.tick_100us);
            if (have_tick) ++deltas[r.imu.tick_100us - prev_tick];
            prev_tick = r.imu.tick_100us;
            have_tick = true;
            if (dumped < 4) {
                char label[48];
                std::snprintf(label, sizeof(label), "imu #%d tick=%u", dumped, r.imu.tick_100us);
                hexdump(label, buf, n);
                ++dumped;
            }
        } else {
            ++unknown;
            if (unknown_dumped < 8) {
                hexdump("UNKNOWN", buf, n);
                ++unknown_dumped;
            }
        }
    }

    const auto off = gt::build_command(gt::kCmdStreamOff);
    t->write(off.data(), off.size());
    const double wall =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    std::printf("\n--- %.2f s wall ---\n", wall);
    std::printf("reports %llu (imu %llu, ack %llu, unknown %llu), host rate %.1f Hz\n",
                static_cast<unsigned long long>(total), static_cast<unsigned long long>(imu),
                static_cast<unsigned long long>(acks), static_cast<unsigned long long>(unknown),
                static_cast<double>(total) / wall);
    for (const auto& [len, c] : lengths) std::printf("  length %d: %llu\n", len, static_cast<unsigned long long>(c));
    for (const auto& [h, c] : heads)
        std::printf("  head %02x %02x: %llu\n", h >> 8, h & 0xff, static_cast<unsigned long long>(c));
    for (const auto& [d, c] : deltas)
        std::printf("  tick delta %u: %llu\n", d, static_cast<unsigned long long>(c));
    std::printf("device-clock rate %.2f Hz, missed %llu, irregular %llu, max delta %u ticks\n", ticks.rate_hz(),
                static_cast<unsigned long long>(ticks.missed()), static_cast<unsigned long long>(ticks.irregular()),
                ticks.max_delta());
    std::printf("stream-on ACK: %s\n", ack_seen ? "yes" : "NO");

    t->close();
    hal::hal_shutdown();
    return ack_seen && unknown == 0 ? 0 : 2;
}

// ---------------------------------------------------------------------------
// Guided sensor -> head calibration
// ---------------------------------------------------------------------------

class CalibrationSession {
public:
    bool open(std::string* err) {
        if (!hal::hal_init()) {
            *err = "hid_init failed";
            return false;
        }
        return reader_.open_device(err) && reader_.start();
    }
    ~CalibrationSession() {
        reader_.stop();
        hal::hal_shutdown();
    }

    // Drains the ring; false when the device is gone or the user quit.
    bool pump() {
        hal::RawReport rr;
        bool any = false;
        while (reader_.pop(rr)) {
            const auto p = pipe_.process_report(rr.data, rr.length);
            if (!p) continue;
            dt_ = have_tick_ ? static_cast<float>(gt::tick_delta_seconds(last_.tick_100us, p->tick_100us)) : 0.0f;
            have_tick_ = true;
            last_ = *p;
            on_sample();
            any = true;
        }
        if (!any) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        return !g_quit.load() && !reader_.device_lost();
    }

    // Bias calibration needs true rest: glasses on the table, not on the head.
    bool wait_bias(float timeout_s) {
        const auto t0 = std::chrono::steady_clock::now();
        while (pump()) {
            if (last_.tracking) return true;
            const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            if (el > timeout_s) return false;
            status("waiting for rest");
        }
        return false;
    }

    // After the head has moved (putting the glasses on), averages accel over `secs`
    // of continuous hold.
    bool capture_gravity(core::Vec3* out, float secs, float timeout_s) {
        gravity_ = true;
        reset_gravity();
        bool moved = false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(timeout_s);
        while (pump() && std::chrono::steady_clock::now() < deadline) {
            if (hold_.moving()) moved = true;
            if (moved && grav_s_ >= secs && grav_n_ > 0) {
                gravity_ = false;
                *out = grav_sum_ * (1.0f / static_cast<float>(grav_n_));
                return true;
            }
            status(moved ? "hold still" : "put the glasses on");
        }
        gravity_ = false;
        return false;
    }

    // Integrates bias-corrected gyro from now until the head has moved and is held again
    // with at least `min_deg` of rotation. Returns the rotation vector (deg, sensor frame).
    bool capture_motion(core::Vec3* out, float min_deg, float timeout_s) {
        integ_ = {};
        integrating_ = true;
        bool moved = false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(timeout_s);
        while (pump() && std::chrono::steady_clock::now() < deadline) {
            if (hold_.moving()) moved = true;
            if (moved && hold_.holding() && integ_.norm() >= min_deg) {
                integrating_ = false;
                *out = integ_;
                return true;
            }
            status(!moved ? "move" : (integ_.norm() < min_deg ? "further" : "hold"));
        }
        integrating_ = false;
        return false;
    }

    // Waits until the head has moved back and is held again (no measurement).
    bool settle(float timeout_s) {
        core::Vec3 ignored;
        return capture_motion(&ignored, 0.0f, timeout_s);
    }

    void end_status() {
        std::printf("\r%60s\r", "");
        std::fflush(stdout);
    }

private:
    void on_sample() {
        hold_.update(last_.gyro_corrected_degs, dt_);
        if (integrating_) integ_ = integ_ + last_.gyro_corrected_degs * dt_;
        if (!gravity_) return;
        if (!hold_.holding()) {  // contiguous hold window only
            reset_gravity();
            return;
        }
        grav_sum_ = grav_sum_ + last_.accel_mps2;
        ++grav_n_;
        grav_s_ += dt_;
    }

    void reset_gravity() {
        grav_sum_ = {};
        grav_n_ = 0;
        grav_s_ = 0.0f;
    }

    // Live feedback, ~10 Hz.
    void status(const char* what) {
        const auto now = std::chrono::steady_clock::now();
        if (now - last_status_ < std::chrono::milliseconds(100)) return;
        last_status_ = now;
        const char* bias = last_.tracking ? "ok" : (pipe_.bias_estimator().state() == core::GyroBiasEstimator::State::Warmup
                                                        ? "warmup"
                                                        : "calibrating");
        std::printf("\r  [%-18s] rate %5.1f deg/s  rotation %5.1f deg  bias %-11s", what, hold_.rate_degs(),
                    integ_.norm(), bias);
        std::fflush(stdout);
    }

    hal::ImuStreamReader reader_;
    core::ImuPipeline pipe_;
    core::Pose last_;
    core::HoldDetector hold_;
    float dt_ = 0.0f;
    bool have_tick_ = false;
    bool integrating_ = false;
    core::Vec3 integ_;
    bool gravity_ = false;
    core::Vec3 grav_sum_;
    int grav_n_ = 0;
    float grav_s_ = 0.0f;
    std::chrono::steady_clock::time_point last_status_{};
};

int run_calibrate(const std::string& out_path) {
    CalibrationSession s;
    std::string err;
    if (!s.open(&err)) {
        std::fprintf(stderr, "start failed: %s\n", err.c_str());
        return 1;
    }

    std::printf("[0/5] Leave the glasses on the TABLE, untouched, until 'bias ok' (~6 s)\n");
    std::fflush(stdout);
    if (!s.wait_bias(60.0f)) {
        s.end_status();
        std::fprintf(stderr, "no rest detected in 60 s: put the glasses on a stable surface\n");
        return 1;
    }
    s.end_status();
    std::printf("      bias ok\n");

    core::HeadCalibrationInput in;
    std::printf("[1/5] Put the glasses on, look straight ahead with the head level and hold\n");
    std::fflush(stdout);
    if (!s.capture_gravity(&in.gravity_center, 1.0f, 60.0f)) {
        s.end_status();
        std::fprintf(stderr, "timed out or interrupted\n");
        return 1;
    }
    s.end_status();
    std::printf("      ok (gravity %.2f %.2f %.2f)\n", in.gravity_center.x, in.gravity_center.y, in.gravity_center.z);

    struct Step {
        const char* prompt;
        core::Vec3* out;
    };
    // Bigger nods dilute any turn or tilt mixed into them; 15 deg is the minimum accepted.
    const Step steps[] = {{"look UP (~30 deg) and hold", &in.rot_up},
                          {"look DOWN (~30 deg) and hold", &in.rot_down},
                          {"turn LEFT (~30 deg) and hold", &in.rot_left},
                          {"turn RIGHT (~30 deg) and hold", &in.rot_right}};
    int i = 2;
    for (const Step& st : steps) {
        std::printf("[%d/5] %s\n", i++, st.prompt);
        std::fflush(stdout);
        if (!s.capture_motion(st.out, 15.0f, 30.0f)) {
            s.end_status();
            std::fprintf(stderr, "timed out or interrupted\n");
            return 1;
        }
        s.end_status();
        std::printf("      %.1f deg. Back to CENTER and hold\n", st.out->norm());
        std::fflush(stdout);
        if (!s.settle(30.0f)) {
            s.end_status();
            std::fprintf(stderr, "timed out or interrupted\n");
            return 1;
        }
        s.end_status();
        std::printf("      ok\n");
    }

    const auto r = core::solve_head_calibration(in);
    std::printf("\nyaw axis vs gravity %.1f deg, nod axis tilt %.1f deg, up/down nod mismatch %.1f deg\n",
                r.yaw_axis_error_deg, r.pitch_axis_tilt_deg, r.nod_mismatch_deg);
    if (!r.ok) {
        std::fprintf(stderr, "calibration rejected: %s\n", r.error.c_str());
        return 2;
    }
    std::ofstream f(out_path);
    f << core::calibration_to_json(r.sensor_to_head);
    if (!f) {
        std::fprintf(stderr, "cannot write %s\n", out_path.c_str());
        return 1;
    }
    std::printf("sensor_to_head = [%.5f %.5f %.5f %.5f] -> %s\n", r.sensor_to_head.w, r.sensor_to_head.x,
                r.sensor_to_head.y, r.sensor_to_head.z, out_path.c_str());
    return 0;
}

std::optional<core::Quat> load_calibration(const std::string& path, bool required) {
    std::ifstream f(path);
    if (!f) {
        if (required) std::fprintf(stderr, "cannot read %s\n", path.c_str());
        return std::nullopt;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    const auto q = core::calibration_from_json(ss.str());
    if (!q) std::fprintf(stderr, "invalid calibration file %s%s\n", path.c_str(), required ? "" : " (ignored)");
    return q;
}

int run_stream(const std::string& calib_path, bool calib_required) {
    const auto calib = load_calibration(calib_path, calib_required);
    if (calib_required && !calib) return 1;

    rayneo_config cfg = rayneo_default_config();
    rayneo_engine* engine = rayneo_engine_create(&cfg);
    if (calib) {
        rayneo_engine_set_sensor_to_head(engine, calib->w, calib->x, calib->y, calib->z);
        std::printf("calibration: %s sensor_to_head = [%.5f %.5f %.5f %.5f]\n", calib_path.c_str(), calib->w,
                    calib->x, calib->y, calib->z);
    } else {
        std::printf("calibration: none (identity sensor_to_head)\n");
    }
    if (rayneo_engine_start(engine) != RAYNEO_OK) {
        std::fprintf(stderr, "start failed: %s\n", rayneo_engine_last_error(engine));
        rayneo_engine_destroy(engine);
        return 1;
    }
    std::printf("streaming... (calibration needs ~5 s with the glasses at rest)\n");

    rayneo_stats prev{};
    while (!g_quit.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        rayneo_pose p;
        rayneo_stats s;
        if (!rayneo_engine_latest_pose(engine, &p) || !rayneo_engine_stats(engine, &s)) continue;

        // Windowed device-clock rate: samples per device second since the last print.
        const uint64_t dspan = s.device_span_100us - prev.device_span_100us;
        const double hz = dspan > 0 ? static_cast<double>(s.imu_samples - prev.imu_samples) / (dspan * 100e-6) : 0.0;
        prev = s;

        std::printf("\r%s yaw %+7.2f pitch %+7.2f roll %+7.2f | bias %+5.2f %+5.2f %+5.2f | %6.1f Hz"
                    " | miss %llu drop %llu  ",
                    p.tracking ? "TRACK" : "CALIB", p.yaw_deg, p.pitch_deg, p.roll_deg, p.bias_x, p.bias_y,
                    p.bias_z, hz, static_cast<unsigned long long>(s.missed_samples),
                    static_cast<unsigned long long>(s.reports_dropped));
        std::fflush(stdout);
    }
    std::printf("\n");

    rayneo_stats s;
    if (rayneo_engine_stats(engine, &s)) {
        std::printf("session: %llu imu samples, device rate %.2f Hz, missed %llu, irregular %llu, "
                    "ring drops %llu, non-imu reports %llu\n",
                    static_cast<unsigned long long>(s.imu_samples), s.device_rate_hz,
                    static_cast<unsigned long long>(s.missed_samples),
                    static_cast<unsigned long long>(s.irregular_ticks),
                    static_cast<unsigned long long>(s.reports_dropped),
                    static_cast<unsigned long long>(s.other_reports));
    }
    rayneo_engine_destroy(engine);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    const std::string_view mode = argc > 1 ? argv[1] : "";
    if (mode == "--list") return run_list();
    if (mode == "--raw") return run_raw(argc > 2 ? std::atof(argv[2]) : 5.0);
    if (mode == "--calibrate") return run_calibrate(argc > 2 ? argv[2] : kDefaultCalibPath);
    if (mode == "--calib") {
        if (argc < 3) {
            std::fprintf(stderr, "--calib needs a file\n");
            return 1;
        }
        return run_stream(argv[2], true);
    }
    if (!mode.empty()) {
        std::fprintf(stderr, "usage: imu_probe [--list | --raw [secs] | --calibrate [out.json] | --calib file]\n");
        return 1;
    }
    return run_stream(kDefaultCalibPath, false);
}
