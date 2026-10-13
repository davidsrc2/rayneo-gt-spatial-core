#pragma once

#include <cmath>
#include <cstdint>

namespace rayneo::core {

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    constexpr Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    constexpr Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    constexpr Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    float norm() const { return std::sqrt(x * x + y * y + z * z); }
};

// Hamilton convention, w + xi + yj + zk. Sensor -> earth rotation, earth frame Z-up.
struct Quat {
    float w = 1.0f;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    constexpr Quat conjugate() const { return {w, -x, -y, -z}; }
    constexpr Quat operator*(const Quat& b) const {
        return {w * b.w - x * b.x - y * b.y - z * b.z,
                w * b.x + x * b.w + y * b.z - z * b.y,
                w * b.y - x * b.z + y * b.w + z * b.x,
                w * b.z + x * b.y - y * b.x + z * b.w};
    }
    Quat normalized() const {
        const float n = std::sqrt(w * w + x * x + y * y + z * z);
        return n > 0.0f ? Quat{w / n, x / n, y / n, z / n} : Quat{};
    }
};

// Raw, unit-converted sample decoded from a 0x99 0x65 report.
struct ImuSample {
    Vec3 accel_mps2;
    Vec3 gyro_degs;
    Vec3 mag_ut;
    float temp_c = 0.0f;
    uint32_t tick_100us = 0;
};

struct EulerDeg {
    float yaw = 0.0f;
    float pitch = 0.0f;
    float roll = 0.0f;
};

// Output of the engine: stabilized orientation, ready for the OS layer.
struct Pose {
    Quat absolute;        // sensor -> earth
    Quat relative;        // q * q_ref^-1, earth frame (see docs/ARCHITECTURE.md)
    EulerDeg euler;       // diagnostics only, never drive rendering from it
    Vec3 gyro_bias_degs;
    uint32_t tick_100us = 0;
    bool tracking = false;  // false until the startup bias calibration completes
};

}  // namespace rayneo::core
