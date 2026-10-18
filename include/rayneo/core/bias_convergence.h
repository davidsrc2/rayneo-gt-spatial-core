#pragma once

#include "rayneo/core/types.h"

namespace rayneo::core {

// Whether a gyro bias estimate has settled ("freeze" in RealWarp: R4FreezeEps / Windows /
// TempC): over consecutive windows of window_s with the head at rest the whole window, the
// estimate changed less than eps_degs and the sensor temperature less than temp_c; windows
// is how many in a row. A window with motion in it does not count (the estimate does not
// learn then, so it would look settled while still wrong) and restarts the count, as does a
// window that changed too much.
struct BiasConvergenceConfig {
    int windows = 4;
    float window_s = 0.5f;
    float eps_degs = 0.02f;   // |bias change| over one window, deg/s
    float temp_c = 0.3f;      // |temperature change| over one window
};

class BiasConvergence {
public:
    explicit BiasConvergence(BiasConvergenceConfig cfg = {}) : cfg_(cfg) {}

    void reset();
    // One sample: the estimate (deg/s), the temperature, the head at rest, seconds elapsed.
    void update(const Vec3& bias_degs, float temp_c, bool rest, float dt);
    bool converged() const { return stable_ >= cfg_.windows; }
    int stable_windows() const { return stable_; }

private:
    BiasConvergenceConfig cfg_;
    bool open_ = false;  // a window is being measured
    bool window_rest_ = true;
    float window_t_ = 0.0f;
    Vec3 start_bias_;
    float start_temp_ = 0.0f;
    int stable_ = 0;
};

}  // namespace rayneo::core
