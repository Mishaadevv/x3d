// ZNano virtual geometry (M15): per view, the GPU picks the clusters of every instance's LOD DAG that the screen needs
// and turns them into one index buffer drawn per material bin (no mesh shaders needed):
//   NanoClearCS   - the view's bins start empty (DrawIndexedIndirect records), counters reset.
//   NanoCullCS    - one thread per candidate cluster (the CPU sends each instance's prefix of clusters that can matter):
//                   LOD cut (own error small enough on screen, parent's not), frustum, two-phase HiZ occlusion; a
//                   visible cluster reserves its triangles in its bin and appends a visible record.
//   NanoArgsCS    - bins laid out one after another in the view's index region (prefix sum of their index counts).
//   NanoExpandCS  - one group per visible cluster writes its triangles: index = visible record << 8 | local vertex, so
//                   the vertex cache works inside a cluster; ForwardPBR.hlsl VSMain (ZX_NANO) decodes it.
// Data: GpuMesh::nanoBuffer = NanoClusterHeader[] (80 bytes, Asset/CookedAssets.h) with page-relative offsets
// (vertexStart = vertex in the page, triangleStart = byte in the page, level = page); the pages themselves live in
// RenderAssets' pool, found through the mesh's page table. Only a prefix of each mesh's pages may be resident: a cluster
// whose children aren't is drawn in their place (asset::nano::SelectCutResident).
// The per-frame tables come from SceneRendererNano.cpp (keep the layouts in sync).

#include "Common/Bindless.hlsli"
#include "Common/HiZ.hlsli"

struct NanoParams
{
    float4x4 viewProj;
    float4x4 occlusionViewProj; // the view the HiZ was rendered with
    float4 planes[6];           // frustum planes (xyz normal pointing inside, w distance)
    float3 eye;                 // perspective views: camera position
    float pixelsPerUnit;        // perspective: height / (2 tan(fov / 2)); orthographic: pixels per meter
    float threshold;            // allowed error on screen, pixels
    uint ortho;
    uint mode;                  // 0 frustum only, 1 phase 1 (last frame's HiZ, hidden ones become candidates), 2 phase 2
    uint threadCount;           // threads dispatched (cull: candidate clusters; phase 2 / expand: grid-stride step)
    uint tables;                // this frame's upload buffer
    uint instancesOffset;       // NanoInstance[] (32 bytes)
    uint itemsOffset;           // uint2 (first thread, instance) per instance in this view
    uint itemCount;
    uint sectionBinsOffset;     // uint per (instance's template, section): material bin or invalid
    uint instanceBuffer;        // instance rows (world matrix rows first)
    uint args;                  // RW IndirectDrawArgs per (region, bin)
    uint counters;              // RW per view: visible clusters, indices, overflow
    uint visible;               // RW uint4 per (region, record): instance, cluster, bin, index offset in the bin
    uint indices;               // RW index buffer
    uint candidates;            // RW [0] count, then uint2 (instance, cluster) from 8
    uint hiz;
    uint hizOdd;
    uint hizMips;
    float2 hizSize;
    uint view;                  // counters slot
    uint region;                // args / visible / index region
    uint binCount;
    uint binCapacity;
    uint visibleCapacity;       // records per region
    uint indexCapacity;         // indices of this region
    uint indexBase;             // the region's first index
    uint resetCandidates;
    uint candidateCapacity;
    uint pool;                  // cluster page pool (vertices + packed triangles)
    uint2 pad;
};
ZX_PASS_CONSTANTS(NanoParams, g_Nano);

static const uint HeaderStride = 80, InstanceStride = 32, ArgsStride = 24, CounterStride = 16, RowStride = 192;
static const float RootError = 1.0e38;
static const uint PageBytes = 131040, PageVertices = 3640; // asset::nano::PageBytes

[numthreads(64, 1, 1)]
void NanoClearCS(uint3 id : SV_DispatchThreadID)
{
    if (id.x == 0) {
        RWByteAddressBuffer counters = GetRWBuffer(g_Nano.counters);
        counters.Store4(g_Nano.view * CounterStride, uint4(0, 0, 0, 0));
        if (g_Nano.resetCandidates)
            GetRWBuffer(g_Nano.candidates).Store(0, 0);
    }
    if (id.x >= g_Nano.binCount)
        return;
    // drawId = bin (the vertex shader reads the bin's material), no indices, one instance.
    RWByteAddressBuffer args = GetRWBuffer(g_Nano.args);
    const uint o = (g_Nano.region * g_Nano.binCapacity + id.x) * ArgsStride;
    args.Store3(o, uint3(id.x, 0, 1));
    args.Store3(o + 12, uint3(0, 0, 0));
}

