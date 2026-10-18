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
    // > 0: the dead zone up / down (pitch) and tilting (roll), wider than enter_deg (left /
    // right): the head bobs vertically far more than it drifts sideways at rest (breathing,
    // talking, chewing, a step), and each bob past the round zone let go and shook the panels up
    // and down. An ellipse: a real look up or down still leaves it.
    float enter_vertical_deg = 0.0f;
    // > 0: tremor damping per axis. A head turning on purpose keeps its direction; tremor and
    // the jolts of a moving head reverse it again and again. Each reversal of an axis's angular
    // velocity (beyond reversal_threshold_rads) within reversal_window_s of the previous one
    // raises that axis's damping (decaying over reversal_decay_s); while damped the axis is
    // smoothed down to reversal_min_cutoff_hz and not predicted (predicting a tremor 30 ms
    // ahead doubled it). A steady turn has no reversals: followed as before.
    float reversal_decay_s = 0.0f;
    float reversal_window_s = 0.3f;
    float reversal_threshold_rads = 0.02f;
    float reversal_min_cutoff_hz = 1.0f;
    float reversal_speed_rads = 0.0f;  // > 0: damping fades out as the axis turns faster than this

    // Stationary lock (the space at real size): replaces everything above. Two phases:
    //  LOCKED: the output is the anchor, frozen, while the head's speed stays under
    //    unlock_rads and its angle from the anchor under unlock_deg (the band above
    //    0.018 rad/s and 0.12 deg is the hysteresis: still locked). Nothing to predict.
    //  UNLOCKING: a smoothstep over unlock_ramp_s from the anchor to the head itself; then
    //    TRACKING: the head 1:1, no filtering, its instantaneous angular velocity for the
    //    prediction.
    //  Back to LOCKED once the speed stays under relock_rads for relock_s (anchor = the head).
    // Speeds are those of the gyro low-passed at speed_cutoff_hz (1.5 Hz): raw, the tremor
    // alone (~2 deg/s at 8 Hz) crossed unlock_rads all the time, and at 3 Hz it hovered at
    // relock_rads and the panels never locked again; a real turn still unlocks within ms.
    bool stationary_lock = false;
    float unlock_rads = 0.025f;
    float unlock_deg = 0.15f;
    float unlock_ramp_s = 0.012f;  // 40 ms let the panels lag up to 0.4 deg into a turn; released early, 12 ms is enough
    float relock_rads = 0.012f;
    float relock_s = 0.150f;
    // Locked and unlocked by an outside rest detector instead (set_rest: the orientation
    // filter's "bias gate"), the speed thresholds unused; locked, a drift past lock_max_deg
    // from the anchor still unlocks (a very slow turn reads as rest).
    bool lock_on_rest = false;
    float lock_max_deg = 1.0f;
    // ...and the start of a turn unlocks at once, before the rest detector (low-passed, ~0.1 s
    // late) notices: the gyro itself past lock_motion_rads with the head already lock_motion_deg
    // off the anchor. Waiting for the detector, the frozen panels went along with the head for
    // the first fraction of a degree of every turn and then caught up: a small slide each time
    // the gaze moved to another monitor. The offset condition keeps tremor at rest locked.
    float lock_motion_rads = 0.07f;  // 4 deg/s
    float lock_motion_deg = 0.05f;
};

// The space at real size: the stationary lock (see PoseStabilizerConfig::stationary_lock).
inline PoseStabilizerConfig stationary_lock_stabilizer() {
    PoseStabilizerConfig c;
    c.stationary_lock = true;
    c.enter_deg = 0.0f;
    c.speed_cutoff_hz = 1.5f;
    return c;
}

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
    // Tremor damping fading out above ~11 deg/s. Simulated (tremor 0.15 deg at 8-9 Hz, head
    // nearly still): frame-to-frame unevenness 0.148 -> 0.033 deg; looking around, the error
    // 0.138 -> 0.152 deg rms and unevenness 0.054 -> 0.051.
    c.reversal_decay_s = 0.25f;
    c.reversal_speed_rads = 0.2f;
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
    // Stationary lock: how long the current LOCKED phase has lasted (0 when not locked).
    float locked_seconds() const { return locked_s_; }
    // lock_on_rest: whether the head rests now (before update).
    void set_rest(bool rest) { rest_hint_ = rest; }
    // See predict_cutoff_hz; the slow one right after a knock (tap_accel_rads2).
    Vec3 omega() const {
        if (cfg_.stationary_lock) return lock_omega_;
        if (cfg_.predict_cutoff_hz <= 0.0f || (holding_ && cfg_.enter_deg > 0.0f)) return omega_out_;  // anchored: nothing to predict
        const Vec3 w = tap_left_s_ > 0.0f ? omega_slow_ : omega_fast_;
        return {w.x * (1.0f - effective_damp(0)), w.y * (1.0f - effective_damp(1)), w.z * (1.0f - effective_damp(2))};  // a trembling axis: not predicted
    }
    // Tremor damping of each axis (x, y, z of omega), 0..1.
    Vec3 damping() const { return {damp_[0], damp_[1], damp_[2]}; }
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
    // Tremor damping (reversal_decay_s).
    float damp_[3] = {0, 0, 0};
    int sign_[3] = {0, 0, 0};
    float since_flip_[3] = {1e3f, 1e3f, 1e3f};
    void update_damping(const Vec3& omega, float dt);
    // Stationary lock.
    enum class Lock { Locked, Unlocking, Tracking };
    Lock lock_ = Lock::Locked;
    Quat anchor_;
    float ramp_t_ = 0.0f, calm_s_ = 0.0f, locked_s_ = 0.0f;
    bool rest_hint_ = false;
    Vec3 lock_omega_;
    Quat update_lock(const Quat& q, const Vec3& omega, float dt);
    float effective_damp(int k) const;
};

// How much of the presentation-time prediction to apply at a given angular speed: none
// while holding still (it would only extrapolate noise), full during real turns.
float prediction_gain(float speed_rads, float start_rads = 0.05f, float full_rads = 0.3f);

}  // namespace rayneo::core
