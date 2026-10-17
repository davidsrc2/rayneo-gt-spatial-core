#pragma once

#include <array>

#include "rayneo/core/types.h"

namespace rayneo::core {

// Runtime gyro bias / drift correction. The GT firmware does not answer the factory
// bias request (66 3c), so bias is estimated online:
//  1. Warmup: discard the contaminated samples right after stream-on.
//  2. Startup calibration: average ONE contiguous rest window (fragments never stitched).
//     Worn fallback: a worn head never meets the desk rest gate (pulse, tremor). If no rest
//     window arrives within worn_fallback_s, adopt the component-wise median of the steady
//     1 s blocks seen (head micro-motions average out across blocks).
//  3. Routine adaptation: during dwell-qualified rest, chase only small residuals so a
//     deliberate slow head turn is not learned as drift.
//  4. Escape path: a larger residual (thermal step) is adopted only after it has been
//     stable for escape_persist_s of uninterrupted rest and stays inside the clamp.
//  5. Rollback: for verify_s after an escape, if the gyro settles back on the previous
//     bias for rollback_persist_s, the escape was a slow motion, not drift: undo it.
//  6. Worn adaptation: while worn (no desk rest), during head holds (1 s-smoothed residual
//     under worn_hold_degs for worn_hold_s) residuals up to worn_residual_max_degs are
//     chased with worn_tau_s. Deliberate turns, even slow ones, exceed the gate.
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

    float worn_fallback_s = 10.0f;     // time in Calibrating before the fallback applies
    float worn_block_s = 1.0f;
    float worn_max_std_degs = 4.0f;    // |per-axis std| of a usable block (tremor ~3)

    float worn_ema_tau_s = 1.0f;
    float worn_hold_degs = 1.0f;
    float worn_hold_s = 1.5f;
    float worn_residual_max_degs = 0.5f;
    float worn_tau_s = 20.0f;
    // The first bias, taken worn in a second or two, is rough: for this long after tracking
    // starts the worn residual is chased faster, so the panels stop creeping within seconds
    // (with 20 s they crept for about that long after every start).
    float worn_startup_s = 30.0f;
    float worn_startup_tau_s = 10.0f;

    float escape_persist_s = 10.0f;
    float escape_stability_degs = 0.15f;  // candidate EMA must stay this close to its anchor
    float verify_s = 30.0f;
    float rollback_persist_s = 2.0f;

    // Stationary lock (adapt_locked): while the panels are locked (head still > 150 ms) the
    // gyro reads its bias plus tremor; chased with this slow EMA, residuals up to
    // worn_residual_max_degs (a slow deliberate turn unlocks first).
    float locked_tau_s = 20.0f;
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
    // The stationary lock holds (head still): the raw gyro (deg/s) refines the bias.
    void adapt_locked(const Vec3& gyro_degs, float dt);

    // True when tracking started from the worn fallback (bias less certain until the next
    // rest on a desk refines it through routine adaptation).
    bool worn_start() const { return worn_start_; }
    int escapes() const { return escapes_; }
    int rollbacks() const { return rollbacks_; }
    bool verifying() const { return verify_left_s_ > 0.0f; }

private:
    void update_rest_detector(const Vec3& g, const Vec3& a, float dt);
    void reset_window();
    void reset_escape();
    void track_at_rest(const Vec3& g, float dt);
    void update_worn_block(const Vec3& g, float dt);
    void track_worn(const Vec3& g, float dt);

    GyroBiasConfig cfg_;
    State state_ = State::Warmup;

    float elapsed_s_ = 0.0f;
    float tracking_s_ = 0.0f;  // since the state became Tracking
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

    // Escape candidate: contiguous rest with a stable, out-of-routine residual.
    Vec3 escape_anchor_;
    Vec3 escape_sum_;
    int escape_n_ = 0;
    float escape_s_ = 0.0f;

    // Post-escape verification.
    Vec3 prev_bias_;
    float verify_left_s_ = 0.0f;
    float rollback_s_ = 0.0f;

    int escapes_ = 0;
    int rollbacks_ = 0;

    // Worn fallback.
    float calib_elapsed_s_ = 0.0f;
    Vec3 block_sum_, block_sq_;
    int block_n_ = 0;
    float block_s_ = 0.0f;
    static constexpr int kMaxBlocks = 32;
    std::array<Vec3, kMaxBlocks> block_means_{};
    int block_count_ = 0;
    bool worn_start_ = false;

    // Worn adaptation.
    bool worn_ema_init_ = false;
    Vec3 worn_ema_;  // residual (gyro - bias), 1 s low-pass
    float worn_hold_time_ = 0.0f;
};

}  // namespace rayneo::core
