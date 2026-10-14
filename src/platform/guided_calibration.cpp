#include "rayneo/platform/guided_calibration.h"

#include <chrono>
#include <thread>

#include "rayneo/core/gt_protocol.h"
#include "rayneo/core/imu_pipeline.h"
#include "rayneo/hal/hid_transport.h"
#include "rayneo/hal/imu_stream_reader.h"

namespace rayneo::platform {
namespace {

namespace core = rayneo::core;
namespace gt = rayneo::core::gt;
namespace hal = rayneo::hal;
using Clock = std::chrono::steady_clock;

class Session {
public:
    Session(const std::atomic<bool>& cancel, const GuidedCalibration::StatusFn& fn) : cancel_(cancel), fn_(fn) {
        // Calibration needs the raw head orientation; no stabilizer dead zone.
        core::ImuPipelineConfig cfg;
        cfg.stabilize = false;
        pipe_ = core::ImuPipeline(cfg);
    }
    ~Session() {
        reader_.stop();
        if (hal_ready_) hal::hal_shutdown();
    }

    bool open(std::string* err) {
        if (!hal::hal_init()) {
            *err = "hid_init failed";
            return false;
        }
        hal_ready_ = true;
        return reader_.open_device(err) && reader_.start();
    }

    CalibStatus status;

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
        return !cancel_.load() && !reader_.device_lost();
    }

    bool wait_bias(float timeout_s) {
        const auto t0 = Clock::now();
        while (pump()) {
            if (last_.tracking) return true;
            if (std::chrono::duration<double>(Clock::now() - t0).count() > timeout_s) return false;
            report("waiting for rest");
        }
        return false;
    }

    bool capture_gravity(core::Vec3* out, float secs, float timeout_s) {
        gravity_ = true;
        reset_gravity();
        bool moved = false;
        const auto deadline = Clock::now() + std::chrono::duration<double>(timeout_s);
        while (pump() && Clock::now() < deadline) {
            if (hold_.moving()) moved = true;
            if (moved && grav_s_ >= secs && grav_n_ > 0) {
                gravity_ = false;
                *out = grav_sum_ * (1.0f / static_cast<float>(grav_n_));
                return true;
            }
            report(moved ? "hold" : "put the glasses on");
        }
        gravity_ = false;
        return false;
    }

    bool capture_motion(core::Vec3* out, float min_deg, float timeout_s) {
        integ_ = {};
        integrating_ = true;
        bool moved = false;
        const auto deadline = Clock::now() + std::chrono::duration<double>(timeout_s);
        while (pump() && Clock::now() < deadline) {
            if (hold_.moving()) moved = true;
            if (moved && hold_.holding() && integ_.norm() >= min_deg) {
                integrating_ = false;
                *out = integ_;
                return true;
            }
            report(!moved ? "move" : (integ_.norm() < min_deg ? "further" : "hold"));
        }
        integrating_ = false;
        return false;
    }

    bool settle(float timeout_s) {
        core::Vec3 ignored;
        return capture_motion(&ignored, 0.0f, timeout_s);
    }

    void report(const char* hint, bool force = false) {
        const auto now = Clock::now();
        if (!force && now - last_report_ < std::chrono::milliseconds(100)) return;
        last_report_ = now;
        status.hint = hint;
        status.rate_degs = hold_.rate_degs();
        status.rotation_deg = integ_.norm();
        status.bias_ok = last_.tracking;
        if (fn_) fn_(status);
    }

private:
    void on_sample() {
        hold_.update(last_.gyro_corrected_degs, dt_);
        if (integrating_) integ_ = integ_ + last_.gyro_corrected_degs * dt_;
        if (!gravity_) return;
        if (!hold_.holding()) {
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

    const std::atomic<bool>& cancel_;
    const GuidedCalibration::StatusFn& fn_;
    bool hal_ready_ = false;
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
    Clock::time_point last_report_{};
};

GuidedCalibrationOutcome fail(const std::string& msg) {
    GuidedCalibrationOutcome o;
    o.error = msg;
    return o;
}

}  // namespace

GuidedCalibrationOutcome GuidedCalibration::run(const std::atomic<bool>& cancel, const StatusFn& on_status) {
    Session s(cancel, on_status);
    std::string err;
    if (!s.open(&err)) return fail(err.empty() ? "glasses not found" : err);

    s.status.step = CalibStep::Bias;
    s.report("waiting for rest", true);
    if (!s.wait_bias(60.0f)) return fail("no rest detected: put the glasses on a stable surface");

    core::HeadCalibrationInput in;
    s.status.step = CalibStep::Center;
    s.report("put the glasses on", true);
    if (!s.capture_gravity(&in.gravity_center, 1.0f, 60.0f)) return fail("timed out or cancelled");

    const struct {
        CalibStep step;
        core::Vec3* out;
    } steps[] = {{CalibStep::Up, &in.rot_up},
                 {CalibStep::Down, &in.rot_down},
                 {CalibStep::Left, &in.rot_left},
                 {CalibStep::Right, &in.rot_right}};
    for (const auto& st : steps) {
        s.status.step = st.step;
        s.status.returning = false;
        s.status.captured_deg = 0.0f;
        s.report("move", true);
        // Bigger nods dilute any turn or tilt mixed into them; 15 deg is the minimum accepted.
        if (!s.capture_motion(st.out, 15.0f, 30.0f)) return fail("timed out or cancelled");
        s.status.returning = true;
        s.status.captured_deg = st.out->norm();
        s.report("move", true);
        if (!s.settle(30.0f)) return fail("timed out or cancelled");
    }
    GuidedCalibrationOutcome o;
    o.completed = true;
    o.result = core::solve_head_calibration(in);
    return o;
}

}  // namespace rayneo::platform
