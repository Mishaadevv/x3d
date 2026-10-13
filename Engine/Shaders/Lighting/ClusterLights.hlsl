// Light clustering for Forward+: the view frustum is split into 16 x 9 screen tiles x 24 exponential depth slices;
// every cluster gets the list of point/spot lights whose bounding sphere touches its view-space AABB.
// Output per cluster (256 bytes): uint count, uint indices[63].

#include "Common/Bindless.hlsli"
#include "Common/Lights.hlsli"

struct ClusterParams
{
    float4x4 view; // world -> view
    uint lightBuffer;
    uint clusterBuffer;
    uint lightCount;
    float nearZ;
    float farZ;
    float tanHalfX; // tan(fovX / 2)
    float tanHalfY;
    float pad;
};
ZX_PASS_CONSTANTS(ClusterParams, g_Params);

float3 ViewRay(float2 ndc)
{
    return float3(ndc.x * g_Params.tanHalfX, ndc.y * g_Params.tanHalfY, 1.0);
}

[numthreads(64, 1, 1)]
void CSMain(uint id : SV_DispatchThreadID)
{
    const uint total = ZX_CLUSTER_GRID_X * ZX_CLUSTER_GRID_Y * ZX_CLUSTER_GRID_Z;
    if (id >= total)
        return;
    const uint3 c = uint3(id % ZX_CLUSTER_GRID_X, (id / ZX_CLUSTER_GRID_X) % ZX_CLUSTER_GRID_Y,
                          id / (ZX_CLUSTER_GRID_X * ZX_CLUSTER_GRID_Y));
    // Tile in NDC (y up; tile row 0 is the top of the screen).
    const float2 ndcMin = float2(float(c.x) / ZX_CLUSTER_GRID_X * 2.0 - 1.0, 1.0 - float(c.y + 1) / ZX_CLUSTER_GRID_Y * 2.0);
    const float2 ndcMax = float2(float(c.x + 1) / ZX_CLUSTER_GRID_X * 2.0 - 1.0, 1.0 - float(c.y) / ZX_CLUSTER_GRID_Y * 2.0);
    const float ratio = g_Params.farZ / g_Params.nearZ;
    const float zNear = g_Params.nearZ * pow(ratio, float(c.z) / ZX_CLUSTER_GRID_Z);
    const float zFar = g_Params.nearZ * pow(ratio, float(c.z + 1) / ZX_CLUSTER_GRID_Z);
    float3 bmin = 1e30, bmax = -1e30;
    [unroll] for (uint k = 0; k < 4; ++k) {
        const float3 ray = ViewRay(float2((k & 1) ? ndcMax.x : ndcMin.x, (k & 2) ? ndcMax.y : ndcMin.y));
        bmin = min(bmin, min(ray * zNear, ray * zFar));
        bmax = max(bmax, max(ray * zNear, ray * zFar));
    }

    ByteAddressBuffer lights = GetBuffer(g_Params.lightBuffer);
    RWByteAddressBuffer clusters = GetRWBuffer(g_Params.clusterBuffer);
    const uint base = id * (ZX_MAX_LIGHTS_PER_CLUSTER + 1) * 4;
    uint count = 0;
    for (uint i = 0; i < g_Params.lightCount && count < ZX_MAX_LIGHTS_PER_CLUSTER; ++i) {
        const PunctualLight l = LoadLight(lights, i);
        const float3 center = mul(g_Params.view, float4(l.position, 1.0)).xyz;
        // Sphere vs AABB.
        const float3 closest = clamp(center, bmin, bmax);
        const float3 d = closest - center;
        if (dot(d, d) <= l.range * l.range) {
            clusters.Store(base + 4 + count * 4, i);
            ++count;
        }
    }
    clusters.Store(base, count);
}
