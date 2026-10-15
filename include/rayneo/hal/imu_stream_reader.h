#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

#include "rayneo/hal/hid_transport.h"
#include "rayneo/hal/spsc_ring.h"

namespace rayneo::hal {

struct RawReport {
    uint8_t data[65];
    uint8_t length;
    uint64_t host_time_ns;  // steady_clock at read completion
};

// Discovers the RayNeo GT, enables IMU streaming and pumps raw HID reports into a
// lock-free ring on a dedicated high-priority thread (~457 Hz). Does not parse payloads.
class ImuStreamReader {
public:
    static constexpr size_t kRingCapacity = 1024;  // ~2 s of headroom at 457 Hz
    using Ring = SpscRing<RawReport, kRingCapacity>;

    ImuStreamReader();
    ~ImuStreamReader();

    ImuStreamReader(const ImuStreamReader&) = delete;
    ImuStreamReader& operator=(const ImuStreamReader&) = delete;

    // Finds the vendor interface (usage page 0xFF00), opens it and sends stream-on.
    bool open_device(std::string* error = nullptr);
    bool start();
    // Stops the thread and sends stream-off (streaming otherwise persists after exit).
    void stop();

    // Any thread: the reader thread writes it between reads (one transport user at a time).
    void send_command(uint8_t cmd, uint8_t value) {
        pending_command_.store((static_cast<int>(cmd) << 8) | value, std::memory_order_release);
    }

    bool running() const { return running_.load(std::memory_order_acquire); }
    bool device_lost() const { return device_lost_.load(std::memory_order_acquire); }

    // Consumer side (single consumer thread).
    bool pop(RawReport& out) { return ring_.try_pop(out); }

    uint64_t reports_read() const { return reports_read_.load(std::memory_order_relaxed); }
    uint64_t reports_dropped() const { return reports_dropped_.load(std::memory_order_relaxed); }

private:
    bool send_command_verified(uint8_t cmd, int timeout_ms);
    void run();

    std::unique_ptr<HidTransport> transport_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> device_lost_{false};
    std::atomic<int> pending_command_{-1};  // (cmd << 8) | value, -1 = none
    std::atomic<uint64_t> reports_read_{0};
    std::atomic<uint64_t> reports_dropped_{0};
    Ring ring_;
};

}  // namespace rayneo::hal
