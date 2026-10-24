// Builds one level of the global distance field (Common: GI/GlobalSDF.hlsli):
//   ClearCS      empties the level (albedo, normal) and its seed buffer
//   VoxelizeCS   one thread per triangle of a mesh section: marks the voxels it passes through (conservatively, by
//                its dominant axis), writes their color (base color x texture at a mip that matches the voxel) and
//                normal, and seeds them with their own coordinate
//   JumpFloodCS  jump flooding: each voxel takes the nearest seed among its 27 neighbors `step` voxels away
//                (steps 8, 4, 2, 1: exact enough up to SdfMaxVoxels)
//   FinalizeCS   distance to the nearest seed -> the level's slab of the distance texture

#include "Common/Bindless.hlsli"
#include "Common/Mesh.hlsli"
#include "GI/GlobalSDF.hlsli"

// Keep in sync with render::SdfBuildParams (GlobalIllumination.cpp).
struct BuildParams
{
    float4 worldRows[3];  // object -> world
    float4 levelMin;      // xyz: world min corner of the level, w: voxel size
    uint vertexBuffer;
    uint indexBuffer;
    uint firstIndex;
    uint triangleCount;
    uint index16;
    uint materialBuffer;
    uint materialIndex;   // invalid: plain grey
    uint level;
    uint albedoUav;       // RWTexture3D of all levels (slab level * 128)
    uint normalUav;
    uint seedsUav;        // RWByteAddressBuffer, one uint per voxel of the level being built
    uint seedsInUav;      // jump flooding: the other seed buffer
    uint distanceUav;
    uint step;
    uint maxCells;        // per-triangle work cap
    uint pad;
};
ZX_PASS_CONSTANTS(BuildParams, g_Build);

static const uint EmptySeed = 0xFFFFFFFFu;

uint PackVoxel(uint3 v) { return v.x | (v.y << 7) | (v.z << 14); }
uint3 UnpackVoxel(uint p) { return uint3(p & 127u, (p >> 7) & 127u, (p >> 14) & 127u); }
uint VoxelIndex(uint3 v) { return (v.z * SDF_RESOLUTION + v.y) * SDF_RESOLUTION + v.x; }
uint3 SlabCoord(uint3 v) { return uint3(v.xy, v.z + g_Build.level * SDF_RESOLUTION); }

[numthreads(4, 4, 4)]
void ClearCS(uint3 id : SV_DispatchThreadID)
{
    if (any(id >= SDF_RESOLUTION))
        return;
    GetRWTexture3D(g_Build.albedoUav)[SlabCoord(id)] = 0.0;
    GetRWTexture3D(g_Build.normalUav)[SlabCoord(id)] = 0.0;
    GetRWBuffer(g_Build.seedsUav).Store(VoxelIndex(id) * 4, EmptySeed);
}

uint LoadIndex(ByteAddressBuffer ib, uint i)
{
    if (g_Build.index16 != 0) {
        const uint word = ib.Load((i * 2) & ~3u);
        return (i & 1u) ? (word >> 16) : (word & 0xFFFFu);
    }
    return ib.Load(i * 4);
}

float3 ToWorld(float3 p)
{
    return float3(dot(g_Build.worldRows[0].xyz, p) + g_Build.worldRows[0].w, dot(g_Build.worldRows[1].xyz, p) + g_Build.worldRows[1].w,
                  dot(g_Build.worldRows[2].xyz, p) + g_Build.worldRows[2].w);
}

float3 NormalToWorld(float3 n)
{
    return float3(dot(g_Build.worldRows[0].xyz, n), dot(g_Build.worldRows[1].xyz, n), dot(g_Build.worldRows[2].xyz, n));
}

/// Swizzles so that the axis `axis` comes last.
float3 ToAxis(float3 p, uint axis) { return axis == 0 ? p.yzx : (axis == 1 ? p.zxy : p); }
float3 FromAxis(float3 p, uint axis) { return axis == 0 ? p.zxy : (axis == 1 ? p.yzx : p); }

