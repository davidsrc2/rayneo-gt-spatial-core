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
@property(nonatomic) BOOL curved;            // shorthand for curvature 1
@property(nonatomic) float curvature;        // 0 = flat monitors angled towards you, 1 = cylinder
@property(nonatomic) BOOL neckModel;         // eyes ahead of the neck's pivot (core::view_matrix)
@property(nonatomic) float tilt;             // panels above / below: 1 face you, 0 upright (core::place_pitched)
@property(nonatomic) float fovYDegrees;      // per-eye vertical FOV of the optics, 24.5
@property(nonatomic) float eyeAspect;        // perceived per-eye aspect, 16/9
@property(nonatomic) float k1;               // barrel pre-distortion, 0 = off
@property(nonatomic) float k2;
@property(nonatomic) float maxPredictionMs;  // extrapolation clamp, 50
@property(nonatomic) float dimOthers;        // brightness of the panels you are not looking at, 1 = off
@property(nonatomic) NSInteger trackingAxes; // core::TrackingAxes: 0 both, 1 left/right only, 2 up/down only
@property(nonatomic) BOOL staticHead;        // glasses in their own pinned mode: they anchor the image, not us
@property(nonatomic) float layoutArcDegrees; // panel size the hand-placed poses refer to (0 = arcDegrees)
@property(nonatomic) NSInteger focusPanel;   // focus mode: only this panel is drawn (-1 = all)
@property(nonatomic) BOOL cinema;            // with focusPanel: centred straight ahead and large
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

// Grid cell of each panel (column, row; row 0 = top), in panel order. Empty or mismatched
// count: one row, left -> right. Any thread.
- (void)setGridColumns:(NSArray<NSNumber*>*)columns rows:(NSArray<NSNumber*>*)rows;

// Hand-placed position (deg, yaw right / pitch up) and size of each panel, in panel order
// (Nimbo's layout editor). Empty or a count other than panelCount: the grid. Any thread.
- (void)setPanelPosesYaw:(NSArray<NSNumber*>*)yaw pitch:(NSArray<NSNumber*>*)pitch scale:(NSArray<NSNumber*>*)scale;

// Floating windows: the last N panels (after the monitors) show single windows, placed at
// these poses (same units as the monitor poses). Their shape follows each window. Any thread.
- (void)setFloatingPosesYaw:(NSArray<NSNumber*>*)yaw pitch:(NSArray<NSNumber*>*)pitch scale:(NSArray<NSNumber*>*)scale;

// A message fixed in front of the eyes (start-up: "preparing, keep the glasses still"), two
// lines separated by "\n"; nil hides it. Any thread.
- (void)setOverlayText:(nullable NSString*)text;

// Panel arc (deg) that fits `count` panels (columns) in the horizontal FOV of the options.
- (float)fitArcDegreesForPanels:(NSUInteger)count;

// Glasses in pinned mode: the arc option (deg) for panels of `sizeDegrees` (at 2 m) at the
// current distance, never wider than what fits `count` columns in the glasses' image.
- (float)pinnedArcDegreesForPanels:(NSUInteger)count sizeDegrees:(float)sizeDegrees;

// Glasses in their pinned mode: this panel is drawn large and the others small around it,
// all still fitted in the glasses' image; changing it eases over a third of a second. -1: all
// the same size. Any thread.
@property(nonatomic) NSInteger growPanel;

// Panel index the given head orientation (core frame) looks at, or -1.
- (NSInteger)panelAtGaze:(simd_quatf)orientation;

// A brightness fade is in progress: the next frame differs even with a still head and
// unchanged sources. Render thread only.
@property(nonatomic, readonly) BOOL animating;

// True when the frame about to be drawn equals the one on screen (head still, no new
// capture, no fade, not forced): skip it. Otherwise remembers it as drawn. Render thread only.
- (BOOL)canSkipFrameWithOrientation:(simd_quatf)orientation
                    angularVelocity:(simd_float3)angularVelocity
                           tracking:(BOOL)tracking
                             forced:(BOOL)forced;
// The screen shows something else (placeholder frame without a pose): never skip next.
- (void)invalidateSkippedFrame;

// The pixel format must be an sRGB one (MTLPixelFormatBGRA8Unorm_sRGB): filtering and
// dimming happen in linear light, which keeps text edges from darkening.
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
