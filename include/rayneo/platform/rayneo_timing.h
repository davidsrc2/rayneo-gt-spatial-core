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

/* Supersampling lowered to 1 while the glasses show frames a refresh late
 * (core/supersample_policy.h). Once a second: missed refreshes in it, the time (s), the factor
 * wanted; returns the factor to draw with. */
/* The optics' field of view found from the wearer's answers (core/fov_tuner.h). */
typedef struct {
    float lo, hi, fov, step, min_step;
    int last;
    bool done;
} rayneo_fov_tuner;
void rayneo_fov_tuner_start(rayneo_fov_tuner* t, float start_fov, float lo, float hi);
/* against_head: the panels drift against the head when turning (FOV too small). New FOV. */
float rayneo_fov_tuner_answer(rayneo_fov_tuner* t, bool against_head);

typedef struct rayneo_supersample rayneo_supersample;
rayneo_supersample* rayneo_supersample_create(void);
void rayneo_supersample_destroy(rayneo_supersample* p);
int rayneo_supersample_update(rayneo_supersample* p, int missed, double now_s, int wanted);

#ifdef __cplusplus
}
#endif

#endif /* RAYNEO_TIMING_H */
