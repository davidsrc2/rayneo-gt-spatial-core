#pragma once

#include "rayneo/core/types.h"

namespace rayneo::core {

// Spherical linear interpolation between unit quaternions (shortest arc).
Quat slerp(const Quat& a, const Quat& b, float t);

// Adaptive low-pass on orientation (One Euro filter on SO(3)). Looking at a fixed spot,
// head tremor and gyro noise are removed so the panels stand still; during a real turn the
// cutoff rises with angular speed so the panels do not lag behind the head.
struct PoseStabilizerConfig {
    float min_cutoff_hz = 0.3f;    // at rest
    float beta = 20.0f;            // cutoff increase per rad/s of (smoothed) angular speed
    float speed_cutoff_hz = 1.5f;  // low-pass on the angular velocity vector for the speed
};

class PoseStabilizer {
public:
    explicit PoseStabilizer(PoseStabilizerConfig cfg = {}) : cfg_(cfg) {}

    void reset() { init_ = false; }
    // q: orientation; omega: angular velocity (rad/s) in the frame of q's rotation axes.
    Quat update(const Quat& q, const Vec3& omega, float dt);

    const Vec3& omega() const { return omega_; }  // smoothed with the same adaptive cutoff
    float speed() const { return speed_; }        // smoothed |omega|, rad/s
    float cutoff_hz() const { return cutoff_; }

private:
    PoseStabilizerConfig cfg_;
    bool init_ = false;
    Quat state_;
    Vec3 omega_;
    Vec3 omega_slow_;
    float speed_ = 0.0f;
    float cutoff_ = 0.0f;
};

// How much of the presentation-time prediction to apply at a given angular speed: none
// while holding still (it would only extrapolate noise), full during real turns.
float prediction_gain(float speed_rads, float start_rads = 0.05f, float full_rads = 0.3f);

}  // namespace rayneo::core