[numthreads(64, 1, 1)]
void VoxelizeCS(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_Build.triangleCount)
        return;
    ByteAddressBuffer vb = GetBuffer(g_Build.vertexBuffer);
    ByteAddressBuffer ib = GetBuffer(g_Build.indexBuffer);
    const uint i0 = LoadIndex(ib, g_Build.firstIndex + id.x * 3), i1 = LoadIndex(ib, g_Build.firstIndex + id.x * 3 + 1),
               i2 = LoadIndex(ib, g_Build.firstIndex + id.x * 3 + 2);
    const MeshVertex v0 = LoadMeshVertex(vb, i0), v1 = LoadMeshVertex(vb, i1), v2 = LoadMeshVertex(vb, i2);
    const float voxel = g_Build.levelMin.w;
    // Voxel space of the level.
    const float3 a = (ToWorld(v0.position) - g_Build.levelMin.xyz) / voxel;
    const float3 b = (ToWorld(v1.position) - g_Build.levelMin.xyz) / voxel;
    const float3 c = (ToWorld(v2.position) - g_Build.levelMin.xyz) / voxel;
    const float3 lo = min(a, min(b, c)), hi = max(a, max(b, c));
    if (any(hi < 0.0) || any(lo > float(SDF_RESOLUTION)))
        return;
    const float3 faceN = cross(b - a, c - a);
    const float area2 = length(faceN);
    if (area2 < 1e-8)
        return;
    float3 n = NormalToWorld(v0.normal + v1.normal + v2.normal);
    n = dot(n, n) > 1e-8 ? normalize(n) : faceN / area2;

    // Color: base color x the texture at the triangle's center, at the mip whose texels match a voxel.
    float3 albedo = 0.6;
    if (g_Build.materialIndex != ZX_INVALID_INDEX) {
        ByteAddressBuffer mb = GetBuffer(g_Build.materialBuffer);
        const uint o = g_Build.materialIndex * 96;
        albedo = asfloat(mb.Load3(o));
        const uint texture = mb.Load(o + 48);
        if (texture != ZX_INVALID_INDEX) {
            const float rotation = asfloat(mb.Load(o + 72));
            const float4 so = asfloat(mb.Load4(o + 80));
            float2 uv = (v0.uv0 + v1.uv0 + v2.uv0) / 3.0 * so.xy;
            float s, co;
            sincos(rotation, s, co);
            uv = float2(co * uv.x + s * uv.y, -s * uv.x + co * uv.y) + so.zw;
            Texture2D<float4> tex = GetTexture2D(texture);
            float w, h, mips;
            tex.GetDimensions(0, w, h, mips);
            const float uvArea = abs(cross(float3(v1.uv0 - v0.uv0, 0), float3(v2.uv0 - v0.uv0, 0)).z) * so.x * so.y;
            const float texelsPerVoxel = sqrt(uvArea * w * h / max(area2, 1e-8));
            albedo *= tex.SampleLevel(g_LinearWrap, uv, clamp(log2(max(texelsPerVoxel, 1.0)) + 1.0, 0.0, mips - 1.0)).rgb;
        }
    }
    // Dark albedo still bounces a little; clamp so walls don't become perfect mirrors of energy either.
    albedo = clamp(albedo, 0.02, 0.95);

    // Rasterize along the dominant axis: every cell of the projected bounds whose center is within half a cell of the
    // triangle, then the voxels the plane crosses in that cell.
    const float3 an = abs(faceN);
    const uint axis = an.x > an.y && an.x > an.z ? 0u : (an.y > an.z ? 1u : 2u);
    const float3 A = ToAxis(a, axis), B = ToAxis(b, axis), C = ToAxis(c, axis), N = ToAxis(faceN, axis);
    const int2 cmin = max(int2(floor(min(A.xy, min(B.xy, C.xy)))), 0);
    const int2 cmax = min(int2(floor(max(A.xy, max(B.xy, C.xy)))), int(SDF_RESOLUTION) - 1);
    if (any(cmax < cmin))
        return;
    const float sign = N.z >= 0.0 ? 1.0 : -1.0;
    // Edge functions (positive inside), expanded by half a cell along each edge's normal (conservative).
    const float2 e0 = B.xy - A.xy, e1 = C.xy - B.xy, e2 = A.xy - C.xy;
    const float expand0 = 0.5 * (abs(e0.x) + abs(e0.y)), expand1 = 0.5 * (abs(e1.x) + abs(e1.y)), expand2 = 0.5 * (abs(e2.x) + abs(e2.y));
    const float dz = 0.5 * (abs(N.x) + abs(N.y)) / max(abs(N.z), 1e-6);
    const float4 albedoOut = float4(albedo, 1.0);
    const float4 normalOut = float4(n * 0.5 + 0.5, 1.0);
    RWTexture3D<float4> albedoTex = GetRWTexture3D(g_Build.albedoUav);
    RWTexture3D<float4> normalTex = GetRWTexture3D(g_Build.normalUav);
    RWByteAddressBuffer seeds = GetRWBuffer(g_Build.seedsUav);
    uint cells = 0;
    [loop] for (int y = cmin.y; y <= cmax.y; ++y) {
        [loop] for (int x = cmin.x; x <= cmax.x; ++x) {
            if (++cells > g_Build.maxCells)
                return;
            const float2 p = float2(x, y) + 0.5;
            const float w0 = sign * (e0.x * (p.y - A.y) - e0.y * (p.x - A.x)) + expand0;
            const float w1 = sign * (e1.x * (p.y - B.y) - e1.y * (p.x - B.x)) + expand1;
            const float w2 = sign * (e2.x * (p.y - C.y) - e2.y * (p.x - C.x)) + expand2;
            if (w0 < 0.0 || w1 < 0.0 || w2 < 0.0)
                continue;
            // Plane depth at the cell center and its spread over the cell.
            const float z = A.z - (N.x * (p.x - A.x) + N.y * (p.y - A.y)) / (abs(N.z) > 1e-6 ? N.z : 1e-6);
            const int z0 = max(int(floor(z - dz)), 0), z1 = min(int(floor(z + dz)), int(SDF_RESOLUTION) - 1);
            for (int zz = z0; zz <= z1; ++zz) {
                const uint3 v = uint3(FromAxis(float3(x, y, zz), axis));
                albedoTex[SlabCoord(v)] = albedoOut;
                normalTex[SlabCoord(v)] = normalOut;
                seeds.Store(VoxelIndex(v) * 4, PackVoxel(v));
            }
        }
    }
}

