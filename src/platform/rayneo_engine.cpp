#include "rayneo/platform/rayneo_engine.h"

#include <atomic>
#include <cstring>
#include <iterator>
#include <vector>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
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
#include "rayneo/core/imu_recording.h"
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
    std::string bias_map_path;  // set before start

    // Recording (raw reports to a file) and replay (a recording instead of the glasses).
    std::string record_path, replay_path;  // set before start
    std::ofstream record_file;             // fusion thread
    std::vector<uint8_t> record_buf;       // fusion thread: flushed about once a second
    std::vector<rayneo::core::ImuRecord> replay;
};

namespace {

void save_bias_map(rayneo_engine* e) {
    if (e->bias_map_path.empty() || !e->pipeline || !e->pipeline->bias_map().dirty()) return;
    const auto path = std::filesystem::u8path(e->bias_map_path);
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    auto tmp = path;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return;
        f << e->pipeline->bias_map().to_json();
    }
    std::filesystem::rename(tmp, path, ec);
    if (!ec) e->pipeline->bias_map().clear_dirty();
}

void load_bias_map(rayneo_engine* e) {
    if (e->bias_map_path.empty() || !e->pipeline) return;
    std::ifstream f(std::filesystem::u8path(e->bias_map_path), std::ios::binary);
    if (!f) return;
    std::stringstream ss;
    ss << f.rdbuf();
    if (auto map = rayneo::core::BiasMap::from_json(ss.str())) e->pipeline->set_bias_map(*map);
}

uint64_t now_ns() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

bool load_replay(rayneo_engine* e) {
    std::ifstream f(std::filesystem::u8path(e->replay_path), std::ios::binary);
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    auto records = rayneo::core::parse_imu_recording(bytes.data(), bytes.size());
    if (!f.good() && !f.eof()) return false;
    if (!records || records->empty()) return false;
    e->replay = std::move(*records);
    return true;
}

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
    uint64_t seq_since_save = 0;
    // Replay: each recorded report when its time comes, on today's clock.
    size_t replay_next = 0;
    const uint64_t replay_start = now_ns();
    const auto next_report = [&](RawReport& out) {
        if (e->replay.empty()) return e->reader->pop(out);
        if (replay_next >= e->replay.size()) return false;
        const auto& r = e->replay[replay_next];
        const uint64_t at = replay_start + (r.host_ns - e->replay.front().host_ns);
        if (now_ns() < at) return false;
        std::memcpy(out.data, r.data, r.length);
        out.length = r.length;
        out.host_time_ns = at;
        ++replay_next;
        return true;
    };
    while (e->running.load(std::memory_order_acquire)) {
        bool drained = false;
        while (next_report(rr)) {
            if (e->record_file.is_open()) {
                rayneo::core::append_imu_record(e->record_buf, rr.host_time_ns, rr.data, rr.length);
                if (e->record_buf.size() >= 457 * 73) {
                    e->record_file.write(reinterpret_cast<const char*>(e->record_buf.data()),
                                         static_cast<std::streamsize>(e->record_buf.size()));
                    e->record_file.flush();
                    e->record_buf.clear();
                }
            }
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
            if (++seq_since_save >= 457 * 30) {  // ~30 s of samples: the temperature map, if it changed
                seq_since_save = 0;
                save_bias_map(e);
            }
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
            if (e->reader && !e->reader->running()) break;  // device lost
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
        if (cfg->dead_zone_deg < 0.0f) {  // the space at real size: fixed in the room
            // Stationary lock: frozen while the head holds still, 1:1 the moment it moves
            // (replaces the adaptive follow filter, the elliptical anchor and its glide, tremor
            // damping, knock rejection and lag compensation of earlier builds).
            // Orientation from Vqf6D (gyro heading, gentle gravity tilt correction, gyro bias by
            // Kalman at rest from the temperature map); its rest detection is the lock ("bias gate").
            e->cfg.stabilizer = rayneo::core::stationary_lock_stabilizer();
            e->cfg.stabilizer.lock_on_rest = true;
            e->cfg.use_vqf = true;
            e->cfg.body_frame_relative = true;  // exact on both axes whatever the posture at the recenter
            e->cfg.level_reference = true;      // the room's vertical is "up": panels on the horizon, level
            e->cfg.hold_until_bias_settles = true;  // no start with a rough gyro bias (yaw drift)
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
    e->pipeline = std::make_unique<ImuPipeline>(e->cfg);
    // A recording keeps the bias map it started with, and its replay starts from that one, so
    // every replay of it begins as the session did.
    const std::string map_path = e->bias_map_path;
    if (!e->replay_path.empty() && std::filesystem::exists(std::filesystem::u8path(e->replay_path + ".biasmap.json"))) {
        e->bias_map_path = e->replay_path + ".biasmap.json";
    }
    load_bias_map(e);
    e->bias_map_path = map_path;
    if (!e->record_path.empty() && !map_path.empty()) {
        std::error_code ec;
        std::filesystem::copy_file(std::filesystem::u8path(map_path), std::filesystem::u8path(e->record_path + ".biasmap.json"),
                                   std::filesystem::copy_options::overwrite_existing, ec);
    }
    {
        std::lock_guard<std::mutex> lock(e->calib_mutex);
        e->pipeline->set_sensor_to_head(e->sensor_to_head);
        e->calib_pending = false;
    }
    e->replay.clear();
    if (!e->replay_path.empty()) {
        e->reader.reset();
        if (!load_replay(e)) {
            e->error = "not an IMU recording: " + e->replay_path;
            return RAYNEO_ERR_NOT_FOUND;
        }
    } else {
        e->reader = std::make_unique<ImuStreamReader>();
        if (!e->reader->open_device(&e->error)) return RAYNEO_ERR_NOT_FOUND;
        if (!e->reader->start()) {
            e->error = "reader start failed";
            return RAYNEO_ERR_STATE;
        }
    }
    if (!e->record_path.empty()) {
        e->record_file.open(std::filesystem::u8path(e->record_path), std::ios::binary | std::ios::trunc);
        e->record_buf.clear();
        rayneo::core::append_imu_recording_header(e->record_buf);
    }
    e->running.store(true, std::memory_order_release);
    e->fusion_thread = std::thread(fusion_loop, e);
    return RAYNEO_OK;
}

void rayneo_engine_stop(rayneo_engine* e) {
    if (e == nullptr) return;
    e->running.store(false, std::memory_order_release);
    if (e->fusion_thread.joinable()) e->fusion_thread.join();
    if (e->record_file.is_open()) {
        e->record_file.write(reinterpret_cast<const char*>(e->record_buf.data()),
                             static_cast<std::streamsize>(e->record_buf.size()));
        e->record_file.close();
        e->record_buf.clear();
    }
    if (e->replay.empty()) save_bias_map(e);  // a replay teaches nothing new
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

void rayneo_engine_set_bias_map_path(rayneo_engine* e, const char* path) {
    if (e != nullptr) e->bias_map_path = path != nullptr ? path : "";
}

void rayneo_engine_set_record_path(rayneo_engine* e, const char* path) {
    if (e != nullptr) e->record_path = path != nullptr ? path : "";
}

void rayneo_engine_set_replay_path(rayneo_engine* e, const char* path) {
    if (e != nullptr) e->replay_path = path != nullptr ? path : "";
}

const char* rayneo_engine_last_error(rayneo_engine* e) {
    return e != nullptr ? e->error.c_str() : "null engine";
}

}  // extern "C"
