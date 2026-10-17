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
    for (float& p : bias_p_) p = cfg_.bias_sigma_init_degs * cfg_.bias_sigma_init_degs;
}

void Vqf6D::initialize_from_accel(const Vec3& accel) {
    gyr_q_ = Quat{};
    const float n = accel.norm();
    acc_q_ = n > 1e-6f ? to_vertical(accel * (1.0f / n)) : Quat{};
    acc_i_lp_ = accel;
    lp_init_ = false;
}

void Vqf6D::set_bias(const Vec3& bias_degs) {
    const float n = bias_degs.norm();
    bias_ = n > cfg_.bias_clip_degs ? bias_degs * (cfg_.bias_clip_degs / n) : bias_degs;
}

void Vqf6D::update(const Vec3& gyro_rads, const Vec3& accel, float dt) {
    if (dt <= 0.0f || dt > 0.1f) return;
    const Vec3 gyr_degs = gyro_rads * kRadToDeg;

    // Rest detection ("bias gate").
    if (!lp_init_) {
        gyr_lp_ = gyr_degs;
        acc_lp_ = accel;
        lp_init_ = true;
    }
    const float ar = lp_alpha(cfg_.rest_filter_tau_s, dt);
    gyr_lp_ = gyr_lp_ + (gyr_degs - gyr_lp_) * ar;
    acc_lp_ = acc_lp_ + (accel - acc_lp_) * ar;
    const bool calm = (gyr_lp_ - bias_).norm() < cfg_.rest_gyro_degs && (accel - acc_lp_).norm() < cfg_.rest_acc_mps2;
    rest_s_ = calm ? rest_s_ + dt : 0.0f;
    rest_ = rest_s_ >= cfg_.rest_min_s;

    // Gyro bias: 1D Kalman per axis, measured at rest, more uncertain in motion.
    const float q = cfg_.bias_sigma_init_degs * cfg_.bias_sigma_init_degs / cfg_.bias_forgetting_s;
    const float r = cfg_.bias_sigma_rest_degs * cfg_.bias_sigma_rest_degs;
    const float meas[3] = {gyr_lp_.x, gyr_lp_.y, gyr_lp_.z};
    float* b[3] = {&bias_.x, &bias_.y, &bias_.z};
    for (int k = 0; k < 3; ++k) {
        bias_p_[k] = std::min(bias_p_[k] + q * dt, cfg_.bias_sigma_init_degs * cfg_.bias_sigma_init_degs);
        if (!rest_) continue;
        const float gain = bias_p_[k] / (bias_p_[k] + r) * std::min(1.0f, dt / cfg_.rest_filter_tau_s);
        *b[k] += gain * (meas[k] - *b[k]);
        bias_p_[k] *= 1.0f - gain;
    }
    set_bias(bias_);

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
