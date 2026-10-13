#include "rayneo/core/gt_protocol.h"

#include <cmath>
#include <cstring>

namespace rayneo::core::gt {
namespace {

constexpr uint8_t kTagIn = 0x99;
constexpr uint8_t kTagAck = 0xC8;
constexpr uint8_t kTagImu = 0x65;

// Offsets inside the 64-byte 0x99 0x65 report.
constexpr size_t kOffAccel = 4;
constexpr size_t kOffGyro = 16;
constexpr size_t kOffTemp = 28;
constexpr size_t kOffMagX = 32;
constexpr size_t kOffMagY = 36;
constexpr size_t kOffTick = 40;
constexpr size_t kOffMagZ = 52;
constexpr size_t kOffAckCmd = 8;

static_assert(sizeof(float) == 4, "float32 required");

float read_f32(const uint8_t* p) {
    float v;
    std::memcpy(&v, p, sizeof(v));  // Wire is LE; arm64 and x86-64 are LE.
    return v;
}

uint32_t read_u32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

Vec3 read_vec3(const uint8_t* p) { return {read_f32(p), read_f32(p + 4), read_f32(p + 8)}; }

bool finite(const Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

}  // namespace

CommandFrame build_command(uint8_t cmd) {
    CommandFrame f{};
    f[0] = 0x00;
    f[1] = kCmdTag;
    f[2] = cmd;
    return f;
}

Report decode_report(const uint8_t* data, size_t length) {
    Report r;
    if (data == nullptr) return r;

    if (length >= 2 && data[0] == 0x00 && data[1] == kTagIn) {
        ++data;
        --length;
    }
    if (length < 4 || data[0] != kTagIn) return r;

    r.board_id = data[2];

    if (data[1] == kTagAck) {
        r.kind = ReportKind::Ack;
        r.ack_cmd = length > kOffAckCmd ? data[kOffAckCmd] : 0;
        return r;
    }

    if (data[1] == kTagImu && length >= kReportSize) {
        ImuSample& s = r.imu;
        s.accel_mps2 = read_vec3(data + kOffAccel);
        s.gyro_degs = read_vec3(data + kOffGyro);
        s.temp_c = read_f32(data + kOffTemp);
        s.mag_ut = {read_f32(data + kOffMagX), read_f32(data + kOffMagY), read_f32(data + kOffMagZ)};
        s.tick_100us = read_u32(data + kOffTick);
        if (finite(s.accel_mps2) && finite(s.gyro_degs) && finite(s.mag_ut) && std::isfinite(s.temp_c)) {
            r.kind = ReportKind::Imu;
        }
    }
    return r;
}

double tick_delta_seconds(uint32_t prev, uint32_t now) {
    return static_cast<double>(static_cast<uint32_t>(now - prev)) * 100e-6;
}

}  // namespace rayneo::core::gt