struct Cluster
{
    float4 bounds;
    float4 selfSphere;
    float4 parentSphere;
    float selfError;
    float parentError;
    uint triangleCount;
    uint section;
};

Cluster LoadCluster(ByteAddressBuffer nano, uint index)
{
    const uint o = index * HeaderStride;
    Cluster c;
    c.bounds = asfloat(nano.Load4(o));
    c.selfSphere = asfloat(nano.Load4(o + 16));
    c.parentSphere = asfloat(nano.Load4(o + 32));
    const uint4 e = nano.Load4(o + 48); // selfError, parentError, triangleStart, triangleCount
    c.selfError = asfloat(e.x);
    c.parentError = asfloat(e.y);
    c.triangleCount = e.w;
    c.section = nano.Load(o + 72);
    return c;
}

float3 ToWorld(float3x4 world, float3 p)
{
    return mul(world, float4(p, 1.0));
}

/// The error's size on screen in pixels (the same formula as asset::nano::ProjectedError, in world space).
float Projected(float error, float4 sphere, float3x4 world, float scale)
{
    if (error >= RootError)
        return RootError;
    if (g_Nano.ortho)
        return error * scale * g_Nano.pixelsPerUnit;
    const float d = max(length(ToWorld(world, sphere.xyz) - g_Nano.eye) - sphere.w * scale, 1e-4);
    return error * scale * g_Nano.pixelsPerUnit / d;
}

void Emit(uint inst, uint cluster, uint bin, uint triangleCount)
{
    RWByteAddressBuffer counters = GetRWBuffer(g_Nano.counters);
    const uint c = g_Nano.view * CounterStride;
    uint indexAt;
    counters.InterlockedAdd(c + 4, triangleCount * 3, indexAt);
    if (indexAt + triangleCount * 3 > g_Nano.indexCapacity) {
        counters.InterlockedOr(c + 8, 1); // over budget: the CPU raises the error threshold
        return;
    }
    uint record;
    counters.InterlockedAdd(c, 1, record);
    if (record >= g_Nano.visibleCapacity) {
        counters.InterlockedOr(c + 8, 2);
        return;
    }
    uint offset;
    GetRWBuffer(g_Nano.args).InterlockedAdd((g_Nano.region * g_Nano.binCapacity + bin) * ArgsStride + 4, triangleCount * 3, offset);
    GetRWBuffer(g_Nano.visible).Store4((g_Nano.region * g_Nano.visibleCapacity + record) * 16, uint4(inst, cluster, bin, offset));
}

void Process(uint inst, uint cluster)
{
    ByteAddressBuffer tables = GetBuffer(g_Nano.tables);
    const uint4 r0 = tables.Load4(g_Nano.instancesOffset + inst * InstanceStride);      // row, headers, page table, section bins
    const uint4 r1 = tables.Load4(g_Nano.instancesOffset + inst * InstanceStride + 16); // resident error, resident clusters, -, scale
    const Cluster c = LoadCluster(GetBuffer(r0.y), cluster);
    const uint bin = tables.Load(g_Nano.sectionBinsOffset + (r0.w + c.section) * 4);
    if (bin == ZX_INVALID_INDEX)
        return; // translucent section: drawn by the regular path
    ByteAddressBuffer rows = GetBuffer(g_Nano.instanceBuffer);
    const uint ro = r0.x * RowStride;
    const float3x4 world = float3x4(asfloat(rows.Load4(ro)), asfloat(rows.Load4(ro + 16)), asfloat(rows.Load4(ro + 32)));
    const float scale = asfloat(r1.w);
    const float3 center = ToWorld(world, c.bounds.xyz);
    const float radius = c.bounds.w * scale;
    if (g_Nano.mode != 2) {
        // The cut: this cluster's level is fine enough here, the level that replaces it isn't.
        // A cluster whose children haven't streamed in counts as fine enough.
        const bool selfOk = c.selfError < asfloat(r1.x) || Projected(c.selfError, c.selfSphere, world, scale) <= g_Nano.threshold;
        if (!selfOk || Projected(c.parentError, c.parentSphere, world, scale) <= g_Nano.threshold)
            return;
        [unroll]
        for (uint p = 0; p < 6; ++p)
            if (dot(g_Nano.planes[p].xyz, center) + g_Nano.planes[p].w < -radius)
                return;
    }
    if (g_Nano.mode != 0 &&
        HiZOccluded(center, radius, g_Nano.occlusionViewProj, g_Nano.hiz, g_Nano.hizOdd, g_Nano.hizSize, g_Nano.hizMips)) {
        if (g_Nano.mode == 1) { // maybe visible after all: test again against this frame's depth
            RWByteAddressBuffer cand = GetRWBuffer(g_Nano.candidates);
            uint at;
            cand.InterlockedAdd(0, 1, at);
            if (at < g_Nano.candidateCapacity)
                cand.Store2(8 + at * 8, uint2(inst, cluster));
        }
        return;
    }
    Emit(inst, cluster, bin, c.triangleCount);
}

