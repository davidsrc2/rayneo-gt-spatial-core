#include "rayneo/core/gyro_bias_estimator.h"

#include <algorithm>
#include <cmath>

namespace rayneo::core {
namespace {

Vec3 clamp_norm(const Vec3& v, float max_norm) {
    const float n = v.norm();
    return n > max_norm && n > 0.0f ? v * (max_norm / n) : v;
}

}  // namespace

void GyroBiasEstimator::reset() {
    *this = GyroBiasEstimator(cfg_);
}

void GyroBiasEstimator::reset_window() {
    window_sum_ = {};
    window_n_ = 0;
    window_s_ = 0.0f;
}

void GyroBiasEstimator::update_rest_detector(const Vec3& g, const Vec3& a, float dt) {
    if (!ema_init_) {
        gyro_ema_ = g;
        accel_ema_ = a;
        ema_init_ = true;
    }
    const float k = std::clamp(dt / (cfg_.ema_tau_s + dt), 0.0f, 1.0f);
    gyro_ema_ = gyro_ema_ + (g - gyro_ema_) * k;
    accel_ema_ = accel_ema_ + (a - accel_ema_) * k;

    const float gdev = (g - gyro_ema_).norm();
    const float adev = (a - accel_ema_).norm();

    if (gdev > cfg_.rest_exit_gyro_degs || adev > cfg_.rest_exit_accel_mps2) {
        dwell_s_ = 0.0f;
        holdoff_s_ = cfg_.motion_holdoff_s;
        still_ = false;
        return;
    }
    holdoff_s_ = std::max(0.0f, holdoff_s_ - dt);

    if (gdev < cfg_.rest_enter_gyro_degs && adev < cfg_.rest_enter_accel_mps2 && holdoff_s_ <= 0.0f) {
        dwell_s_ += dt;
    } else {
        dwell_s_ = 0.0f;
    }
    still_ = dwell_s_ >= cfg_.rest_dwell_s;
}

Vec3 GyroBiasEstimator::update(const Vec3& g, const Vec3& a, float dt) {
    if (dt <= 0.0f) return g - bias_;

    if (dt >= cfg_.gap_reset_s) {
        // Timestamp gap: invalidate any partial window and rest dwell.
        reset_window();
        dwell_s_ = 0.0f;
        still_ = false;
        return g - bias_;
    }

    elapsed_s_ += dt;
    ++samples_;

    switch (state_) {
        case State::Warmup:
            if (elapsed_s_ >= cfg_.warmup_s && samples_ >= cfg_.warmup_min_samples) {
                state_ = State::Calibrating;
            }
            return g;

        case State::Calibrating:
            update_rest_detector(g, a, dt);
            if (!still_) {
                reset_window();  // Contiguous window only.
                return g;
            }
            window_sum_ = window_sum_ + g;
            ++window_n_;
            window_s_ += dt;
            if (window_s_ >= cfg_.calib_window_s && window_n_ >= cfg_.calib_min_samples) {
                bias_ = clamp_norm(window_sum_ * (1.0f / static_cast<float>(window_n_)), cfg_.bias_clamp_degs);
                state_ = State::Tracking;
            }
            return g;

        case State::Tracking: {
            update_rest_detector(g, a, dt);
            if (still_) {
                const Vec3 residual = gyro_ema_ - bias_;
                if (residual.norm() <= cfg_.routine_residual_max_degs) {
                    const float k = dt / (cfg_.routine_tau_s + dt);
                    bias_ = clamp_norm(bias_ + residual * k, cfg_.bias_clamp_degs);
                }
                // TODO: slow escape path + rollback for persistent bias steps
                // (see Rayneo-Spatial-App docs/PROTOCOL-NOTES.md, "Fusion notes").
            }
            return g - bias_;
        }
    }
    return g;
}

}  // namespace rayneo::core
