// CLI probe for the RayNeo GT.
//   imu_probe --list          enumerate 0x3941 HID interfaces
//   imu_probe --raw [secs]    bypass the engine: dump the descriptor and raw reports,
//                             validate framing and measure the device-clock rate
//   imu_probe                 stream IMU and print the fused pose at 10 Hz
// Ctrl+C to exit, 'stream-off' is sent on shutdown.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <string_view>
#include <thread>

#include "rayneo/core/gt_protocol.h"
#include "rayneo/core/tick_monitor.h"
#include "rayneo/hal/hid_transport.h"
#include "rayneo/platform/rayneo_engine.h"

namespace {

namespace gt = rayneo::core::gt;
namespace hal = rayneo::hal;

std::atomic<bool> g_quit{false};
void on_signal(int) { g_quit.store(true); }

void hexdump(const char* label, const uint8_t* d, int n) {
    std::printf("%s len=%d:", label, n);
    for (int i = 0; i < n; ++i) std::printf("%s%02x", i % 16 == 0 ? "\n  " : " ", d[i]);
    std::printf("\n");
}

int run_list() {
    hal::hal_init();
    const auto devices = hal::enumerate(gt::kVendorId, 0);
    for (const auto& d : devices) std::printf("%s\n", hal::describe(d).c_str());
    hal::hal_shutdown();
    return devices.empty() ? 1 : 0;
}

int run_raw(double secs) {
    if (!hal::hal_init()) {
        std::fprintf(stderr, "hid_init failed\n");
        return 1;
    }
    const auto devices = hal::enumerate(gt::kVendorId, gt::kProductId);
    const hal::HidDeviceInfo* target = nullptr;
    for (const auto& d : devices) {
        if (d.usage_page == gt::kUsagePage) {
            target = &d;
            break;
        }
    }
    if (target == nullptr) {
        std::fprintf(stderr, "RayNeo GT (3941:AF50) not found\n");
        return 1;
    }
    auto t = hal::create_transport();
    if (!t->open(target->path)) {
        std::fprintf(stderr, "open failed: %s\n", t->last_error().c_str());
        return 1;
    }
    std::printf("opened %s\n", hal::describe(*target).c_str());

    uint8_t desc[4096];
    const int dn = t->report_descriptor(desc, sizeof(desc));
    if (dn > 0) {
        hexdump("report descriptor", desc, dn);
        // Short item 0x85 = Report ID. Absent -> unnumbered reports.
        bool numbered = false;
        for (int i = 0; i < dn;) {
            const uint8_t b = desc[i];
            if (b == 0xFE) {  // long item
                i += 3 + (i + 1 < dn ? desc[i + 1] : 0);
                continue;
            }
            const int sz = (b & 3) == 3 ? 4 : (b & 3);
            if ((b & 0xFC) == 0x84 && sz >= 1 && i + 1 < dn) {
                std::printf("  Report ID item: 0x%02x\n", desc[i + 1]);
                numbered = true;
            }
            i += 1 + sz;
        }
        std::printf("descriptor uses numbered reports: %s\n", numbered ? "YES" : "no");
    }

    const auto on = gt::build_command(gt::kCmdStreamOn);
    const int wn = t->write(on.data(), on.size());
    std::printf("stream-on write -> %d\n", wn);

    std::map<int, uint64_t> lengths;
    std::map<int, uint64_t> heads;  // first two bytes
    std::map<uint32_t, uint64_t> deltas;
    bool have_tick = false;
    uint32_t prev_tick = 0;
    rayneo::core::TickMonitor ticks;
    uint64_t total = 0, acks = 0, imu = 0, unknown = 0;
    int dumped = 0, unknown_dumped = 0;
    bool ack_seen = false;

    const auto t0 = std::chrono::steady_clock::now();
    const auto deadline = t0 + std::chrono::duration<double>(secs);
    uint8_t buf[65];
    while (!g_quit.load() && std::chrono::steady_clock::now() < deadline) {
        const int n = t->read(buf, sizeof(buf), 100);
        if (n < 0) {
            std::fprintf(stderr, "read error: %s\n", t->last_error().c_str());
            break;
        }
        if (n == 0) continue;
        ++total;
        ++lengths[n];
        ++heads[(buf[0] << 8) | (n > 1 ? buf[1] : 0)];
        const auto r = gt::decode_report(buf, static_cast<size_t>(n));
        if (r.kind == gt::ReportKind::Ack) {
            ++acks;
            if (r.ack_cmd == gt::kCmdStreamOn && !ack_seen) {
                ack_seen = true;
                const double ms =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                std::printf("ACK 99 c8 for stream-on after %.1f ms (report #%llu)\n", ms,
                            static_cast<unsigned long long>(total));
                hexdump("ack", buf, n);
            }
        } else if (r.kind == gt::ReportKind::Imu) {
            ++imu;
            ticks.update(r.imu.tick_100us);
            if (have_tick) ++deltas[r.imu.tick_100us - prev_tick];
            prev_tick = r.imu.tick_100us;
            have_tick = true;
            if (dumped < 4) {
                char label[48];
                std::snprintf(label, sizeof(label), "imu #%d tick=%u", dumped, r.imu.tick_100us);
                hexdump(label, buf, n);
                ++dumped;
            }
        } else {
            ++unknown;
            if (unknown_dumped < 8) {
                hexdump("UNKNOWN", buf, n);
                ++unknown_dumped;
            }
        }
    }

    const auto off = gt::build_command(gt::kCmdStreamOff);
    t->write(off.data(), off.size());
    const double wall =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    std::printf("\n--- %.2f s wall ---\n", wall);
    std::printf("reports %llu (imu %llu, ack %llu, unknown %llu), host rate %.1f Hz\n",
                static_cast<unsigned long long>(total), static_cast<unsigned long long>(imu),
                static_cast<unsigned long long>(acks), static_cast<unsigned long long>(unknown),
                static_cast<double>(total) / wall);
    for (const auto& [len, c] : lengths) std::printf("  length %d: %llu\n", len, static_cast<unsigned long long>(c));
    for (const auto& [h, c] : heads)
        std::printf("  head %02x %02x: %llu\n", h >> 8, h & 0xff, static_cast<unsigned long long>(c));
    for (const auto& [d, c] : deltas)
        std::printf("  tick delta %u: %llu\n", d, static_cast<unsigned long long>(c));
    std::printf("device-clock rate %.2f Hz, missed %llu, irregular %llu, max delta %u ticks\n", ticks.rate_hz(),
                static_cast<unsigned long long>(ticks.missed()), static_cast<unsigned long long>(ticks.irregular()),
                ticks.max_delta());
    std::printf("stream-on ACK: %s\n", ack_seen ? "yes" : "NO");

    t->close();
    hal::hal_shutdown();
    return ack_seen && unknown == 0 ? 0 : 2;
}

int run_stream() {
    rayneo_config cfg = rayneo_default_config();
    rayneo_engine* engine = rayneo_engine_create(&cfg);
    if (rayneo_engine_start(engine) != RAYNEO_OK) {
        std::fprintf(stderr, "start failed: %s\n", rayneo_engine_last_error(engine));
        rayneo_engine_destroy(engine);
        return 1;
    }
    std::printf("streaming... (calibration needs ~5 s with the glasses at rest)\n");

    rayneo_stats prev{};
    while (!g_quit.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        rayneo_pose p;
        rayneo_stats s;
        if (!rayneo_engine_latest_pose(engine, &p) || !rayneo_engine_stats(engine, &s)) continue;

        // Windowed device-clock rate: samples per device second since the last print.
        const uint64_t dspan = s.device_span_100us - prev.device_span_100us;
        const double hz = dspan > 0 ? static_cast<double>(s.imu_samples - prev.imu_samples) / (dspan * 100e-6) : 0.0;
        prev = s;

        std::printf("\r%s yaw %+7.2f pitch %+7.2f roll %+7.2f | bias %+5.2f %+5.2f %+5.2f | %6.1f Hz"
                    " | miss %llu drop %llu  ",
                    p.tracking ? "TRACK" : "CALIB", p.yaw_deg, p.pitch_deg, p.roll_deg, p.bias_x, p.bias_y,
                    p.bias_z, hz, static_cast<unsigned long long>(s.missed_samples),
                    static_cast<unsigned long long>(s.reports_dropped));
        std::fflush(stdout);
    }
    std::printf("\n");

    rayneo_stats s;
    if (rayneo_engine_stats(engine, &s)) {
        std::printf("session: %llu imu samples, device rate %.2f Hz, missed %llu, irregular %llu, "
                    "ring drops %llu, non-imu reports %llu\n",
                    static_cast<unsigned long long>(s.imu_samples), s.device_rate_hz,
                    static_cast<unsigned long long>(s.missed_samples),
                    static_cast<unsigned long long>(s.irregular_ticks),
                    static_cast<unsigned long long>(s.reports_dropped),
                    static_cast<unsigned long long>(s.other_reports));
    }
    rayneo_engine_destroy(engine);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    const std::string_view mode = argc > 1 ? argv[1] : "";
    if (mode == "--list") return run_list();
    if (mode == "--raw") return run_raw(argc > 2 ? std::atof(argv[2]) : 5.0);
    return run_stream();
}
