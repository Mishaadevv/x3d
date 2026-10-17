// GPU-driven opaque / masked geometry for SceneRenderer (M12). The CPU writes the instance rows and one cull item per
// (instance, mesh section) - in parallel - and keeps draw groups (mesh section + material) with their LOD slots; the
// GPU culls (frustum, two-phase HiZ occlusion), picks LODs and fills per-slot instance lists and DrawIndexedIndirect
// records, which one call per (pipeline, shader, index buffer) bucket draws. Shaders: Forward/GpuCull.hlsl, the
// indirect branch of VSMain in Forward/ForwardPBR.hlsl.

#include "Render/SceneRenderer.h"
#include "Core/Containers/Algorithm.h"
#include "Core/Jobs/JobSystem.h"
#include "Core/Log/Log.h"
#include "Core/Math/Geometry.h"
#include "Core/Profiler/Profiler.h"
#include "Core/String/Name.h"
#include "RHI/ShaderCompiler.h"

ZX_DEFINE_LOG_CATEGORY_STATIC(LogGpuDriven, Info);

namespace zx::render {

using namespace rhi;

namespace {

constexpr u32 InstanceStride = 192; // keep in sync with SceneRenderer.cpp / ForwardPBR.hlsl
constexpr u32 ItemStride = 32, GroupStride = 32, SlotStride = 32;
constexpr u32 ItemCastsShadow = 1;

struct DrawPushGpu // SceneRenderer.cpp DrawPush: the indirect draw id lands in dword 4
{
    u32 instanceIndex = 0, materialIndex = 0, vertexBuffer = 0, lod = 0, drawId = 0;
};
static_assert(offsetof(DrawPushGpu, drawId) == IndirectDrawIdConstant * 4);

struct CullParams // GpuCull.hlsl
{
    Mat4 viewProj;
    Mat4 occlusionViewProj;
    Vec4 planes[6];
    Vec3 cameraPosition;
    f32 invTanHalfFov;
    f32 lodBias;
    i32 forcedLod;
    u32 itemCount;
    u32 mode;
    u32 items, groups, slots, slotCount;
    u32 args, lists, candidates, hiz;
    u32 viewIndex, listCapacity, hizMips, requiredFlags;
    Vec2 hizSize;
    f32 minRadius;
    u32 lodOffset;
    u32 hizOdd;
    u32 pad[3];
};
static_assert(sizeof(CullParams) == 336); // HLSL constant buffer layout

struct HiZParams
{
    u32 source, sourceMip, target, pad;
    UVec2 sourceSize, targetSize;
};

f32 MaxScaleOf(const Mat4& m)
{
    return Max(Max(Length(m.columns[0].XYZ()), Length(m.columns[1].XYZ())), Length(m.columns[2].XYZ()));
}

u64 Mix(u64 h, u64 v) { return (h ^ (v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2))) * 0xFF51AFD7ED558CCDull; }

u64 AssetKey(const AssetId& id) { return Mix(id.high, id.low); }

} // namespace

bool SceneRenderer::InitGpuDriven()
{
    struct Entry
    {
        const char* name;
        ShaderBytecode* out;
    } entries[] = {{"InitArgsCS", &m_cullInitCS}, {"CullCS", &m_cullCS}, {"HiZCS", &m_hizCS}};
    for (const Entry& e : entries) {
        ShaderCompileDesc d;
        d.path = "/Shaders/Forward/GpuCull.hlsl";
        d.entryPoint = e.name;
        d.stage = ShaderStage::Compute;
        Result<ShaderBytecode> r = ShaderCompiler::Compile(d);
        if (!r) {
            ZX_LOG_ERROR(LogGpuDriven, "{}", r.GetError().message);
            return false;
        }
        *e.out = move(*r);
    }
    ComputePipelineDesc cd;
    cd.computeShader = &m_cullInitCS;
    cd.debugName = "GpuCull.InitArgs";
    m_cullInitPipeline = m_device.CreateComputePipeline(cd);
    cd.computeShader = &m_cullCS;
    cd.debugName = "GpuCull";
    m_cullPipeline = m_device.CreateComputePipeline(cd);
    cd.computeShader = &m_hizCS;
    cd.debugName = "HiZ";
    m_hizPipeline = m_device.CreateComputePipeline(cd);
    return m_cullInitPipeline && m_cullPipeline && m_hizPipeline;
}

