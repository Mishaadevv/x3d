// Volumetric clouds (see Render/Clouds.h): a layer between `bottom` and `bottom + thickness` meters above the ground,
// shaped by a tiling 3D Perlin-Worley noise (made on the CPU at startup) (coverage cuts it into clouds, a finer Worley noise erodes the edges) that
// drifts with the wind. Raymarched with a per-frame jitter and blended with the reprojected last
// frame (quarter resolution); lit by the sun (a short march towards it: self-shadowing, Beer-powder, two-lobe phase) and the sky above.
// A top-down transmittance map around the camera gives cloud shadows on the ground and in the volumetric fog.

#include "Common/Bindless.hlsli"

struct CloudParams
{
    float4x4 invViewProj;    // clip (no jitter) -> world
    float4x4 prevViewProj;
    float3 cameraPosition;
    float bottom;            // m
    float3 dirToSun;
    float thickness;         // m
    float3 sunColor;         // pre-exposed illuminance
    float coverage;
    float3 skyColor;         // pre-exposed sky light from above
    float density;           // 1/m at full
    float3 albedo;
    float detail;
    float2 wind;             // m of drift so far (xz)
    float2 outputSize;       // half-resolution target
    uint shapeNoise;         // Texture3D (R8)
    uint detailNoise;        // Texture3D (R8)
    uint output;             // RWTexture2D (rgba16f): in-scatter, transmittance
    uint history;            // Texture2D: last frame's result
    uint current;            // Texture2D: this frame's raymarch (temporal pass input)
    uint resolved;           // RWTexture2D: blended result
    uint depthTexture;
    uint frame;
    float historyWeight;     // 0 = none
    float shadowStrength;
    uint shadowOutput;       // RWTexture2D (r16f): transmittance from the sun, top-down
    uint shadowSize;
    float2 shadowOrigin;     // world xz of the map's corner
    float shadowExtent;      // m covered
    float pad;
};
ZX_PASS_CONSTANTS(CloudParams, g_C);

#define PI 3.14159265
#define SHAPE_SCALE (1.0 / 6000.0) // m -> shape noise tiles
#define DETAIL_SCALE (1.0 / 900.0)

float Remap(float v, float lo, float hi, float newLo, float newHi)
{
    return newLo + (v - lo) / max(hi - lo, 1e-4) * (newHi - newLo);
}

// --- Cloud density --------------------------------------------------------------------------------------------------

float HeightFraction(float y) { return saturate((y - g_C.bottom) / g_C.thickness); }

/// Rounded at the bottom, wispy at the top.
float HeightGradient(float h) { return saturate(h * 8.0) * saturate((1.0 - h) * 2.5); }

float CloudDensity(float3 p, bool detail)
{
    const float h = HeightFraction(p.y);
    if (h <= 0.0 || h >= 1.0)
        return 0.0;
    const float3 q = float3(p.x + g_C.wind.x, p.y, p.z + g_C.wind.y);
    const float shape = GetTexture3D(g_C.shapeNoise).SampleLevel(g_LinearWrap, q * SHAPE_SCALE, 0).r;
    // Coverage cuts the noise into clouds; tops lean into the wind.
    float base = saturate(Remap(shape, 1.0 - g_C.coverage * 0.8, 1.0, 0.0, 1.0)) * HeightGradient(h);
    if (base <= 0.0)
        return 0.0;
    if (detail && g_C.detail > 0.0) {
        const float d = GetTexture3D(g_C.detailNoise).SampleLevel(g_LinearWrap, q * DETAIL_SCALE + float3(0, h * 0.3, 0), 0).r;
        base = saturate(Remap(base, d * g_C.detail * lerp(0.6, 1.0, h), 1.0, 0.0, 1.0));
    }
    return base * g_C.density;
}

