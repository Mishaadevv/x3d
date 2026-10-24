#include "Render/GlobalIllumination.h"
#include "Core/Log/Log.h"
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
    f32 pad;
    Mat4 shadowMatrices[SceneRenderer::MaxCascades];
    u32 shadowMaps[SceneRenderer::MaxCascades];
    u32 cubeSrv[8];
    u32 cubeUav[8];
    u32 cubeValid;
    u32 compactStride, phase;
    u32 probeDataUav;
};
static_assert(sizeof(ProbeParams) == 704);

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
    for (Pipeline* p : {m_clearPipeline, m_voxelizePipeline, m_jumpFloodPipeline, m_finalizePipeline, m_tracePipeline, m_irradiancePipeline,
                        m_distancePipeline, m_cubePipeline})
        m_device.Destroy(p);
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
                {&m_cubeCS, &m_cubePipeline, "/Shaders/GI/DDGI.hlsl", "AmbientCubeCS"}};
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
    for (Buffer** b : {&m_seeds[0], &m_seeds[1], &m_rays}) {
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
    m_active = settings.enabled && m_tracePipeline;
    if (!m_active)
        return;
    m_settings = settings;
    EnsureResources(settings);
    if (!m_distance || !m_albedo || !m_normal || !m_seeds[0] || !m_seeds[1] || !m_irradiance || !m_probeDistance || !m_rays || !m_cubes[5] || !m_probeData) {
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
            for (const asset::CookedMeshSection& s : mesh.sections) {
                const AABB sb = s.bounds.Transformed(inst.world);
                if (sb.max.x < lo.x || sb.max.y < lo.y || sb.max.z < lo.z || sb.min.x > hi.x || sb.min.y > hi.y || sb.min.z > hi.z)
                    continue;
                u32 materialIndex = InvalidIndex;
                if (s.materialSlot < inst.materials.Size()) {
                    materialIndex = m_assets.GetMaterialIndex(inst.materials[s.materialSlot]);
                    if (m_assets.GetMaterialInfo(materialIndex).blend == MaterialBlend::Translucent)
                        continue;
                }
                // The coarsest LOD whose error stays under half a voxel.
                u32 lod = 0;
                while (lod + 1 < s.lodCount && mesh.SectionLodAt(s, lod + 1).error * scale < voxel * 0.5f)
                    ++lod;
                const asset::CookedSectionLod& sl = mesh.SectionLodAt(s, lod);
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
    const f32 spacing = Clamp(settings.probeSpacing, 0.25f, 16.0f);
    const IVec3 counts(i32(m_probeCounts.x), i32(m_probeCounts.y), i32(m_probeCounts.z));
    const IVec3 cell = FloorDiv(cameraPosition, spacing);
    m_originIndex = IVec3(cell.x - counts.x / 2 + 1, cell.y - counts.y / 2 + 1, cell.z - counts.z / 2 + 1);
    m_stats.probes = m_probeCounts.x * m_probeCounts.y * m_probeCounts.z;

    // Shading reads the maps as the last update left them.
    if (m_probesValid) {
        GIVolumeConstants& g = lighting.gi;
        g.originIndex = m_updatedOrigin;
        g.irradiance = m_irradiance->GetSrvIndex();
        g.distance = m_probeDistance->GetSrvIndex();
        g.spacing = Vec3(spacing);
        g.counts = m_probeCounts;
        g.intensity = Max(settings.intensity, 0.0f);
        g.normalBias = 0.25f * spacing;
        g.viewBias = 0.1f * spacing;
        g.invIrradianceSize = Vec2(1.0f / f32(m_probeCounts.x * m_probeCounts.y * IrradianceTexels), 1.0f / f32(m_probeCounts.z * IrradianceTexels));
        g.invDistanceSize = Vec2(1.0f / f32(m_probeCounts.x * m_probeCounts.y * DistanceTexels), 1.0f / f32(m_probeCounts.z * DistanceTexels));
        g.maxDistance = Max(settings.maxRayDistance, spacing);
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
    graph.AddPass(
        "GI.ProbeIrradiance", PassType::Compute,
        [=](PassBuilder& b) {
            b.ReadBuffer(rays);
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
}

} // namespace zx::render
