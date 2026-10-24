// DDGI probe update (Common/DDGI.hlsli, GlobalIllumination.cpp):
//   TraceCS             one thread per ray: rays leave each probe in a randomly rotated spherical Fibonacci pattern and
//                       march through the global distance field; a hit is lit by the sun (soft SDF shadow), the point
//                       and spot lights, and last frame's probes (light bounces on and on); a miss sees the sky
//   UpdateIrradianceCS  one thread per irradiance texel: cosine-weighted average of the rays, blended with the history
//   UpdateDistanceCS    one thread per distance texel: mean distance / squared distance along the texel's direction
//   AmbientCubeCS       one thread per probe: its irradiance along +-X, +-Y, +-Z into six small 3D textures (slot
//                       order, wrap addressing = world order), premultiplied by "usable" - rays look up bounced light
//                       there with three filtered samples instead of eight probe lookups
// Probes are updated in turns (one of every `probeStride` per frame); probes that just scrolled in update at once.

#include "Common/Bindless.hlsli"
#include "Common/DDGI.hlsli"
#include "Common/Lights.hlsli"
#include "GI/GlobalSDF.hlsli"

// Keep in sync with render::ProbeParams (GlobalIllumination.cpp).
struct ProbeParams
{
    DDGIVolume volume;
    GlobalSDF sdf;
    float4 rotation[3];      // rows: random rotation of the ray pattern this frame
    float3 sunIrradiance;    // pre-exposed sun illuminance x color (0 = no sun)
    uint raysPerProbe;
    float3 dirToSun;
    uint rayBuffer;          // RWByteAddressBuffer: float4 (radiance, distance; < 0 = hit a back face) per ray
    float3 skyIrradianceScale;
    uint skySH;              // != invalid: g_SkySH is bound
    float3 skyColor;
    float ambientIntensity;
    float3 groundColor;
    float ambientFloor;
    int3 prevOriginIndex;
    float hysteresis;
    uint irradianceUav;
    uint distanceUav;
    uint frame;
    uint probeStride;
    float exposureRatio;     // this frame's exposure / last frame's (the history is pre-exposed)
    uint lightBuffer;
    uint lightCount;
    uint historyValid;
    float sunShadowDistance;
    float energy;            // fraction of bounced light kept per bounce (< 1 keeps feedback stable)
    uint cascadeCount;       // the scene's sun cascades (this frame), 0 = none
    float pad;
    float4x4 shadowMatrices[4];
    uint4 shadowMaps;
    uint4 cubeSrv[2];        // ambient cube textures +X -X +Y -Y +Z -Z (SRV; .zw of [1] unused)
    uint4 cubeUav[2];
    uint cubeValid;          // the cubes hold last update's probes
    uint compactStride;      // > 0: group g updates probe g * compactStride + phase (no probe scrolled in this frame)
    uint phase;
    uint probeDataUav;       // RWTexture2D: probe offsets (relocation), written by UpdateIrradianceCS
};
ZX_PASS_CONSTANTS(ProbeParams, g_Probe);

struct SkySH
{
    float4 c[9];
};
ConstantBuffer<SkySH> g_SkySH : register(b2, space0);

/// The sky in the SkyIrradiance convention (also used as the radiance of rays that escape).
float3 SkyLight(float3 n)
{
    const float3 hemi = lerp(g_Probe.groundColor, g_Probe.skyColor, n.y * 0.5 + 0.5) * g_Probe.ambientIntensity;
    if (g_Probe.skySH == ZX_INVALID_INDEX)
        return hemi;
    const float y[9] = {0.282095, 0.488603 * n.y, 0.488603 * n.z, 0.488603 * n.x, 1.092548 * n.x * n.y, 1.092548 * n.y * n.z,
                        0.315392 * (3.0 * n.z * n.z - 1.0), 1.092548 * n.x * n.z, 0.546274 * (n.x * n.x - n.y * n.y)};
    float3 e = 0;
    [unroll] for (uint k = 0; k < 9; ++k)
        e += g_SkySH.c[k].rgb * y[k];
    return max(e, 0.0) * g_Probe.skyIrradianceScale + hemi * g_Probe.ambientFloor;
}

