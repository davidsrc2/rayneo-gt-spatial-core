#include "rayneo/platform/rayneo_engine.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "rayneo/core/imu_pipeline.h"
#include "rayneo/hal/hid_transport.h"
#include "rayneo/hal/imu_stream_reader.h"

using rayneo::core::ImuPipeline;
using rayneo::core::ImuPipelineConfig;
using rayneo::core::Pose;
using rayneo::hal::ImuStreamReader;
using rayneo::hal::RawReport;

struct rayneo_engine {
    ImuPipelineConfig cfg;
    std::unique_ptr<ImuStreamReader> reader;
    std::unique_ptr<ImuPipeline> pipeline;
    std::thread fusion_thread;
    std::atomic<bool> running{false};
    std::atomic<bool> recenter{false};

    std::mutex pose_mutex;
    rayneo_pose latest{};
    bool has_pose = false;

    std::string error;
    bool hal_ready = false;
};

namespace {

void fusion_loop(rayneo_engine* e) {
    RawReport rr;
    uint64_t seq = 0;
    while (e->running.load(std::memory_order_acquire)) {
        bool drained = false;
        while (e->reader->pop(rr)) {
            drained = true;
            if (e->recenter.exchange(false)) e->pipeline->recenter();
            const auto pose = e->pipeline->process_report(rr.data, rr.length);
            if (!pose) continue;

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
            p.device_tick_100us = pose->tick_100us;
            p.host_time_ns = rr.host_time_ns;
            p.sequence = ++seq;
            p.tracking = pose->tracking;

            std::lock_guard<std::mutex> lock(e->pose_mutex);
            e->latest = p;
            e->has_pose = true;
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
    return rayneo_config{d.madgwick_beta, d.use_magnetometer};
}

rayneo_engine* rayneo_engine_create(const rayneo_config* cfg) {
    auto* e = new rayneo_engine();
    if (cfg != nullptr) {
        e->cfg.madgwick_beta = cfg->madgwick_beta;
        e->cfg.use_magnetometer = cfg->use_magnetometer;
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
    e->reader = std::make_unique<ImuStreamReader>();
    e->pipeline = std::make_unique<ImuPipeline>(e->cfg);
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

void rayneo_engine_recenter(rayneo_engine* e) {
    if (e != nullptr) e->recenter.store(true, std::memory_order_release);
}

const char* rayneo_engine_last_error(rayneo_engine* e) {
    return e != nullptr ? e->error.c_str() : "null engine";
}

}  // extern "C"