u32 SceneRenderer::TemplateFor(const GpuMesh* mesh, Span<const AssetId> materials)
{
    // Keyed by the mesh (its address and section count: a replaced memory mesh gets new templates) and materials.
    u64 key = Mix(u64(reinterpret_cast<uintptr_t>(mesh)), mesh->sections.Size());
    for (const AssetId& m : materials)
        key = Mix(key, AssetKey(m));
    for (;; ++key) {
        const u32* found = m_templateIndex.Find(key);
        if (!found)
            break;
        const DrawTemplate& t = m_templates[*found];
        bool same = t.mesh == mesh && t.materials.Size() == materials.Size() && t.groups.Size() == mesh->sections.Size();
        for (usize i = 0; same && i < materials.Size(); ++i)
            same = t.materials[i] == materials[i];
        if (same)
            return *found;
    }
    DrawTemplate t;
    t.mesh = mesh;
    t.materials.Append(materials);
    for (u32 s = 0; s < mesh->sections.Size(); ++s) {
        const asset::CookedMeshSection& section = mesh->sections[s];
        const AssetId materialId = section.materialSlot < materials.Size() ? materials[section.materialSlot] : AssetId();
        u64 gkey = Mix(Mix(u64(reinterpret_cast<uintptr_t>(mesh)), s), AssetKey(materialId));
        u32 group = InvalidIndex;
        for (;; ++gkey) {
            const u32* found = m_groupIndex.Find(gkey);
            if (!found)
                break;
            const DrawGroup& g = m_groups[*found];
            if (g.mesh == mesh && g.section == s && g.materialId == materialId) {
                group = *found;
                break;
            }
        }
        if (group == InvalidIndex) {
            DrawGroup g;
            g.mesh = mesh;
            g.section = s;
            g.materialId = materialId;
            g.lodCount = Min(section.lodCount, MaxGpuLods);
            group = u32(m_groups.Size());
            m_groups.PushBack(g);
            m_groupIndex.InsertOrAssign(gkey, group);
        }
        t.groups.PushBack(group);
    }
    const u32 index = u32(m_templates.Size());
    m_templates.PushBack(move(t));
    m_templateIndex.InsertOrAssign(key, index);
    return index;
}

void SceneRenderer::EnsureGpuBuffers()
{
    auto grow = [&](Buffer*& b, u32& capacity, u32 needed, u64 elementBytes, BufferUsage usage, const char* name) {
        if (needed <= capacity && b)
            return;
        u32 c = Max(capacity, 1024u);
        while (c < needed)
            c *= 2;
        m_device.Destroy(b);
        b = m_device.CreateBuffer(BufferDesc{u64(c) * elementBytes, usage, MemoryType::GpuOnly, 0, name});
        capacity = c;
    };
    grow(m_indirectArgs, m_argsCapacity, GpuViewCount * m_slotCount, sizeof(IndirectDrawArgs),
         BufferUsage::UnorderedAccess | BufferUsage::Indirect | BufferUsage::ShaderResource, "GpuDriven.Args");
    grow(m_visibleLists, m_listsCapacity, GpuViewCount * m_listCapacity, 4, BufferUsage::UnorderedAccess | BufferUsage::ShaderResource,
         "GpuDriven.Lists");
    grow(m_candidates, m_candidateCapacity, m_itemCount + 1, 4, BufferUsage::UnorderedAccess | BufferUsage::ShaderResource, "GpuDriven.Candidates");
    // Tables (upload, one set per frame in flight).
    auto growTable = [&](Buffer* (&set)[FramesInFlight], u64& capacity, u64 needed, const char* name) {
        if (needed <= capacity && set[0])
            return;
        u64 c = Max(capacity, u64(16 * 1024));
        while (c < needed)
            c *= 2;
        for (Buffer*& b : set) {
            m_device.Destroy(b);
            b = m_device.CreateBuffer(BufferDesc{c, BufferUsage::ShaderResource, MemoryType::Upload, 0, name});
        }
        capacity = c;
    };
    growTable(m_cullItems, m_cullItemBytes, u64(Max(m_itemCount, 1u)) * ItemStride, "GpuDriven.Items");
    growTable(m_cullGroups, m_cullGroupBytes, u64(Max<usize>(m_activeGroups.Size(), 1)) * GroupStride, "GpuDriven.Groups");
    growTable(m_cullSlots, m_cullSlotBytes, u64(Max(m_slotCount, 1u)) * SlotStride, "GpuDriven.Slots");
}

