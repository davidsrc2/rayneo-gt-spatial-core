#include "rayneo/core/madgwick.h"

#include <cmath>

namespace rayneo::core {
namespace {

constexpr float kRadToDeg = 57.29577951308232f;

float inv_sqrt(float v) { return 1.0f / std::sqrt(v); }

}  // namespace

void MadgwickFilter::initialize_from_accel(const Vec3& a) {
    const float n = a.norm();
    if (n < 1e-6f) {
        q_ = Quat{};
        return;
    }
    const float ax = a.x / n, ay = a.y / n, az = a.z / n;
    const float roll = std::atan2(ay, az);
    const float pitch = std::atan2(-ax, std::sqrt(ay * ay + az * az));
    const float cr = std::cos(roll * 0.5f), sr = std::sin(roll * 0.5f);
    const float cp = std::cos(pitch * 0.5f), sp = std::sin(pitch * 0.5f);
    q_ = Quat{cr * cp, sr * cp, cr * sp, -sr * sp}.normalized();
}

void MadgwickFilter::update_imu(const Vec3& g, const Vec3& a, float dt) {
    float q0 = q_.w, q1 = q_.x, q2 = q_.y, q3 = q_.z;

    float qDot1 = 0.5f * (-q1 * g.x - q2 * g.y - q3 * g.z);
    float qDot2 = 0.5f * (q0 * g.x + q2 * g.z - q3 * g.y);
    float qDot3 = 0.5f * (q0 * g.y - q1 * g.z + q3 * g.x);
    float qDot4 = 0.5f * (q0 * g.z + q1 * g.y - q2 * g.x);

    const float an2 = a.x * a.x + a.y * a.y + a.z * a.z;
    if (an2 > 0.0f) {
        const float r = inv_sqrt(an2);
        const float ax = a.x * r, ay = a.y * r, az = a.z * r;

        const float _2q0 = 2.0f * q0, _2q1 = 2.0f * q1, _2q2 = 2.0f * q2, _2q3 = 2.0f * q3;
        const float _4q0 = 4.0f * q0, _4q1 = 4.0f * q1, _4q2 = 4.0f * q2;
        const float _8q1 = 8.0f * q1, _8q2 = 8.0f * q2;
        const float q0q0 = q0 * q0, q1q1 = q1 * q1, q2q2 = q2 * q2, q3q3 = q3 * q3;

        float s0 = _4q0 * q2q2 + _2q2 * ax + _4q0 * q1q1 - _2q1 * ay;
        float s1 = _4q1 * q3q3 - _2q3 * ax + 4.0f * q0q0 * q1 - _2q0 * ay - _4q1 + _8q1 * q1q1 +
                   _8q1 * q2q2 + _4q1 * az;
        float s2 = 4.0f * q0q0 * q2 + _2q0 * ax + _4q2 * q3q3 - _2q3 * ay - _4q2 + _8q2 * q1q1 +
                   _8q2 * q2q2 + _4q2 * az;
        float s3 = 4.0f * q1q1 * q3 - _2q1 * ax + 4.0f * q2q2 * q3 - _2q2 * ay;

        const float sn2 = s0 * s0 + s1 * s1 + s2 * s2 + s3 * s3;
        if (sn2 > 0.0f) {
            const float rs = inv_sqrt(sn2);
            qDot1 -= beta_ * s0 * rs;
            qDot2 -= beta_ * s1 * rs;
            qDot3 -= beta_ * s2 * rs;
            qDot4 -= beta_ * s3 * rs;
        }
    }

    q_ = Quat{q0 + qDot1 * dt, q1 + qDot2 * dt, q2 + qDot3 * dt, q3 + qDot4 * dt}.normalized();
}

void MadgwickFilter::update_marg(const Vec3& g, const Vec3& a, const Vec3& m, float dt) {
    const float mn2 = m.x * m.x + m.y * m.y + m.z * m.z;
    const float an2 = a.x * a.x + a.y * a.y + a.z * a.z;
    if (mn2 <= 0.0f || an2 <= 0.0f) {
        update_imu(g, a, dt);
        return;
    }

    float q0 = q_.w, q1 = q_.x, q2 = q_.y, q3 = q_.z;

    float qDot1 = 0.5f * (-q1 * g.x - q2 * g.y - q3 * g.z);
    float qDot2 = 0.5f * (q0 * g.x + q2 * g.z - q3 * g.y);
    float qDot3 = 0.5f * (q0 * g.y - q1 * g.z + q3 * g.x);
    float qDot4 = 0.5f * (q0 * g.z + q1 * g.y - q2 * g.x);

    float r = inv_sqrt(an2);
    const float ax = a.x * r, ay = a.y * r, az = a.z * r;
    r = inv_sqrt(mn2);
    const float mx = m.x * r, my = m.y * r, mz = m.z * r;

    const float _2q0mx = 2.0f * q0 * mx, _2q0my = 2.0f * q0 * my, _2q0mz = 2.0f * q0 * mz;
    const float _2q1mx = 2.0f * q1 * mx;
    const float _2q0 = 2.0f * q0, _2q1 = 2.0f * q1, _2q2 = 2.0f * q2, _2q3 = 2.0f * q3;
    const float _2q0q2 = 2.0f * q0 * q2, _2q2q3 = 2.0f * q2 * q3;
    const float q0q0 = q0 * q0, q0q1 = q0 * q1, q0q2 = q0 * q2, q0q3 = q0 * q3;
    const float q1q1 = q1 * q1, q1q2 = q1 * q2, q1q3 = q1 * q3;
    const float q2q2 = q2 * q2, q2q3 = q2 * q3, q3q3 = q3 * q3;

    const float hx = mx * q0q0 - _2q0my * q3 + _2q0mz * q2 + mx * q1q1 + _2q1 * my * q2 +
                     _2q1 * mz * q3 - mx * q2q2 - mx * q3q3;
    const float hy = _2q0mx * q3 + my * q0q0 - _2q0mz * q1 + _2q1mx * q2 - my * q1q1 + my * q2q2 +
                     _2q2 * mz * q3 - my * q3q3;
    const float _2bx = std::sqrt(hx * hx + hy * hy);
    const float _2bz = -_2q0mx * q2 + _2q0my * q1 + mz * q0q0 + _2q1mx * q3 - mz * q1q1 +
                       _2q2 * my * q3 - mz * q2q2 + mz * q3q3;
    const float _4bx = 2.0f * _2bx, _4bz = 2.0f * _2bz;

    float s0 = -_2q2 * (2.0f * q1q3 - _2q0q2 - ax) + _2q1 * (2.0f * q0q1 + _2q2q3 - ay) -
               _2bz * q2 * (_2bx * (0.5f - q2q2 - q3q3) + _2bz * (q1q3 - q0q2) - mx) +
               (-_2bx * q3 + _2bz * q1) * (_2bx * (q1q2 - q0q3) + _2bz * (q0q1 + q2q3) - my) +
               _2bx * q2 * (_2bx * (q0q2 + q1q3) + _2bz * (0.5f - q1q1 - q2q2) - mz);
    float s1 = _2q3 * (2.0f * q1q3 - _2q0q2 - ax) + _2q0 * (2.0f * q0q1 + _2q2q3 - ay) -
               4.0f * q1 * (1 - 2.0f * q1q1 - 2.0f * q2q2 - az) +
               _2bz * q3 * (_2bx * (0.5f - q2q2 - q3q3) + _2bz * (q1q3 - q0q2) - mx) +
               (_2bx * q2 + _2bz * q0) * (_2bx * (q1q2 - q0q3) + _2bz * (q0q1 + q2q3) - my) +
               (_2bx * q3 - _4bz * q1) * (_2bx * (q0q2 + q1q3) + _2bz * (0.5f - q1q1 - q2q2) - mz);
    float s2 = -_2q0 * (2.0f * q1q3 - _2q0q2 - ax) + _2q3 * (2.0f * q0q1 + _2q2q3 - ay) -
               4.0f * q2 * (1 - 2.0f * q1q1 - 2.0f * q2q2 - az) +
               (-_4bx * q2 - _2bz * q0) * (_2bx * (0.5f - q2q2 - q3q3) + _2bz * (q1q3 - q0q2) - mx) +
               (_2bx * q1 + _2bz * q3) * (_2bx * (q1q2 - q0q3) + _2bz * (q0q1 + q2q3) - my) +
               (_2bx * q0 - _4bz * q2) * (_2bx * (q0q2 + q1q3) + _2bz * (0.5f - q1q1 - q2q2) - mz);
    float s3 = _2q1 * (2.0f * q1q3 - _2q0q2 - ax) + _2q2 * (2.0f * q0q1 + _2q2q3 - ay) +
               (-_4bx * q3 + _2bz * q1) * (_2bx * (0.5f - q2q2 - q3q3) + _2bz * (q1q3 - q0q2) - mx) +
               (-_2bx * q0 + _2bz * q2) * (_2bx * (q1q2 - q0q3) + _2bz * (q0q1 + q2q3) - my) +
               _2bx * q1 * (_2bx * (q0q2 + q1q3) + _2bz * (0.5f - q1q1 - q2q2) - mz);

    const float sn2 = s0 * s0 + s1 * s1 + s2 * s2 + s3 * s3;
    if (sn2 > 0.0f) {
        const float rs = inv_sqrt(sn2);
        qDot1 -= beta_ * s0 * rs;
        qDot2 -= beta_ * s1 * rs;
        qDot3 -= beta_ * s2 * rs;
        qDot4 -= beta_ * s3 * rs;
    }

    q_ = Quat{q0 + qDot1 * dt, q1 + qDot2 * dt, q2 + qDot3 * dt, q3 + qDot4 * dt}.normalized();
}

EulerDeg to_euler_deg(const Quat& q) {
    EulerDeg e;
    e.roll = std::atan2(2.0f * (q.w * q.x + q.y * q.z), 1.0f - 2.0f * (q.x * q.x + q.y * q.y)) * kRadToDeg;
    const float sp = 2.0f * (q.w * q.y - q.z * q.x);
    e.pitch = (std::fabs(sp) >= 1.0f ? std::copysign(90.0f, sp) : std::asin(sp) * kRadToDeg);
    e.yaw = std::atan2(2.0f * (q.w * q.z + q.x * q.y), 1.0f - 2.0f * (q.y * q.y + q.z * q.z)) * kRadToDeg;
    return e;
}

}  // namespace rayneo::core