/// Sun visibility at `p` from the scene's cascaded shadow maps (the first cascade that holds it), or -1 outside them.
float CascadeShadow(float3 p)
{
    [loop] for (uint c = 0; c < g_Probe.cascadeCount; ++c) {
        const float4 h = mul(g_Probe.shadowMatrices[c], float4(p, 1.0));
        const float3 uvz = h.xyz / h.w;
        if (all(uvz.xy > 0.01) && all(uvz.xy < 0.99) && uvz.z > 0.0 && uvz.z < 1.0)
            return g_Texture2D[NonUniformResourceIndex(g_Probe.shadowMaps[c])].SampleCmpLevelZero(g_ShadowSampler, uvz.xy, uvz.z);
    }
    return -1.0;
}

uint CubeSrv(uint i) { return i < 4 ? g_Probe.cubeSrv[0][i] : g_Probe.cubeSrv[1][i - 4]; }
uint CubeUav(uint i) { return i < 4 ? g_Probe.cubeUav[0][i] : g_Probe.cubeUav[1][i - 4]; }

/// Bounced light at a ray hit from last update's ambient cubes (`coverage`: 0 outside the grid or without data).
float3 CubeIrradiance(float3 p, float3 n, out float coverage)
{
    coverage = 0.0;
    if (g_Probe.cubeValid == 0)
        return 0.0;
    const DDGIVolume v = g_Probe.volume;
    // Grid of the last update (the cubes' contents).
    const float3 gridPos = p / v.spacing - float3(g_Probe.prevOriginIndex);
    const float3 counts = float3(v.counts);
    const float3 edge = min(gridPos, counts - 1.0 - gridPos);
    const float inside = saturate(min(edge.x, min(edge.y, edge.z)) + 0.5);
    if (inside <= 0.0)
        return 0.0;
    // Slot coordinates (world index mod counts): wrap filtering joins neighbors across the storage seam.
    const float3 uvw = (gridPos + float3(g_Probe.prevOriginIndex) + 0.5) / counts;
    const float3 w = n * n;
    float4 sum = 0.0;
    sum += w.x * GetTexture3D(CubeSrv(n.x >= 0.0 ? 0 : 1)).SampleLevel(g_LinearWrap, uvw, 0);
    sum += w.y * GetTexture3D(CubeSrv(n.y >= 0.0 ? 2 : 3)).SampleLevel(g_LinearWrap, uvw, 0);
    sum += w.z * GetTexture3D(CubeSrv(n.z >= 0.0 ? 4 : 5)).SampleLevel(g_LinearWrap, uvw, 0);
    if (sum.a <= 1e-3)
        return 0.0;
    coverage = inside * saturate(sum.a * 4.0);
    return sum.rgb / sum.a;
}

int3 WorldOfSlot(int3 originIndex, uint3 slot)
{
    const int3 counts = int3(g_Probe.volume.counts);
    return originIndex + DDGIPositiveMod(int3(slot) - originIndex, counts);
}

uint3 SlotOfLinear(uint probeIndex)
{
    const uint3 c = g_Probe.volume.counts;
    return uint3(probeIndex % c.x, (probeIndex / c.x) % c.y, probeIndex / (c.x * c.y));
}

/// Whether the probe in this slot just scrolled in (its world index differs from last frame's).
bool IsNewProbe(uint3 slot)
{
    return g_Probe.historyValid == 0 || any(WorldOfSlot(g_Probe.volume.originIndex, slot) != WorldOfSlot(g_Probe.prevOriginIndex, slot));
}

/// The probe a thread group works on (compact dispatches skip the probes that wait for a later frame).
uint ProbeOfGroup(uint group)
{
    return g_Probe.compactStride > 0 ? group * g_Probe.compactStride + g_Probe.phase : group;
}

bool UpdatesThisFrame(uint probeIndex, uint3 slot)
{
    if (probeIndex >= g_Probe.volume.counts.x * g_Probe.volume.counts.y * g_Probe.volume.counts.z)
        return false;
    return (probeIndex + g_Probe.frame) % max(g_Probe.probeStride, 1u) == 0 || IsNewProbe(slot);
}

