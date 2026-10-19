#pragma once

#include "rayneo/core/types.h"

namespace rayneo::core {

// Whether the glasses are on a head: a head is never perfectly still (breathing, pulse,
// posture), a table is. Per window of window_s: the rms of the gyro low-passed at lowpass_hz
// (slow micro-motion, without the sensor's white noise) around its window mean. Reference
// glasses (recording 18-10-2026): worn with the head still 0.086-1.17 deg/s, on the table
// 0.017-0.032 deg/s; the threshold sits between. Off after off_after_s of windows below it;
// on again at the first window above it (or any motion).
struct WearConfig {
    float window_s = 1.0f;
    float lowpass_hz = 3.0f;
    float worn_degs = 0.055f;
    float off_after_s = 30.0f;
};

class WearDetector {
public:
    explicit WearDetector(WearConfig cfg = {}) : cfg_(cfg) {}

    void reset() { *this = WearDetector(cfg_); }
    void update(const Vec3& gyro_degs, float dt);
    bool worn() const { return worn_; }
    float still_s() const { return still_s_; }        // time with no micro-motion so far
    float last_micro_degs() const { return last_; }   // the last window's micro-motion

private:
    WearConfig cfg_;
    bool init_ = false;
    Vec3 lp_;
    Vec3 sum_, sum_sq_;
    int n_ = 0;
    float t_ = 0.0f;
    float still_s_ = 0.0f;
    float last_ = 0.0f;
    bool worn_ = true;  // until shown otherwise: never blank a head that just started
};

}  // namespace rayneo::core
