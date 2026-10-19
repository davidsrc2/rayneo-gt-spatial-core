#ifndef RAYNEO_ENGINE_H
#define RAYNEO_ENGINE_H

/*
 * Stable C ABI over the engine. This is the only surface the OS integration layer
 * (Swift via module.modulemap, Objective-C++, or Win32/C#) needs to consume.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rayneo_engine rayneo_engine;

typedef enum {
    RAYNEO_OK = 0,
    RAYNEO_ERR_INIT = -1,
    RAYNEO_ERR_NOT_FOUND = -2,
    RAYNEO_ERR_STATE = -3,
} rayneo_status;

typedef struct {
    float qw, qx, qy, qz;      /* relative orientation, earth frame Z-up */
    float yaw_deg, pitch_deg, roll_deg; /* diagnostics only */
    float bias_x, bias_y, bias_z; /* gyro bias, deg/s */
    float wx, wy, wz;          /* angular velocity, rad/s, same frame as q (for prediction) */
    uint32_t device_tick_100us;
    uint64_t host_time_ns;     /* std::chrono::steady_clock, see RNSpatialEngine.hostNowNs */
    uint64_t sequence;         /* increments per fused sample */
    bool tracking;
    bool stale;                /* no sample for 0.5 s (stream stalled): w zeroed, the pose held */
    float panels_pitch_rad;    /* elevation to lift the panels to: the last user recentre's (up > 0) */
    float gyro_noise_degs;     /* gyro white noise at rest, deg/s per axis */
    bool gyro_noisy;           /* far above normal: a poor cable or USB port, drift likely */
} rayneo_pose;

/* Transport and device-clock health counters, cumulative since start. */
typedef struct {
    uint64_t reports_read;       /* HID reports read by the I/O thread */
    uint64_t reports_dropped;    /* ring overflow (fusion thread too slow) */
    uint64_t imu_samples;        /* decoded 99 65 reports */
    uint64_t other_reports;      /* acks and anything that is not 99 65 */
    uint64_t missed_samples;     /* inferred from device tick gaps */
    uint64_t irregular_ticks;    /* tick deltas off the 21-tick grid */
    uint64_t device_span_100us;  /* device time covered by imu_samples */
    double device_rate_hz;       /* imu_samples per device second */
    uint64_t stream_resends;     /* stream-on sent again after 0.5 s without samples */
    uint64_t stream_acks;        /* ... and acknowledged */
    uint64_t stalls_recovered;   /* stalls that ended with samples again */
    double last_stall_ms;        /* length of the last one */
} rayneo_stats;

typedef struct {
    float madgwick_beta;
    bool use_magnetometer;
    float dead_zone_deg; /* stabilizer: head motion below this keeps the image still; < 0 = world-locked;
                            below -1, -1 - dead_zone_deg is an anchor (deg): panels frozen while the head holds within it */
} rayneo_config;

rayneo_config rayneo_default_config(void);

rayneo_engine* rayneo_engine_create(const rayneo_config* cfg);
void rayneo_engine_destroy(rayneo_engine* e);

rayneo_status rayneo_engine_start(rayneo_engine* e);
void rayneo_engine_stop(rayneo_engine* e);

/* Lock-light snapshot of the most recent pose. Safe from any thread (e.g. CVDisplayLink). */
bool rayneo_engine_latest_pose(rayneo_engine* e, rayneo_pose* out);

bool rayneo_engine_stats(rayneo_engine* e, rayneo_stats* out);

void rayneo_engine_recenter(rayneo_engine* e);

/* Glasses' own display mode (0 follow, 1 pinned, 2 steady). Setting a mode recentres the
 * glasses' pinned anchor, so setting pinned while in pinned recentres it. While running. */
void rayneo_engine_set_glasses_mode(rayneo_engine* e, int mode);

/* Glasses' display brightness, 1 (dim) .. 8 (factory level). While running. */
void rayneo_engine_set_glasses_brightness(rayneo_engine* e, int level);

/* Sensor -> head mounting quaternion (calibration.json). Applied on the fusion thread at
 * the next sample; implies a recenter. Identity by default. */
void rayneo_engine_set_sensor_to_head(rayneo_engine* e, float qw, float qx, float qy, float qz);

/* Where the gyro bias learned per sensor temperature is kept (UTF-8 path, JSON; the space at
 * real size). Loaded at start, saved every 30 s while it changes and at stop. Before start. */
void rayneo_engine_set_bias_map_path(rayneo_engine* e, const char* path);

/* Raw HID reports of the session, with their arrival times, written to this file (UTF-8 path;
 * core/imu_recording.h). Before start; empty: off. */
void rayneo_engine_set_record_path(rayneo_engine* e, const char* path);

/* A recording played back in real time instead of the glasses' IMU (no HID device opened;
 * the bias map is read, not saved). Before start; empty: the glasses. */
void rayneo_engine_set_replay_path(rayneo_engine* e, const char* path);

/* Test hook: the IMU stream stops (stream-off) and no sample is let through for ms. */
void rayneo_engine_simulate_stall(rayneo_engine* e, int ms);

/* Last error message, valid until the next call on this engine. */
const char* rayneo_engine_last_error(rayneo_engine* e);

#ifdef __cplusplus
}
#endif

#endif /* RAYNEO_ENGINE_H */
