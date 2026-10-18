// GPU-driven rendering (SceneRenderer, M12): every frame the CPU uploads one item per (instance, mesh section) - its
// world bounding sphere, instance row and draw group - and these passes decide what gets drawn:
//   InitArgsCS  - indirect draw records of every view region start with zero instances.
//   CullCS      - frustum test, two-phase HiZ occlusion test, LOD pick; visible items append their instance index to
//                 the list of their (group, LOD) slot and bump that slot's instance count (DrawIndexedIndirect).
//   HiZCS       - min-depth pyramid of the current depth (reversed-Z: smaller = farther), mip by mip.
// Layouts mirror SceneRenderer.cpp (GpuCullItem, GpuDrawGroup, GpuDrawSlot, rhi::IndirectDrawArgs).

#include "Common/Bindless.hlsli"
#include "Common/HiZ.hlsli"

struct CullParams
{
    float4x4 viewProj;         // the view being culled
    float4x4 occlusionViewProj; // the view the HiZ was rendered with (last frame for phase 1)
    float4 planes[6];          // frustum planes (xyz normal pointing inside, w distance)
    float3 cameraPosition;
    float invTanHalfFov;
    float lodBias;
    int forcedLod;             // -1 = automatic
    uint itemCount;
    uint mode;                 // 0 frustum only, 1 phase 1 (HiZ of last frame + candidates), 2 phase 2 (candidates)
    uint items;                // GpuCullItem[]
    uint groups;               // GpuDrawGroup[]
    uint slots;                // GpuDrawSlot[]
    uint slotCount;
    uint args;                 // RW IndirectDrawArgs[] (whole buffer, view region at argsBase)
    uint lists;                // RW uint[] (instance indices, view region at listBase)
    uint candidates;           // RW uint[]: [0] = count, then item indices
    uint hiz;                  // min-depth pyramid (even mips) or invalid
    uint viewIndex;            // region of args / lists
    uint listCapacity;         // per view region
    uint hizMips;
    uint requiredFlags;        // item flags that must be set (shadow casters for shadow views)
    float2 hizSize;            // mip 0
    float minRadius;           // world radius below which items are skipped (shadow texels)
    uint lodOffset;            // coarser LODs (shadow views)
    uint hizOdd;               // the odd mip levels live in a second texture (the even ones in `hiz`)
    uint3 pad;
};
ZX_PASS_CONSTANTS(CullParams, g_Cull);

static const uint ItemStride = 32, GroupStride = 32, SlotStride = 32, ArgsStride = 24;

[numthreads(64, 1, 1)]
void InitArgsCS(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_Cull.slotCount)
        return;
    ByteAddressBuffer slots = GetBuffer(g_Cull.slots);
    RWByteAddressBuffer args = GetRWBuffer(g_Cull.args);
    const uint4 s1 = slots.Load4(id.x * SlotStride + 16); // indexCount, firstIndex
    const uint o = (g_Cull.viewIndex * g_Cull.slotCount + id.x) * ArgsStride;
    args.Store3(o, uint3(g_Cull.viewIndex * g_Cull.slotCount + id.x, s1.x, 0));
    args.Store3(o + 12, uint3(s1.y, 0, 0));
    if (id.x == 0 && g_Cull.candidates != ZX_INVALID_INDEX)
        GetRWBuffer(g_Cull.candidates).Store(0, 0);
}

/// Conservative occlusion against the min-depth pyramid: the sphere's screen box and nearest depth.
bool Occluded(float3 center, float radius)
{
    return HiZOccluded(center, radius, g_Cull.occlusionViewProj, g_Cull.hiz, g_Cull.hizOdd, g_Cull.hizSize, g_Cull.hizMips);
}

