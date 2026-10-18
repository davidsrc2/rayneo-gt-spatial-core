#include "rayneo/core/vqf.h"

#include <algorithm>
#include <cmath>

namespace rayneo::core {
namespace {

constexpr float kRadToDeg = 57.29577951308232f;

float lp_alpha(float tau_s, float dt) { return tau_s > 0.0f ? dt / (tau_s + dt) : 1.0f; }

// Rotation taking the unit vector `a` onto +Z (axis a x z, no component about Z).
Quat to_vertical(const Vec3& a) {
    const float w = std::sqrt(std::max(0.0f, (a.z + 1.0f) * 0.5f));
    if (w < 1e-6f) return Quat{0.0f, 1.0f, 0.0f, 0.0f};  // upside down: half a turn about X
    return Quat{w, a.y / (2.0f * w), -a.x / (2.0f * w), 0.0f};
}

}  // namespace

void Vqf6D::reset() {
    gyr_q_ = Quat{};
    acc_q_ = Quat{};
    acc_i_lp_ = {0.0f, 0.0f, 9.81f};
    gyr_lp_ = acc_lp_ = {};
    lp_init_ = false;
    rest_s_ = 0.0f;
    rest_ = false;
    bias_ = {};
    const float v = cfg_.bias_start_std_degs * cfg_.bias_start_std_degs;
    bias_var_ = {v, v, v};
    gyr_slow_ = {};
    elapsed_s_ = 0.0f;
}

void Vqf6D::initialize_from_accel(const Vec3& accel) {
    gyr_q_ = Quat{};
    const float n = accel.norm();
    acc_q_ = n > 1e-6f ? to_vertical(accel * (1.0f / n)) : Quat{};
    acc_i_lp_ = accel;
    lp_init_ = false;
}

void Vqf6D::set_bias(const Vec3& bias_degs, float std_degs) {
    bias_ = bias_degs;
    clip_bias();
    const float s = std_degs > 0.0f ? std_degs : cfg_.bias_start_std_degs;
    bias_var_ = {s * s, s * s, s * s};
}

void Vqf6D::clip_bias() {
    const float n = bias_.norm();
    if (n > cfg_.bias_clip_degs) bias_ = bias_ * (cfg_.bias_clip_degs / n);
}

void Vqf6D::update(const Vec3& gyro_rads, const Vec3& accel, float dt) {
    if (dt <= 0.0f || dt > 0.1f) return;
    const Vec3 gyr_degs = gyro_rads * kRadToDeg;

    // Rest detection ("bias gate").
    if (!lp_init_) {
        gyr_lp_ = gyr_slow_ = gyr_degs;
        acc_lp_ = accel;
        lp_init_ = true;
    }
    elapsed_s_ += dt;
    gyr_slow_ = gyr_slow_ + (gyr_degs - gyr_slow_) * lp_alpha(cfg_.bias_smooth_tau_s, dt);
    const float ar = lp_alpha(cfg_.rest_filter_tau_s, dt);
    gyr_lp_ = gyr_lp_ + (gyr_degs - gyr_lp_) * ar;
    acc_lp_ = acc_lp_ + (accel - acc_lp_) * ar;
    const bool calm = (gyr_lp_ - bias_).norm() < cfg_.rest_gyro_degs && (accel - acc_lp_).norm() < cfg_.rest_acc_mps2;
    rest_s_ = calm ? rest_s_ + dt : 0.0f;
    rest_ = rest_s_ >= cfg_.rest_min_s;

    // Gyro bias, 1D Kalman per axis. Predict: the bias may have walked.
    const float q = cfg_.bias_walk_degs * cfg_.bias_walk_degs * dt;
    bias_var_ = {bias_var_.x + q, bias_var_.y + q, bias_var_.z + q};
    // Update at rest from a residual small enough to be bias. The averaged gyro changes little
    // from one sample to the next: its variance is spread over the samples of one average.
    if (rest_) {
        const Vec3 residual = gyr_slow_ - bias_;
        const bool start = elapsed_s_ < cfg_.bias_start_s;
        if (residual.norm() < (start ? cfg_.bias_gate_start_degs : cfg_.bias_gate_degs)) {
            const float r = cfg_.bias_meas_degs * cfg_.bias_meas_degs * std::max(1.0f, cfg_.bias_smooth_tau_s / dt);
            const auto step = [r](float& b, float& p, float e) {
                const float k = p / (p + r);
                b += k * e;
                p *= 1.0f - k;
            };
            step(bias_.x, bias_var_.x, residual.x);
            step(bias_.y, bias_var_.y, residual.y);
            step(bias_.z, bias_var_.z, residual.z);
            clip_bias();
        }
    }

    // Strapdown integration of the corrected gyro.
    const Vec3 w = gyro_rads - bias_ * (1.0f / kRadToDeg);
    const float angle = w.norm() * dt;
    if (angle > 1e-9f) gyr_q_ = (gyr_q_ * Quat::from_axis_angle(w, angle)).normalized();

    // Inclination: the low-passed accelerometer in the gyro frame, tilted onto vertical.
    acc_i_lp_ = acc_i_lp_ + (gyr_q_.rotate(accel) - acc_i_lp_) * lp_alpha(cfg_.tau_acc_s, dt);
    const Vec3 a = acc_q_.rotate(acc_i_lp_);
    const float n = a.norm();
    if (n > 1e-3f) acc_q_ = (to_vertical(a * (1.0f / n)) * acc_q_).normalized();
}

}  // namespace rayneo::core
