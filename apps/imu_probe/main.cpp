// CLI probe: discovers the RayNeo GT, streams IMU and prints the fused pose at 10 Hz.
// Ctrl+C to exit, 'stream-off' is sent on shutdown.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <string_view>
#include <thread>

#include "rayneo/hal/hid_transport.h"
#include "rayneo/core/gt_protocol.h"
#include "rayneo/platform/rayneo_engine.h"

namespace {
std::atomic<bool> g_quit{false};
void on_signal(int) { g_quit.store(true); }
}  // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    if (argc > 1 && std::string_view(argv[1]) == "--list") {
        rayneo::hal::hal_init();
        for (const auto& d : rayneo::hal::enumerate(rayneo::core::gt::kVendorId, 0)) {
            std::printf("%s\n", rayneo::hal::describe(d).c_str());
        }
        rayneo::hal::hal_shutdown();
        return 0;
    }

    rayneo_config cfg = rayneo_default_config();
    rayneo_engine* engine = rayneo_engine_create(&cfg);
    if (rayneo_engine_start(engine) != RAYNEO_OK) {
        std::fprintf(stderr, "start failed: %s\n", rayneo_engine_last_error(engine));
        rayneo_engine_destroy(engine);
        return 1;
    }
    std::printf("streaming... (calibration needs ~5 s with the glasses at rest)\n");

    uint64_t last_seq = 0;
    auto last_t = std::chrono::steady_clock::now();
    while (!g_quit.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        rayneo_pose p;
        if (!rayneo_engine_latest_pose(engine, &p)) continue;

        const auto now = std::chrono::steady_clock::now();
        const double dt = std::chrono::duration<double>(now - last_t).count();
        const double hz = dt > 0 ? static_cast<double>(p.sequence - last_seq) / dt : 0.0;
        last_seq = p.sequence;
        last_t = now;

        std::printf("\r%s yaw %+7.2f pitch %+7.2f roll %+7.2f | bias %+5.2f %+5.2f %+5.2f | %5.1f Hz  ",
                    p.tracking ? "TRACK" : "CALIB", p.yaw_deg, p.pitch_deg, p.roll_deg, p.bias_x,
                    p.bias_y, p.bias_z, hz);
        std::fflush(stdout);
    }
    std::printf("\n");
    rayneo_engine_destroy(engine);
    return 0;
}
