#pragma once

#include "rayneo/core/types.h"

namespace rayneo::core {

// Madgwick AHRS (gradient-descent orientation filter).
// Frame: sensor -> earth, earth Z-up. Gyro input in rad/s, accel/mag any consistent unit.
class MadgwickFilter {
public:
    explicit MadgwickFilter(float beta = 0.033f) : beta_(beta) {}

    void set_beta(float beta) { beta_ = beta; }
    float beta() const { return beta_; }

    // Snaps the attitude to gravity (yaw = 0). Removes the convergence transient.
    void initialize_from_accel(const Vec3& accel);
    void reset() { q_ = Quat{}; }

    void update_imu(const Vec3& gyro_rads, const Vec3& accel, float dt);
    void update_marg(const Vec3& gyro_rads, const Vec3& accel, const Vec3& mag, float dt);

    const Quat& quaternion() const { return q_; }

private:
    float beta_;
    Quat q_;
};

EulerDeg to_euler_deg(const Quat& q);

}  // namespace rayneo::core
