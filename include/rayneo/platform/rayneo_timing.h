#ifndef RAYNEO_TIMING_H
#define RAYNEO_TIMING_H

/*
 * Frame timing decisions shared by the renderers (core/latch_controller.h) over a C ABI, so
 * the macOS app (Swift) and the Windows server decide the same way.
 */

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Late latching: the pose is read margin_ms before the frame's deadline. */
typedef struct {
    float margin_ms;
    int on_time;
} rayneo_latch;

rayneo_latch rayneo_latch_start(float frame_period_ms);
/* One presented frame: missed its refresh?, its work from the wake-up until ready (GPU end, or
 * the commit when the GPU's end is not known; measured).
 * Returns whether the latch caused the miss (the margin then grew). */
bool rayneo_latch_presented(rayneo_latch* latch, bool missed, float cpu_ms, float frame_period_ms);
float rayneo_latch_cap_ms(float frame_period_ms);

#ifdef __cplusplus
}
#endif

#endif /* RAYNEO_TIMING_H */
