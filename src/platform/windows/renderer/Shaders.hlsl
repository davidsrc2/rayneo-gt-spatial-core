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
