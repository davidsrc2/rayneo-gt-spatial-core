#include "rayneo/core/latch_controller.h"

#include <algorithm>

namespace rayneo::core {

float latch_cap_ms(float frame_period_ms, const LatchConfig& c) {
    return std::max(0.0f, std::min(c.max_ms, c.max_period_fraction * frame_period_ms));
}

LatchState latch_start(float frame_period_ms, const LatchConfig& c) {
    const float cap = latch_cap_ms(frame_period_ms, c);
    return LatchState{std::clamp(c.start_ms, std::min(c.min_ms, cap), cap), 0};
}

bool latch_caused_miss(const LatchState& s, float cpu_ms, const LatchConfig& c) {
    return cpu_ms > s.margin_ms - c.slack_ms;
}

LatchState latch_presented(LatchState s, bool missed, float cpu_ms, float frame_period_ms, const LatchConfig& c) {
    const float cap = latch_cap_ms(frame_period_ms, c);
    const float floor = std::min(c.min_ms, cap);
    if (missed) {
        if (latch_caused_miss(s, cpu_ms, c)) {
            s.margin_ms += c.grow_ms;
            s.on_time = 0;
        }
        // Otherwise the compositor's: neither a reason to read earlier nor a frame on time.
    } else if (++s.on_time >= c.shrink_after) {
        s.margin_ms -= c.shrink_ms;
        s.on_time = 0;
    }
    s.margin_ms = std::clamp(s.margin_ms, floor, cap);
    return s;
}

}  // namespace rayneo::core
