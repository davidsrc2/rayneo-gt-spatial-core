#import "RNSpatialRenderer.h"

#include <os/lock.h>
#import <CoreText/CoreText.h>

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
};

simd_float4x4 to_simd(const core::Mat4& m) {
    simd_float4x4 out;
    static_assert(sizeof(out) == sizeof(m.m), "column-major 4x4 floats");
    std::memcpy(&out, m.m, sizeof(m.m));
    return out;
}

core::Quat from_simd(simd_quatf q) { return core::Quat{q.vector.w, q.vector.x, q.vector.y, q.vector.z}; }

constexpr NSUInteger kMaxPanels = 8;
constexpr float kCinemaScale = 1.6f;  // cinema mode: the panel this much larger than a monitor
constexpr float kReferenceArcDeg = 38.0f;  // glasses' pinned mode: this panel size at 2 m fits exactly
constexpr float kGrowShrink = 0.3f;        // glasses' pinned mode: the panels around the enlarged one
constexpr double kGrowSeconds = 0.35;      // and the transition when another one is enlarged

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
    float brightness = 1.0f;  // render thread: eased towards 1 (looked at) or dimOthers
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
        _fovYDegrees = 24.5f;
        _eyeAspect = 16.0f / 9.0f;
        _k1 = 0.0f;
        _k2 = 0.0f;
        _maxPredictionMs = 50.0f;
        _dimOthers = 1.0f;
        _trackingAxes = 0;
        _staticHead = NO;
        _layoutArcDegrees = 0.0f;
        _focusPanel = -1;
        _cinema = NO;
    }
    return self;
}
@end

// The panel arc as seen at the current distance (Distancia acerca y aleja de verdad).
static float panel_arc(RNRenderOptions* o) {
    return core::arc_at_distance(o.arcDegrees * kDegToRad, o.distanceMeters);
}

