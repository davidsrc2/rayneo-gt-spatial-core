#include "rayneo/core/panel_dimmer.h"

#include <algorithm>
#include <cmath>

namespace rayneo::core {

void PanelDimmer::update(std::size_t count, int gaze, float dim, float dt) {
    b_.resize(count, 1.0f);
    dim = count > 1 ? std::clamp(dim, 0.0f, 1.0f) : 1.0f;
    if (gaze >= 0 && static_cast<std::size_t>(gaze) < count) lit_ = gaze;
    if (lit_ >= static_cast<int>(count)) lit_ = -1;
    const float k = tau_ > 0.0f ? 1.0f - std::exp(-std::max(0.0f, dt) / tau_) : 1.0f;
    animating_ = false;
    for (std::size_t i = 0; i < count; ++i) {
        const float target = (dim >= 1.0f || lit_ < 0 || static_cast<int>(i) == lit_) ? 1.0f : dim;
        float& b = b_[i];
        b += (target - b) * k;
        if (std::abs(target - b) < 0.004f) {
            b = target;
        } else {
            animating_ = true;
        }
    }
}

}  // namespace rayneo::core
