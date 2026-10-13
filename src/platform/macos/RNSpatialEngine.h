// Objective-C facade over the C ABI. Consumed from Swift via a bridging header,
// or from AppKit/Metal code directly. Only compiled on Apple platforms.

#import <Foundation/Foundation.h>
#import <simd/simd.h>

NS_ASSUME_NONNULL_BEGIN

@interface RNPose : NSObject
@property(nonatomic, readonly) simd_quatf orientation;  // earth frame, Z-up
@property(nonatomic, readonly) float yawDegrees;
@property(nonatomic, readonly) float pitchDegrees;
@property(nonatomic, readonly) float rollDegrees;
@property(nonatomic, readonly) uint64_t sequence;
@property(nonatomic, readonly, getter=isTracking) BOOL tracking;
@end

@interface RNSpatialEngine : NSObject
- (instancetype)init;
- (BOOL)start:(NSError* _Nullable* _Nullable)error;
- (void)stop;
- (void)recenter;
- (nullable RNPose*)latestPose;
@end

NS_ASSUME_NONNULL_END
