#pragma once

namespace rayneo::core {

// The optics' field of view found by the wearer in a few answers: turning the head while
// looking at a panel's edge, the panels drift WITH the head when the render FOV is too large
// and AGAINST it when too small. Each answer moves the FOV one step that way; the step halves
// whenever the answer flips (the right value was crossed), down to min_step: then it is done.
struct FovTuner {
    float lo = 22.2f, hi = 23.8f;  // the optics' range (NormalMode.opticalVFovRange)
    float fov = 23.0f;
    float step = 0.4f;
    float min_step = 0.05f;
    int last = 0;                  // the last answer: +1 against (grow), -1 with (shrink)
    bool done = false;

    void start(float start_fov, float range_lo, float range_hi, float first_step = 0.4f);
    // against_head: the panels drift against the head (FOV too small). Returns the new FOV.
    float answer(bool against_head);
};

}  // namespace rayneo::core
