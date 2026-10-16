// Spatial screen renderer. Compiled at runtime (newLibraryWithSource) so the build only
// needs the Command Line Tools; CMake embeds this file into rn_shaders.h.
#include <metal_stdlib>
using namespace metal;

// Mirrors rayneo::core::ScreenVertex (5 tightly packed floats).
struct ScreenVertex {
    packed_float3 position;
    packed_float2 uv;
};

struct SceneUniforms {
    float4x4 view_projection;
};

struct VOut {
    float4 position [[position]];
    float2 uv;
};

// Pass 1: curved virtual screen into the per-eye texture.
vertex VOut screen_vertex(uint vid [[vertex_id]],
                          const device ScreenVertex* vertices [[buffer(0)]],
                          constant SceneUniforms& u [[buffer(1)]]) {
    VOut out;
    out.position = u.view_projection * float4(float3(vertices[vid].position), 1.0);
    out.uv = float2(vertices[vid].uv);
    return out;
}

// Textures are sRGB views: sampling, mipmaps and this brightness work on linear light.
fragment float4 screen_fragment(VOut in [[stage_in]],
                                texture2d<float> source [[texture(0)]],
                                sampler s [[sampler(0)]],
                                constant float& brightness [[buffer(0)]]) {
    // A slight bias towards the full-size level: a panel drawn a little smaller than its capture
    // blended in the half-size mipmap and softened the text.
    return float4(source.sample(s, in.uv, bias(-0.5)).rgb * brightness, 1.0);
}

// Pass 2: per-eye barrel pre-distortion into the drawable viewport (half for SBS).
struct DistortUniforms {
    float k1;
    float k2;
    float aspect;  // eye image width / height as perceived (16:9 even when squeezed SBS)
};

vertex VOut fullscreen_vertex(uint vid [[vertex_id]]) {
    const float2 p = float2((vid << 1) & 2, vid & 2);  // (0,0) (2,0) (0,2)
    VOut out;
    out.position = float4(p * 2.0 - 1.0, 0.0, 1.0);
    out.uv = float2(p.x, 1.0 - p.y);
    return out;
}

// Same formula as rayneo::core::barrel_distort (normalised: centre 0, half-height 1).
fragment float4 distort_fragment(VOut in [[stage_in]],
                                 texture2d<float> eye [[texture(0)]],
                                 sampler s [[sampler(0)]],
                                 constant DistortUniforms& d [[buffer(0)]]) {
    float2 c = in.uv * 2.0 - 1.0;
    c.x *= d.aspect;
    const float r2 = dot(c, c);
    float2 p = c * (1.0 + d.k1 * r2 + d.k2 * r2 * r2);
    p.x /= d.aspect;
    const float2 uv = p * 0.5 + 0.5;
    if (any(uv < 0.0) || any(uv > 1.0)) return float4(0.0, 0.0, 0.0, 1.0);
    return float4(eye.sample(s, uv).rgb, 1.0);
}
