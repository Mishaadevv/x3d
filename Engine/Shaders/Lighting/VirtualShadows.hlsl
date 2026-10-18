// Virtual shadow maps: marks the pages the visible pixels need (see Common/VirtualShadows.hlsli). One thread per 2x2
// pixels of the depth buffer; requests are one bit per window page per level, copied to the CPU.

#include "Common/Bindless.hlsli"
#include "Common/VirtualShadows.hlsli"

struct MarkParams
{
    float4x4 invViewProj;
    float4 lightRows[3];
    float3 camera;
    float pixelScale;
    float texel0;
    uint levels;
    uint depth;     // scene depth (reversed-Z)
    uint requests;  // RW: levels * 512 words
    uint table;     // level headers (window origins)
    uint2 size;     // depth size
    float maxDistance;
    float4 buildRows[3]; // light space of the generation being built (the sun turned)
    uint building;       // 1: mark its pages too (second half of the requests)
    uint3 pad;
};
ZX_PASS_CONSTANTS(MarkParams, g_Mark);

[numthreads(64, 1, 1)]
void ClearCS(uint3 id : SV_DispatchThreadID)
{
    if (id.x < g_Mark.levels * 512 * 2)
        GetRWBuffer(g_Mark.requests).Store(id.x * 4, 0);
}

/// gen 0: the shown pages, 1: the generation being built (its own window headers and request half).
void Request(uint gen, uint level, float2 l)
{
    const uint4 header = GetBuffer(g_Mark.table).Load4(gen * VsmBuildHeader + level * 16);
    const int2 page = int2(floor(l / asfloat(header.z))) - int2(header.xy);
    if (any(page < 0) || any(page >= int(VsmWindowPages)))
        return;
    const uint bit = uint(page.y) * VsmWindowPages + uint(page.x);
    GetRWBuffer(g_Mark.requests).InterlockedOr(((gen * g_Mark.levels + level) * 512 + bit / 32) * 4, 1u << (bit & 31));
}

void RequestWithNeighbor(uint gen, uint level, float2 l)
{
    Request(gen, level, l);
    // Filter taps and normal offsets can cross into the neighbor page: request it too when close to an edge.
    const float pageWorld = asfloat(GetBuffer(g_Mark.table).Load(gen * VsmBuildHeader + level * 16 + 8));
    const float2 f = frac(l / pageWorld);
    const float2 edge = float2(f.x < 0.02 ? -1 : (f.x > 0.98 ? 1 : 0), f.y < 0.02 ? -1 : (f.y > 0.98 ? 1 : 0));
    if (any(edge != 0))
        Request(gen, level, l + edge * pageWorld * 0.05);
}

[numthreads(8, 8, 1)]
void MarkCS(uint3 id : SV_DispatchThreadID)
{
    const uint2 pixel = min(id.xy * 2 + 1, g_Mark.size - 1);
    if (any(id.xy * 2 >= g_Mark.size))
        return;
    const float z = GetTexture2D(g_Mark.depth).Load(int3(pixel, 0)).r;
    if (z <= 0.0)
        return; // sky
    const float2 ndc = (float2(pixel) + 0.5) / float2(g_Mark.size) * float2(2, -2) + float2(-1, 1);
    const float4 w = mul(g_Mark.invViewProj, float4(ndc, z, 1.0));
    const float3 world = w.xyz / w.w;
    const float distance = length(world - g_Mark.camera);
    if (distance > g_Mark.maxDistance)
        return;
    const uint level = VsmLevel(distance, g_Mark.pixelScale, g_Mark.texel0, g_Mark.levels);
    RequestWithNeighbor(0, level, VsmLightSpace(g_Mark.lightRows, world).xy);
    if (g_Mark.building)
        RequestWithNeighbor(1, level, VsmLightSpace(g_Mark.buildRows, world).xy);
}
