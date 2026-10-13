// Metal renderer for the spatial screen. Pure Objective-C interface (Swift-importable);
// the implementation uses rayneo::core render math directly.

#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <simd/simd.h>

NS_ASSUME_NONNULL_BEGIN

@interface RNRenderOptions : NSObject
@property(nonatomic) BOOL sideBySide;        // half-SBS for the glasses' 3D mode
@property(nonatomic) float ipdMeters;        // 0.063
@property(nonatomic) float distanceMeters;   // cylinder radius, 2.0
@property(nonatomic) float arcDegrees;       // horizontal extent of the screen, 50
@property(nonatomic) float fovYDegrees;      // per-eye vertical FOV of the optics, 24.5
@property(nonatomic) float eyeAspect;        // perceived per-eye aspect, 16/9
@property(nonatomic) float k1;               // barrel pre-distortion, 0 = off
@property(nonatomic) float k2;
@property(nonatomic) float maxPredictionMs;  // extrapolation clamp, 50
@end

@interface RNSpatialRenderer : NSObject
@property(nonatomic, readonly) id<MTLDevice> device;
@property(nonatomic, strong) RNRenderOptions* options;

- (nullable instancetype)initWithDevice:(id<MTLDevice>)device
                            pixelFormat:(MTLPixelFormat)pixelFormat
                                  error:(NSError* _Nullable* _Nullable)error;

// Latest captured frame (IOSurface-backed BGRA). Any thread; retained until replaced.
- (void)setSourcePixelBuffer:(CVPixelBufferRef)pixelBuffer;

// Predicts the pose to targetHostNs, renders both eyes and presents. Render thread only.
// orientation/angularVelocity are in the core frame (RNPose). Returns the prediction
// horizon actually applied, in ms.
- (float)renderToDrawable:(id<CAMetalDrawable>)drawable
              orientation:(simd_quatf)orientation
          angularVelocity:(simd_float3)angularVelocity
             sampleHostNs:(uint64_t)sampleHostNs
             targetHostNs:(uint64_t)targetHostNs;
@end

NS_ASSUME_NONNULL_END
