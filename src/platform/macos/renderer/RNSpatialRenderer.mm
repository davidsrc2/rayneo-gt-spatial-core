#import "RNSpatialRenderer.h"

#include <os/lock.h>
#import <CoreText/CoreText.h>
#import <ImageIO/ImageIO.h>
#import <QuartzCore/QuartzCore.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "rayneo/core/frame_skip.h"
#include "rayneo/core/panel_dimmer.h"
#include "rayneo/core/pose_stabilizer.h"
#include "rayneo/core/render_math.h"
#include "rn_shaders.h"  // generated: kRNShaderSource

namespace core = rayneo::core;

namespace {

constexpr float kDegToRad = 0.017453292519943295f;

struct SceneUniforms {
    simd_float4x4 view_projection;
};

struct DistortUniforms {
    float k1;
    float k2;
    float aspect;
    float box;  // 1: no distortion and a 2x supersampled eye: one bilinear tap of level 0 (2x2 box)
};

struct StereoUniforms {
    float brightness;
    float eye;
    float strength;
    float convergence;
    float edge;
    float pad[3];
    simd_float4 rect;  // where the video is, 0..1: x0, y0, x1, y1
};

// Mirrors BackgroundUniforms in Shaders.metal (float3x3: three 16-byte columns).
struct BackgroundUniforms {
    simd_float3x3 eye_to_world;
    float tan_x;
    float tan_y;
    float time;
    float brightness;
    int style;
    float image_aspect;
};

struct DepthBlendUniforms {
    float low;
    float high;
    float alpha;
};

constexpr float kDepthEdge = 0.05f;  // AI 3D: side margins where the image returns to the panel's plane

simd_float4x4 to_simd(const core::Mat4& m) {
    simd_float4x4 out;
    static_assert(sizeof(out) == sizeof(m.m), "column-major 4x4 floats");
    std::memcpy(&out, m.m, sizeof(m.m));
    return out;
}

core::Quat from_simd(simd_quatf q) { return core::Quat{q.vector.w, q.vector.x, q.vector.y, q.vector.z}; }

constexpr NSUInteger kMaxPanels = 8;
constexpr float kCinemaScale = 1.6f;  // cinema mode: the panel this much larger than a monitor
constexpr double kGrowSeconds = 0.5;       // glasses' pinned mode: pan and zoom to another highlighted panel
constexpr float kGrowDim = 0.45f;          // glasses' pinned mode: the panels around the highlighted one

uint64_t now_ns() { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

struct Panel {
    CVMetalTextureRef texture = nullptr;  // guarded by _sourceLock
    float aspect = 16.0f / 9.0f;          // guarded by _sourceLock
    bool dirty = false;                   // guarded by _sourceLock: new frame not yet mipmapped

    // Render thread only. The capture is copied here once per new frame and mipmapped:
    // the panels are drawn at a fraction of the capture's size, and sampling the full-size
    // frame directly aliases (text shimmers and looks jagged at the slightest head motion).
    id<MTLTexture> mipmapped;

    // Render thread only.
    id<MTLBuffer> vertices;
    id<MTLBuffer> indices;
    NSUInteger index_count = 0;
    float mesh_aspect = 0, mesh_distance = 0, mesh_arc = 0, mesh_center = 0, mesh_pitch = 0;
    float mesh_curvature = -1.0f;
    float mesh_scale = 0.0f;
    float mesh_tilt = -1.0f;
    float mesh_zoom = 0.0f;
    bool mesh_wall = false;
    float brightness = 1.0f;  // render thread: eased towards 1 (looked at) or dimOthers

    // AI 3D. Guarded by _sourceLock: the latest depth map, its range and whether it starts a scene.
    CVMetalTextureRef depth = nullptr;
    float depth_low = 0.0f, depth_high = 1.0f;
    simd_float4 depth_rect = {0, 0, 1, 1};  // the video's part of the frame (the rest stays flat)
    bool depth_dirty = false, depth_cut = false;
    // Render thread: the smoothed maps (ping-pong); `smoothed_valid` false until the first one.
    id<MTLTexture> smoothed[2];
    int smoothed_index = 0;
    bool smoothed_valid = false;
};

}  // namespace

@implementation RNRenderOptions
- (instancetype)init {
    if ((self = [super init])) {
        _sideBySide = YES;
        _ipdMeters = 0.063f;
        _distanceMeters = 2.0f;
        _arcDegrees = 50.0f;
        _gapDegrees = 4.0f;
        _curved = NO;
        _curvature = 0.0f;
        _tilt = 1.0f;
        _neckModel = NO;
        _fovYDegrees = 24.5f;
        _eyeAspect = 16.0f / 9.0f;
        _k1 = 0.0f;
        _k2 = 0.0f;
        _maxPredictionMs = 50.0f;
        _fixedPredictionMs = 0.0f;
        _presentLateMs = 0.0f;
        _framePeriodMs = 1000.0f / 60;
        _sampleAgeMs = 0.0f;
        _keyPanel = -1;
        _flatWall = YES;
        _supersample = 1;
        _dimOthers = 1.0f;
        _trackingAxes = 0;
        _staticHead = NO;
        _layoutArcDegrees = 0.0f;
        _focusPanel = -1;
        _cinema = NO;
        _zoom = 1.0f;
        _backgroundStyle = 0;
        _backgroundBrightness = 0.6f;
        _depthStrength = 0.03f;
        _depthConvergence = 0.35f;
        _depthSmoothing = 0.35f;
    }
    return self;
}
@end

// The panel arc as seen at the current distance (Distancia acerca y aleja de verdad).
static float panel_arc(RNRenderOptions* o) {
    return core::arc_at_distance(o.arcDegrees * kDegToRad, o.distanceMeters) * std::clamp(o.zoom, 0.2f, 4.0f);
}

// The flat wall (RNRenderOptions.flatWall): the space at real size and its overview; never in the
// glasses' pinned mode, where the glasses anchor the image and the arrangement is fitted in it.
static bool wall_mode(RNRenderOptions* o) { return o.flatWall && !o.staticHead; }

@implementation RNSpatialRenderer {
    id<MTLCommandQueue> _queue;
    id<MTLRenderPipelineState> _screenPipeline;
    id<MTLRenderPipelineState> _sharpPipeline;        // the space at real size: Catmull-Rom
    id<MTLRenderPipelineState> _distortPipeline;
    id<MTLRenderPipelineState> _stereoPipeline;       // AI 3D panel
    id<MTLRenderPipelineState> _depthBlendPipeline;   // AI 3D temporal smoothing (R16Float)
    id<MTLRenderPipelineState> _backgroundPipeline;   // around the panels
    id<MTLSamplerState> _mirrorSampler;               // background picture, mirrored beyond its edges
    id<MTLTexture> _backgroundImage;                  // guarded by _sourceLock
    id<MTLRenderPipelineState> _cursorPipeline;       // premultiplied alpha over the panel
    id<MTLTexture> _cursorTexture;                    // guarded by _sourceLock
    CGPoint _cursorHot;                               // guarded by _sourceLock (points)
    CGSize _cursorSize;                               // guarded by _sourceLock (points)
    NSInteger _cursorPanel;                           // guarded by _sourceLock, -1 none
    CGPoint _cursorPos;                               // guarded by _sourceLock (points in its display)
    CGSize _cursorDisplay;                            // guarded by _sourceLock
    uint64_t _cursorGeneration;                       // guarded by _sourceLock
    id<MTLSamplerState> _depthSampler;
    id<MTLSamplerState> _sampler;
    CVMetalTextureCacheRef _textureCache;

    os_unfair_lock _sourceLock;
    Panel _panels[kMaxPanels];
    NSUInteger _panelCount;  // guarded by _sourceLock
    std::vector<core::GridCell> _cells;  // guarded by _sourceLock
    std::vector<core::PanelPose> _poses;  // guarded by _sourceLock: hand-placed panels
    std::vector<core::PanelPose> _floating;  // guarded by _sourceLock: floating windows, after the monitors

    id<MTLTexture> _placeholder;
    id<MTLTexture> _overlay;            // guarded by _sourceLock: start-up message, nil = none
    id<MTLBuffer> _overlayVertices;     // render thread
    NSUInteger _overlayMeshHeight;      // render thread: the height its mesh was built for
    id<MTLBuffer> _overlayIndices;
    NSUInteger _overlayIndexCount;
    id<MTLTexture> _eyeTextures[2];
    os_unfair_lock _gpuLock;  // the GPU times below, written by completion handlers
    double _gpuSumMs;
    int _gpuN;

    core::PanelDimmer _dimmer;     // render thread
    core::FrameSkipper _skipper;   // render thread
    uint64_t _lastFrameNs;         // render thread: for frame-rate independent fades
    uint64_t _sourceGeneration;    // guarded by _sourceLock: bumps on every new frame

    NSInteger _growPanel;          // guarded by _sourceLock: enlarged panel (-1: none)
    NSInteger _growFrom;           // guarded by _sourceLock: the one before, during the transition
    uint64_t _growStartNs;         // guarded by _sourceLock
}

- (BOOL)growAnimating {
    os_unfair_lock_lock(&_sourceLock);
    const bool moving = _growStartNs != 0 && static_cast<double>(now_ns() - _growStartNs) * 1e-9 < kGrowSeconds + 0.05;  // and a frame at the end
    os_unfair_lock_unlock(&_sourceLock);
    return moving;
}

- (BOOL)animating {
    return _dimmer.animating() || [self growAnimating];
}

- (NSInteger)growPanel {
    os_unfair_lock_lock(&_sourceLock);
    const NSInteger g = _growPanel;
    os_unfair_lock_unlock(&_sourceLock);
    return g;
}

- (void)setGrowPanel:(NSInteger)panel {
    os_unfair_lock_lock(&_sourceLock);
    if (panel != _growPanel) {
        _growFrom = _growPanel;
        _growPanel = panel;
        _growStartNs = now_ns();
        ++_sourceGeneration;
    }
    os_unfair_lock_unlock(&_sourceLock);
}

- (nullable instancetype)initWithDevice:(id<MTLDevice>)device
                            pixelFormat:(MTLPixelFormat)pixelFormat
                                  error:(NSError**)error {
    if (!(self = [super init])) return nil;
    _device = device;
    _options = [RNRenderOptions new];
    _queue = [device newCommandQueue];
    _sourceLock = OS_UNFAIR_LOCK_INIT;
    _panelCount = 1;
    _cursorPanel = -1;
    _growPanel = -1;
    _growFrom = -1;

    id<MTLLibrary> lib = [device newLibraryWithSource:@(kRNShaderSource) options:nil error:error];
    if (lib == nil) return nil;

    MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = [lib newFunctionWithName:@"screen_vertex"];
    d.fragmentFunction = [lib newFunctionWithName:@"screen_fragment"];
    d.colorAttachments[0].pixelFormat = pixelFormat;
    _screenPipeline = [device newRenderPipelineStateWithDescriptor:d error:error];
    if (_screenPipeline == nil) return nil;
    d.fragmentFunction = [lib newFunctionWithName:@"screen_sharp_fragment"];
    _sharpPipeline = [device newRenderPipelineStateWithDescriptor:d error:error];
    if (_sharpPipeline == nil) return nil;

    d.vertexFunction = [lib newFunctionWithName:@"fullscreen_vertex"];
    d.fragmentFunction = [lib newFunctionWithName:@"distort_fragment"];
    _distortPipeline = [device newRenderPipelineStateWithDescriptor:d error:error];
    if (_distortPipeline == nil) return nil;

    d.vertexFunction = [lib newFunctionWithName:@"screen_vertex"];
    d.fragmentFunction = [lib newFunctionWithName:@"stereo_screen_fragment"];
    _stereoPipeline = [device newRenderPipelineStateWithDescriptor:d error:error];
    if (_stereoPipeline == nil) return nil;

    {
        MTLRenderPipelineDescriptor* c = [MTLRenderPipelineDescriptor new];
        c.vertexFunction = [lib newFunctionWithName:@"screen_vertex"];
        c.fragmentFunction = [lib newFunctionWithName:@"cursor_fragment"];
        c.colorAttachments[0].pixelFormat = pixelFormat;
        c.colorAttachments[0].blendingEnabled = YES;
        c.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorOne;
        c.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        c.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorZero;
        c.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOne;
        _cursorPipeline = [device newRenderPipelineStateWithDescriptor:c error:error];
        if (_cursorPipeline == nil) return nil;
    }

    d.vertexFunction = [lib newFunctionWithName:@"fullscreen_vertex"];
    d.fragmentFunction = [lib newFunctionWithName:@"background_fragment"];
    _backgroundPipeline = [device newRenderPipelineStateWithDescriptor:d error:error];
    if (_backgroundPipeline == nil) return nil;

    d.vertexFunction = [lib newFunctionWithName:@"fullscreen_vertex"];
    d.fragmentFunction = [lib newFunctionWithName:@"depth_blend_fragment"];
    d.colorAttachments[0].pixelFormat = MTLPixelFormatR16Float;
    _depthBlendPipeline = [device newRenderPipelineStateWithDescriptor:d error:error];
    if (_depthBlendPipeline == nil) return nil;

    MTLSamplerDescriptor* s = [MTLSamplerDescriptor new];
    s.minFilter = MTLSamplerMinMagFilterLinear;
    s.magFilter = MTLSamplerMinMagFilterLinear;
    s.sAddressMode = MTLSamplerAddressModeClampToZero;
    s.tAddressMode = MTLSamplerAddressModeClampToZero;
    s.mipFilter = MTLSamplerMipFilterLinear;  // trilinear + anisotropic: crisp, stable text
    s.maxAnisotropy = 16;
    _sampler = [device newSamplerStateWithDescriptor:s];
    s.sAddressMode = MTLSamplerAddressModeMirrorRepeat;
    s.tAddressMode = MTLSamplerAddressModeMirrorClampToEdge;
    _mirrorSampler = [device newSamplerStateWithDescriptor:s];
    // Depth: plain bilinear, edges clamped (beyond the frame is as deep as its border).
    MTLSamplerDescriptor* ds = [MTLSamplerDescriptor new];
    ds.minFilter = MTLSamplerMinMagFilterLinear;
    ds.magFilter = MTLSamplerMinMagFilterLinear;
    ds.sAddressMode = MTLSamplerAddressModeClampToEdge;
    ds.tAddressMode = MTLSamplerAddressModeClampToEdge;
    _depthSampler = [device newSamplerStateWithDescriptor:ds];

    MTLTextureDescriptor* pd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm_sRGB
                                                                                  width:1
                                                                                 height:1
                                                                              mipmapped:NO];
    _placeholder = [device newTextureWithDescriptor:pd];
    const uint8_t grey[4] = {28, 24, 22, 255};  // BGRA: dark panel until the first frame
    [_placeholder replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:grey bytesPerRow:4];