float3 RayDirection(uint i)
{
    const float n = float(g_Probe.raysPerProbe);
    const float phi = 6.2831853 * frac(float(i) * 0.6180339887);
    const float z = 1.0 - (2.0 * float(i) + 1.0) / n;
    const float r = sqrt(saturate(1.0 - z * z));
    const float3 d = float3(r * cos(phi), z, r * sin(phi));
    return normalize(float3(dot(g_Probe.rotation[0].xyz, d), dot(g_Probe.rotation[1].xyz, d), dot(g_Probe.rotation[2].xyz, d)));
}

[numthreads(64, 1, 1)]
void TraceCS(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID)
{
    const uint probe = ProbeOfGroup(group.x);
    const uint ray = group.y * 64 + thread.x;
    const uint3 slot = SlotOfLinear(probe);
    if (ray >= g_Probe.raysPerProbe || !UpdatesThisFrame(probe, slot))
        return;
    const DDGIVolume v = g_Probe.volume;
    const float3 origin = DDGIProbePosition(v, WorldOfSlot(v.originIndex, slot)) + (IsNewProbe(slot) ? 0.0 : DDGIProbeOffset(v, slot));
    const float3 dir = RayDirection(ray);
    const SdfHit hit = SdfTrace(g_Probe.sdf, origin, dir, v.maxDistance, 96);
    float3 radiance = 0.0;
    float distance = v.maxDistance;
    if (!hit.hit) {
        radiance = SkyLight(dir);
    } else {
        distance = hit.t;
        float3 albedo, n;
        SdfSurface(g_Probe.sdf, hit.level, hit.position, albedo, n);
        if (dot(n, n) < 0.5)
            n = -dir;
        if (dot(n, dir) > 0.2) {
            // The back of a surface: the probe is inside something (negative = back face; relocation moves it out).
            distance = -hit.t;
        } else {
            const float voxel = g_Probe.sdf.levelMin[hit.level].w;
            const float3 p = hit.position + n * voxel * 0.75;
            float3 direct = 0.0;
            const float ndl = dot(n, g_Probe.dirToSun);
            if (ndl > 0.0 && any(g_Probe.sunIrradiance > 0.0)) {
                // The scene's shadow maps where they reach (one lookup), else a soft ray through the distance field.
                float shadow = CascadeShadow(p);
                if (shadow < 0.0)
                    shadow = SdfShadow(g_Probe.sdf, p, g_Probe.dirToSun, g_Probe.sunShadowDistance, 40, 6.0);
                direct += g_Probe.sunIrradiance * ndl * shadow;
            }
            // Point / spot lights (unshadowed: they are short-ranged).
            if (g_Probe.lightCount > 0) {
                ByteAddressBuffer lights = GetBuffer(g_Probe.lightBuffer);
                [loop] for (uint k = 0; k < g_Probe.lightCount; ++k) {
                    const PunctualLight l = LoadLight(lights, k);
                    const float3 toLight = l.position - hit.position;
                    const float d2 = dot(toLight, toLight);
                    if (d2 > l.range * l.range)
                        continue;
                    const float3 L = toLight * rsqrt(max(d2, 1e-6));
                    direct += l.color * (DistanceAttenuation(d2, l.range) * SpotAttenuation(l, L) * saturate(dot(n, L)));
                }
            }
            // Bounced light from last frame's probes; beyond the grid, the open sky.
            float coverage;
            const float3 bounce = CubeIrradiance(hit.position + n * v.normalBias, n, coverage);
            const float3 indirect = lerp(SkyLight(n), bounce, coverage);
            radiance = albedo * (direct / 3.14159265 + indirect * g_Probe.energy);
        }
    }
    GetRWBuffer(g_Probe.rayBuffer).Store4((probe * g_Probe.raysPerProbe + ray) * 16, asuint(float4(radiance, distance)));
}

