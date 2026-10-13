// Depth of field (physical circle of confusion, half-resolution scatter-as-gather bokeh, full-resolution composite)
// and camera motion blur (velocity from depth reprojection). Linear, pre-exposed HDR in and out.

#include "Common/Bindless.hlsli"

struct DofParams
{
    float4x4 invViewProj;  // current, without jitter
    float4x4 prevViewProj; // previous frame, without jitter
    uint color;            // SRV: full-resolution scene color (prepare, composite, motion blur)
    uint depth;            // SRV: scene depth (reversed-Z, infinite far)
    uint input;            // SRV: half-resolution DOF texture (blur input / composite blurred)
    uint output;           // UAV
    float2 size;           // pass target size
    float2 invSize;
    float2 fullInvSize;    // 1 / full resolution
    float nearZ;
    float cocScale;        // signed CoC (full-res pixels) = cocScale * (1 - focus / z)
    float focusDistance;   // meters
    float maxCoc;          // full-res pixels
    float radiusStep;      // blur: spiral step (half-res pixels)
    float motionScale;     // motion blur: fraction of the per-frame motion (shutter)
    float motionMax;       // pixels
    uint samples;          // motion blur samples
    uint viewMode;         // 5 = CoC visualization
    float pad;
};
ZX_PASS_CONSTANTS(DofParams, g_Dof);

static const float GoldenAngle = 2.39996323;

float LinearDepth(float d) { return g_Dof.nearZ / max(d, 1e-7); } // reversed-Z infinite projection

float SignedCoc(float d)
{
    const float z = LinearDepth(d);
    return clamp(g_Dof.cocScale * (1.0 - g_Dof.focusDistance / z), -g_Dof.maxCoc, g_Dof.maxCoc); // < 0 near, > 0 far
}

// Full -> half resolution: color + signed CoC (half-res pixels). Near CoC wins so foreground edges blur outwards.
[numthreads(8, 8, 1)]
void DofPrepareCS(uint2 id : SV_DispatchThreadID)
{
    if (any(id >= uint2(g_Dof.size)))
        return;
    Texture2D<float4> color = GetTexture2D(g_Dof.color);
    Texture2D<float4> depth = GetTexture2D(g_Dof.depth);
    const int2 full = int2(id) * 2;
    const int2 maxP = int2(1.0 / g_Dof.fullInvSize) - 1;
    float3 c = 0;
    float nearCoc = 0, farCoc = 0;
    [unroll] for (int i = 0; i < 4; ++i) {
        const int2 p = min(full + int2(i & 1, i >> 1), maxP);
        c += color.Load(int3(p, 0)).rgb;
        const float coc = SignedCoc(depth.Load(int3(p, 0)).r);
        nearCoc = min(nearCoc, coc);
        farCoc = max(farCoc, coc);
    }
    const float coc = nearCoc < -0.5 ? nearCoc : farCoc;
    GetRWTexture2D(g_Dof.output)[id] = float4(c * 0.25, coc * 0.5);
}

// Half-resolution bokeh gather along a golden-angle spiral (Dennis Gustafsson's single-pass method): each sample
// contributes when its own CoC reaches this pixel; samples farther away than the center are limited to twice the
// center's CoC, so a blurred background doesn't smear over an in-focus foreground.
[numthreads(8, 8, 1)]
void DofBlurCS(uint2 id : SV_DispatchThreadID)
{
    if (any(id >= uint2(g_Dof.size)))
        return;
    Texture2D<float4> input = GetTexture2D(g_Dof.input);
    const float4 center = input.Load(int3(id, 0));
    const float centerSize = abs(center.a);
    const float maxRadius = g_Dof.maxCoc * 0.5;
    float3 acc = center.rgb;
    float total = 1.0;
    float radius = g_Dof.radiusStep;
    float angle = 0.0;
    const float2 uv0 = (float2(id) + 0.5) * g_Dof.invSize;
    [loop] for (uint n = 0; n < 256 && radius < maxRadius; ++n) {
        const float2 uv = uv0 + float2(cos(angle), sin(angle)) * radius * g_Dof.invSize;
        const float4 s = input.SampleLevel(g_LinearClamp, uv, 0);
        float sampleSize = abs(s.a);
        if (s.a > center.a)
            sampleSize = clamp(sampleSize, 0.0, centerSize * 2.0);
        const float m = smoothstep(radius - 0.5, radius + 0.5, sampleSize);
        acc += lerp(acc / total, s.rgb, m);
        total += 1.0;
        radius += g_Dof.radiusStep / radius;
        angle += GoldenAngle;
    }
    GetRWTexture2D(g_Dof.output)[id] = float4(acc / total, center.a);
}

