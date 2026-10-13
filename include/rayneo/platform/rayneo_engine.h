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
    uint32_t device_tick_100us;
    uint64_t host_time_ns;
    uint64_t sequence;         /* increments per fused sample */
    bool tracking;
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
} rayneo_stats;

typedef struct {
    float madgwick_beta;
    bool use_magnetometer;
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

/* Sensor -> head mounting quaternion (calibration.json). Applied on the fusion thread at
 * the next sample; implies a recenter. Identity by default. */
void rayneo_engine_set_sensor_to_head(rayneo_engine* e, float qw, float qx, float qy, float qz);

/* Last error message, valid until the next call on this engine. */
const char* rayneo_engine_last_error(rayneo_engine* e);

#ifdef __cplusplus
}
#endif

#endif /* RAYNEO_ENGINE_H */
