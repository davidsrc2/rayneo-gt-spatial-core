#pragma once

#include <cstdint>

namespace rayneo::core {

// The IMU stream stalling (a USB hiccup, the glasses' firmware): no report for stale_ns makes
// the pose stale (renderers stop predicting and hold the last one) and asks for stream-on to
// be sent again, every retry_ns while it lasts, without touching the displays. Declaring the
// glasses gone stays with the caller's longer deadline. Times in ns on any monotonic clock.
struct StreamWatchdogConfig {
    uint64_t stale_ns = 500000000ull;
    uint64_t retry_ns = 500000000ull;
};

// A sample from last_ns is stale at now_ns.
bool pose_stale(uint64_t last_ns, uint64_t now_ns, const StreamWatchdogConfig& c = {});

class StreamWatchdog {
public:
    explicit StreamWatchdog(StreamWatchdogConfig cfg = {}) : cfg_(cfg) {}

    void start(uint64_t now_ns);           // streaming starts: counts as a report
    void report(uint64_t now_ns);          // any report arrived
    // Whether to send stream-on again now (call it at least every retry interval).
    bool should_resend(uint64_t now_ns);
    bool stale(uint64_t now_ns) const { return pose_stale(last_ns_, now_ns, cfg_); }
    // The stall that just ended (report() after a stale spell), ns; 0 otherwise. Read once.
    uint64_t take_recovered_ns();
    uint64_t resends() const { return resends_; }

private:
    StreamWatchdogConfig cfg_;
    uint64_t last_ns_ = 0;
    uint64_t last_resend_ns_ = 0;
    uint64_t resends_ = 0;
    bool stalled_ = false;
    uint64_t recovered_ns_ = 0;
};

}  // namespace rayneo::core
