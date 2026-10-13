#include "rayneo/core/tick_monitor.h"

#include <cmath>

#include "rayneo/core/gt_protocol.h"

namespace rayneo::core {

void TickMonitor::update(uint32_t tick) {
    ++samples_;
    if (!have_prev_) {
        have_prev_ = true;
        prev_ = tick;
        return;
    }
    const uint32_t d = tick - prev_;  // wrap-safe
    prev_ = tick;
    span_ticks_ += d;
    if (d > max_delta_) max_delta_ = d;

    // The firmware period is fractional (21/22 ticks), so a gap of n samples lands
    // within one tick quantum of n * step. Anything further off is irregular.
    const double steps = std::round(static_cast<double>(d) / gt::kNominalTickStep);
    const double err = std::fabs(static_cast<double>(d) - steps * gt::kNominalTickStep);
    if (steps < 1.0 || err > 1.5) {
        ++irregular_;
        return;
    }
    missed_ += static_cast<uint64_t>(steps) - 1;
}

double TickMonitor::rate_hz() const {
    if (span_ticks_ == 0) return 0.0;
    return static_cast<double>(samples_ - 1) / (static_cast<double>(span_ticks_) * 100e-6);
}

}  // namespace rayneo::core
