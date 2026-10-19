// Core engine tests. No hardware, no hidapi: the core must build and pass on any host.

#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

#include "rayneo/core/frame_skip.h"
#include "rayneo/core/gt_protocol.h"
#include "rayneo/core/gyro_bias_estimator.h"
#include "rayneo/core/gyro_noise_monitor.h"
#include "rayneo/core/head_calibration.h"
#include "rayneo/core/imu_pipeline.h"
#include "rayneo/core/imu_recording.h"
#include "rayneo/core/latch_controller.h"
#include "rayneo/core/stream_watchdog.h"
#include "rayneo/core/madgwick.h"
#include "rayneo/core/panel_dimmer.h"
#include "rayneo/core/pose_stabilizer.h"
#include "rayneo/core/vqf.h"
#include "rayneo/core/bias_map.h"
#include "rayneo/core/bias_convergence.h"
#include "rayneo/core/presets.h"
#include "rayneo/core/settings.h"
#include "rayneo/core/render_math.h"
#include "rayneo/core/tick_monitor.h"

using namespace rayneo::core;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

bool near(float a, float b, float eps) { return std::fabs(a - b) <= eps; }

void put_f32(uint8_t* p, float v) { std::memcpy(p, &v, 4); }
void put_u32(uint8_t* p, uint32_t v) {
    p[0] = v & 0xff;
    p[1] = (v >> 8) & 0xff;
    p[2] = (v >> 16) & 0xff;
    p[3] = (v >> 24) & 0xff;
}

std::vector<uint8_t> make_imu_report(Vec3 a, Vec3 g, uint32_t tick, bool with_report_id = false) {
    std::vector<uint8_t> r(64, 0);
    r[0] = 0x99;
    r[1] = 0x65;
    r[2] = gt::kBoardGt;
    put_f32(&r[4], a.x);
    put_f32(&r[8], a.y);
    put_f32(&r[12], a.z);
    put_f32(&r[16], g.x);
    put_f32(&r[20], g.y);
    put_f32(&r[24], g.z);
    put_f32(&r[28], 39.4f);
    put_f32(&r[32], 20.0f);
    put_f32(&r[36], -5.0f);
    put_u32(&r[40], tick);
    put_f32(&r[52], 40.0f);
    if (with_report_id) r.insert(r.begin(), 0x00);
    return r;
}

void test_build_command() {
    const auto f = gt::build_command(gt::kCmdStreamOn);
    CHECK(f.size() == 65);
    CHECK(f[0] == 0x00 && f[1] == 0x66 && f[2] == 0x01 && f[3] == 0x00);
}

void test_build_command_value() {
    const auto f = gt::build_command(gt::kCmdSetDisplayMode, static_cast<uint8_t>(gt::DisplayMode::Pinned));
    CHECK(f[1] == 0x66 && f[2] == 0x08 && f[3] == 0x01 && f[4] == 0x00);
}

void test_decode_imu() {
    for (bool rid : {false, true}) {
        const auto r = make_imu_report({0.1f, 0.2f, 9.81f}, {1.0f, -2.0f, 0.5f}, 123456, rid);
        const auto rep = gt::decode_report(r.data(), r.size());
        CHECK(rep.kind == gt::ReportKind::Imu);
        CHECK(rep.board_id == gt::kBoardGt);
        CHECK(near(rep.imu.accel_mps2.z, 9.81f, 1e-6f));
        CHECK(near(rep.imu.gyro_degs.y, -2.0f, 1e-6f));
        CHECK(near(rep.imu.mag_ut.z, 40.0f, 1e-6f));
        CHECK(rep.imu.tick_100us == 123456);
    }
}

void test_decode_ack_and_garbage() {
    uint8_t ack[64] = {0x99, 0xC8, 0x40};
    ack[8] = gt::kCmdStreamOn;
    const auto a = gt::decode_report(ack, sizeof(ack));
    CHECK(a.kind == gt::ReportKind::Ack && a.ack_cmd == gt::kCmdStreamOn);

    uint8_t junk[64] = {0x12, 0x34};
    CHECK(gt::decode_report(junk, sizeof(junk)).kind == gt::ReportKind::Unknown);

    auto shortr = make_imu_report({}, {}, 0);
    CHECK(gt::decode_report(shortr.data(), 40).kind == gt::ReportKind::Unknown);

    auto nanr = make_imu_report({}, {}, 0);
    put_f32(&nanr[16], NAN);
    CHECK(gt::decode_report(nanr.data(), nanr.size()).kind == gt::ReportKind::Unknown);
}

void test_tick_wrap() {
    CHECK(near(static_cast<float>(gt::tick_delta_seconds(0xFFFFFFF0u, 5u)), 21 * 100e-6f, 1e-7f));
}

void test_madgwick_level_stable() {
    MadgwickFilter f;
    f.initialize_from_accel({0, 0, 9.81f});
    for (int i = 0; i < 4760; ++i) f.update_imu({0, 0, 0}, {0, 0, 9.81f}, 1.0f / 476.0f);
    const auto e = to_euler_deg(f.quaternion());
    CHECK(near(e.roll, 0, 0.01f) && near(e.pitch, 0, 0.01f) && near(e.yaw, 0, 0.01f));
}

void test_madgwick_init_from_tilt() {
    MadgwickFilter f;
    const float r = 20.0f * 0.0174533f;
    f.initialize_from_accel({0, 9.81f * std::sin(r), 9.81f * std::cos(r)});
    CHECK(near(to_euler_deg(f.quaternion()).roll, 20.0f, 0.05f));
}

void test_madgwick_yaw_integration() {
    MadgwickFilter f;
    f.initialize_from_accel({0, 0, 9.81f});
    const float rate = 90.0f * 0.0174533f;  // 90 deg/s around Z for 1 s
    for (int i = 0; i < 476; ++i) f.update_imu({0, 0, rate}, {0, 0, 9.81f}, 1.0f / 476.0f);
    CHECK(near(to_euler_deg(f.quaternion()).yaw, 90.0f, 0.5f));
}

// Constant gyro bias at rest: after calibration the pipeline must not drift.
void test_pipeline_bias_removed() {
    ImuPipeline p;
    const Vec3 bias{0.8f, -0.4f, 1.1f};
    uint32_t tick = 1000;
    Pose last;
    for (int i = 0; i < 476 * 30; ++i) {
        tick += 21;
        const auto r = make_imu_report({0, 0, 9.81f}, bias, tick);
        const auto pose = p.process_report(r.data(), r.size());
        CHECK(pose.has_value());
        last = *pose;
    }
    CHECK(last.tracking);
    CHECK(near(last.gyro_bias_degs.z, 1.1f, 0.01f));
    CHECK(near(last.euler.yaw, 0.0f, 0.2f));
}

void test_pipeline_recenter() {
    ImuPipeline p;
    uint32_t tick = 0;
    auto feed = [&](Vec3 g, int n) {
        Pose last;
        for (int i = 0; i < n; ++i) {
            tick += 21;
            const auto r = make_imu_report({0, 0, 9.81f}, g, tick);
            last = *p.process_report(r.data(), r.size());
        }
        return last;
    };
    feed({0, 0, 0}, 476 * 6);  // warmup + calibration
    const Pose turned = feed({0, 0, 45.0f}, 476);  // 45 deg yaw
    CHECK(near(turned.euler.yaw, 45.0f, 1.0f));
    p.recenter();
    const Pose after = feed({0, 0, 0}, 10);
    CHECK(near(after.euler.yaw, 0.0f, 0.05f));
}

// User recenter glides: no jump on the first frame, monotonic, settled after the glide time.
void test_pipeline_recenter_glide() {
    ImuPipeline p;
    uint32_t tick = 0;
    auto feed = [&](Vec3 g) {
        tick += 21;
        const auto r = make_imu_report({0, 0, 9.81f}, g, tick);
        return *p.process_report(r.data(), r.size());
    };
    for (int i = 0; i < 476 * 6; ++i) feed({0, 0, 0});
    Pose turned;
    for (int i = 0; i < 476; ++i) turned = feed({0, 0, 45.0f});
    p.recenter(true);
    float prev = feed({0, 0, 0}).euler.yaw;
    CHECK(near(prev, turned.euler.yaw, 1.0f));
    bool monotonic = true;
    for (int i = 0; i < 476 * 3 / 10; ++i) {  // 0.3 s
        const float y = feed({0, 0, 0}).euler.yaw;
        monotonic = monotonic && y <= prev + 1e-3f;
        prev = y;
    }
    CHECK(monotonic);
    CHECK(near(feed({0, 0, 0}).euler.yaw, 0.0f, 0.05f));
}

// Reference-unit cadence: 7 deltas of 22 ticks, then one of 21 (21.875 avg, ~457.14 Hz).
uint32_t reference_delta(int i) { return i % 8 == 7 ? 21u : 22u; }

void test_tick_monitor_reference_cadence() {
    TickMonitor m;
    uint32_t tick = 0xFFFF0000u;  // crosses the 32-bit wrap
    m.update(tick);
    for (int i = 0; i < 8 * 1000; ++i) {
        tick += reference_delta(i);
        m.update(tick);
    }
    CHECK(m.samples() == 8001);
    CHECK(m.missed() == 0 && m.irregular() == 0);
    CHECK(near(static_cast<float>(m.rate_hz()), static_cast<float>(gt::kNominalRateHz), 0.01f));
    CHECK(near(static_cast<float>(gt::kNominalRateHz), 457.14f, 0.01f));
}

void test_tick_monitor_gaps() {
    TickMonitor m;
    uint32_t tick = 1000;
    m.update(tick);
    tick += 22;
    m.update(tick);
    tick += 22 + 22 + 22 + 21;  // three samples lost on the wire
    m.update(tick);
    CHECK(m.missed() == 3 && m.irregular() == 0);
    tick += 10;  // off the sample grid
    m.update(tick);
    CHECK(m.irregular() == 1 && m.missed() == 3);
    tick += 0;  // duplicated tick
    m.update(tick);
    CHECK(m.irregular() == 2);
    CHECK(m.max_delta() == 87);
}

// ---------------------------------------------------------------------------
// Gyro bias escape path / rollback
// ---------------------------------------------------------------------------

struct Lcg {
    uint32_t s = 12345;
    float uni() {  // [-1, 1)
        s = s * 1664525u + 1013904223u;
        return static_cast<float>(s >> 8) * (2.0f / 16777216.0f) - 1.0f;
    }
};

constexpr float kRefDt = 1.0f / 457.14f;
const Vec3 kBias0{0.4f, 0.45f, 0.02f};  // reference unit at room temperature

void run_rest(GyroBiasEstimator& e, Lcg& rng, Vec3 g, float secs, float noise = 0.05f) {
    const int n = static_cast<int>(secs / kRefDt);
    for (int i = 0; i < n; ++i) {
        const Vec3 jitter{rng.uni() * noise, rng.uni() * noise, rng.uni() * noise};
        e.update(g + jitter, {0, 0, 9.81f}, kRefDt);
    }
}

bool vec_near(const Vec3& a, const Vec3& b, float eps) { return (a - b).norm() <= eps; }

GyroBiasEstimator calibrated(Lcg& rng) {
    GyroBiasEstimator e;
    run_rest(e, rng, kBias0, 8.0f);
    CHECK(e.tracking());
    CHECK(vec_near(e.bias(), kBias0, 0.02f));
    return e;
}

// A 3 s step (e.g. slow, smooth head turn) must not be learned as drift.
void test_bias_rejects_transient_step() {
    Lcg rng;
    GyroBiasEstimator e = calibrated(rng);
    run_rest(e, rng, kBias0 + Vec3{1.0f, 0, 0}, 3.0f);
    run_rest(e, rng, kBias0, 5.0f);
    CHECK(vec_near(e.bias(), kBias0, 0.02f));
    CHECK(e.escapes() == 0);
}

// Zero-mean noise step (amplitude just under the rest gate) must not move the bias.
void test_bias_rejects_noise_burst() {
    Lcg rng;
    GyroBiasEstimator e = calibrated(rng);
    run_rest(e, rng, kBias0, 6.0f, 1.2f);
    CHECK(vec_near(e.bias(), kBias0, 0.03f));
    CHECK(e.escapes() == 0);
}

// A persistent step larger than any plausible bias is never adopted.
void test_bias_rejects_out_of_clamp_step() {
    Lcg rng;
    GyroBiasEstimator e = calibrated(rng);
    run_rest(e, rng, kBias0 + Vec3{0, 0, 2.5f}, 25.0f);
    CHECK(vec_near(e.bias(), kBias0, 0.02f));
    CHECK(e.escapes() == 0);
}

// A stable thermal step outside the routine window is adopted through the escape path.
void test_bias_escape_adopts_persistent_step() {
    Lcg rng;
    GyroBiasEstimator e = calibrated(rng);
    const Vec3 stepped = kBias0 + Vec3{0.6f, 0, 0};
    run_rest(e, rng, stepped, 9.0f);
    CHECK(vec_near(e.bias(), kBias0, 0.02f));  // not yet: needs escape_persist_s
    run_rest(e, rng, stepped, 4.0f);
    CHECK(e.escapes() == 1);
    CHECK(e.verifying());
    CHECK(vec_near(e.bias(), stepped, 0.03f));
    run_rest(e, rng, stepped, 35.0f);  // survives verification
    CHECK(e.rollbacks() == 0 && !e.verifying());
    CHECK(vec_near(e.bias(), stepped, 0.03f));
}

// The plateau ends and the gyro returns to the old bias: the escape was motion, undo it.
void test_bias_escape_rollback() {
    Lcg rng;
    GyroBiasEstimator e = calibrated(rng);
    run_rest(e, rng, kBias0 + Vec3{0.8f, 0, 0}, 11.0f);
    CHECK(e.escapes() == 1);
    run_rest(e, rng, kBias0, 4.0f);
    CHECK(e.rollbacks() == 1);
    CHECK(!e.verifying());
    CHECK(vec_near(e.bias(), kBias0, 0.03f));
}

