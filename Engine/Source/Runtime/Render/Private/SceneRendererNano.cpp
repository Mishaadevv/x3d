// ZNano virtual geometry for SceneRenderer (M15). Instances of meshes cooked with a cluster DAG (Asset/Nano.h) skip the
// per-section items of the GPU-driven path. Per frame the CPU sorts their materials into bins and, per view, sends each
// visible instance's prefix of clusters that can matter at its distance (clusters are sorted by parent error); the GPU
// (Shaders/Forward/Nano.hlsl) cuts the DAG, culls and expands the chosen clusters into an index buffer drawn with one
// DrawIndexedIndirect per (pipeline, shader) bucket. Over the triangle budget the error threshold rises (read back a
// frame or two later), so dense scenes degrade gracefully instead of failing.

#include "Render/SceneRenderer.h"
#include "Core/Containers/Algorithm.h"
#include "Core/Log/Log.h"
#include "Core/Math/Geometry.h"
#include "Core/Profiler/Profiler.h"
#include "Core/String/Name.h"
#include "RHI/ShaderCompiler.h"

#include <bit>

ZX_DEFINE_LOG_CATEGORY_STATIC(LogNano, Info);

namespace zx::render {

using namespace rhi;

namespace {

constexpr u32 NanoInstanceStride = 32, NanoBinStride = 16, NanoCounterStride = 16, NanoRecordStride = 16;
constexpr u32 ExpandGroups = 2048;      // grid-stride over the visible clusters (their count lives on the GPU)
constexpr u32 Phase2Threads = 64 * 1024; // grid-stride over the phase 2 candidates

struct NanoParams // Nano.hlsl
{
    Mat4 viewProj;
    Mat4 occlusionViewProj;
    Vec4 planes[6];
    Vec3 eye;
    f32 pixelsPerUnit;
    f32 threshold;
    u32 ortho;
    u32 mode;
    u32 threadCount;
    u32 tables, instancesOffset, itemsOffset, itemCount;
    u32 sectionBinsOffset, instanceBuffer, args, counters;
    u32 visible, indices, candidates, hiz;
    u32 hizOdd, hizMips;
    Vec2 hizSize;
    u32 view, region, binCount, binCapacity;
    u32 visibleCapacity, indexCapacity, indexBase, resetCandidates;
    u32 candidateCapacity;
    u32 pool;
    u32 vsmTable;
    u32 vsmMapOffset;
    Vec4 vsmRows[3];
    i32 vsmOrigin[2];
    f32 vsmPageWorld;
    u32 vsmPad;
};
static_assert(sizeof(NanoParams) == 432); // HLSL constant buffer layout

struct NanoPush // ForwardPBR.hlsl DrawData under ZX_NANO; the bin (draw id) lands in dword 4
{
    u32 visible = 0, tables = 0, instancesOffset = 0, binsOffset = 0, drawId = 0, pool = 0;
    u32 vsmTable = 0, vsmPages = 0; // virtual shadow pages (ZX_VSM)
};
static_assert(offsetof(NanoPush, drawId) == IndirectDrawIdConstant * 4);

f32 MaxScaleOf(const Mat4& m)
{
    return Max(Max(Length(m.columns[0].XYZ()), Length(m.columns[1].XYZ())), Length(m.columns[2].XYZ()));
}

u32 Align16(u32 v) { return (v + 15u) & ~15u; }

/// How many clusters (sorted by parent error, descending) have a parent error above `cutoff`.
u32 CountAbove(Span<const f32> errors, f32 cutoff)
{
    usize lo = 0, hi = errors.Size();
    while (lo < hi) {
        const usize mid = (lo + hi) / 2;
        if (errors[mid] > cutoff)
            lo = mid + 1;
        else
            hi = mid;
    }
    return u32(lo);
}

} // namespace

bool SceneRenderer::InitNano()
{
    struct Entry
    {
        const char* name;
        ShaderBytecode* out;
        Pipeline** pipeline;
    } entries[] = {{"NanoClearCS", &m_nanoClearCS, &m_nanoClearPipeline},
                   {"NanoCullCS", &m_nanoCullCS, &m_nanoCullPipeline},
                   {"NanoArgsCS", &m_nanoArgsCS, &m_nanoArgsPipeline},
                   {"NanoExpandCS", &m_nanoExpandCS, &m_nanoExpandPipeline}};
    for (const Entry& e : entries) {
        ShaderCompileDesc d;
        d.path = "/Shaders/Forward/Nano.hlsl";
        d.entryPoint = e.name;
        d.stage = ShaderStage::Compute;
        Result<ShaderBytecode> r = ShaderCompiler::Compile(d);
        if (!r) {
            ZX_LOG_ERROR(LogNano, "{}", r.GetError().message);
            return false;
        }
        *e.out = move(*r);
        ComputePipelineDesc cd;
        cd.computeShader = e.out;
        cd.debugName = e.name;
        *e.pipeline = m_device.CreateComputePipeline(cd);
        if (!*e.pipeline)
            return false;
    }
    for (Buffer*& b : m_nanoReadback)
        b = m_device.CreateBuffer(BufferDesc{NanoViewCount * NanoCounterStride, BufferUsage::CopyDest, MemoryType::Readback, 0, "Nano.Readback"});
    m_nanoCounters = m_device.CreateBuffer(BufferDesc{NanoViewCount * NanoCounterStride,
                                                      BufferUsage::UnorderedAccess | BufferUsage::ShaderResource | BufferUsage::CopySource,
                                                      MemoryType::GpuOnly, 0, "Nano.Counters"});
    return m_nanoCounters != nullptr;
}

void SceneRenderer::EnsureNanoBuffers(u32 binCount, u32 candidates)
{
    // Regions sized by the triangle budget: main phase 1 gets it all, phase 2 and the shadows half each.
    const u32 budget = Clamp(m_settings.nano.triangleBudget, 100000u, 20000000u);
    const u32 triangles[NanoRegions] = {budget, budget / 2, budget / 2};
    u64 indexTotal = 0;
    for (u32 r = 0; r < NanoRegions; ++r) {
        m_nanoIndexBase[r] = u32(indexTotal);
        m_nanoIndexCapacity[r] = triangles[r] * 3;
        indexTotal += m_nanoIndexCapacity[r];
    }
    const u32 visibleCapacity = budget / 16 + 4096; // clusters average well above 16 triangles
    if (indexTotal * 4 != m_nanoIndexBytes || !m_nanoIndices) {
        m_device.Destroy(m_nanoIndices);
        m_nanoIndexBytes = indexTotal * 4;
        m_nanoIndices = m_device.CreateBuffer(BufferDesc{m_nanoIndexBytes, BufferUsage::Index | BufferUsage::UnorderedAccess,
                                                         MemoryType::GpuOnly, 0, "Nano.Indices"});
    }
    if (visibleCapacity != m_nanoVisibleCapacity || !m_nanoVisible) {
        m_device.Destroy(m_nanoVisible);
        m_nanoVisibleCapacity = visibleCapacity;
        m_nanoVisible = m_device.CreateBuffer(BufferDesc{u64(visibleCapacity) * NanoRegions * NanoRecordStride,
                                                         BufferUsage::UnorderedAccess | BufferUsage::ShaderResource, MemoryType::GpuOnly, 0,
                                                         "Nano.Visible"});
    }
    if (binCount > m_nanoBinCapacity || !m_nanoArgs) {
        u32 c = Max(m_nanoBinCapacity, 64u);
        while (c < binCount)
            c *= 2;
        m_device.Destroy(m_nanoArgs);
        m_nanoBinCapacity = c;
        m_nanoArgs = m_device.CreateBuffer(BufferDesc{u64(c) * NanoRegions * sizeof(IndirectDrawArgs),
                                                      BufferUsage::UnorderedAccess | BufferUsage::Indirect, MemoryType::GpuOnly, 0, "Nano.Args"});
    }
    if (candidates > m_nanoCandidateCapacity || !m_nanoCandidates) {
        u32 c = Max(m_nanoCandidateCapacity, 64u * 1024u);
        while (c < candidates)
            c *= 2;
        m_device.Destroy(m_nanoCandidates);
        m_nanoCandidateCapacity = c;
        m_nanoCandidates = m_device.CreateBuffer(BufferDesc{8 + u64(c) * 8, BufferUsage::UnorderedAccess, MemoryType::GpuOnly, 0,
                                                            "Nano.Candidates"});
    }
}

void SceneRenderer::PrepareNano(Span<const RenderInstance> instances, const RenderView& view, const Frustum& frustum, f32 invTanHalfFov,
                                const Frustum* cascadeFrustums)
{
    ZX_PROFILE_SCOPE("SceneRenderer::PrepareNano");
    m_nanoActive = false;
    m_nanoBuckets.Clear();
    for (NanoView& v : m_nanoViews)
        v = NanoView{};

    // Budget feedback from the frame that last used this slot (its fence has been waited on): the threshold scale is
    // steered so the view uses ~70 % of its budget (triangles fall roughly with the square of the error), at once when
    // something didn't fit, never finer than asked.
    auto adapt = [](f32& scale, f32 indices, f32 capacity, bool over) {
        if (over) {
            scale = Min(scale * 1.5f, 64.0f);
            return;
        }
        const f32 ratio = Clamp(indices / (capacity * 0.7f), 0.25f, 4.0f);
        scale = Clamp(scale * Pow(ratio, 0.25f), 1.0f, 64.0f);
    };
    const u32 frame = m_device.GetFrameIndex() % FramesInFlight;
    if (m_nanoReadbackValid[frame]) {
        m_nanoReadbackValid[frame] = false;
        u32 c[NanoViewCount * 4];
        MemCopy(c, m_nanoReadback[frame]->GetMappedData(), sizeof(c));
        const u64 mainIndices = u64(Min(c[1], m_nanoIndexCapacity[0])) + Min(c[5], m_nanoIndexCapacity[1]);
        const bool mainOver = (c[2] | c[6]) != 0;
        m_nanoLast.clusters = c[0] + c[4];
        m_nanoLast.triangles = mainIndices / 3;
        m_nanoLast.overflow = mainOver;
        adapt(m_nanoErrorScale, f32(mainIndices), f32(m_nanoIndexCapacity[0]), mainOver);
        u32 shadowIndices = 0;
        bool shadowOver = false;
        for (u32 v = 2; v < NanoViewCount; ++v) {
            shadowIndices = Max(shadowIndices, Min(c[v * 4 + 1], m_nanoIndexCapacity[2]));
            shadowOver |= c[v * 4 + 2] != 0;
        }
        if (shadowIndices > 0) {
            m_nanoLast.shadowTriangles = shadowIndices / 3;
            adapt(m_nanoShadowErrorScale, f32(shadowIndices), f32(m_nanoIndexCapacity[2]), shadowOver);
        }
    }
    m_stats.nanoClusters = m_nanoLast.clusters;
    m_stats.nanoTriangles = m_nanoLast.triangles;
    m_stats.nanoShadowTriangles = m_nanoLast.shadowTriangles;
    m_stats.nanoOverflow = m_nanoLast.overflow;
    m_stats.nanoErrorScale = m_nanoErrorScale;
    m_stats.nanoInstances = u32(m_nanoRefs.Size());
    if (m_nanoRefs.Empty() || !m_nanoCullPipeline)
        return;

    // World bounds of every instance.
    for (NanoRef& r : m_nanoRefs) {
        const Mat4& world = instances[r.instance].world;
        r.scale = Max(MaxScaleOf(world), 1e-6f);
        r.sphere = Sphere{world.TransformPoint(r.mesh->nanoSphere.center), r.mesh->nanoSphere.radius * r.scale};
        m_stats.nanoSourceTriangles += r.mesh->nanoLeafTriangles;
    }

    // Material bins: (pipeline, shader, material) of every opaque / masked section; sorted so that a bucket (one
    // DrawIndexedIndirect) is a run of bins with the same pipeline and shader.
    HashMap<u64, u32> keyIndex;
    Array<u64> keys;
    HashMap<u32, u32> templateBase;
    Array<u32> sectionBins;
    for (NanoRef& r : m_nanoRefs) {
        if (const u32* base = templateBase.Find(r.templ)) {
            r.sectionBase = *base;
            continue;
        }
        r.sectionBase = u32(sectionBins.Size());
        templateBase.InsertOrAssign(r.templ, r.sectionBase);
        for (u32 g : m_templates[r.templ].groups) {
            const DrawGroup& group = m_groups[g];
            if (group.translucent) {
                sectionBins.PushBack(InvalidIndex);
                continue;
            }
            const u64 key = (u64(group.pipeline) << 56) | (u64(group.shader & 0xFFFFFFu) << 32) | group.material;
            const auto ins = keyIndex.Insert(key, u32(keys.Size()));
            if (ins.second)
                keys.PushBack(key);
            sectionBins.PushBack(*ins.first);
        }
    }
    if (keys.Empty())
        return; // only translucent sections
    Array<u32> order;
    for (u32 i = 0; i < keys.Size(); ++i)
        order.PushBack(i);
    Sort(order, [&](u32 a, u32 b) { return keys[a] < keys[b]; });
    Array<u32> rank;
    rank.Resize(keys.Size());
    for (u32 i = 0; i < order.Size(); ++i)
        rank[order[i]] = i;
    for (u32& b : sectionBins)
        if (b != InvalidIndex)
            b = rank[b];
    m_nanoBinCount = u32(keys.Size());
    for (u32 i = 0; i < order.Size(); ++i) {
        const u64 key = keys[order[i]];
        const u8 pipeline = u8(key >> 56);
        const u32 shader = u32(key >> 32) & 0xFFFFFFu;
        if (m_nanoBuckets.Empty() || m_nanoBuckets.Back().pipeline != pipeline || m_nanoBuckets.Back().shader != shader)
            m_nanoBuckets.PushBack(NanoBucket{pipeline, shader, i, 0});
        ++m_nanoBuckets.Back().binCount;
    }

    // Per view: the instances it sees and, for each, the prefix of clusters whose parent error can still show. A cluster
    // is drawn only if its parent's error projects above the threshold; from the instance's nearest point (every cluster
    // sphere lies inside r.sphere) that needs parentError > threshold * distance / (scale * pixelsPerUnit).
    Array<UVec2> items[NanoViewCount];
    HashMap<const GpuMesh*, u32> wanted; // clusters each mesh needs in some view (streaming requests)
    auto addView = [&](u32 v, const Frustum& fr, bool ortho, f32 pixelsPerUnit, f32 threshold, bool castersOnly) {
        NanoView& nv = m_nanoViews[v];
        nv.ortho = ortho;
        nv.pixelsPerUnit = pixelsPerUnit;
        nv.threshold = threshold;
        for (u32 i = 0; i < m_nanoRefs.Size(); ++i) {
            const NanoRef& r = m_nanoRefs[i];
            if (castersOnly && !instances[r.instance].castShadows)
                continue;
            if (!fr.Intersects(r.sphere))
                continue;
            f32 cutoff = 0.0f;
            if (ortho) {
                cutoff = threshold / (r.scale * pixelsPerUnit);
            } else {
                const f32 d = Length(r.sphere.center - view.position) - r.sphere.radius;
                cutoff = d > 1e-4f ? threshold * d / (r.scale * pixelsPerUnit) : 0.0f;
            }
            const u32 needed = CountAbove(r.mesh->nanoParentErrors.View(), cutoff * 0.999f);
            u32& w = *wanted.Insert(r.mesh, 0u).first;
            w = Max(w, needed);
            const u32 count = Min(needed, r.mesh->NanoResidentClusters()); // what's streamed in so far
            if (count == 0)
                continue;
            items[v].PushBack(UVec2(nv.threads, i));
            nv.threads += count;
        }
        nv.itemCount = u32(items[v].Size());
    };
    const NanoSettings& ns = m_settings.nano;
    const f32 pixelsPerUnit = f32(view.height) * invTanHalfFov * 0.5f;
    addView(0, frustum, false, pixelsPerUnit, Max(ns.pixelError, 0.1f) * m_nanoErrorScale, false);
    for (u32 c = 0; c < m_cascadeCount; ++c)
        if (m_cascades[c].update)
            addView(2 + c, cascadeFrustums[c], true, 1.0f / Max(m_viewConstants.cascadeTexelWorld[c], 1e-6f),
                    Max(ns.shadowPixelError, 0.1f) * m_nanoShadowErrorScale, true);
    // Virtual shadow levels rendering pages this frame: casters inside the rectangle around those pages.
    for (u32 k = 0; m_vsmActive && k < m_vsmLevels; ++k)
        if (m_vsmRenderLevels & (1u << k)) {
            const u32 v = GpuViewCount + k;
            addView(v, Frustum::FromMatrix(m_vsmLevelViewProj[k]), true, 1.0f / (VsmPageWorld(k) / 128.0f),
                    Max(ns.shadowPixelError, 0.1f) * m_nanoShadowErrorScale, true);
            m_nanoViews[v].vsmLevel = k;
            m_nanoViews[v].vsmMapOffset = m_vsmMapOffset[k];
        }
    m_nanoViews[1] = m_nanoViews[0]; // phase 2 tests phase 1's candidates with the same cut
    m_nanoViews[1].itemCount = 0;
    for (const auto& kv : wanted)
        m_assets.RequestNanoClusters(kv.first, kv.second);
    m_stats.nanoTested = m_nanoViews[0].threads;
    m_stats.nanoBins = m_nanoBinCount;

    // Tables: instances, section bins, bins, view items (one upload buffer per frame in flight).
    m_nanoInstancesOffset = 0;
    m_nanoSectionBinsOffset = Align16(u32(m_nanoRefs.Size()) * NanoInstanceStride);
    m_nanoBinsOffset = m_nanoSectionBinsOffset + Align16(u32(sectionBins.Size()) * 4);
    u32 bytes = m_nanoBinsOffset + m_nanoBinCount * NanoBinStride;
    // Page tables (pool slot per resident page) of every mesh in use.
    HashMap<const GpuMesh*, u32> pageTables;
    Array<const GpuMesh*> tableMeshes;
    for (const NanoRef& r : m_nanoRefs)
        if (pageTables.Insert(r.mesh, bytes).second) {
            tableMeshes.PushBack(r.mesh);
            bytes += Align16(u32(r.mesh->nanoSlots.Size()) * 4);
        }
    for (u32 v = 0; v < NanoViewCount; ++v) {
        m_nanoViews[v].itemsOffset = bytes;
        bytes += Align16(u32(items[v].Size()) * 8);
    }
    if (bytes > m_nanoTableBytes || !m_nanoTables[0]) {
        u64 c = Max(m_nanoTableBytes, u64(64 * 1024));
        while (c < bytes)
            c *= 2;
        for (Buffer*& b : m_nanoTables) {
            m_device.Destroy(b);
            b = m_device.CreateBuffer(BufferDesc{c, BufferUsage::ShaderResource, MemoryType::Upload, 0, "Nano.Tables"});
        }
        m_nanoTableBytes = c;
    }
    u8* data = static_cast<u8*>(m_nanoTables[frame]->GetMappedData());
    for (u32 i = 0; i < m_nanoRefs.Size(); ++i) {
        const NanoRef& r = m_nanoRefs[i];
        const u32 record[8] = {r.row,
                               r.mesh->nanoBuffer->GetSrvIndex(),
                               *pageTables.Find(r.mesh),
                               r.sectionBase,
                               std::bit_cast<u32>(r.mesh->NanoResidentError()),
                               r.mesh->NanoResidentClusters(),
                               0,
                               std::bit_cast<u32>(r.scale)};
        MemCopy(data + m_nanoInstancesOffset + usize(i) * NanoInstanceStride, record, sizeof(record));
    }
    MemCopy(data + m_nanoSectionBinsOffset, sectionBins.Data(), sectionBins.SizeBytes());
    for (const GpuMesh* mesh : tableMeshes)
        MemCopy(data + *pageTables.Find(mesh), mesh->nanoSlots.Data(), mesh->nanoSlots.SizeBytes());
    for (u32 i = 0; i < order.Size(); ++i) {
        const u32 bin[4] = {u32(keys[order[i]] & 0xFFFFFFFFu), 0, 0, 0}; // material
        MemCopy(data + m_nanoBinsOffset + usize(i) * NanoBinStride, bin, sizeof(bin));
    }
    for (u32 v = 0; v < NanoViewCount; ++v)
        if (!items[v].Empty())
            MemCopy(data + m_nanoViews[v].itemsOffset, items[v].Data(), items[v].SizeBytes());

    EnsureNanoBuffers(m_nanoBinCount, m_nanoViews[0].threads);
    m_nanoActive = m_nanoIndices && m_nanoVisible && m_nanoArgs && m_nanoCandidates;
}

void SceneRenderer::AddNanoCullPass(RenderGraph& graph, u32 view, u32 mode, const Mat4& viewProj, const NanoBuffers& buffers)
{
    const NanoView& nv = m_nanoViews[view];
    const u32 region = Min(view, NanoRegions - 1);
    NanoParams p = {};
    p.viewProj = viewProj;
    p.occlusionViewProj = mode == 2 ? m_cullViewProj : m_hizViewProj;
    const Frustum fr = Frustum::FromMatrix(viewProj);
    for (u32 i = 0; i < 6; ++i)
        p.planes[i] = i < fr.planeCount ? Vec4(fr.planes[i].normal, fr.planes[i].d) : Vec4(0, 0, 0, 1e30f);
    p.eye = m_cullCamera;
    p.pixelsPerUnit = nv.pixelsPerUnit;
    p.threshold = nv.threshold;
    p.ortho = nv.ortho ? 1u : 0u;
    p.mode = mode;
    p.instancesOffset = m_nanoInstancesOffset;
    p.itemsOffset = nv.itemsOffset;
    p.itemCount = nv.itemCount;
    p.sectionBinsOffset = m_nanoSectionBinsOffset;
    p.instanceBuffer = m_viewConstants.instanceBuffer;
    p.hizMips = m_hizMips;
    p.hizSize = Vec2(f32(m_hizSize.x), f32(m_hizSize.y));
    p.view = view;
    p.region = region;
    p.binCount = m_nanoBinCount;
    p.binCapacity = m_nanoBinCapacity;
    p.visibleCapacity = m_nanoVisibleCapacity;
    p.indexCapacity = m_nanoIndexCapacity[region];
    p.indexBase = m_nanoIndexBase[region];
    p.resetCandidates = view == 0 ? 1u : 0u;
    p.candidateCapacity = m_nanoCandidateCapacity;
    p.pool = m_assets.GetNanoPoolSrv();
    p.vsmTable = m_viewConstants.vsmTable;
    p.vsmMapOffset = nv.vsmLevel != InvalidIndex ? nv.vsmMapOffset : InvalidIndex;
    if (nv.vsmLevel != InvalidIndex) {
        for (u32 r = 0; r < 3; ++r)
            p.vsmRows[r] = m_vsmRenderRows[r];
        p.vsmOrigin[0] = m_vsmRenderOrigin[nv.vsmLevel][0];
        p.vsmOrigin[1] = m_vsmRenderOrigin[nv.vsmLevel][1];
        p.vsmPageWorld = VsmPageWorld(nv.vsmLevel);
    }
    const u32 frame = m_device.GetFrameIndex() % FramesInFlight;
    p.tables = m_nanoTables[frame]->GetSrvIndex();
    // Phase 1 reads last frame's pyramid, phase 2 this frame's.
    const u32 hizSet = mode == 2 ? m_hizCurrent : (m_hizCurrent ^ 1u);
    const bool useHiz = mode != 0 && m_hiz[hizSet][0];
    RGTexture hizEven, hizOdd;
    if (useHiz) {
        hizEven = graph.ImportTexture(m_hiz[hizSet][0], ResourceState::ShaderResource);
        hizOdd = graph.ImportTexture(m_hiz[hizSet][1], ResourceState::ShaderResource);
    }
    const NanoBuffers b = buffers;
    graph.AddPass(
        Name(String::Format("NanoCull{}", view)).CStr(), PassType::Compute,
        [b, useHiz, hizEven, hizOdd](PassBuilder& pb) {
            pb.WriteBuffer(b.args);
            pb.WriteBuffer(b.counters);
            pb.WriteBuffer(b.visible);
            pb.WriteBuffer(b.indices);
            pb.WriteBuffer(b.candidates);
            if (useHiz) {
                pb.ReadTexture(hizEven);
                pb.ReadTexture(hizOdd);
            }
        },
        [this, p, useHiz, hizSet, mode, threads = nv.threads](PassContext& ctx) {
            NanoParams q = p;
            q.args = m_nanoArgs->GetUavIndex();
            q.counters = m_nanoCounters->GetUavIndex();
            q.visible = m_nanoVisible->GetUavIndex();
            q.indices = m_nanoIndices->GetUavIndex();
            q.candidates = m_nanoCandidates->GetUavIndex();
            q.hiz = useHiz ? m_hiz[hizSet][0]->GetSrvIndex() : InvalidDescriptor;
            q.hizOdd = useHiz ? m_hiz[hizSet][1]->GetSrvIndex() : InvalidDescriptor;
            CommandList& cmd = ctx.cmd;
            auto uavBarriers = [&] {
                cmd.Barrier(m_nanoArgs, ResourceState::UnorderedAccess);
                cmd.Barrier(m_nanoCounters, ResourceState::UnorderedAccess);
                cmd.Barrier(m_nanoVisible, ResourceState::UnorderedAccess);
                cmd.Barrier(m_nanoCandidates, ResourceState::UnorderedAccess);
            };
            auto run = [&](Pipeline* pipeline, u32 threadCount, u32 groups) {
                q.threadCount = threadCount;
                cmd.SetPipeline(pipeline);
                cmd.SetConstants(0, &q, sizeof(q));
                if (groups > 0)
                    cmd.Dispatch(groups);
            };
            run(m_nanoClearPipeline, 0, Max((m_nanoBinCount + 63) / 64, 1u));
            uavBarriers();
            if (mode == 2)
                run(m_nanoCullPipeline, Phase2Threads, Phase2Threads / 64);
            else
                run(m_nanoCullPipeline, threads, (threads + 63) / 64);
            uavBarriers();
            run(m_nanoArgsPipeline, 0, 1);
            uavBarriers();
            run(m_nanoExpandPipeline, ExpandGroups, ExpandGroups);
        });
}

void SceneRenderer::AddNanoStatsPass(RenderGraph& graph, const NanoBuffers& buffers)
{
    const u32 frame = m_device.GetFrameIndex() % FramesInFlight;
    m_nanoReadbackValid[frame] = true;
    const RGBuffer counters = buffers.counters;
    graph.AddPass(
        "NanoStats", PassType::Compute,
        [counters](PassBuilder& b) {
            b.ReadBuffer(counters, ResourceState::CopySource);
            b.SideEffects();
        },
        [this, frame](PassContext& ctx) { ctx.cmd.CopyBuffer(m_nanoReadback[frame], 0, m_nanoCounters, 0, NanoViewCount * NanoCounterStride); });
}

void SceneRenderer::DrawNano(CommandList& cmd, u32 view, PipelineVariant variant)
{
    if (!m_nanoActive)
        return;
    const u32 region = Min(view, NanoRegions - 1);
    NanoPush push;
    push.visible = m_nanoVisible->GetSrvIndex();
    push.tables = m_nanoTables[m_device.GetFrameIndex() % FramesInFlight]->GetSrvIndex();
    push.instancesOffset = m_nanoInstancesOffset;
    push.binsOffset = m_nanoBinsOffset;
    push.pool = m_assets.GetNanoPoolSrv();
    const bool vsm = view >= GpuViewCount; // virtual shadow level: page-replicated records
    push.vsmTable = m_viewConstants.vsmTable;
    push.vsmPages = m_vsmRenderPagesOffset;
    Pipeline* bound = nullptr;
    cmd.SetIndexBuffer(m_nanoIndices, IndexFormat::UInt32);
    for (const NanoBucket& b : m_nanoBuckets) {
        u32 kind = b.pipeline;
        if (variant == VariantShadow) // shadow passes are two-sided anyway
            kind = (kind == Masked || kind == MaskedTwoSided) ? Masked : Opaque;
        Pipeline* p = vsm ? GetVsmPipeline(kind == Masked, b.shader) : GetPipeline(kind, variant, b.shader, true);
        if (!p)
            continue;
        if (p != bound) {
            cmd.SetPipeline(p);
            bound = p;
        }
        cmd.SetPushConstants(push);
        cmd.DrawIndexedIndirect(m_nanoArgs, u64(region * m_nanoBinCapacity + b.firstBin) * sizeof(IndirectDrawArgs), b.binCount);
        ++m_stats.indirectCalls;
    }
}

} // namespace zx::render
