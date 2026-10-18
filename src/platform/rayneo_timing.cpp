#include "rayneo/platform/rayneo_timing.h"

#include "rayneo/core/latch_controller.h"

namespace core = rayneo::core;

extern "C" {

rayneo_latch rayneo_latch_start(float frame_period_ms) {
    const core::LatchState s = core::latch_start(frame_period_ms);
    return rayneo_latch{s.margin_ms, s.on_time};
}

bool rayneo_latch_presented(rayneo_latch* latch, bool missed, float cpu_ms, float frame_period_ms) {
    if (latch == nullptr) return false;
    const core::LatchState before{latch->margin_ms, latch->on_time};
    const bool caused = missed && core::latch_caused_miss(before, cpu_ms);
    const core::LatchState s = core::latch_presented(before, missed, cpu_ms, frame_period_ms);
    latch->margin_ms = s.margin_ms;
    latch->on_time = s.on_time;
    return caused;
}

float rayneo_latch_cap_ms(float frame_period_ms) { return core::latch_cap_ms(frame_period_ms); }

}  // extern "C"