    if (CVMetalTextureCacheCreate(kCFAllocatorDefault, nil, device, nil, &_textureCache) != kCVReturnSuccess) {
        if (error != nullptr) {
            *error = [NSError errorWithDomain:@"RNSpatialRenderer"
                                         code:1
                                     userInfo:@{NSLocalizedDescriptionKey : @"CVMetalTextureCacheCreate failed"}];
        }
        return nil;
    }
    return self;
}

- (void)dealloc {
    for (auto& p : _panels) {
        if (p.texture != nullptr) CFRelease(p.texture);
        if (p.depth != nullptr) CFRelease(p.depth);
    }
    if (_textureCache != nullptr) CFRelease(_textureCache);
}

- (NSUInteger)panelCount {
    os_unfair_lock_lock(&_sourceLock);
    const NSUInteger n = _panelCount;
    os_unfair_lock_unlock(&_sourceLock);
    return n;
}

- (void)setPanelCount:(NSUInteger)count {
    os_unfair_lock_lock(&_sourceLock);
    _panelCount = std::clamp<NSUInteger>(count, 1, kMaxPanels);
    os_unfair_lock_unlock(&_sourceLock);
}

- (void)setGridColumns:(NSArray<NSNumber*>*)columns rows:(NSArray<NSNumber*>*)rows {
    std::vector<core::GridCell> cells;
    for (NSUInteger i = 0; i < MIN(columns.count, rows.count); ++i) {
        cells.push_back({columns[i].intValue, rows[i].intValue});
    }
    os_unfair_lock_lock(&_sourceLock);
    _cells = std::move(cells);
    os_unfair_lock_unlock(&_sourceLock);
}