// Worn from the start: tremor never meets the desk rest gate. Tracking must still begin,
// from the steadiest block, with a bias close to the true one.
void test_bias_worn_start() {
    Lcg rng;
    GyroBiasEstimator e;
    run_rest(e, rng, kBias0, 13.0f, 3.0f);
    CHECK(!e.tracking());  // fallback needs 4 s warmup + 10 s
    run_rest(e, rng, kBias0, 2.0f, 3.0f);
    CHECK(e.tracking() && e.worn_start());
    CHECK(vec_near(e.bias(), kBias0, 0.15f));

    // Desk start is unchanged: rest window, no fallback.
    GyroBiasEstimator d;
    run_rest(d, rng, kBias0, 8.0f);
    CHECK(d.tracking() && !d.worn_start());

    // A steady turn larger than any plausible bias is never adopted as bias.
    GyroBiasEstimator t;
    run_rest(t, rng, kBias0 + Vec3{0, 0, 4.0f}, 20.0f, 3.0f);
    CHECK(!t.tracking());
}

// Worn head: tremor plus the occasional slow drift of the head while settling.
void run_worn(GyroBiasEstimator& e, Lcg& rng, Vec3 bias, float secs) {
    const int n = static_cast<int>(secs / kRefDt);
    for (int i = 0; i < n; ++i) {
        const float t = static_cast<float>(i) * kRefDt;
        // 0.8 deg/s yaw for 0.5 s every 2 s, alternating direction (net zero).
        const float phase = std::fmod(t, 4.0f);
        const float slow = phase < 0.5f ? 0.8f : (phase >= 2.0f && phase < 2.5f ? -0.8f : 0.0f);
        const Vec3 tremor{rng.uni() * 3.0f, rng.uni() * 3.0f, rng.uni() * 3.0f};
        e.update(bias + Vec3{0, 0, slow} + tremor, {0, 0, 9.81f}, kRefDt);
    }
}

// Worn start: median over the steady blocks is close to the true bias even with head motion.
void test_bias_worn_start_median() {
    Lcg rng;
    GyroBiasEstimator e;
    run_worn(e, rng, kBias0, 16.0f);
    CHECK(e.tracking() && e.worn_start());
    CHECK(vec_near(e.bias(), kBias0, 0.08f));
}

// Thermal shift while worn (no desk rest at all): the bias follows.
void test_bias_worn_drift_correction() {
    Lcg rng;
    GyroBiasEstimator e = calibrated(rng);
    const Vec3 warm = kBias0 + Vec3{0, 0, 0.3f};
    run_worn(e, rng, warm, 90.0f);
    CHECK(vec_near(e.bias(), warm, 0.05f));
}

// Reading-like head motion while worn: slow pans (1 deg/s, 3 s) with fast returns. The
// pans exceed the residual gate; the bias must not be dragged.
void test_bias_worn_ignores_head_motion() {
    Lcg rng;
    GyroBiasEstimator e = calibrated(rng);
    for (int cycle = 0; cycle < 20; ++cycle) {
        run_rest(e, rng, kBias0, 4.0f, 3.0f);                    // hold (tremor)
        run_rest(e, rng, kBias0 + Vec3{0, 0, 1.0f}, 3.0f, 3.0f);   // slow pan +3 deg
        run_rest(e, rng, kBias0 + Vec3{0, 0, -30.0f}, 0.1f, 3.0f); // fast return
    }
    CHECK(vec_near(e.bias(), kBias0, 0.05f));
}

// ---------------------------------------------------------------------------
// Sensor -> head calibration
// ---------------------------------------------------------------------------

bool quat_near(const Quat& a, const Quat& b, float eps) {
    const float d = std::fabs(a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z);  // q ~ -q
    return d >= 1.0f - eps;
}

HeadCalibrationInput synth_calibration(const Quat& sensor_to_head, Vec3 yaw_axis_head = {0, 0, 1}) {
    const Quat to_sensor = sensor_to_head.conjugate();
    const Vec3 left{0, 1, 0};
    HeadCalibrationInput in;
    in.gravity_center = to_sensor.rotate({0, 0, 9.81f});
    in.rot_up = to_sensor.rotate(left * -25.0f);
    in.rot_down = to_sensor.rotate(left * 22.0f);
    in.rot_left = to_sensor.rotate(yaw_axis_head * 35.0f);
    in.rot_right = to_sensor.rotate(yaw_axis_head * -30.0f);
    return in;
}

void test_head_calibration_solves_mounting() {
    const Quat mount = Quat::from_axis_angle({0.3f, 1.0f, 0.2f}, 50.0f * 0.0174533f);
    HeadCalibrationInput in = synth_calibration(mount);
    in.rot_up = in.rot_up + Vec3{0.5f, -0.3f, 0.4f};  // imperfect human motion
    in.rot_left = in.rot_left + Vec3{-0.8f, 0.6f, 0.2f};
    const auto r = solve_head_calibration(in);
    CHECK(r.ok);
    CHECK(quat_near(r.sensor_to_head, mount, 2e-4f));
    CHECK(r.yaw_axis_error_deg < 3.0f);

    // Rows -> quaternion -> rows round trip for a non-trivial frame.
    const Quat q = Quat::from_rows({0, 1, 0}, {-1, 0, 0}, {0, 0, 1});
    CHECK(vec_near(q.rotate({0, 1, 0}), {1, 0, 0}, 1e-5f));
    CHECK(vec_near(q.rotate({1, 0, 0}), {0, -1, 0}, 1e-5f));
}

// Reference-unit run (2026-10-13): small nods with a turn mixed in tilted the nod axis
// ~30 deg. The turn is removable, so the mounting must still be recovered.
void test_head_calibration_tolerates_turn_in_nods() {
    const Quat mount = Quat::from_axis_angle({1, 0, 0}, -29.4f * 0.0174533f);  // IMU pitched in the frame
    const Quat to_sensor = mount.conjugate();
    const Vec3 left{0, 1, 0}, up{0, 0, 1};
    HeadCalibrationInput in = synth_calibration(mount);
    in.rot_up = to_sensor.rotate(left * -15.0f + up * 9.0f);
    in.rot_down = to_sensor.rotate(left * 17.4f + up * -8.0f);
    const auto r = solve_head_calibration(in);
    CHECK(r.pitch_axis_tilt_deg > 25.0f);  // what the old check rejected
    CHECK(r.ok);
    CHECK(quat_near(r.sensor_to_head, mount, 2e-4f));
    CHECK(r.nod_mismatch_deg < 1.0f);

    // A head tilt (roll about forward) mixed in with opposite senses is not removable.
    const Vec3 fwd{1, 0, 0};
    HeadCalibrationInput rolled = synth_calibration(mount);
    rolled.rot_up = to_sensor.rotate(left * -15.0f + fwd * 10.0f);
    rolled.rot_down = to_sensor.rotate(left * 17.0f + fwd * 10.0f);
    const auto rr = solve_head_calibration(rolled);
    CHECK(!rr.ok && rr.nod_mismatch_deg > 25.0f);

    // Nods that are really turns: degenerate.
    HeadCalibrationInput turned = synth_calibration(mount);
    turned.rot_up = to_sensor.rotate(left * -5.0f + up * 20.0f);
    turned.rot_down = to_sensor.rotate(left * 5.0f + up * -20.0f);
    CHECK(!solve_head_calibration(turned).ok);
}

void test_head_calibration_rejects_bad_input() {
    HeadCalibrationInput small = synth_calibration(Quat{});
    small.rot_left = small.rot_left * 0.1f;
    CHECK(!solve_head_calibration(small).ok);

    // Turned around a 40 deg tilted axis: the head was not level.
    const Vec3 tilted{0.0f, std::sin(0.698f), std::cos(0.698f)};
    CHECK(!solve_head_calibration(synth_calibration(Quat{}, tilted)).ok);

    HeadCalibrationInput same_sense = synth_calibration(Quat{});
    same_sense.rot_down = same_sense.rot_up;
    CHECK(!solve_head_calibration(same_sense).ok);
}

// Worn-head tremor (~3 deg/s per axis) still counts as holding; a nod does not.
void test_hold_detector() {
    Lcg rng;
    HoldDetector h;
    auto run = [&](Vec3 base, float noise, float secs) {
        for (int i = 0; i < static_cast<int>(secs / kRefDt); ++i) {
            h.update(base + Vec3{rng.uni() * noise, rng.uni() * noise, rng.uni() * noise}, kRefDt);
        }
    };
    run({0, 0, 0}, 3.0f, 0.5f);
    CHECK(!h.holding());  // needs hold_s
    run({0, 0, 0}, 3.0f, 0.3f);
    CHECK(h.holding() && !h.moving());
    run({0, -40.0f, 0}, 3.0f, 0.5f);
    CHECK(h.moving() && !h.holding());
    run({0, 0, 0}, 3.0f, 0.6f);
    CHECK(!h.holding());  // decay + hold time not yet elapsed
    run({0, 0, 0}, 3.0f, 0.6f);
    CHECK(h.holding());
    // A slow drift of the head (6 deg/s) is neither a hold nor a deliberate move.
    h.reset();
    run({6.0f, 0, 0}, 0.5f, 2.0f);
    CHECK(!h.holding() && !h.moving());
    // The bias estimator's desk rest gate rejects the same tremor.
    GyroBiasEstimator e;
    Lcg rng2;
    run_rest(e, rng2, {0, 0, 0}, 10.0f, 3.0f);
    CHECK(!e.tracking());
}

void test_calibration_json_roundtrip() {
    const Quat q = Quat::from_axis_angle({1, -2, 0.5f}, 1.1f);
    const auto back = calibration_from_json(calibration_to_json(q));
    CHECK(back.has_value() && quat_near(*back, q, 1e-6f));
    CHECK(!calibration_from_json("{}").has_value());
    CHECK(!calibration_from_json(R"({"sensor_to_head": {"w": 2, "x": 0, "y": 0, "z": 0}})").has_value());
    CHECK(!calibration_from_json(R"({"sensor_to_head": {"w": 1, "x": 0}})").has_value());
}

// IMU mounted with its +Y pointing forward. A nod (head pitch) is a sensor X rotation:
// without calibration it reads as roll, with the calibration quaternion as pitch.
void test_pipeline_applies_sensor_to_head() {
    const Quat mount = Quat::from_rows({0, 1, 0}, {-1, 0, 0}, {0, 0, 1});
    for (bool calibrated_pipeline : {false, true}) {
        ImuPipeline p;
        if (calibrated_pipeline) p.set_sensor_to_head(mount);
        uint32_t tick = 0;
        Quat truth;  // sensor -> earth
        const float rate = 60.0f;
        auto feed = [&](float gx_degs, int n) {
            Pose last;
            for (int i = 0; i < n; ++i) {
                tick += 21;
                truth = (truth * Quat::from_axis_angle({1, 0, 0}, gx_degs * 0.0174533f * 0.0021f)).normalized();
                const Vec3 a = truth.conjugate().rotate({0, 0, 9.81f});
                const auto r = make_imu_report(a, {gx_degs, 0, 0}, tick);
                last = *p.process_report(r.data(), r.size());
            }
            return last;
        };
        feed(0.0f, 476 * 6);
        const Pose nod = feed(rate, 238);  // 0.5 s -> 30 deg
        if (calibrated_pipeline) {
            CHECK(near(std::fabs(nod.euler.pitch), 30.0f, 1.5f));
            CHECK(near(nod.euler.roll, 0.0f, 1.5f));
        } else {
            CHECK(near(std::fabs(nod.euler.roll), 30.0f, 1.5f));
            CHECK(near(nod.euler.pitch, 0.0f, 1.5f));
        }
        CHECK(near(nod.euler.yaw, 0.0f, 1.5f));
        // Angular velocity is reported in the relative frame: a nod is about head -Y
        // (calibrated) or core X (uncalibrated: sensor axes taken as head axes).
        const Vec3 w = nod.angular_velocity_rads;
        const float r = rate * 0.0174533f;
        if (calibrated_pipeline) {
            CHECK(near(std::fabs(w.y), r, 0.03f) && near(w.x, 0, 0.03f));
        } else {
            CHECK(near(std::fabs(w.x), r, 0.03f) && near(w.y, 0, 0.03f));
        }
        // Predicting 10 ms ahead matches the pose 10 ms later.
        const Quat pred = predict_orientation(nod.relative, w, 0.0105f);
        const Pose later = feed(rate, 5);
        CHECK(quat_near(pred, later.relative, 2e-5f));
    }
}

// ---------------------------------------------------------------------------
// Render math (core Z-up RH -> render Y-up LH)
// ---------------------------------------------------------------------------

constexpr float kDeg = 0.0174532925f;

float det3(const Mat4& m) {
    return m.at(0, 0) * (m.at(1, 1) * m.at(2, 2) - m.at(1, 2) * m.at(2, 1)) -
           m.at(0, 1) * (m.at(1, 0) * m.at(2, 2) - m.at(1, 2) * m.at(2, 0)) +
           m.at(0, 2) * (m.at(1, 0) * m.at(2, 1) - m.at(1, 1) * m.at(2, 0));
}

void test_core_to_render_axes() {
    CHECK(vec_near(core_to_render({1, 0, 0}), {0, 0, 1}, 1e-6f));   // forward -> +Z
    CHECK(vec_near(core_to_render({0, 1, 0}), {-1, 0, 0}, 1e-6f));  // left -> -X
    CHECK(vec_near(core_to_render({0, 0, 1}), {0, 1, 0}, 1e-6f));   // up -> +Y
    // Handedness flips: core X x Y = Z, render images satisfy the left-handed rule.
    const Vec3 x = core_to_render({1, 0, 0}), y = core_to_render({0, 1, 0});
    CHECK(vec_near(x.cross(y), core_to_render({0, 0, 1}) * -1.0f, 1e-6f));
}

void test_render_rotation() {
    // Turn left 30 deg: the view direction swings to -X.
    const Mat4 yaw = render_rotation(Quat::from_axis_angle({0, 0, 1}, 30 * kDeg));
    CHECK(vec_near(yaw.transform_point({0, 0, 1}), {-std::sin(30 * kDeg), 0, std::cos(30 * kDeg)}, 1e-5f));
    CHECK(near(det3(yaw), 1.0f, 1e-5f));
    // Look up 20 deg (rotation about core -Y): view direction gains +Y.
    const Mat4 up = render_rotation(Quat::from_axis_angle({0, -1, 0}, 20 * kDeg));
    CHECK(vec_near(up.transform_point({0, 0, 1}), {0, std::sin(20 * kDeg), std::cos(20 * kDeg)}, 1e-5f));
    // Roll right ear down (rotation about core +X): render up vector tilts to +X.
    const Mat4 roll = render_rotation(Quat::from_axis_angle({1, 0, 0}, 15 * kDeg));
    CHECK(vec_near(roll.transform_point({0, 1, 0}), {std::sin(15 * kDeg), std::cos(15 * kDeg), 0}, 1e-5f));
    const Mat4 any = render_rotation(Quat::from_axis_angle({0.3f, -0.7f, 0.4f}, 1.3f));
    CHECK(near(det3(any), 1.0f, 1e-5f));
}