void SceneRenderer::PrepareGpu(Span<const RenderInstance> instances, const RenderView& view, const Frustum& frustum, f32 invTanHalfFov,
                               u8* instanceData)
{
    ZX_PROFILE_SCOPE("SceneRenderer::PrepareGpu");
    const usize n = instances.Size();
    // A. Serial: resident meshes, templates, items per draw group (mesh lookups and the maps are not thread-safe).
    Array<const GpuMesh*> meshes;
    meshes.Resize(n);
    Array<u32> rows, templates, firstItem;
    rows.Resize(n);
    templates.Resize(n);
    firstItem.Resize(n);
    m_activeGroups.Clear();
    u32 rowCount = 0, items = 0;
    ZX_PROFILE_SCOPE("PrepareGpu.Serial");
    if (m_instanceCacheGeneration != m_assets.MeshGeneration()) {
        m_instanceCache.Clear();
        m_instanceCacheGeneration = m_assets.MeshGeneration();
    }
    m_instanceCache.Resize(n);
    for (usize i = 0; i < n; ++i) {
        const RenderInstance& inst = instances[i];
        if (const u32 skin = SkinSlot(i); skin != InvalidIndex) {
            // Skinned: own vertex buffer, drawn from the CPU lists (Prepare); only the instance row is made here.
            meshes[i] = m_assets.GetMesh(inst.mesh);
            rows[i] = rowCount++;
            m_skinJobs[skin].row = rows[i];
            templates[i] = InvalidIndex;
            continue;
        }
        InstanceCache& cache = m_instanceCache[i];
        bool hit = cache.gpuMesh && cache.mesh == inst.mesh && cache.materialCount == inst.materials.Size() &&
                   cache.materials == inst.materials.Data();
        for (u32 m = 0; hit && m < cache.materialCount; ++m)
            hit = m_templates[cache.templateIndex].materials[m] == inst.materials[m];
        const GpuMesh* mesh = hit ? cache.gpuMesh : m_assets.GetMesh(inst.mesh);
        meshes[i] = mesh;
        if (!mesh) {
            ++m_stats.notResident;
            cache.gpuMesh = nullptr;
            continue;
        }
        rows[i] = rowCount++;
        const u32 t = hit ? cache.templateIndex : TemplateFor(mesh, inst.materials);
        cache = InstanceCache{inst.mesh, mesh, inst.materials.Data(), u32(inst.materials.Size()), t};
        templates[i] = t;
        firstItem[i] = items;
        DrawTemplate& tmpl = m_templates[t];
        tmpl.hasTranslucent = false;
        for (u32 g : tmpl.groups) {
            DrawGroup& group = m_groups[g];
            if (group.lastFrame != m_frame) {
                // First use this frame: current material state (shaders compile, textures stream in).
                group.lastFrame = m_frame;
                group.count = 0;
                group.mesh = mesh;
                group.lodCount = Min(mesh->sections[group.section].lodCount, MaxGpuLods);
                group.material = m_assets.GetMaterialIndex(group.materialId);
                const MaterialInfo& info = m_assets.GetMaterialInfo(group.material);
                group.translucent = info.blend == MaterialBlend::Translucent;
                const bool masked = info.blend == MaterialBlend::Masked;
                group.pipeline = u8(masked ? (info.doubleSided ? MaskedTwoSided : Masked) : (info.doubleSided ? OpaqueTwoSided : Opaque));
                group.shader = info.shader;
                if (!group.translucent)
                    m_activeGroups.PushBack(g);
            }
            if (group.translucent) {
                tmpl.hasTranslucent = true;
                continue;
            }
            ++group.count;
            ++items;
        }
        if (inst.castShadows)
            m_casters.PushBack(InstanceRef{rows[i], mesh, &inst});
    }
    m_itemCount = items;

    // Slots: each group gets lodCount consecutive slots; groups sorted so one bucket = one pipeline + index buffer.
    Sort(m_activeGroups, [this](u32 a, u32 b) {
        const DrawGroup& x = m_groups[a];
        const DrawGroup& y = m_groups[b];
        if (x.pipeline != y.pipeline)
            return x.pipeline < y.pipeline;
        if (x.shader != y.shader)
            return x.shader < y.shader;
        return x.mesh < y.mesh;
    });
    m_buckets.Clear();
    u32 slot = 0, list = 0;
    for (u32 d = 0; d < m_activeGroups.Size(); ++d) {
        DrawGroup& g = m_groups[m_activeGroups[d]];
        g.dense = d;
        g.firstSlot = slot;
        g.listBase = list;
        if (m_buckets.Empty() || m_buckets.Back().pipeline != g.pipeline || m_buckets.Back().shader != g.shader || m_buckets.Back().mesh != g.mesh)
            m_buckets.PushBack(Bucket{g.pipeline, g.shader, g.mesh, slot, 0});
        m_buckets.Back().slotCount += g.lodCount;
        slot += g.lodCount;
        list += g.count * g.lodCount;
    }
    m_slotCount = slot;
    m_listCapacity = list;
    EnsureGpuBuffers();
    const u32 frame = m_device.GetFrameIndex() % FramesInFlight;
    u8* itemData = static_cast<u8*>(m_cullItems[frame]->GetMappedData());
    u8* groupData = static_cast<u8*>(m_cullGroups[frame]->GetMappedData());
    u8* slotData = static_cast<u8*>(m_cullSlots[frame]->GetMappedData());

    // Group and slot tables.
    for (u32 d = 0; d < m_activeGroups.Size(); ++d) {
        const DrawGroup& g = m_groups[m_activeGroups[d]];
        const asset::CookedMeshSection& section = g.mesh->sections[g.section];
        u32 header[4] = {g.firstSlot, g.lodCount, 0, 0};
        f32 screen[4] = {1.0f, 0.0f, 0.0f, 0.0f};
        for (u32 l = 1; l < g.lodCount; ++l)
            screen[l] = g.mesh->SectionLodAt(section, l).screenSize;
        u8* dst = groupData + usize(d) * GroupStride;
        MemCopy(dst, header, 16);
        MemCopy(dst + 16, screen, 16);
        for (u32 l = 0; l < g.lodCount; ++l) {
            const asset::CookedSectionLod& range = g.mesh->SectionLodAt(section, l);
            const u32 record[8] = {g.listBase + l * g.count, g.material, g.mesh->vertexBuffer->GetSrvIndex(), l, range.indexCount,
                                   range.indexOffset, 0, 0};
            MemCopy(slotData + usize(g.firstSlot + l) * SlotStride, record, sizeof(record));
        }
    }

    // B. Parallel: instance rows and cull items.
    ZX_PROFILE_SCOPE("PrepareGpu.Parallel");
    Array<u8> moved;
    moved.Resize(n);
    auto work = [&](u32 begin, u32 end) {
        for (u32 i = begin; i < end; ++i) {
            const GpuMesh* mesh = meshes[i];
            moved[i] = 0;
            if (!mesh)
                continue;
            const RenderInstance& inst = instances[i];
            const Mat4 rowsT = Transpose(inst.world);
            u8* dst = instanceData + usize(rows[i]) * InstanceStride;
            MemCopy(dst, &rowsT, 64);
            // Normal matrix = inverse transpose of the upper 3x3; its rows are the columns of the inverse, whose rows
            // are the cross products of the matrix columns (over the determinant).
            const Vec3 a0 = inst.world.columns[0].XYZ(), a1 = inst.world.columns[1].XYZ(), a2 = inst.world.columns[2].XYZ();
            const Vec3 r0 = Cross(a1, a2), r1 = Cross(a2, a0), r2 = Cross(a0, a1);
            const f32 det = Dot(a0, r0);
            const f32 invDet = Abs(det) > 1e-20f ? 1.0f / det : 0.0f;
            const f32 burnParams[3] = {inst.charring, inst.burnGlow, inst.embers};
            for (u32 r = 0; r < 3; ++r) {
                const Vec4 col(r0[r] * invDet, r1[r] * invDet, r2[r] * invDet, burnParams[r]);
                MemCopy(dst + 64 + r * 16, &col, 16);
            }
            const Vec4 burn(inst.burnOrigin, inst.burnRadius);
            MemCopy(dst + 112, &burn, 16);
            moved[i] = inst.prevWorld ? 1 : 0;
            if (inst.prevWorld) { // only the velocity pass reads last frame's rows, and only for moved instances
                const Mat4 prevRows = Transpose(*inst.prevWorld);
                MemCopy(dst + 128, &prevRows, 64);
            }
            if (templates[i] == InvalidIndex) { // skinned: the velocity pass draws it every frame
                if (!inst.prevWorld)
                    MemCopy(dst + 128, &rowsT, 64);
                moved[i] = 0;
                continue;
            }
            const f32 scale = MaxScaleOf(inst.world);
            const DrawTemplate& tmpl = m_templates[templates[i]];
            u32 item = firstItem[i];
            for (u32 s = 0; s < tmpl.groups.Size(); ++s) {
                const DrawGroup& g = m_groups[tmpl.groups[s]];
                if (g.translucent)
                    continue;
                const asset::CookedMeshSection& section = mesh->sections[s];
                const Vec3 center = inst.world.TransformPoint(section.bounds.Center());
                const f32 radius = Length(section.bounds.Extents()) * scale;
                const u32 info[4] = {rows[i], g.dense, inst.castShadows ? ItemCastsShadow : 0u, 0};
                u8* it = itemData + usize(item++) * ItemStride;
                const Vec4 sphere(center, radius);
                MemCopy(it, &sphere, 16);
                MemCopy(it + 16, info, 16);
            }
        }
    };
    if (jobs::IsInitialized() && n > 512)
        jobs::ParallelFor(u32(n), 256, work);
    else
        work(0, u32(n));

    // C. Serial, few: translucent sections (sorted, CPU-drawn) and the velocity draws of instances that moved.
    for (usize i = 0; i < n; ++i) {
        const GpuMesh* mesh = meshes[i];
        if (!mesh || templates[i] == InvalidIndex || (!m_templates[templates[i]].hasTranslucent && !(moved[i] && m_motion)))
            continue;
        const RenderInstance& inst = instances[i];
        const f32 scale = MaxScaleOf(inst.world);
        const DrawTemplate& tmpl = m_templates[templates[i]];
        for (u32 s = 0; s < tmpl.groups.Size(); ++s) {
            const DrawGroup& g = m_groups[tmpl.groups[s]];
            if (!g.translucent && !moved[i])
                continue;
            const asset::CookedMeshSection& section = mesh->sections[s];
            const Sphere sp{inst.world.TransformPoint(section.bounds.Center()), Length(section.bounds.Extents()) * scale};
            if (!frustum.Intersects(sp))
                continue;
            const f32 distance = Length(sp.center - view.position);
            const f32 screenSize = sp.radius * invTanHalfFov / Max(distance - sp.radius, 1e-3f);
            u32 lod = 0;
            for (u32 l = 1; l < section.lodCount; ++l)
                if (screenSize < mesh->SectionLodAt(section, l).screenSize * m_settings.lodBias)
                    lod = l;
            if (m_settings.forcedLod >= 0)
                lod = Min(u32(m_settings.forcedLod), section.lodCount - 1);
            const asset::CookedSectionLod& range = mesh->SectionLodAt(section, lod);
            DrawItem d;
            d.instance = rows[i];
            d.material = g.material;
            d.mesh = mesh;
            d.indexOffset = range.indexOffset;
            d.indexCount = range.indexCount;
            d.lod = u8(lod);
            d.shader = g.shader;
            if (g.translucent) {
                const MaterialInfo& info = m_assets.GetMaterialInfo(g.material);
                d.pipeline = info.doubleSided ? TranslucentTwoSided : Translucent;
                d.sortKey = ~u64(u32(Min(distance, 65535.0f) * 16.0f) & 0xFFFFF);
                m_translucent.PushBack(d);
                ++m_stats.draws;
                m_stats.triangles += range.indexCount / 3;
            } else {
                d.pipeline = g.pipeline;
                m_velocityDraws.PushBack(d);
            }
        }
    }

    m_stats.gpuDriven = true;
    m_stats.gpuItems = m_itemCount;
    m_stats.gpuGroups = u32(m_activeGroups.Size());
    m_cullViewProj = view.viewProj;
    m_cullCamera = view.position;
    m_cullInvTanHalfFov = invTanHalfFov;
    m_viewConstants.drawSlots = m_cullSlots[frame]->GetSrvIndex();
    m_viewConstants.slotCount = Max(m_slotCount, 1u);
    m_viewConstants.listCapacity = m_listCapacity;
    m_viewConstants.visibleLists = m_visibleLists->GetSrvIndex();
}