// Layout for `count` panels of `aspect`: the grid cells if they match, else one row.
- (void)setPanelPosesYaw:(NSArray<NSNumber*>*)yaw pitch:(NSArray<NSNumber*>*)pitch scale:(NSArray<NSNumber*>*)scale {
    std::vector<core::PanelPose> poses;
    for (NSUInteger i = 0; i < MIN(yaw.count, MIN(pitch.count, scale.count)); ++i) {
        poses.push_back({yaw[i].floatValue, pitch[i].floatValue, scale[i].floatValue});
    }
    os_unfair_lock_lock(&_sourceLock);
    _poses = std::move(poses);
    os_unfair_lock_unlock(&_sourceLock);
}

- (void)setCursorImage:(CGImageRef)image hotspot:(CGPoint)hotspot pointSize:(CGSize)size {
    id<MTLTexture> tex = nil;
    if (image != nullptr) {
        const size_t w = CGImageGetWidth(image), h = CGImageGetHeight(image);
        if (w > 0 && h > 0 && w <= 512 && h <= 512) {
            std::vector<uint8_t> px(w * h * 4, 0);
            CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
            CGContextRef ctx = CGBitmapContextCreate(px.data(), w, h, 8, w * 4, cs,
                                                     kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
            CGColorSpaceRelease(cs);
            if (ctx != nullptr) {
                CGContextDrawImage(ctx, CGRectMake(0, 0, w, h), image);
                CGContextRelease(ctx);
                MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm_sRGB
                                                                                              width:w
                                                                                             height:h
                                                                                          mipmapped:NO];
                tex = [_device newTextureWithDescriptor:td];
                [tex replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:px.data() bytesPerRow:w * 4];
            }
        }
    }
    os_unfair_lock_lock(&_sourceLock);
    _cursorTexture = tex;
    _cursorHot = hotspot;
    _cursorSize = size;
    if (_cursorPanel >= 0) _cursorGeneration += 1;
    os_unfair_lock_unlock(&_sourceLock);
}

- (void)setCursorPanel:(NSInteger)panel position:(CGPoint)position displaySize:(CGSize)displaySize {
    os_unfair_lock_lock(&_sourceLock);
    if (panel != _cursorPanel || (panel >= 0 && (!CGPointEqualToPoint(position, _cursorPos) ||
                                                  !CGSizeEqualToSize(displaySize, _cursorDisplay)))) {
        _cursorPanel = panel;
        _cursorPos = position;
        _cursorDisplay = displaySize;
        _cursorGeneration += 1;
    }
    os_unfair_lock_unlock(&_sourceLock);
}

- (void)setOverlayText:(NSString*)text {
    id<MTLTexture> tex = nil;
    if (text.length > 0) {
        // Dark rounded card with white text (black would be see-through on the glasses); a title
        // and any number of lines under it (core::overlay_line).
        NSArray<NSString*>* lines = [text componentsSeparatedByString:@"\n"];
        const int n = static_cast<int>(std::min<NSUInteger>(lines.count, core::kOverlayMaxLines));
        const size_t w = core::kOverlayCardWidth, h = static_cast<size_t>(core::overlay_card_height(n));
        CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGContextRef ctx = CGBitmapContextCreate(nullptr, w, h, 8, w * 4, cs,
                                                 kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
        CGColorSpaceRelease(cs);
        if (ctx == nullptr) return;
        CGContextSetRGBFillColor(ctx, 0.13, 0.15, 0.16, 1.0);
        CGPathRef card = CGPathCreateWithRoundedRect(CGRectMake(0, 0, w, h), 48, 48, nullptr);
        CGContextAddPath(ctx, card);
        CGContextFillPath(ctx);
        CGPathRelease(card);
        for (int i = 0; i < n; ++i) {
            const core::OverlayLine l = core::overlay_line(i, n);
            const CGFloat size = l.font_px;
            CTFontRef font = CTFontCreateUIFontForLanguage(i == 0 ? kCTFontUIFontEmphasizedSystem : kCTFontUIFontSystem, size, nullptr);
            CGColorRef color = CGColorCreateSRGB(1, 1, 1, i == 0 ? 1.0 : 0.75);
            NSDictionary* attrs = @{(id)kCTFontAttributeName : (__bridge id)font, (id)kCTForegroundColorAttributeName : (__bridge id)color};
            CTLineRef line = CTLineCreateWithAttributedString(
                (__bridge CFAttributedStringRef)[[NSAttributedString alloc] initWithString:lines[i] attributes:attrs]);
            const CGRect bounds = CTLineGetBoundsWithOptions(line, 0);
            // Core Graphics counts from the bottom: the baseline a third of the size below the centre.
            CGContextSetTextPosition(ctx, (w - bounds.size.width) / 2, h - l.center_y_px - 0.35 * size);
            CTLineDraw(line, ctx);
            CFRelease(line);
            CGColorRelease(color);
            CFRelease(font);
        }
        MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm_sRGB
                                                                                     width:w
                                                                                    height:h
                                                                                 mipmapped:NO];
        tex = [_device newTextureWithDescriptor:d];
        [tex replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:CGBitmapContextGetData(ctx) bytesPerRow:w * 4];
        CGContextRelease(ctx);
    }
    os_unfair_lock_lock(&_sourceLock);
    _overlay = tex;
    ++_sourceGeneration;  // a new picture: the next frame is drawn even with a still head
    os_unfair_lock_unlock(&_sourceLock);
}

- (void)setFloatingPosesYaw:(NSArray<NSNumber*>*)yaw pitch:(NSArray<NSNumber*>*)pitch scale:(NSArray<NSNumber*>*)scale {
    std::vector<core::PanelPose> poses;
    for (NSUInteger i = 0; i < MIN(yaw.count, MIN(pitch.count, scale.count)); ++i) {
        poses.push_back({yaw[i].floatValue, pitch[i].floatValue, scale[i].floatValue});
    }
    os_unfair_lock_lock(&_sourceLock);
    _floating = std::move(poses);
    os_unfair_lock_unlock(&_sourceLock);
}

// Monitors first (grid or hand-placed), then the floating windows.
- (std::vector<core::PanelPlacement>)placements:(NSUInteger)count aspect:(float)aspect {
    RNRenderOptions* o = _options;
    os_unfair_lock_lock(&_sourceLock);
    std::vector<core::PanelPose> floating = _floating;
    os_unfair_lock_unlock(&_sourceLock);
    const NSUInteger floats = std::min<NSUInteger>(floating.size(), count > 0 ? count - 1 : 0);
    auto out = [self monitorPlacements:count - floats aspect:aspect];
    // Placed at the editor's panel size: angles follow the arc actually shown (distance,
    // pinned fitting), so the arrangement keeps its shape.
    const float placed = (o.layoutArcDegrees > 0.0f ? o.layoutArcDegrees : o.arcDegrees) * kDegToRad;
    const float k = placed > 0.0f ? panel_arc(o) / placed : 1.0f;
    floating.resize(floats);
    for (const auto& pl : core::placements_from_poses(floating, k)) out.push_back(pl);
    if (floats > 0) {  // windows never over the monitors, whatever their shape
        float aspects[kMaxPanels];
        os_unfair_lock_lock(&_sourceLock);
        for (NSUInteger i = 0; i < kMaxPanels; ++i) aspects[i] = _panels[i].aspect;
        os_unfair_lock_unlock(&_sourceLock);
        std::vector<float> w, h;
        for (size_t i = 0; i < out.size(); ++i) {
            w.push_back(panel_arc(o) * out[i].scale);
            h.push_back(core::panel_vertical_arc(w.back(), aspects[std::min<size_t>(i, kMaxPanels - 1)], true));
        }
        core::separate_floating(out, w, h, count - floats, wall_mode(o) ? 0.0f : o.gapDegrees * kDegToRad);
    }
    if (wall_mode(o)) {
        core::lift_wall(out, o.panelsPitchRadians);  // the recentre's elevation, rows still touching
    } else if (!o.staticHead) {
        core::lift_placements(out, o.panelsPitchRadians);
    }
    // Cinema: the focused panel straight ahead, large (the others are not drawn), but never
    // larger than the glasses' image: at real size a panel nearly fills it already, and 1.6x
    // left the film cut on every side.
    if (o.cinema && o.focusPanel >= 0 && static_cast<NSUInteger>(o.focusPanel) < out.size()) {
        os_unfair_lock_lock(&_sourceLock);
        const float a = _panels[std::min<NSUInteger>(static_cast<NSUInteger>(o.focusPanel), kMaxPanels - 1)].aspect;
        os_unfair_lock_unlock(&_sourceLock);
        // Fitted at zoom 1; the zoom (⌘⌥+ / -) then scales the fitted size: larger than the
        // image if asked (fitted with the zoom inside, ⌘⌥+ did nothing in cinema).
        const float arc = panel_arc(o) / std::clamp(o.zoom, 0.2f, 4.0f);
        const float fw = core::horizontal_fov(o.fovYDegrees * kDegToRad, o.eyeAspect), fh = o.fovYDegrees * kDegToRad;
        const float fit = 0.95f * std::min(fw / arc, fh / core::panel_vertical_arc(arc, a, true));
        // Centred where you looked on entering cinema: straight ahead of the last recentre left
        // uneven borders (top and right with the head a little down and left).
        out[static_cast<size_t>(o.focusPanel)] = core::PanelPlacement{o.cinemaYawRadians, o.cinemaPitchRadians, std::min(kCinemaScale, fit)};
    }
    if (o.staticHead) [self arrangeInGlassesImage:out];
    return out;
}