void test_view_and_projection() {
    // Head turned left 30 deg: a world point 30 deg to the left lands straight ahead.
    const Quat q = Quat::from_axis_angle({0, 0, 1}, 30 * kDeg);
    const Vec3 world{-2.0f * std::sin(30 * kDeg), 0.0f, 2.0f * std::cos(30 * kDeg)};
    CHECK(vec_near(view_matrix(q, 0.0f).transform_point(world), {0, 0, 2}, 1e-5f));
    // Left eye sits at -IPD/2: a centred point appears to its right.
    CHECK(vec_near(view_matrix(Quat{}, -0.032f).transform_point({0, 0, 2}), {0.032f, 0, 2}, 1e-6f));

    const Mat4 p = perspective_lh(40 * kDeg, 16.0f / 9.0f, 0.1f, 100.0f);
    auto ndc_z = [&](float z) {
        const float cz = p.at(2, 2) * z + p.at(2, 3);
        const float cw = p.at(3, 2) * z;
        return cz / cw;
    };
    CHECK(near(ndc_z(0.1f), 0.0f, 1e-5f));
    CHECK(near(ndc_z(100.0f), 1.0f, 1e-5f));
    // Top edge of the vertical FOV maps to NDC y = 1.
    const float y_top = std::tan(20 * kDeg) * 5.0f;
    CHECK(near(p.at(1, 1) * y_top / 5.0f, 1.0f, 1e-5f));
}

void test_predict_orientation() {
    const Quat q = predict_orientation(Quat{}, {0, 0, 1.0f}, 0.5f);
    CHECK(near(to_euler_deg(q).yaw, 28.6479f, 0.01f));
    const Quat base = Quat::from_axis_angle({0, -1, 0}, 10 * kDeg);
    CHECK(quat_near(predict_orientation(base, {0, 0, 0}, 0.02f), base, 1e-7f));
    // Extrapolation composes in the world frame: yaw added on top of a pitch.
    const Quat p = predict_orientation(base, {0, 0, 1.0f}, 0.1f);
    CHECK(quat_near(p, Quat::from_axis_angle({0, 0, 1}, 0.1f) * base, 1e-6f));
}

void test_curved_screen_mesh() {
    const ScreenMesh m = curved_screen_mesh(2.0f, 60 * kDeg, 1.0f, 32);
    CHECK(m.vertices.size() == 66 && m.indices.size() == 32 * 6);
    for (const auto& v : m.vertices) {
        CHECK(near(std::sqrt(v.px * v.px + v.pz * v.pz), 2.0f, 1e-5f));
        CHECK(near(std::fabs(v.py), 0.5f, 1e-6f));
    }
    const auto& left_top = m.vertices.front();
    CHECK(left_top.px < 0 && left_top.u == 0.0f && left_top.v == 0.0f && left_top.py > 0);
    CHECK(near(left_top.px, -2.0f * std::sin(30 * kDeg), 1e-5f));
    const auto& mid = m.vertices[32];  // u = 0.5, top
    CHECK(near(mid.px, 0.0f, 1e-6f) && near(mid.pz, 2.0f, 1e-6f));
    for (uint16_t i : m.indices) CHECK(i < m.vertices.size());
    CHECK(curved_screen_mesh(2.0f, 0.0f, 1.0f, 8).vertices.empty());
}

void test_panel_layout() {
    const auto c3 = panel_centers(3, 45 * kDeg, 5 * kDeg);
    CHECK(c3.size() == 3);
    CHECK(near(c3[0], -50 * kDeg, 1e-6f) && near(c3[1], 0.0f, 1e-6f) && near(c3[2], 50 * kDeg, 1e-6f));
    const auto c2 = panel_centers(2, 40 * kDeg, 4 * kDeg);  // even count straddles the centre
    CHECK(near(c2[0], -22 * kDeg, 1e-6f) && near(c2[1], 22 * kDeg, 1e-6f));
    CHECK(panel_centers(0, 1, 0).empty());
    CHECK(near(panel_centers(1, 1, 0)[0], 0.0f, 1e-7f));

    // Right panel: its mesh is the centre mesh rotated 50 deg to the right, edges included.
    const ScreenMesh right = curved_screen_mesh(2.0f, 45 * kDeg, 1.0f, 8, c3[2], 0.1f);
    const auto& l = right.vertices.front();  // left/top edge at 27.5 deg
    CHECK(near(std::atan2(l.px, l.pz), 27.5f * kDeg, 1e-5f) && near(l.py, 0.6f, 1e-6f));
    const auto& r = right.vertices.back();  // right/bottom edge at 72.5 deg
    CHECK(near(std::atan2(r.px, r.pz), 72.5f * kDeg, 1e-5f) && near(r.py, -0.4f, 1e-6f));

    // Looking 50 deg right (core yaw -50) puts that panel's centre straight ahead.
    const Quat look_right = Quat::from_axis_angle({0, 0, 1}, -50 * kDeg);
    const Vec3 centre{2.0f * std::sin(c3[2]), 0.0f, 2.0f * std::cos(c3[2])};
    CHECK(vec_near(view_matrix(look_right, 0.0f).transform_point(centre), {0, 0, 2}, 1e-5f));

    CHECK(near(arc_for_pixels(1920.0f, 48.0f), 40 * kDeg, 1e-6f));
    CHECK(arc_for_pixels(100.0f, 0.0f) == 0.0f);
}

void test_flat_panels() {
    const float arc = 38 * kDeg, r = 2.0f;
    const auto centers = panel_centers(2, arc, 0.0f);
    const ScreenMesh left = flat_screen_mesh(r, arc, 1.0f, 8, centers[0]);
    const ScreenMesh right = flat_screen_mesh(r, arc, 1.0f, 8, centers[1]);
    CHECK(left.vertices.size() == 18 && left.indices.size() == 48);
    // Planar: every vertex lies on the plane tangent to the circle at the panel centre.
    for (const auto& v : right.vertices) {
        CHECK(near(v.px * std::sin(centers[1]) + v.pz * std::cos(centers[1]), r, 1e-5f));
    }
    // Spans exactly `arc` from the eye.
    const auto& l0 = right.vertices.front();
    const auto& l1 = right.vertices.back();
    CHECK(near(std::atan2(l1.px, l1.pz) - std::atan2(l0.px, l0.pz), arc, 1e-5f));
    // With gap 0, adjacent flat panels share their edge (contiguous monitors).
    const auto& left_edge = left.vertices[left.vertices.size() - 2];  // right/top of left panel
    const auto& right_edge = right.vertices[0];                      // left/top of right panel
    CHECK(vec_near({left_edge.px, left_edge.py, left_edge.pz}, {right_edge.px, right_edge.py, right_edge.pz}, 1e-5f));
    // Centre of a straight-ahead panel is at distance r on +Z; u runs left -> right.
    const ScreenMesh ahead = flat_screen_mesh(r, arc, 1.0f, 2);
    CHECK(near(ahead.vertices[2].px, 0.0f, 1e-6f) && near(ahead.vertices[2].pz, r, 1e-6f));
    CHECK(ahead.vertices[0].px < 0 && ahead.vertices[0].u == 0.0f);
    CHECK(near(panel_width(r, arc, true), 2 * r * std::tan(arc / 2), 1e-6f));
    CHECK(near(panel_width(r, arc, false), r * arc, 1e-6f));
}

void test_panel_at_gaze() {
    const float arc = 38 * kDeg, gap = 1 * kDeg;
    const auto c = panel_centers(3, arc, gap);  // -39, 0, +39 deg
    auto look = [](float yaw_left_deg, float pitch_up_deg) {
        return Quat::from_axis_angle({0, 0, 1}, yaw_left_deg * kDeg) *
               Quat::from_axis_angle({0, -1, 0}, pitch_up_deg * kDeg);
    };
    CHECK(panel_at_gaze(look(0, 0), c, arc, gap) == 1);
    CHECK(panel_at_gaze(look(35, 0), c, arc, gap) == 0);    // turned left
    CHECK(panel_at_gaze(look(-35, 10), c, arc, gap) == 2);  // right, slightly up
    CHECK(panel_at_gaze(look(-21, 0), c, arc, gap) == 2);   // just past the seam at 19.5
    CHECK(panel_at_gaze(look(80, 0), c, arc, gap) == -1);   // beyond the row
    CHECK(panel_at_gaze(look(0, 0), {}, arc, gap) == -1);
}

void test_fit_panels() {
    const float hfov = horizontal_fov(24.5f * kDeg, 16.0f / 9.0f);
    CHECK(near(hfov, 42.2f * kDeg, 0.2f * kDeg));
    const float arc = fit_panel_arc(hfov, 2, 1 * kDeg, 1 * kDeg);
    // Both panels, the gap and the margins exactly fill the view.
    CHECK(near(2 * arc + 1 * kDeg + 2 * 1 * kDeg, hfov, 1e-5f));
    const auto c = panel_centers(2, arc, 1 * kDeg);
    CHECK(near(c[1] + arc / 2, hfov / 2 - 1 * kDeg, 1e-5f));  // right edge inside the view
    CHECK(fit_panel_arc(hfov, 0, 0, 0) == 0.0f);
    CHECK(fit_panel_arc(0.1f, 3, 0.1f, 0.0f) == 0.0f);
}

void test_grid_layout() {
    const float arc = 38 * kDeg, gap = 2 * kDeg;
    const float varc = panel_vertical_arc(arc, 16.0f / 9.0f, true);
    // Flat 16:9 panel spanning 38 deg: vertical span from the same tangent geometry.
    CHECK(near(varc, 2 * std::atan(std::tan(19 * kDeg) * 9.0f / 16.0f), 1e-6f));
    CHECK(near(panel_vertical_arc(arc, 16.0f / 9.0f, false), 2 * std::atan(arc * 9.0f / 32.0f), 1e-6f));

    const auto quad = grid_layout({{0, 0}, {1, 0}, {0, 1}, {1, 1}}, arc, varc, gap);
    CHECK(near(quad[0].yaw_rad, -(arc + gap) / 2, 1e-6f) && near(quad[0].pitch_rad, (varc + gap) / 2, 1e-6f));
    CHECK(near(quad[3].yaw_rad, (arc + gap) / 2, 1e-6f) && near(quad[3].pitch_rad, -(varc + gap) / 2, 1e-6f));
    const auto stacked = grid_layout({{0, 0}, {0, 1}}, arc, varc, gap);
    CHECK(near(stacked[0].yaw_rad, 0.0f, 1e-7f) && stacked[0].pitch_rad > 0 && near(stacked[1].pitch_rad, -stacked[0].pitch_rad, 1e-6f));
    const auto offset = grid_layout({{3, 5}, {4, 5}}, arc, varc, gap);  // translation-invariant
    CHECK(near(offset[0].yaw_rad, -(arc + gap) / 2, 1e-6f) && near(offset[0].pitch_rad, 0.0f, 1e-7f));
    CHECK(grid_layout({}, arc, varc, gap).empty());

    // Stacked flat panels with gap 0 share their horizontal edge, like a monitor on a monitor.
    const auto s0 = grid_layout({{0, 0}, {0, 1}}, arc, varc, 0.0f);
    ScreenMesh top = flat_screen_mesh(2.0f, arc, panel_width(2.0f, arc, true) * 9.0f / 16.0f, 1);
    ScreenMesh bottom = top;
    pitch_mesh(top, s0[0].pitch_rad);
    pitch_mesh(bottom, s0[1].pitch_rad);
    const auto& top_bottom_left = top.vertices[1];        // v = 1 (bottom edge), u = 0
    const auto& bottom_top_left = bottom.vertices[0];     // v = 0 (top edge), u = 0
    CHECK(vec_near({top_bottom_left.px, top_bottom_left.py, top_bottom_left.pz},
                   {bottom_top_left.px, bottom_top_left.py, bottom_top_left.pz}, 1e-5f));

    // Gaze: up -> top panel, down -> bottom, diagonal in a 2x2, beyond -> none.
    auto look = [](float yaw_left_deg, float pitch_up_deg) {
        return Quat::from_axis_angle({0, 0, 1}, yaw_left_deg * kDeg) * Quat::from_axis_angle({0, -1, 0}, pitch_up_deg * kDeg);
    };
    const float up = (varc + gap) / 2 / kDeg;
    CHECK(panel_at_gaze(look(0, up), stacked, arc, varc, gap) == 0);
    CHECK(panel_at_gaze(look(0, -up), stacked, arc, varc, gap) == 1);
    CHECK(panel_at_gaze(look(-15, -up), quad, arc, varc, gap) == 3);  // right + down
    CHECK(panel_at_gaze(look(15, up), quad, arc, varc, gap) == 0);    // left + up
    CHECK(panel_at_gaze(look(0, 60), stacked, arc, varc, gap) == -1);
}

