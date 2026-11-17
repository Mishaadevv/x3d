// Global distance field of the scene (GlobalIllumination.cpp): up to 4 nested levels of 128^3 voxels around the camera,
// each twice as coarse as the one inside it, stacked along z in three 3D textures:
//   distance  R8      distance to the nearest surface voxel, in voxels / SdfMaxVoxels (saturates)
//   albedo    RGBA8   surface color premultiplied by occupancy, a = occupancy (linear filtering averages surfaces)
//   normal    RGBA8   (normal * 0.5 + 0.5) premultiplied by occupancy, a = occupancy
// Rays march through the finest level that holds them ("sphere tracing": step by the distance).
// Keep GlobalSDF in sync with render::GlobalSdfConstants (GlobalIllumination.cpp).

#ifndef ZX_GLOBAL_SDF_HLSLI
#define ZX_GLOBAL_SDF_HLSLI

#include "Common/Bindless.hlsli"

#define SDF_RESOLUTION 128u
static const float SdfMaxVoxels = 16.0;

struct GlobalSDF
{
    float4 levelMin[4]; // xyz: world min corner, w: voxel size (meters)
    uint distanceTex;
    uint albedoTex;
    uint normalTex;
    uint levels;   // built levels (rays use these)
    uint faces;    // ByteAddressBuffer: one byte per voxel, bits 2a / 2a+1 = a face pointing along +axis a / -axis a
    uint slabs;    // levels the textures hold
    uint pad0;
    uint pad1;
};

/// The finest level that holds `p` with a margin of `margin` voxels, or levels when none does.
uint SdfLevelAt(GlobalSDF s, float3 p, float margin)
{
    [loop] for (uint k = 0; k < s.levels; ++k) {
        const float3 local = (p - s.levelMin[k].xyz) / s.levelMin[k].w;
        if (all(local >= margin) && all(local <= float(SDF_RESOLUTION) - margin))
            return k;
    }
    return s.levels;
}

/// Texture coordinate of `p` in level k (clamped inside the level's slab so filtering never reads the next level).
float3 SdfUVW(GlobalSDF s, uint k, float3 p)
{
    float3 local = (p - s.levelMin[k].xyz) / s.levelMin[k].w;
    local = clamp(local, 0.5, float(SDF_RESOLUTION) - 0.5);
    // The textures hold `slabs` levels even while the outer ones are being rebuilt (levels < slabs).
    return float3(local.xy / float(SDF_RESOLUTION), (local.z + float(k * SDF_RESOLUTION)) / float(SDF_RESOLUTION * max(s.slabs, s.levels)));
}

/// Distance (meters) to the nearest surface at `p` in level k.
float SdfDistance(GlobalSDF s, uint k, float3 p)
{
    const float d = GetTexture3D(s.distanceTex).SampleLevel(g_LinearClamp, SdfUVW(s, k, p), 0).r;
    return d * SdfMaxVoxels * s.levelMin[k].w;
}

struct SdfHit
{
    bool hit;
    float t;        // meters along the ray
    float3 position;
    uint level;
};

/// Sphere-traces a ray from `origin` (unit `dir`) up to `maxT` meters.
SdfHit SdfTrace(GlobalSDF s, float3 origin, float3 dir, float maxT, uint maxSteps)
{
    SdfHit h;
    h.hit = false;
    h.t = maxT;
    h.position = origin + dir * maxT;
    h.level = s.levels;
    float t = 0.0;
    [loop] for (uint i = 0; i < maxSteps && t < maxT; ++i) {
        const float3 p = origin + dir * t;
        const uint k = SdfLevelAt(s, p, 1.0);
        if (k >= s.levels)
            break; // left the field: a miss
        const float voxel = s.levelMin[k].w;
        const float d = SdfDistance(s, k, p);
        if (d < 0.35 * voxel) {
            h.hit = true;
            h.t = t;
            h.position = p;
            h.level = k;
            return h;
        }
        t += max(d - 0.25 * voxel, 0.35 * voxel);
    }
    return h;
}

