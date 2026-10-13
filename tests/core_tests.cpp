// Core engine tests. No hardware, no hidapi: the core must build and pass on any host.

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "rayneo/core/gt_protocol.h"
#include "rayneo/core/gyro_bias_estimator.h"
#include "rayneo/core/head_calibration.h"
#include "rayneo/core/imu_pipeline.h"
#include "rayneo/core/madgwick.h"
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
    }
}

}  // namespace

int main() {
    test_build_command();
    test_decode_imu();
    test_decode_ack_and_garbage();
    test_tick_wrap();
    test_madgwick_level_stable();
    test_madgwick_init_from_tilt();
    test_madgwick_yaw_integration();
    test_pipeline_bias_removed();
    test_pipeline_recenter();
    test_tick_monitor_reference_cadence();
    test_tick_monitor_gaps();
    test_bias_rejects_transient_step();
    test_bias_rejects_noise_burst();
    test_bias_rejects_out_of_clamp_step();
    test_bias_escape_adopts_persistent_step();
    test_bias_escape_rollback();
    test_head_calibration_solves_mounting();
    test_head_calibration_rejects_bad_input();
    test_head_calibration_tolerates_turn_in_nods();
    test_calibration_json_roundtrip();
    test_hold_detector();
    test_pipeline_applies_sensor_to_head();
    if (g_failures == 0) std::printf("all core tests passed\n");
    return g_failures == 0 ? 0 : 1;
}