void SceneRenderer::AddCullPass(RenderGraph& graph, u32 viewIndex, u32 mode, const Mat4& viewProj, RGBuffer args, RGBuffer lists,
                                RGBuffer candidates, u32 requiredFlags, f32 minRadius, u32 lodOffset)
{
    if (m_itemCount == 0 || m_slotCount == 0)
        return;
    CullParams p = {};
    p.viewProj = viewProj;
    p.occlusionViewProj = mode == 2 ? m_cullViewProj : m_hizViewProj;
    const Frustum fr = Frustum::FromMatrix(viewProj);
    for (u32 i = 0; i < 6; ++i)
        p.planes[i] = i < fr.planeCount ? Vec4(fr.planes[i].normal, fr.planes[i].d) : Vec4(0, 0, 0, 1e30f);
    p.cameraPosition = m_cullCamera;
    p.invTanHalfFov = m_cullInvTanHalfFov;
    p.lodBias = m_settings.lodBias;
    p.forcedLod = m_settings.forcedLod;
    p.itemCount = m_itemCount;
    p.mode = mode;
    const u32 frame = m_device.GetFrameIndex() % FramesInFlight;
    p.items = m_cullItems[frame]->GetSrvIndex();
    p.groups = m_cullGroups[frame]->GetSrvIndex();
    p.slots = m_cullSlots[frame]->GetSrvIndex();
    p.slotCount = m_slotCount;
    p.viewIndex = viewIndex;
    p.listCapacity = m_listCapacity;
    p.requiredFlags = requiredFlags;
    p.minRadius = minRadius;
    p.lodOffset = lodOffset;
    p.hizMips = m_hizMips;
    p.hizSize = Vec2(f32(m_hizSize.x), f32(m_hizSize.y));
    // Phase 1 reads last frame's pyramid, phase 2 this frame's.
    const u32 hizSet = mode == 2 ? m_hizCurrent : (m_hizCurrent ^ 1u);
    const bool useHiz = mode != 0 && m_hiz[hizSet][0];
    RGTexture hizEven, hizOdd;
    if (useHiz) {
        hizEven = graph.ImportTexture(m_hiz[hizSet][0], ResourceState::ShaderResource);
        hizOdd = graph.ImportTexture(m_hiz[hizSet][1], ResourceState::ShaderResource);
    }
    graph.AddPass(
        Name(String::Format("GpuCull{}", viewIndex)).CStr(), PassType::Compute,
        [args, lists, candidates, useHiz, hizEven, hizOdd](PassBuilder& b) {
            b.WriteBuffer(args);
            b.WriteBuffer(lists);
            b.WriteBuffer(candidates);
            if (useHiz) {
                b.ReadTexture(hizEven);
                b.ReadTexture(hizOdd);
            }
        },
        [this, p, useHiz, hizSet](PassContext& ctx) {
            CullParams q = p;
            q.args = m_indirectArgs->GetUavIndex();
            q.lists = m_visibleLists->GetUavIndex();
            q.candidates = m_candidates->GetUavIndex();
            q.hiz = useHiz ? m_hiz[hizSet][0]->GetSrvIndex() : InvalidDescriptor;
            q.hizOdd = useHiz ? m_hiz[hizSet][1]->GetSrvIndex() : InvalidDescriptor;
            ctx.cmd.SetPipeline(m_cullPipeline);
            ctx.cmd.SetConstants(0, &q, sizeof(q));
            ctx.cmd.Dispatch((m_itemCount + 63) / 64);
        });
}

