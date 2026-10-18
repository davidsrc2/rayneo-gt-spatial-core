#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include "rayneo/core/bias_convergence.h"
#include "rayneo/core/bias_map.h"
#include "rayneo/core/gyro_bias_estimator.h"
#include "rayneo/core/madgwick.h"
#include "rayneo/core/pose_stabilizer.h"
#include "rayneo/core/types.h"
#include "rayneo/core/vqf.h"

namespace rayneo::core {

struct ImuPipelineConfig {
    float madgwick_beta = 0.033f;
    // The space at real size: orientation from Vqf6D instead of Madgwick, its gyro bias (Kalman
    // at rest, started from the temperature map when it knows the temperature), its rest
    // detection locking the stabilizer (lock_on_rest) and teaching the map.
    bool use_vqf = false;
    VqfConfig vqf;
    // use_vqf, start-up: the temperature map seeds VQF's bias (trusted) when it learned within
    // bias_map_seed_gap_c of the temperature now. Without that, the start-up calibration's
    // bias is rough (0.3-0.5 deg/s off the converged one on the reference glasses: panels
    // drifting ~5 deg in yaw the first seconds): with hold_until_bias_settles, tracking waits
    // until VQF's own estimate settles (bias_convergence) or bias_settle_max_s go by, and the
    // settled bias is taught to the map as trusted for the next start.
    float bias_map_seed_gap_c = 1.5f;
    // Nothing learned that close: the map's linear trend over temperature starts VQF instead of
    // the start-up calibration, less trusted (bias_trend_std_degs), and tracking still waits for
    // the bias to settle.
    float bias_trend_std_degs = 0.15f;
    bool hold_until_bias_settles = false;
    BiasConvergenceConfig bias_convergence;
    float bias_settle_max_s = 12.0f;
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
    // With body_frame_relative: the reference keeps only the heading (pitch taken out too), so
    // the relative frame stands on the floor: "up" is the world's vertical, not the head's
    // pitch at the recenter. Recentered looking down, the panels were laid on a tilted circle
    // and swept diagonally when turning about the vertical.
    bool level_reference = false;
    // Right after the attitude is initialised (from one accelerometer sample, maybe while the
    // head moves) a low gain takes many seconds to settle it: start at this gain and ease down
    // to madgwick_beta between startup_beta_hold_s and startup_beta_end_s.
    float startup_beta = 0.1f;
    float startup_beta_hold_s = 2.0f;
    float startup_beta_end_s = 6.0f;
};

// The space at real size (fixed in the room), on top of `base`: stationary lock locked by VQF's
// rest detection, orientation and gyro bias from Vqf6D, body-frame relative pose on a level
// reference, tracking held until the start-up bias settles. The engine and the replay tools
// use this one definition.
ImuPipelineConfig real_size_pipeline_config(ImuPipelineConfig base = {});

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
// heading_only: the reference levelled on both axes (see ImuPipelineConfig::level_reference).
Quat body_frame_delta(const Quat& reference, const Quat& current, const Quat& sensor_to_head, bool heading_only = false);

// A head orientation reduced to its heading (pitch and roll taken out).
Quat heading_only(const Quat& head);

// The roll of a head orientation (head -> earth), as a rotation about head X: head =
// level_head(head) * head_roll(head).
Quat head_roll(const Quat& head);

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
    // use_vqf: how the start-up bias was decided (seeded from the map, settled, or held).
    bool bias_seeded_from_map() const { return bias_seeded_; }
    bool bias_settled() const { return bias_settled_; }
    // The gyro bias per temperature (use_vqf), loaded and saved by the caller.
    void set_bias_map(const BiasMap& map) { bias_map_ = map; }
    BiasMap& bias_map() { return bias_map_; }

private:
    ImuPipelineConfig cfg_;
    MadgwickFilter filter_;
    GyroBiasEstimator bias_;
    PoseStabilizer stabilizer_;
    Vqf6D vqf_;
    BiasMap bias_map_;
    BiasConvergence convergence_;
    bool bias_seeded_ = false;
    bool bias_settled_ = false;

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
    // level_reference: the head's roll at the recenter, taken as "upright" (a mounting or
    // calibration error of a few degrees, or a head held slightly tilted): with it left in,
    // every panel came up leaning right after Ctrl+Alt+C.
    Quat roll_fix_;
    Quat sensor_to_head_;
};

}  // namespace rayneo::core
