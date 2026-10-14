#pragma once

#include <cstddef>
#include <vector>

namespace rayneo::core {

// Brightness per panel: the one you look at stays at 1, the others ease to `dim`. Between
// panels (gaze -1) the last one looked at stays lit. Frame-rate independent; converged
// values snap so animating() turns off and still frames can be skipped (see FrameSkipper).
class PanelDimmer {
public:
    explicit PanelDimmer(float time_constant_s = 0.15f) : tau_(time_constant_s) {}

    // gaze: panel index or -1; dim: brightness of the other panels (1 = off); dt in seconds.
    void update(std::size_t count, int gaze, float dim, float dt);

    float brightness(std::size_t panel) const { return panel < b_.size() ? b_[panel] : 1.0f; }
    bool animating() const { return animating_; }
    int lit_panel() const { return lit_; }

private:
    float tau_;
    std::vector<float> b_;
    int lit_ = -1;
    bool animating_ = false;
};

}  // namespace rayneo::core