[numthreads(64, 1, 1)]
void NanoCullCS(uint3 id : SV_DispatchThreadID)
{
    if (g_Nano.mode == 2) {
        // Phase 2: the clusters phase 1 found behind last frame's depth (grid-stride: the count lives on the GPU).
        RWByteAddressBuffer cand = GetRWBuffer(g_Nano.candidates);
        const uint count = min(cand.Load(0), g_Nano.candidateCapacity);
        for (uint i = id.x; i < count; i += g_Nano.threadCount) {
            const uint2 c = cand.Load2(8 + i * 8);
            Process(c.x, c.y);
        }
        return;
    }
    if (id.x >= g_Nano.threadCount || g_Nano.itemCount == 0)
        return;
    // The instance owning this thread: the last item whose first thread is <= id.
    ByteAddressBuffer tables = GetBuffer(g_Nano.tables);
    uint lo = 0, hi = g_Nano.itemCount - 1;
    while (lo < hi) {
        const uint mid = (lo + hi + 1) >> 1;
        if (tables.Load(g_Nano.itemsOffset + mid * 8) <= id.x)
            lo = mid;
        else
            hi = mid - 1;
    }
    const uint2 item = tables.Load2(g_Nano.itemsOffset + lo * 8);
    Process(item.y, id.x - item.x);
}

[numthreads(64, 1, 1)]
void NanoArgsCS(uint3 id : SV_DispatchThreadID)
{
    if (id.x != 0)
        return;
    RWByteAddressBuffer args = GetRWBuffer(g_Nano.args);
    uint start = g_Nano.indexBase;
    for (uint b = 0; b < g_Nano.binCount; ++b) {
        const uint o = (g_Nano.region * g_Nano.binCapacity + b) * ArgsStride;
        args.Store(o + 12, start);
        start += args.Load(o + 4);
    }
}

[numthreads(128, 1, 1)]
void NanoExpandCS(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID)
{
    RWByteAddressBuffer counters = GetRWBuffer(g_Nano.counters);
    const uint count = min(counters.Load(g_Nano.view * CounterStride), g_Nano.visibleCapacity);
    ByteAddressBuffer tables = GetBuffer(g_Nano.tables);
    RWByteAddressBuffer visible = GetRWBuffer(g_Nano.visible);
    RWByteAddressBuffer args = GetRWBuffer(g_Nano.args);
    RWByteAddressBuffer indices = GetRWBuffer(g_Nano.indices);
    for (uint v = group.x; v < count; v += g_Nano.threadCount) {
        const uint globalRecord = g_Nano.region * g_Nano.visibleCapacity + v;
        const uint4 rec = visible.Load4(globalRecord * 16);
        const uint4 r0 = tables.Load4(g_Nano.instancesOffset + rec.x * InstanceStride);
        ByteAddressBuffer headers = GetBuffer(r0.y);
        const uint2 tri = headers.Load2(rec.y * HeaderStride + 56); // byte offset in the page, triangle count
        if (thread.x >= tri.y)
            continue;
        const uint slot = tables.Load(r0.z + headers.Load(rec.y * HeaderStride + 76) * 4);
        const uint packed = GetBuffer(g_Nano.pool).Load(slot * PageBytes + tri.x + thread.x * 4);
        const uint at = args.Load((g_Nano.region * g_Nano.binCapacity + rec.z) * ArgsStride + 12) + rec.w + thread.x * 3;
        const uint base = globalRecord << 8;
        indices.Store3(at * 4, uint3(base | (packed & 0xFF), base | ((packed >> 8) & 0xFF), base | ((packed >> 16) & 0xFF)));
    }
}
