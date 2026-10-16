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
    // > 0: omega() is the angular velocity low-passed here (for the renderer's prediction)
    // instead of the speed filter's, and never zeroed: a slow one predicts turns late.
    float predict_cutoff_hz = 0.0f;
    // The output is advanced by the follow filter's own delay (1 / (2 pi cutoff)) along omega(),
    // ramped in with speed: the smoothing then costs no lag while the head turns.
    bool compensate_lag = false;
    // > 0: an angular acceleration above this (rad/s^2; a tap on the frame is ~170, a brisk
    // head turn under 20) is a knock, not a movement: for tap_hold_s the prediction and the
    // lag compensation use the slow angular velocity, which barely sees it. Predicting the
    // knock from the quick one tripled it.
    float tap_accel_rads2 = 0.0f;
    float tap_hold_s = 0.12f;
    // > 0, with a dead zone (anchor at rest): leaving it slowly (breathing, sway: below
    // settle_speed_rads) glides to the head at this cutoff instead of catching up at once; a
    // real turn still follows at the follow cutoff. The catch-up of the sway read as a tremor.
    float release_cutoff_hz = 0.0f;
};

// Panels fixed in the room at every moment (no dead zone, nothing frozen). Tuned on a
// simulated chain (head looking around with tremor, sensor noise, 30 ms to the glasses;
// error = where a panel is shown vs where it is in the room): 0.027 deg rms at rest, 0.12 deg
// while looking around, against 0.054 / 0.44 with a 0.3 Hz floor and the slow speed filter
// driving the prediction (it noticed turns late: the panels dragged with the head).
inline PoseStabilizerConfig world_locked_stabilizer() {
    PoseStabilizerConfig c;
    c.enter_deg = 0.0f;
    c.follow_min_cutoff_hz = 2.0f;
    c.follow_beta = 40.0f;
    c.speed_cutoff_hz = 3.0f;
    c.predict_cutoff_hz = 15.0f;
    c.compensate_lag = true;
    c.tap_accel_rads2 = 60.0f;
    return c;
}

// Speeds (rad/s) between which the renderers ramp their presentation-time prediction in.
inline constexpr float kPredictStartRads = 0.02f;
inline constexpr float kPredictFullRads = 0.1f;

class PoseStabilizer {
public:
    explicit PoseStabilizer(PoseStabilizerConfig cfg = {}) : cfg_(cfg) {}

    void reset() { init_ = false; }
    // q: orientation; omega: angular velocity (rad/s) in the frame of q's rotation axes.
    Quat update(const Quat& q, const Vec3& omega, float dt);

    bool holding() const { return holding_; }
    // See predict_cutoff_hz; the slow one right after a knock (tap_accel_rads2).
    Vec3 omega() const {
        if (cfg_.predict_cutoff_hz <= 0.0f || (holding_ && cfg_.enter_deg > 0.0f)) return omega_out_;  // anchored: nothing to predict
        return tap_left_s_ > 0.0f ? omega_slow_ : omega_fast_;
    }
    float speed() const { return speed_; }            // |low-passed omega|, rad/s

private:
    PoseStabilizerConfig cfg_;
    bool init_ = false;
    bool holding_ = true;
    Quat out_;
    Vec3 omega_slow_;
    Vec3 omega_out_;
    Vec3 omega_fast_;
    float tap_left_s_ = 0.0f;
    Quat finish() const;  // out_, advanced by the filter's delay when compensating lag
    float speed_ = 0.0f;
    float slow_time_ = 0.0f;
};

// How much of the presentation-time prediction to apply at a given angular speed: none
// while holding still (it would only extrapolate noise), full during real turns.
float prediction_gain(float speed_rads, float start_rads = 0.05f, float full_rads = 0.3f);

}  // namespace rayneo::core
