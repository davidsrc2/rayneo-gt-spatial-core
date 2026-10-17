// Spatial screen renderer, Direct3D 11 port of Shaders.metal. Compiled at runtime
// (D3DCompile) so the build needs no fxc step; CMake embeds this file into rn_shaders_hlsl.h.

// Mirrors rayneo::core::ScreenVertex (5 tightly packed floats).
struct ScreenVertex {
    float3 position : POSITION;
    float2 uv : TEXCOORD0;
};

struct VOut {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

// Column-major (HLSL's default cbuffer packing), memcpy'd from rayneo::core::Mat4.
cbuffer SceneUniforms : register(b0) {
    float4x4 view_projection;
};

cbuffer PanelUniforms : register(b1) {
    float brightness;
    float3 panel_pad;
};

cbuffer DistortUniforms : register(b2) {
    float k1;
    float k2;
    float aspect;  // eye image width / height as perceived (16:9 even when squeezed SBS)
    float distort_pad;
};

// Overlay quad (cursor) in normalised target coordinates: x0, y0, x1, y1 (0..1, y down).
cbuffer OverlayUniforms : register(b3) {
    float4 rect;
};

Texture2D source : register(t0);
SamplerState samp : register(s0);

// Pass 1: virtual screen into the per-eye texture.
VOut screen_vertex(ScreenVertex v) {
    VOut o;
    o.position = mul(view_projection, float4(v.position, 1.0));
    o.uv = v.uv;
    return o;
}

// Textures are sRGB views: sampling, mipmaps and this brightness work on linear light.
float4 screen_fragment(VOut i) : SV_Target {
    // A slight bias towards the full-size level: a panel drawn a little smaller than its capture
    // blended in the half-size mipmap and softened the text.
    return float4(source.SampleBias(samp, i.uv, -0.5).rgb * brightness, 1.0);
}

// Pass 2: per-eye barrel pre-distortion into the back buffer viewport (half for SBS).
VOut fullscreen_vertex(uint vid : SV_VertexID) {
    const float2 p = float2((vid << 1) & 2, vid & 2);  // (0,0) (2,0) (0,2)
    VOut o;
    o.position = float4(p * 2.0 - 1.0, 0.0, 1.0);
    o.uv = float2(p.x, 1.0 - p.y);
    return o;
}

// Same formula as rayneo::core::barrel_distort (normalised: centre 0, half-height 1).
float4 distort_fragment(VOut i) : SV_Target {
    float2 c = i.uv * 2.0 - 1.0;
    c.x *= aspect;
    const float r2 = dot(c, c);
    float2 p = c * (1.0 + k1 * r2 + k2 * r2 * r2);
    p.x /= aspect;
    const float2 uv = p * 0.5 + 0.5;
    if (any(uv < 0.0) || any(uv > 1.0)) return float4(0.0, 0.0, 0.0, 1.0);
    return float4(source.Sample(samp, uv).rgb, 1.0);
}

// Cursor composited into a panel's copy of the captured frame (Desktop Duplication hands
// the pointer over separately). Two passes share this quad: alpha blend for the colour
// pixels and an inverting blend for the XOR pixels of monochrome / masked shapes.
VOut overlay_vertex(uint vid : SV_VertexID) {
    const float2 corner = float2(vid & 1, vid >> 1);  // triangle strip (0,0) (1,0) (0,1) (1,1)
    const float2 t = lerp(rect.xy, rect.zw, corner);
    VOut o;
    o.position = float4(t.x * 2.0 - 1.0, 1.0 - t.y * 2.0, 0.0, 1.0);
    o.uv = corner;
    return o;
}

float4 overlay_fragment(VOut i) : SV_Target {
    return source.Sample(samp, i.uv);
}

// ---------------------------------------------------------------------------------------
// AI 3D (cinema): a 2D video seen in depth, as in Shaders.metal. Each eye samples the frame
// shifted by a parallax taken from its depth map (0 far .. 1 near).
// ---------------------------------------------------------------------------------------

cbuffer StereoUniforms : register(b4) {
    float stereo_brightness;
    float stereo_eye;          // -1 left, +1 right
    float stereo_strength;     // parallax between the eyes at depth 1, in panel widths
    float stereo_convergence;  // depth seen on the panel's plane (no parallax)
    float stereo_edge;         // margins where the parallax fades to 0, in widths of the video
    float3 stereo_pad;
    float4 stereo_rect;        // where the video is in the frame, 0..1 (x0, y0, x1, y1): flat outside
};

cbuffer DepthBlendUniforms : register(b5) {
    float blend_low;
    float blend_high;
    float blend_alpha;
    float downscale_lod;  // downscale_fragment: the mipmap level about the size of the target
};

Texture2D depth_map : register(t1);
SamplerState depth_samp : register(s1);  // bilinear, clamped

float stereo_shift(float x, float y) {
    const float d = depth_map.SampleLevel(depth_samp, float2(x, y), 0).r;
    // At the video's sides the image goes back to the panel's plane (no window violation); above
    // and below too when the video does not reach the panel's edge (a page around it).
    const float ew = stereo_edge * (stereo_rect.z - stereo_rect.x), eh = stereo_edge * (stereo_rect.w - stereo_rect.y);
    float fade = smoothstep(0.0, ew, x - stereo_rect.x) * smoothstep(0.0, ew, stereo_rect.z - x);
    if (stereo_rect.y > 0.005) fade *= smoothstep(0.0, eh, y - stereo_rect.y);
    if (stereo_rect.w < 0.995) fade *= smoothstep(0.0, eh, stereo_rect.w - y);
    return -stereo_eye * 0.5 * stereo_strength * (d - stereo_convergence) * fade;
}

// Backward mapping, nearest candidate first: occlusions resolve to the nearer surface and no
// output pixel is left without a source.
float4 stereo_screen_fragment(VOut i) : SV_Target {
    const int kSteps = 8;
    const float k = -stereo_eye;
    const float near_t = 0.5 * stereo_strength * (1.0 - stereo_convergence);
    const float far_t = 0.5 * stereo_strength * (0.0 - stereo_convergence);
    float prev_t = near_t, prev_g = -1.0;
    float t = far_t;
    [loop] for (int s = 0; s <= kSteps; ++s) {
        const float ti = lerp(near_t, far_t, float(s) / float(kSteps));
        const float a = k * stereo_shift(i.uv.x - k * ti, i.uv.y);
        const float g = a - ti;
        if (g >= 0.0) {
            t = s == 0 ? ti : ti + (g / (g - prev_g)) * (prev_t - ti);
            break;
        }
        prev_t = ti;
        prev_g = g;
    }
    const float2 uv = float2(i.uv.x - k * t, i.uv.y);
    // Gradients of the unshifted coordinates: the shift jumps at depth edges.
    const float3 c = source.SampleGrad(samp, uv, ddx(i.uv), ddy(i.uv)).rgb;
    return float4(c * stereo_brightness, 1.0);
}

// Temporal smoothing of the depth maps: the new one (t0) normalised to its 2-98 % range and
// blended into the previous one (t1). alpha 1 at a scene cut.
float4 depth_blend_fragment(VOut i) : SV_Target {
    const float d = saturate((source.SampleLevel(depth_samp, i.uv, 0).r - blend_low) / max(blend_high - blend_low, 1e-4));
    const float p = depth_map.SampleLevel(depth_samp, i.uv, 0).r;
    return float4(lerp(p, d, blend_alpha), 0.0, 0.0, 1.0);
}

// The frame shrunk to the depth model's input (raw sRGB bytes, as the model was trained on),
// from the mipmap level about that size so it does not alias.
float4 downscale_fragment(VOut i) : SV_Target {
    return source.SampleLevel(depth_samp, i.uv, downscale_lod);
}

// Compiled only for the background (NIMBO_BACKGROUND defined): a mistake here never takes the
// panels' shaders down with it.
#ifdef NIMBO_BACKGROUND
// Background around the panels, as in Shaders.metal (styles 1 rain, 2 stars, 3 sunset, 4 aurora,
// 5 a picture); fixed in the room, at infinity.
cbuffer BackgroundUniforms : register(b6) {
    float4 bg_right;    // the eye's axes in the room (xyz)
    float4 bg_up;
    float4 bg_forward;
    float bg_tan_x;
    float bg_tan_y;
    float bg_time;
    float bg_brightness;
    int bg_style;
    float bg_image_aspect;
    float2 bg_pad;
};
Texture2D bg_picture : register(t2);
SamplerState bg_samp : register(s2);  // mirrored beyond the picture's edges

float bg_hash(float2 p) {
    p = frac(p * float2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return frac(p.x * p.y);
}

float bg_hash3(float3 p) {
    p = frac(p * float3(0.1031, 0.1030, 0.0973));
    p += dot(p, p.yxz + 33.33);
    return frac((p.x + p.y) * p.z);
}

float bg_noise(float2 p) {
    float2 i = floor(p), f = frac(p), u = f * f * (3.0 - 2.0 * f);
    return lerp(lerp(bg_hash(i), bg_hash(i + float2(1, 0)), u.x), lerp(bg_hash(i + float2(0, 1)), bg_hash(i + float2(1, 1)), u.x), u.y);
}

float4 background_fragment(VOut i) : SV_Target {
    float2 ndc = float2(i.uv.x * 2.0 - 1.0, 1.0 - i.uv.y * 2.0);
    float3 d = normalize(bg_right.xyz * (ndc.x * bg_tan_x) + bg_up.xyz * (ndc.y * bg_tan_y) + bg_forward.xyz);
    float yaw = atan2(d.x, d.z);             // -pi..pi, 0 straight ahead at the recentre
    float pitch = asin(clamp(d.y, -1.0, 1.0));  // -pi/2..pi/2, up positive
    float3 c = (float3)0;
    if (bg_style == 1) {  // rain: streaks falling down a dim blue-grey sky
        c = lerp(float3(0.035, 0.045, 0.07), float3(0.09, 0.11, 0.15), saturate(pitch * 0.8 + 0.5));
        float columns = 140.0;
        float x = (yaw / 3.14159265 + 1.0) * columns;
        float col = floor(x);
        float speed = 0.35 + 0.4 * bg_hash(float2(col, 1.0));
        float phase = bg_hash(float2(col, 7.0));
        float y = frac(pitch / 3.14159265 + 0.5 + bg_time * speed + phase);  // falls as time goes
        float streak = smoothstep(0.0, 0.03, y) * (1.0 - smoothstep(0.03, 0.09, y));
        float thin = 1.0 - smoothstep(0.0, 0.18, abs(frac(x) - 0.5));
        c += float3(0.55, 0.62, 0.75) * streak * thin * (0.4 + 0.6 * bg_hash(float2(col, 3.0)));
    } else if (bg_style == 2) {  // stars: a night sky, slowly twinkling
        c = lerp(float3(0.005, 0.008, 0.02), float3(0.02, 0.03, 0.07), saturate(1.0 - abs(pitch)));
        for (int layer = 0; layer < 2; ++layer) {
            float scale = layer == 0 ? 90.0 : 220.0;
            float3 cell = floor(d * scale);
            float h = bg_hash3(cell + float(layer) * 17.0);
            if (h > (layer == 0 ? 0.985 : 0.992)) {
                float3 centre = (cell + 0.5) / scale;
                float dist = length(d - normalize(centre)) * scale;
                float tw = 0.65 + 0.35 * sin(bg_time * (1.0 + 3.0 * h) + h * 50.0);
                c += float3(0.9, 0.92, 1.0) * (1.0 - smoothstep(0.05, 0.45, dist)) * tw * (layer == 0 ? 1.0 : 0.6);
            }
        }
    } else if (bg_style == 3) {  // sunset: warm at the horizon, deep blue above, the sun ahead
        float t = saturate(pitch / 1.2 + 0.1);
        c = lerp(float3(0.95, 0.45, 0.18), float3(0.10, 0.12, 0.35), pow(t, 0.6));
        c = lerp(c, float3(0.05, 0.04, 0.08), saturate(-pitch * 3.0));  // ground below
        float sun = length(float2(yaw, pitch - 0.08));
        c += float3(1.0, 0.75, 0.4) * (1.0 - smoothstep(0.02, 0.05, sun)) + float3(0.9, 0.4, 0.15) * exp(-sun * 6.0) * 0.5;
    } else if (bg_style == 4) {  // aurora: green curtains drifting over a night sky
        c = lerp(float3(0.005, 0.01, 0.02), float3(0.01, 0.02, 0.05), saturate(pitch + 0.5));
        float band = bg_noise(float2(yaw * 3.0 + bg_time * 0.05, bg_time * 0.08)) * 0.5
                         + bg_noise(float2(yaw * 7.0 - bg_time * 0.03, 3.0)) * 0.5;
        float h = pitch - (0.25 + 0.25 * band);
        float curtain = exp(-h * h * 40.0) * smoothstep(0.0, 0.3, pitch);
        float rays = 0.6 + 0.4 * bg_noise(float2(yaw * 40.0, bg_time * 0.3));
        c += lerp(float3(0.1, 0.9, 0.5), float3(0.5, 0.2, 0.8), saturate(h * 4.0 + 0.5)) * curtain * rays * 0.6;
    } else if (bg_style == 5) {  // a picture: 180 deg wide, mirrored beyond (a 2:1 panorama fills all around)
        float wide = bg_image_aspect >= 1.9 ? 2.0 * 3.14159265 : 3.14159265;
        float2 uv = float2(yaw / wide + 0.5, 0.5 - pitch / (wide / max(0.2, bg_image_aspect)));
        c = bg_picture.Sample(bg_samp, uv).rgb;
    }
    return float4(c * bg_brightness, 1.0);
}
#endif  // NIMBO_BACKGROUND
