// Objective-C facade over the C ABI. Consumed from Swift via a bridging header,
// or from AppKit/Metal code directly. Only compiled on Apple platforms.

#import <Foundation/Foundation.h>
#import <simd/simd.h>

NS_ASSUME_NONNULL_BEGIN

@interface RNPose : NSObject
// Core frame (X forward, Y left, Z up, RH): head rotation since recenter, level frame.
@property(nonatomic, readonly) simd_quatf orientation;
@property(nonatomic, readonly) simd_float3 angularVelocity;  // rad/s, same frame
@property(nonatomic, readonly) uint64_t hostTimeNs;          // sample arrival, hostNowNs clock
@property(nonatomic, readonly) float yawDegrees;
@property(nonatomic, readonly) float pitchDegrees;
@property(nonatomic, readonly) float rollDegrees;
@property(nonatomic, readonly) uint64_t sequence;
@property(nonatomic, readonly, getter=isTracking) BOOL tracking;
@property(nonatomic, readonly, getter=isStale) BOOL stale;  // no sample for 0.5 s: held, not predicted
@property(nonatomic, readonly) float panelsPitch;  // rad, up > 0: the last recentre's elevation
@property(nonatomic, readonly) float gyroNoise;    // deg/s per axis, white noise at rest
@property(nonatomic, readonly, getter=isGyroNoisy) BOOL gyroNoisy;  // far above normal (cable / port)
@property(nonatomic, readonly) float temperature;  // the IMU's, C
@property(nonatomic, readonly, getter=isWorn) BOOL worn;  // on a head (false after 30 s lying still)
@property(nonatomic, readonly) simd_float3 gyroBias;          // deg/s, sensor frame (diagnostics)
@end

@interface RNSpatialEngine : NSObject
- (instancetype)init;
- (instancetype)initWithDeadZoneDegrees:(float)deadZone;
- (BOOL)start:(NSError* _Nullable* _Nullable)error;
- (void)stop;
- (void)recenter;
// The glasses' own display mode: 0 follow, 1 pinned, 2 steady. Setting pinned recentres its anchor.
- (void)setGlassesMode:(NSInteger)mode;
// The glasses' brightness, 1 (dim) .. 8 (factory level).
- (void)setGlassesBrightness:(NSInteger)level;
- (nullable RNPose*)latestPose;
// IMU stream stalls so far: stream-on resent / acknowledged / stalls ended, and the last one (ms).
- (void)streamStallsResent:(uint64_t*)resent acked:(uint64_t*)acked recovered:(uint64_t*)recovered lastMs:(double*)lastMs;
// Test hook: the stream stops and no sample gets through for ms.
- (void)simulateStallMs:(NSInteger)ms;
// The last 30 s of raw IMU reports to a recording file (for a problem report).
- (BOOL)dumpRecentImuToPath:(NSString*)path;

// Before start: the session's raw IMU reports written to this file / a recording played back
// instead of the glasses (core/imu_recording.h). nil: off.
- (void)setRecordPath:(nullable NSString*)path;
- (void)setReplayPath:(nullable NSString*)path;

// Loads calibration.json (sensor_to_head). Applied immediately, implies a recenter.
- (BOOL)loadCalibrationFromFile:(NSString*)path error:(NSError* _Nullable* _Nullable)error;

// Clock used by RNPose.hostTimeNs (std::chrono::steady_clock).
+ (uint64_t)hostNowNs;
@end

NS_ASSUME_NONNULL_END