[numthreads(4, 4, 4)]
void JumpFloodCS(uint3 id : SV_DispatchThreadID)
{
    if (any(id >= SDF_RESOLUTION))
        return;
    ByteAddressBuffer seedsIn = GetBuffer(g_Build.seedsInUav);
    uint best = seedsIn.Load(VoxelIndex(id) * 4);
    float bestD = 1e9;
    if (best != EmptySeed) {
        const float3 d = float3(UnpackVoxel(best)) - float3(id);
        bestD = dot(d, d);
    }
    const int step = int(g_Build.step);
    [unroll] for (int z = -1; z <= 1; ++z)
        [unroll] for (int y = -1; y <= 1; ++y)
            [unroll] for (int x = -1; x <= 1; ++x) {
                if (x == 0 && y == 0 && z == 0)
                    continue;
                const int3 q = int3(id) + int3(x, y, z) * step;
                if (any(q < 0) || any(q >= int(SDF_RESOLUTION)))
                    continue;
                const uint s = seedsIn.Load(VoxelIndex(uint3(q)) * 4);
                if (s == EmptySeed)
                    continue;
                const float3 d = float3(UnpackVoxel(s)) - float3(id);
                const float dd = dot(d, d);
                if (dd < bestD) {
                    bestD = dd;
                    best = s;
                }
            }
    GetRWBuffer(g_Build.seedsUav).Store(VoxelIndex(id) * 4, best);
}

[numthreads(4, 4, 4)]
void FinalizeCS(uint3 id : SV_DispatchThreadID)
{
    if (any(id >= SDF_RESOLUTION))
        return;
    const uint s = GetBuffer(g_Build.seedsInUav).Load(VoxelIndex(id) * 4);
    // No seed within reach: at least half the range away (jump flooding finds every seed closer than that).
    float d = SdfMaxVoxels * 0.5;
    if (s != EmptySeed)
        d = length(float3(UnpackVoxel(s)) - float3(id));
    GetRWTexture3D(g_Build.distanceUav)[SlabCoord(id)] = saturate(d / SdfMaxVoxels);
}
