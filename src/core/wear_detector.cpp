#include "rayneo/core/wear_detector.h"

#include <cmath>

namespace rayneo::core {

void WearDetector::update(const Vec3& g, float dt) {
    if (dt <= 0.0f || dt > 0.1f) return;
    const float tau = 1.0f / (2.0f * 3.14159265f * cfg_.lowpass_hz);
    if (!init_) {
        lp_ = g;
        init_ = true;
    }
    lp_ = lp_ + (g - lp_) * (dt / (tau + dt));
    sum_ = sum_ + lp_;
    sum_sq_ = sum_sq_ + Vec3{lp_.x * lp_.x, lp_.y * lp_.y, lp_.z * lp_.z};
    ++n_;
    t_ += dt;
    if (t_ < cfg_.window_s) return;
    const float inv = 1.0f / static_cast<float>(n_);
    const Vec3 m = sum_ * inv;
    const Vec3 var = sum_sq_ * inv - Vec3{m.x * m.x, m.y * m.y, m.z * m.z};
    last_ = std::sqrt(std::fmax(0.0f, var.x) + std::fmax(0.0f, var.y) + std::fmax(0.0f, var.z));
    if (last_ >= cfg_.worn_degs) {
        still_s_ = 0.0f;
        worn_ = true;
    } else {
        still_s_ += t_;
        if (still_s_ >= cfg_.off_after_s) worn_ = false;
    }
    sum_ = sum_sq_ = Vec3{};
    n_ = 0;
    t_ = 0.0f;
}

}  // namespace rayneo::core