void test_bent_panels() {
    const float r = 2.0f, arc = 38 * kDeg, h = 0.8f;
    // Curvature 0 is exactly the flat panel.
    const ScreenMesh flat = flat_screen_mesh(r, arc, h, 16, 10 * kDeg);
    const ScreenMesh zero = bent_screen_mesh(r, arc, h, 16, 0.0f, 10 * kDeg);
    CHECK(flat.vertices.size() == zero.vertices.size());
    for (size_t i = 0; i < flat.vertices.size(); ++i) {
        CHECK(near(flat.vertices[i].px, zero.vertices[i].px, 1e-6f) && near(flat.vertices[i].pz, zero.vertices[i].pz, 1e-6f));
    }
    // Curvature 1: every column on the viewing circle.
    const ScreenMesh full = bent_screen_mesh(r, arc, h, 16, 1.0f);
    for (const auto& v : full.vertices) CHECK(near(std::sqrt(v.px * v.px + v.pz * v.pz), r, 1e-5f));
    // Any curvature: centre stays at distance r straight ahead; the sheet keeps its width
    // (arc length between the edges equals the flat width).
    for (float k : {0.25f, 0.5f, 0.8f}) {
        const ScreenMesh m = bent_screen_mesh(r, arc, h, 64, k);
        const auto& mid = m.vertices[64];  // u = 0.5, top
        CHECK(near(mid.px, 0.0f, 1e-5f) && near(mid.pz, r, 1e-5f));
        float length = 0.0f;
        for (size_t i = 2; i < m.vertices.size(); i += 2) {
            const float dx = m.vertices[i].px - m.vertices[i - 2].px, dz = m.vertices[i].pz - m.vertices[i - 2].pz;
            length += std::sqrt(dx * dx + dz * dz);
        }
        CHECK(near(length, 2 * r * std::tan(arc / 2), 2e-3f));
        // More curvature pulls the edges closer to the viewer (towards the viewing circle).
        const ScreenMesh more = bent_screen_mesh(r, arc, h, 64, k + 0.1f);
        auto edge = [](const ScreenMesh& mm) { return std::sqrt(mm.vertices[0].px * mm.vertices[0].px + mm.vertices[0].pz * mm.vertices[0].pz); };
        CHECK(edge(more) < edge(m) && edge(more) >= r - 1e-5f);
    }
}

void test_barrel_distort() {
    const Vec2 c = barrel_distort({0, 0}, 0.2f, 0.1f);
    CHECK(c.x == 0.0f && c.y == 0.0f);
    const Vec2 e = barrel_distort({0, 1}, 0.1f, 0.0f);
    CHECK(near(e.y, 1.1f, 1e-6f));
    const Vec2 d = barrel_distort({0.6f, 0.8f}, 0.1f, 0.05f);  // r = 1
    CHECK(near(d.x, 0.6f * 1.15f, 1e-6f) && near(d.y, 0.8f * 1.15f, 1e-6f));
    const Vec2 id = barrel_distort({0.3f, -0.4f}, 0, 0);
    CHECK(id.x == 0.3f && id.y == -0.4f);
}

// ---------------------------------------------------------------------------
// Pose stabilizer
// ---------------------------------------------------------------------------

float angle_between_deg(const Quat& a, const Quat& b) {
    const float d = std::fabs(a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z);
    return 2.0f * std::acos(std::fmin(1.0f, d)) * 57.29578f;
}

// Yaw-only head: keeps the turn, drops nod and tilt.
void test_yaw_only() {
    const Quat yaw = Quat::from_axis_angle({0, 0, 1}, 0.6f);
    const Quat pitch = Quat::from_axis_angle({0, -1, 0}, 0.4f);
    const Quat roll = Quat::from_axis_angle({1, 0, 0}, 0.2f);
    const Quat y = yaw_only(yaw * pitch * roll);  // turn, then nod and tilt in the head frame
    CHECK(near(y.w, yaw.w, 1e-4f) && near(y.z, yaw.z, 1e-4f) && y.x == 0.0f && y.y == 0.0f);
    const Quat level = yaw_only(pitch);  // looking up: no turn
    CHECK(near(level.w, 1.0f, 1e-6f) && near(level.z, 0.0f, 1e-6f));
    const Vec3 fwd = y.rotate({1, 0, 0});  // forward stays on the horizon
    CHECK(near(fwd.z, 0.0f, 1e-5f));
    const Quat v = pitch_only(yaw * pitch);  // up/down only: forward keeps its elevation, no turn
    const Vec3 vf = v.rotate({1, 0, 0}), hf = (yaw * pitch).rotate({1, 0, 0});
    CHECK(near(vf.z, hf.z, 1e-5f) && near(vf.y, 0.0f, 1e-6f));
    CHECK(track_axes(yaw, TrackingAxes::Both).z == yaw.z);
    const float arc = 0.66f;  // ~38 deg at the 2 m reference
    CHECK(near(arc_at_distance(arc, 2.0f), arc, 1e-6f));
    CHECK(arc_at_distance(arc, 1.2f) > arc && arc_at_distance(arc, 3.0f) < arc);
    CHECK(near(arc_at_distance(arc_at_distance(arc, 3.0f), 2.0f, 3.0f), arc, 1e-5f));  // inverse
}

// Hand-placed panels: placements in radians with their scale; OS cells by columns and rows.
void test_panel_poses() {
    const std::vector<PanelPose> row = {{-20, 0, 1}, {20, 0, 0.6f}, {60, 2, 1}};
    const auto pl = placements_from_poses(row, 0.5f);
    CHECK(pl.size() == 3 && near(pl[1].yaw_rad, 0.17453f, 1e-4f) && near(pl[1].scale, 0.6f, 1e-6f));
    const auto cells = cells_from_poses(row, 38, 22);
    CHECK(cells[0].col == 0 && cells[1].col == 1 && cells[2].col == 2 && cells[0].row == 0 && cells[2].row == 0);
    const std::vector<PanelPose> stacked = {{0, 12, 1}, {1, -12, 1}};  // one above the other
    const auto sc = cells_from_poses(stacked, 38, 22);
    CHECK(sc[0].col == sc[1].col && sc[0].row == 0 && sc[1].row == 1);
    const std::vector<PanelPose> same = {{0, 0, 1}, {0, 0, 1}};  // overlapping: still two cells
    const auto dc = cells_from_poses(same, 38, 22);
    CHECK(!(dc[0].col == dc[1].col && dc[0].row == dc[1].row));
    // Gaze honours each panel's size: the small one is narrower.
    const auto head = Quat::from_axis_angle({0, 0, 1}, -0.30f);  // ~17 deg right of panel 2's centre
    CHECK(panel_at_gaze(head, placements_from_poses({{0, 0, 1}}), 0.66f, 0.38f, 0.0f) == 0);
    CHECK(panel_at_gaze(head, placements_from_poses({{0, 0, 0.5f}}), 0.66f, 0.38f, 0.0f) == -1);
}

// Pinned zoom: continuous, never past the image.
void test_pinned_zoom() {
    const float d2r = 0.0174533f;
    CHECK(near(pinned_zoom(arc_at_distance(60 * d2r, 1.2f, 2.0f)), 1.0f, 1e-4f));
    CHECK(pinned_zoom(arc_at_distance(70 * d2r, 1.0f, 2.0f)) <= 1.0f);
    const float z38 = pinned_zoom(38 * d2r), z40 = pinned_zoom(40 * d2r);
    CHECK(z38 > 0.78f && z38 < 0.84f && z40 > z38);
}

// A window placed above two monitors, low enough to overlap them once they are taller.
void test_separate_floating() {
    std::vector<PanelPlacement> pl = {{-0.33f, 0, 1}, {0.33f, 0, 1}, {0, 0.38f, 1}};
    const std::vector<float> w = {0.64f, 0.64f, 0.64f}, h = {0.42f, 0.42f, 0.36f};  // 16:10 monitors
    separate_floating(pl, w, h, 2, 0.02f);
    CHECK(near(pl[2].yaw_rad, 0.0f, 1e-6f));                         // only lifted
    CHECK(pl[2].pitch_rad - 0.18f >= 0.21f + 0.02f - 1e-5f);         // clear of the monitors' tops
    CHECK(near(pl[0].pitch_rad, 0.0f, 1e-6f) && near(pl[1].yaw_rad, 0.33f, 1e-6f));  // monitors untouched
    std::vector<PanelPlacement> clear = {{0, 0, 1}, {0, 0.6f, 1}};
    separate_floating(clear, {0.64f, 0.64f}, {0.36f, 0.36f}, 1, 0.02f);
    CHECK(near(clear[1].pitch_rad, 0.6f, 1e-6f));                    // already clear: as placed
}

// A panel raised 20 deg: tilt 1 faces the viewer, tilt 0 stands upright, same centre.
void test_place_pitched() {
    const float d2r = 0.0174533f;
    const auto mesh = [] {
        ScreenMesh m;
        m.vertices = {{0, 0.5f, 2.0f, 0, 0}, {0, -0.5f, 2.0f, 0, 1}};  // top and bottom edge centres
        return m;
    };
    ScreenMesh facing = mesh(), upright = mesh();
    place_pitched(facing, 20 * d2r, 1.0f, 2.0f);
    place_pitched(upright, 20 * d2r, 0.0f, 2.0f);
    const float cy = 2.0f * std::sin(20 * d2r), cz = 2.0f * std::cos(20 * d2r);
    CHECK(near(0.5f * (upright.vertices[0].py + upright.vertices[1].py), cy, 1e-4f));
    CHECK(near(0.5f * (upright.vertices[0].pz + upright.vertices[1].pz), cz, 1e-4f));
    CHECK(near(upright.vertices[0].pz, upright.vertices[1].pz, 1e-4f));    // upright: same depth
    CHECK(facing.vertices[0].pz < facing.vertices[1].pz - 0.2f);           // facing: top leans back
}

// NoRoll: same view direction as the full rotation, no tilt about it.
void test_track_no_roll() {
    const float d2r = 0.0174533f;
    const Quat q = Quat::from_axis_angle({0, 0, 1}, 30 * d2r) * Quat::from_axis_angle({0, -1, 0}, 10 * d2r) *
                   Quat::from_axis_angle({1, 0, 0}, 15 * d2r);  // turned, looking up, head tilted
    const Quat n = track_axes(q, TrackingAxes::NoRoll);
    const Vec3 f1 = q.rotate({1, 0, 0}), f2 = n.rotate({1, 0, 0});
    CHECK(near(f1.x, f2.x, 1e-5f) && near(f1.y, f2.y, 1e-5f) && near(f1.z, f2.z, 1e-5f));  // same direction
    CHECK(std::fabs(n.rotate({0, 1, 0}).z) < 1e-5f);  // the view's left axis stays level: no tilt
}

// Neck model: at rest nothing moves; turned 30 deg, the eyes have moved sideways by
// forward * sin(30) and a point straight ahead in the room shifts accordingly.
void test_neck_model() {
    const Mat4 rest = view_matrix(Quat{}, 0.0f, kNeckUpM, kNeckForwardM);
    CHECK(near(rest.at(0, 3), 0.0f, 1e-6f) && near(rest.at(1, 3), 0.0f, 1e-6f) && near(rest.at(2, 3), 0.0f, 1e-6f));
    const float d2r = 0.0174533f;
    const Quat yaw = Quat::from_axis_angle({0, 0, 1}, 30 * d2r);  // core: +Z up, turning left
    const Mat4 plain = view_matrix(yaw, 0.0f), neck = view_matrix(yaw, 0.0f, kNeckUpM, kNeckForwardM);
    const auto eye_x = [](const Mat4& v) { return v.at(0, 0) * 0 + v.at(0, 1) * 0 + v.at(0, 2) * 2.0f + v.at(0, 3); };
    // The point 2 m ahead in the room, horizontally in the eye frame: shifted by about the eyes'
    // sideways travel (0.08 * sin 30 = 4 cm).
    CHECK(std::fabs(std::fabs(eye_x(neck) - eye_x(plain)) - kNeckForwardM * 0.5f) < 0.01f);
}

void test_slerp() {
    const Quat a{}, b = Quat::from_axis_angle({0, 0, 1}, 1.0f);
    CHECK(quat_near(slerp(a, b, 0.0f), a, 1e-6f) && quat_near(slerp(a, b, 1.0f), b, 1e-6f));
    CHECK(quat_near(slerp(a, b, 0.25f), Quat::from_axis_angle({0, 0, 1}, 0.25f), 1e-6f));
    const Quat nb{-b.w, -b.x, -b.y, -b.z};  // same rotation, other hemisphere
    CHECK(quat_near(slerp(a, nb, 0.5f), Quat::from_axis_angle({0, 0, 1}, 0.5f), 1e-6f));
}

// Looking at a fixed spot: 8 Hz tremor (0.3 deg), breathing sway and sensor noise must not
// move the output at all (like a normal monitor).
void test_stabilizer_holds_still() {
    Lcg rng;
    PoseStabilizer s;
    const float dt = kRefDt, d2r = 0.0174533f;
    Quat first;
    float max_in = 0.0f, max_out = 0.0f;
    for (int i = 0; i < 457 * 8; ++i) {
        const float t = static_cast<float>(i) * dt;
        const float tremor = 0.3f * d2r * std::sin(2 * 3.14159f * 8.0f * t);
        const float sway = 0.5f * d2r * std::sin(2 * 3.14159f * 0.25f * t);  // breathing
        const float pitch = tremor + sway + rng.uni() * 0.05f * d2r;
        const float rate = 0.3f * d2r * 2 * 3.14159f * 8.0f * std::cos(2 * 3.14159f * 8.0f * t) +
                           0.5f * d2r * 2 * 3.14159f * 0.25f * std::cos(2 * 3.14159f * 0.25f * t) +
                           rng.uni() * 0.03f;
        const Quat q = Quat::from_axis_angle({0, 1, 0}, pitch);
        const Quat out = s.update(q, {0, rate, 0}, dt);
        if (i == 0) first = out;
        max_in = std::fmax(max_in, angle_between_deg(q, Quat{}));
        max_out = std::fmax(max_out, angle_between_deg(out, first));
    }
    CHECK(max_in > 0.7f);
    CHECK(max_out == 0.0f);  // frozen
    CHECK(s.holding() && s.omega().norm() == 0.0f);
    CHECK(prediction_gain(s.omega().norm()) == 0.0f);
}