/// Soft visibility (1 = open) from `origin` towards unit `dir` (cone of tangent ~ 1 / k).
float SdfShadow(GlobalSDF s, float3 origin, float3 dir, float maxT, uint maxSteps, float k)
{
    float visibility = 1.0;
    float t = 0.0;
    [loop] for (uint i = 0; i < maxSteps && t < maxT; ++i) {
        const float3 p = origin + dir * t;
        const uint level = SdfLevelAt(s, p, 1.0);
        if (level >= s.levels)
            break;
        const float voxel = s.levelMin[level].w;
        const float d = SdfDistance(s, level, p) - 0.3 * voxel;
        if (d <= 0.0)
            return 0.0;
        visibility = min(visibility, saturate(k * d / max(t, 1e-3)));
        t += max(d, 0.4 * voxel);
    }
    return visibility;
}

/// Surface color and normal around a hit (filtered over the occupied voxels).
void SdfSurface(GlobalSDF s, uint k, float3 p, out float3 albedo, out float3 normal)
{
    const float3 uvw = SdfUVW(s, k, p);
    const float4 a = GetTexture3D(s.albedoTex).SampleLevel(g_LinearClamp, uvw, 0);
    const float4 n = GetTexture3D(s.normalTex).SampleLevel(g_LinearClamp, uvw, 0);
    albedo = a.a > 1e-3 ? a.rgb / a.a : 0.5;
    normal = n.a > 1e-3 ? n.rgb / n.a * 2.0 - 1.0 : 0.0;
    const float len = length(normal);
    normal = len > 1e-3 ? normal / len : 0.0;
}

/// Face direction bits of a normal (see GlobalSDF::faces).
uint SdfFaceBits(float3 n)
{
    uint bits = 0;
    [unroll] for (uint a = 0; a < 3; ++a) {
        if (n[a] > 0.3)
            bits |= 1u << (2 * a);
        else if (n[a] < -0.3)
            bits |= 2u << (2 * a);
    }
    return bits;
}

/// Byte address of voxel v of level k in the face buffer.
uint SdfFaceAddress(uint k, uint3 v) { return ((k * SDF_RESOLUTION + v.z) * SDF_RESOLUTION + v.y) * SDF_RESOLUTION + v.x; }

/// Whether a ray travelling along `dir` that stopped at `p` (level k) met only the backs of surfaces (it started inside
/// something). Uses the face directions of the 8 voxels around p: a thin wall holds both of its sides in one voxel,
/// and its single stored normal would make one side look like a back face at random. `n`: the sampled normal
/// (fallback without face data).
bool SdfBackFace(GlobalSDF s, uint k, float3 p, float3 dir, float3 n)
{
    if (s.faces == ZX_INVALID_INDEX)
        return dot(n, dir) > 0.2;
    ByteAddressBuffer faces = GetBuffer(s.faces);
    const int3 base = int3(floor((p - s.levelMin[k].xyz) / s.levelMin[k].w - 0.5));
    uint bits = 0;
    [unroll] for (uint i = 0; i < 8; ++i) {
        const int3 v = base + int3(i & 1, (i >> 1) & 1, (i >> 2) & 1);
        if (any(v < 0) || any(v >= int(SDF_RESOLUTION)))
            continue;
        const uint address = SdfFaceAddress(k, uint3(v));
        bits |= (faces.Load(address & ~3u) >> ((address & 3u) * 8)) & 0xFFu;
    }
    if (bits == 0)
        return dot(n, dir) > 0.2;
    // Faces turned towards the ray: their normal points against its direction.
    uint toward = 0;
    [unroll] for (uint a = 0; a < 3; ++a) {
        if (dir[a] > 0.2)
            toward |= 2u << (2 * a);
        else if (dir[a] < -0.2)
            toward |= 1u << (2 * a);
    }
    return (bits & toward) == 0;
}

#endif // ZX_GLOBAL_SDF_HLSLI
