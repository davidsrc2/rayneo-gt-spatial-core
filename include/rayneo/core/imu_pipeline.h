#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include "rayneo/core/gyro_bias_estimator.h"
#include "rayneo/core/madgwick.h"
#include "rayneo/core/types.h"

namespace rayneo::core {

struct ImuPipelineConfig {
    float madgwick_beta = 0.033f;
    bool use_magnetometer = false;  // Off: desk/laptop interference spins yaw.
    GyroBiasConfig bias;
};

// Raw HID payload -> stabilized Pose. Single-threaded, allocation-free, OS-agnostic.
class ImuPipeline {
public:
    explicit ImuPipeline(ImuPipelineConfig cfg = {});

    // Feeds one raw HID input report. Returns a pose for 0x99 0x65 reports, nullopt otherwise.
    std::optional<Pose> process_report(const uint8_t* data, size_t length);

    // Feeds an already decoded sample (useful for replay / tests).
    Pose process_sample(const ImuSample& s);

    // Next pose's relative orientation becomes identity.
    void recenter() { recenter_pending_ = true; }
    void reset();

    const GyroBiasEstimator& bias_estimator() const { return bias_; }

private:
    ImuPipelineConfig cfg_;
    MadgwickFilter filter_;
    GyroBiasEstimator bias_;

    bool have_prev_tick_ = false;
    uint32_t prev_tick_ = 0;
    bool attitude_init_ = false;
    bool recenter_pending_ = true;
    Quat reference_;
};

}  // namespace rayneo::core