// A real 60 deg/s turn: leaves the dead zone, follows with little lag, then holds again.
// World-locked (space mode), measured on the whole chain: a head looking around (or still)
// with tremor, sensor noise, frames reaching the glasses 30 ms after the sample and predicted
// for then as the renderers do. Error = where a panel is shown vs where it is in the room.
float world_lock_error_deg(const PoseStabilizerConfig& c, bool moving, bool taps = false) {
    const float dt = kRefDt, d2r = 0.0174533f, pi = 3.14159265f, latency = 0.030f;
    const auto head = [&](float t) {
        float yaw = 0.05f * d2r * std::sin(2 * pi * 8 * t), pitch = 0.2f * d2r * std::sin(2 * pi * 0.25f * t);
        float wy = 0.05f * d2r * 2 * pi * 8 * std::cos(2 * pi * 8 * t), wp = 0.2f * d2r * 2 * pi * 0.25f * std::cos(2 * pi * 0.25f * t);
        if (taps) {  // a knock on the frame every 2 s: 0.4 deg at 25 Hz, gone in ~40 ms
            const float tt = std::fmod(t, 2.0f), a = 0.4f * d2r * std::exp(-tt / 0.04f);
            yaw += a * std::sin(2 * pi * 25 * tt);
            wy += a * (2 * pi * 25 * std::cos(2 * pi * 25 * tt) - std::sin(2 * pi * 25 * tt) / 0.04f);
        }
        if (moving) {
            yaw += 20 * d2r * std::sin(2 * pi * 0.25f * t) + 6 * d2r * std::sin(2 * pi * 0.9f * t + 1);
            wy += 20 * d2r * 2 * pi * 0.25f * std::cos(2 * pi * 0.25f * t) + 6 * d2r * 2 * pi * 0.9f * std::cos(2 * pi * 0.9f * t + 1);
            pitch += 8 * d2r * std::sin(2 * pi * 0.17f * t + 2);
            wp += 8 * d2r * 2 * pi * 0.17f * std::cos(2 * pi * 0.17f * t + 2);
        }
        const Quat q = Quat::from_axis_angle({0, 0, 1}, yaw) * Quat::from_axis_angle({0, -1, 0}, pitch);
        const Vec3 pitch_axis = Quat::from_axis_angle({0, 0, 1}, yaw).rotate({0, -1, 0});
        return std::pair<Quat, Vec3>{q, pitch_axis * wp + Vec3{0, 0, wy}};
    };
    PoseStabilizer s(c);
    Lcg rng;
    const auto gauss = [&] { return (rng.uni() + rng.uni() + rng.uni() + rng.uni()) * 0.866f; };
    double sum = 0;
    int n = 0;
    for (int i = 0; i < static_cast<int>(20 / dt); ++i) {
        const float t = static_cast<float>(i) * dt;
        const auto [q, w] = head(t);
        const Quat noisy = Quat::from_axis_angle({gauss(), gauss(), gauss()}, 0.02f * d2r * std::fabs(gauss())) * q;
        const Quat out = s.update(noisy, w + Vec3{gauss(), gauss(), gauss()} * 0.02f, dt);
        if (i % 4 != 0 || t < 2) continue;  // ~114 frames/s, after settling
        const Vec3 om = s.omega();
        const Quat shown = predict_orientation(out, om, latency * prediction_gain(om.norm(), kPredictStartRads, kPredictFullRads));
        const float e = angle_between_deg(shown, head(t + latency).first);
        sum += e * e;
        ++n;
    }
    return static_cast<float>(std::sqrt(sum / n));
}

void test_stabilizer_world_locked() {
    PoseStabilizerConfig plain;  // no dead zone, 8 Hz floor, slow speed filter driving prediction
    plain.enter_deg = 0.0f;
    const auto locked = world_locked_stabilizer();
    const float rest = world_lock_error_deg(locked, false), moving = world_lock_error_deg(locked, true);
    CHECK(rest < 0.04f && rest < world_lock_error_deg(plain, false));
    CHECK(moving < 0.16f && moving < 0.5f * world_lock_error_deg(plain, true));
    // Knocks on the frame: not amplified by the prediction (it tripled them).
    auto no_damping = locked;
    no_damping.reversal_decay_s = 0.0f;
    auto no_tap_rejection = no_damping;
    no_tap_rejection.tap_accel_rads2 = 0.0f;
    const float knocked = world_lock_error_deg(locked, false, true);
    CHECK(knocked < 0.06f && world_lock_error_deg(no_damping, false, true) < 0.5f * world_lock_error_deg(no_tap_rejection, false, true));
    CHECK(knocked <= world_lock_error_deg(no_damping, false, true));
}

// Anchor at rest (world-locked with a 0.3 deg hold): still head with tremor, breathing and
// noise -> the panels do not move at all; a real turn is still followed.
void test_world_locked_anchor() {
    auto c = world_locked_stabilizer();
    c.enter_deg = 0.3f;
    PoseStabilizer s(c);
    Lcg rng;
    const float dt = kRefDt, d2r = 0.0174533f, pi = 3.14159265f;
    Quat first;
    float moved = 0.0f;
    for (int i = 0; i < 457 * 6; ++i) {
        const float t = static_cast<float>(i) * dt;
        const float pitch = 0.05f * d2r * std::sin(2 * pi * 8 * t) + 0.1f * d2r * std::sin(2 * pi * 0.25f * t) + rng.uni() * 0.02f * d2r;
        const Quat out = s.update(Quat::from_axis_angle({0, 1, 0}, pitch), {0, rng.uni() * 0.01f, 0}, dt);
        if (t < 1) continue;
        if (t < 1 + dt) first = out;
        moved = std::fmax(moved, angle_between_deg(out, first));
        CHECK(s.omega().norm() == 0.0f);  // nothing for the renderer to predict
    }
    CHECK(moved == 0.0f);
    float lag = 0.0f;
    for (int i = 0; i < 457; ++i) {  // then a 40 deg/s turn
        const float t = static_cast<float>(i) * dt;
        const Quat q = Quat::from_axis_angle({0, 0, 1}, t * 40 * d2r);
        const Quat out = s.update(q, {0, 0, 40 * d2r}, dt);
        if (t > 0.5f) lag = std::fmax(lag, angle_between_deg(out, q));
    }
    CHECK(lag < 1.0f);
}

// Head still but breathing (5 mm fore-aft, 3 mm side sway): the accelerometer reads that as
// tilt. With the world-locked gain the estimated "up" rocks far less.
float breathing_tilt_error_deg(float beta) {
    const float dt = kRefDt, pi = 3.14159265f, d2r = 0.0174533f;
    MadgwickFilter f(beta);
    f.initialize_from_accel({0, 0, 9.81f});
    Lcg rng;
    const auto gauss = [&] { return (rng.uni() + rng.uni() + rng.uni() + rng.uni()) * 0.866f; };
    const Vec3 bias{0.05f * d2r, -0.03f * d2r, 0.02f * d2r};  // left after bias estimation
    double sum = 0;
    int n = 0;
    for (int i = 0; i < static_cast<int>(30 / dt); ++i) {
        const float t = static_cast<float>(i) * dt;
        const float p = 0.2f * d2r * std::sin(2 * pi * 0.25f * t), wp = 0.2f * d2r * 2 * pi * 0.25f * std::cos(2 * pi * 0.25f * t);
        const Quat q = Quat::from_axis_angle({0, 1, 0}, p);
        const Vec3 lin{-0.005f * std::pow(2 * pi * 0.25f, 2.0f) * std::sin(2 * pi * 0.25f * t),
                       -0.003f * std::pow(2 * pi * 1.2f, 2.0f) * std::sin(2 * pi * 1.2f * t), 0};
        const Vec3 accel = q.conjugate().rotate(lin + Vec3{0, 0, 9.81f}) + Vec3{gauss(), gauss(), gauss()} * 0.02f;
        f.update_imu(Vec3{0, wp, 0} + bias + Vec3{gauss(), gauss(), gauss()} * 0.003f, accel, dt);
        if (t < 5) continue;
        const Vec3 u1 = f.quaternion().conjugate().rotate({0, 0, 1}), u2 = q.conjugate().rotate({0, 0, 1});
        const float e = std::acos(std::fmin(1.0f, u1.x * u2.x + u1.y * u2.y + u1.z * u2.z)) * 57.29578f;
        sum += e * e;
        ++n;
    }
    return static_cast<float>(std::sqrt(sum / n));
}

void test_world_locked_madgwick() {
    const float locked = breathing_tilt_error_deg(kWorldLockedMadgwickBeta);
    CHECK(locked < 0.2f && locked < 0.5f * breathing_tilt_error_deg(0.033f));
}

// Recentered with the head pitched down and rolled, then turned about the vertical: content
// fixed in the room must be seen where the real head sees it.
// Recentered with the head rolled 10 deg: a panel's horizontal edge stays level with the floor.
void test_level_reference() {
    const float d2r = 0.0174533f;
    const Quat mount = Quat::from_axis_angle({0.3f, -0.8f, 0.5f}, 1.1f);
    const Quat head_ref = Quat::from_axis_angle({0, 0, 1}, 25 * d2r) * Quat::from_axis_angle({0, -1, 0}, 5 * d2r) *
                          Quat::from_axis_angle({1, 0, 0}, 10 * d2r);  // turned, looking up a bit, rolled
    const Quat d = body_frame_delta(head_ref * mount, head_ref * mount, mount);
    // The panel straight ahead is drawn at identity; its left edge direction (head Y) in the room:
    const Vec3 left_in_room = level_head(head_ref).rotate({0, 1, 0});
    CHECK(std::fabs(left_in_room.z) < 1e-4f);  // horizontal
    // Seen from the real (rolled) head, that edge leans by the head's roll.
    const Vec3 seen = d.conjugate().rotate({0, 1, 0});
    CHECK(std::fabs(std::asin(seen.z) / d2r) > 9.0f);
}

void test_body_frame_delta() {
    const float d2r = 0.0174533f;
    const Quat mount = Quat::from_axis_angle({0.3f, -0.8f, 0.5f}, 1.1f);  // any sensor mounting
    const Quat head_ref = Quat::from_axis_angle({0, 1, 0}, 20 * d2r) * Quat::from_axis_angle({1, 0, 0}, 8 * d2r);
    const Quat head_now = Quat::from_axis_angle({0, 0, 1}, 35 * d2r) * head_ref;  // turned 35 deg
    // Sensor orientation (sensor -> earth) from the head's: v_head = mount v_sensor.
    const auto sensor = [&](const Quat& head) { return head * mount; };
    const Quat d = body_frame_delta(sensor(head_ref), sensor(head_now), mount);
    const Quat level_ref = level_head(head_ref);  // roll taken out: the panels stand level
    for (const Vec3 x : {Vec3{1, 0, 0}, Vec3{0.7f, 0.5f, 0.3f}, Vec3{0.8f, -0.2f, -0.4f}}) {
        // x: a direction fixed in the room, given in the levelled head frame at the recenter.
        const Vec3 world = level_ref.rotate(x);
        const Vec3 truth = head_now.conjugate().rotate(world);
        const Vec3 shown = d.conjugate().rotate(x);
        CHECK(near(truth.x, shown.x, 1e-4f) && near(truth.y, shown.y, 1e-4f) && near(truth.z, shown.z, 1e-4f));
    }
}

// Recentered looking 15 deg down, then turned 40 deg about the room's vertical: with the level
// reference the head keeps its true pitch against the floor (panels on the horizon, the turn
// purely horizontal) and no roll comes from the recenter.
void test_level_reference_heading_only() {
    const float d2r = 0.0174533f;
    const Quat mount = Quat::from_axis_angle({0.3f, -0.8f, 0.5f}, 1.1f);
    const Quat pitch_down = Quat::from_axis_angle({0, -1, 0}, -15 * d2r);
    const Quat head_ref = Quat::from_axis_angle({0, 0, 1}, 10 * d2r) * pitch_down;
    const Quat head_now = Quat::from_axis_angle({0, 0, 1}, 40 * d2r) * head_ref;
    const Quat d = body_frame_delta(head_ref * mount, head_now * mount, mount, true);
    const Vec3 f = d.rotate({1, 0, 0});
    CHECK(near(std::asin(f.z) / d2r, -15.0f, 0.01f));                 // pitch against the floor
    CHECK(near(std::atan2(f.y, f.x) / d2r, 40.0f, 0.01f));            // the turn, all of it sideways
    const Vec3 left = d.rotate({0, 1, 0});
    CHECK(std::fabs(left.z) < 1e-4f);                                  // no roll from the recenter
}

// Locked by the rest detector, then a turn starts (accelerating to 60 deg/s in 0.1 s) while
// the detector still says rest: the lock lets go within a fraction of a degree, not after 1.
// Recentered with the head (or the calibration) rolled 12 deg and pitched down 10: right after
// the recenter the panels stand upright; turning 40 deg about the vertical keeps them so and
// sweeps them horizontally.
void test_recenter_takes_roll_as_upright() {
    const float d2r = 0.0174533f;
    const Quat mount = Quat::from_axis_angle({0.3f, -0.8f, 0.5f}, 1.1f);
    const Quat h0 = Quat::from_axis_angle({0, 0, 1}, 15 * d2r) * Quat::from_axis_angle({0, -1, 0}, -10 * d2r) *
                    Quat::from_axis_angle({1, 0, 0}, 12 * d2r);
    const Quat fix = head_roll(h0);
    const auto rel = [&](const Quat& h) {
        return (body_frame_delta(h0 * mount, h * mount, mount, true) * fix.conjugate()).normalized();
    };
    const Quat r0 = rel(h0);
    CHECK(std::fabs(r0.rotate({0, 1, 0}).z) < 1e-4f);                       // upright at the recenter
    CHECK(near(std::asin(r0.rotate({1, 0, 0}).z) / d2r, -10.0f, 0.05f));   // the pitch stays
    const Quat r1 = rel(Quat::from_axis_angle({0, 0, 1}, 40 * d2r) * h0);
    CHECK(std::fabs(r1.rotate({0, 1, 0}).z) < 1e-4f);                       // still upright after the turn
    const Vec3 f = r1.rotate({1, 0, 0});
    CHECK(near(std::atan2(f.y, f.x) / d2r, 40.0f, 0.05f) && near(std::asin(f.z) / d2r, -10.0f, 0.05f));
}

void test_rest_lock_releases_at_turn_start() {
    PoseStabilizerConfig cfg = stationary_lock_stabilizer();
    cfg.lock_on_rest = true;
    PoseStabilizer s(cfg);
    const float dt = kRefDt, d2r = 0.0174533f;
    s.set_rest(true);
    Quat q;
    for (int i = 0; i < 200; ++i) s.update(q, {0, 0, 0}, dt);
    float worst = 0.0f, rate = 0.0f;
    for (int i = 0; i < 46; ++i) {  // ~0.1 s, the detector not caught up yet
        rate = std::fmin(60.0f, rate + 60.0f / 46.0f);
        q = (Quat::from_axis_angle({0, 0, 1}, rate * d2r * dt) * q).normalized();
        const Quat out = s.update(q, {0, 0, rate * d2r}, dt);
        const Quat r = out.conjugate() * q;
        worst = std::fmax(worst, 2.0f * std::asin(std::fmin(1.0f, std::sqrt(r.x * r.x + r.y * r.y + r.z * r.z))) / d2r);
    }
    // Behind the head by at most the release offset plus the short smoothstep.
    CHECK(worst < 0.25f);  // was 1.1 deg waiting for the detector with a 40 ms ramp
}

