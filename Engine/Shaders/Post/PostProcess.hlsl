// Post-processing chain (linear, pre-exposed HDR in): luminance histogram -> auto exposure -> bloom (dual filter)
// -> tonemapping (ACES fit / AgX) + grading -> FXAA + upscale to the back buffer.

#include "Common/Bindless.hlsli"
#include "Common/Color.hlsli"

struct PostParams
{
    uint input;         // SRV of the pass input
    uint output;        // UAV (compute passes)
    uint histogram;     // RW buffer: 64 bins + exposure state
    uint bloom;         // SRV of the bloom result (0 level), or invalid
    float2 inputTexel;  // 1 / input size
    float2 outputSize;
    float exposure;     // pre-exposure applied to the scene color this frame
    float targetEVBias; // exposure compensation (EV)
    float minEV;
    float maxEV;
    float adaptation;   // 1 - exp(-dt * speed)
    float bloomIntensity;
    float contrast;
    float saturation;
    float3 whiteBalance; // per-channel gains
    float vignette;
    uint tonemapper;    // 0 ACES, 1 AgX, 2 Filmic (Hable)
    uint viewMode;      // 0 final, 1 HDR (no tonemap), 2 depth, 3 luminance heatmap, 4 bloom only, 5 DOF focus
    uint depth;
    uint lut;           // color grading LUT (256 x 16, 16 slices of 16 x 16) or invalid
    float3 colorFilter;
    float lutIntensity;
};
ZX_PUSH_CONSTANTS(PostParams, g_Post);

#define HISTOGRAM_BINS 64
static const float HistogramMinLog = -10.0; // log2 luminance range of the histogram
static const float HistogramMaxLog = 20.0;


// --- Histogram ---------------------------------------------------------------------------------------------------

groupshared uint gs_bins[HISTOGRAM_BINS];

