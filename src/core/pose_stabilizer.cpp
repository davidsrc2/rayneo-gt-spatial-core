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
        holding_ = true;
        out_ = q.normalized();
        omega_slow_ = omega;
        omega_out_ = {};
        speed_ = omega.norm();
        slow_time_ = 0.0f;
        return out_;
    }
    // Speed of the low-passed vector: tremor is zero-mean and cancels, a real turn persists.
    omega_slow_ = omega_slow_ + (omega - omega_slow_) * alpha(cfg_.speed_cutoff_hz, dt);
    speed_ = omega_slow_.norm();

    if (holding_) {
        const float d = std::fabs(out_.w * q.w + out_.x * q.x + out_.y * q.y + out_.z * q.z);
        const float err_deg = 2.0f * std::acos(std::fmin(1.0f, d)) * 57.29578f;
        if (err_deg <= cfg_.enter_deg) {
            omega_out_ = {};
            return out_;  // frozen: identical to a head-locked monitor
        }
        holding_ = false;
        slow_time_ = 0.0f;
    }

    const float cutoff = cfg_.follow_min_cutoff_hz + cfg_.follow_beta * speed_;
    out_ = slerp(out_, q, alpha(cutoff, dt));
    omega_out_ = omega_slow_;
    slow_time_ = speed_ < cfg_.settle_speed_rads ? slow_time_ + dt : 0.0f;
    if (slow_time_ >= cfg_.settle_s) {
        holding_ = true;
        omega_out_ = {};
    }
    return out_;
}

float prediction_gain(float speed_rads, float start_rads, float full_rads) {
    if (speed_rads <= start_rads) return 0.0f;
    if (speed_rads >= full_rads) return 1.0f;
    return (speed_rads - start_rads) / (full_rads - start_rads);
}

}  // namespace rayneo::core
