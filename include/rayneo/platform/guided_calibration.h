#pragma once

// Guided sensor -> head calibration against the live glasses: bias on the table, then
// centre, up, down, left, right. Shared by imu_probe (CLI) and the macOS app (wizard).

#include <atomic>
#include <functional>
#include <string>

#include "rayneo/core/head_calibration.h"

namespace rayneo::platform {

enum class CalibStep { Bias, Center, Up, Down, Left, Right };

struct CalibStatus {
    CalibStep step = CalibStep::Bias;
    bool returning = false;   // a direction was captured; waiting for the head back at centre
    const char* hint = "";    // "waiting for rest", "put the glasses on", "move", "further", "hold"
    float rate_degs = 0.0f;   // smoothed head rate
    float rotation_deg = 0.0f;
    bool bias_ok = false;
    float captured_deg = 0.0f;  // size of the last captured direction (0 if none yet)
};

struct GuidedCalibrationOutcome {
    bool completed = false;  // all movements captured; `result` holds the solver's verdict
    std::string error;       // why the flow stopped (no device, timeout, cancel) if !completed
    core::HeadCalibrationResult result;
};

class GuidedCalibration {
public:
    using StatusFn = std::function<void(const CalibStatus&)>;

    // Blocking. Opens the glasses, runs the whole flow and solves. `on_status` is called
    // ~10 Hz and on every step change, from the calling thread.
    GuidedCalibrationOutcome run(const std::atomic<bool>& cancel, const StatusFn& on_status);
};

}  // namespace rayneo::platform