void test_stabilizer_follows_turns() {
    PoseStabilizer s;
    const float dt = kRefDt, rate = 60.0f * 0.0174533f;
    Quat q, out;
    for (int i = 0; i < 457; ++i) s.update(q, {0, 0, 0}, dt);
    float worst = 0.0f;
    for (int i = 0; i < 457; ++i) {
        q = (Quat::from_axis_angle({0, 0, 1}, rate * dt) * q).normalized();
        out = s.update(q, {0, 0, rate}, dt);
        if (i > 45) worst = std::fmax(worst, angle_between_deg(out, q));  // after 0.1 s
    }
    CHECK(!s.holding());
    CHECK(worst < 0.6f);
    CHECK(prediction_gain(s.omega().norm()) == 1.0f);
    for (int i = 0; i < 457; ++i) out = s.update(q, {0, 0, 0}, dt);  // stop
    CHECK(s.holding());
    CHECK(angle_between_deg(out, q) < 0.2f);
}

// Slow drift (0.5 deg/s): nothing moves until the dead zone is left, then it catches up.
void test_stabilizer_dead_zone() {
    PoseStabilizer s;
    const float dt = kRefDt, rate = 0.5f * 0.0174533f;
    Quat q, out, first;
    for (int i = 0; i < 457 * 2; ++i) {  // 1 deg
        q = (Quat::from_axis_angle({0, 0, 1}, rate * dt) * q).normalized();
        out = s.update(q, {0, 0, rate}, dt);
        if (i == 0) first = out;
    }
    CHECK(angle_between_deg(out, first) == 0.0f);
    for (int i = 0; i < 457 * 2; ++i) {  // to 2 deg: crosses 1.5
        q = (Quat::from_axis_angle({0, 0, 1}, rate * dt) * q).normalized();
        out = s.update(q, {0, 0, rate}, dt);
    }
    CHECK(angle_between_deg(out, first) > 1.0f);
}

// Wider up / down than sideways: a 1.5 deg nod is held, a 1 deg turn is not.
void test_stabilizer_vertical_zone() {
    PoseStabilizerConfig c;
    c.enter_deg = 0.6f;
    c.enter_vertical_deg = 2.0f;
    const float dt = kRefDt, rate = 0.5f * 0.0174533f;
    for (int axis = 1; axis <= 2; ++axis) {  // 1: Y (up / down), 2: Z (left / right)
        PoseStabilizer s(c);
        const Vec3 a = axis == 1 ? Vec3{0, 1, 0} : Vec3{0, 0, 1};
        Quat q, out, first;
        for (int i = 0; i < 457 * 2; ++i) {  // to 1 deg
            q = (Quat::from_axis_angle(a, rate * dt) * q).normalized();
            out = s.update(q, a * rate, dt);
            if (i == 0) first = out;
        }
        if (axis == 1) {
            CHECK(angle_between_deg(out, first) == 0.0f);
        } else {
            CHECK(angle_between_deg(out, first) > 0.1f);
        }
    }
}

// Tremor damping: with the head nearly still and trembling, the panels' frame-to-frame
// unevenness (second difference of the error, per frame) falls by over half; turning, the
// world-lock error stays about the same.
float world_lock_jitter_deg(const PoseStabilizerConfig& c) {
    const float dt = kRefDt, d2r = 0.0174533f, pi = 3.14159265f, latency = 0.020f;
    const auto head = [&](float t) {
        const float yaw = 0.15f * d2r * std::sin(2 * pi * 8 * t), wy = 0.15f * d2r * 2 * pi * 8 * std::cos(2 * pi * 8 * t);
        return std::pair<Quat, Vec3>{Quat::from_axis_angle({0, 0, 1}, yaw), Vec3{0, 0, wy}};
    };
    PoseStabilizer s(c);
    double sum = 0;
    int n = 0;
    float e1 = 0, e2 = 0;
    for (int i = 0; i < static_cast<int>(10 / dt); ++i) {
        const float t = static_cast<float>(i) * dt;
        const auto [q, w] = head(t);
        const Quat out = s.update(q, w, dt);
        if (i % 4 != 0 || t < 2) continue;
        const Vec3 om = s.omega();
        const Quat shown = predict_orientation(out, om, latency * prediction_gain(om.norm(), kPredictStartRads, kPredictFullRads));
        const Quat er = head(t + latency).first.conjugate() * shown;
        const float e = 2.0f * er.z * 57.29578f * (er.w < 0 ? -1.0f : 1.0f);
        if (n++ >= 2) sum += std::pow(e - 2 * e1 + e2, 2.0f);
        e2 = e1;
        e1 = e;
    }
    return static_cast<float>(std::sqrt(sum / (n - 2)));
}

void test_stabilizer_tremor_damping() {
    const auto locked = world_locked_stabilizer();
    auto plain = locked;
    plain.reversal_decay_s = 0.0f;
    CHECK(world_lock_jitter_deg(locked) < 0.5f * world_lock_jitter_deg(plain));
    CHECK(world_lock_error_deg(locked, true) < 1.2f * world_lock_error_deg(plain, true));
}

// Stationary lock: a still head trembling stays frozen exactly; a turn is followed 1:1 within
// the 40 ms ramp, with its angular velocity to predict; still again for 150 ms: locked anew.
void test_stationary_lock() {
    PoseStabilizer s(stationary_lock_stabilizer());
    const float dt = kRefDt, d2r = 0.0174533f, pi = 3.14159265f;
    Quat first, out;
    float moved = 0.0f;
    for (int i = 0; i < 457 * 2; ++i) {  // tremor 0.04 deg at 8 Hz
        const float t = static_cast<float>(i) * dt;
        const float a = 0.04f * d2r * std::sin(2 * pi * 8 * t), w = 0.04f * d2r * 2 * pi * 8 * std::cos(2 * pi * 8 * t);
        out = s.update(Quat::from_axis_angle({0, 0, 1}, a), {0, 0, w}, dt);
        if (i == 0) first = out;
        moved = std::fmax(moved, angle_between_deg(out, first));
        CHECK(s.omega().norm() == 0.0f);
    }
    CHECK(moved == 0.0f && s.holding());
    float lag = 0.0f;
    Quat q;
    for (int i = 0; i < 457; ++i) {  // a 30 deg/s turn
        const float t = static_cast<float>(i) * dt;
        q = Quat::from_axis_angle({0, 0, 1}, t * 30 * d2r);
        out = s.update(q, {0, 0, 30 * d2r}, dt);
        if (t > 0.3f) lag = std::fmax(lag, angle_between_deg(out, q));
    }
    CHECK(lag < 1e-3f && !s.holding() && s.omega().norm() > 0.4f);
    for (int i = 0; i < 457; ++i) out = s.update(q, {0, 0, 0}, dt);  // still for 1 s (the 1.5 Hz speed settles in ~0.4 s, then 150 ms)
    CHECK(s.holding() && s.locked_seconds() > 0.2f && angle_between_deg(out, q) < 1e-3f);
}

// Vqf6D: a sensor at rest with a gyro bias detects rest and learns the bias; a 90 deg turn
// about the vertical integrates to 90 deg of heading; tilted, the inclination settles level.
void test_vqf() {
    const float dt = kRefDt, d2r = 0.0174533f;
    Vqf6D f;
    f.initialize_from_accel({0, 0, 9.81f});
    f.set_bias({0, 0, 0});
    const Vec3 bias_degs{0.4f, -0.3f, 0.2f};
    for (int i = 0; i < 457 * 25; ++i) f.update(bias_degs * d2r, {0, 0, 9.81f}, dt);
    CHECK(f.rest());
    CHECK((f.bias_degs() - bias_degs).norm() < 0.03f);
    // Settled, a slow head drift at rest (0.6 deg/s off the bias) is motion, not bias.
    Vqf6D h;
    h.initialize_from_accel({0, 0, 9.81f});
    for (int i = 0; i < 457 * 35; ++i) h.update({0, 0, 0}, {0, 0, 9.81f}, dt);
    CHECK(h.settled());
    for (int i = 0; i < 457 * 5; ++i) h.update(Vec3{0, 0, 0.6f} * d2r, {0, 0, 9.81f}, dt);
    CHECK(h.bias_degs().norm() < 0.01f);
    // Kalman: uncertain at first, sure once learned; a trusted start barely moves.
    CHECK(f.bias_variance().x < 0.01f);
    Vqf6D k;
    k.initialize_from_accel({0, 0, 9.81f});
    k.set_bias(bias_degs, 0.05f);
    for (int i = 0; i < 457 * 5; ++i) k.update((bias_degs + Vec3{0.1f, 0, 0}) * d2r, {0, 0, 9.81f}, dt);
    CHECK(k.bias_degs().x > bias_degs.x + 0.01f && k.bias_degs().x < bias_degs.x + 0.09f);
    const Quat before = f.quaternion();
    for (int i = 0; i < 457; ++i) f.update(Vec3{0, 0, 90 * d2r} + bias_degs * d2r, {0, 0, 9.81f}, dt);  // 1 s at 90 deg/s
    CHECK(std::fabs(angle_between_deg(f.quaternion(), before) - 90.0f) < 1.0f);
    // Level: a sensor pitched 10 deg reads gravity tilted; the estimate follows within seconds.
    Vqf6D g;
    g.initialize_from_accel({0, 0, 9.81f});
    const Quat tilt = Quat::from_axis_angle({0, 1, 0}, 10 * d2r);
    const Vec3 a = tilt.conjugate().rotate({0, 0, 9.81f});
    for (int i = 0; i < 457 * 20; ++i) g.update({0, 0, 0}, a, dt);
    const Vec3 up = g.quaternion().conjugate().rotate({0, 0, 1});
    CHECK(std::fabs(std::acos(std::fmin(1.0f, up.x * a.x / 9.81f + up.y * a.y / 9.81f + up.z * a.z / 9.81f))) < 0.5f * d2r);
}

// A good starting bias survives a noisy first sample (the bias average starts at the bias).
void test_vqf_start_keeps_good_bias() {
    const float dt = kRefDt, d2r = 0.0174533f;
    const Vec3 bias{0.37f, 0.40f, 0.01f};
    Vqf6D f;
    f.initialize_from_accel({0, 0, 9.81f});
    f.set_bias(bias);  // the start-up calibration's, uncertain (0.5 deg/s)
    f.update((bias + Vec3{-0.65f, 0.1f, -0.08f}) * d2r, {0, 0, 9.81f}, dt);  // one noisy sample first
    float worst = 0.0f;
    for (int i = 0; i < 457 * 3; ++i) {
        f.update(bias * d2r, {0, 0, 9.81f}, dt);
        worst = std::fmax(worst, (f.bias_degs() - bias).norm());
    }
    CHECK(worst < 0.03f);
}

void test_bias_convergence() {
    const float dt = kRefDt;
    BiasConvergenceConfig c;  // 4 windows of 0.5 s, 0.02 deg/s, 0.3 C
    // Steady at rest: settled after the 4 windows, not before.
    BiasConvergence a(c);
    int n = 0;
    while (!a.converged() && n < 457 * 10) { a.update({0.3f, 0.4f, 0.0f}, 33.0f, true, dt); ++n; }
    CHECK(a.converged() && n >= static_cast<int>(4 * 0.5f / dt) - 2 && n <= static_cast<int>(4 * 0.5f / dt) + 2);
    // Still converging (0.1 deg/s per second, 0.05 per window): never.
    BiasConvergence b(c);
    for (int i = 0; i < 457 * 10; ++i) b.update({0.1f * i * dt, 0, 0}, 33.0f, true, dt);
    CHECK(!b.converged());
    // Warming up fast (1 C/s): never.
    BiasConvergence t(c);
    for (int i = 0; i < 457 * 10; ++i) t.update({0.3f, 0, 0}, 33.0f + i * dt, true, dt);
    CHECK(!t.converged());
    // The head moving every other window: those windows do not count, the count restarts.
    BiasConvergence m(c);
    for (int w = 0; w < 20; ++w) {
        for (int i = 0; i < 229; ++i) m.update({0.3f, 0, 0}, 33.0f, w % 2 == 0, dt);
    }
    CHECK(!m.converged() && m.stable_windows() <= 1);
    // Configurable: one window of 0.2 s is enough.
    BiasConvergenceConfig quick;
    quick.windows = 1;
    quick.window_s = 0.2f;
    BiasConvergence q(quick);
    for (int i = 0; i < 100; ++i) q.update({0.3f, 0, 0}, 33.0f, true, dt);
    CHECK(q.converged());
}

// Start-up with VQF: a map entry within 1.5 C seeds the bias and tracking starts as before;
// without one, tracking waits for the bias to settle and the settled bias is learned as trusted.
void test_pipeline_bias_seed_or_settle() {
    ImuPipelineConfig cfg;
    cfg.use_vqf = true;
    cfg.hold_until_bias_settles = true;
    const Vec3 bias{0.3f, -0.2f, 0.1f};
    auto start = [&](ImuPipeline& p, float temp, int max_n) {
        uint32_t tick = 0;
        for (int i = 0; i < max_n; ++i) {
            tick += 22;
            auto r = make_imu_report({0, 0, 9.81f}, bias, tick);
            put_f32(&r[28], temp);  // sensor temperature
            if (p.process_report(r.data(), r.size())->tracking) return i;
        }
        return -1;
    };
    ImuPipelineConfig plain = cfg;
    plain.hold_until_bias_settles = false;
    ImuPipeline ref(plain);
    const int t_ref = start(ref, 33.0f, 457 * 30);
    CHECK(t_ref > 0);

    ImuPipeline cold(cfg);  // empty map: waits for the bias to settle (>= 4 windows of 0.5 s)
    const int t_cold = start(cold, 33.0f, 457 * 30);
    CHECK(t_cold >= t_ref + 457 * 2 - 10 && t_cold <= t_ref + 457 * 3);
    CHECK(cold.bias_settled() && !cold.bias_seeded_from_map());
    const auto learned = cold.bias_map().lookup(33.0f);  // trusted after one settled start
    CHECK(learned && ((*learned) - bias).norm() < 0.05f);

    ImuPipeline warm(cfg);  // the next start at the same temperature: seeded, no wait
    warm.set_bias_map(cold.bias_map());
    CHECK(start(warm, 33.4f, 457 * 30) == t_ref);
    CHECK(warm.bias_seeded_from_map());

    ImuPipeline far(cfg);  // learned 2 C away: beyond 1.5 C, not seeded
    far.set_bias_map(cold.bias_map());
    CHECK(start(far, 35.0f, 457 * 30) > t_ref + 457);
    CHECK(!far.bias_seeded_from_map());
}

