// Metal renderer for the spatial desktop: N curved panels in a row around the viewer. Pure Objective-C interface (Swift-importable);
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
@property(nonatomic) float arcDegrees;       // horizontal extent of each panel, 50
@property(nonatomic) float gapDegrees;       // space between panels, 4
@property(nonatomic) BOOL curved;            // NO: flat monitors angled towards you
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

// Number of panels laid out left -> right (default 1). Any thread.
@property(nonatomic) NSUInteger panelCount;

// Latest captured frame (IOSurface-backed BGRA) of a panel. Any thread; retained until
// replaced. Panels without a frame are drawn as dark placeholders.
- (void)setSourcePixelBuffer:(CVPixelBufferRef)pixelBuffer forPanel:(NSUInteger)panel;
- (void)setSourcePixelBuffer:(CVPixelBufferRef)pixelBuffer;  // panel 0

// Panel arc (deg) that fits `count` panels in the horizontal FOV of the current options.
- (float)fitArcDegreesForPanels:(NSUInteger)count;

// Panel index the given head orientation (core frame) looks at, or -1.
- (NSInteger)panelAtGaze:(simd_quatf)orientation;

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