/// Distances along the ray to where it enters / leaves the layer (flat slab).
bool LayerRange(float3 origin, float3 dir, out float tEnter, out float tExit)
{
    const float top = g_C.bottom + g_C.thickness;
    if (abs(dir.y) < 1e-4) {
        tEnter = 0.0;
        tExit = (origin.y > g_C.bottom && origin.y < top) ? 50000.0 : 0.0;
        return tExit > 0.0;
    }
    const float t0 = (g_C.bottom - origin.y) / dir.y, t1 = (top - origin.y) / dir.y;
    tEnter = max(min(t0, t1), 0.0);
    tExit = max(t0, t1);
    tExit = min(tExit, tEnter + 30000.0); // nearly horizontal rays: cap the marched length
    return tExit > tEnter;
}

float HG(float c, float g) { return (1.0 - g * g) / (4.0 * PI * pow(max(1.0 + g * g - 2.0 * g * c, 1e-4), 1.5)); }

float InterleavedNoise(float2 p, uint frame)
{
    return frac(52.9829189 * frac(dot(p + float(frame % 16) * float2(47.0, 17.0), float2(0.06711056, 0.00583715))));
}

// --- Raymarch (half resolution) ------------------------------------------------------------------------------------

[numthreads(8, 8, 1)]
void CloudsCS(uint3 id : SV_DispatchThreadID)
{
    if (any(float2(id.xy) >= g_C.outputSize))
        return;
    const float2 uv = (float2(id.xy) + 0.5) / g_C.outputSize;
    const float4 p = mul(g_C.invViewProj, float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.5, 1.0));
    const float3 dir = normalize(p.xyz / p.w - g_C.cameraPosition);
    float3 inscatter = 0.0;
    float transmittance = 1.0;
    float tEnter, tExit;
    if (LayerRange(g_C.cameraPosition, dir, tEnter, tExit)) {
        const uint steps = 48;
        const float stepLen = (tExit - tEnter) / float(steps);
        const float cosSun = dot(dir, g_C.dirToSun);
        const float phase = lerp(HG(cosSun, 0.75), HG(cosSun, -0.25), 0.3); // bright silver lining, some back-scatter
        float t = tEnter + stepLen * InterleavedNoise(float2(id.xy), g_C.frame);
        // Empty sky is crossed in double steps with the cheap (shape-only) density; inside clouds, single detailed ones.
        uint emptySteps = 2;
        for (uint s = 0; s < steps * 2 && t < tExit && transmittance > 0.01; ++s) {
            const float3 pos = g_C.cameraPosition + dir * t;
            if (emptySteps > 1) {
                if (CloudDensity(pos, false) <= 0.0) {
                    t += stepLen * 2.0;
                    continue;
                }
                emptySteps = 0;
                t = max(t - stepLen, tEnter); // step back into the edge we just jumped over
                continue;
            }
            const float sigma = CloudDensity(pos, true);
            t += stepLen;
            if (sigma <= 0.0) {
                ++emptySteps;
                continue;
            }
            emptySteps = 0;
            // Light towards the sun: optical depth through the cloud above this point (4 growing steps).
            float sunDepth = 0.0;
            float ls = g_C.thickness * 0.05;
            float3 lp = pos;
            [unroll] for (uint k = 0; k < 4; ++k) {
                lp += g_C.dirToSun * ls;
                sunDepth += CloudDensity(lp, k < 2) * ls;
                ls *= 2.0;
            }
            const float beer = exp(-sunDepth) * 0.7 + exp(-sunDepth * 0.25) * 0.3; // multiple scattering approximation
            const float powder = 1.0 - exp(-sigma * stepLen * 2.0);
            const float h = HeightFraction(pos.y);
            // Many scattering events inside a cloud: lit sides about as bright as a white surface in the sun, brighter
            // (silver lining) looking towards it.
            const float phaseBoost = min(phase * 4.0 * PI, 6.0);
            const float3 lit = g_C.sunColor * (beer * (0.4 + 0.6 * phaseBoost) / PI) * lerp(1.0, powder, 0.6) + g_C.skyColor * lerp(0.4, 1.0, h);
            const float tStep = exp(-sigma * stepLen);
            inscatter += transmittance * g_C.albedo * lit * (1.0 - tStep);
            transmittance *= tStep;
        }
        // Fade into the haze towards the horizon (far clouds sit in the atmosphere).
        const float fade = saturate((dir.y - 0.01) * 12.0);
        inscatter *= fade;
        transmittance = lerp(1.0, transmittance, fade);
    }
    GetRWTexture2D(g_C.output)[id.xy] = float4(min(inscatter, 60000.0), transmittance);
}

