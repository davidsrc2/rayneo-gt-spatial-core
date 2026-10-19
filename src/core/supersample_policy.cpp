#include "rayneo/core/supersample_policy.h"

#include <algorithm>

namespace rayneo::core {

int SupersamplePolicy::update(int missed, double now_s, int wanted) {
    if (hold_ < 0.0f) hold_ = cfg_.hold_s;
    if (wanted <= 1) {
        lowered_ = false;
        return 1;
    }
    if (lowered_) {
        if (now_s < until_ || given_up()) return 1;
        lowered_ = false;
        restored_at_ = now_s;
        return wanted;
    }
    if (missed >= cfg_.missed_per_s) {
        // Back soon after the last try: the GPU cost is the cause, wait longer this time.
        const bool relapse = now_s - restored_at_ < cfg_.relapse_s;
        if (relapse) ++relapses_;
        hold_ = relapse ? std::min(cfg_.max_hold_s, hold_ * 2.0f) : cfg_.hold_s;
        lowered_ = true;
        until_ = now_s + hold_;
        return 1;
    }
    return wanted;
}

}  // namespace rayneo::core
