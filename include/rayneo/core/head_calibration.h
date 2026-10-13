#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "rayneo/core/types.h"

namespace rayneo::core {

// Sensor -> head mounting from a guided look-around. All vectors are in the sensor frame.
// Each rot_* is the integrated bias-corrected gyro (deg) from looking straight ahead to
// holding the target direction; small-angle single-axis motions keep this a rotation vector.
struct HeadCalibrationInput {
    Vec3 gravity_center;  // mean accel at rest, looking straight ahead with a level head
    Vec3 rot_up;
    Vec3 rot_down;
    Vec3 rot_left;
    Vec3 rot_right;
};

struct HeadCalibrationResult {
    bool ok = false;
    Quat sensor_to_head;           // v_head = q * v_sensor * q^-1
    float yaw_axis_error_deg = 0;  // gyro yaw axis vs gravity (quality metric)
    float pitch_axis_tilt_deg = 0; // gyro pitch axis vs horizontal plane (quality metric)
    std::string error;
};

struct HeadCalibrationLimits {
    float min_rotation_deg = 10.0f;
    float max_yaw_axis_error_deg = 20.0f;
    float max_pitch_axis_tilt_deg = 25.0f;
};

// Up = gravity at center; left = pitch axis (look down - look up) orthogonalized against up;
// forward = left x up. The left/right turns only validate the result.
HeadCalibrationResult solve_head_calibration(const HeadCalibrationInput& in,
                                             const HeadCalibrationLimits& lim = {});

// "Holding still" for a worn head. The bias estimator's rest gate (1.5 deg/s, 1 s
// holdoff) is built for a device on a desk; pulse, breathing and micro-tremor of a worn
// head never meet it. This gates on the smoothed bias-corrected rate instead.
struct HoldDetectorConfig {
    float tau_s = 0.1f;       // EMA of |gyro|
    float hold_degs = 4.0f;   // below: candidate hold
    float move_degs = 10.0f;  // above: deliberate motion
    float hold_s = 0.6f;      // continuous time below hold_degs
};

class HoldDetector {
public:
    explicit HoldDetector(HoldDetectorConfig cfg = {}) : cfg_(cfg) {}

    void reset() { *this = HoldDetector(cfg_); }
    void update(const Vec3& gyro_corrected_degs, float dt);

    float rate_degs() const { return rate_; }
    bool moving() const { return rate_ > cfg_.move_degs; }
    bool holding() const { return hold_time_ >= cfg_.hold_s; }

private:
    HoldDetectorConfig cfg_;
    bool init_ = false;
    float rate_ = 0.0f;
    float hold_time_ = 0.0f;
};

std::string calibration_to_json(const Quat& sensor_to_head);
// Accepts the format written by calibration_to_json; rejects non-unit quaternions.
std::optional<Quat> calibration_from_json(std::string_view json);

}  // namespace rayneo::core
