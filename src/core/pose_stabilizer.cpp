#include "rayneo/core/pose_stabilizer.h"

#include <cmath>

namespace rayneo::core {
namespace {

constexpr float kTwoPi = 6.283185307179586f;

float alpha(float cutoff_hz, float dt) {
    const float tau = 1.0f / (kTwoPi * cutoff_hz);
    return 1.0f / (1.0f + tau / dt);
}

}  // namespace

Quat slerp(const Quat& a, const Quat& b_in, float t) {
    Quat b = b_in;
    float d = a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;
    if (d < 0.0f) {  // shortest arc
        b = {-b.w, -b.x, -b.y, -b.z};
        d = -d;
    }
    if (d > 0.9995f) {  // nearly parallel: nlerp
        return Quat{a.w + (b.w - a.w) * t, a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t}
            .normalized();
    }
    const float theta = std::acos(d);
    const float s = std::sin(theta);
    const float wa = std::sin((1.0f - t) * theta) / s;
    const float wb = std::sin(t * theta) / s;
    return Quat{wa * a.w + wb * b.w, wa * a.x + wb * b.x, wa * a.y + wb * b.y, wa * a.z + wb * b.z}.normalized();
}

Quat PoseStabilizer::update(const Quat& q, const Vec3& omega, float dt) {
    if (!init_ || dt <= 0.0f) {
        init_ = true;
        state_ = q.normalized();
        omega_ = omega;
        omega_slow_ = omega;
        speed_ = omega.norm();
        cutoff_ = cfg_.min_cutoff_hz;
        return state_;
    }
    // Speed of the low-passed vector: tremor is zero-mean and cancels, a real turn persists.
    omega_slow_ = omega_slow_ + (omega - omega_slow_) * alpha(cfg_.speed_cutoff_hz, dt);
    speed_ = omega_slow_.norm();
    cutoff_ = cfg_.min_cutoff_hz + cfg_.beta * speed_;
    const float a = alpha(cutoff_, dt);
    state_ = slerp(state_, q, a);
    omega_ = omega_ + (omega - omega_) * a;
    return state_;
}

float prediction_gain(float speed_rads, float start_rads, float full_rads) {
    if (speed_rads <= start_rads) return 0.0f;
    if (speed_rads >= full_rads) return 1.0f;
    return (speed_rads - start_rads) / (full_rads - start_rads);
}

}  // namespace rayneo::core
