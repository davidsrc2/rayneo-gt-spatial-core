#include "rayneo/core/gyro_bias_estimator.h"

#include <algorithm>
#include <cmath>

namespace rayneo::core {
namespace {

float median(float* v, int n) {
    std::sort(v, v + n);
    return n % 2 == 1 ? v[n / 2] : 0.5f * (v[n / 2 - 1] + v[n / 2]);
}

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

void GyroBiasEstimator::reset_escape() {
    escape_anchor_ = {};
    escape_sum_ = {};
    escape_n_ = 0;
    escape_s_ = 0.0f;
}

void GyroBiasEstimator::track_at_rest(const Vec3& g, float dt) {
    if (verify_left_s_ > 0.0f) {
        // Gyro back on the pre-escape bias while the new one is off: the "step" was motion.
        const bool on_prev = (gyro_ema_ - prev_bias_).norm() <= cfg_.routine_residual_max_degs;
        const bool off_new = (gyro_ema_ - bias_).norm() > cfg_.routine_residual_max_degs;
        rollback_s_ = on_prev && off_new ? rollback_s_ + dt : 0.0f;
        if (rollback_s_ >= cfg_.rollback_persist_s) {
            bias_ = prev_bias_;
            verify_left_s_ = 0.0f;
            rollback_s_ = 0.0f;
            ++rollbacks_;
            reset_escape();
            return;
        }
    }

    const Vec3 residual = gyro_ema_ - bias_;
    if (residual.norm() <= cfg_.routine_residual_max_degs) {
        reset_escape();
        const float k = dt / (cfg_.routine_tau_s + dt);
        bias_ = clamp_norm(bias_ + residual * k, cfg_.bias_clamp_degs);
        return;
    }

    // Out-of-routine residual: only a long, stable plateau inside the clamp qualifies.
    if (escape_n_ == 0 || (gyro_ema_ - escape_anchor_).norm() > cfg_.escape_stability_degs) {
        reset_escape();
        escape_anchor_ = gyro_ema_;
    }
    escape_sum_ = escape_sum_ + g;
    ++escape_n_;
    escape_s_ += dt;
    if (escape_s_ < cfg_.escape_persist_s) return;

    const Vec3 candidate = escape_sum_ * (1.0f / static_cast<float>(escape_n_));
    reset_escape();
    if (candidate.norm() > cfg_.bias_clamp_degs) return;  // implausible as bias: reject
    prev_bias_ = bias_;
    bias_ = candidate;
    verify_left_s_ = cfg_.verify_s;
    rollback_s_ = 0.0f;
    ++escapes_;
}

void GyroBiasEstimator::track_worn(const Vec3& g, float dt) {
    const Vec3 r = g - bias_;
    if (!worn_ema_init_) {
        worn_ema_ = r;
        worn_ema_init_ = true;
    }
    worn_ema_ = worn_ema_ + (r - worn_ema_) * (dt / (cfg_.worn_ema_tau_s + dt));
    const float m = worn_ema_.norm();
    worn_hold_time_ = m < cfg_.worn_hold_degs ? worn_hold_time_ + dt : 0.0f;
    if (worn_hold_time_ < cfg_.worn_hold_s || m > cfg_.worn_residual_max_degs) return;
    bias_ = clamp_norm(bias_ + worn_ema_ * (dt / (cfg_.worn_tau_s + dt)), cfg_.bias_clamp_degs);
}

void GyroBiasEstimator::update_worn_block(const Vec3& g, float dt) {
    block_sum_ = block_sum_ + g;
    block_sq_ = block_sq_ + Vec3{g.x * g.x, g.y * g.y, g.z * g.z};
    ++block_n_;
    block_s_ += dt;
    if (block_s_ < cfg_.worn_block_s) return;

    const float inv = 1.0f / static_cast<float>(block_n_);
    const Vec3 mean = block_sum_ * inv;
    const Vec3 var = block_sq_ * inv - Vec3{mean.x * mean.x, mean.y * mean.y, mean.z * mean.z};
    const float std_norm = std::sqrt(std::max(0.0f, var.x) + std::max(0.0f, var.y) + std::max(0.0f, var.z));
    if (mean.norm() <= cfg_.bias_clamp_degs && std_norm <= cfg_.worn_max_std_degs) {
        block_means_[static_cast<size_t>(block_count_ % kMaxBlocks)] = mean;
        ++block_count_;
    }
    block_sum_ = {};
    block_sq_ = {};
    block_n_ = 0;
    block_s_ = 0.0f;
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
        reset_escape();
        rollback_s_ = 0.0f;
        block_sum_ = {};
        block_sq_ = {};
        block_n_ = 0;
        block_s_ = 0.0f;
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
            calib_elapsed_s_ += dt;
            update_worn_block(g, dt);
            if (!still_) {
                reset_window();  // Contiguous window only.
                if (calib_elapsed_s_ >= cfg_.worn_fallback_s && block_count_ >= 3) {
                    const int n = block_count_ < kMaxBlocks ? block_count_ : kMaxBlocks;
                    float xs[kMaxBlocks], ys[kMaxBlocks], zs[kMaxBlocks];
                    for (int i = 0; i < n; ++i) {
                        xs[i] = block_means_[static_cast<size_t>(i)].x;
                        ys[i] = block_means_[static_cast<size_t>(i)].y;
                        zs[i] = block_means_[static_cast<size_t>(i)].z;
                    }
                    bias_ = clamp_norm({median(xs, n), median(ys, n), median(zs, n)}, cfg_.bias_clamp_degs);
                    worn_start_ = true;
                    state_ = State::Tracking;
                }
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
            verify_left_s_ = std::max(0.0f, verify_left_s_ - dt);
            if (still_) {
                track_at_rest(g, dt);
                worn_hold_time_ = 0.0f;
                worn_ema_init_ = false;
            } else {
                reset_escape();  // contiguous rest only
                rollback_s_ = 0.0f;
                track_worn(g, dt);
            }
            return g - bias_;
        }
    }
    return g;
}

}  // namespace rayneo::core
