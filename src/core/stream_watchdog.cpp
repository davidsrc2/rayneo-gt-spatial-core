#include "rayneo/core/stream_watchdog.h"

namespace rayneo::core {

bool pose_stale(uint64_t last_ns, uint64_t now_ns, const StreamWatchdogConfig& c) {
    return now_ns > last_ns && now_ns - last_ns > c.stale_ns;
}

void StreamWatchdog::start(uint64_t now_ns) {
    last_ns_ = now_ns;
    last_resend_ns_ = 0;
    stalled_ = false;
    recovered_ns_ = 0;
}

void StreamWatchdog::report(uint64_t now_ns) {
    if (stalled_ && now_ns > last_ns_) recovered_ns_ = now_ns - last_ns_;
    stalled_ = false;
    last_ns_ = now_ns;
}

bool StreamWatchdog::should_resend(uint64_t now_ns) {
    if (!stale(now_ns)) return false;
    if (stalled_ && now_ns - last_resend_ns_ < cfg_.retry_ns) return false;
    stalled_ = true;
    last_resend_ns_ = now_ns;
    ++resends_;
    return true;
}

uint64_t StreamWatchdog::take_recovered_ns() {
    const uint64_t r = recovered_ns_;
    recovered_ns_ = 0;
    return r;
}

}  // namespace rayneo::core
