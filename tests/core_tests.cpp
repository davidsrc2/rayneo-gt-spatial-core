// Core engine tests. No hardware, no hidapi: the core must build and pass on any host.

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "rayneo/core/gt_protocol.h"
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
    if (g_failures == 0) std::printf("all core tests passed\n");
    return g_failures == 0 ? 0 : 1;
}
