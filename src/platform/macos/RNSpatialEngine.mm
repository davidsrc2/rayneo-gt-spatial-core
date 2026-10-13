#import "RNSpatialEngine.h"

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
- (float)yawDegrees { return _p.yaw_deg; }
- (float)pitchDegrees { return _p.pitch_deg; }
- (float)rollDegrees { return _p.roll_deg; }
- (uint64_t)sequence { return _p.sequence; }
- (BOOL)isTracking { return _p.tracking ? YES : NO; }

@end

@implementation RNSpatialEngine {
    rayneo_engine* _engine;
}

- (instancetype)init {
    if ((self = [super init])) {
        const rayneo_config cfg = rayneo_default_config();
        _engine = rayneo_engine_create(&cfg);
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
                                 userInfo:@{NSLocalizedDescriptionKey : msg ?: @"unknown"}];
    }
    return NO;
}

- (void)stop { rayneo_engine_stop(_engine); }
- (void)recenter { rayneo_engine_recenter(_engine); }

- (RNPose*)latestPose {
    rayneo_pose p;
    if (!rayneo_engine_latest_pose(_engine, &p)) return nil;
    return [[RNPose alloc] initWithPose:p];
}

@end