/// Interior texel a border texel copies (octahedral wrap), for tiles of `texels` (1-texel border).
uint2 BorderSource(uint2 t, uint texels)
{
    const uint last = texels - 1;
    const bool bx = t.x == 0 || t.x == last, by = t.y == 0 || t.y == last;
    if (bx && by)
        return uint2(t.x == 0 ? last - 1 : 1, t.y == 0 ? last - 1 : 1);
    if (by)
        return uint2(last - t.x, t.y == 0 ? 1 : last - 1);
    if (bx)
        return uint2(t.x == 0 ? 1 : last - 1, last - t.y);
    return t;
}

float3 TexelDirection(uint2 t, uint texels)
{
    const uint2 s = BorderSource(t, texels);
    return DDGIOctDecode((float2(s) - 0.5) / float(texels - 2) * 2.0 - 1.0);
}

float BlendWeight(uint3 slot)
{
    return IsNewProbe(slot) ? 0.0 : g_Probe.hysteresis;
}

#define MAX_RAYS 256
groupshared float4 s_ray[MAX_RAYS]; // radiance, distance
groupshared float3 s_dir[MAX_RAYS];

/// Loads the probe's rays into group memory (each thread a share): the texel loops then read them from there.
void LoadRays(uint probe, uint thread, uint threads)
{
    ByteAddressBuffer rays = GetBuffer(g_Probe.rayBuffer);
    for (uint i = thread; i < g_Probe.raysPerProbe; i += threads) {
        s_ray[i] = asfloat(rays.Load4((probe * g_Probe.raysPerProbe + i) * 16));
        s_dir[i] = RayDirection(i);
    }
    GroupMemoryBarrierWithGroupSync();
}

/// Moves a probe off its grid point (RTXGI-style relocation, from this update's rays): out through the nearest back face
/// when it is inside geometry, away from a surface that is too close; never more than 45 % of the spacing.
void RelocateProbe(uint3 slot)
{
    const DDGIVolume v = g_Probe.volume;
    const float spacing = min(v.spacing.x, min(v.spacing.y, v.spacing.z));
    const float minFront = 0.25 * spacing; // keep this far from surfaces
    uint backfaces = 0;
    float closestBack = 1e9, closestFront = 1e9;
    float3 backDir = 0.0, frontDir = 0.0;
    for (uint i = 0; i < g_Probe.raysPerProbe; ++i) {
        const float d = s_ray[i].w;
        if (d < 0.0) {
            ++backfaces;
            if (-d < closestBack) {
                closestBack = -d;
                backDir = s_dir[i];
            }
        } else if (d < closestFront) {
            closestFront = d;
            frontDir = s_dir[i];
        }
    }
    RWTexture2D<float4> data = GetRWTexture2D(g_Probe.probeDataUav);
    const uint2 texel = uint2(slot.x + slot.y * v.counts.x, slot.z);
    float3 offset = IsNewProbe(slot) ? 0.0 : data[texel].xyz;
    float3 moved = offset;
    if (float(backfaces) > 0.25 * float(g_Probe.raysPerProbe) && closestBack < spacing)
        moved += backDir * (closestBack + minFront * 0.5); // through the wall
    else if (closestFront < minFront)
        moved -= frontDir * min(minFront - closestFront, 0.1 * spacing); // step back from a close surface
    if (all(abs(moved) <= 0.45 * spacing))
        offset = moved;
    data[texel] = float4(offset, 0.0);
}

