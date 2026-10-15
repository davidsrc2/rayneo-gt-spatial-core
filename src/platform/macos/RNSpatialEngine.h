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
@end

@interface RNSpatialEngine : NSObject
- (instancetype)init;
- (instancetype)initWithDeadZoneDegrees:(float)deadZone;
- (BOOL)start:(NSError* _Nullable* _Nullable)error;
- (void)stop;
- (void)recenter;
// The glasses' own display mode: 0 follow, 1 pinned, 2 steady. Setting pinned recentres its anchor.
- (void)setGlassesMode:(NSInteger)mode;
- (nullable RNPose*)latestPose;

// Loads calibration.json (sensor_to_head). Applied immediately, implies a recenter.
- (BOOL)loadCalibrationFromFile:(NSString*)path error:(NSError* _Nullable* _Nullable)error;

// Clock used by RNPose.hostTimeNs (std::chrono::steady_clock).
+ (uint64_t)hostNowNs;
@end

NS_ASSUME_NONNULL_END
