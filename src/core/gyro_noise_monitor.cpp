#include "rayneo/core/gyro_noise_monitor.h"

#include <algorithm>
#include <cmath>

namespace rayneo::core {

void GyroNoiseMonitor::reset() { *this = GyroNoiseMonitor(cfg_); }

void GyroNoiseMonitor::update(const Vec3& gyro_degs, bool rest, float dt) {
    if (!rest || dt <= 0.0f) {  // a window counts only if the head rested all through it
        have_prev_ = false;
        sum_sq_ = 0.0;
        n_ = 0;
        t_ = 0.0f;
        return;
    }
    if (have_prev_) {
        const Vec3 d = gyro_degs - prev_;
        sum_sq_ += 0.5 * (static_cast<double>(d.x) * d.x + static_cast<double>(d.y) * d.y + static_cast<double>(d.z) * d.z);
        ++n_;
    }
    prev_ = gyro_degs;
    have_prev_ = true;
    t_ += dt;
    if (t_ < cfg_.window_s || n_ == 0) return;
    last_ = static_cast<float>(std::sqrt(sum_sq_ / n_ / 3.0));  // per axis
    history_[count_ % kMax] = last_;
    ++count_;
    sum_sq_ = 0.0;
    n_ = 0;
    t_ = 0.0f;
}

bool GyroNoiseMonitor::noisy() const {
    const int k = std::min({count_, cfg_.windows, kMax});
    if (k < cfg_.windows) return false;
    float v[kMax];
    for (int i = 0; i < k; ++i) v[i] = history_[(count_ - 1 - i) % kMax];
    std::nth_element(v, v + k / 2, v + k);
    return v[k / 2] > cfg_.noisy_degs;
}

}  // namespace rayneo::core