// Glasses in their own pinned mode anchor the image we send and let the eyes roam over it:
// nothing outside it can ever be seen. With a panel highlighted (the one you look at, or focus
// / cinema) that panel fills the whole image, sharp, as the Mac's own screen would; the
// arrangement keeps its shape, so the others lie just beyond the edges, where a look picks
// them. With none, the whole arrangement is fitted in the image. Changing the highlighted
// panel pans and zooms there over a third of a second.
- (void)arrangeInGlassesImage:(std::vector<core::PanelPlacement>&)out {
    RNRenderOptions* o = _options;
    os_unfair_lock_lock(&_sourceLock);
    float aspects[kMaxPanels];
    for (NSUInteger i = 0; i < kMaxPanels; ++i) aspects[i] = _panels[i].aspect;
    const NSInteger to = _growPanel, from = _growFrom;
    const double t = _growStartNs == 0 ? 1.0 : std::min(1.0, static_cast<double>(now_ns() - _growStartNs) * 1e-9 / kGrowSeconds);
    os_unfair_lock_unlock(&_sourceLock);
    if (o.focusPanel >= 0) {
        [self fill:out panel:o.focusPanel aspects:aspects];
        return;
    }
    auto target = out;
    [self fill:target panel:to aspects:aspects];
    if (t >= 1.0 || from == to) {
        out = target;
        return;
    }
    [self fill:out panel:from aspects:aspects];
    // The pan eases in and out (cubic); the size settles with a slight overshoot, as if the
    // panel stepped forward: livelier than the same curve for both.
    const float u = static_cast<float>(t);
    const float pan = u < 0.5f ? 4.0f * u * u * u : 1.0f - std::pow(-2.0f * u + 2.0f, 3.0f) / 2.0f;
    constexpr float c1 = 0.9f, c3 = c1 + 1.0f;  // ease-out-back, ~4 % overshoot
    const float grow = 1.0f + c3 * std::pow(u - 1.0f, 3.0f) + c1 * std::pow(u - 1.0f, 2.0f);
    for (size_t i = 0; i < out.size(); ++i) {
        out[i].yaw_rad += (target[i].yaw_rad - out[i].yaw_rad) * pan;
        out[i].pitch_rad += (target[i].pitch_rad - out[i].pitch_rad) * pan;
        out[i].scale += (target[i].scale - out[i].scale) * grow;
    }
}

// `panel` centred and scaled to fill the glasses' image (the others keep their place around
// it); -1 or out of range: everything fitted, zoomed by distance and size (core::pinned_zoom).
- (void)fill:(std::vector<core::PanelPlacement>&)out panel:(NSInteger)panel aspects:(const float*)aspects {
    RNRenderOptions* o = _options;
    const float arc = panel_arc(o);
    const float fw = core::horizontal_fov(o.fovYDegrees * kDegToRad, o.eyeAspect), fh = o.fovYDegrees * kDegToRad;
    float y0 = 1e9f, y1 = -1e9f, p0 = 1e9f, p1 = -1e9f;
    const bool one = panel >= 0 && static_cast<size_t>(panel) < out.size();
    for (size_t i = 0; i < out.size() && i < kMaxPanels; ++i) {
        if (one && static_cast<NSInteger>(i) != panel) continue;
        const float w = arc * out[i].scale, h = core::panel_vertical_arc(w, aspects[i], true);
        y0 = std::min(y0, out[i].yaw_rad - w / 2);
        y1 = std::max(y1, out[i].yaw_rad + w / 2);
        p0 = std::min(p0, out[i].pitch_rad - h / 2);
        p1 = std::max(p1, out[i].pitch_rad + h / 2);
    }
    if (y1 <= y0 || p1 <= p0) return;
    const float room = one ? 1.0f : 0.94f * core::pinned_zoom(arc);
    const float s = room * std::min(fw / (y1 - y0), fh / (p1 - p0));
    const float yc = 0.5f * (y0 + y1), pc = 0.5f * (p0 + p1);
    for (auto& pl : out) {
        pl.yaw_rad = (pl.yaw_rad - yc) * s;
        pl.pitch_rad = (pl.pitch_rad - pc) * s;
        pl.scale *= s;
    }
}

// A panel fills the glasses' image: drawn flat, like the Mac's own screen (a curve would
// bend its edges and soften the 1:1 pixels).
- (BOOL)fillingImage {
    RNRenderOptions* o = _options;
    if (!o.staticHead) return NO;
    if (o.focusPanel >= 0) return YES;
    os_unfair_lock_lock(&_sourceLock);
    const bool grown = _growPanel >= 0;
    os_unfair_lock_unlock(&_sourceLock);
    return grown;
}

- (std::vector<core::PanelPlacement>)monitorPlacements:(NSUInteger)count aspect:(float)aspect {
    RNRenderOptions* o = _options;
    os_unfair_lock_lock(&_sourceLock);
    std::vector<core::GridCell> cells = _cells;
    std::vector<core::PanelPose> poses = _poses;
    os_unfair_lock_unlock(&_sourceLock);
    if (poses.size() == count) {
        const float placed = (o.layoutArcDegrees > 0.0f ? o.layoutArcDegrees : o.arcDegrees) * kDegToRad;
        return core::placements_from_poses(poses, placed > 0.0f ? panel_arc(o) / placed : 1.0f);
    }
    if (cells.size() != count) {
        cells.clear();
        for (NSUInteger i = 0; i < count; ++i) cells.push_back({static_cast<int>(i), 0});
    }
    const float arc = panel_arc(o);
    if (wall_mode(o)) return core::wall_grid_layout(cells, arc, aspect);  // one plane, edge to edge
    return core::grid_layout(cells, arc, core::panel_vertical_arc(arc, aspect, true), o.gapDegrees * kDegToRad);
}

- (float)overviewZoom {
    RNRenderOptions* o = _options;
    os_unfair_lock_lock(&_sourceLock);
    const NSUInteger count = _panelCount;
    float aspects[kMaxPanels];
    for (NSUInteger i = 0; i < kMaxPanels; ++i) aspects[i] = _panels[i].aspect;
    os_unfair_lock_unlock(&_sourceLock);
    const float saved = o.zoom;
    o.zoom = 1.0f;
    const auto pls = [self placements:count aspect:aspects[0]];
    const float arc = panel_arc(o);
    o.zoom = saved;
    float y0 = 1e9f, y1 = -1e9f, p0 = 1e9f, p1 = -1e9f;
    for (size_t i = 0; i < pls.size() && i < kMaxPanels; ++i) {
        const float w = arc * pls[i].scale, h = core::panel_vertical_arc(w, aspects[i], true);
        // Both sides of straight ahead: the zoom scales about it.
        y0 = std::min(y0, pls[i].yaw_rad - w / 2);
        y1 = std::max(y1, pls[i].yaw_rad + w / 2);
        p0 = std::min(p0, pls[i].pitch_rad - h / 2);
        p1 = std::max(p1, pls[i].pitch_rad + h / 2);
    }
    if (y1 <= y0 || p1 <= p0) return 1.0f;
    const float fw = core::horizontal_fov(o.fovYDegrees * kDegToRad, o.eyeAspect), fh = o.fovYDegrees * kDegToRad;
    const float half_w = std::max(std::fabs(y0), std::fabs(y1)), half_h = std::max(std::fabs(p0), std::fabs(p1));
    return std::clamp(0.92f * std::min(fw / (2 * half_w), fh / (2 * half_h)), 0.2f, 1.0f);
}