void SceneRenderer::AddCullInitPass(RenderGraph& graph, RGBuffer args, RGBuffer candidates, u32 viewMask)
{
    // Every view region this frame draws from starts with zero instances per slot.
    graph.AddPass(
        "GpuCullInit", PassType::Compute,
        [args, candidates](PassBuilder& b) {
            b.WriteBuffer(args);
            b.WriteBuffer(candidates);
        },
        [this, viewMask](PassContext& ctx) {
            const u32 frame = m_device.GetFrameIndex() % FramesInFlight;
            ctx.cmd.SetPipeline(m_cullInitPipeline);
            for (u32 v = 0; v < GpuViewCount; ++v) {
                if (!((viewMask >> v) & 1u))
                    continue;
                CullParams p = {};
                p.slots = m_cullSlots[frame]->GetSrvIndex();
                p.slotCount = m_slotCount;
                p.args = m_indirectArgs->GetUavIndex();
                p.candidates = v == 0 ? m_candidates->GetUavIndex() : InvalidDescriptor;
                p.viewIndex = v;
                ctx.cmd.SetConstants(0, &p, sizeof(p));
                ctx.cmd.Dispatch((m_slotCount + 63) / 64);
            }
        });
}

void SceneRenderer::DrawIndirect(CommandList& cmd, u32 viewIndex, PipelineVariant variant)
{
    if (m_slotCount == 0)
        return;
    Pipeline* bound = nullptr;
    const DrawPushGpu push;
    for (const Bucket& b : m_buckets) {
        u32 kind = b.pipeline;
        if (variant == VariantShadow) // shadow passes are two-sided anyway
            kind = (kind == Masked || kind == MaskedTwoSided) ? Masked : Opaque;
        Pipeline* p = GetPipeline(kind, variant, b.shader);
        if (!p)
            continue;
        if (p != bound) {
            cmd.SetPipeline(p);
            bound = p;
        }
        cmd.SetIndexBuffer(b.mesh->indexBuffer, b.mesh->index16 ? IndexFormat::UInt16 : IndexFormat::UInt32);
        cmd.SetPushConstants(push);
        cmd.DrawIndexedIndirect(m_indirectArgs, u64(viewIndex * m_slotCount + b.firstSlot) * sizeof(IndirectDrawArgs), b.slotCount);
        ++m_stats.indirectCalls;
    }
}

