#import "RNSpatialEngine.h"

#include <chrono>
#include <fstream>
#include <sstream>

#include "rayneo/core/head_calibration.h"
#include "rayneo/platform/rayneo_engine.h"

@implementation RNPose {
    rayneo_pose _p;
}

- (instancetype)initWithPose:(const rayneo_pose&)p {
    if ((self = [super init])) {
        _p = p;
    }
    return self;
}

- (simd_quatf)orientation { return simd_quaternion(_p.qx, _p.qy, _p.qz, _p.qw); }
- (simd_float3)angularVelocity { return simd_make_float3(_p.wx, _p.wy, _p.wz); }
- (simd_float3)gyroBias { return simd_make_float3(_p.bias_x, _p.bias_y, _p.bias_z); }
- (uint64_t)hostTimeNs { return _p.host_time_ns; }
- (float)yawDegrees { return _p.yaw_deg; }
- (float)pitchDegrees { return _p.pitch_deg; }
- (float)rollDegrees { return _p.roll_deg; }
- (uint64_t)sequence { return _p.sequence; }
- (BOOL)isTracking { return _p.tracking ? YES : NO; }
- (BOOL)isStale { return _p.stale ? YES : NO; }
- (float)panelsPitch { return _p.panels_pitch_rad; }
- (float)gyroNoise { return _p.gyro_noise_degs; }
- (BOOL)isGyroNoisy { return _p.gyro_noisy ? YES : NO; }
- (float)temperature { return _p.temp_c; }

@end

@implementation RNSpatialEngine {
    rayneo_engine* _engine;
}

- (instancetype)init {
    return [self initWithDeadZoneDegrees:0.0f];
}

- (instancetype)initWithDeadZoneDegrees:(float)deadZone {
    if ((self = [super init])) {
        rayneo_config cfg = rayneo_default_config();
        if (deadZone != 0.0f) cfg.dead_zone_deg = deadZone;  // < 0: off
        _engine = rayneo_engine_create(&cfg);
        // The gyro bias per sensor temperature, kept between runs.
        NSString* caches = NSSearchPathForDirectoriesInDomains(NSCachesDirectory, NSUserDomainMask, YES).firstObject;
        if (caches != nil) {
            rayneo_engine_set_bias_map_path(_engine, [caches stringByAppendingPathComponent:@"Nimbo/rayneo_biasmap.json"].UTF8String);
        }
    }
    return self;
}

- (void)dealloc {
    rayneo_engine_destroy(_engine);
}

- (BOOL)start:(NSError**)error {
    const rayneo_status st = rayneo_engine_start(_engine);
    if (st == RAYNEO_OK) return YES;
    if (error != nullptr) {
        NSString* msg = [NSString stringWithUTF8String:rayneo_engine_last_error(_engine)];
        *error = [NSError errorWithDomain:@"RayNeoEngine"
                                     code:st
                                 userInfo:@{NSLocalizedDescriptionKey : msg != nil ? msg : @"unknown"}];
    }
    return NO;
}

- (void)stop { rayneo_engine_stop(_engine); }
- (void)recenter { rayneo_engine_recenter(_engine); }
- (void)setGlassesMode:(NSInteger)mode { rayneo_engine_set_glasses_mode(_engine, static_cast<int>(mode)); }
- (void)setGlassesBrightness:(NSInteger)level { rayneo_engine_set_glasses_brightness(_engine, static_cast<int>(level)); }

- (void)streamStallsResent:(uint64_t*)resent acked:(uint64_t*)acked recovered:(uint64_t*)recovered lastMs:(double*)lastMs {
    rayneo_stats s{};
    rayneo_engine_stats(_engine, &s);
    *resent = s.stream_resends;
    *acked = s.stream_acks;
    *recovered = s.stalls_recovered;
    *lastMs = s.last_stall_ms;
}

- (void)simulateStallMs:(NSInteger)ms { rayneo_engine_simulate_stall(_engine, static_cast<int>(ms)); }

- (void)setRecordPath:(NSString*)path { rayneo_engine_set_record_path(_engine, path != nil ? path.UTF8String : ""); }
- (void)setReplayPath:(NSString*)path { rayneo_engine_set_replay_path(_engine, path != nil ? path.UTF8String : ""); }

- (BOOL)loadCalibrationFromFile:(NSString*)path error:(NSError**)error {
    std::ifstream f(path.fileSystemRepresentation);
    std::stringstream ss;
    ss << f.rdbuf();
    const auto q = f ? rayneo::core::calibration_from_json(ss.str()) : std::nullopt;
    if (!q) {
        if (error != nullptr) {
            *error = [NSError errorWithDomain:@"RayNeoEngine"
                                         code:RAYNEO_ERR_STATE
                                     userInfo:@{NSLocalizedDescriptionKey : [NSString
                                                   stringWithFormat:@"invalid or missing calibration: %@", path]}];
        }
        return NO;
    }
    rayneo_engine_set_sensor_to_head(_engine, q->w, q->x, q->y, q->z);
    return YES;
}

+ (uint64_t)hostNowNs {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

- (RNPose*)latestPose {
    rayneo_pose p;
    if (!rayneo_engine_latest_pose(_engine, &p)) return nil;
    return [[RNPose alloc] initWithPose:p];
}

@end
