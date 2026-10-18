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

// The space at real size: bicubic Catmull-Rom (4x4 texels, 9 bilinear fetches) on the mipmap
// level the pixel needs (same -0.5 bias as above): text stays crisp and does not shimmer on a
// panel held at an angle. Negative lobes clamped.
// The cursor over a panel at the glasses' rate, on the panel's own mesh (the capture runs
// without it): `rect` is the shape's place in the panel (0..1), premultiplied alpha.
fragment float4 cursor_fragment(VOut in [[stage_in]],
                                texture2d<float> cursor [[texture(0)]],
                                sampler s [[sampler(0)]],
                                constant float4& rect [[buffer(0)]],
                                constant float& brightness [[buffer(1)]]) {
    const float2 c = (in.uv - rect.xy) / (rect.zw - rect.xy);
    if (any(c < 0.0) || any(c > 1.0)) discard_fragment();
    const float4 t = cursor.sample(s, c, level(0.0));
    return float4(t.rgb * brightness, t.a);
}

fragment float4 screen_sharp_fragment(VOut in [[stage_in]],
                                      texture2d<float> source [[texture(0)]],
                                      sampler s [[sampler(0)]],
                                      constant float& brightness [[buffer(0)]]) {
    const float top = float(source.get_num_mip_levels() - 1);
    const float lv = min(top, floor(max(0.0, source.calculate_clamped_lod(s, in.uv) - 0.5) + 0.5));
    const float2 size = float2(source.get_width(uint(lv)), source.get_height(uint(lv)));
    const float2 p = in.uv * size;
    const float2 t1 = floor(p - 0.5) + 0.5;
    const float2 f = p - t1;
    const float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    const float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    const float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    const float2 w3 = f * f * (-0.5 + 0.5 * f);
    const float2 w12 = w1 + w2;
    const float2 c0 = (t1 - 1.0) / size, c3 = (t1 + 2.0) / size, c12 = (t1 + w2 / w12) / size;
    float3 r = float3(0.0);
    r += source.sample(s, float2(c0.x, c0.y), level(lv)).rgb * w0.x * w0.y;
    r += source.sample(s, float2(c12.x, c0.y), level(lv)).rgb * w12.x * w0.y;
    r += source.sample(s, float2(c3.x, c0.y), level(lv)).rgb * w3.x * w0.y;
    r += source.sample(s, float2(c0.x, c12.y), level(lv)).rgb * w0.x * w12.y;
    r += source.sample(s, float2(c12.x, c12.y), level(lv)).rgb * w12.x * w12.y;
    r += source.sample(s, float2(c3.x, c12.y), level(lv)).rgb * w3.x * w12.y;
    r += source.sample(s, float2(c0.x, c3.y), level(lv)).rgb * w0.x * w3.y;
    r += source.sample(s, float2(c12.x, c3.y), level(lv)).rgb * w12.x * w3.y;
    r += source.sample(s, float2(c3.x, c3.y), level(lv)).rgb * w3.x * w3.y;
    return float4(max(r, 0.0) * brightness, 1.0);
}

// AI 3D (cinema): a 2D video seen in depth. Each eye samples the frame shifted by a parallax
// taken from its depth map (0 far .. 1 near), so near things stand out and the rest recedes.
struct StereoUniforms {
    float brightness;
    float eye;          // -1 left, +1 right
    float strength;     // parallax between the eyes at depth 1, in panel widths
    float convergence;  // depth seen on the panel's plane (no parallax)
    float edge;         // margins where the parallax fades to 0, in widths of the video
    float pad0, pad1, pad2;
    float4 rect;        // where the video is in the frame, 0..1 (x0, y0, x1, y1): flat outside
};

float stereo_shift(texture2d<float> depth, sampler s, float x, float y, constant StereoUniforms& u) {
    const float d = depth.sample(s, float2(x, y), level(0)).r;
    // At the video's sides the image goes back to the panel's plane: something in front of it
    // cut by the frame would contradict the frame being in front of it (window violation). Above
    // and below too when the video does not reach the panel's edge (a page around it).
    const float ew = u.edge * (u.rect.z - u.rect.x), eh = u.edge * (u.rect.w - u.rect.y);
    float fade = smoothstep(0.0, ew, x - u.rect.x) * smoothstep(0.0, ew, u.rect.z - x);
    if (u.rect.y > 0.005) fade *= smoothstep(0.0, eh, y - u.rect.y);
    if (u.rect.w < 0.995) fade *= smoothstep(0.0, eh, u.rect.w - y);
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
    float box;     // 1: sample level 0 only (2x supersampled, no distortion: a 2x2 box per pixel)
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
    if (d.box > 0.5) return float4(eye.sample(s, uv, level(0.0)).rgb, 1.0);
    return float4(eye.sample(s, uv).rgb, 1.0);
}

// Background around the panels (on see-through glasses black is the room itself: "none" draws
// nothing). Fixed in the room like the panels, at infinity: each pixel's direction in the room
// picks the colour. Styles: 1 rain drops, 2 stars, 3 sunset, 4 aurora, 5 a picture.
struct BackgroundUniforms {
    float3x3 eye_to_world;  // columns: the eye's right, up and forward in the room
    float tan_x;            // tangent of half the eye's horizontal field of view
    float tan_y;
    float time;             // seconds, for the moving styles
    float brightness;       // 0..1
    int style;
    float image_aspect;     // style 5: width / height
};

