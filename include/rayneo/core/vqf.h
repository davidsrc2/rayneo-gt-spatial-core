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
//  - Gyro bias: a 1D Kalman filter per axis fed by the low-passed gyro at rest, its
//    uncertainty growing in motion, clipped to bias_clip_degs.
// Frame: sensor -> earth, earth Z up (as MadgwickFilter). Gyro in rad/s, accel in m/s^2.
struct VqfConfig {
    float tau_acc_s = 3.0f;
    float rest_filter_tau_s = 0.1f;
    float rest_gyro_degs = 1.5f;
    float rest_acc_mps2 = 0.5f;
    float rest_min_s = 0.2f;
    float bias_sigma_init_degs = 0.5f;
    float bias_forgetting_s = 100.0f;  // the bias' uncertainty grows to sigma_init in this long
    float bias_sigma_rest_degs = 0.03f;  // measurement noise of the low-passed gyro at rest
    float bias_clip_degs = 2.0f;
};

class Vqf6D {
public:
    explicit Vqf6D(VqfConfig cfg = {}) : cfg_(cfg) { reset(); }

    void reset();
    // Level from one accelerometer sample (heading 0).
    void initialize_from_accel(const Vec3& accel);
    // The starting bias (deg/s), e.g. the startup calibration's or the temperature map's.
    void set_bias(const Vec3& bias_degs);

    // Raw gyro (rad/s, bias not removed), accelerometer, seconds since the previous sample.
    void update(const Vec3& gyro_rads, const Vec3& accel, float dt);

    Quat quaternion() const { return (acc_q_ * gyr_q_).normalized(); }
    bool rest() const { return rest_; }
    const Vec3& bias_degs() const { return bias_; }

private:
    VqfConfig cfg_;
    Quat gyr_q_, acc_q_;
    Vec3 acc_i_lp_;               // accelerometer in the gyro frame, low-passed
    Vec3 gyr_lp_, acc_lp_;        // for rest detection
    bool lp_init_ = false;
    float rest_s_ = 0.0f;
    bool rest_ = false;
    Vec3 bias_;                   // deg/s
    float bias_p_[3] = {0, 0, 0};  // variance, (deg/s)^2
};

}  // namespace rayneo::core