- (float)fitArcDegreesForPanels:(NSUInteger)count {
    RNRenderOptions* o = _options;
    const float hfov = core::horizontal_fov(o.fovYDegrees * kDegToRad, o.eyeAspect);
    return core::fit_panel_arc(hfov, static_cast<int>(count), o.gapDegrees * kDegToRad, 1.0f * kDegToRad) / kDegToRad;
}

- (float)pinnedArcDegreesForPanels:(NSUInteger)count sizeDegrees:(float)sizeDegrees {
    const float d = _options.distanceMeters;
    const float fit = [self fitArcDegreesForPanels:count] * kDegToRad;
    const float seen = std::min(fit, core::arc_at_distance(sizeDegrees * kDegToRad, d));
    return core::arc_at_distance(seen, 2.0f, d) / kDegToRad;  // undo the distance scaling the render applies
}

- (BOOL)setBackgroundImageAtPath:(NSString*)path {
    id<MTLTexture> texture = nil;
    if (path.length > 0) {
        CGImageSourceRef src = CGImageSourceCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:path], nullptr);
        // At most 4096 px on the long side (a panorama for the whole room does not need more).
        NSDictionary* opts = @{(id)kCGImageSourceCreateThumbnailFromImageAlways: @YES,
                               (id)kCGImageSourceThumbnailMaxPixelSize: @4096,
                               (id)kCGImageSourceCreateThumbnailWithTransform: @YES};
        CGImageRef image = src ? CGImageSourceCreateThumbnailAtIndex(src, 0, (__bridge CFDictionaryRef)opts) : nullptr;
        if (src) CFRelease(src);
        if (image) {
            const size_t w = CGImageGetWidth(image), h = CGImageGetHeight(image);
            std::vector<uint8_t> pixels(w * h * 4);
            CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
            CGContextRef ctx = CGBitmapContextCreate(pixels.data(), w, h, 8, w * 4, cs,
                                                     kCGImageAlphaNoneSkipLast | kCGBitmapByteOrder32Big);
            CGColorSpaceRelease(cs);
            if (ctx) {
                CGContextDrawImage(ctx, CGRectMake(0, 0, w, h), image);
                CGContextRelease(ctx);
                MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm_sRGB
                                                                                             width:w height:h mipmapped:NO];
                d.usage = MTLTextureUsageShaderRead;
                texture = [_device newTextureWithDescriptor:d];
                [texture replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:pixels.data() bytesPerRow:w * 4];
            }
            CGImageRelease(image);
        }
    }
    os_unfair_lock_lock(&_sourceLock);
    _backgroundImage = texture;
    os_unfair_lock_unlock(&_sourceLock);
    return texture != nil || path.length == 0;
}

- (void)centreCinemaOnGaze:(simd_quatf)orientation {
    const core::PanelPlacement g = core::gaze_placement(from_simd(orientation));
    _options.cinemaYawRadians = g.yaw_rad;
    _options.cinemaPitchRadians = g.pitch_rad;
}

- (NSInteger)panelAtGaze:(simd_quatf)orientation {
    RNRenderOptions* o = _options;
    if (o.focusPanel >= 0 && static_cast<NSUInteger>(o.focusPanel) < self.panelCount) return o.focusPanel;
    os_unfair_lock_lock(&_sourceLock);
    const float aspect = _panels[0].aspect;
    os_unfair_lock_unlock(&_sourceLock);
    const float arc = panel_arc(o);
    core::Quat head = core::track_axes(from_simd(orientation), static_cast<core::TrackingAxes>(o.trackingAxes));
    if (o.vehicleMode && !o.staticHead) head = core::scale_heading(head, core::kVehicleHeadingScale);
    const auto pls = [self placements:self.panelCount aspect:aspect];
    if (wall_mode(o)) return core::panel_at_gaze_wall(head, pls, arc, aspect);
    return core::panel_at_gaze(head, pls, arc, core::panel_vertical_arc(arc, aspect, true), o.gapDegrees * kDegToRad);
}

- (void)setSourcePixelBuffer:(CVPixelBufferRef)pb {
    [self setSourcePixelBuffer:pb forPanel:0];
}

- (void)setSourcePixelBuffer:(CVPixelBufferRef)pb forPanel:(NSUInteger)panel {
    if (panel >= kMaxPanels) return;
    const size_t w = CVPixelBufferGetWidth(pb);
    const size_t h = CVPixelBufferGetHeight(pb);
    CVMetalTextureRef tex = nullptr;
    if (CVMetalTextureCacheCreateTextureFromImage(kCFAllocatorDefault, _textureCache, pb, nil,
                                                  MTLPixelFormatBGRA8Unorm_sRGB, w, h, 0, &tex) != kCVReturnSuccess) {
        return;
    }
    os_unfair_lock_lock(&_sourceLock);
    Panel& p = _panels[panel];
    CVMetalTextureRef old = p.texture;
    p.texture = tex;
    p.dirty = true;
    ++_sourceGeneration;
    p.aspect = h > 0 ? static_cast<float>(w) / static_cast<float>(h) : 16.0f / 9.0f;
    os_unfair_lock_unlock(&_sourceLock);
    if (old != nullptr) CFRelease(old);
}

- (void)setSourcePixelBuffer:(CVPixelBufferRef)pb
                       depth:(CVPixelBufferRef)depth
                         low:(float)low
                        high:(float)high
                    sceneCut:(BOOL)sceneCut
                   videoRect:(CGRect)videoRect
                    forPanel:(NSUInteger)panel {
    if (panel >= kMaxPanels) return;
    CVMetalTextureRef dtex = nullptr;
    if (CVMetalTextureCacheCreateTextureFromImage(kCFAllocatorDefault, _textureCache, depth, nil, MTLPixelFormatR16Float,
                                                  CVPixelBufferGetWidth(depth), CVPixelBufferGetHeight(depth), 0,
                                                  &dtex) != kCVReturnSuccess) {
        [self setSourcePixelBuffer:pb forPanel:panel];
        return;
    }
    os_unfair_lock_lock(&_sourceLock);
    Panel& p = _panels[panel];
    CVMetalTextureRef old = p.depth;
    p.depth = dtex;
    p.depth_low = low;
    p.depth_high = high;
    p.depth_cut = p.depth_cut || sceneCut;  // a cut between two drawn frames still resets
    p.depth_rect = simd_make_float4(static_cast<float>(CGRectGetMinX(videoRect)), static_cast<float>(CGRectGetMinY(videoRect)),
                                    static_cast<float>(CGRectGetMaxX(videoRect)), static_cast<float>(CGRectGetMaxY(videoRect)));
    p.depth_dirty = true;
    os_unfair_lock_unlock(&_sourceLock);
    if (old != nullptr) CFRelease(old);
    [self setSourcePixelBuffer:pb forPanel:panel];  // same lock round: drawn together from the next frame
}

- (void)clearDepthForPanel:(NSUInteger)panel {
    if (panel >= kMaxPanels) return;
    os_unfair_lock_lock(&_sourceLock);
    Panel& p = _panels[panel];
    CVMetalTextureRef old = p.depth;
    p.depth = nullptr;
    p.depth_dirty = true;  // tells the render thread to drop its smoothed maps
    ++_sourceGeneration;
    os_unfair_lock_unlock(&_sourceLock);
    if (old != nullptr) CFRelease(old);
}

