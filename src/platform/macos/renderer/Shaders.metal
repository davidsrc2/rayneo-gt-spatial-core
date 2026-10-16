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

// AI 3D (cinema): a 2D video seen in depth. Each eye samples the frame shifted by a parallax
// taken from its depth map (0 far .. 1 near), so near things stand out and the rest recedes.
struct StereoUniforms {
    float brightness;
    float eye;          // -1 left, +1 right
    float strength;     // parallax between the eyes at depth 1, in panel widths
    float convergence;  // depth seen on the panel's plane (no parallax)
    float edge;         // width of the side margins where the parallax fades to 0, in panel widths
};

float stereo_shift(texture2d<float> depth, sampler s, float x, float y, constant StereoUniforms& u) {
    const float d = depth.sample(s, float2(x, y), level(0)).r;
    // At the sides the image goes back to the panel's plane: something in front of it cut by the
    // frame would contradict the frame being in front of it (window violation).
    const float fade = smoothstep(0.0, u.edge, x) * smoothstep(0.0, u.edge, 1.0 - x);
    return -u.eye * 0.5 * u.strength * (d - u.convergence) * fade;
}

// Backward mapping: for this output pixel, the source pixel whose own shift lands here. The
// candidate shifts are walked from the nearest to the farthest, so where several sources land
// on the same pixel the nearest wins (occlusion), and every output pixel finds one (no holes).
fragment float4 stereo_screen_fragment(VOut in [[stage_in]],
                                       texture2d<float> source [[texture(0)]],
                                       texture2d<float> depth [[texture(1)]],
                                       sampler s [[sampler(0)]],
                                       constant StereoUniforms& u [[buffer(0)]]) {
    constexpr int kSteps = 8;
    const float k = -u.eye;  // the direction near things move in this eye
    const float near_t = 0.5 * u.strength * (1.0 - u.convergence);
    const float far_t = 0.5 * u.strength * (0.0 - u.convergence);
    float prev_t = near_t, prev_g = -1.0;
    float t = far_t;
    for (int i = 0; i <= kSteps; ++i) {
        const float ti = mix(near_t, far_t, float(i) / float(kSteps));  // signed along k
        const float a = k * stereo_shift(depth, s, in.uv.x - k * ti, in.uv.y, u);
        const float g = a - ti;
        if (g >= 0.0) {
            // Between the last step short of a surface and this one: where the shift crosses.
            t = i == 0 ? ti : ti + (g / (g - prev_g)) * (prev_t - ti);
            break;
        }
        prev_t = ti;
        prev_g = g;
    }
    const float2 uv = float2(in.uv.x - k * t, in.uv.y);
    // Gradients of the unshifted coordinates: the shift jumps at depth edges and would pick a
    // blurry mipmap level there.
    const float3 c = source.sample(s, uv, gradient2d(dfdx(in.uv), dfdy(in.uv))).rgb;
    return float4(c * u.brightness, 1.0);
}

// Temporal smoothing of the depth maps (anti-flicker): the new map, normalised to its 2-98 %
// range, blended into the previous one. alpha 1 at a scene cut starts afresh.
struct DepthBlendUniforms {
    float low;
    float high;
    float alpha;
};

fragment float4 depth_blend_fragment(VOut in [[stage_in]],
                                     texture2d<float> raw [[texture(0)]],
                                     texture2d<float> previous [[texture(1)]],
                                     sampler s [[sampler(0)]],
                                     constant DepthBlendUniforms& u [[buffer(0)]]) {
    const float d = saturate((raw.sample(s, in.uv, level(0)).r - u.low) / max(u.high - u.low, 1e-4));
    const float p = previous.sample(s, in.uv, level(0)).r;
    return float4(mix(p, d, u.alpha), 0.0, 0.0, 1.0);
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
