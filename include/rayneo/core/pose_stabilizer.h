#pragma once

#include "rayneo/core/types.h"

namespace rayneo::core {

// Spherical linear interpolation between unit quaternions (shortest arc).
Quat slerp(const Quat& a, const Quat& b, float t);

// Hold-or-follow stabilizer. A world-locked image re-renders every micro-movement of the head
// and never looks as still as a plain (head-locked) monitor. So:
//  - Holding: while the head stays within enter_deg of the held orientation, the output is
//    frozen: the panels behave exactly like a normal monitor (zero jitter). Tremor, pulse
//    and sensor noise never reach the screen.
//  - Following: a real turn leaves the dead zone; the output tracks the head with a fast
//    adaptive low-pass (little lag) until the head has been slow for settle_s, then holds.
struct PoseStabilizerConfig {
    float enter_deg = 1.0f;           // dead zone: above tremor + breathing sway (~0.5-0.8 deg)
    float settle_speed_rads = 0.06f;  // ~3.4 deg/s: below this the head is "holding"
    float settle_s = 0.25f;
    float follow_min_cutoff_hz = 8.0f;
    float follow_beta = 40.0f;        // cutoff increase per rad/s while following
    float speed_cutoff_hz = 3.0f;     // low-pass on the angular velocity vector
};

// Panels fixed in the room at every moment (no dead zone, nothing frozen): sensor noise and
// tremor smoothed hard while the head is nearly still, released with speed so turns stay
// tight. Simulated (tests/core_tests.cpp): ~2x less shake at rest than an 8 Hz floor, ~0.7
// deg of lag at 60 deg/s.
inline PoseStabilizerConfig world_locked_stabilizer() {
    PoseStabilizerConfig c;
    c.enter_deg = 0.0f;
    c.follow_min_cutoff_hz = 0.3f;
    c.follow_beta = 40.0f;
    c.speed_cutoff_hz = 3.0f;  // 1.5 noticed turns late: the panels dragged with the head first
    return c;
}

class PoseStabilizer {
public:
    explicit PoseStabilizer(PoseStabilizerConfig cfg = {}) : cfg_(cfg) {}

    void reset() { init_ = false; }
    // q: orientation; omega: angular velocity (rad/s) in the frame of q's rotation axes.
    Quat update(const Quat& q, const Vec3& omega, float dt);

    bool holding() const { return holding_; }
    const Vec3& omega() const { return omega_out_; }  // zero while holding
    float speed() const { return speed_; }            // |low-passed omega|, rad/s

private:
    PoseStabilizerConfig cfg_;
    bool init_ = false;
    bool holding_ = true;
    Quat out_;
    Vec3 omega_slow_;
    Vec3 omega_out_;
    float speed_ = 0.0f;
    float slow_time_ = 0.0f;
};

// How much of the presentation-time prediction to apply at a given angular speed: none
// while holding still (it would only extrapolate noise), full during real turns.
float prediction_gain(float speed_rads, float start_rads = 0.05f, float full_rads = 0.3f);

}  // namespace rayneo::core
