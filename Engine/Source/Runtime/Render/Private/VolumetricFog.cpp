#include "Render/VolumetricFog.h"
#include "Core/Log/Log.h"
#include "RHI/ShaderCompiler.h"

namespace zx::render {

using namespace rhi;

ZX_DEFINE_LOG_CATEGORY_STATIC(LogVolumetricFog, Info);

namespace {

// Keep in sync with VolumeParams (Shaders/Volumetric/VolumetricFog.hlsl).
struct VolumeParams
{
    Mat4 invViewProj;
    Mat4 prevViewProj;
    Mat4 shadowMatrices[SceneRenderer::MaxCascades];
    Vec4 cascadeSplits;
    u32 shadowMaps[SceneRenderer::MaxCascades];
    Vec3 cameraPosition;
    f32 distance;
    Vec3 cameraForward;
    u32 cascadeCount;
    u32 grid[3];
    u32 volumeCount;
    Vec3 sunColor;
    f32 anisotropy;
    Vec3 dirToSun;
    f32 jitter;
    Vec3 ambient;
    f32 time;
    Vec3 albedo;
    f32 fogDensity;
    f32 fogFalloff;
    f32 fogBase;
    f32 localLights;
    f32 history;
    u32 lightBuffer;
    u32 clusterBuffer;
    u32 lightCount;
    f32 clusterDepthScale;
    f32 clusterDepthBias;
    Vec2 clusterTileScale;
    u32 volumes;
    Vec2 renderSize;
    u32 scatterOut;
    u32 lightingOut;
    u32 historyIn;
    u32 scatterIn;
    u32 integratedOut;
    u32 integratedIn;
    Vec3 prevCameraPosition;
    u32 depthTexture;
    Vec3 prevCameraForward;
    f32 nearPlane;
    f32 skyScale; // < 0: no sky SH, use ambient
    f32 pad[3];
};

// Keep in sync with AddVolumes (112 bytes).
struct GpuFogVolume
{
    Vec4 rows[3];
    Vec3 albedo;
    f32 density;
    Vec3 emissive;
    u32 shape;
    f32 noiseScale, noiseAmount, edgeFade, pad0;
    Vec3 wind;
    f32 pad1;
};
static_assert(sizeof(GpuFogVolume) == 112);

/// Halton(2) sequence in -0.5 .. 0.5: the slice jitter of a frame.
f32 Jitter(u64 frame)
{
    f32 f = 1.0f, r = 0.0f;
    for (u64 i = (frame % 16) + 1; i > 0; i /= 2) {
        f *= 0.5f;
        r += f * f32(i % 2);
    }
    return r - 0.5f;
}

} // namespace

VolumetricFogRenderer::VolumetricFogRenderer(Device& device) : m_device(device) {}

VolumetricFogRenderer::~VolumetricFogRenderer()
{
    m_device.Destroy(m_injectPipeline);
    m_device.Destroy(m_integratePipeline);
    m_device.Destroy(m_applyPipeline);
    for (Texture* t : m_scatter)
        m_device.Destroy(t);
    m_device.Destroy(m_lighting);
    m_device.Destroy(m_integrated);
    for (Buffer* b : m_volumeBuffers)
        m_device.Destroy(b);
}

bool VolumetricFogRenderer::Init(Format colorFormat)
{
    struct Job
    {
        ShaderBytecode* out;
        StringView entry;
        ShaderStage stage;
    } jobs[] = {{&m_injectCS, "InjectCS", ShaderStage::Compute},
                {&m_integrateCS, "IntegrateCS", ShaderStage::Compute},
                {&m_applyVS, "ApplyVS", ShaderStage::Vertex},
                {&m_applyPS, "ApplyPS", ShaderStage::Pixel}};
    for (const Job& j : jobs) {
        ShaderCompileDesc d;
        d.path = "/Shaders/Volumetric/VolumetricFog.hlsl";
        d.entryPoint = j.entry;
        d.stage = j.stage;
        Result<ShaderBytecode> r = ShaderCompiler::Compile(d);
        if (!r) {
            ZX_LOG_ERROR(LogVolumetricFog, "{}", r.GetError().message);
            return false;
        }
        *j.out = move(*r);
    }
    ComputePipelineDesc cd;
    cd.computeShader = &m_injectCS;
    cd.debugName = "FogInject";
    m_injectPipeline = m_device.CreateComputePipeline(cd);
    cd.computeShader = &m_integrateCS;
    cd.debugName = "FogIntegrate";
    m_integratePipeline = m_device.CreateComputePipeline(cd);
    GraphicsPipelineDesc pd;
    pd.vertexShader = &m_applyVS;
    pd.pixelShader = &m_applyPS;
    pd.rasterizer.cullMode = CullMode::None;
    pd.depthStencil.depthTest = false;
    pd.depthStencil.depthWrite = false;
    pd.colorFormats[0] = colorFormat;
    pd.colorCount = 1;
    pd.blend[0].enable = true;
    pd.blend[0].srcColor = BlendFactor::One;
    pd.blend[0].dstColor = BlendFactor::SrcAlpha;
    pd.blend[0].srcAlpha = BlendFactor::Zero;
    pd.blend[0].dstAlpha = BlendFactor::One;
    pd.debugName = "VolumetricFog";
    m_applyPipeline = m_device.CreateGraphicsPipeline(pd);
    for (u32 i = 0; i < FramesInFlight; ++i)
        m_volumeBuffers[i] = m_device.CreateBuffer(
            BufferDesc{u64(MaxVolumes) * sizeof(GpuFogVolume), BufferUsage::ShaderResource, MemoryType::Upload, 0, "FogVolumes"});
    return m_injectPipeline && m_integratePipeline && m_applyPipeline;
}

void VolumetricFogRenderer::EnsureTextures(UVec3 grid)
{
    if (grid == m_grid && m_integrated)
        return;
    for (Texture*& t : m_scatter)
        m_device.Destroy(t), t = nullptr;
    m_device.Destroy(m_lighting);
    m_device.Destroy(m_integrated);
    TextureDesc d;
    d.dimension = TextureDimension::Texture3D;
    d.width = grid.x;
    d.height = grid.y;
    d.depthOrArraySize = grid.z;
    d.format = Format::R16G16B16A16_FLOAT;
    d.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess;
    d.debugName = "FogScatter";
    m_scatter[0] = m_device.CreateTexture(d);
    m_scatter[1] = m_device.CreateTexture(d);
    d.debugName = "FogLighting";
    m_lighting = m_device.CreateTexture(d);
    d.debugName = "FogIntegrated";
    m_integrated = m_device.CreateTexture(d);
    m_grid = grid;
    m_historyValid = false;
}

void VolumetricFogRenderer::AddPasses(RenderGraph& graph, RGTexture color, RGTexture depth, const RenderView& view,
                                      const Mat4& viewProjNoJitter, const SceneLighting& lighting,
                                      const SceneRenderer::LightingInputs& lights, const FogSettings& fog,
                                      const VolumetricFogSettings& settings, Span<const FogVolumeDesc> volumes, f32 exposure, f32 time,
                                      const SkyRenderer* sky)
{
    m_active = false;
    if (!settings.enabled || !m_applyPipeline)
        return;
    const u32 tile = Clamp(settings.tileSize, 4u, 64u);
    const UVec3 grid((view.width + tile - 1) / tile, (view.height + tile - 1) / tile, Clamp(settings.slices, 16u, 128u));
    EnsureTextures(grid);
    if (!m_integrated || !m_scatter[0] || !m_scatter[1] || !m_lighting)
        return;
    m_active = true;
    ++m_frame;
    m_distance = Max(settings.distance, 1.0f);

    // Local volumes.
    Buffer* volumeBuffer = m_volumeBuffers[m_device.GetFrameIndex() % FramesInFlight];
    const u32 volumeCount = Min(u32(volumes.Size()), MaxVolumes);
    auto* gpu = static_cast<GpuFogVolume*>(volumeBuffer->GetMappedData());
    for (u32 i = 0; i < volumeCount; ++i) {
        const FogVolumeDesc& v = volumes[i];
        GpuFogVolume& g = gpu[i];
        const Mat4 rows = Transpose(v.worldToVolume);
        for (u32 r = 0; r < 3; ++r)
            g.rows[r] = rows.columns[r];
        g.albedo = v.albedo;
        g.density = Max(v.density, 0.0f);
        g.emissive = v.emissive * exposure;
        g.shape = u32(v.shape);
        g.noiseScale = v.noiseScale;
        g.noiseAmount = Clamp(v.noiseAmount, 0.0f, 1.0f);
        g.edgeFade = Clamp(v.edgeFade, 0.01f, 1.0f);
        g.wind = v.wind;
        g.pad0 = g.pad1 = 0.0f;
    }

    VolumeParams p = {};
    p.invViewProj = Inverse(viewProjNoJitter);
    p.prevViewProj = m_prevViewProj;
    for (u32 c = 0; c < lights.cascadeCount; ++c) {
        p.shadowMatrices[c] = lights.shadowMatrices[c];
        p.shadowMaps[c] = lights.shadowMaps[c];
    }
    p.cascadeSplits = lights.cascadeSplits;
    p.cascadeCount = lights.cascadeCount;
    const Mat4 invView = Inverse(view.view);
    p.cameraPosition = view.position;
    p.cameraForward = SafeNormalize(invView.TransformVector(Vec3(0, 0, 1)), Vec3(0, 0, 1));
    p.distance = m_distance;
    p.grid[0] = grid.x;
    p.grid[1] = grid.y;
    p.grid[2] = grid.z;
    p.volumeCount = volumeCount;
    p.sunColor = lighting.sunColor * (lighting.sunIlluminance * exposure);
    p.anisotropy = Clamp(fog.sunAnisotropy, -0.95f, 0.95f);
    p.dirToSun = -lighting.sunDirection;
    p.jitter = Jitter(m_frame);
    p.ambient = lighting.skyColor * (lighting.skyLuminance * exposure);
    // Sky light: the sky's SH (same as the far height fog, so the two meet without a seam), else the flat ambient.
    Buffer* skySH = sky ? sky->IrradianceSH() : nullptr;
    const RGBuffer skySHHandle = skySH ? sky->IrradianceSHHandle() : RGBuffer();
    p.skyScale = skySH && skySHHandle.IsValid() ? lighting.skyIrradianceScale.x : -1.0f;
    p.time = time;
    p.albedo = fog.albedo;
    p.fogDensity = fog.enabled ? fog.density * Max(settings.scattering, 0.0f) : 0.0f;
    p.fogFalloff = Max(fog.falloff, 1e-5f);
    p.fogBase = fog.baseHeight;
    p.localLights = Max(settings.localLights, 0.0f);
    p.history = m_historyValid ? Clamp(settings.history, 0.0f, 0.98f) : 0.0f;
    p.lightBuffer = lights.lightBuffer;
    p.clusterBuffer = lights.clusterBuffer;
    p.lightCount = lights.lightCount;
    p.clusterDepthScale = lights.clusterDepthScale;
    p.clusterDepthBias = lights.clusterDepthBias;
    p.clusterTileScale = lights.clusterTileScale;
    p.volumes = volumeBuffer->GetSrvIndex();
    p.renderSize = Vec2(f32(view.width), f32(view.height));
    p.prevCameraPosition = m_prevPosition;
    p.prevCameraForward = m_prevForward;
    p.nearPlane = view.nearPlane;
    m_prevViewProj = viewProjNoJitter;
    m_prevPosition = view.position;
    m_prevForward = p.cameraForward;

    Texture* current = m_scatter[m_current];
    Texture* history = m_scatter[m_current ^ 1];
    m_current ^= 1;
    m_historyValid = true;
    const RGTexture scatterRG = graph.ImportTexture(current, ResourceState::ShaderResource);
    const RGTexture historyRG = graph.ImportTexture(history, ResourceState::ShaderResource);
    const RGTexture lightingRG = graph.ImportTexture(m_lighting, ResourceState::ShaderResource);
    const RGTexture integratedRG = graph.ImportTexture(m_integrated, ResourceState::ShaderResource);
    m_rgLighting = lightingRG;
    m_rgIntegrated = integratedRG;
    p.scatterOut = current->GetUavIndex();
    p.lightingOut = m_lighting->GetUavIndex();
    p.historyIn = history->GetSrvIndex();
    p.scatterIn = current->GetSrvIndex();
    p.integratedOut = m_integrated->GetUavIndex();
    p.integratedIn = m_integrated->GetSrvIndex();

    const SceneRenderer::LightingInputs li = lights;
    graph.AddPass(
        "FogInject", PassType::Compute,
        [=](PassBuilder& b) {
            b.ReadTexture(historyRG);
            for (u32 c = 0; c < li.cascadeCount; ++c)
                if (li.shadowTextures[c].IsValid())
                    b.ReadTexture(li.shadowTextures[c]);
            if (li.lightCount && li.clusters.IsValid())
                b.ReadBuffer(li.clusters);
            if (skySHHandle.IsValid())
                b.ReadBuffer(skySHHandle, ResourceState::ConstantBuffer);
            b.WriteTexture(scatterRG);
            b.WriteTexture(lightingRG);
        },
        [=, this](PassContext& ctx) {
            ctx.cmd.SetPipeline(m_injectPipeline);
            ctx.cmd.SetConstants(0, &p, sizeof(p));
            if (skySH && skySHHandle.IsValid())
                ctx.cmd.SetConstantBuffer(1, skySH->GetGpuAddress());
            ctx.cmd.Dispatch((grid.x + 3) / 4, (grid.y + 3) / 4, (grid.z + 3) / 4);
        });
    graph.AddPass(
        "FogIntegrate", PassType::Compute,
        [=](PassBuilder& b) {
            b.ReadTexture(scatterRG);
            b.WriteTexture(integratedRG);
        },
        [=, this](PassContext& ctx) {
            ctx.cmd.SetPipeline(m_integratePipeline);
            ctx.cmd.SetConstants(0, &p, sizeof(p));
            ctx.cmd.Dispatch((grid.x + 7) / 8, (grid.y + 7) / 8, 1);
        });
    const UVec2 size(view.width, view.height);
    graph.AddPass(
        "FogApply", PassType::Graphics,
        [=](PassBuilder& b) {
            b.ReadTexture(integratedRG);
            b.ReadTexture(depth);
            b.RenderTarget(color, LoadOp::Load);
        },
        [=, this](PassContext& ctx) {
            VolumeParams params = p;
            params.depthTexture = ctx.Texture(depth)->GetSrvIndex();
            ctx.cmd.SetViewport(Viewport{0, 0, f32(size.x), f32(size.y), 0, 1});
            ctx.cmd.SetScissor(0, 0, size.x, size.y);
            ctx.cmd.SetPipeline(m_applyPipeline);
            ctx.cmd.SetConstants(0, &params, sizeof(params));
            ctx.cmd.Draw(3);
        });
}

} // namespace zx::render