// The map's linear trend over temperature: through the trusted bins, for temperatures with
// nothing learned near them; not from one bin, not from untrusted ones, not too far out.
void test_bias_map_trend() {
    BiasMap m;
    CHECK(!m.trend(30.0f));
    m.learn_settled(34.0f, {0.40f, 0.10f, 0.0f});
    CHECK(!m.trend(30.0f));  // one bin: no line
    m.learn_settled(38.0f, {0.20f, 0.10f, 0.0f});
    const auto t = m.trend(30.0f);  // -0.05 deg/s per C on X
    CHECK(t && near(t->x, 0.60f, 0.01f) && near(t->y, 0.10f, 0.01f));
    CHECK(!m.trend(20.0f));  // 14 C from the nearest learned
    BiasMap u;  // untrusted bins only
    for (int i = 0; i < 457 * 3; ++i) u.learn(34.0f, {0.4f, 0, 0}, kRefDt);
    for (int i = 0; i < 457 * 3; ++i) u.learn(38.0f, {0.2f, 0, 0}, kRefDt);
    CHECK(!u.trend(30.0f));
}

void test_bias_map() {
    BiasMap m;
    CHECK(m.empty() && !m.lookup(25.0f));
    for (int i = 0; i < 457 * 3; ++i) m.learn(24.0f, {0.2f, 0, 0}, kRefDt);
    CHECK(!m.lookup(24.0f));  // 3 s of rest: not trusted yet
    for (int i = 0; i < 457 * 30; ++i) m.learn(24.0f, {0.2f, 0, 0}, kRefDt);
    for (int i = 0; i < 457 * 30; ++i) m.learn(26.0f, {0.4f, 0, 0}, kRefDt);
    const auto mid = m.lookup(25.0f);
    CHECK(mid && near(mid->x, 0.3f, 0.01f));
    CHECK(!m.lookup(35.0f));  // too far from anything learned
    const auto back = BiasMap::from_json(m.to_json());
    CHECK(back && back->lookup(25.0f) && near(back->lookup(25.0f)->x, 0.3f, 0.01f));
}

void test_prediction_gain() {
    CHECK(prediction_gain(0.0f) == 0.0f && prediction_gain(0.05f) == 0.0f);
    CHECK(near(prediction_gain(0.175f), 0.5f, 1e-5f));
    CHECK(prediction_gain(0.3f) == 1.0f && prediction_gain(3.0f) == 1.0f);
}

// Recenter snaps the stabilized pose to identity instead of gliding there.
void test_pipeline_stabilized_recenter() {
    ImuPipeline p;
    uint32_t tick = 0;
    auto feed = [&](Vec3 g, int n) {
        Pose last;
        for (int i = 0; i < n; ++i) {
            tick += 22;
            const auto r = make_imu_report({0, 0, 9.81f}, g, tick);
            last = *p.process_report(r.data(), r.size());
        }
        return last;
    };
    feed({0, 0, 0}, 457 * 6);
    const Pose turned = feed({0, 0, 45.0f}, 457);
    CHECK(near(turned.euler.yaw, 45.0f, 1.5f));
    p.recenter();
    const Pose after = feed({0, 0, 0}, 2);
    CHECK(near(after.euler.yaw, 0.0f, 0.05f));
}

}  // namespace


// Shared presets: ids unique, cells normalized, lookup tolerant to offsets.
void test_presets() {
    const auto& l = layout_presets();
    CHECK(l.size() == 3);
    CHECK(layout_preset("dual") != nullptr && layout_preset("dual")->cells.size() == 2);
    CHECK(layout_preset("nope") == nullptr);
    const std::vector<GridCell> shifted = {{3, 2}, {4, 2}, {5, 2}};
    const LayoutPreset* t = find_layout_preset(shifted);
    CHECK(t != nullptr && t->id == "triple");
    CHECK(find_layout_preset({{0, 0}, {0, 1}}) == nullptr);  // stacked: no preset
    for (const auto& p : profiles()) CHECK(layout_preset(p.layout_id) != nullptr);
}

// Settings file: round trip, macOS key names, defaults for missing or broken values.
void test_settings_json() {
    Settings s;
    s.cells = {{0, 0}, {0, 1}, {1, 0}};
    s.text_size = 1;
    s.distance_m = 1.5f;
    s.stereo = true;
    s.dim_others = false;
    s.fov_y_deg = 23.5f;
    s.tracking = 2;
    s.panels = {{-10.5f, 3, 1.2f}, {25, 0, 0.8f}};
    s.glasses_mode = 2;
    s.brightness = 5;
    s.tilt = 0.4f;
    s.anchor_at_rest = true;
    s.mac_screen = false;
    s.depth3d = true;
    s.depth_strength = 0.02f;
    s.depth_plane = 0.5f;
    s.background = 5;
    s.background_image = "C:\\Users\\Ana \"x\"\\fondo.jpg";
    s.background_brightness = 0.4f;
    const std::string json = settings_to_json(s);
    CHECK(json.find("\"layoutCells\"") != std::string::npos && json.find("\"fovY\": 23.5") != std::string::npos);
    const auto back = settings_from_json(json);
    CHECK(back.has_value());
    CHECK(back->cells.size() == 3 && back->cells[1].col == 0 && back->cells[1].row == 1);
    CHECK(back->text_size == 1 && near(back->distance_m, 1.5f, 1e-6f) && back->stereo && !back->dim_others);
    CHECK(near(back->fov_y_deg, 23.5f, 1e-6f) && back->gaze_cursor && back->tracking == 2);
    CHECK(back->panels.size() == 2 && near(back->panels[0].yaw_deg, -10.5f, 1e-5f) &&
          near(back->panels[1].scale, 0.8f, 1e-6f));
    CHECK(settings_from_json("{}")->panels.empty());  // default: the grid
    CHECK(back->background == 5 && back->background_image == s.background_image && near(back->background_brightness, 0.4f, 1e-6f));
    CHECK(back->glasses_mode == 2 && back->brightness == 5);
    CHECK(near(back->tilt, 0.4f, 1e-6f) && back->anchor_at_rest && !back->mac_screen);
    CHECK(near(settings_from_json("{}")->tilt, 1.0f, 1e-6f) && settings_from_json("{}")->mac_screen);
    CHECK(back->depth3d && near(back->depth_strength, 0.02f, 1e-6f) && near(back->depth_plane, 0.5f, 1e-6f));
    CHECK(!settings_from_json("{}")->depth3d && near(settings_from_json("{}")->depth_strength, 0.03f, 1e-6f));
    CHECK(near(settings_from_json(R"({"depthStrength": 0.015})")->depth_strength, 0.03f, 1e-6f));  // first default moved
    CHECK(settings_from_json(R"({"glassesPinned": true})")->glasses_mode == 1);  // earlier builds' switch
    CHECK(settings_from_json("{}")->tracking == 1);                    // default: left/right only
    CHECK(settings_from_json(R"({"yawOnly": false})")->tracking == 0);  // earlier builds' switch

    const auto partial = settings_from_json(R"({"distance": 3, "unknown": {"a": [1, {"b": "}"}]}, "stereo": true})");
    CHECK(partial && near(partial->distance_m, 3.0f, 1e-6f) && partial->stereo && partial->cells.size() == 2);
    const auto wrong_type = settings_from_json(R"({"distance": "far", "ipd": 60})");
    CHECK(wrong_type && near(wrong_type->distance_m, 2.0f, 1e-6f) && near(wrong_type->ipd_mm, 60.0f, 1e-6f));
    const auto bad_cells = settings_from_json(R"({"layoutCells": [], "gap": 2})");
    CHECK(bad_cells && bad_cells->cells.size() == 2 && near(bad_cells->gap_deg, 2.0f, 1e-6f));
    CHECK(!settings_from_json("not json").has_value());
}

// Dimmer: looked-at panel stays lit, others ease to dim in ~5 tau, last panel kept between panels.
void test_panel_dimmer() {
    PanelDimmer d(0.15f);
    d.update(3, 1, 0.5f, 0.0f);
    CHECK(d.brightness(0) == 1.0f && d.animating());
    for (int i = 0; i < 120; ++i) d.update(3, 1, 0.5f, 1.0f / 120.0f);  // 1 s
    CHECK(d.brightness(1) == 1.0f && d.brightness(0) == 0.5f && d.brightness(2) == 0.5f);
    CHECK(!d.animating());
    d.update(3, -1, 0.5f, 1.0f / 120.0f);  // between panels: unchanged
    CHECK(d.lit_panel() == 1 && !d.animating());
    d.update(3, 2, 0.5f, 1.0f / 120.0f);
    CHECK(d.animating() && d.brightness(2) > 0.5f && d.brightness(1) < 1.0f);
    PanelDimmer one;
    one.update(1, -1, 0.3f, 0.1f);  // a single panel is never dimmed
    CHECK(one.brightness(0) == 1.0f && !one.animating());
}

// Skipper: skips only when nothing that affects the image changed.
void test_frame_skipper() {
    FrameSkipper s;
    FrameSkipper::Input in;
    in.tracking = true;
    CHECK(!s.skip(in));  // first frame is always drawn
    CHECK(s.skip(in));
    in.source_generation = 1;
    CHECK(!s.skip(in));
    CHECK(s.skip(in));
    in.orientation = Quat::from_axis_angle({0, 0, 1}, 0.001f);
    CHECK(!s.skip(in));
    in.angular_speed_rads = 0.5f;
    CHECK(!s.skip(in));
    in.angular_speed_rads = 0.0f;
    in.animating = true;
    CHECK(!s.skip(in));
    in.animating = false;
    in.forced = true;
    CHECK(!s.skip(in));
    in.forced = false;
    CHECK(s.skip(in));
    s.invalidate();
    CHECK(!s.skip(in));
}

void test_imu_recording_roundtrip() {
    std::vector<uint8_t> buf;
    append_imu_recording_header(buf);
    uint8_t a[64], b[2] = {0x99, 0xc8};
    for (int i = 0; i < 64; ++i) a[i] = static_cast<uint8_t>(i * 3);
    append_imu_record(buf, 0x0123456789abcdefull, a, sizeof(a));
    append_imu_record(buf, 42, b, sizeof(b));
    auto r = parse_imu_recording(buf.data(), buf.size());
    CHECK(r && r->size() == 2);
    // A recentre marker: a record without bytes.
    std::vector<uint8_t> m;
    append_imu_recording_header(m);
    append_imu_record(m, 7, nullptr, 0);
    const auto mr = parse_imu_recording(m.data(), m.size());
    CHECK(mr && mr->size() == 1 && (*mr)[0].length == 0 && (*mr)[0].host_ns == 7);
    if (r && r->size() == 2) {
        CHECK((*r)[0].host_ns == 0x0123456789abcdefull && (*r)[0].length == 64 && (*r)[0].data[63] == 189);
        CHECK((*r)[1].host_ns == 42 && (*r)[1].length == 2 && (*r)[1].data[1] == 0xc8);
    }
    // Cut short in the last report: the complete ones survive.
    auto cut = parse_imu_recording(buf.data(), buf.size() - 1);
    CHECK(cut && cut->size() == 1);
    // Not a recording.
    buf[0] = 'X';
    CHECK(!parse_imu_recording(buf.data(), buf.size()));
}

void test_latch_cap() {
    CHECK(near(latch_cap_ms(1000.0f / 120), 4.0f, 1e-4f));  // 4.17 ms half period: 4 ms
    CHECK(near(latch_cap_ms(1000.0f / 60), 4.0f, 1e-4f));
    CHECK(near(latch_cap_ms(1000.0f / 240), 2.0833f, 1e-3f));  // half a period
    CHECK(latch_start(1000.0f / 120).margin_ms <= latch_cap_ms(1000.0f / 120));
}

void test_latch_compositor_misses_keep_margin() {
    const float period = 1000.0f / 120;
    LatchState s = latch_start(period);
    const float m = s.margin_ms;
    // Every refresh missed but the commit was early (0.6 ms of CPU): the compositor's.
    for (int i = 0; i < 1000; ++i) s = latch_presented(s, true, 0.6f, period);
    CHECK(near(s.margin_ms, m, 1e-6f));
    CHECK(!latch_caused_miss(s, 0.6f));
}

void test_latch_cpu_misses_grow_to_cap() {
    for (const float hz : {60.0f, 120.0f}) {
        const float period = 1000.0f / hz;
        LatchState s = latch_start(period);
        const float m = s.margin_ms;
        // Committed later than margin - 1 ms: the latch read too late.
        s = latch_presented(s, true, m - 0.5f, period);
        CHECK(s.margin_ms > m);
        for (int i = 0; i < 100; ++i) s = latch_presented(s, true, 10.0f, period);
        CHECK(near(s.margin_ms, latch_cap_ms(period), 1e-6f));
        CHECK(s.margin_ms <= 0.5f * period + 1e-6f && s.margin_ms <= 4.0f + 1e-6f);
    }
}

void test_latch_shrinks_gradually() {
    const float period = 1000.0f / 120;
    LatchState s{4.0f, 0};
    for (int i = 0; i < 239; ++i) s = latch_presented(s, false, 1.0f, period);
    CHECK(near(s.margin_ms, 4.0f, 1e-6f));  // not before 240 frames on time
    s = latch_presented(s, false, 1.0f, period);
    CHECK(near(s.margin_ms, 3.75f, 1e-6f));
    for (int i = 0; i < 240 * 100; ++i) s = latch_presented(s, false, 1.0f, period);
    CHECK(near(s.margin_ms, 1.0f, 1e-6f));  // floor
    // A compositor miss does not restart the count; a frame on time keeps it going.
    LatchState t{3.0f, 200};
    t = latch_presented(t, true, 0.5f, period);
    CHECK(t.on_time == 200);
}

