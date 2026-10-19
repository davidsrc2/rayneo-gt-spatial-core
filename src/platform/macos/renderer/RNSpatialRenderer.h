// Metal renderer for the spatial desktop: N curved panels in a row around the viewer. Pure Objective-C interface (Swift-importable);
// the implementation uses rayneo::core render math directly.

#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <simd/simd.h>

NS_ASSUME_NONNULL_BEGIN

@interface RNRenderOptions : NSObject
@property(nonatomic) BOOL sideBySide;        // the glasses in their 3D mode (double width): the one image in both halves
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
@property(nonatomic) float fixedPredictionMs; // > 0: always this far ahead (space at real size), not the measured horizon
// With fixedPredictionMs: how late frames reach the display (ms, measured) and the refresh
// period, added to it (core::fixed_prediction_horizon_ms). Set before each frame.
@property(nonatomic) float presentLateMs;
@property(nonatomic) float framePeriodMs;
@property(nonatomic) float sampleAgeMs;  // the pose's age when the frame read it
// > 1: the eye images drawn this many times larger and mipmapped down into the glasses'
// image (supersampling: sharper text, no shimmer). 1 = off.
@property(nonatomic) NSUInteger supersample;
@property(nonatomic) float dimOthers;        // brightness of the panels you are not looking at, 1 = off
@property(nonatomic) NSInteger trackingAxes; // core::TrackingAxes: 0 both, 1 left/right only, 2 up/down only
@property(nonatomic) BOOL staticHead;        // glasses in their own pinned mode: they anchor the image, not us
@property(nonatomic) float layoutArcDegrees; // panel size the hand-placed poses refer to (0 = arcDegrees)
@property(nonatomic) NSInteger focusPanel;   // focus mode: only this panel is drawn (-1 = all)
@property(nonatomic) BOOL cinema;            // with focusPanel: centred straight ahead and large
@property(nonatomic) float cinemaYawRadians;   // where the cinema panel is centred (yaw right; 0 = straight ahead)
@property(nonatomic) float cinemaPitchRadians; // (pitch up)
// The arrangement lifted to this elevation (rad, up > 0; the recentre's, RNPose.panelsPitch).
@property(nonatomic) float panelsPitchRadians;
// Vehicle mode: a quarter of the head's heading followed (core::scale_heading).
@property(nonatomic) BOOL vehicleMode;
// Wall: flat panels at distanceMeters, each square to the eye (as RealWarp's flat
// screens), edge to edge as one block curving towards you (no gap, curvature or tilt; core::wall_grid_layout / wall_panel_mesh), fixed
// in the room on every axis. Default YES.
@property(nonatomic) BOOL flatWall;
// The panel that gets the keyboard (the front app's window), framed in the accent colour; -1 none.
@property(nonatomic) NSInteger keyPanel;
@property(nonatomic) float zoom;             // every panel this much larger (and the arrangement with them), 1 = off
// Around the panels, fixed in the room: 0 none (the room seen through), 1 rain, 2 stars,
// 3 sunset, 4 aurora, 5 the picture given with setBackgroundImageAtPath:.
@property(nonatomic) NSInteger backgroundStyle;
@property(nonatomic) float backgroundBrightness;  // 0..1, 0.6
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
// The cursor, drawn over its panel every frame (the captures run without it). Image: any
// thread, when the system cursor changes (hotspot and size in points).
- (void)setCursorImage:(nullable CGImageRef)image hotspot:(CGPoint)hotspot pointSize:(CGSize)size;
// Where it is now: panel (-1: none of ours) and the hotspot in that display's points.
- (void)setCursorPanel:(NSInteger)panel position:(CGPoint)position displaySize:(CGSize)displaySize;

// The zoom (<= 1) at which every panel fits in the glasses' image at once (an overview).
- (float)overviewZoom;
// A monitor's width on the glasses straight ahead, in eye-texture pixels (supersample and cinema
// included); 0 before the first frame and in the glasses' pinned mode. The captures are sized
// from it: more pixels than this only cost WindowServer and the mipmaps.
- (float)panelPixelWidth;

// Panel arc (deg) that fits `count` panels (columns) in the horizontal FOV of the options.
- (float)fitArcDegreesForPanels:(NSUInteger)count;

// Glasses in pinned mode: the arc option (deg) for panels of `sizeDegrees` (at 2 m) at the
// current distance, never wider than what fits `count` columns in the glasses' image.
- (float)pinnedArcDegreesForPanels:(NSUInteger)count sizeDegrees:(float)sizeDegrees;

// Glasses in their pinned mode: this panel is drawn large and the others small around it,
// all still fitted in the glasses' image; changing it eases over a third of a second. -1: all
// the same size. Any thread.
@property(nonatomic) NSInteger growPanel;

// The picture for background style 5 (any image file ImageIO reads); nil or unreadable: none.
// Any thread.
- (BOOL)setBackgroundImageAtPath:(nullable NSString*)path NS_SWIFT_NAME(loadBackgroundImage(path:));

// The cinema panel centred where this head orientation (core frame) looks: set on entering cinema.
- (void)centreCinemaOnGaze:(simd_quatf)orientation;

// Panel index the given head orientation (core frame) looks at, or -1.
- (NSInteger)panelAtGaze:(simd_quatf)orientation;

// A brightness fade is in progress: the next frame differs even with a still head and
// unchanged sources. Render thread only.
@property(nonatomic, readonly) BOOL animating;

// GPU time of the last finished frame, ms (any thread).
@property(atomic, readonly) double lastGPUMs;
// Bit i set: panel i is in the glasses' image or about to be (core::panel_in_view, the last
// frame's pose). The rest are not mipmapped until they come into view; the server slows their
// captures.
@property(atomic, readonly) NSUInteger visiblePanelMask;
// Mean GPU time of the frames completed since the last call (ms; 0 without any).
- (double)takeGPUMeanMs;

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
// The same; gpuDone gets the frame's GPUStartTime and GPUEndTime (CACurrentMediaTime clock) once
// the GPU is done.
- (float)renderToDrawable:(id<CAMetalDrawable>)drawable
              orientation:(simd_quatf)orientation
          angularVelocity:(simd_float3)angularVelocity
             sampleHostNs:(uint64_t)sampleHostNs
             targetHostNs:(uint64_t)targetHostNs
                  gpuDone:(nullable void (^)(double gpuStartTime, double gpuEndTime))gpuDone;
@end

NS_ASSUME_NONNULL_END