[numthreads(8, 8, 1)]
void UpdateIrradianceCS(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID, uint flat : SV_GroupIndex)
{
    const uint probe = ProbeOfGroup(group.x);
    const uint3 slot = SlotOfLinear(probe);
    if (!UpdatesThisFrame(probe, slot))
        return; // uniform per group
    LoadRays(probe, flat, 64);
    const DDGIVolume v = g_Probe.volume;
    const float3 texelDir = TexelDirection(thread.xy, DDGI_IRRADIANCE_TEXELS);
    float3 sum = 0.0;
    float weightSum = 0.0;
    uint backfaces = 0;
    [loop] for (uint i = 0; i < g_Probe.raysPerProbe; ++i) {
        const float4 r = s_ray[i];
        if (r.w < 0.0) {
            ++backfaces;
            continue;
        }
        const float w = saturate(dot(texelDir, s_dir[i]));
        sum += r.rgb * w;
        weightSum += w;
    }
    const float3 result = weightSum > 1e-4 ? sum / weightSum : 0.0;
    if (flat == 0 && g_Probe.probeDataUav != ZX_INVALID_INDEX)
        RelocateProbe(slot);
    // A probe whose rays mostly hit back faces sits inside geometry.
    const float usable = float(backfaces) < 0.25 * float(g_Probe.raysPerProbe) ? 1.0 : 0.0;
    const uint2 texel = DDGITile(v, slot, DDGI_IRRADIANCE_TEXELS) + thread.xy;
    RWTexture2D<float4> tex = GetRWTexture2D(g_Probe.irradianceUav);
    float h = BlendWeight(slot);
    // (No history read for fresh probes: the texture may hold anything.)
    const float3 history = h > 0.0 ? tex[texel].rgb * g_Probe.exposureRatio : result;
    // Big changes (a light switched on, the sun moved a lot) converge faster.
    const float change = max(abs(result.r - history.r), max(abs(result.g - history.g), abs(result.b - history.b)));
    const float scale = max(max(result.r, max(result.g, result.b)), max(history.r, max(history.g, history.b)));
    if (change > 0.5 * scale)
        h = min(h, 0.8);
    tex[texel] = float4(lerp(result, history, h), usable);
}

[numthreads(16, 16, 1)]
void UpdateDistanceCS(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID, uint flat : SV_GroupIndex)
{
    const uint probe = ProbeOfGroup(group.x);
    const uint3 slot = SlotOfLinear(probe);
    if (!UpdatesThisFrame(probe, slot))
        return; // uniform per group
    LoadRays(probe, flat, 256);
    const DDGIVolume v = g_Probe.volume;
    const float3 texelDir = TexelDirection(thread.xy, DDGI_DISTANCE_TEXELS);
    float2 sum = 0.0;
    float weightSum = 0.0;
    [loop] for (uint i = 0; i < g_Probe.raysPerProbe; ++i) {
        const float c = saturate(dot(texelDir, s_dir[i]));
        if (c < 0.5) // pow(c, 16) < 2e-5
            continue;
        float w = c * c;
        w *= w;
        w *= w;
        w *= w; // c^16
        // Back faces count as close walls (the probe looks walled in until relocation moves it out).
        const float d = min(s_ray[i].w < 0.0 ? -s_ray[i].w * 0.2 : s_ray[i].w, v.maxDistance);
        sum += float2(d, d * d) * w;
        weightSum += w;
    }
    if (weightSum <= 1e-5)
        return;
    const uint2 texel = DDGITile(v, slot, DDGI_DISTANCE_TEXELS) + thread.xy;
    RWTexture2D<float4> tex = GetRWTexture2D(g_Probe.distanceUav);
    const float h = BlendWeight(slot);
    const float2 result = sum / weightSum;
    tex[texel] = float4(h > 0.0 ? lerp(result, tex[texel].rg, h) : result, 0, 0);
}

[numthreads(64, 1, 1)]
void AmbientCubeCS(uint3 id : SV_DispatchThreadID)
{
    const DDGIVolume v = g_Probe.volume;
    const uint probes = v.counts.x * v.counts.y * v.counts.z;
    if (id.x >= probes * 6)
        return;
    const uint probe = id.x / 6, axis = id.x % 6;
    const uint3 slot = SlotOfLinear(probe);
    const float3 dirs[6] = {float3(1, 0, 0), float3(-1, 0, 0), float3(0, 1, 0), float3(0, -1, 0), float3(0, 0, 1), float3(0, 0, -1)};
    const float4 irr = GetTexture2D(v.irradiance).SampleLevel(g_LinearClamp, DDGIProbeUV(v, slot, dirs[axis], DDGI_IRRADIANCE_TEXELS, v.invIrradianceSize), 0);
    g_RWTexture3D[NonUniformResourceIndex(CubeUav(axis))][slot] = float4(irr.rgb * irr.a, irr.a);
}
