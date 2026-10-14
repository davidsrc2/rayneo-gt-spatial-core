#include "rayneo/core/frame_skip.h"

#include <cmath>

namespace rayneo::core {

bool FrameSkipper::skip(const Input& in) {
    const Quat& q = in.orientation;
    const float d = std::sqrt((q.w - last_.w) * (q.w - last_.w) + (q.x - last_.x) * (q.x - last_.x) +
                              (q.y - last_.y) * (q.y - last_.y) + (q.z - last_.z) * (q.z - last_.z));
    const bool same = have_last_ && !in.forced && !in.animating && in.tracking &&
                      in.source_generation == last_generation_ && in.angular_speed_rads < 1e-4f &&
                      d < 1e-5f;  // < ~0.001 deg
    if (!same) {
        have_last_ = true;
        last_ = q;
        last_generation_ = in.source_generation;
    }
    return same;
}

}  // namespace rayneo::core