[numthreads(16, 16, 1)]
void HistogramCS(uint2 id : SV_DispatchThreadID, uint gi : SV_GroupIndex)
{
    if (gi < HISTOGRAM_BINS)
        gs_bins[gi] = 0;
    GroupMemoryBarrierWithGroupSync();
    // Sample every other pixel in each direction (quarter of the pixels is plenty for exposure).
    const uint2 p = id * 2;
    if (all(float2(p) < 1.0 / g_Post.inputTexel)) {
        const float3 c = GetTexture2D(g_Post.input).Load(int3(p, 0)).rgb;
        const float lum = Luminance(c) / g_Post.exposure; // absolute luminance (cd/m^2)
        if (lum > 1e-5) {
            const float t = saturate((log2(lum) - HistogramMinLog) / (HistogramMaxLog - HistogramMinLog));
            InterlockedAdd(gs_bins[min(uint(t * HISTOGRAM_BINS), HISTOGRAM_BINS - 1)], 1);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (gi < HISTOGRAM_BINS && gs_bins[gi])
        GetRWBuffer(g_Post.histogram).InterlockedAdd(gi * 4, gs_bins[gi]);
}

// Average of the histogram between the 50th and 95th percentiles -> target EV100, adapted over time.
// Buffer layout: [0..63] bins (cleared here), [64] adapted EV100 (float).
[numthreads(HISTOGRAM_BINS, 1, 1)]
void ExposureCS(uint gi : SV_GroupIndex)
{
    RWByteAddressBuffer h = GetRWBuffer(g_Post.histogram);
    gs_bins[gi] = h.Load(gi * 4);
    GroupMemoryBarrierWithGroupSync();
    if (gi == 0) {
        uint total = 0;
        for (uint i = 0; i < HISTOGRAM_BINS; ++i)
            total += gs_bins[i];
        const float low = total * 0.5, high = total * 0.95;
        float acc = 0, weight = 0, seen = 0;
        for (uint b = 0; b < HISTOGRAM_BINS; ++b) {
            const float n = gs_bins[b];
            const float take = max(0.0, min(seen + n, high) - max(seen, low));
            const float logLum = HistogramMinLog + (b + 0.5) / HISTOGRAM_BINS * (HistogramMaxLog - HistogramMinLog);
            acc += take * logLum;
            weight += take;
            seen += n;
        }
        const float avgLog = weight > 0 ? acc / weight : 0.0;
        // EV100 = log2(L * S / K), S = 100, K = 12.5; positive compensation brightens (as in UE/Unity).
        const float target = clamp(avgLog + log2(100.0 / 12.5) - g_Post.targetEVBias, g_Post.minEV, g_Post.maxEV);
        float ev = asfloat(h.Load(HISTOGRAM_BINS * 4));
        ev = (ev == 0.0 || !(ev == ev)) ? target : lerp(ev, target, g_Post.adaptation);
        h.Store(HISTOGRAM_BINS * 4, asuint(ev));
    }
    GroupMemoryBarrierWithGroupSync();
    h.Store(gi * 4, 0); // clear for the next frame
}

// --- Bloom (dual filter, Marius Bjorge) --------------------------------------------------------------------------

struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOutput FullscreenVS(uint vertexId : SV_VertexID)
{
    VSOutput o;
    o.uv = float2((vertexId << 1) & 2, vertexId & 2);
    o.position = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

float3 SampleInput(float2 uv) { return GetTexture2D(g_Post.input).SampleLevel(g_LinearClamp, uv, 0).rgb; }

float4 BloomDownPS(VSOutput i) : SV_Target0
{
    const float2 t = g_Post.inputTexel;
    float3 c = SampleInput(i.uv) * 4.0;
    c += SampleInput(i.uv + float2(-t.x, -t.y));
    c += SampleInput(i.uv + float2(t.x, -t.y));
    c += SampleInput(i.uv + float2(-t.x, t.y));
    c += SampleInput(i.uv + float2(t.x, t.y));
    c /= 8.0;
    // First level: soft threshold-free (energy conserving bloom), but tame fireflies.
    return float4(c / (1.0 + Luminance(c) * 0.01), 1);
}

float4 BloomUpPS(VSOutput i) : SV_Target0
{
    const float2 t = g_Post.inputTexel;
    float3 c = SampleInput(i.uv + float2(-t.x * 2.0, 0.0));
    c += SampleInput(i.uv + float2(-t.x, t.y)) * 2.0;
    c += SampleInput(i.uv + float2(0.0, t.y * 2.0));
    c += SampleInput(i.uv + float2(t.x, t.y)) * 2.0;
    c += SampleInput(i.uv + float2(t.x * 2.0, 0.0));
    c += SampleInput(i.uv + float2(t.x, -t.y)) * 2.0;
    c += SampleInput(i.uv + float2(0.0, -t.y * 2.0));
    c += SampleInput(i.uv + float2(-t.x, -t.y)) * 2.0;
    return float4(c / 12.0, 1); // blended additively onto the next larger level
}

// --- Tonemapping ---------------------------------------------------------------------------------------------------

// Filmic curve of Uncharted 2 (John Hable, 2010), white point 11.2 - the tonemapper of the late-2000s games.
float3 HableCurve(float3 x)
{
    const float A = 0.15, B = 0.50, C = 0.10, D = 0.20, E = 0.02, F = 0.30;
    return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}

float3 TonemapHable(float3 c)
{
    return saturate(HableCurve(c * 2.0) / HableCurve(11.2.xxx));
}

// AgX (Troy Sobotka), polynomial fit of the default contrast curve (Benjamin Wrensch).
float3 AgXContrast(float3 x)
{
    const float3 x2 = x * x, x4 = x2 * x2;
    return 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232;
}

float3 TonemapAgX(float3 c)
{
    const float3x3 inset = float3x3(0.842479062253094, 0.0423282422610123, 0.0423756549057051,
                                    0.0784335999999992, 0.878468636469772, 0.0784336,
                                    0.0792237451477643, 0.0791661274605434, 0.879142973793104);
    const float3x3 outset = float3x3(1.19687900512017, -0.0528968517574562, -0.0529716355144438,
                                     -0.0980208811401368, 1.15190312990417, -0.0980434501171241,
                                     -0.0990297440797205, -0.0989611768448433, 1.15107367264116);
    const float minEv = -12.47393, maxEv = 4.026069;
    c = mul(c, inset);
    c = clamp(log2(max(c, 1e-10)), minEv, maxEv);
    c = (c - minEv) / (maxEv - minEv);
    c = AgXContrast(c);
    c = mul(c, outset);
    return SRGBToLinear(saturate(c)); // AgX produces display-encoded values; keep the pipeline linear until output
}

float3 Grade(float3 c)
{
    c *= g_Post.whiteBalance * g_Post.colorFilter;
    const float l = Luminance(c);
    c = lerp(l.xxx, c, g_Post.saturation);
    // Contrast around mid-grey in log space.
    c = max(c, 0.0);
    c = exp2((log2(c + 1e-6) - log2(0.18)) * g_Post.contrast + log2(0.18));
    return c;
}

/// Color grading LUT on the display-referred (sRGB) image, UE layout: blue picks the slice, red / green the texel.
float3 ApplyColorLut(float3 srgb)
{
    const float size = 16.0;
    const float3 c = saturate(srgb) * (size - 1.0);
    const float s0 = floor(c.b), s1 = min(s0 + 1.0, size - 1.0);
    const float2 inSlice = float2(c.r + 0.5, c.g + 0.5);
    Texture2D<float4> lut = GetTexture2D(g_Post.lut);
    // The LUT is stored as a color texture (decoded to linear on sampling): back to display values.
    const float3 a = LinearToSRGB(lut.SampleLevel(g_LinearClamp, float2((s0 * size + inSlice.x) / (size * size), inSlice.y / size), 0).rgb);
    const float3 b = LinearToSRGB(lut.SampleLevel(g_LinearClamp, float2((s1 * size + inSlice.x) / (size * size), inSlice.y / size), 0).rgb);
    return lerp(srgb, lerp(a, b, c.b - s0), g_Post.lutIntensity);
}

float3 HeatMap(float t)
{
    return saturate(float3(t * 3.0 - 1.5, t < 0.5 ? t * 2.0 : 2.0 - t * 2.0, 1.5 - t * 3.0));
}

// Output: display-encoded (sRGB) color, luma in alpha for FXAA.
float4 TonemapPS(VSOutput i) : SV_Target0
{
    float3 hdr = SampleInput(i.uv);
    if (g_Post.bloom != ZX_INVALID_INDEX)
        hdr = lerp(hdr, GetTexture2D(g_Post.bloom).SampleLevel(g_LinearClamp, i.uv, 0).rgb, g_Post.bloomIntensity);
    float3 color;
    if (g_Post.viewMode == 1 || g_Post.viewMode == 5) {
        color = saturate(hdr);
    } else if (g_Post.viewMode == 2) {
        const float d = GetTexture2D(g_Post.depth).SampleLevel(g_PointClamp, i.uv, 0).r;
        color = saturate(log2(0.05 / max(d, 1e-6)) / 8.0).xxx;
    } else if (g_Post.viewMode == 3) {
        const float ev = log2(max(Luminance(hdr) / g_Post.exposure, 1e-4) * 100.0 / 12.5);
        color = HeatMap(saturate((ev + 2.0) / 20.0));
    } else if (g_Post.viewMode == 4) {
        color = g_Post.bloom != ZX_INVALID_INDEX ? GetTexture2D(g_Post.bloom).SampleLevel(g_LinearClamp, i.uv, 0).rgb : 0;
    } else {
        hdr = Grade(hdr);
        color = g_Post.tonemapper == 1 ? TonemapAgX(hdr) : g_Post.tonemapper == 2 ? TonemapHable(hdr) : TonemapACES(hdr);
        // Vignette.
        const float2 d = i.uv - 0.5;
        color *= saturate(1.0 - dot(d, d) * g_Post.vignette * 2.0);
    }
    float3 srgb = LinearToSRGB(saturate(color));
    if (g_Post.lut != ZX_INVALID_INDEX && g_Post.viewMode == 0)
        srgb = ApplyColorLut(srgb);
    return float4(srgb, dot(srgb, float3(0.299, 0.587, 0.114)));
}

// --- FXAA (own compact implementation of the 3.11 quality algorithm) + upscale ------------------------------------

float4 SampleLdr(float2 uv) { return GetTexture2D(g_Post.input).SampleLevel(g_LinearClamp, uv, 0); }
float LumaAt(float2 uv) { return SampleLdr(uv).a; }

float4 FxaaPS(VSOutput i) : SV_Target0
{
    const float2 t = g_Post.inputTexel;
    const float2 uv = i.uv;
    const float4 center = SampleLdr(uv);
    const float lumaM = center.a;
    const float lumaN = LumaAt(uv + float2(0, -t.y)), lumaS = LumaAt(uv + float2(0, t.y));
    const float lumaW = LumaAt(uv + float2(-t.x, 0)), lumaE = LumaAt(uv + float2(t.x, 0));
    const float rangeMax = max(lumaM, max(max(lumaN, lumaS), max(lumaW, lumaE)));
    const float rangeMin = min(lumaM, min(min(lumaN, lumaS), min(lumaW, lumaE)));
    const float range = rangeMax - rangeMin;
    if (g_Post.viewMode != 0 || range < max(0.0312, rangeMax * 0.125))
        return float4(center.rgb, 1);

    const float lumaNW = LumaAt(uv + float2(-t.x, -t.y)), lumaNE = LumaAt(uv + float2(t.x, -t.y));
    const float lumaSW = LumaAt(uv + float2(-t.x, t.y)), lumaSE = LumaAt(uv + float2(t.x, t.y));
    const float edgeH = abs(lumaNW + lumaNE - 2.0 * lumaN) + 2.0 * abs(lumaW + lumaE - 2.0 * lumaM) + abs(lumaSW + lumaSE - 2.0 * lumaS);
    const float edgeV = abs(lumaNW + lumaSW - 2.0 * lumaW) + 2.0 * abs(lumaN + lumaS - 2.0 * lumaM) + abs(lumaNE + lumaSE - 2.0 * lumaE);
    const bool horizontal = edgeH >= edgeV;

    float lumaNeg = horizontal ? lumaN : lumaW, lumaPos = horizontal ? lumaS : lumaE;
    const float gradNeg = abs(lumaNeg - lumaM), gradPos = abs(lumaPos - lumaM);
    float stepLen = horizontal ? t.y : t.x;
    float lumaLocal;
    float gradient;
    if (gradNeg >= gradPos) {
        stepLen = -stepLen;
        lumaLocal = (lumaNeg + lumaM) * 0.5;
        gradient = gradNeg;
    } else {
        lumaLocal = (lumaPos + lumaM) * 0.5;
        gradient = gradPos;
    }
    float2 edgeUV = uv;
    if (horizontal)
        edgeUV.y += stepLen * 0.5;
    else
        edgeUV.x += stepLen * 0.5;
    const float2 dir = horizontal ? float2(t.x, 0) : float2(0, t.y);
    const float threshold = gradient * 0.25;
    static const float steps[10] = {1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 4.0, 8.0, 8.0};
    float2 uvN = edgeUV - dir, uvP = edgeUV + dir;
    float endN = LumaAt(uvN) - lumaLocal, endP = LumaAt(uvP) - lumaLocal;
    bool doneN = abs(endN) >= threshold, doneP = abs(endP) >= threshold;
    [unroll] for (uint s = 1; s < 10; ++s) {
        if (doneN && doneP)
            break;
        if (!doneN) {
            uvN -= dir * steps[s];
            endN = LumaAt(uvN) - lumaLocal;
            doneN = abs(endN) >= threshold;
        }
        if (!doneP) {
            uvP += dir * steps[s];
            endP = LumaAt(uvP) - lumaLocal;
            doneP = abs(endP) >= threshold;
        }
    }
    const float distN = horizontal ? uv.x - uvN.x : uv.y - uvN.y;
    const float distP = horizontal ? uvP.x - uv.x : uvP.y - uv.y;
    const bool closerN = distN < distP;
    const float dist = min(distN, distP);
    const float span = distN + distP;
    const bool centerSmaller = lumaM < lumaLocal;
    const bool correct = ((closerN ? endN : endP) < 0.0) != centerSmaller;
    float offset = correct ? -dist / span + 0.5 : 0.0;
    // Subpixel aliasing.
    const float avg = (2.0 * (lumaN + lumaS + lumaW + lumaE) + lumaNW + lumaNE + lumaSW + lumaSE) / 12.0;
    const float sub = saturate(abs(avg - lumaM) / range);
    const float subOffset = (-2.0 * sub + 3.0) * sub * sub;
    offset = max(offset, subOffset * subOffset * 0.75);
    float2 finalUV = uv;
    if (horizontal)
        finalUV.y += offset * stepLen;
    else
        finalUV.x += offset * stepLen;
    return float4(SampleLdr(finalUV).rgb, 1);
}
