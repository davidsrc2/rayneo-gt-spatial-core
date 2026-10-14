#pragma once

#include <cstdint>

#include "rayneo/core/types.h"

namespace rayneo::core {

// Decides whether the frame about to be drawn equals the one on screen: head still (same
// orientation, no angular velocity), no new source frame, no fade, no forced redraw. Then
// the renderer skips it (less GPU work, less heat). Call once per display refresh.
class FrameSkipper {
public:
    struct Input {
        Quat orientation;
        float angular_speed_rads = 0.0f;
        uint64_t source_generation = 0;  // bumps on every new captured frame
        bool tracking = false;
        bool animating = false;           // e.g. PanelDimmer fading
        bool forced = false;              // settings changed
    };

    // True: skip this frame. Remembers the input as the frame on screen otherwise.
    bool skip(const Input& in);

    // The screen content is unknown (e.g. no pose drawn a placeholder): never skip next.
    void invalidate() { have_last_ = false; }

private:
    bool have_last_ = false;
    Quat last_;
    uint64_t last_generation_ = 0;
};

}  // namespace rayneo::core