// Full resolution: sharp image blended with the blurred half-resolution one by the CoC (near field also by the
// blurred CoC, so blurry foreground covers in-focus background).
[numthreads(8, 8, 1)]
void DofCompositeCS(uint2 id : SV_DispatchThreadID)
{
    if (any(id >= uint2(g_Dof.size)))
        return;
    const float3 sharp = GetTexture2D(g_Dof.color).Load(int3(id, 0)).rgb;
    const float coc = SignedCoc(GetTexture2D(g_Dof.depth).Load(int3(id, 0)).r);
    const float2 uv = (float2(id) + 0.5) * g_Dof.invSize;
    const float4 blurred = GetTexture2D(g_Dof.input).SampleLevel(g_LinearClamp, uv, 0);
    float amount = abs(coc);
    if (blurred.a < 0.0)
        amount = max(amount, -blurred.a * 2.0);
    const float t = smoothstep(0.5, 2.5, amount);
    float3 result = lerp(sharp, blurred.rgb, t);
    if (g_Dof.viewMode == 5) {
        // Focus check: near blur red, far blur blue, in focus green-tinted image.
        const float k = saturate(abs(coc) / max(g_Dof.maxCoc * 0.5, 1.0));
        const float l = dot(sharp, float3(0.2126, 0.7152, 0.0722));
        result = coc < 0 ? lerp(float3(0.2, 1.0, 0.2) * l, float3(1.0, 0.1, 0.1), k) : lerp(float3(0.2, 1.0, 0.2) * l, float3(0.1, 0.3, 1.0), k);
    }
    GetRWTexture2D(g_Dof.output)[id] = float4(result, 1);
}

// Camera motion blur: the pixel's screen velocity from reprojecting its depth with last frame's view-projection.
[numthreads(8, 8, 1)]
void MotionBlurCS(uint2 id : SV_DispatchThreadID)
{
    if (any(id >= uint2(g_Dof.size)))
        return;
    Texture2D<float4> color = GetTexture2D(g_Dof.color);
    const float d = GetTexture2D(g_Dof.depth).Load(int3(id, 0)).r;
    const float2 uv = (float2(id) + 0.5) * g_Dof.invSize;
    const float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    const float4 world = mul(g_Dof.invViewProj, float4(ndc, d, 1.0));
    const float4 prevClip = mul(g_Dof.prevViewProj, world);
    const float3 current = color.Load(int3(id, 0)).rgb;
    if (prevClip.w <= 0.0) {
        GetRWTexture2D(g_Dof.output)[id] = float4(current, 1);
        return;
    }
    const float2 prevNdc = prevClip.xy / prevClip.w;
    const float2 prevUv = float2(prevNdc.x * 0.5 + 0.5, 0.5 - prevNdc.y * 0.5);
    float2 velocity = (uv - prevUv) * g_Dof.size * g_Dof.motionScale; // pixels
    const float len = length(velocity);
    // A camera cut (or a huge jump) is not motion.
    if (len < 0.5 || len > g_Dof.size.x * 0.25) {
        GetRWTexture2D(g_Dof.output)[id] = float4(current, 1);
        return;
    }
    if (len > g_Dof.motionMax)
        velocity *= g_Dof.motionMax / len;
    const uint n = max(g_Dof.samples, 2u);
    float3 acc = 0;
    // Interleaved offset along the streak hides banding (resolved by TAA).
    const float jitter = frac(52.9829189 * frac(dot(float2(id), float2(0.06711056, 0.00583715)))) - 0.5;
    for (uint i = 0; i < n; ++i) {
        const float t = (float(i) + 0.5 + jitter) / float(n) - 0.5;
        acc += color.SampleLevel(g_LinearClamp, uv + velocity * t * g_Dof.invSize, 0).rgb;
    }
    GetRWTexture2D(g_Dof.output)[id] = float4(acc / float(n), 1);
}
