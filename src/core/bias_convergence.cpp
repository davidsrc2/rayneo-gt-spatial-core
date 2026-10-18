#include "rayneo/core/bias_convergence.h"

#include <cmath>

namespace rayneo::core {

void BiasConvergence::reset() {
    open_ = false;
    stable_ = 0;
}

void BiasConvergence::update(const Vec3& bias_degs, float temp_c, bool rest, float dt) {
    if (converged() || dt <= 0.0f) return;  // settled once: stays (the caller decides when to ask again)
    if (!open_) {
        open_ = true;
        window_rest_ = true;
        window_t_ = 0.0f;
        start_bias_ = bias_degs;
        start_temp_ = temp_c;
    }
    window_rest_ = window_rest_ && rest;
    window_t_ += dt;
    if (window_t_ < cfg_.window_s) return;
    open_ = false;
    const bool steady = (bias_degs - start_bias_).norm() < cfg_.eps_degs && std::fabs(temp_c - start_temp_) < cfg_.temp_c;
    stable_ = window_rest_ && steady ? stable_ + 1 : 0;
}

}  // namespace rayneo::core