void Emit(uint itemIndex, uint instance, uint group, float3 center, float radius)
{
    ByteAddressBuffer groups = GetBuffer(g_Cull.groups);
    const uint4 g0 = groups.Load4(group * GroupStride);       // firstSlot, lodCount, flags, pad
    const float4 screen = asfloat(groups.Load4(group * GroupStride + 16)); // lod switch screen sizes (x unused)
    uint lod = 0;
    if (g_Cull.forcedLod >= 0) {
        lod = min(uint(g_Cull.forcedLod), g0.y - 1);
    } else {
        const float distance = length(center - g_Cull.cameraPosition);
        const float screenSize = radius * g_Cull.invTanHalfFov / max(distance - radius, 1e-3);
        [unroll]
        for (uint l = 1; l < 4; ++l)
            if (l < g0.y && screenSize < screen[l] * g_Cull.lodBias)
                lod = l;
    }
    lod = min(lod + g_Cull.lodOffset, g0.y - 1);
    const uint slot = g0.x + lod;
    RWByteAddressBuffer args = GetRWBuffer(g_Cull.args);
    uint index;
    args.InterlockedAdd((g_Cull.viewIndex * g_Cull.slotCount + slot) * ArgsStride + 8, 1, index);
    const uint listOffset = GetBuffer(g_Cull.slots).Load(slot * SlotStride);
    GetRWBuffer(g_Cull.lists).Store((g_Cull.viewIndex * g_Cull.listCapacity + listOffset + index) * 4, instance);
}

[numthreads(64, 1, 1)]
void CullCS(uint3 id : SV_DispatchThreadID)
{
    uint itemIndex = id.x;
    if (g_Cull.mode == 2) {
        // Phase 2: the candidates phase 1 found hidden behind last frame's depth, against this frame's.
        RWByteAddressBuffer cand = GetRWBuffer(g_Cull.candidates);
        if (id.x >= cand.Load(0))
            return;
        itemIndex = cand.Load(4 + id.x * 4);
    } else if (id.x >= g_Cull.itemCount) {
        return;
    }
    ByteAddressBuffer items = GetBuffer(g_Cull.items);
    const float4 sphere = asfloat(items.Load4(itemIndex * ItemStride));
    const uint3 info = items.Load3(itemIndex * ItemStride + 16); // instance, group, flags
    if ((info.z & g_Cull.requiredFlags) != g_Cull.requiredFlags || sphere.w < g_Cull.minRadius)
        return;
    if (g_Cull.mode != 2) {
        [unroll]
        for (uint p = 0; p < 6; ++p)
            if (dot(g_Cull.planes[p].xyz, sphere.xyz) + g_Cull.planes[p].w < -sphere.w)
                return;
    }
    if (g_Cull.mode != 0 && Occluded(sphere.xyz, sphere.w)) {
        if (g_Cull.mode == 1) {
            RWByteAddressBuffer cand = GetRWBuffer(g_Cull.candidates);
            uint at;
            cand.InterlockedAdd(0, 1, at);
            cand.Store(4 + at * 4, itemIndex);
        }
        return;
    }
    Emit(itemIndex, info.x, info.y, sphere.xyz, sphere.w);
}

struct HiZParams
{
    uint source;      // depth (mip 0 pass) or the previous HiZ level (Texture2D<float>)
    uint sourceMip;   // level of `source` to read
    uint target;      // RWTexture2D<float> of the level to write
    uint pad;
    uint2 sourceSize;
    uint2 targetSize;
};
ZX_PUSH_CONSTANTS(HiZParams, g_HiZ);

[numthreads(8, 8, 1)]
void HiZCS(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= g_HiZ.targetSize))
        return;
    Texture2D<float4> src = GetTexture2D(g_HiZ.source);
    // 2x2 footprint, plus the extra row / column when the source size is odd (conservative).
    const uint2 base = id.xy * 2;
    const uint2 last = g_HiZ.sourceSize - 1;
    float d = 1.0;
    const uint ex = (id.x == g_HiZ.targetSize.x - 1 && (g_HiZ.sourceSize.x & 1)) ? 3 : 2;
    const uint ey = (id.y == g_HiZ.targetSize.y - 1 && (g_HiZ.sourceSize.y & 1)) ? 3 : 2;
    for (uint y = 0; y < ey; ++y)
        for (uint x = 0; x < ex; ++x)
            d = min(d, src.Load(int3(min(base + uint2(x, y), last), g_HiZ.sourceMip)).x);
    RWTexture2D<float4> dst = GetRWTexture2D(g_HiZ.target);
    dst[id.xy] = float4(d, 0, 0, 0);
}