// AI 3D: the new depth map blended into the smoothed one (render thread).
- (void)blendDepth:(Panel&)p raw:(id<MTLTexture>)raw low:(float)low high:(float)high cut:(bool)cut
     commandBuffer:(id<MTLCommandBuffer>)cb {
    if (p.smoothed[0] == nil || p.smoothed[0].width != raw.width || p.smoothed[0].height != raw.height) {
        MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR16Float
                                                                                     width:raw.width
                                                                                    height:raw.height
                                                                                 mipmapped:NO];
        d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        d.storageMode = MTLStorageModePrivate;
        for (auto& t : p.smoothed) t = [_device newTextureWithDescriptor:d];
        p.smoothed_valid = false;
    }
    const int next = 1 - p.smoothed_index;
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = p.smoothed[next];
    pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:pass];
    const float alpha = cut || !p.smoothed_valid ? 1.0f : std::clamp(_options.depthSmoothing, 0.01f, 1.0f);
    const DepthBlendUniforms u{low, high, alpha};
    [enc setRenderPipelineState:_depthBlendPipeline];
    [enc setFragmentTexture:raw atIndex:0];
    [enc setFragmentTexture:p.smoothed[p.smoothed_index] atIndex:1];
    [enc setFragmentSamplerState:_depthSampler atIndex:0];
    [enc setFragmentBytes:&u length:sizeof(u) atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [enc endEncoding];
    p.smoothed_index = next;
    p.smoothed_valid = true;
}

static float effective_curvature(RNRenderOptions* o) {
    return o.curved && o.curvature <= 0.0f ? 1.0f : std::clamp(o.curvature, 0.0f, 1.0f);
}

- (void)rebuildMesh:(Panel&)p aspect:(float)aspect placement:(core::PanelPlacement)pl wall:(bool)wall {
    RNRenderOptions* o = _options;
    const float center = pl.yaw_rad;
    const float k = [self fillingImage] || wall ? 0.0f : effective_curvature(o);
    if (p.vertices != nil && aspect == p.mesh_aspect && o.distanceMeters == p.mesh_distance &&
        o.arcDegrees == p.mesh_arc && center == p.mesh_center && pl.pitch_rad == p.mesh_pitch &&
        k == p.mesh_curvature && pl.scale == p.mesh_scale && o.tilt == p.mesh_tilt && o.zoom == p.mesh_zoom &&
        wall == p.mesh_wall) {
        return;
    }
    core::ScreenMesh mesh;
    if (wall) {  // flat wall: a plain rectangle in the shared plane, no rotation of its own
        mesh = core::wall_panel_mesh(o.distanceMeters, panel_arc(o), aspect, pl);
    } else {
        const float arc = panel_arc(o) * pl.scale;
        const float width = core::panel_width(o.distanceMeters, arc, true);  // a sheet: bending keeps its width
        mesh = core::bent_screen_mesh(o.distanceMeters, arc, width / aspect, k > 0.0f ? 64 : 1, k, center);
        core::place_pitched(mesh, pl.pitch_rad, o.tilt, o.distanceMeters);
    }
    p.vertices = [_device newBufferWithBytes:mesh.vertices.data()
                                      length:mesh.vertices.size() * sizeof(core::ScreenVertex)
                                     options:MTLResourceStorageModeShared];
    p.indices = [_device newBufferWithBytes:mesh.indices.data()
                                     length:mesh.indices.size() * sizeof(uint16_t)
                                    options:MTLResourceStorageModeShared];
    p.index_count = mesh.indices.size();
    p.mesh_aspect = aspect;
    p.mesh_distance = o.distanceMeters;
    p.mesh_arc = o.arcDegrees;
    p.mesh_center = center;
    p.mesh_pitch = pl.pitch_rad;
    p.mesh_curvature = k;
    p.mesh_scale = pl.scale;
    p.mesh_tilt = o.tilt;
    p.mesh_zoom = o.zoom;
    p.mesh_wall = wall;
}

// Panels you are not looking at fade to dimOthers (core::PanelDimmer, shared with Windows).
- (void)updateBrightness:(NSUInteger)count
                    head:(const core::Quat&)head
                  aspect:(float)aspect
              placements:(const std::vector<core::PanelPlacement>&)placements
                   nowNs:(uint64_t)nowNs {
    RNRenderOptions* o = _options;
    int gaze = -1;
    float dim = o.dimOthers;
    if (count > 1 && o.dimOthers < 1.0f) {
        const float arc = panel_arc(o);
        gaze = wall_mode(o) ? core::panel_at_gaze_wall(head, placements, arc, aspect)
                            : core::panel_at_gaze(head, placements, arc, core::panel_vertical_arc(arc, aspect, true),
                                                  o.gapDegrees * kDegToRad);
    }
    // Glasses' pinned mode with a highlighted panel: the ones around it step back.
    if (o.staticHead && count > 1) {
        os_unfair_lock_lock(&_sourceLock);
        const NSInteger grown = _growPanel;
        os_unfair_lock_unlock(&_sourceLock);
        if (grown >= 0) {
            gaze = static_cast<int>(grown);
            dim = std::min(dim, kGrowDim);
        }
    }
    const float dt = _lastFrameNs > 0 && nowNs > _lastFrameNs ? std::min(0.1f, (nowNs - _lastFrameNs) * 1e-9f) : 0.0f;
    _lastFrameNs = nowNs;
    _dimmer.update(count, gaze, dim, dt);
    for (NSUInteger i = 0; i < count; ++i) _panels[i].brightness = _dimmer.brightness(i);
}

- (BOOL)canSkipFrameWithOrientation:(simd_quatf)orientation
                    angularVelocity:(simd_float3)angularVelocity
                           tracking:(BOOL)tracking
                             forced:(BOOL)forced {
    os_unfair_lock_lock(&_sourceLock);
    const uint64_t generation = _sourceGeneration + _cursorGeneration;
    os_unfair_lock_unlock(&_sourceLock);
    core::FrameSkipper::Input in;
    in.orientation = _options.staticHead ? core::Quat{} : from_simd(orientation);
    in.angular_speed_rads = _options.staticHead ? 0.0f : simd_length(angularVelocity);
    in.source_generation = generation;
    in.tracking = tracking;
    const NSInteger bg = _options.backgroundStyle;
    in.animating = _dimmer.animating() || [self growAnimating] || bg == 1 || bg == 2 || bg == 4;  // moving backgrounds
    in.forced = forced;
    return _skipper.skip(in);
}

- (void)invalidateSkippedFrame {
    _skipper.invalidate();
}

- (void)ensureEyeTextures:(MTLPixelFormat)format width:(NSUInteger)w height:(NSUInteger)h mipmapped:(BOOL)mips {
    if (_eyeTextures[0] != nil && _eyeTextures[0].width == w && _eyeTextures[0].height == h &&
        _eyeTextures[0].pixelFormat == format && (_eyeTextures[0].mipmapLevelCount > 1) == mips) {
        return;
    }
    MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                                                 width:w
                                                                                height:h
                                                                             mipmapped:mips];
    d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    d.storageMode = MTLStorageModePrivate;
    for (auto& t : _eyeTextures) t = [_device newTextureWithDescriptor:d];
}

- (double)takeGPUMeanMs {
    os_unfair_lock_lock(&_gpuLock);  // zero-initialised ivar = OS_UNFAIR_LOCK_INIT
    const double mean = _gpuN > 0 ? _gpuSumMs / _gpuN : 0.0;
    _gpuSumMs = 0.0;
    _gpuN = 0;
    os_unfair_lock_unlock(&_gpuLock);
    return mean;
}

- (float)renderToDrawable:(id<CAMetalDrawable>)drawable
              orientation:(simd_quatf)orientation
          angularVelocity:(simd_float3)angularVelocity
             sampleHostNs:(uint64_t)sampleHostNs
             targetHostNs:(uint64_t)targetHostNs {
    return [self renderToDrawable:drawable orientation:orientation angularVelocity:angularVelocity
                     sampleHostNs:sampleHostNs targetHostNs:targetHostNs gpuDone:nil];
}

