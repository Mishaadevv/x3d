// Dynamic diffuse global illumination (DDGI, Majercik et al. 2019): a grid of light probes around the camera. Each
// probe keeps the light arriving from every direction (irradiance, 8 x 8 octahedral texels: 6 x 6 + a 1-texel border)
// and how far it can see (mean distance and its square, 16 x 16: 14 x 14 + border). A surface blends its 8 surrounding
// probes, weighted by trilinear position, by whether it faces the probe, and by a Chebyshev test against the probe's
// distances - light does not leak through walls. GlobalIllumination.cpp updates the probes (rays through the global
// signed distance field, Shaders/GI/DDGI.hlsl).
//
// Probes live at world positions index * spacing; a probe with world index w is stored in the slot w mod counts, so the
// grid scrolls with the camera without moving data (slots that change probe start over). Irradiance is stored
// pre-exposed in the convention of SkyIrradiance: the diffuse light of a surface is albedo * value.
// Keep DDGIVolume in sync with render::GIVolumeConstants (GlobalIllumination.h).

#ifndef ZX_DDGI_HLSLI
#define ZX_DDGI_HLSLI

#include "Common/Bindless.hlsli"

#define DDGI_IRRADIANCE_TEXELS 8u  // per probe, with the border
#define DDGI_DISTANCE_TEXELS 16u

struct DDGIVolume
{
    int3 originIndex;   // world index of the grid's first probe
    uint irradiance;    // Texture2D SRV (rgb, a = 1 probe usable / 0 inside geometry), invalid = no GI
    float3 spacing;     // meters between probes
    uint distance;      // Texture2D SRV (mean distance, mean squared)
    uint3 counts;       // probes per axis
    float intensity;    // multiplies the indirect light
    float normalBias;   // meters along the surface normal before looking up probes
    float viewBias;     // meters towards the camera
    float2 invIrradianceSize;
    float2 invDistanceSize;
    float maxDistance;  // meters a probe's rays travel
    uint probeData;     // Texture2D SRV, one texel per slot (x + y * countX, z): xyz = offset from the grid point (relocation)
};

// --- Octahedral mapping ----------------------------------------------------------------------------------------------

float2 DDGIOctEncode(float3 n)
{
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    float2 e = n.xz;
    if (n.y < 0.0)
        e = (1.0 - abs(n.zx)) * float2(n.x >= 0.0 ? 1.0 : -1.0, n.z >= 0.0 ? 1.0 : -1.0);
    return e; // -1..1
}

float3 DDGIOctDecode(float2 e)
{
    float3 n = float3(e.x, 1.0 - abs(e.x) - abs(e.y), e.y);
    if (n.y < 0.0)
        n.xz = (1.0 - abs(n.zx)) * float2(n.x >= 0.0 ? 1.0 : -1.0, n.z >= 0.0 ? 1.0 : -1.0);
    return normalize(n);
}

// --- Probe addressing ------------------------------------------------------------------------------------------------

int3 DDGIPositiveMod(int3 a, int3 b) { return ((a % b) + b) % b; }

/// Storage slot of the probe with world index `world`.
uint3 DDGISlot(DDGIVolume v, int3 world) { return uint3(DDGIPositiveMod(world, int3(v.counts))); }

/// Top-left texel of a slot's tile (tiles: x + y * countX across, z down).
uint2 DDGITile(DDGIVolume v, uint3 slot, uint texels)
{
    return uint2(slot.x + slot.y * v.counts.x, slot.z) * texels;
}

/// Atlas uv of direction `dir` in a probe's tile (interior texels only).
float2 DDGIProbeUV(DDGIVolume v, uint3 slot, float3 dir, uint texels, float2 invSize)
{
    const float interior = float(texels - 2);
    const float2 local = (DDGIOctEncode(dir) * 0.5 + 0.5) * interior + 1.0;
    return (float2(DDGITile(v, slot, texels)) + local) * invSize;
}

float3 DDGIProbePosition(DDGIVolume v, int3 world) { return float3(world) * v.spacing; }

