#pragma once

#include "rayneo/core/types.h"

namespace rayneo::core {

// Runtime gyro bias / drift correction. The GT firmware does not answer the factory
// bias request (66 3c), so bias is estimated online:
//  1. Warmup: discard the contaminated samples right after stream-on.
//  2. Startup calibration: average ONE contiguous rest window (fragments never stitched).
//  3. Routine adaptation: during dwell-qualified rest, chase only small residuals so a
//     deliberate slow head turn is not learned as drift.
// Gates are vector magnitudes, so they are invariant to the sensor->head rotation.
struct GyroBiasConfig {
    float warmup_s = 4.0f;
    int warmup_min_samples = 150;
    float calib_window_s = 1.0f;
    int calib_min_samples = 600;

    float rest_enter_gyro_degs = 1.5f;
    float rest_enter_accel_mps2 = 0.5f;
    float rest_exit_gyro_degs = 3.0f;
    float rest_exit_accel_mps2 = 1.0f;
    float rest_dwell_s = 0.5f;
    float motion_holdoff_s = 1.0f;

    float bias_clamp_degs = 1.5f;
    float routine_residual_max_degs = 0.35f;
    float routine_tau_s = 5.0f;
    float ema_tau_s = 0.2f;
    float gap_reset_s = 0.05f;
};

class GyroBiasEstimator {
public:
    enum class State { Warmup, Calibrating, Tracking };

    explicit GyroBiasEstimator(GyroBiasConfig cfg = {}) : cfg_(cfg) {}

    void reset();

    // Returns the bias-corrected gyro (deg/s). Before Tracking, the raw value is returned.
    Vec3 update(const Vec3& gyro_degs, const Vec3& accel_mps2, float dt);

    State state() const { return state_; }
    bool tracking() const { return state_ == State::Tracking; }
    bool still() const { return still_; }
    const Vec3& bias() const { return bias_; }

private:
    void update_rest_detector(const Vec3& g, const Vec3& a, float dt);
    void reset_window();

    GyroBiasConfig cfg_;
    State state_ = State::Warmup;

    float elapsed_s_ = 0.0f;
    int samples_ = 0;

    // Rest detector (EMA low-pass + deviation from it).
    bool ema_init_ = false;
    Vec3 gyro_ema_;
    Vec3 accel_ema_;
    float dwell_s_ = 0.0f;
    float holdoff_s_ = 0.0f;
    bool still_ = false;

    // Startup calibration window.
    Vec3 window_sum_;
    int window_n_ = 0;
    float window_s_ = 0.0f;

    Vec3 bias_;
};

}  // namespace rayneo::core
