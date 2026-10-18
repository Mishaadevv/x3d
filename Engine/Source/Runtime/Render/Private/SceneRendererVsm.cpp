// Virtual shadow maps of the sun for SceneRenderer (M16), see Shaders/Common/VirtualShadows.hlsli.
//
// Every level is a 128 x 128 window of 128-texel pages centered on the camera (texels double per level). The GPU marks
// the pages the visible pixels need (MarkCS, read back FramesInFlight frames later); here they are mapped to slots of
// a D32 atlas (least recently needed pages evicted) and the new ones, plus those something moved through, are rendered
// within a per-frame budget, coarse levels first. Shading walks from the pixel's level to coarser ones until it finds a
// resident page, so a page that isn't there yet only means a softer shadow for a few frames. Pages stay valid while
// the sun and the casters in them don't move: a static scene renders each page once.
//
// Casters: regular meshes are drawn per page (viewport = the page's atlas rectangle); virtual geometry is culled once
// per level with the nano pipeline and each cluster replicated into the pages it touches (clipped in the vertex shader).

#include "Render/SceneRenderer.h"
#include "Core/Containers/Algorithm.h"
#include "Core/Log/Log.h"
#include "Core/Math/Geometry.h"
#include "Core/Profiler/Profiler.h"
#include "Core/String/Name.h"
#include "RHI/ShaderCompiler.h"

#include <bit>
#include <cmath>

ZX_DEFINE_LOG_CATEGORY_STATIC(LogVsm, Info);

