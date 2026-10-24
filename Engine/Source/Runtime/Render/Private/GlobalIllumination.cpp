#include "Render/GlobalIllumination.h"
#include "Core/FileSystem/FileSystem.h"
#include "Core/Log/Log.h"
#include "Core/Serialization/Binary.h"
#include "Core/Math/Quat.h"
#include "Core/Math/Random.h"
#include "RHI/ShaderCompiler.h"

#include <bit>
#include <cmath>

namespace zx::render {

using namespace rhi;

ZX_DEFINE_LOG_CATEGORY_STATIC(LogGI, Info);

namespace {

// Keep in sync with BuildParams (Shaders/GI/GlobalSDF.hlsl).
struct SdfBuildParams
{
    Vec4 worldRows[3];
    Vec4 levelMin;
    u32 vertexBuffer, indexBuffer, firstIndex, triangleCount;
    u32 index16, materialBuffer, materialIndex, level;
    u32 albedoUav, normalUav, seedsUav, seedsInUav;
    u32 distanceUav, step, maxCells, pad;
};
static_assert(sizeof(SdfBuildParams) == 128);

// Keep in sync with GlobalSDF (Shaders/GI/GlobalSDF.hlsli).
struct GlobalSdfConstants
{
    Vec4 levelMin[GlobalIllumination::MaxLevels];
    u32 distanceTex, albedoTex, normalTex, levels;
};
static_assert(sizeof(GlobalSdfConstants) == 80);

// Keep in sync with ProbeParams (Shaders/GI/DDGI.hlsl).
struct ProbeParams
{
    GIVolumeConstants volume;
    GlobalSdfConstants sdf;
    Vec4 rotation[3];
    Vec3 sunIrradiance;
    u32 raysPerProbe;
    Vec3 dirToSun;
    u32 rayBuffer;
    Vec3 skyIrradianceScale;
    u32 skySH;
    Vec3 skyColor;
    f32 ambientIntensity;
    Vec3 groundColor;
    f32 ambientFloor;
    IVec3 prevOriginIndex;
    f32 hysteresis;
    u32 irradianceUav, distanceUav, frame, probeStride;
    f32 exposureRatio;
    u32 lightBuffer, lightCount, historyValid;
    f32 sunShadowDistance, energy;
    u32 cascadeCount;
    u32 baking;
    Mat4 shadowMatrices[SceneRenderer::MaxCascades];
    u32 shadowMaps[SceneRenderer::MaxCascades];
    u32 cubeSrv[8];
    u32 cubeUav[8];
    u32 cubeValid;
    u32 compactStride, phase;
    u32 probeDataUav;
    u32 flagsBuffer;
    u32 pad3[3];
};
static_assert(sizeof(ProbeParams) == 720);

constexpr u32 IrradianceTexels = 8, DistanceTexels = 16;
constexpr u32 SnapVoxels = 16; // levels move in steps of this many voxels

u64 HashFloats(u64 h, const f32* v, u32 n)
{
    for (u32 i = 0; i < n; ++i)
        h = HashCombine(h, std::bit_cast<u32>(v[i]));
    return h;
}

IVec3 FloorDiv(Vec3 p, f32 s) { return IVec3(i32(std::floor(p.x / s)), i32(std::floor(p.y / s)), i32(std::floor(p.z / s))); }

} // namespace

GlobalIllumination::GlobalIllumination(Device& device, RenderAssets& assets) : m_device(device), m_assets(assets) {}

GlobalIllumination::~GlobalIllumination()
{
    DestroyResources();
    for (Texture* t : m_bakedTextures)
        m_device.Destroy(t);
    for (Buffer* b : m_bake.readback)
        m_device.Destroy(b);
    for (Pipeline* p : {m_clearPipeline, m_voxelizePipeline, m_jumpFloodPipeline, m_finalizePipeline, m_tracePipeline, m_irradiancePipeline,
                        m_distancePipeline, m_cubePipeline, m_ssgiTracePipeline, m_ssgiTemporalPipeline})
        m_device.Destroy(p);
    for (Texture* t : {m_ssgiTrace, m_ssgi[0], m_ssgi[1]})
        m_device.Destroy(t);
}

bool GlobalIllumination::Init()
{
    struct Job
    {
        ShaderBytecode* out;
        Pipeline** pipeline;
        const char* path;
        const char* entry;
    } jobs[] = {{&m_clearCS, &m_clearPipeline, "/Shaders/GI/GlobalSDF.hlsl", "ClearCS"},
                {&m_voxelizeCS, &m_voxelizePipeline, "/Shaders/GI/GlobalSDF.hlsl", "VoxelizeCS"},
                {&m_jumpFloodCS, &m_jumpFloodPipeline, "/Shaders/GI/GlobalSDF.hlsl", "JumpFloodCS"},
                {&m_finalizeCS, &m_finalizePipeline, "/Shaders/GI/GlobalSDF.hlsl", "FinalizeCS"},
                {&m_traceCS, &m_tracePipeline, "/Shaders/GI/DDGI.hlsl", "TraceCS"},
                {&m_irradianceCS, &m_irradiancePipeline, "/Shaders/GI/DDGI.hlsl", "UpdateIrradianceCS"},
                {&m_distanceCS, &m_distancePipeline, "/Shaders/GI/DDGI.hlsl", "UpdateDistanceCS"},
                {&m_cubeCS, &m_cubePipeline, "/Shaders/GI/DDGI.hlsl", "AmbientCubeCS"},
                {&m_ssgiTraceCS, &m_ssgiTracePipeline, "/Shaders/GI/SSGI.hlsl", "TraceCS"},
                {&m_ssgiTemporalCS, &m_ssgiTemporalPipeline, "/Shaders/GI/SSGI.hlsl", "TemporalCS"}};
    for (const Job& j : jobs) {
        ShaderCompileDesc d;
        d.path = j.path;
        d.entryPoint = j.entry;
        d.stage = ShaderStage::Compute;
        Result<ShaderBytecode> r = ShaderCompiler::Compile(d);
        if (!r) {
            ZX_LOG_ERROR(LogGI, "{}", r.GetError().message);
            return false;
        }
        *j.out = move(*r);
        ComputePipelineDesc cd;
        cd.computeShader = j.out;
        cd.debugName = j.entry;
        *j.pipeline = m_device.CreateComputePipeline(cd);
        if (!*j.pipeline)
            return false;
    }
    return true;
}

void GlobalIllumination::DestroyResources()
{
    for (Texture** t : {&m_distance, &m_albedo, &m_normal, &m_irradiance, &m_probeDistance, &m_probeData}) {
        m_device.Destroy(*t);
        *t = nullptr;
    }
    for (Texture*& t : m_cubes) {
        m_device.Destroy(t);
        t = nullptr;
    }
    for (Buffer** b : {&m_seeds[0], &m_seeds[1], &m_rays, &m_flags}) {
        m_device.Destroy(*b);
        *b = nullptr;
    }
    m_levelCount = 0;
    m_probeCounts = UVec3();
    m_rayCount = 0;
}

void GlobalIllumination::Reset()
{
    for (Level& l : m_levels)
        l = Level{};
    m_probesValid = false;
}

void GlobalIllumination::EnsureResources(const GISettings& settings)
{
    const u32 levels = Clamp(settings.levels, 1u, MaxLevels);
    const f32 voxel = Clamp(settings.voxelSize, 0.05f, 4.0f);
    if (!m_distance || levels != m_levelCount) {
        for (Texture** t : {&m_distance, &m_albedo, &m_normal}) {
            m_device.Destroy(*t);
            *t = nullptr;
        }
        TextureDesc d;
        d.dimension = TextureDimension::Texture3D;
        d.width = SdfResolution;
        d.height = SdfResolution;
        d.depthOrArraySize = SdfResolution * levels;
        d.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess;
        d.format = Format::R8_UNORM;
        d.debugName = "GI.Distance";
        m_distance = m_device.CreateTexture(d);
        d.format = Format::R8G8B8A8_UNORM;
        d.debugName = "GI.Albedo";
        m_albedo = m_device.CreateTexture(d);
        d.debugName = "GI.Normal";
        m_normal = m_device.CreateTexture(d);
        if (!m_seeds[0]) {
            const u64 bytes = u64(SdfResolution) * SdfResolution * SdfResolution * 4;
            for (Buffer*& b : m_seeds)
                b = m_device.CreateBuffer(BufferDesc{bytes, BufferUsage::ShaderResource | BufferUsage::UnorderedAccess, MemoryType::GpuOnly, 0, "GI.Seeds"});
        }
        m_levelCount = levels;
        Reset();
    }
    if (voxel != m_voxel) {
        m_voxel = voxel;
        Reset();
    }
    const UVec3 counts(Clamp(settings.probeCounts.x, 2u, 64u), Clamp(settings.probeCounts.y, 2u, 32u), Clamp(settings.probeCounts.z, 2u, 64u));
    const u32 rays = Clamp(settings.raysPerProbe, 16u, 256u);
    if (!m_irradiance || counts != m_probeCounts || rays != m_rayCount) {
        for (Texture** t : {&m_irradiance, &m_probeDistance}) {
            m_device.Destroy(*t);
            *t = nullptr;
        }
        m_device.Destroy(m_rays);
        TextureDesc d;
        d.width = counts.x * counts.y * IrradianceTexels;
        d.height = counts.z * IrradianceTexels;
        d.format = Format::R16G16B16A16_FLOAT;
        d.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess;
        d.debugName = "GI.ProbeIrradiance";
        m_irradiance = m_device.CreateTexture(d);
        d.width = counts.x * counts.y * DistanceTexels;
        d.height = counts.z * DistanceTexels;
        d.format = Format::R16G16_FLOAT;
        d.debugName = "GI.ProbeDistance";
        m_probeDistance = m_device.CreateTexture(d);
        m_device.Destroy(m_probeData);
        d.width = counts.x * counts.y;
        d.height = counts.z;
        d.format = Format::R16G16B16A16_FLOAT;
        d.debugName = "GI.ProbeData";
        m_probeData = m_device.CreateTexture(d);
        for (Texture*& t : m_cubes) {
            m_device.Destroy(t);
            TextureDesc cd;
            cd.dimension = TextureDimension::Texture3D;
            cd.width = counts.x;
            cd.height = counts.y;
            cd.depthOrArraySize = counts.z;
            cd.format = Format::R16G16B16A16_FLOAT;
            cd.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess;
            cd.debugName = "GI.AmbientCube";
            t = m_device.CreateTexture(cd);
        }
        const u64 probes = u64(counts.x) * counts.y * counts.z;
        m_device.Destroy(m_flags);
        m_flags = m_device.CreateBuffer(BufferDesc{probes * 4, BufferUsage::ShaderResource | BufferUsage::UnorderedAccess, MemoryType::GpuOnly, 0, "GI.ProbeFlags"});
        m_rays = m_device.CreateBuffer(
            BufferDesc{probes * rays * 16, BufferUsage::ShaderResource | BufferUsage::UnorderedAccess, MemoryType::GpuOnly, 0, "GI.ProbeRays"});
        m_probeCounts = counts;
        m_rayCount = rays;
        m_probesValid = false;
    }
}

void GlobalIllumination::Prepare(Span<const RenderInstance> instances, Vec3 cameraPosition, f32 exposure, const GISettings& settings,
                                 SceneLighting& lighting)
{
    m_stats = Stats{};
    m_rebuild = ~0u;
    m_voxelDraws.Clear();
    if (m_bake.state == BakeState::Readback)
        FinishBakeReadback();
    // Screen-space GI of last frame (with baked probes too).
    m_ssgiEnabled = settings.enabled && settings.screenSpace && m_ssgiTracePipeline;
    if (m_ssgiEnabled && m_ssgiValid) {
        lighting.ssgiTexture = m_ssgi[m_ssgiRead]->GetSrvIndex();
        lighting.ssgiViewProj = m_ssgiViewProj;
    } else if (!m_ssgiEnabled) {
        m_ssgiValid = false;
    }
    const bool baking = m_bake.state == BakeState::Pending || m_bake.state == BakeState::Running || m_bake.state == BakeState::Readback;
    // Baked probes: no tracing at all.
    if (!baking && settings.enabled && settings.mode == GIMode::Baked && m_bakedValid) {
        m_active = false;
        lighting.gi = m_bakedConstants;
        lighting.gi.intensity = Max(settings.intensity, 0.0f) * (m_bakedExposure > 0.0f ? exposure / m_bakedExposure : 1.0f);
        m_stats.baked = true;
        return;
    }
    m_active = (settings.enabled || baking) && m_tracePipeline;
    if (!m_active)
        return;
    GISettings s = settings;
    if (m_bake.state == BakeState::Pending) {
        // Fit the bake grid around the static, resident scene.
        AABB bounds;
        for (const RenderInstance& inst : instances) {
            if (!inst.skinMatrices.Empty() || inst.prevWorld)
                continue;
            if (const GpuMesh* mesh = m_assets.GetMesh(inst.mesh))
                bounds.Expand(mesh->bounds.Transformed(inst.world));
        }
        if (bounds.min.x > bounds.max.x) {
            ZX_LOG_WARN(LogGI, "Bake: no static meshes to light");
            m_bake.state = BakeState::Idle;
        } else {
            const Vec3 extent = bounds.max - bounds.min;
            f32 spacing = Max(m_bake.settings.spacing, 0.25f);
            UVec3 counts;
            for (u32 guard = 0; guard < 200; ++guard) {
                counts = UVec3(u32(std::ceil(extent.x / spacing)) + 3, u32(std::ceil(extent.y / spacing)) + 3, u32(std::ceil(extent.z / spacing)) + 3);
                if (counts.x <= 64 && counts.y <= 32 && counts.z <= 64 && u64(counts.x) * counts.y * counts.z <= m_bake.settings.maxProbes)
                    break;
                spacing *= 1.1f;
            }
            counts = UVec3(Clamp(counts.x, 2u, 64u), Clamp(counts.y, 2u, 32u), Clamp(counts.z, 2u, 64u));
            m_bake.spacing = spacing;
            m_bake.counts = counts;
            m_bake.origin = FloorDiv(bounds.min, spacing) - IVec3(1, 1, 1);
            m_bake.center = (bounds.min + bounds.max) * 0.5f;
            m_bake.iteration = 0;
            m_bake.state = BakeState::Running;
            ZX_LOG_INFO(LogGI, "Baking lighting: {} x {} x {} probes, {:.2f} m apart, {} rays x {} iterations", counts.x, counts.y, counts.z, spacing,
                        m_bake.settings.raysPerProbe, m_bake.settings.iterations);
        }
    }
    if (m_bake.state == BakeState::Running || m_bake.state == BakeState::Readback) {
        s.enabled = true;
        s.levels = MaxLevels;
        s.probeCounts = m_bake.counts;
        s.probeSpacing = m_bake.spacing;
        s.raysPerProbe = m_bake.settings.raysPerProbe;
        s.probeStride = 1;
        cameraPosition = m_bake.center;
    }
    m_settings = s;
    EnsureResources(s);
    if (!m_distance || !m_albedo || !m_normal || !m_seeds[0] || !m_seeds[1] || !m_irradiance || !m_probeDistance || !m_rays || !m_cubes[5] || !m_probeData || !m_flags) {
        m_active = false;
        return;
    }
    ++m_frame;
    m_exposure = exposure;
    m_stats.active = true;
    m_stats.finestVoxel = m_voxel;

    // Distance field levels: keep each one until the camera leaves its central region, then re-center it.
    Vec3 wantedMin[MaxLevels];
    bool moved[MaxLevels] = {};
    for (u32 k = 0; k < m_levelCount; ++k) {
        const f32 voxel = m_voxel * f32(1u << k);
        const f32 snap = voxel * f32(SnapVoxels);
        const f32 half = voxel * f32(SdfResolution / 2);
        const Level& l = m_levels[k];
        const Vec3 local = cameraPosition - l.min;
        const bool centered = l.built && local.x > half - snap && local.x < half + snap && local.y > half - snap && local.y < half + snap &&
                              local.z > half - snap && local.z < half + snap;
        if (centered) {
            wantedMin[k] = l.min;
        } else {
            const Vec3 c = cameraPosition - Vec3(half);
            wantedMin[k] = Vec3(std::floor(c.x / snap), std::floor(c.y / snap), std::floor(c.z / snap)) * snap;
            moved[k] = true;
        }
    }
    // What each level holds: static, resident, non-translucent instances overlapping it.
    u64 keys[MaxLevels] = {};
    for (u32 k = 0; k < m_levelCount; ++k)
        keys[k] = HashCombine(m_assets.MeshGeneration(), k);
    struct Candidate
    {
        u32 instance;
        const GpuMesh* mesh;
        AABB bounds;
    };
    Array<Candidate> candidates;
    candidates.Reserve(instances.Size());
    for (u32 i = 0; i < u32(instances.Size()); ++i) {
        const RenderInstance& inst = instances[i];
        if (!inst.skinMatrices.Empty() || inst.prevWorld)
            continue; // moving: left out of the field
        const GpuMesh* mesh = m_assets.GetMesh(inst.mesh);
        if (!mesh || !mesh->vertexBuffer || !mesh->indexBuffer)
            continue;
        const AABB bounds = mesh->bounds.Transformed(inst.world);
        u64 h = HashCombine(Hasher<Guid>::Hash(inst.mesh), u64(uptr(mesh)));
        h = HashFloats(h, &inst.world.columns[0].x, 16);
        for (const AssetId& m : inst.materials)
            h = HashCombine(h, Hasher<Guid>::Hash(m));
        bool any = false;
        for (u32 k = 0; k < m_levelCount; ++k) {
            const f32 extent = m_voxel * f32(1u << k) * f32(SdfResolution);
            const Vec3 lo = wantedMin[k], hi = lo + Vec3(extent);
            if (bounds.max.x < lo.x || bounds.max.y < lo.y || bounds.max.z < lo.z || bounds.min.x > hi.x || bounds.min.y > hi.y || bounds.min.z > hi.z)
                continue;
            keys[k] += HashMix64(h); // order independent
            any = true;
        }
        if (any)
            candidates.PushBack(Candidate{i, mesh, bounds});
    }
    // Rebuild at most one level per frame: a moved one first (finest first), else one whose content changed (not more
    // often than every 20 frames, so a settling physics object doesn't rebuild every frame).
    for (u32 k = 0; k < m_levelCount && m_rebuild == ~0u; ++k)
        if (moved[k])
            m_rebuild = k;
    for (u32 k = 0; k < m_levelCount && m_rebuild == ~0u; ++k)
        if (keys[k] != m_levels[k].contentKey && m_frame >= m_levels[k].builtFrame + 20)
            m_rebuild = k;
    if (m_rebuild != ~0u) {
        const u32 k = m_rebuild;
        m_rebuildMin = wantedMin[k];
        m_rebuildKey = keys[k];
        const f32 voxel = m_voxel * f32(1u << k);
        const Vec3 lo = wantedMin[k], hi = lo + Vec3(voxel * f32(SdfResolution));
        for (const Candidate& c : candidates) {
            if (c.bounds.max.x < lo.x || c.bounds.max.y < lo.y || c.bounds.max.z < lo.z || c.bounds.min.x > hi.x || c.bounds.min.y > hi.y ||
                c.bounds.min.z > hi.z)
                continue;
            const RenderInstance& inst = instances[c.instance];
            const GpuMesh& mesh = *c.mesh;
            const f32 scale = Max(Length(inst.world.columns[0].XYZ()), Max(Length(inst.world.columns[1].XYZ()), Length(inst.world.columns[2].XYZ())));
            const Mat4 rows = Transpose(inst.world);
            for (const asset::CookedMeshSection& sec : mesh.sections) {
                const AABB sb = sec.bounds.Transformed(inst.world);
                if (sb.max.x < lo.x || sb.max.y < lo.y || sb.max.z < lo.z || sb.min.x > hi.x || sb.min.y > hi.y || sb.min.z > hi.z)
                    continue;
                u32 materialIndex = InvalidIndex;
                if (sec.materialSlot < inst.materials.Size()) {
                    materialIndex = m_assets.GetMaterialIndex(inst.materials[sec.materialSlot]);
                    if (m_assets.GetMaterialInfo(materialIndex).blend == MaterialBlend::Translucent)
                        continue;
                }
                // The coarsest LOD whose error stays under half a voxel.
                u32 lod = 0;
                while (lod + 1 < sec.lodCount && mesh.SectionLodAt(sec, lod + 1).error * scale < voxel * 0.5f)
                    ++lod;
                const asset::CookedSectionLod& sl = mesh.SectionLodAt(sec, lod);
                if (sl.indexCount < 3)
                    continue;
                VoxelDraw d;
                for (u32 r = 0; r < 3; ++r)
                    d.rows[r] = rows.columns[r];
                d.vertexBuffer = mesh.vertexBuffer->GetSrvIndex();
                d.indexBuffer = mesh.indexBuffer->GetSrvIndex();
                d.firstIndex = sl.indexOffset;
                d.triangleCount = sl.indexCount / 3;
                d.index16 = mesh.index16 ? 1u : 0u;
                d.material = materialIndex;
                m_voxelDraws.PushBack(d);
                m_stats.voxelizedTriangles += d.triangleCount;
            }
        }
        m_stats.levelRebuilt = k;
        m_stats.voxelizedSections = u32(m_voxelDraws.Size());
    }

    // Probe grid: centered on the camera, in whole probe steps.
    const f32 spacing = Clamp(s.probeSpacing, 0.25f, 16.0f);
    const IVec3 counts(i32(m_probeCounts.x), i32(m_probeCounts.y), i32(m_probeCounts.z));
    const IVec3 cell = FloorDiv(cameraPosition, spacing);
    m_originIndex = IVec3(cell.x - counts.x / 2 + 1, cell.y - counts.y / 2 + 1, cell.z - counts.z / 2 + 1);
    if (m_bake.state == BakeState::Running || m_bake.state == BakeState::Readback)
        m_originIndex = m_bake.origin;
    m_stats.probes = m_probeCounts.x * m_probeCounts.y * m_probeCounts.z;

    // Shading reads the maps as the last update left them.
    if (m_probesValid) {
        GIVolumeConstants& g = lighting.gi;
        g.originIndex = m_updatedOrigin;
        g.irradiance = m_irradiance->GetSrvIndex();
        g.distance = m_probeDistance->GetSrvIndex();
        g.spacing = Vec3(spacing);
        g.counts = m_probeCounts;
        g.intensity = Max(s.intensity, 0.0f);
        g.normalBias = 0.25f * spacing;
        g.viewBias = 0.1f * spacing;
        g.invIrradianceSize = Vec2(1.0f / f32(m_probeCounts.x * m_probeCounts.y * IrradianceTexels), 1.0f / f32(m_probeCounts.z * IrradianceTexels));
        g.invDistanceSize = Vec2(1.0f / f32(m_probeCounts.x * m_probeCounts.y * DistanceTexels), 1.0f / f32(m_probeCounts.z * DistanceTexels));
        g.maxDistance = Max(s.maxRayDistance, spacing);
        g.probeData = m_probeData->GetSrvIndex();
    }
}

void GlobalIllumination::AddLevelBuild(RenderGraph& graph, u32 level)
{
    const RGTexture distance = graph.ImportTexture(m_distance, ResourceState::ShaderResource);
    const RGTexture albedo = graph.ImportTexture(m_albedo, ResourceState::ShaderResource);
    const RGTexture normal = graph.ImportTexture(m_normal, ResourceState::ShaderResource);
    const RGBuffer seedsA = graph.ImportBuffer(m_seeds[0], ResourceState::ShaderResource);
    const RGBuffer seedsB = graph.ImportBuffer(m_seeds[1], ResourceState::ShaderResource);
    SdfBuildParams base = {};
    base.levelMin = Vec4(m_rebuildMin, m_voxel * f32(1u << level));
    base.level = level;
    base.materialBuffer = m_assets.GetMaterialBuffer()->GetSrvIndex();
    base.albedoUav = m_albedo->GetUavIndex();
    base.normalUav = m_normal->GetUavIndex();
    base.distanceUav = m_distance->GetUavIndex();
    base.maxCells = 128 * 128;
    const u32 groups = SdfResolution / 4;
    graph.AddPass(
        "GI.SdfClear", PassType::Compute,
        [=](PassBuilder& b) {
            b.WriteTexture(albedo);
            b.WriteTexture(normal);
            b.WriteBuffer(seedsA);
        },
        [=, this](PassContext& ctx) {
            SdfBuildParams p = base;
            p.seedsUav = m_seeds[0]->GetUavIndex();
            ctx.cmd.SetPipeline(m_clearPipeline);
            ctx.cmd.SetConstants(0, &p, sizeof(p));
            ctx.cmd.Dispatch(groups, groups, groups);
        });
    const Array<VoxelDraw>* draws = &m_voxelDraws;
    graph.AddPass(
        "GI.Voxelize", PassType::Compute,
        [=](PassBuilder& b) {
            b.WriteTexture(albedo);
            b.WriteTexture(normal);
            b.WriteBuffer(seedsA);
        },
        [=, this](PassContext& ctx) {
            ctx.cmd.SetPipeline(m_voxelizePipeline);
            SdfBuildParams p = base;
            p.seedsUav = m_seeds[0]->GetUavIndex();
            for (const VoxelDraw& d : *draws) {
                for (u32 r = 0; r < 3; ++r)
                    p.worldRows[r] = d.rows[r];
                p.vertexBuffer = d.vertexBuffer;
                p.indexBuffer = d.indexBuffer;
                p.firstIndex = d.firstIndex;
                p.triangleCount = d.triangleCount;
                p.index16 = d.index16;
                p.materialIndex = d.material;
                ctx.cmd.SetConstants(0, &p, sizeof(p));
                ctx.cmd.Dispatch((d.triangleCount + 63) / 64);
            }
        });
    // Jump flooding 8, 4, 2, 1: A -> B -> A -> B -> A.
    const u32 steps[] = {8, 4, 2, 1};
    for (u32 i = 0; i < 4; ++i) {
        const RGBuffer in = (i % 2 == 0) ? seedsA : seedsB;
        const RGBuffer out = (i % 2 == 0) ? seedsB : seedsA;
        const u32 inIndex = i % 2, step = steps[i];
        graph.AddPass(
            "GI.SdfJumpFlood", PassType::Compute,
            [=](PassBuilder& b) {
                b.ReadBuffer(in);
                b.WriteBuffer(out);
            },
            [=, this](PassContext& ctx) {
                SdfBuildParams p = base;
                p.seedsInUav = m_seeds[inIndex]->GetSrvIndex();
                p.seedsUav = m_seeds[inIndex ^ 1]->GetUavIndex();
                p.step = step;
                ctx.cmd.SetPipeline(m_jumpFloodPipeline);
                ctx.cmd.SetConstants(0, &p, sizeof(p));
                ctx.cmd.Dispatch(groups, groups, groups);
            });
    }
    graph.AddPass(
        "GI.SdfFinalize", PassType::Compute,
        [=](PassBuilder& b) {
            b.ReadBuffer(seedsA);
            b.WriteTexture(distance);
        },
        [=, this](PassContext& ctx) {
            SdfBuildParams p = base;
            p.seedsInUav = m_seeds[0]->GetSrvIndex();
            ctx.cmd.SetPipeline(m_finalizePipeline);
            ctx.cmd.SetConstants(0, &p, sizeof(p));
            ctx.cmd.Dispatch(groups, groups, groups);
        });
    Level& l = m_levels[level];
    l.min = m_rebuildMin;
    l.built = true;
    l.contentKey = m_rebuildKey;
    l.builtFrame = m_frame;
}

void GlobalIllumination::AddPasses(RenderGraph& graph, const SceneLighting& lighting, const SceneRenderer::LightingInputs& inputs,
                                   u32 lightBuffer, u32 lightCount, const SkyRenderer* sky)
{
    if (m_bakedPending) {
        // Upload the baked probes (used from the next frame on).
        m_bakedPending = false;
        for (Texture*& t : m_bakedTextures) {
            m_device.Destroy(t);
            t = nullptr;
        }
        const BakedGI& b = m_bakedData;
        const u32 w = b.counts.x * b.counts.y, h = b.counts.z;
        const UVec2 sizes[3] = {UVec2(w * IrradianceTexels, h * IrradianceTexels), UVec2(w * DistanceTexels, h * DistanceTexels), UVec2(w, h)};
        const Format formats[3] = {Format::R16G16B16A16_FLOAT, Format::R16G16_FLOAT, Format::R16G16B16A16_FLOAT};
        const u32 bpp[3] = {8, 4, 8};
        const Array<u8>* sources[3] = {&b.irradiance, &b.distance, &b.probeData};
        for (u32 i = 0; i < 3; ++i) {
            TextureDesc d;
            d.width = sizes[i].x;
            d.height = sizes[i].y;
            d.format = formats[i];
            d.usage = TextureUsage::ShaderResource;
            d.debugName = "GI.Baked";
            const SubresourceData init[] = {{sources[i]->Data(), u64(sizes[i].x) * bpp[i], 0}};
            m_bakedTextures[i] = m_device.CreateTexture(d, init);
        }
        if (!m_bakedTextures[0] || !m_bakedTextures[1] || !m_bakedTextures[2]) {
            ZX_LOG_ERROR(LogGI, "Can't create the baked GI textures");
            m_bakedData = BakedGI{};
            return;
        }
        GIVolumeConstants& g = m_bakedConstants;
        g = GIVolumeConstants{};
        g.originIndex = b.originIndex;
        g.irradiance = m_bakedTextures[0]->GetSrvIndex();
        g.distance = m_bakedTextures[1]->GetSrvIndex();
        g.probeData = m_bakedTextures[2]->GetSrvIndex();
        g.spacing = Vec3(b.spacing);
        g.counts = b.counts;
        g.normalBias = 0.25f * b.spacing;
        g.viewBias = 0.1f * b.spacing;
        g.invIrradianceSize = Vec2(1.0f / f32(sizes[0].x), 1.0f / f32(sizes[0].y));
        g.invDistanceSize = Vec2(1.0f / f32(sizes[1].x), 1.0f / f32(sizes[1].y));
        g.maxDistance = b.maxDistance;
        m_bakedExposure = b.exposure;
        m_bakedValid = true;
        m_bakedData = BakedGI{}; // the GPU has its copy
    }
    if (!m_active)
        return;
    if (m_rebuild != ~0u)
        AddLevelBuild(graph, m_rebuild);
    // Rays may only use the built levels from the finest out.
    u32 levels = 0;
    while (levels < m_levelCount && m_levels[levels].built)
        ++levels;
    if (levels == 0)
        return;

    const GISettings& s = m_settings;
    const f32 spacing = Clamp(s.probeSpacing, 0.25f, 16.0f);
    ProbeParams p = {};
    GIVolumeConstants& v = p.volume;
    v.originIndex = m_originIndex;
    v.irradiance = m_probesValid ? m_irradiance->GetSrvIndex() : InvalidDescriptor; // bounces read last update's probes
    v.distance = m_probeDistance->GetSrvIndex();
    v.spacing = Vec3(spacing);
    v.counts = m_probeCounts;
    v.intensity = 1.0f;
    v.normalBias = 0.25f * spacing;
    v.viewBias = 0.0f;
    v.invIrradianceSize = Vec2(1.0f / f32(m_probeCounts.x * m_probeCounts.y * IrradianceTexels), 1.0f / f32(m_probeCounts.z * IrradianceTexels));
    v.invDistanceSize = Vec2(1.0f / f32(m_probeCounts.x * m_probeCounts.y * DistanceTexels), 1.0f / f32(m_probeCounts.z * DistanceTexels));
    v.maxDistance = Max(s.maxRayDistance, spacing);
    v.probeData = m_probesValid ? m_probeData->GetSrvIndex() : InvalidDescriptor;
    for (u32 k = 0; k < MaxLevels; ++k)
        p.sdf.levelMin[k] = Vec4(m_levels[k].min, m_voxel * f32(1u << k));
    p.sdf.distanceTex = m_distance->GetSrvIndex();
    p.sdf.albedoTex = m_albedo->GetSrvIndex();
    p.sdf.normalTex = m_normal->GetSrvIndex();
    p.sdf.levels = levels;
    // A random rotation of the ray pattern per frame.
    Random rng(m_seed + m_frame * 0x9E3779B97F4A7C15ull);
    const Vec3 axis = SafeNormalize(Vec3(rng.Range(-1.0f, 1.0f), rng.Range(-1.0f, 1.0f), rng.Range(-1.0f, 1.0f)), Vec3(0, 1, 0));
    const Mat4 rotation = Transpose(Mat4::FromQuat(Quat::FromAxisAngle(axis, rng.Range(0.0f, 2.0f * Pi))));
    for (u32 r = 0; r < 3; ++r)
        p.rotation[r] = rotation.columns[r];
    p.raysPerProbe = m_rayCount;
    p.sunIrradiance = lighting.sunColor * (lighting.sunIlluminance * m_exposure);
    p.dirToSun = -Normalize(lighting.sunDirection);
    p.rayBuffer = m_rays->GetUavIndex();
    p.flagsBuffer = m_flags->GetUavIndex();
    Buffer* skySH = sky ? sky->IrradianceSH() : nullptr;
    const RGBuffer skySHHandle = skySH ? sky->IrradianceSHHandle() : RGBuffer();
    const bool hasSky = skySH && skySHHandle.IsValid() && lighting.skyIrradianceBuffer;
    p.skySH = hasSky ? 1u : InvalidDescriptor;
    p.skyIrradianceScale = lighting.skyIrradianceScale;
    p.skyColor = lighting.skyColor;
    p.groundColor = lighting.groundColor;
    p.ambientIntensity = lighting.skyLuminance * m_exposure;
    p.ambientFloor = lighting.ambientFloor;
    p.prevOriginIndex = m_probesValid ? m_updatedOrigin : m_originIndex;
    // The hysteresis is per update at stride 2: probes updated less often keep less history, so the light settles in
    // about the same time.
    p.hysteresis = std::pow(Clamp(s.hysteresis, 0.0f, 0.995f), f32(Max(Clamp(s.probeStride, 1u, 16u), 2u)) * 0.5f);
    p.irradianceUav = m_irradiance->GetUavIndex();
    p.distanceUav = m_probeDistance->GetUavIndex();
    p.frame = u32(m_frame);
    p.probeStride = Clamp(s.probeStride, 1u, 16u);
    p.exposureRatio = m_probesValid && m_prevExposure > 0.0f ? m_exposure / m_prevExposure : 1.0f;
    p.lightBuffer = lightBuffer;
    p.lightCount = lightBuffer != InvalidDescriptor ? Min(lightCount, 64u) : 0u;
    p.historyValid = m_probesValid ? 1u : 0u;
    p.sunShadowDistance = Max(s.sunShadowDistance, 1.0f);
    p.energy = Clamp(s.bounce, 0.0f, 1.0f);
    p.cascadeCount = inputs.cascadeCount;
    // Baking: an exact running average over the iterations (no fast-response shortcut, no history before the first).
    const bool baking = m_bake.state == BakeState::Running;
    const bool fieldReady = levels == m_levelCount && m_rebuild == ~0u;
    if (baking) {
        if (!fieldReady)
            m_bake.iteration = 0; // the field changed: start over
        p.baking = 1;
        p.historyValid = m_bake.iteration > 0 ? 1u : 0u;
        p.prevOriginIndex = m_originIndex;
        p.hysteresis = f32(m_bake.iteration) / f32(m_bake.iteration + 1);
        p.compactStride = 0;
    }
    for (u32 c = 0; c < inputs.cascadeCount; ++c) {
        p.shadowMatrices[c] = inputs.shadowMatrices[c];
        p.shadowMaps[c] = inputs.shadowMaps[c];
    }
    const SceneRenderer::LightingInputs li = inputs;
    for (u32 i = 0; i < 6; ++i) {
        p.cubeSrv[i] = m_cubes[i]->GetSrvIndex();
        p.cubeUav[i] = m_cubes[i]->GetUavIndex();
    }
    p.cubeValid = m_probesValid ? 1u : 0u;
    p.probeDataUav = m_probeData->GetUavIndex();
    const RGTexture probeData = graph.ImportTexture(m_probeData, ResourceState::ShaderResource);
    RGTexture cubes[6];
    for (u32 i = 0; i < 6; ++i)
        cubes[i] = graph.ImportTexture(m_cubes[i], ResourceState::ShaderResource);

    const RGTexture distance = graph.ImportTexture(m_distance, ResourceState::ShaderResource);
    const RGTexture albedo = graph.ImportTexture(m_albedo, ResourceState::ShaderResource);
    const RGTexture normal = graph.ImportTexture(m_normal, ResourceState::ShaderResource);
    const RGTexture irradiance = graph.ImportTexture(m_irradiance, ResourceState::ShaderResource);
    const RGTexture probeDistance = graph.ImportTexture(m_probeDistance, ResourceState::ShaderResource);
    const RGBuffer rays = graph.ImportBuffer(m_rays, ResourceState::ShaderResource);
    const RGBuffer flags = graph.ImportBuffer(m_flags, ResourceState::ShaderResource);
    const u32 probes = m_probeCounts.x * m_probeCounts.y * m_probeCounts.z;
    const u32 rayGroups = (m_rayCount + 63) / 64;
    // Without newly scrolled-in probes only this frame's turn needs thread groups.
    const bool scrolled = !m_probesValid || m_originIndex != m_updatedOrigin;
    if (!scrolled && p.probeStride > 1) {
        p.compactStride = p.probeStride;
        p.phase = (p.probeStride - u32(m_frame % p.probeStride)) % p.probeStride;
    }
    const u32 groups = p.compactStride > 0 ? (probes + p.compactStride - 1) / p.compactStride : probes;
    graph.AddPass(
        "GI.ProbeTrace", PassType::Compute,
        [=](PassBuilder& b) {
            b.ReadTexture(distance);
            b.ReadTexture(albedo);
            b.ReadTexture(normal);
            b.ReadTexture(irradiance);
            b.ReadTexture(probeDistance);
            if (hasSky)
                b.ReadBuffer(skySHHandle, ResourceState::ConstantBuffer);
            for (u32 c = 0; c < li.cascadeCount; ++c)
                if (li.shadowTextures[c].IsValid())
                    b.ReadTexture(li.shadowTextures[c]);
            for (const RGTexture& t : cubes)
                b.ReadTexture(t);
            b.ReadTexture(probeData);
            b.WriteBuffer(rays);
            b.WriteBuffer(flags);
        },
        [=, this](PassContext& ctx) {
            ctx.cmd.SetPipeline(m_tracePipeline);
            ctx.cmd.SetConstants(0, &p, sizeof(p));
            if (hasSky)
                ctx.cmd.SetConstantBuffer(1, skySH->GetGpuAddress());
            ctx.cmd.Dispatch(groups, rayGroups);
        });
    ProbeParams up = p;
    up.rayBuffer = m_rays->GetSrvIndex();
    up.flagsBuffer = m_flags->GetSrvIndex();
    graph.AddPass(
        "GI.ProbeIrradiance", PassType::Compute,
        [=](PassBuilder& b) {
            b.ReadBuffer(rays);
            b.ReadBuffer(flags);
            b.WriteTexture(irradiance);
            b.WriteTexture(probeData);
        },
        [=, this](PassContext& ctx) {
            ctx.cmd.SetPipeline(m_irradiancePipeline);
            ctx.cmd.SetConstants(0, &up, sizeof(up));
            ctx.cmd.Dispatch(groups);
        });
    graph.AddPass(
        "GI.ProbeDistance", PassType::Compute,
        [=](PassBuilder& b) {
            b.ReadBuffer(rays);
            b.ReadBuffer(flags);
            b.WriteTexture(probeDistance);
        },
        [=, this](PassContext& ctx) {
            ctx.cmd.SetPipeline(m_distancePipeline);
            ctx.cmd.SetConstants(0, &up, sizeof(up));
            ctx.cmd.Dispatch(groups);
        });
    // Ambient cubes of the updated probes for next frame's rays.
    ProbeParams cp = up;
    cp.volume.irradiance = m_irradiance->GetSrvIndex();
    graph.AddPass(
        "GI.AmbientCubes", PassType::Compute,
        [=](PassBuilder& b) {
            b.ReadTexture(irradiance);
            for (const RGTexture& t : cubes)
                b.WriteTexture(t);
        },
        [=, this](PassContext& ctx) {
            ctx.cmd.SetPipeline(m_cubePipeline);
            ctx.cmd.SetConstants(0, &cp, sizeof(cp));
            ctx.cmd.Dispatch((probes * 6 + 63) / 64);
        });
    m_stats.probesUpdated = (probes + p.probeStride - 1) / p.probeStride;
    m_stats.rays = m_stats.probesUpdated * m_rayCount;
    m_probesValid = true;
    m_updatedOrigin = m_originIndex;
    m_prevExposure = m_exposure;
    if (baking && fieldReady && ++m_bake.iteration >= Max(m_bake.settings.iterations, 1u))
        AddBakeReadback(graph);
}

// --- Screen-space GI ------------------------------------------------------------------------------------------------

namespace {
// Keep in sync with SsgiParams (Shaders/GI/SSGI.hlsl).
struct SsgiParams
{
    Mat4 invViewProj, viewProj, prevViewProj;
    Vec3 cameraPosition;
    f32 nearZ;
    Vec3 cameraForward;
    f32 maxDistance;
    Vec2 targetSize, sceneSize;
    u32 color, depth, normals, traceUav, traceSrv, history, output, frame, rays, steps;
    f32 thickness, historyWeight;
};
static_assert(sizeof(SsgiParams) == 288);
} // namespace

void GlobalIllumination::AddScreenPass(RenderGraph& graph, RGTexture color, RGTexture depth, RGTexture normals, const RenderView& view)
{
    if (!m_ssgiEnabled || !normals.IsValid()) {
        m_ssgiValid = false;
        return;
    }
    const UVec2 size(Max(1u, (view.width + 1) / 2), Max(1u, (view.height + 1) / 2));
    if (size != m_ssgiSize || !m_ssgiTrace) {
        for (Texture** t : {&m_ssgiTrace, &m_ssgi[0], &m_ssgi[1]}) {
            m_device.Destroy(*t);
            TextureDesc d;
            d.width = size.x;
            d.height = size.y;
            d.format = Format::R16G16B16A16_FLOAT;
            d.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess;
            d.debugName = "GI.Screen";
            *t = m_device.CreateTexture(d);
        }
        m_ssgiSize = size;
        m_ssgiValid = false;
    }
    if (!m_ssgiTrace || !m_ssgi[0] || !m_ssgi[1])
        return;
    const u32 write = m_ssgiRead ^ 1;
    SsgiParams p = {};
    p.invViewProj = Inverse(view.viewProj);
    p.viewProj = view.viewProj;
    p.prevViewProj = m_ssgiViewProj;
    p.cameraPosition = view.position;
    p.cameraForward = Normalize(Vec3(view.view.columns[0].z, view.view.columns[1].z, view.view.columns[2].z));
    p.nearZ = view.nearPlane;
    p.maxDistance = Max(m_settings.screenDistance, 0.1f);
    p.targetSize = Vec2(f32(size.x), f32(size.y));
    p.sceneSize = Vec2(f32(view.width), f32(view.height));
    p.traceUav = m_ssgiTrace->GetUavIndex();
    p.traceSrv = m_ssgiTrace->GetSrvIndex();
    p.history = m_ssgiValid ? m_ssgi[m_ssgiRead]->GetSrvIndex() : InvalidDescriptor;
    p.output = m_ssgi[write]->GetUavIndex();
    p.frame = u32(++m_ssgiFrame);
    p.rays = Clamp(m_settings.screenRays, 1u, 16u);
    p.steps = 12;
    p.thickness = 0.3f;
    p.historyWeight = Clamp(m_settings.screenHistory, 0.0f, 0.98f);
    const RGTexture trace = graph.ImportTexture(m_ssgiTrace, ResourceState::ShaderResource);
    const RGTexture history = graph.ImportTexture(m_ssgi[m_ssgiRead], ResourceState::ShaderResource);
    const RGTexture output = graph.ImportTexture(m_ssgi[write], ResourceState::ShaderResource);
    graph.AddPass(
        "GI.ScreenTrace", PassType::Compute,
        [=](PassBuilder& b) {
            b.ReadTexture(color);
            b.ReadTexture(depth);
            b.ReadTexture(normals);
            b.WriteTexture(trace);
        },
        [this, p, color, depth, normals, size](PassContext& ctx) {
            SsgiParams q = p;
            q.color = ctx.Texture(color)->GetSrvIndex();
            q.depth = ctx.Texture(depth)->GetSrvIndex();
            q.normals = ctx.Texture(normals)->GetSrvIndex();
            ctx.cmd.SetPipeline(m_ssgiTracePipeline);
            ctx.cmd.SetConstants(0, &q, sizeof(q));
            ctx.cmd.Dispatch((size.x + 7) / 8, (size.y + 7) / 8);
        });
    graph.AddPass(
        "GI.ScreenTemporal", PassType::Compute,
        [=](PassBuilder& b) {
            b.ReadTexture(trace);
            b.ReadTexture(history);
            b.ReadTexture(depth);
            b.WriteTexture(output);
            b.SideEffects(); // read by the next frame
        },
        [this, p, depth, size](PassContext& ctx) {
            SsgiParams q = p;
            q.depth = ctx.Texture(depth)->GetSrvIndex();
            ctx.cmd.SetPipeline(m_ssgiTemporalPipeline);
            ctx.cmd.SetConstants(0, &q, sizeof(q));
            ctx.cmd.Dispatch((size.x + 7) / 8, (size.y + 7) / 8);
        });
    m_ssgiRead = write;
    m_ssgiViewProj = view.viewProj;
    m_ssgiValid = true;
}

// --- Baking ---------------------------------------------------------------------------------------------------------

void GlobalIllumination::StartBake(const GIBakeSettings& settings)
{
    CancelBake();
    m_bake.settings = settings;
    m_bake.state = BakeState::Pending;
}

void GlobalIllumination::CancelBake()
{
    for (Buffer*& b : m_bake.readback) {
        m_device.Destroy(b);
        b = nullptr;
    }
    m_bake.state = BakeState::Idle;
    m_bake.result = BakedGI{};
    m_probesValid = false;
}

f32 GlobalIllumination::BakeProgress() const
{
    switch (m_bake.state) {
    case BakeState::Pending: return 0.0f;
    case BakeState::Running: return 0.95f * f32(m_bake.iteration) / f32(Max(m_bake.settings.iterations, 1u));
    case BakeState::Readback: return 0.97f;
    case BakeState::Done: return 1.0f;
    default: return 0.0f;
    }
}

void GlobalIllumination::AddBakeReadback(RenderGraph& graph)
{
    Texture* sources[3] = {m_irradiance, m_probeDistance, m_probeData};
    const u32 bpp[3] = {8, 4, 8};
    RGTexture handles[3];
    for (u32 i = 0; i < 3; ++i) {
        const TextureDesc& d = sources[i]->GetDesc();
        const u64 pitch = (u64(d.width) * bpp[i] + 255) & ~u64(255);
        m_device.Destroy(m_bake.readback[i]);
        m_bake.readback[i] = m_device.CreateBuffer(BufferDesc{pitch * d.height, BufferUsage::CopyDest, MemoryType::Readback, 0, "GI.BakeReadback"});
        handles[i] = graph.ImportTexture(sources[i], ResourceState::ShaderResource);
    }
    graph.AddPass(
        "GI.BakeReadback", PassType::Copy,
        [=](PassBuilder& b) {
            for (const RGTexture& t : handles)
                b.ReadTexture(t, ResourceState::CopySource);
            b.SideEffects();
        },
        [this, sources](PassContext& ctx) {
            for (u32 i = 0; i < 3; ++i)
                m_bake.pitches[i] = ctx.cmd.CopyTextureToBuffer(m_bake.readback[i], 0, sources[i]);
        });
    m_bake.copyFrame = m_frame;
    m_bake.state = BakeState::Readback;
    BakedGI& r = m_bake.result;
    r.originIndex = m_bake.origin;
    r.counts = m_bake.counts;
    r.spacing = m_bake.spacing;
    r.maxDistance = Max(m_settings.maxRayDistance, m_bake.spacing);
    r.exposure = m_exposure;
}

void GlobalIllumination::FinishBakeReadback()
{
    if (m_frame < m_bake.copyFrame + FramesInFlight + 1)
        return; // the copies may not have run yet
    Texture* sources[3] = {m_irradiance, m_probeDistance, m_probeData};
    const u32 bpp[3] = {8, 4, 8};
    Array<u8>* outs[3] = {&m_bake.result.irradiance, &m_bake.result.distance, &m_bake.result.probeData};
    for (u32 i = 0; i < 3; ++i) {
        const TextureDesc& d = sources[i]->GetDesc();
        const u64 row = u64(d.width) * bpp[i];
        const u8* data = static_cast<const u8*>(m_bake.readback[i]->GetMappedData());
        outs[i]->ResizeUninitialized(row * d.height);
        for (u32 y = 0; y < d.height; ++y)
            MemCopy(outs[i]->Data() + row * y, data + m_bake.pitches[i] * y, row);
        m_device.Destroy(m_bake.readback[i]);
        m_bake.readback[i] = nullptr;
    }
    m_bake.state = BakeState::Done;
    m_probesValid = false; // the dynamic probes go back to the camera grid
    ZX_LOG_INFO(LogGI, "Lighting baked: {} probes", m_bake.counts.x * m_bake.counts.y * m_bake.counts.z);
}

bool GlobalIllumination::TakeBake(BakedGI& out)
{
    if (m_bake.state != BakeState::Done)
        return false;
    out = move(m_bake.result);
    m_bake.result = BakedGI{};
    m_bake.state = BakeState::Idle;
    return true;
}

void GlobalIllumination::SetBaked(const BakedGI* data)
{
    m_bakedValid = false;
    m_bakedPending = data && data->IsValid();
    if (m_bakedPending)
        m_bakedData = *data;
    else
        m_bakedData = BakedGI{};
}

bool BakedGI::IsValid() const
{
    const u64 probes = u64(counts.x) * counts.y * counts.z;
    return probes > 0 && counts.x <= 64 && counts.y <= 32 && counts.z <= 64 && spacing > 0.0f &&
           irradiance.Size() == probes * IrradianceTexels * IrradianceTexels * 8 && distance.Size() == probes * DistanceTexels * DistanceTexels * 4 &&
           probeData.Size() == probes * 8;
}

namespace {
constexpr u32 BakedMagic = 0x3149475A; // "ZGI1"
constexpr u32 BakedVersion = 1;
} // namespace

bool SaveBakedGI(StringView path, const BakedGI& d)
{
    if (!d.IsValid())
        return false;
    Array<u8> bytes;
    BinaryWriter w(bytes);
    w.Write<u32>(BakedMagic);
    w.Write<u32>(BakedVersion);
    for (i32 v : {d.originIndex.x, d.originIndex.y, d.originIndex.z})
        w.Write<i32>(v);
    for (u32 v : {d.counts.x, d.counts.y, d.counts.z})
        w.Write<u32>(v);
    w.Write<f32>(d.spacing);
    w.Write<f32>(d.maxDistance);
    w.Write<f32>(d.exposure);
    w.WriteArray(d.irradiance.View());
    w.WriteArray(d.distance.View());
    w.WriteArray(d.probeData.View());
    return fs::WriteFile(path, bytes.View());
}

bool LoadBakedGI(StringView path, BakedGI& d)
{
    Array<u8> bytes;
    if (!fs::ReadFile(path, bytes))
        return false;
    BinaryReader r(bytes.View());
    if (r.Read<u32>() != BakedMagic || r.Read<u32>() != BakedVersion)
        return false;
    d.originIndex.x = r.Read<i32>();
    d.originIndex.y = r.Read<i32>();
    d.originIndex.z = r.Read<i32>();
    d.counts.x = r.Read<u32>();
    d.counts.y = r.Read<u32>();
    d.counts.z = r.Read<u32>();
    d.spacing = r.Read<f32>();
    d.maxDistance = r.Read<f32>();
    d.exposure = r.Read<f32>();
    r.ReadArray(d.irradiance);
    r.ReadArray(d.distance);
    r.ReadArray(d.probeData);
    return !r.HasError() && d.IsValid();
}

} // namespace zx::render