- (float)renderToDrawable:(id<CAMetalDrawable>)drawable
              orientation:(simd_quatf)orientation
          angularVelocity:(simd_float3)angularVelocity
             sampleHostNs:(uint64_t)sampleHostNs
             targetHostNs:(uint64_t)targetHostNs
                  gpuDone:(void (^)(double, double))gpuDone {
    RNRenderOptions* o = _options;

    // Pose prediction to the photon time.
    float horizon_s = 0.0f;
    if (sampleHostNs > 0 && targetHostNs > sampleHostNs) {
        horizon_s = std::min(static_cast<float>(targetHostNs - sampleHostNs) * 1e-9f, o.maxPredictionMs * 1e-3f);
    }
    // Extrapolating while the head holds still would only amplify noise: ramp the prediction
    // in with angular speed.
    const core::Vec3 w{angularVelocity.x, angularVelocity.y, angularVelocity.z};
    horizon_s *= core::prediction_gain(w.norm(), core::kPredictStartRads, core::kPredictFullRads);
    // Space at real size: a fixed motion-to-photon time along the gyro's instantaneous angular
    // velocity (zero while the stationary lock holds), no speed ramp.
    core::Vec3 wp = w;
    if (o.fixedPredictionMs > 0.0f && !o.staticHead) {
        horizon_s = core::fixed_prediction_horizon_ms(o.fixedPredictionMs, o.presentLateMs, o.framePeriodMs, o.sampleAgeMs) * 1e-3f;
        wp = core::without_roll(from_simd(orientation), w);  // PredXY: turning and nodding
    }
    core::Quat head = core::predict_orientation(from_simd(orientation), wp, horizon_s);
    head = core::track_axes(head, static_cast<core::TrackingAxes>(o.trackingAxes));
    if (o.vehicleMode && !o.staticHead) head = core::scale_heading(head, core::kVehicleHeadingScale);
    if (o.staticHead) {  // the glasses' pinned mode moves the whole image: the panels stay put in it
        head = core::Quat{};
        horizon_s = 0.0f;
    }

    // Snapshot the panels' latest frames; each stays retained until the GPU is done.
    CVMetalTextureRef textures[kMaxPanels] = {};
    CVMetalTextureRef depths[kMaxPanels] = {};  // only the new depth maps
    float aspects[kMaxPanels];
    bool fresh[kMaxPanels] = {};
    bool depthChanged[kMaxPanels] = {}, depthCut[kMaxPanels] = {};
    float depthLow[kMaxPanels] = {}, depthHigh[kMaxPanels] = {};
    simd_float4 depthRect[kMaxPanels];
    os_unfair_lock_lock(&_sourceLock);
    const NSUInteger count = _panelCount;
    for (NSUInteger i = 0; i < count; ++i) {
        Panel& p = _panels[i];
        textures[i] = p.texture;
        if (textures[i] != nullptr) CFRetain(textures[i]);
        aspects[i] = p.aspect;
        depthRect[i] = p.depth_rect;
        fresh[i] = p.dirty;
        p.dirty = false;
        if (p.depth_dirty) {
            depthChanged[i] = true;
            depths[i] = p.depth;
            if (depths[i] != nullptr) CFRetain(depths[i]);
            depthLow[i] = p.depth_low;
            depthHigh[i] = p.depth_high;
            depthCut[i] = p.depth_cut;
            p.depth_dirty = false;
            p.depth_cut = false;
        }
    }
    os_unfair_lock_unlock(&_sourceLock);

    // One layout for all panels (they share a size); per-panel aspect only shapes its mesh.
    const auto placements = [self placements:count aspect:aspects[0]];
    for (NSUInteger i = 0; i < count; ++i) {
        // On the wall, but the cinema panel faces you wherever you looked when it opened.
        const bool cinemaPanel = o.cinema && o.focusPanel == static_cast<NSInteger>(i);
        [self rebuildMesh:_panels[i] aspect:aspects[i] placement:placements[i] wall:wall_mode(o) && !cinemaPanel];
    }

    [self updateBrightness:count head:head aspect:aspects[0] placements:placements nowNs:targetHostNs];

    id<MTLTexture> target = drawable.texture;
    // Supersampling (space at real size): the eyes drawn larger, mipmapped down in the last pass.
    const NSUInteger ss = o.staticHead ? 1 : std::clamp<NSUInteger>(o.supersample, 1, 2);
    [self ensureEyeTextures:target.pixelFormat width:target.width * ss height:target.height * ss mipmapped:ss > 1];

    id<MTLCommandBuffer> cb = [_queue commandBuffer];

    // New frames -> mipmapped copies (only panels whose content changed).
    id<MTLBlitCommandEncoder> blit = nil;
    for (NSUInteger i = 0; i < count; ++i) {
        if (!fresh[i] || textures[i] == nullptr) continue;
        id<MTLTexture> src = CVMetalTextureGetTexture(textures[i]);
        Panel& p = _panels[i];
        if (p.mipmapped == nil || p.mipmapped.width != src.width || p.mipmapped.height != src.height) {
            MTLTextureDescriptor* md = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:src.pixelFormat
                                                                                          width:src.width
                                                                                         height:src.height
                                                                                      mipmapped:YES];
            md.usage = MTLTextureUsageShaderRead;
            md.storageMode = MTLStorageModePrivate;
            p.mipmapped = [_device newTextureWithDescriptor:md];
        }
        if (blit == nil) blit = [cb blitCommandEncoder];
        [blit copyFromTexture:src sourceSlice:0 sourceLevel:0 toTexture:p.mipmapped destinationSlice:0
             destinationLevel:0 sliceCount:1 levelCount:1];
        [blit generateMipmapsForTexture:p.mipmapped];
    }
    [blit endEncoding];

    for (NSUInteger i = 0; i < count; ++i) {
        if (!depthChanged[i]) continue;
        if (depths[i] == nullptr) {  // AI 3D off
            _panels[i].smoothed_valid = false;
            continue;
        }
        [self blendDepth:_panels[i] raw:CVMetalTextureGetTexture(depths[i]) low:depthLow[i] high:depthHigh[i]
                     cut:depthCut[i] commandBuffer:cb];
    }

    const int eyes = o.sideBySide ? 2 : 1;
    const core::Mat4 proj =
        core::perspective_lh(o.fovYDegrees * kDegToRad, o.eyeAspect, 0.05f, 100.0f);

    os_unfair_lock_lock(&_sourceLock);
    id<MTLTexture> overlay = _overlay;
    os_unfair_lock_unlock(&_sourceLock);
    if (overlay != nil && (_overlayVertices == nil || _overlayMeshHeight != overlay.height)) {
        // 20° wide card at 2 m, 4° below straight ahead, as tall as its lines.
        _overlayMeshHeight = overlay.height;
        core::ScreenMesh m = core::flat_screen_mesh(2.0f, 20.0f * kDegToRad,
                                                    core::panel_width(2.0f, 20.0f * kDegToRad, true) *
                                                        static_cast<float>(overlay.height) / static_cast<float>(overlay.width), 1);
        core::pitch_mesh(m, -4.0f * kDegToRad);
        _overlayVertices = [_device newBufferWithBytes:m.vertices.data()
                                                length:m.vertices.size() * sizeof(core::ScreenVertex)
                                               options:MTLResourceStorageModeShared];
        _overlayIndices = [_device newBufferWithBytes:m.indices.data()
                                               length:m.indices.size() * sizeof(uint16_t)
                                              options:MTLResourceStorageModeShared];
        _overlayIndexCount = m.indices.size();
    }
    // Background: the eye's axes in the room (the view's rotation), the same for both eyes (at
    // infinity). Static head (glasses' pinned mode): fixed in their image.
    const NSInteger bgStyle = o.backgroundStyle;
    os_unfair_lock_lock(&_sourceLock);
    id<MTLTexture> bgImage = _backgroundImage;
    id<MTLTexture> cursorTex = _cursorTexture;
    const NSInteger cursorPanel = _cursorPanel;
    simd_float4 cursorRect = {0, 0, 0, 0};
    if (_cursorDisplay.width > 0 && _cursorDisplay.height > 0) {
        const float x0 = static_cast<float>((_cursorPos.x - _cursorHot.x) / _cursorDisplay.width);
        const float y0 = static_cast<float>((_cursorPos.y - _cursorHot.y) / _cursorDisplay.height);
        cursorRect = simd_make_float4(x0, y0, x0 + static_cast<float>(_cursorSize.width / _cursorDisplay.width),
                                      y0 + static_cast<float>(_cursorSize.height / _cursorDisplay.height));
    }
    os_unfair_lock_unlock(&_sourceLock);
    BackgroundUniforms bgu{};
    {
        const core::Mat4 r = core::render_rotation(head);
        bgu.eye_to_world = simd_matrix(simd_make_float3(r.at(0, 0), r.at(1, 0), r.at(2, 0)),
                                       simd_make_float3(r.at(0, 1), r.at(1, 1), r.at(2, 1)),
                                       simd_make_float3(r.at(0, 2), r.at(1, 2), r.at(2, 2)));
        bgu.tan_y = std::tan(o.fovYDegrees * kDegToRad * 0.5f);
        bgu.tan_x = bgu.tan_y * o.eyeAspect;
        bgu.time = static_cast<float>(std::fmod(CACurrentMediaTime(), 3600.0));
        bgu.brightness = std::clamp(o.backgroundBrightness, 0.0f, 1.0f);
        bgu.style = static_cast<int>(bgStyle);
        bgu.image_aspect = bgImage != nil ? static_cast<float>(bgImage.width) / std::max<NSUInteger>(1, bgImage.height) : 1.0f;
    }
    for (int eye = 0; eye < eyes; ++eye) {
        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = _eyeTextures[eye];
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);  // black = see-through
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:pass];
        const float offset = o.sideBySide ? (eye == 0 ? -0.5f : 0.5f) * o.ipdMeters : 0.0f;
        const float up = o.neckModel ? core::kNeckUpM : 0.0f, fwd = o.neckModel ? core::kNeckForwardM : 0.0f;
        SceneUniforms u{to_simd(proj * core::view_matrix(head, offset, up, fwd))};
        if (bgStyle > 0 && bgStyle <= 5 && (bgStyle != 5 || bgImage != nil)) {
            [enc setRenderPipelineState:_backgroundPipeline];
            [enc setFragmentBytes:&bgu length:sizeof(bgu) atIndex:0];
            [enc setFragmentTexture:bgImage != nil ? bgImage : _placeholder atIndex:0];
            [enc setFragmentSamplerState:_mirrorSampler atIndex:0];
            [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        }
        // The space at real size: bicubic panels; the glasses' pinned mode as before.
        id<MTLRenderPipelineState> panelPipeline = o.staticHead ? _screenPipeline : _sharpPipeline;
        [enc setRenderPipelineState:panelPipeline];
        [enc setCullMode:MTLCullModeNone];
        [enc setVertexBytes:&u length:sizeof(u) atIndex:1];
        [enc setFragmentSamplerState:_sampler atIndex:0];
        for (NSUInteger i = 0; i < count; ++i) {
            if (o.focusPanel >= 0 && static_cast<NSInteger>(i) != o.focusPanel) continue;  // focus / cinema
            const Panel& p = _panels[i];
            [enc setVertexBuffer:p.vertices offset:0 atIndex:0];
            [enc setFragmentTexture:_panels[i].mipmapped != nil ? _panels[i].mipmapped : _placeholder
                            atIndex:0];
            // AI 3D: only with one image per eye (in mono there is no depth to show).
            const bool deep = o.sideBySide && p.smoothed_valid && p.mipmapped != nil;
            if (deep) {
                const StereoUniforms su{p.brightness, eye == 0 ? -1.0f : 1.0f, o.depthStrength,
                                        std::clamp(o.depthConvergence, 0.0f, 1.0f), kDepthEdge, {0, 0, 0}, depthRect[i]};
                [enc setRenderPipelineState:_stereoPipeline];
                [enc setFragmentBytes:&su length:sizeof(su) atIndex:0];
                [enc setFragmentTexture:p.smoothed[p.smoothed_index] atIndex:1];
                [enc setFragmentSamplerState:_depthSampler atIndex:1];
            } else {
                const simd_float2 pu = {p.brightness, static_cast<NSInteger>(i) == o.keyPanel ? 1.0f : 0.0f};
                [enc setFragmentBytes:&pu length:sizeof(pu) atIndex:0];
            }
            [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                            indexCount:p.index_count
                             indexType:MTLIndexTypeUInt16
                           indexBuffer:p.indices
                     indexBufferOffset:0];
            if (deep) [enc setRenderPipelineState:panelPipeline];
            if (cursorTex != nil && static_cast<NSInteger>(i) == cursorPanel) {
                [enc setRenderPipelineState:_cursorPipeline];
                [enc setFragmentTexture:cursorTex atIndex:0];
                [enc setFragmentBytes:&cursorRect length:sizeof(cursorRect) atIndex:0];
                [enc setFragmentBytes:&p.brightness length:sizeof(float) atIndex:1];
                [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                                indexCount:p.index_count
                                 indexType:MTLIndexTypeUInt16
                               indexBuffer:p.indices
                         indexBufferOffset:0];
                [enc setRenderPipelineState:panelPipeline];
            }
        }
        if (overlay != nil) {
            [enc setRenderPipelineState:_screenPipeline];
            // Head-locked: straight ahead whatever the pose, slightly below the centre.
            SceneUniforms ou{to_simd(proj * core::view_matrix(core::Quat{}, offset))};
            const simd_float2 one = {1.0f, 0.0f};  // full brightness, no frame
            [enc setVertexBytes:&ou length:sizeof(ou) atIndex:1];
            [enc setVertexBuffer:_overlayVertices offset:0 atIndex:0];
            [enc setFragmentBytes:&one length:sizeof(one) atIndex:0];
            [enc setFragmentTexture:overlay atIndex:0];
            [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                            indexCount:_overlayIndexCount
                             indexType:MTLIndexTypeUInt16
                           indexBuffer:_overlayIndices
                     indexBufferOffset:0];
        }
        [enc endEncoding];
    }

    // Supersampled 2x without lens distortion: each output pixel's centre falls between four
    // eye texels, so one bilinear tap of level 0 is their 2x2 average, exactly what mip level 1
    // holds: no mip chain to build every frame. With distortion the chain is still needed.
    const bool box = ss == 2 && o.k1 == 0.0f && o.k2 == 0.0f;
    if (ss > 1 && !box) {  // supersampled: the mip chain the last pass reads at the glasses' size
        id<MTLBlitCommandEncoder> mips = [cb blitCommandEncoder];
        for (int eye = 0; eye < eyes; ++eye) [mips generateMipmapsForTexture:_eyeTextures[eye]];
        [mips endEncoding];
    }
    MTLRenderPassDescriptor* out = [MTLRenderPassDescriptor renderPassDescriptor];
    out.colorAttachments[0].texture = target;
    out.colorAttachments[0].loadAction = MTLLoadActionClear;
    out.colorAttachments[0].storeAction = MTLStoreActionStore;
    out.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:out];
    [enc setRenderPipelineState:_distortPipeline];
    const DistortUniforms du{o.k1, o.k2, o.eyeAspect, box ? 1.0f : 0.0f};
    const double half = static_cast<double>(target.width) / eyes;
    for (int eye = 0; eye < eyes; ++eye) {
        const MTLViewport vp{half * eye, 0.0, half, static_cast<double>(target.height), 0.0, 1.0};
        [enc setViewport:vp];
        [enc setFragmentTexture:_eyeTextures[eye] atIndex:0];
        [enc setFragmentSamplerState:_sampler atIndex:0];
        [enc setFragmentBytes:&du length:sizeof(du) atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    }
    [enc endEncoding];

    [cb presentDrawable:drawable];
    std::vector<CVMetalTextureRef> held;
    for (NSUInteger i = 0; i < count; ++i) {
        if (textures[i] != nullptr) held.push_back(textures[i]);
        if (depths[i] != nullptr) held.push_back(depths[i]);
    }
    if (!held.empty()) {
        [cb addCompletedHandler:^(id<MTLCommandBuffer>) {
          for (CVMetalTextureRef t : held) CFRelease(t);  // IOSurfaces live until the GPU is done
        }];
    }
    __weak RNSpatialRenderer* weakSelf = self;
    [cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
      RNSpatialRenderer* strong = weakSelf;
      if (strong != nil && done.GPUEndTime > done.GPUStartTime) {
          const double ms = (done.GPUEndTime - done.GPUStartTime) * 1e3;
          strong->_lastGPUMs = ms;
          os_unfair_lock_lock(&strong->_gpuLock);
          strong->_gpuSumMs += ms;
          strong->_gpuN += 1;
          os_unfair_lock_unlock(&strong->_gpuLock);
      }
      if (gpuDone != nil && done.GPUEndTime > 0) gpuDone(done.GPUStartTime, done.GPUEndTime);
    }];
    [cb commit];
    return horizon_s * 1e3f;
}

@end
