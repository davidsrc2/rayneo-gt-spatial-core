#include "rayneo/platform/rayneo_timing.h"

#include "rayneo/core/latch_controller.h"
#include "rayneo/core/fov_tuner.h"
#include "rayneo/core/supersample_policy.h"

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

static core::FovTuner from_c(const rayneo_fov_tuner& t) {
    core::FovTuner c;
    c.lo = t.lo;
    c.hi = t.hi;
    c.fov = t.fov;
    c.step = t.step;
    c.min_step = t.min_step;
    c.last = t.last;
    c.done = t.done;
    return c;
}

static void to_c(const core::FovTuner& c, rayneo_fov_tuner* t) {
    *t = rayneo_fov_tuner{c.lo, c.hi, c.fov, c.step, c.min_step, c.last, c.done};
}

void rayneo_fov_tuner_start(rayneo_fov_tuner* t, float start_fov, float lo, float hi) {
    if (t == nullptr) return;
    core::FovTuner c;
    c.start(start_fov, lo, hi);
    to_c(c, t);
}

float rayneo_fov_tuner_answer(rayneo_fov_tuner* t, bool against_head) {
    if (t == nullptr) return 0.0f;
    core::FovTuner c = from_c(*t);
    const float fov = c.answer(against_head);
    to_c(c, t);
    return fov;
}

struct rayneo_supersample {
    core::SupersamplePolicy policy;
};

rayneo_supersample* rayneo_supersample_create(void) { return new rayneo_supersample(); }
void rayneo_supersample_destroy(rayneo_supersample* p) { delete p; }
int rayneo_supersample_update(rayneo_supersample* p, int missed, double now_s, int wanted) {
    return p != nullptr ? p->policy.update(missed, now_s, wanted) : wanted;
}

}  // extern "C"
