#pragma once

#include "rayneo/core/types.h"

namespace rayneo::core {

// 6D orientation filter after the structure of VQF (Laidig & Seel, "VQF: Highly accurate IMU
// orientation estimation with bias estimation and magnetic disturbance rejection", Information
// Fusion 2023), written for Nimbo: no magnetometer.
//  - Strapdown: the bias-corrected gyro integrated into gyr_q (sensor -> an almost inertial
//    frame), heading from the gyro alone.
//  - Inclination: the accelerometer seen in that frame, low-passed (tau_acc_s), tilts a pure
//    inclination correction acc_q towards gravity: pitch and roll stay level, slowly, and
//    typing or a step never reaches the panels.
//  - Rest detection: low-passed gyro under rest_gyro_degs and accelerometer within
//    rest_acc_mps2 of its own low-pass for rest_min_s ("bias gate" locked).
//  - Gyro bias: a 1D Kalman filter per axis. The bias is a random walk (bias_walk_degs per
//    sqrt(s)); at rest, the gyro averaged over bias_smooth_tau_s measures it (bias_meas_degs,
//    one independent measurement per bias_smooth_tau_s). Its variance says how much to trust
//    each reading: fast while uncertain (cold start, a guess), slow once known (a ~20 s time
//    constant). Readings further than bias_gate_degs from the estimate (bias_gate_start_degs
//    the first bias_start_s) are a head drifting slowly: rejected. Clipped to bias_clip_degs.
//    (Learning in ~0.1 s from the 0.1 s low-pass, the first version took slow head motion for
//    bias, up to 0.7 deg/s off, and the panels drifted sideways on their own.)
// Frame: sensor -> earth, earth Z up (as MadgwickFilter). Gyro in rad/s, accel in m/s^2.
struct VqfConfig {
    float tau_acc_s = 3.0f;
    float rest_filter_tau_s = 0.1f;
    float rest_gyro_degs = 1.5f;
    float rest_acc_mps2 = 0.5f;
    float rest_min_s = 0.2f;
    float bias_smooth_tau_s = 1.0f;
    float bias_gate_degs = 0.3f;
    float bias_gate_start_degs = 1.0f;  // the first bias_start_s: the starting bias may be rough
    float bias_walk_degs = 0.0025f;   // process noise: steady-state time constant ~sqrt(meas^2 / walk^2) = 20 s
    float bias_meas_degs = 0.05f;     // noise of one bias_smooth_tau_s average at rest
    float bias_start_std_degs = 0.5f; // uncertainty of a starting bias given without one
    float bias_start_s = 30.0f;
    float bias_clip_degs = 2.0f;
};

class Vqf6D {
public:
    explicit Vqf6D(VqfConfig cfg = {}) : cfg_(cfg) { reset(); }

    void reset();
    // Level from one accelerometer sample (heading 0).
    void initialize_from_accel(const Vec3& accel);
    // The starting bias (deg/s), e.g. the startup calibration's or the temperature map's, and
    // how sure it is (standard deviation, deg/s; <= 0: bias_start_std_degs).
    void set_bias(const Vec3& bias_degs, float std_degs = 0.0f);

    // Raw gyro (rad/s, bias not removed), accelerometer, seconds since the previous sample.
    void update(const Vec3& gyro_rads, const Vec3& accel, float dt);

    Quat quaternion() const { return (acc_q_ * gyr_q_).normalized(); }
    bool rest() const { return rest_; }
    // Past the start phase: the bias has had time to settle (worth remembering per temperature).
    bool settled() const { return elapsed_s_ >= cfg_.bias_start_s; }
    const Vec3& bias_degs() const { return bias_; }
    // The Kalman variance per axis, (deg/s)^2.
    const Vec3& bias_variance() const { return bias_var_; }

private:
    VqfConfig cfg_;
    Quat gyr_q_, acc_q_;
    Vec3 acc_i_lp_;               // accelerometer in the gyro frame, low-passed
    Vec3 gyr_lp_, acc_lp_;        // for rest detection
    bool lp_init_ = false;
    float rest_s_ = 0.0f;
    bool rest_ = false;
    Vec3 bias_;                   // deg/s
    Vec3 bias_var_;               // (deg/s)^2, per axis
    void clip_bias();
    Vec3 gyr_slow_;               // gyro averaged over bias_smooth_tau_s
    float elapsed_s_ = 0.0f;
};

}  // namespace rayneo::core
