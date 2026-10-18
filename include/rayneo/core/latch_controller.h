#pragma once

namespace rayneo::core {

// Late latching: the head pose is read this long (the margin) before the frame's deadline, so
// as little of the head's motion as possible has to be predicted. The margin grows only when a
// missed refresh is the latch's fault: the frame's work from the wake-up until it was ready
// (its GPU done; the commit where the GPU's end is not known) took longer than the margin
// minus some slack. A refresh lost to the compositor or the display (the frame was ready in
// time) leaves it where it is: before,
// every such refresh widened it, and it sat at 12 ms (more than a 120 Hz period) for good.
// A long run on time narrows it again, a step at a time.
struct LatchConfig {
    float max_ms = 4.0f;          // cap: min(max_ms, max_period_fraction * frame period)
    float max_period_fraction = 0.5f;
    float min_ms = 1.0f;
    float start_ms = 2.0f;
    float grow_ms = 0.5f;          // per missed refresh the latch caused
    float slack_ms = 1.0f;         // the latch's fault: cpu > margin - slack
    float shrink_ms = 0.25f;       // per shrink_after frames on time
    int shrink_after = 240;
};

struct LatchState {
    float margin_ms = 0.0f;
    int on_time = 0;  // frames on time since the last change
};

float latch_cap_ms(float frame_period_ms, const LatchConfig& c = {});
LatchState latch_start(float frame_period_ms, const LatchConfig& c = {});
// The margin of a refresh that missed its target only grows when the latch caused it.
bool latch_caused_miss(const LatchState& s, float cpu_ms, const LatchConfig& c = {});
// One presented frame: whether it missed its refresh, and its measured work from the wake-up
// (pose read) until it was ready (cpu_ms: GPU end, or the commit). Also re-clamps to the cap
// of the current period. On macOS the GPU's end matters: committed 0.4 ms before the deadline
// with 1.5-2.5 ms of GPU work left, frames went a refresh late for seconds at a time.
LatchState latch_presented(LatchState s, bool missed, float cpu_ms, float frame_period_ms, const LatchConfig& c = {});

}  // namespace rayneo::core