void test_latch_period_change_reclamps() {
    // 60 Hz margin of 4 ms, then the display goes to 240 Hz: re-clamped to half a period.
    LatchState s{4.0f, 0};
    s = latch_presented(s, false, 0.5f, 1000.0f / 240);
    CHECK(near(s.margin_ms, latch_cap_ms(1000.0f / 240), 1e-6f));
}

void test_fixed_prediction_horizon() {
    const float p120 = 1000.0f / 120, p60 = 1000.0f / 60;
    CHECK(near(fixed_prediction_horizon_ms(16.0f, 0.0f, p120), 16.0f, 1e-5f));     // on time: the tuned 16 ms
    CHECK(near(fixed_prediction_horizon_ms(16.0f, 3.5f, p120), 19.5f, 1e-5f));     // the measured lateness added
    CHECK(near(fixed_prediction_horizon_ms(16.0f, 20.0f, p120), 16.0f + p120, 1e-4f));  // at most a period
    CHECK(near(fixed_prediction_horizon_ms(16.0f, 20.0f, p60), 16.0f + p60, 1e-4f));
    CHECK(near(fixed_prediction_horizon_ms(16.0f, -4.0f, p120), 16.0f, 1e-5f));    // early: never less
    CHECK(near(fixed_prediction_horizon_ms(16.0f, std::nanf(""), p120), 16.0f, 1e-5f));
    // The sample's age at the pose read is added, at most a period.
    CHECK(near(fixed_prediction_horizon_ms(16.0f, 0.0f, p120, 1.4f), 17.4f, 1e-5f));
    CHECK(near(fixed_prediction_horizon_ms(16.0f, 2.0f, p120, 1.0f), 19.0f, 1e-5f));
    CHECK(near(fixed_prediction_horizon_ms(16.0f, 0.0f, p120, 500.0f), 16.0f + p120, 1e-4f));
    CHECK(near(fixed_prediction_horizon_ms(16.0f, 0.0f, p120, -1.0f), 16.0f, 1e-5f));
}

void test_stream_watchdog() {
    const uint64_t ms = 1000000ull;
    StreamWatchdog w;
    w.start(0);
    // Reports every 2.2 ms: never stale, nothing sent.
    uint64_t t = 0;
    for (int i = 0; i < 1000; ++i) {
        t += 22 * ms / 10;
        w.report(t);
        CHECK(!w.should_resend(t) && !w.stale(t));
    }
    // A 1 s stall: stale after 0.5 s, stream-on at 0.5 s and again every 0.5 s, not more often.
    const uint64_t last = t;
    CHECK(!w.stale(last + 499 * ms) && !w.should_resend(last + 499 * ms));
    CHECK(w.stale(last + 501 * ms) && w.should_resend(last + 501 * ms));
    CHECK(!w.should_resend(last + 700 * ms));
    CHECK(w.should_resend(last + 1002 * ms));
    CHECK(w.resends() == 2);
    // Samples again: recovered, the stall's length read once.
    w.report(last + 1000 * ms + 50 * ms);
    CHECK(!w.stale(last + 1060 * ms));
    CHECK(w.take_recovered_ns() == 1050 * ms && w.take_recovered_ns() == 0);
    // The free check the engine uses for the pose.
    CHECK(!pose_stale(100 * ms, 600 * ms) && pose_stale(100 * ms, 601 * ms) && !pose_stale(700 * ms, 600 * ms));
    // A stall at start (stream-on not taken): the first resend after 0.5 s too.
    StreamWatchdog s;
    s.start(10 * ms);
    CHECK(!s.should_resend(400 * ms) && s.should_resend(520 * ms));
}

// The recentre button takes the head's elevation too: the panels are lifted there (eased in),
// while the reference stays level (turning still sweeps them level). The start keeps 0.
void test_recenter_lifts_panels_to_gaze() {
    ImuPipelineConfig cfg = real_size_pipeline_config();
    cfg.hold_until_bias_settles = false;
    ImuPipeline p(cfg);
    uint32_t tick = 0;
    Vec3 accel{0, 0, 9.81f};
    auto feed = [&](Vec3 g, int n) {
        Pose last;
        for (int i = 0; i < n; ++i) {
            tick += 22;
            const auto r = make_imu_report(accel, g, tick);
            last = *p.process_report(r.data(), r.size());
        }
        return last;
    };
    Pose start = feed({0, 0, 0}, 457 * 8);
    CHECK(start.tracking && near(start.panels_pitch_rad, 0.0f, 1e-4f));
    // Nod 20 deg about the sensor's Y (identity mounting): the recentre lifts the panels to where
    // the head then looks, whichever way that is.
    const Pose nodded = feed({0, 20.0f, 0}, 457);
    const float gaze = gaze_placement(nodded.relative).pitch_rad;
    CHECK(std::fabs(gaze) > 0.25f);
    // Gravity seen tilted now (the head stays nodded).
    const Quat nod = Quat::from_axis_angle({0, 1, 0}, 20.0f * 0.0174533f);
    accel = nod.conjugate().rotate({0, 0, 9.81f});
    feed({0, 0, 0}, 457);
    p.recenter(true);
    const Pose mid = feed({0, 0, 0}, 40);  // glide under way (0.3 s)
    const Pose after = feed({0, 0, 0}, 457);
    CHECK(near(after.panels_pitch_rad, gaze, 0.05f));
    CHECK(std::fabs(mid.panels_pitch_rad) < std::fabs(after.panels_pitch_rad));  // eased in
    // Looking straight at the lifted panels: the relative pose itself keeps the nod (level reference).
    CHECK(near(gaze_placement(after.relative).pitch_rad, gaze, 0.05f));
    // lift_placements moves every panel by the same elevation.
    std::vector<PanelPlacement> pl{{0.0f, 0.0f, 1.0f}, {0.5f, 0.3f, 1.0f}};
    lift_placements(pl, -0.2f);
    CHECK(near(pl[0].pitch_rad, -0.2f, 1e-6f) && near(pl[1].pitch_rad, 0.1f, 1e-6f) && near(pl[1].yaw_rad, 0.5f, 1e-6f));
}

// Cold sensor: the gyro rest threshold grows below 30 C (x2 at 22 C and below); warm or unknown
// temperature: unchanged. A cold head resting with noise above the warm threshold still rests.
void test_vqf_cold_rest_threshold() {
    Vqf6D f;
    CHECK(near(f.rest_gyro_threshold(), 1.5f, 1e-6f));  // temperature unknown
    f.set_temperature(35.0f);
    CHECK(near(f.rest_gyro_threshold(), 1.5f, 1e-6f));
    f.set_temperature(26.0f);
    CHECK(near(f.rest_gyro_threshold(), 2.25f, 1e-5f));
    f.set_temperature(15.0f);
    CHECK(near(f.rest_gyro_threshold(), 3.0f, 1e-5f));
    // 2 deg/s of slow wander around the bias (low-passed it stays above 1.5): warm, no rest;
    // cold, rest.
    const float dt = kRefDt, d2r = 0.0174533f;
    auto rests = [&](float temp) {
        Vqf6D v;
        v.initialize_from_accel({0, 0, 9.81f});
        v.set_temperature(temp);
        int rest = 0;
        for (int i = 0; i < 457 * 4; ++i) {
            const float w = 2.0f * std::sin(2.0f * 3.14159f * 0.5f * i * dt);
            v.update(Vec3{w, 0, 0} * d2r, {0, 0, 9.81f}, dt);
            rest += v.rest() ? 1 : 0;
        }
        return rest;
    };
    CHECK(rests(35.0f) < rests(18.0f));
}

void test_gyro_noise_monitor() {
    const float dt = kRefDt;
    uint64_t rng = 1;
    auto noise = [&](float sigma) {  // roughly gaussian (sum of uniforms)
        float s = 0;
        for (int k = 0; k < 6; ++k) {
            rng = rng * 6364136223846793005ull + 1442695040888963407ull;
            s += static_cast<float>(rng >> 40) / static_cast<float>(1 << 24) - 0.5f;
        }
        return s * sigma;  // 6 uniforms of var 1/12: sigma
    };
    // Normal sensor (0.1 deg/s per axis) at rest: measured close to it, not noisy.
    GyroNoiseMonitor ok;
    for (int i = 0; i < 457 * 12; ++i) ok.update(Vec3{0.4f + noise(0.1f), 0.4f + noise(0.1f), noise(0.1f)}, true, dt);
    CHECK(ok.windows_measured() >= 5 && near(ok.noise_degs(), 0.1f, 0.03f) && !ok.noisy());
    // 1 deg/s of white noise: noisy after 5 rest windows (10 s), not before.
    GyroNoiseMonitor bad;
    for (int i = 0; i < 457 * 8; ++i) bad.update(Vec3{noise(1.0f), noise(1.0f), noise(1.0f)}, true, dt);
    CHECK(!bad.noisy());
    for (int i = 0; i < 457 * 3; ++i) bad.update(Vec3{noise(1.0f), noise(1.0f), noise(1.0f)}, true, dt);
    CHECK(bad.noisy());
    // A slow turn (tremor, head motion) with normal noise is not white noise: never noisy.
    GyroNoiseMonitor slow;
    for (int i = 0; i < 457 * 12; ++i) {
        const float w = 3.0f * std::sin(2.0f * 3.14159f * 2.0f * i * dt);
        slow.update(Vec3{w + noise(0.1f), noise(0.1f), noise(0.1f)}, true, dt);
    }
    CHECK(!slow.noisy());
    // Motion in a window: it does not count.
    GyroNoiseMonitor moving;
    for (int i = 0; i < 457 * 12; ++i) moving.update(Vec3{noise(1.0f), 0, 0}, i % 400 != 0, dt);
    CHECK(moving.windows_measured() == 0);
}

// Vehicle mode: a quarter of the heading, pitch untouched; the setting round-trips.
void test_vehicle_mode() {
    const float d2r = 0.0174533f;
    const Quat yaw40 = Quat::from_axis_angle({0, 0, 1}, 40 * d2r);
    const Quat pitch10 = Quat::from_axis_angle({0, -1, 0}, 10 * d2r);  // looking up (core frame)
    const Quat head = yaw40 * pitch10;
    const Quat v = scale_heading(head, kVehicleHeadingScale);
    const Vec3 f = v.rotate({1, 0, 0});
    CHECK(near(std::atan2(f.y, f.x) / d2r, 10.0f, 0.05f));  // 40 -> 10 deg
    const Vec3 f0 = head.rotate({1, 0, 0});
    CHECK(near(f.z, f0.z, 1e-4f));                           // same elevation
    CHECK(angle_between_deg(scale_heading(head, 1.0f), head) < 0.01f);
    Settings st;
    st.vehicle_mode = true;
    const auto back = settings_from_json(settings_to_json(st));
    CHECK(back && back->vehicle_mode);
    CHECK(settings_from_json("{}") && !settings_from_json("{}")->vehicle_mode);
}

int main() {
    test_build_command();
    test_build_command_value();
    test_decode_imu();
    test_decode_ack_and_garbage();
    test_tick_wrap();
    test_madgwick_level_stable();
    test_madgwick_init_from_tilt();
    test_madgwick_yaw_integration();
    test_pipeline_bias_removed();
    test_pipeline_recenter();
    test_pipeline_recenter_glide();
    test_presets();
    test_settings_json();
    test_panel_dimmer();
    test_frame_skipper();
    test_tick_monitor_reference_cadence();
    test_tick_monitor_gaps();
    test_bias_rejects_transient_step();
    test_bias_rejects_noise_burst();
    test_bias_rejects_out_of_clamp_step();
    test_bias_escape_adopts_persistent_step();
    test_bias_escape_rollback();
    test_bias_worn_start();
    test_bias_worn_start_median();
    test_bias_worn_drift_correction();
    test_bias_worn_ignores_head_motion();
    test_slerp();
    test_stabilizer_holds_still();
    test_stabilizer_world_locked();
    test_body_frame_delta();
    test_level_reference_heading_only();
    test_rest_lock_releases_at_turn_start();
    test_recenter_takes_roll_as_upright();
    test_level_reference();
    test_world_locked_madgwick();
    test_world_locked_anchor();
    test_stabilizer_follows_turns();
    test_stabilizer_dead_zone();
    test_stabilizer_vertical_zone();
    test_stabilizer_tremor_damping();
    test_stationary_lock();
    test_vqf();
    test_bias_map();
    test_vqf_start_keeps_good_bias();
    test_bias_convergence();
    test_bias_map_trend();
    test_pipeline_bias_seed_or_settle();
    test_prediction_gain();
    test_yaw_only();
    test_panel_poses();
    test_separate_floating();
    test_place_pitched();
    test_track_no_roll();
    test_neck_model();
    test_pinned_zoom();
    test_pipeline_stabilized_recenter();
    test_head_calibration_solves_mounting();
    test_head_calibration_rejects_bad_input();
    test_head_calibration_tolerates_turn_in_nods();
    test_calibration_json_roundtrip();
    test_hold_detector();
    test_pipeline_applies_sensor_to_head();
    test_core_to_render_axes();
    test_render_rotation();
    test_view_and_projection();
    test_predict_orientation();
    test_curved_screen_mesh();
    test_barrel_distort();
    test_panel_layout();
    test_flat_panels();
    test_panel_at_gaze();
    test_fit_panels();
    test_grid_layout();
    test_bent_panels();
    test_imu_recording_roundtrip();
    test_latch_cap();
    test_latch_compositor_misses_keep_margin();
    test_latch_cpu_misses_grow_to_cap();
    test_latch_shrinks_gradually();
    test_latch_period_change_reclamps();
    test_fixed_prediction_horizon();
    test_stream_watchdog();
    test_recenter_lifts_panels_to_gaze();
    test_vqf_cold_rest_threshold();
    test_gyro_noise_monitor();
    test_vehicle_mode();
    if (g_failures == 0) std::printf("all core tests passed\n");
    return g_failures == 0 ? 0 : 1;
}