/// How far the probe in `slot` moved off its grid point (out of walls, away from surfaces).
float3 DDGIProbeOffset(DDGIVolume v, uint3 slot)
{
    if (v.probeData == ZX_INVALID_INDEX)
        return 0.0;
    return GetTexture2D(v.probeData).Load(int3(slot.x + slot.y * v.counts.x, slot.z, 0)).xyz;
}

// --- Irradiance lookup -----------------------------------------------------------------------------------------------

/// Indirect diffuse light at `worldPos` with normal `n` (`toCamera`: unit vector towards the viewer). `coverage`: 1
/// well inside the grid, fading to 0 within a probe spacing of its edge (blend with a fallback by it). Without
/// `visibility` the distance maps are not consulted (cheaper; for secondary bounces).
float3 DDGIIrradianceEx(DDGIVolume v, float3 worldPos, float3 n, float3 toCamera, bool visibility, out float coverage)
{
    coverage = 0.0;
    if (v.irradiance == ZX_INVALID_INDEX)
        return 0.0;
    const float3 biased = worldPos + n * v.normalBias + toCamera * v.viewBias;
    const float3 gridPos = biased / v.spacing - float3(v.originIndex);
    const float3 counts = float3(v.counts);
    // Fade out within one spacing of the grid's outer probes.
    const float3 edge = min(gridPos, counts - 1.0 - gridPos);
    coverage = saturate(min(edge.x, min(edge.y, edge.z)));
    if (coverage <= 0.0)
        return 0.0;
    const int3 base = clamp(int3(floor(gridPos)), 0, int3(v.counts) - 2);
    const float3 alpha = saturate(gridPos - float3(base));
    Texture2D<float4> irradianceTex = GetTexture2D(v.irradiance);
    Texture2D<float4> distanceTex = GetTexture2D(v.distance);
    float3 sum = 0.0;
    float weightSum = 0.0;
    [unroll] for (uint i = 0; i < 8; ++i) {
        const int3 offset = int3(i & 1, (i >> 1) & 1, (i >> 2) & 1);
        const int3 world = v.originIndex + base + offset;
        const uint3 slot = DDGISlot(v, world);
        const float3 probe = DDGIProbePosition(v, world) + DDGIProbeOffset(v, slot);
        const float3 trilinear = lerp(1.0 - alpha, alpha, float3(offset));
        float weight = trilinear.x * trilinear.y * trilinear.z;
        // Facing the probe (smooth backface term, never quite zero).
        const float3 toProbe = normalize(probe - worldPos);
        const float facing = (dot(toProbe, n) + 1.0) * 0.5;
        weight *= facing * facing + 0.2;
        // Visibility: Chebyshev test of the biased point against the probe's distances in its direction.
        if (visibility) {
            const float3 probeToPoint = biased - probe;
            const float d = length(probeToPoint);
            const float2 moments = distanceTex.SampleLevel(g_LinearClamp, DDGIProbeUV(v, slot, probeToPoint / max(d, 1e-4), DDGI_DISTANCE_TEXELS, v.invDistanceSize), 0).rg;
            if (d > moments.x) {
                const float variance = abs(moments.x * moments.x - moments.y);
                const float delta = d - moments.x;
                float chebyshev = variance / (variance + delta * delta);
                chebyshev = chebyshev * chebyshev * chebyshev;
                weight *= max(chebyshev, 0.05);
            }
        }
        // Crush tiny weights (keeps dark corners from being lifted by far probes).
        weight = max(weight, 1e-6);
        if (weight < 0.2)
            weight *= weight * weight / 0.04;
        const float4 irr = irradianceTex.SampleLevel(g_LinearClamp, DDGIProbeUV(v, slot, n, DDGI_IRRADIANCE_TEXELS, v.invIrradianceSize), 0);
        weight *= irr.a; // probes stuck inside geometry don't count
        sum += irr.rgb * weight;
        weightSum += weight;
    }
    if (weightSum <= 1e-5) {
        coverage = 0.0;
        return 0.0;
    }
    return sum / weightSum * v.intensity;
}

float3 DDGIIrradiance(DDGIVolume v, float3 worldPos, float3 n, float3 toCamera, out float coverage)
{
    return DDGIIrradianceEx(v, worldPos, n, toCamera, true, coverage);
}

#endif // ZX_DDGI_HLSLI
