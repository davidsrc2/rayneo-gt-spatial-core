#pragma once

#include "rayneo/core/types.h"

namespace rayneo::core {

// The gyro's white noise, measured while the head rests, to tell the user when it is far above
// what the sensor normally shows (a poor USB cable or port, a noisy hub): with it the bias is
// learned badly and the panels drift. Per window of window_s spent at rest from start to end,
// sigma = rms(difference of consecutive samples) / sqrt(2): white noise passes, slow motion
// (breathing, tremor with the glasses on) hardly does. Noisy when the median of the last
// `windows` such windows exceeds noisy_degs (per axis). Reference glasses: 0.05-0.16 deg/s
// per axis at rest (recording of 18-10-2026), so the default is ~3x the worst of it.
struct GyroNoiseConfig {
    float window_s = 2.0f;
    int windows = 5;
    float noisy_degs = 0.5f;
};

class GyroNoiseMonitor {
public:
    explicit GyroNoiseMonitor(GyroNoiseConfig cfg = {}) : cfg_(cfg) {}

    void reset();
    // One raw sample (deg/s), whether the head rests, seconds elapsed.
    void update(const Vec3& gyro_degs, bool rest, float dt);
    bool noisy() const;
    // The last rest window's noise (deg/s); 0 before any.
    float noise_degs() const { return last_; }
    int windows_measured() const { return count_; }

private:
    static constexpr int kMax = 16;
    GyroNoiseConfig cfg_;
    bool have_prev_ = false;
    Vec3 prev_;
    double sum_sq_ = 0.0;
    int n_ = 0;
    float t_ = 0.0f;
    float history_[kMax] = {};
    int count_ = 0;  // windows measured (all, the history keeps the last kMax)
    float last_ = 0.0f;
};

}  // namespace rayneo::core
