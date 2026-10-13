#import "RNSpatialRenderer.h"

#include <os/lock.h>

#include <algorithm>
#include <cstring>

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

}  // namespace

@implementation RNRenderOptions
- (instancetype)init {
    if ((self = [super init])) {
        _sideBySide = YES;
        _ipdMeters = 0.063f;
        _distanceMeters = 2.0f;
        _arcDegrees = 50.0f;
        _fovYDegrees = 24.5f;
        _eyeAspect = 16.0f / 9.0f;
        _k1 = 0.0f;
        _k2 = 0.0f;
        _maxPredictionMs = 50.0f;
    }
    return self;
}
@end

@implementation RNSpatialRenderer {
    id<MTLCommandQueue> _queue;
    id<MTLRenderPipelineState> _screenPipeline;
    id<MTLRenderPipelineState> _distortPipeline;
    id<MTLSamplerState> _sampler;
    CVMetalTextureCacheRef _textureCache;

    // Latest capture, guarded by _sourceLock.
    os_unfair_lock _sourceLock;
    CVMetalTextureRef _sourceTexture;
    float _sourceAspect;

    // Mesh, rebuilt when the screen geometry changes.
    id<MTLBuffer> _vertices;
    id<MTLBuffer> _indices;
    NSUInteger _indexCount;
    float _meshAspect, _meshDistance, _meshArc;

    id<MTLTexture> _eyeTextures[2];
}

- (nullable instancetype)initWithDevice:(id<MTLDevice>)device
                            pixelFormat:(MTLPixelFormat)pixelFormat
                                  error:(NSError**)error {
    if (!(self = [super init])) return nil;
    _device = device;
    _options = [RNRenderOptions new];
    _queue = [device newCommandQueue];
    _sourceLock = OS_UNFAIR_LOCK_INIT;
    _sourceAspect = 16.0f / 9.0f;

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
    s.maxAnisotropy = 8;
    _sampler = [device newSamplerStateWithDescriptor:s];

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
    if (_sourceTexture != nullptr) CFRelease(_sourceTexture);
    if (_textureCache != nullptr) CFRelease(_textureCache);
}

- (void)setSourcePixelBuffer:(CVPixelBufferRef)pb {
    const size_t w = CVPixelBufferGetWidth(pb);
    const size_t h = CVPixelBufferGetHeight(pb);
    CVMetalTextureRef tex = nullptr;
    if (CVMetalTextureCacheCreateTextureFromImage(kCFAllocatorDefault, _textureCache, pb, nil,
                                                  MTLPixelFormatBGRA8Unorm, w, h, 0, &tex) != kCVReturnSuccess) {
        return;
    }
    os_unfair_lock_lock(&_sourceLock);
    CVMetalTextureRef old = _sourceTexture;
    _sourceTexture = tex;
    _sourceAspect = h > 0 ? static_cast<float>(w) / static_cast<float>(h) : 16.0f / 9.0f;
    os_unfair_lock_unlock(&_sourceLock);
    if (old != nullptr) CFRelease(old);
}

- (void)rebuildMeshIfNeeded:(float)aspect {
    RNRenderOptions* o = _options;
    if (_vertices != nil && aspect == _meshAspect && o.distanceMeters == _meshDistance &&
        o.arcDegrees == _meshArc) {
        return;
    }
    const float arc = o.arcDegrees * kDegToRad;
    const float width = o.distanceMeters * arc;  // arc length
    const core::ScreenMesh mesh = core::curved_screen_mesh(o.distanceMeters, arc, width / aspect, 64);
    _vertices = [_device newBufferWithBytes:mesh.vertices.data()
                                     length:mesh.vertices.size() * sizeof(core::ScreenVertex)
                                    options:MTLResourceStorageModeShared];
    _indices = [_device newBufferWithBytes:mesh.indices.data()
                                    length:mesh.indices.size() * sizeof(uint16_t)
                                   options:MTLResourceStorageModeShared];
    _indexCount = mesh.indices.size();
    _meshAspect = aspect;
    _meshDistance = o.distanceMeters;
    _meshArc = o.arcDegrees;
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
    const core::Quat head = core::predict_orientation(
        from_simd(orientation), core::Vec3{angularVelocity.x, angularVelocity.y, angularVelocity.z}, horizon_s);

    os_unfair_lock_lock(&_sourceLock);
    CVMetalTextureRef src = _sourceTexture;
    if (src != nullptr) CFRetain(src);
    const float aspect = _sourceAspect;
    os_unfair_lock_unlock(&_sourceLock);

    id<MTLTexture> target = drawable.texture;
    [self ensureEyeTextures:target.pixelFormat width:target.width height:target.height];
    [self rebuildMeshIfNeeded:aspect];

    id<MTLCommandBuffer> cb = [_queue commandBuffer];
    const int eyes = o.sideBySide ? 2 : 1;
    const core::Mat4 proj =
        core::perspective_lh(o.fovYDegrees * kDegToRad, o.eyeAspect, 0.05f, 100.0f);

    for (int eye = 0; eye < eyes; ++eye) {
        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = _eyeTextures[eye];
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);  // black = see-through
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:pass];
        if (src != nullptr) {
            const float offset = o.sideBySide ? (eye == 0 ? -0.5f : 0.5f) * o.ipdMeters : 0.0f;
            SceneUniforms u{to_simd(proj * core::view_matrix(head, offset))};
            [enc setRenderPipelineState:_screenPipeline];
            [enc setCullMode:MTLCullModeNone];
            [enc setVertexBuffer:_vertices offset:0 atIndex:0];
            [enc setVertexBytes:&u length:sizeof(u) atIndex:1];
            [enc setFragmentTexture:CVMetalTextureGetTexture(src) atIndex:0];
            [enc setFragmentSamplerState:_sampler atIndex:0];
            [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                            indexCount:_indexCount
                             indexType:MTLIndexTypeUInt16
                           indexBuffer:_indices
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
    if (src != nullptr) {
        [cb addCompletedHandler:^(id<MTLCommandBuffer>) {
          CFRelease(src);  // keep the IOSurface alive until the GPU is done with it
        }];
    }
    [cb commit];
    return horizon_s * 1e3f;
}

@end
