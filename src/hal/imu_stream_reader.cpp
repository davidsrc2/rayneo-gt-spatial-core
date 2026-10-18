#include "rayneo/hal/imu_stream_reader.h"

#include <chrono>

#if defined(__APPLE__)
#include <pthread.h>
#include <pthread/qos.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#elif defined(_WIN32)
#include <windows.h>
// After windows.h:
#include <avrt.h>
#include <timeapi.h>
#endif

#include "rayneo/core/gt_protocol.h"

namespace rayneo::hal {
namespace gt = rayneo::core::gt;
namespace {

constexpr int kReadTimeoutMs = 100;
constexpr int kAckTimeoutMs = 500;

uint64_t now_ns() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

void raise_thread_priority() {
#if defined(__APPLE__)
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    // Mach real-time band: woken every report (~2.2 ms at 457 Hz) for up to 0.5 ms of work,
    // within 1 ms. The scheduler demotes a thread that overruns, so it cannot starve the Mac;
    // if the call is refused the QoS above stays.
    mach_timebase_info_data_t tb{};
    if (mach_timebase_info(&tb) == KERN_SUCCESS && tb.numer != 0) {
        const auto ticks = [&](double ms) { return static_cast<uint32_t>(ms * 1e6 * tb.denom / tb.numer); };
        thread_time_constraint_policy_data_t p{};
        p.period = ticks(2.2);
        p.computation = ticks(0.5);
        p.constraint = ticks(1.0);
        p.preemptible = TRUE;
        thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY,
                          reinterpret_cast<thread_policy_t>(&p), THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    }
#elif defined(_WIN32)
    // MMCSS "Pro Audio": the scheduler service keeps this thread on time under load (a plain
    // TIME_CRITICAL priority alone still gets preempted by DPC-heavy drivers).
    DWORD task = 0;
    if (AvSetMmThreadCharacteristicsW(L"Pro Audio", &task) == nullptr) {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    }
#endif
}

// 1 ms system timer while streaming: the default 15.6 ms tick would batch the ~2.2 ms
// reports. No-op elsewhere.
struct TimerResolution {
#if defined(_WIN32)
    TimerResolution() { timeBeginPeriod(1); }
    ~TimerResolution() { timeEndPeriod(1); }
#endif
};

}  // namespace

ImuStreamReader::ImuStreamReader() : transport_(create_transport()) {
    for (auto& p : pending_) p.store(-1, std::memory_order_relaxed);
}

ImuStreamReader::~ImuStreamReader() { stop(); }

bool ImuStreamReader::open_device(std::string* error) {
    const auto devices = enumerate(gt::kVendorId, gt::kProductId);
    const HidDeviceInfo* target = nullptr;
    for (const auto& d : devices) {
        if (d.usage_page == gt::kUsagePage) {
            target = &d;
            break;
        }
    }
    if (target == nullptr && !devices.empty()) target = &devices.front();
    if (target == nullptr) {
        if (error) *error = "RayNeo GT (3941:AF50) not found";
        return false;
    }
    if (!transport_->open(target->path)) {
        if (error) *error = "open failed: " + describe(*target) + " (" + transport_->last_error() + ")";
        return false;
    }
    if (!send_command_verified(gt::kCmdStreamOn, kAckTimeoutMs)) {
        if (error) *error = "stream-on not acknowledged";
        transport_->close();
        return false;
    }
    return true;
}

bool ImuStreamReader::send_command_verified(uint8_t cmd, int timeout_ms) {
    const auto frame = gt::build_command(cmd);
    if (transport_->write(frame.data(), frame.size()) < 0) return false;

    // The write return value is not reliable across backends; the 99 c8 ack is.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    uint8_t buf[65];
    while (std::chrono::steady_clock::now() < deadline) {
        const int n = transport_->read(buf, sizeof(buf), 20);
        if (n < 0) return false;
        if (n == 0) continue;
        const auto r = gt::decode_report(buf, static_cast<size_t>(n));
        if (r.kind == gt::ReportKind::Ack && r.ack_cmd == cmd) return true;
    }
    return false;
}

bool ImuStreamReader::start() {
    if (!transport_->is_open() || running_.load()) return false;
    device_lost_.store(false);
    running_.store(true, std::memory_order_release);
    thread_ = std::thread(&ImuStreamReader::run, this);
    return true;
}

void ImuStreamReader::stop() {
    running_.store(false, std::memory_order_release);
    if (thread_.joinable()) thread_.join();
    if (transport_->is_open()) {
        const auto frame = gt::build_command(gt::kCmdStreamOff);
        transport_->write(frame.data(), frame.size());
        transport_->close();
    }
}

void ImuStreamReader::run() {
    raise_thread_priority();
    [[maybe_unused]] const TimerResolution timer;
    RawReport rr{};
    while (running_.load(std::memory_order_acquire)) {
        for (int cmd = 0; cmd < 16; ++cmd) {
            const int value = pending_[cmd].exchange(-1, std::memory_order_acq_rel);
            if (value < 0) continue;
            const auto frame = gt::build_command(static_cast<uint8_t>(cmd), static_cast<uint8_t>(value));
            transport_->write(frame.data(), frame.size());
        }
        const int n = transport_->read(rr.data, sizeof(rr.data), kReadTimeoutMs);
        if (n < 0) {
            device_lost_.store(true, std::memory_order_release);
            break;
        }
        if (n == 0) continue;
        rr.length = static_cast<uint8_t>(n);
        rr.host_time_ns = now_ns();
        reports_read_.fetch_add(1, std::memory_order_relaxed);
        if (!ring_.try_push(rr)) reports_dropped_.fetch_add(1, std::memory_order_relaxed);
    }
    running_.store(false, std::memory_order_release);
}

}  // namespace rayneo::hal
