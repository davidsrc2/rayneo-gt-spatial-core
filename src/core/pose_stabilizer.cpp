#include "rayneo/core/pose_stabilizer.h"

#include <cmath>

#include "rayneo/core/render_math.h"

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
        omega_fast_ = omega;
        omega_out_ = {};
        speed_ = omega.norm();
        slow_time_ = 0.0f;
        for (int k = 0; k < 3; ++k) {
            damp_[k] = 0.0f;
            sign_[k] = 0;
            since_flip_[k] = 1e3f;
        }
        lock_ = Lock::Locked;
        anchor_ = out_;
        if (cfg_.stationary_lock) omega_slow_ = {};  // one sample is no speed: start locked
        ramp_t_ = calm_s_ = locked_s_ = 0.0f;
        lock_omega_ = {};
        return out_;
    }
    // Speed of the low-passed vector: tremor is zero-mean and cancels, a real turn persists.
    omega_slow_ = omega_slow_ + (omega - omega_slow_) * alpha(cfg_.speed_cutoff_hz, dt);
    speed_ = omega_slow_.norm();
    if (cfg_.stationary_lock) return update_lock(q.normalized(), omega, dt);
    if (cfg_.predict_cutoff_hz > 0.0f) {
        const Vec3 before = omega_fast_;
        omega_fast_ = omega_fast_ + (omega - omega_fast_) * alpha(cfg_.predict_cutoff_hz, dt);
        tap_left_s_ = std::fmax(0.0f, tap_left_s_ - dt);
        if (cfg_.tap_accel_rads2 > 0.0f && (omega_fast_ - before).norm() / dt > cfg_.tap_accel_rads2) {
            tap_left_s_ = cfg_.tap_hold_s;
        }
    }

    update_damping(cfg_.predict_cutoff_hz > 0.0f ? omega_fast_ : omega_slow_, dt);

    if (holding_) {
        bool inside;
        if (cfg_.enter_vertical_deg > 0.0f && cfg_.enter_deg > 0.0f) {
            // In the held frame (X forward, Y left, Z up): about Z is left / right, about Y up /
            // down, about X a tilt. Small-angle rotation vector of the relative rotation.
            Quat r = out_.conjugate() * q.normalized();
            if (r.w < 0.0f) r = {-r.w, -r.x, -r.y, -r.z};
            const float k = 2.0f * 57.29578f;
            const float yaw = r.z * k / cfg_.enter_deg;
            const float pitch = r.y * k / cfg_.enter_vertical_deg, roll = r.x * k / cfg_.enter_vertical_deg;
            inside = yaw * yaw + pitch * pitch + roll * roll <= 1.0f;
        } else {
            const float d = std::fabs(out_.w * q.w + out_.x * q.x + out_.y * q.y + out_.z * q.z);
            inside = 2.0f * std::acos(std::fmin(1.0f, d)) * 57.29578f <= cfg_.enter_deg;
        }
        if (inside) {
            omega_out_ = {};
            return finish();  // frozen: identical to a head-locked monitor
        }
        holding_ = false;
        slow_time_ = 0.0f;
    }

    float cutoff = cfg_.follow_min_cutoff_hz + cfg_.follow_beta * speed_;
    if (cfg_.release_cutoff_hz > 0.0f && cfg_.enter_deg > 0.0f && speed_ < cfg_.settle_speed_rads) {
        cutoff = cfg_.release_cutoff_hz;
    }
    if (cfg_.reversal_decay_s > 0.0f) {
        // Per axis (the frame of omega: q = rotation * out): a trembling axis eased in at a
        // lower cutoff, the others as before. Converges like the slerp: nothing drifts.
        Quat r = q.normalized() * out_.conjugate();
        if (r.w < 0.0f) r = {-r.w, -r.x, -r.y, -r.z};
        const float e[3] = {2.0f * r.x, 2.0f * r.y, 2.0f * r.z};  // small-angle rotation vector
        Vec3 step;
        float* s[3] = {&step.x, &step.y, &step.z};
        for (int k = 0; k < 3; ++k) {
            const float c = cutoff + (cfg_.reversal_min_cutoff_hz - cutoff) * effective_damp(k);
            *s[k] = e[k] * alpha(std::fmax(cfg_.reversal_min_cutoff_hz, c), dt);
        }
        const float a = step.norm();
        if (a > 1e-9f) out_ = (Quat::from_axis_angle(step, a) * out_).normalized();
    } else {
        out_ = slerp(out_, q, alpha(cutoff, dt));
    }
    omega_out_ = omega_slow_;
    slow_time_ = speed_ < cfg_.settle_speed_rads ? slow_time_ + dt : 0.0f;
    if (slow_time_ >= cfg_.settle_s) {
        holding_ = true;
        omega_out_ = {};
    }
    return finish();
}

