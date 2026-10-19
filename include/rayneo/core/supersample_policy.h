#pragma once

namespace rayneo::core {

// Supersampling (crisper text) costs GPU time, and when the GPU's frame ends after
// WindowServer's latch the glasses show every frame a refresh late (the "late state": a step
// at each switch in and out). Measured: with supersample 1 it did not appear. So: the wanted
// factor while all is well, 1 for a while when a second has many missed refreshes, then the
// wanted factor tried again; each relapse soon after doubles the wait (up to max_hold_s).
struct SupersamplePolicyConfig {
    int missed_per_s = 30;      // a second this bad means the late state
    float hold_s = 20.0f;       // first time at 1
    float max_hold_s = 120.0f;
    float relapse_s = 30.0f;    // late again within this long of restoring: hold twice as long
    // This many relapses: 1 for the rest of the session. Every try of 2 was a stutter and a jump
    // in sharpness, and on a Mac capturing several monitors it relapsed within seconds each
    // time (logs of 19-10-2026: 13 tries in an evening).
    int max_relapses = 2;
};

class SupersamplePolicy {
public:
    explicit SupersamplePolicy(SupersamplePolicyConfig cfg = {}) : cfg_(cfg) {}

    // Once a second: refreshes missed in it, and the time (s, any monotonic clock).
    // Returns the factor to draw with: `wanted`, or 1 while held down.
    int update(int missed, double now_s, int wanted);
    bool lowered() const { return lowered_; }
    bool given_up() const { return relapses_ >= cfg_.max_relapses; }
    float hold_s() const { return hold_; }

private:
    SupersamplePolicyConfig cfg_;
    bool lowered_ = false;
    double until_ = 0.0;
    double restored_at_ = -1e9;
    float hold_ = -1.0f;
    int relapses_ = 0;
};

}  // namespace rayneo::core