void SceneRenderer::AddHiZPass(RenderGraph& graph, RGTexture depth)
{
    // (Re)create the pyramid at half the render size: two textures (even / odd levels) per frame parity.
    const UVec2 size(Max(1u, m_renderSize.x / 2), Max(1u, m_renderSize.y / 2));
    if (size != m_hizSize || !m_hiz[0][0]) {
        for (auto& set : m_hiz)
            for (Texture*& t : set)
                m_device.Destroy(t);
        m_hizSize = size;
        m_hizMips = 1;
        while ((Max(size.x, size.y) >> m_hizMips) > 0)
            ++m_hizMips;
        TextureDesc td;
        td.width = size.x;
        td.height = size.y;
        td.mipLevels = m_hizMips;
        td.format = Format::R32_FLOAT;
        td.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess;
        td.debugName = "HiZ";
        for (auto& set : m_hiz)
            for (Texture*& t : set)
                t = m_device.CreateTexture(td);
        m_hizValid = false;
    }
    const u32 set = m_hizCurrent;
    const RGTexture even = graph.ImportTexture(m_hiz[set][0], ResourceState::ShaderResource);
    const RGTexture odd = graph.ImportTexture(m_hiz[set][1], ResourceState::ShaderResource);
    const UVec2 depthSize = m_renderSize;
    graph.AddPass(
        "HiZ", PassType::Compute,
        [depth, even, odd](PassBuilder& b) {
            b.ReadTexture(depth);
            b.WriteTexture(even);
            b.WriteTexture(odd);
        },
        [this, depth, set, depthSize](PassContext& ctx) {
            ctx.cmd.SetPipeline(m_hizPipeline);
            UVec2 src = depthSize;
            for (u32 m = 0; m < m_hizMips; ++m) {
                Texture* dst = m_hiz[set][m & 1];
                Texture* prev = m_hiz[set][(m + 1) & 1];
                const UVec2 dstSize(Max(1u, m_hizSize.x >> m), Max(1u, m_hizSize.y >> m));
                HiZParams p = {};
                if (m == 0) {
                    p.source = ctx.Texture(depth)->GetSrvIndex();
                    p.sourceMip = 0;
                } else {
                    ctx.cmd.Barrier(prev, ResourceState::ShaderResource);
                    p.source = prev->GetSrvIndex();
                    p.sourceMip = m - 1;
                }
                ctx.cmd.Barrier(dst, ResourceState::UnorderedAccess);
                p.target = dst->GetUavIndex(m);
                p.sourceSize = src;
                p.targetSize = dstSize;
                ctx.cmd.SetPushConstants(p);
                ctx.cmd.Dispatch((dstSize.x + 7) / 8, (dstSize.y + 7) / 8);
                src = dstSize;
            }
            ctx.cmd.Barrier(m_hiz[set][0], ResourceState::ShaderResource);
            ctx.cmd.Barrier(m_hiz[set][1], ResourceState::ShaderResource);
        });
}

} // namespace zx::render
