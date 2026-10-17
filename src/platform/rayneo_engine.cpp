#include "rayneo/platform/rayneo_engine.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#if defined(__APPLE__)
#include <pthread/qos.h>
#elif defined(_WIN32)
#include <windows.h>
#endif

#include "rayneo/core/gt_protocol.h"
#include "rayneo/core/imu_pipeline.h"
#include "rayneo/core/tick_monitor.h"
#include "rayneo/hal/hid_transport.h"
#include "rayneo/hal/imu_stream_reader.h"

using rayneo::core::ImuPipeline;
using rayneo::core::ImuPipelineConfig;
using rayneo::core::Pose;
using rayneo::core::TickMonitor;
using rayneo::hal::ImuStreamReader;
using rayneo::hal::RawReport;

struct rayneo_engine {
    ImuPipelineConfig cfg;
    std::unique_ptr<ImuStreamReader> reader;
    std::unique_ptr<ImuPipeline> pipeline;
    std::thread fusion_thread;
    std::atomic<bool> running{false};
    std::atomic<bool> recenter{false};

    std::mutex calib_mutex;
    rayneo::core::Quat sensor_to_head;  // last requested, survives restarts
    bool calib_pending = false;

    std::mutex pose_mutex;
    rayneo_pose latest{};
    bool has_pose = false;
    TickMonitor ticks;  // guarded by pose_mutex
    uint64_t other_reports = 0;

    std::string error;
    bool hal_ready = false;
};

