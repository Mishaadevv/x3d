// Temporal anti-aliasing (resolve). Camera-motion reprojection from depth (static scenes; per-object velocity comes
// with skinned/moving objects), YCoCg variance clipping of the history, Catmull-Rom history fetch, luminance-weighted
// blend against fireflies. Input/output are linear, pre-exposed HDR.

#include "Common/Bindless.hlsli"

struct TAAParams
{
    float4x4 invViewProj;   // current, without jitter: clip -> world (homogeneous)
    float4x4 prevViewProj;  // previous frame, without jitter
    uint color;             // SRV current
    uint depth;             // SRV
    uint history;           // SRV previous output (or invalid)
    uint output;            // UAV
    float2 size;
    float2 invSize;
    float historyScale;     // exposure(prev -> current) ratio
    float blend;            // weight of the current frame (0.05 .. 0.2)
    uint catmullRom;
    uint velocity;          // SRV: motion of moving objects (uv units, < -100 = none) or invalid
    float2 inputSize;       // ZTSR: render resolution (size = output resolution)
    float2 jitter;          // ZTSR: this frame's jitter in input pixels (screen space, y down)
};
ZX_PASS_CONSTANTS(TAAParams, g_TAA);

float3 ToYCoCg(float3 c) { return float3(dot(c, float3(0.25, 0.5, 0.25)), dot(c, float3(0.5, 0, -0.5)), dot(c, float3(-0.25, 0.5, -0.25))); }
float3 FromYCoCg(float3 c) { return float3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z); }

float3 SampleHistory(Texture2D<float4> h, float2 uv)
{
    if (!g_TAA.catmullRom)
        return h.SampleLevel(g_LinearClamp, uv, 0).rgb;
    // 5-tap Catmull-Rom (corners dropped), Jimenez's optimization.
    const float2 pos = uv * g_TAA.size;
    const float2 center = floor(pos - 0.5) + 0.5;
    const float2 f = pos - center;
    const float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    const float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    const float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    const float2 w3 = f * f * (-0.5 + 0.5 * f);
    const float2 w12 = w1 + w2;
    const float2 tc0 = (center - 1.0) * g_TAA.invSize;
    const float2 tc3 = (center + 2.0) * g_TAA.invSize;
    const float2 tc12 = (center + w2 / w12) * g_TAA.invSize;
    float3 c = h.SampleLevel(g_LinearClamp, float2(tc12.x, tc0.y), 0).rgb * (w12.x * w0.y);
    c += h.SampleLevel(g_LinearClamp, float2(tc0.x, tc12.y), 0).rgb * (w0.x * w12.y);
    c += h.SampleLevel(g_LinearClamp, float2(tc12.x, tc12.y), 0).rgb * (w12.x * w12.y);
    c += h.SampleLevel(g_LinearClamp, float2(tc3.x, tc12.y), 0).rgb * (w3.x * w12.y);
    c += h.SampleLevel(g_LinearClamp, float2(tc12.x, tc3.y), 0).rgb * (w12.x * w3.y);
    const float wsum = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
    return max(c / wsum, 0.0);
}

[numthreads(8, 8, 1)]
void ResolveCS(uint2 id : SV_DispatchThreadID)
{
    if (any(id >= uint2(g_TAA.size)))
        return;
    Texture2D<float4> color = GetTexture2D(g_TAA.color);
    const float3 current = color.Load(int3(id, 0)).rgb;
    RWTexture2D<float4> output = GetRWTexture2D(g_TAA.output);
    if (g_TAA.history == ZX_INVALID_INDEX) {
        output[id] = float4(current, 1);
        return;
    }
    // Neighbourhood statistics (YCoCg) and the closest depth for reprojection (sharper silhouettes).
    float3 m1 = 0, m2 = 0;
    float closest = 0;
    int2 closestOffset = 0;
    [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x) {
            const int2 p = clamp(int2(id) + int2(x, y), 0, int2(g_TAA.size) - 1);
            const float3 c = ToYCoCg(color.Load(int3(p, 0)).rgb);
            m1 += c;
            m2 += c * c;
            const float d = GetTexture2D(g_TAA.depth).Load(int3(p, 0)).r;
            if (d > closest) { // reversed-Z: larger = closer
                closest = d;
                closestOffset = int2(x, y);
            }
        }
    const float3 mean = m1 / 9.0;
    const float3 sigma = sqrt(max(m2 / 9.0 - mean * mean, 0.0));
    const float3 boxMin = mean - 1.25 * sigma, boxMax = mean + 1.25 * sigma;

    // Reproject (homogeneous, so the sky at depth 0 reprojects as a direction).
    const float2 uv = (float2(id) + 0.5) * g_TAA.invSize;
    const float2 duv = float2(closestOffset) * g_TAA.invSize;
    const float2 ndc = float2((uv.x + duv.x) * 2.0 - 1.0, 1.0 - (uv.y + duv.y) * 2.0);
    const float4 world = mul(g_TAA.invViewProj, float4(ndc, closest, 1.0));
    const float4 prevClip = mul(g_TAA.prevViewProj, world);
    const float2 prevNdc = prevClip.xy / prevClip.w;
    float2 prevUv = float2(prevNdc.x * 0.5 + 0.5, 0.5 - prevNdc.y * 0.5) - duv;
    // Moving objects: follow their own motion (no ghost trails behind them).
    if (g_TAA.velocity != ZX_INVALID_INDEX) {
        const float2 objectMotion = GetTexture2D(g_TAA.velocity).SampleLevel(g_PointClamp, uv + duv, 0).rg;
        if (objectMotion.x > -100.0)
            prevUv = uv - objectMotion;
    }
    if (any(prevUv < 0.0) || any(prevUv > 1.0) || prevClip.w <= 0.0) {
        output[id] = float4(current, 1);
        return;
    }
    float3 history = ToYCoCg(SampleHistory(GetTexture2D(g_TAA.history), prevUv) * g_TAA.historyScale);
    // Clip towards the mean (instead of clamping): fewer color shifts.
    const float3 toHistory = history - mean;
    const float3 extent = max((boxMax - boxMin) * 0.5, 1e-5);
    const float3 unit = abs(toHistory / extent);
    const float maxUnit = max(unit.x, max(unit.y, unit.z));
    if (maxUnit > 1.0)
        history = mean + toHistory / maxUnit;
    history = FromYCoCg(history);
    // Luminance weighting (Karis): suppresses flicker from bright subpixel details.
    const float wc = g_TAA.blend / (1.0 + current.g), wh = (1.0 - g_TAA.blend) / (1.0 + history.g);
    output[id] = float4(max((current * wc + history * wh) / (wc + wh), 0.0), 1);
}