@implementation RNSpatialRenderer {
    id<MTLCommandQueue> _queue;
    id<MTLRenderPipelineState> _screenPipeline;
    id<MTLRenderPipelineState> _distortPipeline;
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
    id<MTLBuffer> _overlayIndices;
    NSUInteger _overlayIndexCount;
    id<MTLTexture> _eyeTextures[2];

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

    d.vertexFunction = [lib newFunctionWithName:@"fullscreen_vertex"];
    d.fragmentFunction = [lib newFunctionWithName:@"distort_fragment"];
    _distortPipeline = [device newRenderPipelineStateWithDescriptor:d error:error];
    if (_distortPipeline == nil) return nil;

    MTLSamplerDescriptor* s = [MTLSamplerDescriptor new];
    s.minFilter = MTLSamplerMinMagFilterLinear;
    s.magFilter = MTLSamplerMinMagFilterLinear;
    s.sAddressMode = MTLSamplerAddressModeClampToZero;
    s.tAddressMode = MTLSamplerAddressModeClampToZero;
    s.mipFilter = MTLSamplerMipFilterLinear;  // trilinear + anisotropic: crisp, stable text
    s.maxAnisotropy = 16;
    _sampler = [device newSamplerStateWithDescriptor:s];

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

- (void)setOverlayText:(NSString*)text {
    id<MTLTexture> tex = nil;
    if (text.length > 0) {
        // Dark rounded card with white text (black would be see-through on the glasses).
        const size_t w = 1400, h = 360;
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
        NSArray<NSString*>* lines = [text componentsSeparatedByString:@"\n"];
        for (NSUInteger i = 0; i < lines.count && i < 2; ++i) {
            const CGFloat size = i == 0 ? 76 : 50;
            CTFontRef font = CTFontCreateUIFontForLanguage(i == 0 ? kCTFontUIFontEmphasizedSystem : kCTFontUIFontSystem, size, nullptr);
            CGColorRef color = CGColorCreateSRGB(1, 1, 1, i == 0 ? 1.0 : 0.75);
            NSDictionary* attrs = @{(id)kCTFontAttributeName : (__bridge id)font, (id)kCTForegroundColorAttributeName : (__bridge id)color};
            CTLineRef line = CTLineCreateWithAttributedString(
                (__bridge CFAttributedStringRef)[[NSAttributedString alloc] initWithString:lines[i] attributes:attrs]);
            const CGRect bounds = CTLineGetBoundsWithOptions(line, 0);
            CGContextSetTextPosition(ctx, (w - bounds.size.width) / 2, i == 0 ? 200 : 90);
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
    // Cinema: the focused panel straight ahead, large (the others are not drawn).
    if (o.cinema && o.focusPanel >= 0 && static_cast<NSUInteger>(o.focusPanel) < out.size()) {
        out[static_cast<size_t>(o.focusPanel)] = core::PanelPlacement{0.0f, 0.0f, kCinemaScale};
    }
    if (o.staticHead && o.focusPanel < 0) [self growInGlassesImage:out];
    if (o.staticHead) [self fitInGlassesImage:out];
    return out;
}

// Glasses in their pinned mode: the panel you look at enlarged, the others small around it
// (fitted afterwards), easing from the previous one.
- (void)growInGlassesImage:(std::vector<core::PanelPlacement>&)out {
    float aspects[kMaxPanels];
    os_unfair_lock_lock(&_sourceLock);
    for (NSUInteger i = 0; i < kMaxPanels; ++i) aspects[i] = _panels[i].aspect;
    const NSInteger to = _growPanel, from = _growFrom;
    const double t = _growStartNs == 0 ? 1.0 : std::min(1.0, static_cast<double>(now_ns() - _growStartNs) * 1e-9 / kGrowSeconds);
    os_unfair_lock_unlock(&_sourceLock);
    if (to < 0 && (from < 0 || t >= 1.0)) return;
    const float arc = panel_arc(_options);
    std::vector<float> w, h;
    for (size_t i = 0; i < out.size(); ++i) {
        w.push_back(arc * out[i].scale);
        h.push_back(core::panel_vertical_arc(w.back(), aspects[std::min<size_t>(i, kMaxPanels - 1)], true));
    }
    auto target = out;
    core::grow_panel(target, w, h, static_cast<int>(to), kGrowShrink);
    if (t >= 1.0) {
        out = target;
        return;
    }
    auto start = out;
    core::grow_panel(start, w, h, static_cast<int>(from), kGrowShrink);
    const float k = static_cast<float>(t * t * (3.0 - 2.0 * t));  // smoothstep
    for (size_t i = 0; i < out.size(); ++i) {
        out[i].yaw_rad = start[i].yaw_rad + (target[i].yaw_rad - start[i].yaw_rad) * k;
        out[i].pitch_rad = start[i].pitch_rad + (target[i].pitch_rad - start[i].pitch_rad) * k;
        out[i].scale = start[i].scale + (target[i].scale - start[i].scale) * k;
    }
}

// Glasses in their own pinned mode anchor the image we send and let the eyes roam over it:
// nothing outside it can ever be seen. The whole arrangement (monitors and floating windows,
// both axes; only the focused panel in focus / cinema) is centred and scaled to fit in it.
// Distance and size still zoom out (farther or smaller shrinks it), but never past the
// image: closer or larger than a 38° panel at 2 m stays fitted, or the edges would be cut.
- (void)fitInGlassesImage:(std::vector<core::PanelPlacement>&)out {
    RNRenderOptions* o = _options;
    float aspects[kMaxPanels];
    os_unfair_lock_lock(&_sourceLock);
    for (NSUInteger i = 0; i < kMaxPanels; ++i) aspects[i] = _panels[i].aspect;
    os_unfair_lock_unlock(&_sourceLock);
    const float arc = panel_arc(o);
    float y0 = 1e9f, y1 = -1e9f, p0 = 1e9f, p1 = -1e9f;
    for (size_t i = 0; i < out.size() && i < kMaxPanels; ++i) {
        if (o.focusPanel >= 0 && static_cast<NSInteger>(i) != o.focusPanel) continue;
        const float w = arc * out[i].scale, h = core::panel_vertical_arc(w, aspects[i], true);
        y0 = std::min(y0, out[i].yaw_rad - w / 2);
        y1 = std::max(y1, out[i].yaw_rad + w / 2);
        p0 = std::min(p0, out[i].pitch_rad - h / 2);
        p1 = std::max(p1, out[i].pitch_rad + h / 2);
    }
    if (y1 <= y0 || p1 <= p0) return;
    const float fw = core::horizontal_fov(o.fovYDegrees * kDegToRad, o.eyeAspect), fh = o.fovYDegrees * kDegToRad;
    const float zoom = std::min(1.0f, arc / (kReferenceArcDeg * kDegToRad));
    const float s = 0.94f * std::min(fw * zoom / (y1 - y0), fh * zoom / (p1 - p0));
    const float yc = 0.5f * (y0 + y1), pc = 0.5f * (p0 + p1);
    for (auto& pl : out) {
        pl.yaw_rad = (pl.yaw_rad - yc) * s;
        pl.pitch_rad = (pl.pitch_rad - pc) * s;
        pl.scale *= s;
    }
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
    return core::grid_layout(cells, arc, core::panel_vertical_arc(arc, aspect, true), o.gapDegrees * kDegToRad);
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

- (NSInteger)panelAtGaze:(simd_quatf)orientation {
    RNRenderOptions* o = _options;
    if (o.focusPanel >= 0 && static_cast<NSUInteger>(o.focusPanel) < self.panelCount) return o.focusPanel;
    os_unfair_lock_lock(&_sourceLock);
    const float aspect = _panels[0].aspect;
    os_unfair_lock_unlock(&_sourceLock);
    const float arc = panel_arc(o);
    const core::Quat head = core::track_axes(from_simd(orientation), static_cast<core::TrackingAxes>(o.trackingAxes));
    return core::panel_at_gaze(head, [self placements:self.panelCount aspect:aspect], arc,
                               core::panel_vertical_arc(arc, aspect, true), o.gapDegrees * kDegToRad);
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

static float effective_curvature(RNRenderOptions* o) {
    return o.curved && o.curvature <= 0.0f ? 1.0f : std::clamp(o.curvature, 0.0f, 1.0f);
}

- (void)rebuildMesh:(Panel&)p aspect:(float)aspect placement:(core::PanelPlacement)pl {
    RNRenderOptions* o = _options;
    const float center = pl.yaw_rad;
    const float k = effective_curvature(o);
    if (p.vertices != nil && aspect == p.mesh_aspect && o.distanceMeters == p.mesh_distance &&
        o.arcDegrees == p.mesh_arc && center == p.mesh_center && pl.pitch_rad == p.mesh_pitch &&
        k == p.mesh_curvature && pl.scale == p.mesh_scale) {
        return;
    }
    const float arc = panel_arc(o) * pl.scale;
    const float width = core::panel_width(o.distanceMeters, arc, true);  // a sheet: bending keeps its width
    core::ScreenMesh mesh = core::bent_screen_mesh(o.distanceMeters, arc, width / aspect, k > 0.0f ? 64 : 1, k, center);
    core::pitch_mesh(mesh, pl.pitch_rad);
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
}

// Panels you are not looking at fade to dimOthers (core::PanelDimmer, shared with Windows).
- (void)updateBrightness:(NSUInteger)count
                    head:(const core::Quat&)head
                  aspect:(float)aspect
              placements:(const std::vector<core::PanelPlacement>&)placements
                   nowNs:(uint64_t)nowNs {
    RNRenderOptions* o = _options;
    int gaze = -1;
    if (count > 1 && o.dimOthers < 1.0f) {
        const float arc = panel_arc(o);
        gaze = core::panel_at_gaze(head, placements, arc, core::panel_vertical_arc(arc, aspect, true),
                                   o.gapDegrees * kDegToRad);
    }
    const float dt = _lastFrameNs > 0 && nowNs > _lastFrameNs ? std::min(0.1f, (nowNs - _lastFrameNs) * 1e-9f) : 0.0f;
    _lastFrameNs = nowNs;
    _dimmer.update(count, gaze, o.dimOthers, dt);
    for (NSUInteger i = 0; i < count; ++i) _panels[i].brightness = _dimmer.brightness(i);
}

- (BOOL)canSkipFrameWithOrientation:(simd_quatf)orientation
                    angularVelocity:(simd_float3)angularVelocity
                           tracking:(BOOL)tracking
                             forced:(BOOL)forced {
    os_unfair_lock_lock(&_sourceLock);
    const uint64_t generation = _sourceGeneration;
    os_unfair_lock_unlock(&_sourceLock);
    core::FrameSkipper::Input in;
    in.orientation = _options.staticHead ? core::Quat{} : from_simd(orientation);
    in.angular_speed_rads = _options.staticHead ? 0.0f : simd_length(angularVelocity);
    in.source_generation = generation;
    in.tracking = tracking;
    in.animating = _dimmer.animating() || [self growAnimating];
    in.forced = forced;
    return _skipper.skip(in);
}

- (void)invalidateSkippedFrame {
    _skipper.invalidate();
}

- (void)ensureEyeTextures:(MTLPixelFormat)format width:(NSUInteger)w height:(NSUInteger)h {
    if (_eyeTextures[0] != nil && _eyeTextures[0].width == w && _eyeTextures[0].height == h &&
        _eyeTextures[0].pixelFormat == format) {
        return;
    }
    MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                                                 width:w
                                                                                height:h
                                                                             mipmapped:NO];
    d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    d.storageMode = MTLStorageModePrivate;
    for (auto& t : _eyeTextures) t = [_device newTextureWithDescriptor:d];
}

- (float)renderToDrawable:(id<CAMetalDrawable>)drawable
              orientation:(simd_quatf)orientation
          angularVelocity:(simd_float3)angularVelocity
             sampleHostNs:(uint64_t)sampleHostNs
             targetHostNs:(uint64_t)targetHostNs {
    RNRenderOptions* o = _options;

    // Pose prediction to the photon time.
    float horizon_s = 0.0f;
    if (sampleHostNs > 0 && targetHostNs > sampleHostNs) {
        horizon_s = std::min(static_cast<float>(targetHostNs - sampleHostNs) * 1e-9f, o.maxPredictionMs * 1e-3f);
    }
    // Extrapolating while the head holds still would only amplify noise: ramp the prediction
    // in with angular speed.
    const core::Vec3 w{angularVelocity.x, angularVelocity.y, angularVelocity.z};
    horizon_s *= core::prediction_gain(w.norm());
    core::Quat head = core::predict_orientation(from_simd(orientation), w, horizon_s);
    head = core::track_axes(head, static_cast<core::TrackingAxes>(o.trackingAxes));
    if (o.staticHead) {  // the glasses' pinned mode moves the whole image: the panels stay put in it
        head = core::Quat{};
        horizon_s = 0.0f;
    }

    // Snapshot the panels' latest frames; each stays retained until the GPU is done.
    CVMetalTextureRef textures[kMaxPanels] = {};
    float aspects[kMaxPanels];
    bool fresh[kMaxPanels] = {};
    os_unfair_lock_lock(&_sourceLock);
    const NSUInteger count = _panelCount;
    for (NSUInteger i = 0; i < count; ++i) {
        textures[i] = _panels[i].texture;
        if (textures[i] != nullptr) CFRetain(textures[i]);
        aspects[i] = _panels[i].aspect;
        fresh[i] = _panels[i].dirty;
        _panels[i].dirty = false;
    }
    os_unfair_lock_unlock(&_sourceLock);

    // One layout for all panels (they share a size); per-panel aspect only shapes its mesh.
    const auto placements = [self placements:count aspect:aspects[0]];
    for (NSUInteger i = 0; i < count; ++i) [self rebuildMesh:_panels[i] aspect:aspects[i] placement:placements[i]];

    [self updateBrightness:count head:head aspect:aspects[0] placements:placements nowNs:targetHostNs];

    id<MTLTexture> target = drawable.texture;
    [self ensureEyeTextures:target.pixelFormat width:target.width height:target.height];

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

    const int eyes = o.sideBySide ? 2 : 1;
    const core::Mat4 proj =
        core::perspective_lh(o.fovYDegrees * kDegToRad, o.eyeAspect, 0.05f, 100.0f);

    os_unfair_lock_lock(&_sourceLock);
    id<MTLTexture> overlay = _overlay;
    os_unfair_lock_unlock(&_sourceLock);
    if (overlay != nil && _overlayVertices == nil) {
        // 20° wide card at 2 m, 4° below straight ahead.
        core::ScreenMesh m = core::flat_screen_mesh(2.0f, 20.0f * kDegToRad,
                                                    core::panel_width(2.0f, 20.0f * kDegToRad, true) * 360.0f / 1400.0f, 1);
        core::pitch_mesh(m, -4.0f * kDegToRad);
        _overlayVertices = [_device newBufferWithBytes:m.vertices.data()
                                                length:m.vertices.size() * sizeof(core::ScreenVertex)
                                               options:MTLResourceStorageModeShared];
        _overlayIndices = [_device newBufferWithBytes:m.indices.data()
                                               length:m.indices.size() * sizeof(uint16_t)
                                              options:MTLResourceStorageModeShared];
        _overlayIndexCount = m.indices.size();
    }
    for (int eye = 0; eye < eyes; ++eye) {
        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = _eyeTextures[eye];
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);  // black = see-through
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:pass];
        const float offset = o.sideBySide ? (eye == 0 ? -0.5f : 0.5f) * o.ipdMeters : 0.0f;
        SceneUniforms u{to_simd(proj * core::view_matrix(head, offset))};
        [enc setRenderPipelineState:_screenPipeline];
        [enc setCullMode:MTLCullModeNone];
        [enc setVertexBytes:&u length:sizeof(u) atIndex:1];
        [enc setFragmentSamplerState:_sampler atIndex:0];
        for (NSUInteger i = 0; i < count; ++i) {
            if (o.focusPanel >= 0 && static_cast<NSInteger>(i) != o.focusPanel) continue;  // focus / cinema
            const Panel& p = _panels[i];
            [enc setVertexBuffer:p.vertices offset:0 atIndex:0];
            [enc setFragmentBytes:&p.brightness length:sizeof(float) atIndex:0];
            [enc setFragmentTexture:_panels[i].mipmapped != nil ? _panels[i].mipmapped : _placeholder
                            atIndex:0];
            [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                            indexCount:p.index_count
                             indexType:MTLIndexTypeUInt16
                           indexBuffer:p.indices
                     indexBufferOffset:0];
        }
        if (overlay != nil) {
            // Head-locked: straight ahead whatever the pose, slightly below the centre.
            SceneUniforms ou{to_simd(proj * core::view_matrix(core::Quat{}, offset))};
            const float one = 1.0f;
            [enc setVertexBytes:&ou length:sizeof(ou) atIndex:1];
            [enc setVertexBuffer:_overlayVertices offset:0 atIndex:0];
            [enc setFragmentBytes:&one length:sizeof(float) atIndex:0];
            [enc setFragmentTexture:overlay atIndex:0];
            [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                            indexCount:_overlayIndexCount
                             indexType:MTLIndexTypeUInt16
                           indexBuffer:_overlayIndices
                     indexBufferOffset:0];
        }
        [enc endEncoding];
    }

    MTLRenderPassDescriptor* out = [MTLRenderPassDescriptor renderPassDescriptor];
    out.colorAttachments[0].texture = target;
    out.colorAttachments[0].loadAction = MTLLoadActionClear;
    out.colorAttachments[0].storeAction = MTLStoreActionStore;
    out.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:out];
    [enc setRenderPipelineState:_distortPipeline];
    const DistortUniforms du{o.k1, o.k2, o.eyeAspect};
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
    }
    if (!held.empty()) {
        [cb addCompletedHandler:^(id<MTLCommandBuffer>) {
          for (CVMetalTextureRef t : held) CFRelease(t);  // IOSurfaces live until the GPU is done
        }];
    }
    [cb commit];
    return horizon_s * 1e3f;
}

@end