static float bg_hash(float2 p) {
    p = fract(p * float2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

static float bg_hash3(float3 p) {
    p = fract(p * float3(0.1031, 0.1030, 0.0973));
    p += dot(p, p.yxz + 33.33);
    return fract((p.x + p.y) * p.z);
}

static float bg_noise(float2 p) {
    const float2 i = floor(p), f = fract(p), u = f * f * (3.0 - 2.0 * f);
    return mix(mix(bg_hash(i), bg_hash(i + float2(1, 0)), u.x), mix(bg_hash(i + float2(0, 1)), bg_hash(i + float2(1, 1)), u.x), u.y);
}

fragment float4 background_fragment(VOut in [[stage_in]],
                                    constant BackgroundUniforms& b [[buffer(0)]],
                                    texture2d<float> picture [[texture(0)]],
                                    sampler s [[sampler(0)]]) {
    const float2 ndc = float2(in.uv.x * 2.0 - 1.0, 1.0 - in.uv.y * 2.0);
    const float3 d = normalize(b.eye_to_world * float3(ndc.x * b.tan_x, ndc.y * b.tan_y, 1.0));
    const float yaw = atan2(d.x, d.z);             // -pi..pi, 0 straight ahead at the recentre
    const float pitch = asin(clamp(d.y, -1.0, 1.0));  // -pi/2..pi/2, up positive
    float3 c = float3(0.0);
    if (b.style == 1) {  // rain: streaks falling down a dim blue-grey sky
        c = mix(float3(0.035, 0.045, 0.07), float3(0.09, 0.11, 0.15), saturate(pitch * 0.8 + 0.5));
        const float columns = 140.0;
        const float x = (yaw / 3.14159265 + 1.0) * columns;
        const float col = floor(x);
        const float speed = 0.35 + 0.4 * bg_hash(float2(col, 1.0));
        const float phase = bg_hash(float2(col, 7.0));
        const float y = fract(pitch / 3.14159265 + 0.5 + b.time * speed + phase);  // falls as time goes
        const float streak = smoothstep(0.0, 0.03, y) * (1.0 - smoothstep(0.03, 0.09, y));
        const float thin = 1.0 - smoothstep(0.0, 0.18, abs(fract(x) - 0.5));
        c += float3(0.55, 0.62, 0.75) * streak * thin * (0.4 + 0.6 * bg_hash(float2(col, 3.0)));
    } else if (b.style == 2) {  // stars: a night sky, slowly twinkling
        c = mix(float3(0.005, 0.008, 0.02), float3(0.02, 0.03, 0.07), saturate(1.0 - abs(pitch)));
        for (int layer = 0; layer < 2; ++layer) {
            const float scale = layer == 0 ? 90.0 : 220.0;
            const float3 cell = floor(d * scale);
            const float h = bg_hash3(cell + float(layer) * 17.0);
            if (h > (layer == 0 ? 0.985 : 0.992)) {
                const float3 centre = (cell + 0.5) / scale;
                const float dist = length(d - normalize(centre)) * scale;
                const float tw = 0.65 + 0.35 * sin(b.time * (1.0 + 3.0 * h) + h * 50.0);
                c += float3(0.9, 0.92, 1.0) * (1.0 - smoothstep(0.05, 0.45, dist)) * tw * (layer == 0 ? 1.0 : 0.6);
            }
        }
    } else if (b.style == 3) {  // sunset: warm at the horizon, deep blue above, the sun ahead
        const float t = saturate(pitch / 1.2 + 0.1);
        c = mix(float3(0.95, 0.45, 0.18), float3(0.10, 0.12, 0.35), pow(t, 0.6));
        c = mix(c, float3(0.05, 0.04, 0.08), saturate(-pitch * 3.0));  // ground below
        const float sun = length(float2(yaw, pitch - 0.08));
        c += float3(1.0, 0.75, 0.4) * (1.0 - smoothstep(0.02, 0.05, sun)) + float3(0.9, 0.4, 0.15) * exp(-sun * 6.0) * 0.5;
    } else if (b.style == 4) {  // aurora: green curtains drifting over a night sky
        c = mix(float3(0.005, 0.01, 0.02), float3(0.01, 0.02, 0.05), saturate(pitch + 0.5));
        const float band = bg_noise(float2(yaw * 3.0 + b.time * 0.05, b.time * 0.08)) * 0.5
                         + bg_noise(float2(yaw * 7.0 - b.time * 0.03, 3.0)) * 0.5;
        const float h = pitch - (0.25 + 0.25 * band);
        const float curtain = exp(-h * h * 40.0) * smoothstep(0.0, 0.3, pitch);
        const float rays = 0.6 + 0.4 * bg_noise(float2(yaw * 40.0, b.time * 0.3));
        c += mix(float3(0.1, 0.9, 0.5), float3(0.5, 0.2, 0.8), saturate(h * 4.0 + 0.5)) * curtain * rays * 0.6;
    } else if (b.style == 5) {  // a picture: 180 deg wide, mirrored beyond (a 2:1 panorama fills all around)
        const float wide = b.image_aspect >= 1.9 ? 2.0 * 3.14159265 : 3.14159265;
        const float2 uv = float2(yaw / wide + 0.5, 0.5 - pitch / (wide / max(0.2, b.image_aspect)));
        c = picture.sample(s, uv).rgb;
    }
    return float4(c * b.brightness, 1.0);
}
