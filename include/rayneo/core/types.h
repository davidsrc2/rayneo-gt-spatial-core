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
    constexpr float dot(const Vec3& o) const { return x * o.x + y * o.y + z * o.z; }
    constexpr Vec3 cross(const Vec3& o) const {
        return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x};
    }
    float norm() const { return std::sqrt(x * x + y * y + z * z); }
    Vec3 normalized() const {
        const float n = norm();
        return n > 0.0f ? Vec3{x / n, y / n, z / n} : Vec3{};
    }
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

    // q * v * q^-1 for a unit quaternion.
    Vec3 rotate(const Vec3& v) const {
        const Quat r = *this * Quat{0.0f, v.x, v.y, v.z} * conjugate();
        return {r.x, r.y, r.z};
    }

    static Quat from_axis_angle(const Vec3& axis, float rad) {
        const Vec3 a = axis.normalized();
        const float s = std::sin(rad * 0.5f);
        return Quat{std::cos(rad * 0.5f), a.x * s, a.y * s, a.z * s};
    }

    // Rotation whose matrix rows are the target frame's axes expressed in the source frame
    // (v_target = R * v_source). Rows must be orthonormal and right-handed.
    static Quat from_rows(const Vec3& r0, const Vec3& r1, const Vec3& r2);
};

// Head frame and earth frame share the convention: X forward, Y left, Z up (right-handed).
inline Quat Quat::from_rows(const Vec3& r0, const Vec3& r1, const Vec3& r2) {
    // Shepperd's method on R = [r0; r1; r2].
    const float m00 = r0.x, m01 = r0.y, m02 = r0.z;
    const float m10 = r1.x, m11 = r1.y, m12 = r1.z;
    const float m20 = r2.x, m21 = r2.y, m22 = r2.z;
    const float tr = m00 + m11 + m22;
    Quat q;
    if (tr > 0.0f) {
        const float s = std::sqrt(tr + 1.0f) * 2.0f;
        q = {0.25f * s, (m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s};
    } else if (m00 > m11 && m00 > m22) {
        const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
        q = {(m21 - m12) / s, 0.25f * s, (m01 + m10) / s, (m02 + m20) / s};
    } else if (m11 > m22) {
        const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
        q = {(m02 - m20) / s, (m01 + m10) / s, 0.25f * s, (m12 + m21) / s};
    } else {
        const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
        q = {(m10 - m01) / s, (m02 + m20) / s, (m12 + m21) / s, 0.25f * s};
    }
    if (q.w < 0.0f) q = {-q.w, -q.x, -q.y, -q.z};
    return q.normalized();
}

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
    // Head rotation since recenter, earth-frame delta (q * q_ref^-1) re-expressed in the
    // level frame whose X is the head's forward heading at recenter. Identity at recenter.
    Quat relative;
    EulerDeg euler;       // diagnostics only, never drive rendering from it
    Vec3 gyro_bias_degs;
    Vec3 gyro_corrected_degs;  // sensor frame, bias removed (calibration / diagnostics)
    Vec3 angular_velocity_rads;  // in the same frame as `relative` (for pose prediction)
    Vec3 accel_mps2;           // sensor frame
    uint32_t tick_100us = 0;
    bool tracking = false;  // false until the startup bias calibration completes
};

}  // namespace rayneo::core