namespace {

void fusion_loop(rayneo_engine* e) {
#if defined(__APPLE__)
    // Same class as the reader: in a background app, timer coalescing would otherwise stretch
    // the 0.5 ms polls below and deliver poses in bursts (panels tremble / slide).
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#elif defined(_WIN32)
    // Windows: above the app's own threads so poses keep flowing while the CPU is busy.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
#endif
    RawReport rr;
    uint64_t seq = 0;
    while (e->running.load(std::memory_order_acquire)) {
        bool drained = false;
        while (e->reader->pop(rr)) {
            drained = true;
            {
                std::lock_guard<std::mutex> lock(e->calib_mutex);
                if (e->calib_pending) {
                    e->pipeline->set_sensor_to_head(e->sensor_to_head);
                    e->calib_pending = false;
                }
            }
            if (e->recenter.exchange(false)) e->pipeline->recenter(true);
            const auto pose = e->pipeline->process_report(rr.data, rr.length);
            if (!pose) {
                std::lock_guard<std::mutex> lock(e->pose_mutex);
                ++e->other_reports;
                continue;
            }

            rayneo_pose p{};
            p.qw = pose->relative.w;
            p.qx = pose->relative.x;
            p.qy = pose->relative.y;
            p.qz = pose->relative.z;
            p.yaw_deg = pose->euler.yaw;
            p.pitch_deg = pose->euler.pitch;
            p.roll_deg = pose->euler.roll;
            p.bias_x = pose->gyro_bias_degs.x;
            p.bias_y = pose->gyro_bias_degs.y;
            p.bias_z = pose->gyro_bias_degs.z;
            p.wx = pose->angular_velocity_rads.x;
            p.wy = pose->angular_velocity_rads.y;
            p.wz = pose->angular_velocity_rads.z;
            p.device_tick_100us = pose->tick_100us;
            p.host_time_ns = rr.host_time_ns;
            p.sequence = ++seq;
            p.tracking = pose->tracking;

            std::lock_guard<std::mutex> lock(e->pose_mutex);
            e->latest = p;
            e->has_pose = true;
            e->ticks.update(p.device_tick_100us);
        }
        if (!drained) {
            if (!e->reader->running()) break;  // device lost
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
    }
}

}  // namespace

extern "C" {

rayneo_config rayneo_default_config(void) {
    ImuPipelineConfig d;
    return rayneo_config{d.madgwick_beta, d.use_magnetometer, d.stabilizer.enter_deg};
}

rayneo_engine* rayneo_engine_create(const rayneo_config* cfg) {
    auto* e = new rayneo_engine();
    if (cfg != nullptr) {
        e->cfg.madgwick_beta = cfg->madgwick_beta;
        e->cfg.use_magnetometer = cfg->use_magnetometer;
        if (cfg->dead_zone_deg > 0.0f) e->cfg.stabilizer.enter_deg = cfg->dead_zone_deg;
        if (cfg->dead_zone_deg < 0.0f) {  // never frozen: always world-locked
            e->cfg.stabilizer = rayneo::core::world_locked_stabilizer();
            e->cfg.body_frame_relative = true;  // exact on both axes whatever the posture at the recenter
            e->cfg.madgwick_beta = rayneo::core::kWorldLockedMadgwickBeta;  // breathing no longer rocks the panels
            if (cfg->dead_zone_deg < -1.0f) {  // anchor at rest
                e->cfg.stabilizer.enter_deg = -1.0f - cfg->dead_zone_deg;
                // Sway that slips out of the anchor glides back instead of catching up at once:
                // simulated at rest (breathing, pulse, tremor), 0.009 -> 0.0007 deg/frame and
                // frozen 89 -> 99 % of the time; a 30 deg/s turn lags the same (the zone).
                e->cfg.stabilizer.release_cutoff_hz = 0.4f;
                // Up / down 2 deg (1 sideways): vertical bobbing at rest (breathing, talking, a nod) stays
                // in the anchor; looking up or down at another panel is far more.
                e->cfg.stabilizer.enter_vertical_deg = 2.0f;
            }
        }
    }
    return e;
}

void rayneo_engine_destroy(rayneo_engine* e) {
    if (e == nullptr) return;
    rayneo_engine_stop(e);
    if (e->hal_ready) rayneo::hal::hal_shutdown();
    delete e;
}

rayneo_status rayneo_engine_start(rayneo_engine* e) {
    if (e == nullptr || e->running.load()) return RAYNEO_ERR_STATE;
    if (!e->hal_ready) {
        if (!rayneo::hal::hal_init()) {
            e->error = "hid_init failed";
            return RAYNEO_ERR_INIT;
        }
        e->hal_ready = true;
    }
    {
        std::lock_guard<std::mutex> lock(e->pose_mutex);
        e->has_pose = false;
        e->ticks.reset();
        e->other_reports = 0;
    }
    e->reader = std::make_unique<ImuStreamReader>();
    e->pipeline = std::make_unique<ImuPipeline>(e->cfg);
    {
        std::lock_guard<std::mutex> lock(e->calib_mutex);
        e->pipeline->set_sensor_to_head(e->sensor_to_head);
        e->calib_pending = false;
    }
    if (!e->reader->open_device(&e->error)) return RAYNEO_ERR_NOT_FOUND;
    if (!e->reader->start()) {
        e->error = "reader start failed";
        return RAYNEO_ERR_STATE;
    }
    e->running.store(true, std::memory_order_release);
    e->fusion_thread = std::thread(fusion_loop, e);
    return RAYNEO_OK;
}

void rayneo_engine_stop(rayneo_engine* e) {
    if (e == nullptr) return;
    e->running.store(false, std::memory_order_release);
    if (e->fusion_thread.joinable()) e->fusion_thread.join();
    if (e->reader) e->reader->stop();
}

bool rayneo_engine_latest_pose(rayneo_engine* e, rayneo_pose* out) {
    if (e == nullptr || out == nullptr) return false;
    std::lock_guard<std::mutex> lock(e->pose_mutex);
    if (!e->has_pose) return false;
    *out = e->latest;
    return true;
}

bool rayneo_engine_stats(rayneo_engine* e, rayneo_stats* out) {
    if (e == nullptr || out == nullptr || !e->reader) return false;
    rayneo_stats s{};
    s.reports_read = e->reader->reports_read();
    s.reports_dropped = e->reader->reports_dropped();
    {
        std::lock_guard<std::mutex> lock(e->pose_mutex);
        s.imu_samples = e->ticks.samples();
        s.other_reports = e->other_reports;
        s.missed_samples = e->ticks.missed();
        s.irregular_ticks = e->ticks.irregular();
        s.device_span_100us = e->ticks.span_ticks();
        s.device_rate_hz = e->ticks.rate_hz();
    }
    *out = s;
    return true;
}

void rayneo_engine_recenter(rayneo_engine* e) {
    if (e != nullptr) e->recenter.store(true, std::memory_order_release);
}

void rayneo_engine_set_glasses_mode(rayneo_engine* e, int mode) {
    if (e == nullptr || !e->reader || mode < 0 || mode > 2) return;
    e->reader->send_command(rayneo::core::gt::kCmdSetDisplayMode, static_cast<uint8_t>(mode));
}

void rayneo_engine_set_glasses_brightness(rayneo_engine* e, int level) {
    if (e == nullptr || !e->reader || level < 1 || level > 8) return;
    e->reader->send_command(rayneo::core::gt::kCmdSetBrightness, static_cast<uint8_t>(level));
}

void rayneo_engine_set_sensor_to_head(rayneo_engine* e, float qw, float qx, float qy, float qz) {
    if (e == nullptr) return;
    std::lock_guard<std::mutex> lock(e->calib_mutex);
    e->sensor_to_head = rayneo::core::Quat{qw, qx, qy, qz}.normalized();
    e->calib_pending = true;
}

const char* rayneo_engine_last_error(rayneo_engine* e) {
    return e != nullptr ? e->error.c_str() : "null engine";
}

}  // extern "C"