[numthreads(8, 8, 1)]
void TemporalCS(uint3 id : SV_DispatchThreadID)
{
    if (any(float2(id.xy) >= g_C.outputSize))
        return;
    const float4 cur = GetTexture2D(g_C.current).Load(int3(id.xy, 0));
    float4 result = cur;
    if (g_C.historyWeight > 0.0) {
        // Clouds are far away: reproject by direction.
        const float2 uv = (float2(id.xy) + 0.5) / g_C.outputSize;
        const float4 p = mul(g_C.invViewProj, float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.5, 1.0));
        const float3 dir = normalize(p.xyz / p.w - g_C.cameraPosition);
        const float4 prev = mul(g_C.prevViewProj, float4(g_C.cameraPosition + dir * 10000.0, 1.0));
        const float2 prevUv = float2(prev.x / prev.w * 0.5 + 0.5, 0.5 - prev.y / prev.w * 0.5);
        if (prev.w > 0.0 && all(prevUv >= 0.0) && all(prevUv <= 1.0)) {
            // Neighbourhood clamp keeps moving cloud edges from ghosting.
            float4 lo = cur, hi = cur;
            [unroll] for (int y = -1; y <= 1; ++y)
                [unroll] for (int x = -1; x <= 1; ++x) {
                    const float4 n = GetTexture2D(g_C.current).Load(int3(clamp(int2(id.xy) + int2(x, y), 0, int2(g_C.outputSize) - 1), 0));
                    lo = min(lo, n);
                    hi = max(hi, n);
                }
            const float4 h = clamp(GetTexture2D(g_C.history).SampleLevel(g_LinearClamp, prevUv, 0), lo, hi);
            result = lerp(cur, h, g_C.historyWeight);
        }
    }
    GetRWTexture2D(g_C.resolved)[id.xy] = result;
}

// --- Cloud shadows (top-down transmittance towards the sun) --------------------------------------------------------

[numthreads(8, 8, 1)]
void ShadowCS(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= g_C.shadowSize))
        return;
    const float2 xz = g_C.shadowOrigin + (float2(id.xy) + 0.5) / float(g_C.shadowSize) * g_C.shadowExtent;
    // From the ground point towards the sun through the layer.
    const float3 ground = float3(xz.x, 0.0, xz.y);
    float tEnter, tExit;
    float depth = 0.0;
    if (g_C.dirToSun.y > 0.02 && LayerRange(ground, g_C.dirToSun, tEnter, tExit)) {
        const float stepLen = (tExit - tEnter) / 12.0;
        float t = tEnter + stepLen * 0.5;
        for (uint s = 0; s < 12; ++s, t += stepLen)
            depth += CloudDensity(ground + g_C.dirToSun * t, false) * stepLen;
    }
    const float T = exp(-depth);
    GetRWTexture2D(g_C.shadowOutput)[id.xy] = float4(lerp(1.0, T, g_C.shadowStrength).xxx, 1.0);
}

// --- Composite over the sky ----------------------------------------------------------------------------------------

struct CompositeOut
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

CompositeOut CompositeVS(uint vertexId : SV_VertexID)
{
    CompositeOut o;
    o.uv = float2((vertexId << 1) & 2, vertexId & 2);
    o.position = float4(o.uv * float2(2, -2) + float2(-1, 1), 0.0, 1.0);
    return o;
}

float4 CompositePS(CompositeOut i) : SV_Target0
{
    // Only where the sky shows (reversed-Z: depth 0 = nothing drawn).
    if (GetTexture2D(g_C.depthTexture).Load(int3(i.position.xy, 0)).r > 0.0)
        discard;
    const float4 c = GetTexture2D(g_C.history).SampleLevel(g_LinearClamp, i.uv, 0);
    return float4(c.rgb, c.a); // blend: src One, dst SrcAlpha
}
