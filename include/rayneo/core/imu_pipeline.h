#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include "rayneo/core/gyro_bias_estimator.h"
#include "rayneo/core/madgwick.h"
#include "rayneo/core/pose_stabilizer.h"
#include "rayneo/core/types.h"

namespace rayneo::core {

struct ImuPipelineConfig {
    float madgwick_beta = 0.033f;
    bool use_magnetometer = false;  // Off: desk/laptop interference spins yaw.
    bool stabilize = true;          // PoseStabilizer on `relative` and its angular velocity
    PoseStabilizerConfig stabilizer;
    GyroBiasConfig bias;
    float recenter_glide_s = 0.3f;  // recenter(true): ease into the new center instead of jumping
    // `relative` as the head's rotation in its own axes since the recenter (exact world lock on
    // both axes whatever the posture then) instead of the earth-frame delta (right for
    // left/right-only or up/down-only tracking, but with a head tilted at the recenter, turning
    // swept the panels diagonally).
    bool body_frame_relative = false;
    // Right after the attitude is initialised (from one accelerometer sample, maybe while the
    // head moves) a low gain takes many seconds to settle it: start at this gain and ease down
    // to madgwick_beta between startup_beta_hold_s and startup_beta_end_s.
    float startup_beta = 0.1f;
    float startup_beta_hold_s = 2.0f;
    float startup_beta_end_s = 6.0f;
};

// Madgwick gain for panels fixed in the room. The default 0.033 takes the accelerometer's
// word quickly, and breathing or a millimetre of body sway reads as tilt: simulated, the
// panels rocked 0.44 deg rms (0.76 max) with the head still; 0.01 trusts the gyro more,
// 0.16 deg. Lower lets the gyro's residual bias tilt them instead (0.002: 0.30).
inline constexpr float kWorldLockedMadgwickBeta = 0.01f;

// The head's rotation from a level version of its posture at `reference` (same heading and
// pitch, roll taken out), in head axes (X forward, Y left, Z up): content fixed in the room is
// seen at body_frame_delta^-1 * x, and it stands level with the floor even when the head (or
// the glasses on it) was tilted sideways at the recenter. Without levelling the whole row of
// panels kept that tilt and looked diagonal.
Quat body_frame_delta(const Quat& reference, const Quat& current, const Quat& sensor_to_head);

// A head orientation (head -> earth, Z up) with its roll removed: same heading and pitch.
Quat level_head(const Quat& head);

// Raw HID payload -> stabilized Pose. Single-threaded, allocation-free, OS-agnostic.
class ImuPipeline {
public:
    explicit ImuPipeline(ImuPipelineConfig cfg = {});

    // Feeds one raw HID input report. Returns a pose for 0x99 0x65 reports, nullopt otherwise.
    std::optional<Pose> process_report(const uint8_t* data, size_t length);

    // Feeds an already decoded sample (useful for replay / tests).
    Pose process_sample(const ImuSample& s);

    // Next pose's relative orientation becomes identity. With glide, the output eases there
    // over cfg.recenter_glide_s from where it was (user-triggered recenter, no visible jump).
    void recenter(bool glide = false) {
        recenter_pending_ = true;
        glide_requested_ = glide;
    }

    // Mounting of the IMU on the head: v_head = q * v_sensor * q^-1 (see HeadCalibration).
    // Defines which way is "forward" for the relative pose. Triggers a recenter.
    void set_sensor_to_head(const Quat& q) {
        sensor_to_head_ = q.normalized();
        recenter_pending_ = true;
    }
    const Quat& sensor_to_head() const { return sensor_to_head_; }
    void reset();

    const GyroBiasEstimator& bias_estimator() const { return bias_; }

private:
    ImuPipelineConfig cfg_;
    MadgwickFilter filter_;
    GyroBiasEstimator bias_;
    PoseStabilizer stabilizer_;

    bool have_prev_tick_ = false;
    uint32_t prev_tick_ = 0;
    bool attitude_init_ = false;
    float attitude_s_ = 0.0f;  // since the attitude was initialised
    bool recenter_pending_ = true;
    bool glide_requested_ = false;
    Quat glide_from_;          // output offset at the recenter, eased to identity
    float glide_t_ = 1.0f;     // 0..1 progress; 1 = no glide
    Quat last_relative_;       // last output, the glide's starting point
    Quat reference_;
    Quat heading_ref_;  // pure yaw: level frame aligned with the head's forward at recenter
    Quat sensor_to_head_;
};

}  // namespace rayneo::core
