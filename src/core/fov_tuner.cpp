#include "rayneo/core/fov_tuner.h"

#include <algorithm>

namespace rayneo::core {

void FovTuner::start(float start_fov, float range_lo, float range_hi, float first_step) {
    lo = range_lo;
    hi = range_hi;
    fov = std::clamp(start_fov, lo, hi);
    step = first_step;
    last = 0;
    done = false;
}

float FovTuner::answer(bool against_head) {
    if (done) return fov;
    const int dir = against_head ? 1 : -1;
    if (last != 0 && dir != last) step *= 0.5f;  // crossed it: finer
    last = dir;
    if (step < min_step) {
        done = true;
        return fov;
    }
    fov = std::clamp(fov + static_cast<float>(dir) * step, lo, hi);
    return fov;
}

}  // namespace rayneo::core