// --- ZTSR: temporal upscaling (render resolution -> output resolution) -------------------------------------------------
// Every frame the jittered low-resolution image lands at a different sub-pixel offset; each output pixel reconstructs
// the current frame from the nearby input samples (Gaussian weights by their distance after removing the jitter),
// clips the reprojected history to their neighbourhood and accumulates - faster where an input sample falls close.

[numthreads(8, 8, 1)]
void UpscaleCS(uint2 id : SV_DispatchThreadID)
{
    if (any(id >= uint2(g_TAA.size)))
        return;
    Texture2D<float4> color = GetTexture2D(g_TAA.color);
    Texture2D<float4> depthTex = GetTexture2D(g_TAA.depth);
    RWTexture2D<float4> output = GetRWTexture2D(g_TAA.output);
    const float2 uv = (float2(id) + 0.5) * g_TAA.invSize;
    // Position of this output pixel in the jittered input image (pixel centers at k + 0.5).
    const float2 inPos = uv * g_TAA.inputSize + g_TAA.jitter;
    const int2 center = int2(floor(inPos));
    const int2 maxP = int2(g_TAA.inputSize) - 1;
    float3 recon = 0;
    float wsum = 0, wmax = 0;
    float3 m1 = 0, m2 = 0;
    float closest = 0;
    int2 closestP = clamp(center, 0, maxP);
    [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x) {
            const int2 p = clamp(center + int2(x, y), 0, maxP);
            const float3 c = color.Load(int3(p, 0)).rgb;
            const float2 d = float2(p) + 0.5 - inPos; // input pixels
            const float w = exp(-2.2 * dot(d, d));
            recon += c * w;
            wsum += w;
            wmax = max(wmax, w);
            const float3 ycc = ToYCoCg(c);
            m1 += ycc;
            m2 += ycc * ycc;
            const float dz = depthTex.Load(int3(p, 0)).r;
            if (dz > closest) {
                closest = dz;
                closestP = p;
            }
        }
    const float3 current = max(recon / max(wsum, 1e-5), 0.0);
    if (g_TAA.history == ZX_INVALID_INDEX) {
        output[id] = float4(current, 1);
        return;
    }
    const float3 mean = m1 / 9.0;
    const float3 sigma = sqrt(max(m2 / 9.0 - mean * mean, 0.0));
    const float3 boxMin = mean - 1.25 * sigma, boxMax = mean + 1.25 * sigma;

    // Reproject with the closest depth around (its unjittered screen position).
    const float2 duv = ((float2(closestP) + 0.5 - g_TAA.jitter) / g_TAA.inputSize) - uv;
    const float2 ndc = float2((uv.x + duv.x) * 2.0 - 1.0, 1.0 - (uv.y + duv.y) * 2.0);
    const float4 world = mul(g_TAA.invViewProj, float4(ndc, closest, 1.0));
    const float4 prevClip = mul(g_TAA.prevViewProj, world);
    const float2 prevNdc = prevClip.xy / prevClip.w;
    float2 prevUv = float2(prevNdc.x * 0.5 + 0.5, 0.5 - prevNdc.y * 0.5) - duv;
    // Moving objects: follow their own motion (no ghost trails behind them).
    if (g_TAA.velocity != ZX_INVALID_INDEX) {
        const float2 objectMotion = GetTexture2D(g_TAA.velocity).SampleLevel(g_PointClamp, uv + duv, 0).rg;
        if (objectMotion.x > -100.0)
            prevUv = uv - objectMotion;
    }
    if (any(prevUv < 0.0) || any(prevUv > 1.0) || prevClip.w <= 0.0) {
        output[id] = float4(current, 1);
        return;
    }
    float3 history = ToYCoCg(SampleHistory(GetTexture2D(g_TAA.history), prevUv) * g_TAA.historyScale);
    const float3 toHistory = history - mean;
    const float3 extent = max((boxMax - boxMin) * 0.5, 1e-5);
    const float3 unit = abs(toHistory / extent);
    const float maxUnit = max(unit.x, max(unit.y, unit.z));
    if (maxUnit > 1.0)
        history = mean + toHistory / maxUnit;
    history = FromYCoCg(history);
    // A sample right on this output pixel counts fully, distant ones barely (keeps detail between frames).
    const float alpha = clamp(g_TAA.blend * lerp(0.25, 2.0, wmax), 0.02, 0.5);
    const float wc = alpha / (1.0 + current.g), wh = (1.0 - alpha) / (1.0 + history.g);
    output[id] = float4(max((current * wc + history * wh) / (wc + wh), 0.0), 1);
}
