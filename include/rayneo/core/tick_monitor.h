#pragma once

#include <cstdint>

namespace rayneo::core {

// Device-clock sample-rate and loss accounting from the 0x99 0x65 tick (100 us units).
// Host timestamps carry USB/scheduler jitter; the device tick is the ground truth.
class TickMonitor {
public:
    void reset() { *this = TickMonitor(); }
    void update(uint32_t tick);

    uint64_t samples() const { return samples_; }
    // Total device time covered by consecutive samples, in ticks.
    uint64_t span_ticks() const { return span_ticks_; }
    // Samples the device produced but we never saw, inferred from tick gaps.
    uint64_t missed() const { return missed_; }
    // Deltas that are not a whole number of nominal sample periods (gt::kNominalTickStep).
    uint64_t irregular() const { return irregular_; }
    uint32_t max_delta() const { return max_delta_; }

    // Received samples per device-clock second over the whole session.
    double rate_hz() const;

private:
    bool have_prev_ = false;
    uint32_t prev_ = 0;
    uint64_t samples_ = 0;
    uint64_t span_ticks_ = 0;
    uint64_t missed_ = 0;
    uint64_t irregular_ = 0;
    uint32_t max_delta_ = 0;
};

}  // namespace rayneo::core