float PoseStabilizer::effective_damp(int k) const {
    // Only while the axis moves slowly: a turn under way carrying tremor is followed (damping
    // it dragged the panels behind the turn).
    const float w[3] = {omega_slow_.x, omega_slow_.y, omega_slow_.z};
    const float v0 = cfg_.reversal_speed_rads;
    return v0 > 0.0f ? damp_[k] * std::fmax(0.0f, 1.0f - std::fabs(w[k]) / v0) : damp_[k];
}

Quat PoseStabilizer::update_lock(const Quat& q, const Vec3& omega, float dt) {
    // From the vector part: acos of a dot product near 1 is too coarse in float under ~0.02 deg.
    Quat r = anchor_.conjugate() * q;
    const float off_deg = 2.0f * std::asin(std::fmin(1.0f, std::sqrt(r.x * r.x + r.y * r.y + r.z * r.z))) * 57.29578f;
    switch (lock_) {
    case Lock::Locked:
        if (speed_ > cfg_.unlock_rads || off_deg > cfg_.unlock_deg) {
            lock_ = Lock::Unlocking;
            ramp_t_ = 0.0f;
            locked_s_ = 0.0f;
            break;
        }
        locked_s_ += dt;
        holding_ = true;
        lock_omega_ = {};
        return anchor_;  // frozen: not a sub-pixel of movement
    case Lock::Unlocking:
    case Lock::Tracking:
        break;
    }
    holding_ = false;
    if (lock_ == Lock::Unlocking) {
        ramp_t_ = std::fmin(1.0f, ramp_t_ + dt / std::fmax(1e-3f, cfg_.unlock_ramp_s));
        const float k = ramp_t_ * ramp_t_ * (3.0f - 2.0f * ramp_t_);  // smoothstep
        if (ramp_t_ >= 1.0f) lock_ = Lock::Tracking;
        lock_omega_ = omega * k;
        calm_s_ = 0.0f;
        return slerp(anchor_, q, k);
    }
    // Tracking: the head itself; back to LOCKED after relock_s under relock_rads.
    lock_omega_ = omega;
    calm_s_ = speed_ < cfg_.relock_rads ? calm_s_ + dt : 0.0f;
    if (calm_s_ >= cfg_.relock_s) {
        lock_ = Lock::Locked;
        anchor_ = q;
        locked_s_ = 0.0f;
        lock_omega_ = {};
        holding_ = true;
    }
    return q;
}

void PoseStabilizer::update_damping(const Vec3& omega, float dt) {
    if (cfg_.reversal_decay_s <= 0.0f) return;
    const float w[3] = {omega.x, omega.y, omega.z};
    const float decay = std::exp(-dt / cfg_.reversal_decay_s);
    for (int k = 0; k < 3; ++k) {
        since_flip_[k] += dt;
        damp_[k] *= decay;
        if (std::fabs(w[k]) < cfg_.reversal_threshold_rads) continue;
        const int sign = w[k] > 0.0f ? 1 : -1;
        if (sign_[k] != 0 && sign != sign_[k]) {
            if (since_flip_[k] < cfg_.reversal_window_s) damp_[k] = std::fmin(1.0f, damp_[k] + 0.4f);
            since_flip_[k] = 0.0f;
        }
        sign_[k] = sign;
    }
}

Quat PoseStabilizer::finish() const {
    if (!cfg_.compensate_lag) return out_;
    const float cutoff = cfg_.follow_min_cutoff_hz + cfg_.follow_beta * speed_;
    const float gain = prediction_gain(omega().norm(), kPredictStartRads, kPredictFullRads);
    return predict_orientation(out_, omega(), gain / (kTwoPi * cutoff));
}

float prediction_gain(float speed_rads, float start_rads, float full_rads) {
    if (speed_rads <= start_rads) return 0.0f;
    if (speed_rads >= full_rads) return 1.0f;
    return (speed_rads - start_rads) / (full_rads - start_rads);
}

}  // namespace rayneo::core