namespace zx::render {

using namespace rhi;

namespace {

constexpr u32 PageTexels = 128, WindowPages = 128, WindowHalf = 64;
constexpr u32 TableHeaderBytes = 512, BuildHeader = 256, LevelEntries = WindowPages * WindowPages; // VirtualShadows.hlsli
constexpr f32 DepthRange = 4000.0f; // meters along the sun's rays on either side of the world origin

struct MarkParams // VirtualShadows.hlsl
{
    Mat4 invViewProj;
    Vec4 lightRows[3];
    Vec3 camera;
    f32 pixelScale;
    f32 texel0;
    u32 levels;
    u32 depth;
    u32 requests;
    u32 table;
    UVec2 size;
    f32 maxDistance;
    Vec4 buildRows[3];
    u32 building;
    u32 pad[3];
};
static_assert(sizeof(MarkParams) == 224); // HLSL constant buffer layout

// Page keys: generation (bit 63), level (56..62), x and y (28 bits each, two's complement).
u64 PageKey(u32 gen, u32 level, i32 x, i32 y)
{
    return (u64(gen & 1u) << 63) | (u64(level & 0x7Fu) << 56) | (u64(u32(x) & 0xFFFFFFFu) << 28) | u64(u32(y) & 0xFFFFFFFu);
}
u32 KeyGen(u64 key) { return u32(key >> 63); }
u32 KeyLevel(u64 key) { return u32(key >> 56) & 0x7Fu; }
i32 KeyX(u64 key) { return i32(u32(key >> 28) << 4) >> 4; }
i32 KeyY(u64 key) { return i32(u32(key) << 4) >> 4; }

/// Light space of a sun direction: x / y across the rays, z along them.
void LightRows(Vec3 sun, Vec4 rows[3])
{
    const Vec3 up = Abs(sun.y) > 0.99f ? Vec3(1, 0, 0) : Vec3(0, 1, 0);
    const Vec3 x = Normalize(Cross(up, sun));
    const Vec3 y = Cross(sun, x);
    rows[0] = Vec4(x, 0.0f);
    rows[1] = Vec4(y, 0.0f);
    rows[2] = Vec4(sun, 0.0f);
}

f32 MaxScaleOf(const Mat4& m)
{
    return Max(Max(Length(m.columns[0].XYZ()), Length(m.columns[1].XYZ())), Length(m.columns[2].XYZ()));
}

/// Light space rectangle [min, min + size] (x right, y down the atlas) and the full depth range -> clip space.
Mat4 RectProjection(const Vec4 rows[3], Vec2 rectMin, Vec2 size)
{
    const Vec4 r0 = Vec4(rows[0].XYZ() * (2.0f / size.x), -rectMin.x * 2.0f / size.x - 1.0f);
    const Vec4 r1 = Vec4(rows[1].XYZ() * (-2.0f / size.y), rectMin.y * 2.0f / size.y + 1.0f);
    const Vec4 r2 = Vec4(rows[2].XYZ() * (-0.5f / DepthRange), 0.5f);
    const Vec4 r3 = Vec4(0, 0, 0, 1);
    return Transpose(Mat4(r0, r1, r2, r3));
}

} // namespace

bool SceneRenderer::InitVsm()
{
    struct Entry
    {
        const char* name;
        ShaderBytecode* out;
        Pipeline** pipeline;
    } entries[] = {{"MarkCS", &m_vsmMarkCS, &m_vsmMarkPipeline}, {"ClearCS", &m_vsmClearCS, &m_vsmClearPipeline}};
    for (const Entry& e : entries) {
        ShaderCompileDesc d;
        d.path = "/Shaders/Lighting/VirtualShadows.hlsl";
        d.entryPoint = e.name;
        d.stage = ShaderStage::Compute;
        Result<ShaderBytecode> r = ShaderCompiler::Compile(d);
        if (!r) {
            ZX_LOG_ERROR(LogVsm, "{}", r.GetError().message);
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
    // Two halves: the shown generation and the one being built.
    m_vsmRequests = m_device.CreateBuffer(BufferDesc{2 * VsmMaxLevels * 512 * 4, BufferUsage::UnorderedAccess | BufferUsage::CopySource,
                                                     MemoryType::GpuOnly, 0, "Vsm.Requests"});
    for (Buffer*& b : m_vsmReadback)
        b = m_device.CreateBuffer(BufferDesc{2 * VsmMaxLevels * 512 * 4, BufferUsage::CopyDest, MemoryType::Readback, 0, "Vsm.Readback"});
    return m_vsmRequests != nullptr;
}

Pipeline* SceneRenderer::GetVsmPipeline(bool masked, u32 shader)
{
    if (m_vsmPipelineBias[0] != m_settings.shadows.depthBias || m_vsmPipelineBias[1] != m_settings.shadows.slopeBias) {
        for (auto& kv : m_vsmPipelines)
            m_device.Destroy(kv.second);
        m_vsmPipelines.Clear();
        m_vsmPipelineBias[0] = m_settings.shadows.depthBias;
        m_vsmPipelineBias[1] = m_settings.shadows.slopeBias;
    }
    const MaterialShaderCache::Shaders* graph = masked && shader != 0 ? m_assets.Shaders().Get(shader) : nullptr;
    const u64 key = (u64(graph ? shader : 0) << 1) | u64(masked);
    Pipeline*& p = *m_vsmPipelines.Insert(key, nullptr).first;
    if (p)
        return p;
    GraphicsPipelineDesc pd;
    pd.vertexShader = masked ? &m_vsNanoVsmAlpha : &m_vsNanoVsm;
    pd.pixelShader = masked ? (graph ? &graph->depthAlphaTest : &m_psDepthAlpha) : nullptr;
    pd.rasterizer.cullMode = CullMode::None;
    pd.rasterizer.depthBias = i32(m_settings.shadows.depthBias);
    pd.rasterizer.slopeScaledDepthBias = m_settings.shadows.slopeBias;
    pd.depthFormat = Format::D32_FLOAT;
    pd.colorCount = 0;
    const String name = String::Format("Vsm.Nano.{}.{}", masked ? "Masked" : "Opaque", shader);
    pd.debugName = name.View();
    p = m_device.CreateGraphicsPipeline(pd);
    return p;
}

void SceneRenderer::PrepareVsm(const RenderView& view, const SceneLighting& lighting)
{
    ZX_PROFILE_SCOPE("SceneRenderer::PrepareVsm");
    const ShadowSettings& ss = m_settings.shadows;
    m_vsmRender.Clear();
    m_vsmRenderLevels = 0;
    m_viewConstants.vsmTable = InvalidDescriptor;
    m_vsmActive = ss.enabled && ss.virtualShadows && m_vsmMarkPipeline && m_vsmRequests;
    m_stats.virtualShadows = m_vsmActive;
    if (!m_vsmActive)
        return;
    ++m_vsmFrame;

    // Configuration: any change starts over.
    const u32 levels = Clamp(ss.vsmLevels, 1u, VsmMaxLevels);
    const f32 texel0 = Max(ss.vsmTexel / Clamp(ss.vsmResolution, 0.125f, 4.0f), 1e-5f);
    const u32 poolPages = Clamp(ss.vsmPoolPages, 64u, 4096u);
    const u32 pagesX = 32, pagesY = (poolPages + pagesX - 1) / pagesX;
    if (!m_vsmAtlas || pagesX != m_vsmAtlasPagesX || pagesY != m_vsmAtlasPagesY || levels != m_vsmLevels || texel0 != m_vsmTexel0) {
        m_device.Destroy(m_vsmAtlas);
        TextureDesc td;
        td.width = pagesX * PageTexels;
        td.height = pagesY * PageTexels;
        td.format = Format::D32_FLOAT;
        td.usage = TextureUsage::DepthStencil | TextureUsage::ShaderResource;
        td.debugName = "Vsm.Atlas";
        m_vsmAtlas = m_device.CreateTexture(td);
        m_vsmAtlasPagesX = pagesX;
        m_vsmAtlasPagesY = pagesY;
        m_vsmLevels = levels;
        m_vsmTexel0 = texel0;
        m_vsmPages.Clear();
        m_vsmFree.Clear();
        for (u32 p = pagesX * pagesY; p-- > 0;)
            m_vsmFree.PushBack(p);
        for (bool& v : m_vsmReadbackValid)
            v = false;
        m_vsmLightDir = Vec3();
    }
    if (!m_vsmAtlas) {
        m_vsmActive = false;
        return;
    }

    // Light space. A turned sun (> ~0.25 degrees) starts a second generation of pages for the new direction; the shown
    // one stays until the new one has all the pages it needs, then they swap - shadows step, they never go missing.
    const Vec3 sun = SafeNormalize(lighting.sunDirection, Vec3(0, -1, 0));
    if (Dot(m_vsmLightDir, m_vsmLightDir) < 0.5f) {
        m_vsmLightDir = sun;
        LightRows(sun, m_vsmRows);
        m_vsmBuilding = false;
    } else if (!m_vsmBuilding && Dot(sun, m_vsmLightDir) < 0.99999f) {
        m_vsmBuilding = true;
        m_vsmBuildDir = sun;
        LightRows(sun, m_vsmBuildRows);
        m_vsmBuildMarked = false;
    }
    if (m_vsmInvalidate) {
        for (auto& kv : m_vsmPages)
            kv.second.dirty = true;
        m_vsmInvalidate = false;
    }
    const u32 buildGen = m_vsmGen ^ 1u;

    // Windows: 128 x 128 pages per level centered on the camera, for each generation.
    auto windows = [&](const Vec4 rows[3], i32 (*origin)[2]) {
        const Vec2 cam = VsmLight(rows, view.position);
        for (u32 k = 0; k < levels; ++k) {
            const f32 pw = VsmPageWorld(k);
            origin[k][0] = i32(std::floor(cam.x / pw)) - i32(WindowHalf);
            origin[k][1] = i32(std::floor(cam.y / pw)) - i32(WindowHalf);
        }
    };
    windows(m_vsmRows, m_vsmOrigin);
    if (m_vsmBuilding)
        windows(m_vsmBuildRows, m_vsmBuildOrigin);
    auto originOf = [&](u32 gen) { return gen == m_vsmGen ? m_vsmOrigin : m_vsmBuildOrigin; };
    auto rowsOf = [&](u32 gen) -> const Vec4* { return gen == m_vsmGen ? m_vsmRows : m_vsmBuildRows; };
    auto genLive = [&](u32 gen) { return gen == m_vsmGen || (m_vsmBuilding && gen == buildGen); };

    // Needed pages: what the GPU marked (FramesInFlight frames ago) plus a coarse level covering the whole shadow
    // distance, so every pixel always finds some page.
    Array<u64> needed;
    HashSet<u64> neededSet;
    auto need = [&](u32 gen, u32 k, i32 x, i32 y) {
        const i32 (*origin)[2] = originOf(gen);
        if (x < origin[k][0] || y < origin[k][1] || x >= origin[k][0] + i32(WindowPages) || y >= origin[k][1] + i32(WindowPages))
            return;
        const u64 key = PageKey(gen, k, x, y);
        if (neededSet.Insert(key))
            needed.PushBack(key);
    };
    u32 coarse = levels - 1;
    for (u32 k = 0; k < levels; ++k)
        if (VsmPageWorld(k) * 4.0f >= ss.vsmDistance) {
            coarse = k;
            break;
        }
    for (u32 gen : {m_vsmGen, buildGen}) {
        if (!genLive(gen))
            continue;
        const f32 pw = VsmPageWorld(coarse);
        const i32 r = i32(std::ceil(ss.vsmDistance / pw));
        const Vec2 cam = VsmLight(rowsOf(gen), view.position);
        const i32 cx = i32(std::floor(cam.x / pw)), cy = i32(std::floor(cam.y / pw));
        for (i32 y = cy - r; y <= cy + r; ++y)
            for (i32 x = cx - r; x <= cx + r; ++x)
                need(gen, coarse, x, y);
    }
    const u32 slot = m_device.GetFrameIndex() % FramesInFlight;
    if (m_vsmReadbackValid[slot] && m_vsmReadTexel[slot] == texel0) {
        const u32* bits = static_cast<const u32*>(m_vsmReadback[slot]->GetMappedData());
        const u32 readLevels = Min(m_vsmReadLevels[slot], levels);
        for (u32 half = 0; half < (m_vsmReadBuild[slot] ? 2u : 1u); ++half) {
            const u32 gen = m_vsmReadGen[slot][half];
            if (!genLive(gen))
                continue; // marked for a generation that is gone
            if (m_vsmBuilding && gen == buildGen)
                m_vsmBuildMarked = true;
            for (u32 k = 0; k < readLevels; ++k)
                for (u32 w = 0; w < 512; ++w) {
                    u32 word = bits[(half * m_vsmReadLevels[slot] + k) * 512 + w];
                    while (word) {
                        const u32 b = u32(std::countr_zero(word));
                        word &= word - 1;
                        const u32 index = w * 32 + b;
                        need(gen, k, m_vsmReadOrigin[slot][half][k][0] + i32(index % WindowPages),
                             m_vsmReadOrigin[slot][half][k][1] + i32(index / WindowPages));
                    }
                }
        }
    }
    // This frame's marks land in this slot (with this frame's windows).
    m_vsmReadbackValid[slot] = false;
    m_vsmReadLevels[slot] = levels;
    m_vsmReadTexel[slot] = texel0;
    m_vsmReadBuild[slot] = m_vsmBuilding;
    m_vsmReadGen[slot][0] = m_vsmGen;
    m_vsmReadGen[slot][1] = buildGen;
    for (u32 k = 0; k < levels; ++k)
        for (u32 c = 0; c < 2; ++c) {
            m_vsmReadOrigin[slot][0][k][c] = m_vsmOrigin[k][c];
            m_vsmReadOrigin[slot][1][k][c] = m_vsmBuildOrigin[k][c];
        }

    // Moved casters (and their old places) dirty the pages under them on every level of every generation.
    for (const Sphere& s : m_movedCasters)
        for (u32 gen : {m_vsmGen, buildGen}) {
            if (!genLive(gen))
                continue;
            const Vec2 c = VsmLight(rowsOf(gen), s.center);
            for (u32 k = 0; k < levels; ++k) {
                const f32 pw = VsmPageWorld(k);
                const i32 x0 = i32(std::floor((c.x - s.radius) / pw)), x1 = i32(std::floor((c.x + s.radius) / pw));
                const i32 y0 = i32(std::floor((c.y - s.radius) / pw)), y1 = i32(std::floor((c.y + s.radius) / pw));
                if (i64(x1 - x0 + 1) * i64(y1 - y0 + 1) > i64(m_vsmPages.Size())) {
                    for (auto& kv : m_vsmPages)
                        if (KeyGen(kv.first) == gen && KeyLevel(kv.first) == k) {
                            const i32 px = KeyX(kv.first), py = KeyY(kv.first);
                            if (px >= x0 && px <= x1 && py >= y0 && py <= y1)
                                kv.second.dirty = true;
                        }
                    continue;
                }
                for (i32 y = y0; y <= y1; ++y)
                    for (i32 x = x0; x <= x1; ++x)
                        if (VsmPage* p = m_vsmPages.Find(PageKey(gen, k, x, y)))
                            p->dirty = true;
            }
        }

    // Map the needed pages (the generation being built first, coarse levels first when the pool runs short), evicting
    // the least recently needed others.
    Sort(needed, [&](u64 a, u64 b) {
        const bool ba = KeyGen(a) != m_vsmGen, bb = KeyGen(b) != m_vsmGen;
        if (ba != bb)
            return ba;
        return KeyLevel(a) > KeyLevel(b);
    });
    Array<Pair<u64, u64>> evictable; // (last needed, key), built on first demand
    usize evictAt = 0;
    bool evictableBuilt = false;
    for (u64 key : needed) {
        VsmPage* page = m_vsmPages.Find(key);
        if (!page) {
            if (m_vsmFree.Empty()) {
                if (!evictableBuilt) {
                    for (const auto& kv : m_vsmPages)
                        if (!neededSet.Contains(kv.first))
                            evictable.PushBack({genLive(KeyGen(kv.first)) ? kv.second.lastRequested : 0, kv.first});
                    Sort(evictable, [](const auto& a, const auto& b) { return a.first < b.first; });
                    evictableBuilt = true;
                }
                if (evictAt >= evictable.Size())
                    break; // the pool is full of needed pages: finer levels wait
                const u64 victim = evictable[evictAt++].second;
                m_vsmFree.PushBack(m_vsmPages.Find(victim)->phys);
                m_vsmPages.Remove(victim);
            }
            VsmPage fresh;
            fresh.phys = m_vsmFree.Back();
            m_vsmFree.PopBack();
            page = m_vsmPages.Insert(key, fresh).first;
        }
        page->lastRequested = m_vsmFrame;
    }
    // What the rendering generation still lacks.
    Array<u64> toRender;
    auto collect = [&](u32 gen) {
        toRender.Clear();
        for (u64 key : needed)
            if (KeyGen(key) == gen)
                if (const VsmPage* page = m_vsmPages.Find(key); page && (!page->rendered || page->dirty))
                    toRender.PushBack(key);
    };
    if (m_vsmBuilding) {
        collect(buildGen);
        // Ready when every needed page has been drawn once (moving casters keep dirtying some; they redraw later).
        bool complete = m_vsmBuildMarked;
        for (u64 key : toRender)
            complete &= m_vsmPages.Find(key)->rendered;
        if (complete) {
            // The new direction has everything it needs: show it, drop the old pages.
            for (u32 r = 0; r < 3; ++r)
                m_vsmRows[r] = m_vsmBuildRows[r];
            m_vsmLightDir = m_vsmBuildDir;
            for (u32 k = 0; k < levels; ++k)
                for (u32 c = 0; c < 2; ++c)
                    m_vsmOrigin[k][c] = m_vsmBuildOrigin[k][c];
            Array<u64> old;
            for (const auto& kv : m_vsmPages)
                if (KeyGen(kv.first) == m_vsmGen)
                    old.PushBack(kv.first);
            for (u64 key : old) {
                m_vsmFree.PushBack(m_vsmPages.Find(key)->phys);
                m_vsmPages.Remove(key);
            }
            m_vsmGen = buildGen;
            m_vsmBuilding = false;
            collect(m_vsmGen);
        }
    } else {
        collect(m_vsmGen);
    }
    const u32 renderGen = m_vsmBuilding ? buildGen : m_vsmGen;
    m_vsmRenderRows = renderGen == m_vsmGen ? m_vsmRows : m_vsmBuildRows;
    m_vsmRenderOrigin = renderGen == m_vsmGen ? m_vsmOrigin : m_vsmBuildOrigin;
    // Render order, coarse to fine within each kind: pages never drawn, and pages that show stale shadows of moving
    // casters. Each kind gets its own budget, so animated casters can't starve the rest (and vice versa); while a new
    // generation builds, its missing pages come first (its stale ones don't show yet).
    const u32 budget = Max(ss.vsmPagesPerFrame, 1u);
    u32 pending = 0;
    {
        Array<u64> stale, fresh;
        for (u64 key : toRender)
            (m_vsmPages.Find(key)->rendered ? stale : fresh).PushBack(key);
        auto coarseFirst = [](u64 a, u64 b) { return KeyLevel(a) > KeyLevel(b); };
        StableSort(stale, coarseFirst);
        StableSort(fresh, coarseFirst);
        const usize freshCount = Min<usize>(fresh.Size(), m_vsmBuilding ? budget * 2 : budget);
        const usize staleCount = Min<usize>(stale.Size(), budget * 2 - freshCount);
        pending = u32(fresh.Size() - freshCount + stale.Size() - staleCount);
        toRender.Clear();
        toRender.Append(Span<const u64>(fresh.Data(), freshCount));
        toRender.Append(Span<const u64>(stale.Data(), staleCount));
    }

    // Casters in light space (regular meshes; virtual geometry is culled per level on the GPU).
    struct Caster
    {
        const InstanceRef* ref;
        Vec2 light;
        f32 radius;
        f32 scale;
    };
    Array<Caster> casters;
    if (!toRender.Empty())
        for (const InstanceRef& c : m_casters)
            if (!c.nano) {
                const f32 scale = MaxScaleOf(c.instance->world);
                const f32 radius = c.mesh->sphere.radius * scale * (c.vertexBuffer != InvalidIndex ? 1.5f : 1.0f);
                casters.PushBack(Caster{&c, VsmLight(m_vsmRenderRows, c.instance->world.TransformPoint(c.mesh->sphere.center)), radius, scale});
            }
    for (u64 key : toRender) {
        VsmPage& page = *m_vsmPages.Find(key);
        const u32 k = KeyLevel(key);
        const i32 x = KeyX(key), y = KeyY(key);
        const f32 pw = VsmPageWorld(k), texel = pw / f32(PageTexels);
        VsmRenderPage rp;
        rp.level = k;
        rp.x = x;
        rp.y = y;
        rp.phys = page.phys;
        rp.viewProj = RectProjection(m_vsmRenderRows, Vec2(f32(x), f32(y)) * pw, Vec2(pw));
        const Vec2 lo = Vec2(f32(x), f32(y)) * pw, hi = lo + Vec2(pw);
        for (const Caster& c : casters) {
            if (c.light.x + c.radius < lo.x || c.light.x - c.radius > hi.x || c.light.y + c.radius < lo.y || c.light.y - c.radius > hi.y)
                continue;
            const RenderInstance& inst = *c.ref->instance;
            for (const asset::CookedMeshSection& section : c.ref->mesh->sections) {
                const Vec2 sc = VsmLight(m_vsmRenderRows, inst.world.TransformPoint(section.bounds.Center()));
                const f32 sr = Length(section.bounds.Extents()) * c.scale;
                if (sc.x + sr < lo.x || sc.x - sr > hi.x || sc.y + sr < lo.y || sc.y - sr > hi.y)
                    continue;
                const AssetId materialId = section.materialSlot < inst.materials.Size() ? inst.materials[section.materialSlot] : AssetId();
                const u32 material = m_assets.GetMaterialIndex(materialId);
                const MaterialInfo& info = m_assets.GetMaterialInfo(material);
                if (info.blend == MaterialBlend::Translucent)
                    continue;
                const bool masked = info.blend == MaterialBlend::Masked;
                u32 lod = 0;
                while (lod + 1 < section.lodCount && c.ref->mesh->SectionLodAt(section, lod + 1).error * c.scale <= texel * 1.5f)
                    ++lod;
                const asset::CookedSectionLod& range = c.ref->mesh->SectionLodAt(section, lod);
                DrawItem d{u64(masked) << 56 | material, c.ref->index, material, c.ref->mesh, range.indexOffset, range.indexCount,
                           u8(masked ? Masked : Opaque), u8(lod), info.shader};
                d.vertexBuffer = c.ref->vertexBuffer;
                rp.draws.PushBack(d);
            }
        }
        Sort(rp.draws, [](const DrawItem& a, const DrawItem& b) { return a.sortKey < b.sortKey; });
        m_stats.vsmDraws += u32(rp.draws.Size());
        m_vsmRenderLevels |= 1u << k;
        m_vsmRender.PushBack(move(rp));
        page.rendered = true; // drawn before anything samples it this frame
        page.dirty = false;
    }
    // Per rendering level: the light-space rectangle around its pages (virtual geometry is culled against it).
    for (u32 k = 0; k < levels; ++k) {
        if (!(m_vsmRenderLevels & (1u << k)))
            continue;
        i32 x0 = INT32_MAX, y0 = INT32_MAX, x1 = INT32_MIN, y1 = INT32_MIN;
        for (const VsmRenderPage& rp : m_vsmRender)
            if (rp.level == k) {
                x0 = Min(x0, rp.x);
                y0 = Min(y0, rp.y);
                x1 = Max(x1, rp.x);
                y1 = Max(y1, rp.y);
            }
        const f32 pw = VsmPageWorld(k);
        m_vsmLevelViewProj[k] = RectProjection(m_vsmRenderRows, Vec2(f32(x0), f32(y0)) * pw, Vec2(f32(x1 - x0 + 1), f32(y1 - y0 + 1)) * pw);
    }

    // Table: level headers, page entries (resident pages of each window), per rendering level a window -> render page
    // map (cluster replication), render page rectangles.
    u32 bytes = TableHeaderBytes + levels * LevelEntries * 4;
    for (u32 k = 0; k < levels; ++k)
        if (m_vsmRenderLevels & (1u << k)) {
            m_vsmMapOffset[k] = bytes;
            bytes += LevelEntries * 4;
        }
    m_vsmRenderPagesOffset = bytes;
    bytes += u32(m_vsmRender.Size()) * 16 + 16;
    if (bytes > m_vsmTableBytes || !m_vsmTables[0]) {
        u64 c = Max(m_vsmTableBytes, u64(1) << 20);
        while (c < bytes)
            c *= 2;
        for (Buffer*& b : m_vsmTables) {
            m_device.Destroy(b);
            b = m_device.CreateBuffer(BufferDesc{c, BufferUsage::ShaderResource, MemoryType::Upload, 0, "Vsm.Table"});
        }
        m_vsmTableBytes = c;
    }
    const u32 frame = m_device.GetFrameIndex() % FramesInFlight;
    u8* data = static_cast<u8*>(m_vsmTables[frame]->GetMappedData());
    MemZero(data, TableHeaderBytes + usize(levels) * LevelEntries * 4);
    for (u32 k = 0; k < levels; ++k) {
        const u32 header[4] = {u32(m_vsmOrigin[k][0]), u32(m_vsmOrigin[k][1]), std::bit_cast<u32>(VsmPageWorld(k)), 0};
        MemCopy(data + k * 16, header, 16);
        const u32 build[4] = {u32(m_vsmBuildOrigin[k][0]), u32(m_vsmBuildOrigin[k][1]), std::bit_cast<u32>(VsmPageWorld(k)), 0};
        MemCopy(data + BuildHeader + k * 16, m_vsmBuilding ? build : header, 16);
    }
    u32* entries = reinterpret_cast<u32*>(data + TableHeaderBytes);
    u32 mapped = 0;
    for (const auto& kv : m_vsmPages) {
        if (!kv.second.rendered || KeyGen(kv.first) != m_vsmGen)
            continue;
        ++mapped;
        const u32 k = KeyLevel(kv.first);
        const i32 x = KeyX(kv.first) - m_vsmOrigin[k][0];
        const i32 y = KeyY(kv.first) - m_vsmOrigin[k][1];
        if (k < levels && x >= 0 && y >= 0 && x < i32(WindowPages) && y < i32(WindowPages))
            entries[k * LevelEntries + u32(y) * WindowPages + u32(x)] = kv.second.phys + 1;
    }
    for (u32 k = 0; k < levels; ++k)
        if (m_vsmRenderLevels & (1u << k))
            MemSet(data + m_vsmMapOffset[k], 0xFF, LevelEntries * 4);
    for (u32 i = 0; i < m_vsmRender.Size(); ++i) {
        const VsmRenderPage& rp = m_vsmRender[i];
        const f32 pw = VsmPageWorld(rp.level);
        const u32 local = u32(rp.y - m_vsmRenderOrigin[rp.level][1]) * WindowPages + u32(rp.x - m_vsmRenderOrigin[rp.level][0]);
        MemCopy(data + m_vsmMapOffset[rp.level] + local * 4, &i, 4);
        const u32 record[4] = {std::bit_cast<u32>(f32(rp.x) * pw), std::bit_cast<u32>(f32(rp.y) * pw), std::bit_cast<u32>(pw),
                               (rp.phys % m_vsmAtlasPagesX) | ((rp.phys / m_vsmAtlasPagesX) << 16)};
        MemCopy(data + m_vsmRenderPagesOffset + i * 16, record, 16);
    }

    ViewConstants& vc = m_viewConstants;
    for (u32 r = 0; r < 3; ++r)
        vc.vsmLightRows[r] = m_vsmRows[r];
    vc.vsmTable = m_vsmTables[frame]->GetSrvIndex();
    vc.vsmAtlas = m_vsmAtlas->GetSrvIndex();
    vc.vsmLevels = levels;
    vc.vsmTexel0 = texel0;
    vc.vsmPixelScale = 2.0f * Tan(view.fovY * 0.5f) / f32(Max(view.height, 1u));
    vc.vsmDepthScale = 0.5f / DepthRange;
    vc.vsmNormalBias = ss.vsmNormalBias;
    vc.vsmMaxDistance = ss.vsmDistance;
    vc.vsmInvAtlasSize = Vec2(1.0f / f32(m_vsmAtlasPagesX * PageTexels), 1.0f / f32(m_vsmAtlasPagesY * PageTexels));
    vc.vsmPagesPerRow = m_vsmAtlasPagesX;
    m_stats.vsmMappedPages = mapped;
    m_stats.vsmRequestedPages = u32(needed.Size());
    m_stats.vsmRenderedPages = u32(m_vsmRender.Size());
    m_stats.vsmPendingPages = pending;
    m_stats.vsmBuilding = m_vsmBuilding;
}

void SceneRenderer::AddVsmPasses(RenderGraph& graph, RGTexture atlas, const NanoBuffers* nano)
{
    if (!m_vsmActive || m_vsmRender.Empty())
        return;
    // Clear the pages and draw the regular casters into each (viewport = the page's atlas rectangle).
    graph.AddPass(
        "VsmPages", PassType::Graphics,
        [atlas](PassBuilder& b) {
            b.DepthStencil(atlas, LoadOp::Load, 0.0f);
            b.SideEffects(); // persistent cache
        },
        [this](PassContext& ctx) {
            ViewConstants pageView = m_viewConstants;
            for (const VsmRenderPage& rp : m_vsmRender) {
                const u32 x = (rp.phys % m_vsmAtlasPagesX) * PageTexels, y = (rp.phys / m_vsmAtlasPagesX) * PageTexels;
                ctx.cmd.SetViewport(Viewport{f32(x), f32(y), f32(PageTexels), f32(PageTexels), 0.0f, 1.0f});
                ctx.cmd.SetScissor(i32(x), i32(y), PageTexels, PageTexels);
                ctx.cmd.SetPipeline(m_clearTilePipeline); // depth 0 = far (reversed-Z)
                ctx.cmd.Draw(3);
                if (rp.draws.Empty())
                    continue;
                pageView.viewProj = rp.viewProj;
                ctx.cmd.SetConstants(1, &pageView, sizeof(pageView));
                Pipeline* bound = nullptr;
                for (const DrawItem& d : rp.draws)
                    Draw(ctx.cmd, d, VariantShadow, bound);
            }
        });
    if (!nano)
        return;
    // Virtual geometry: per level, the clusters of its cut replicated into the pages being rendered.
    const NanoBuffers nb = *nano;
    for (u32 k = 0; k < m_vsmLevels; ++k) {
        const u32 view = GpuViewCount + k;
        if (!(m_vsmRenderLevels & (1u << k)) || m_nanoViews[view].vsmLevel != k || m_nanoViews[view].threads == 0)
            continue;
        AddNanoCullPass(graph, view, 0, m_vsmLevelViewProj[k], nb);
        const UVec2 size(m_vsmAtlasPagesX * PageTexels, m_vsmAtlasPagesY * PageTexels);
        graph.AddPass(
            Name(String::Format("VsmNano{}", k)).CStr(), PassType::Graphics,
            [atlas, nb](PassBuilder& b) {
                b.DepthStencil(atlas, LoadOp::Load, 0.0f);
                b.ReadBuffer(nb.args, ResourceState::IndirectArgument);
                b.ReadBuffer(nb.indices, ResourceState::IndexBuffer);
                b.ReadBuffer(nb.visible);
                b.SideEffects();
            },
            [this, view, size](PassContext& ctx) {
                ctx.cmd.SetViewport(Viewport{0.0f, 0.0f, f32(size.x), f32(size.y), 0.0f, 1.0f});
                ctx.cmd.SetScissor(0, 0, size.x, size.y);
                ViewConstants pageView = m_viewConstants; // the generation being rendered (may differ from the shown one)
                for (u32 r = 0; r < 3; ++r)
                    pageView.vsmLightRows[r] = m_vsmRenderRows[r];
                ctx.cmd.SetConstants(1, &pageView, sizeof(pageView));
                DrawNano(ctx.cmd, view, VariantShadow);
            });
    }
}

void SceneRenderer::AddVsmMarkPass(RenderGraph& graph, RGTexture depth)
{
    if (!m_vsmActive)
        return;
    const u32 slot = m_device.GetFrameIndex() % FramesInFlight;
    m_vsmReadbackValid[slot] = true;
    MarkParams p = {};
    p.invViewProj = m_viewConstants.invViewProj;
    for (u32 r = 0; r < 3; ++r)
        p.lightRows[r] = m_vsmRows[r];
    p.camera = m_viewConstants.cameraPosition;
    p.pixelScale = m_viewConstants.vsmPixelScale;
    p.texel0 = m_vsmTexel0;
    p.levels = m_vsmLevels;
    p.table = m_viewConstants.vsmTable;
    p.size = m_renderSize;
    p.maxDistance = m_settings.shadows.vsmDistance;
    p.building = m_vsmBuilding ? 1u : 0u;
    for (u32 r = 0; r < 3; ++r)
        p.buildRows[r] = m_vsmBuildRows[r];
    const RGBuffer requests = graph.ImportBuffer(m_vsmRequests, ResourceState::UnorderedAccess);
    graph.AddPass(
        "VsmMark", PassType::Compute,
        [depth, requests](PassBuilder& b) {
            b.ReadTexture(depth);
            b.WriteBuffer(requests);
            b.SideEffects();
        },
        [this, p, depth, slot](PassContext& ctx) {
            MarkParams q = p;
            q.depth = ctx.Texture(depth)->GetSrvIndex();
            q.requests = m_vsmRequests->GetUavIndex();
            ctx.cmd.SetPipeline(m_vsmClearPipeline);
            ctx.cmd.SetConstants(0, &q, sizeof(q));
            ctx.cmd.Dispatch((q.levels * 512 * 2 + 63) / 64);
            ctx.cmd.Barrier(m_vsmRequests, ResourceState::UnorderedAccess);
            ctx.cmd.SetPipeline(m_vsmMarkPipeline);
            ctx.cmd.SetConstants(0, &q, sizeof(q));
            ctx.cmd.Dispatch((q.size.x / 2 + 8) / 8, (q.size.y / 2 + 8) / 8);
            ctx.cmd.CopyBuffer(m_vsmReadback[slot], 0, m_vsmRequests, 0, u64(q.levels) * 512 * 4 * 2);
            ctx.cmd.Barrier(m_vsmRequests, ResourceState::UnorderedAccess);
        });
}

} // namespace zx::render
