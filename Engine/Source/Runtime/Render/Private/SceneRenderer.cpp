#include "Render/SceneRenderer.h"
#include "Core/Containers/Algorithm.h"
#include "Core/Log/Log.h"
#include "Core/Profiler/Profiler.h"
#include "Core/String/Name.h"
#include "RHI/ShaderCompiler.h"

#include <cmath>

ZX_DEFINE_LOG_CATEGORY_STATIC(LogSceneRenderer, Info);

namespace zx::render {

using namespace rhi;

namespace {

constexpr u32 InstanceStride = 192; // keep in sync with ForwardPBR.hlsl (+ previous world rows at 128, Velocity.hlsl)
constexpr u32 MaxLights = 1024;
constexpr u32 MaxDecals = 256;

struct GpuDecal
{
    Vec4 worldToDecal[4]; // rows
    Vec3 axisY;
    u32 albedo;
    Vec3 axisX;
    u32 normal;
    Vec3 color;
    f32 opacity;
    f32 roughness;
    f32 affectRoughness;
    f32 normalStrength;
    f32 angleFade;
    f32 emissive;
    f32 pad[3];
};
static_assert(sizeof(GpuDecal) == 144);
constexpr u32 ClusterGridX = 16, ClusterGridY = 9, ClusterGridZ = 24; // keep in sync with Lights.hlsli
constexpr u32 ClusterCount = ClusterGridX * ClusterGridY * ClusterGridZ;
constexpr u32 ClusterBytes = 256;
constexpr f32 ClusterNear = 0.1f, ClusterFar = 400.0f;

constexpr u32 LocalFaceStride = 80; // 4 matrix rows + uv rect (keep in sync with Lights.hlsli)
constexpr u32 MaxLocalFaces = 64;

} // namespace

struct GpuLightData
{
    Vec3 position;
    f32 range;
    Vec3 color;
    u32 type;
    Vec3 direction;
    f32 spotScale;
    f32 spotOffset;
    u32 shadowFace;   // first face in the local shadow face buffer, or invalid
    f32 shadowTexel;  // 2 * tan(fov / 2) / tile size: world texel size per meter of distance
    f32 pad;
};
static_assert(sizeof(GpuLightData) == 64);
using GpuLight = GpuLightData;

namespace {

struct DrawPush
{
    u32 instanceIndex;
    u32 materialIndex;
    u32 vertexBuffer;
    u32 lod;
    u32 drawId = InvalidIndex; // direct draw (indirect ones get theirs from DrawIndexedIndirect)
};

Result<ShaderBytecode> Compile(StringView entry, ShaderStage stage, Span<const ShaderDefine> defines)
{
    ShaderCompileDesc d;
    d.path = "/Shaders/Forward/ForwardPBR.hlsl";
    d.entryPoint = entry;
    d.stage = stage;
    d.defines = defines;
    return ShaderCompiler::Compile(d);
}

f32 MaxScale(const Mat4& m)
{
    return Max(Max(Length(m.columns[0].XYZ()), Length(m.columns[1].XYZ())), Length(m.columns[2].XYZ()));
}

} // namespace

SceneRenderer::SceneRenderer(Device& device, RenderAssets& assets) : m_device(device), m_assets(assets) {}

SceneRenderer::~SceneRenderer()
{
    for (auto& kind : m_pipelines)
        for (Pipeline* p : kind)
            m_device.Destroy(p);
    for (auto& kv : m_graphPipelines)
        m_device.Destroy(kv.second);
    for (Buffer* b : m_instanceBuffers)
        m_device.Destroy(b);
    for (Cascade& c : m_cascades)
        m_device.Destroy(c.map);
    for (Buffer* b : m_lightBuffers)
        m_device.Destroy(b);
    for (Buffer* b : m_decalBuffers)
        m_device.Destroy(b);
    for (Buffer* b : m_localFaceBuffers)
        m_device.Destroy(b);
    m_device.Destroy(m_localAtlas);
    m_device.Destroy(m_clearTilePipeline);
    m_device.Destroy(m_deferredPipeline);
    m_device.Destroy(m_clusterBuffer);
    m_device.Destroy(m_clusterPipeline);
    m_device.Destroy(m_velocityPipeline);
    m_device.Destroy(m_gtaoPipeline);
    m_device.Destroy(m_contactPipeline);
    m_device.Destroy(m_aoBlurPipeline);
    for (Buffer* b : m_cullItems)
        m_device.Destroy(b);
    for (Buffer* b : m_cullGroups)
        m_device.Destroy(b);
    for (Buffer* b : m_cullSlots)
        m_device.Destroy(b);
    m_device.Destroy(m_indirectArgs);
    m_device.Destroy(m_visibleLists);
    m_device.Destroy(m_candidates);
    for (auto& set : m_hiz)
        for (Texture* t : set)
            m_device.Destroy(t);
    m_device.Destroy(m_cullInitPipeline);
    m_device.Destroy(m_cullPipeline);
    m_device.Destroy(m_hizPipeline);
}

void SceneRenderer::InvalidateShadows()
{
    for (Cascade& c : m_cascades)
        c.valid = false;
    for (LocalSlot& s : m_localSlots)
        s.valid = false;
}

bool SceneRenderer::Init(Format colorFormat, Format depthFormat)
{
    m_colorFormat = colorFormat;
    m_depthFormat = depthFormat;
    const ShaderDefine depthOnly[] = {{"DEPTH_ONLY"}};
    const ShaderDefine depthAlpha[] = {{"DEPTH_ONLY"}, {"ALPHA_TEST"}};
    const ShaderDefine alpha[] = {{"ALPHA_TEST"}};
    const ShaderDefine normals[] = {{"ZX_NORMAL_OUTPUT"}};
    const ShaderDefine alphaNormals[] = {{"ALPHA_TEST"}, {"ZX_NORMAL_OUTPUT"}};
    struct Job
    {
        ShaderBytecode* out;
        StringView entry;
        ShaderStage stage;
        Span<const ShaderDefine> defines;
    } jobs[] = {
        {&m_vs, "VSMain", ShaderStage::Vertex, {}},
        {&m_vsDepth, "VSMain", ShaderStage::Vertex, Span<const ShaderDefine>(depthOnly)},
        {&m_vsDepthAlpha, "VSMain", ShaderStage::Vertex, Span<const ShaderDefine>(depthAlpha)},
        {&m_ps, "PSMain", ShaderStage::Pixel, {}},
        {&m_psAlpha, "PSMain", ShaderStage::Pixel, Span<const ShaderDefine>(alpha)},
        {&m_psDepthAlpha, "PSDepth", ShaderStage::Pixel, Span<const ShaderDefine>(depthAlpha)},
        {&m_psNormals, "PSMain", ShaderStage::Pixel, Span<const ShaderDefine>(normals)},
        {&m_psAlphaNormals, "PSMain", ShaderStage::Pixel, Span<const ShaderDefine>(alphaNormals)},
        {&m_psGBuffer, "PSGBuffer", ShaderStage::Pixel, {}},
        {&m_psGBufferAlpha, "PSGBuffer", ShaderStage::Pixel, Span<const ShaderDefine>(alpha)},
        {&m_deferredVS, "DeferredVS", ShaderStage::Vertex, {}},
        {&m_deferredPS, "DeferredPS", ShaderStage::Pixel, {}},
    };
    for (const Job& j : jobs) {
        Result<ShaderBytecode> r = Compile(j.entry, j.stage, j.defines);
        if (!r) {
            ZX_LOG_ERROR(LogSceneRenderer, "{}", r.GetError().message);
            return false;
        }
        *j.out = move(*r);
    }
    const Job velocityJobs[] = {{&m_velocityVS, "VelocityVS", ShaderStage::Vertex, {}}, {&m_velocityPS, "VelocityPS", ShaderStage::Pixel, {}}};
    for (const Job& j : velocityJobs) {
        ShaderCompileDesc vd;
        vd.path = "/Shaders/Forward/Velocity.hlsl";
        vd.entryPoint = j.entry;
        vd.stage = j.stage;
        Result<ShaderBytecode> r = ShaderCompiler::Compile(vd);
        if (!r) {
            ZX_LOG_ERROR(LogSceneRenderer, "{}", r.GetError().message);
            return false;
        }
        *j.out = move(*r);
    }
    {
        // Deferred lighting: fullscreen, added over the emission the G-buffer pass wrote (scene alpha kept).
        GraphicsPipelineDesc dp;
        dp.vertexShader = &m_deferredVS;
        dp.pixelShader = &m_deferredPS;
        dp.rasterizer.cullMode = CullMode::None;
        dp.depthStencil.depthTest = false;
        dp.depthStencil.depthWrite = false;
        dp.colorFormats[0] = m_colorFormat;
        dp.colorCount = 1;
        BlendTarget add;
        add.enable = true;
        add.srcColor = BlendFactor::One;
        add.dstColor = BlendFactor::One;
        add.srcAlpha = BlendFactor::Zero;
        add.dstAlpha = BlendFactor::One;
        add.writeMask = 0x7;
        dp.blend[0] = add;
        dp.debugName = "DeferredLighting";
        m_deferredPipeline = m_device.CreateGraphicsPipeline(dp);
    }
    {
        GraphicsPipelineDesc vp;
        vp.vertexShader = &m_velocityVS;
        vp.pixelShader = &m_velocityPS;
        vp.rasterizer.cullMode = CullMode::None; // two-sided parts too
        vp.depthFormat = m_depthFormat;
        vp.depthStencil.depthWrite = false;     // tested against the scene: only visible surfaces write
        vp.colorFormats[0] = Format::R16G16_FLOAT;
        vp.colorCount = 1;
        vp.debugName = "Velocity";
        m_velocityPipeline = m_device.CreateGraphicsPipeline(vp);
    }
    ShaderCompileDesc cd;
    cd.path = "/Shaders/Lighting/ClusterLights.hlsl";
    cd.entryPoint = "CSMain";
    cd.stage = ShaderStage::Compute;
    Result<ShaderBytecode> cs = ShaderCompiler::Compile(cd);
    if (!cs) {
        ZX_LOG_ERROR(LogSceneRenderer, "{}", cs.GetError().message);
        return false;
    }
    m_clusterCS = move(*cs);
    for (auto [out, entry] : {Pair<ShaderBytecode*, StringView>{&m_gtaoCS, "GTAOCS"}, Pair<ShaderBytecode*, StringView>{&m_aoBlurCS, "BlurCS"}}) {
        ShaderCompileDesc ad;
        ad.path = "/Shaders/Lighting/GTAO.hlsl";
        ad.entryPoint = entry;
        ad.stage = ShaderStage::Compute;
        Result<ShaderBytecode> r = ShaderCompiler::Compile(ad);
        if (!r) {
            ZX_LOG_ERROR(LogSceneRenderer, "{}", r.GetError().message);
            return false;
        }
        *out = move(*r);
    }
    {
        ComputePipelineDesc ad;
        ad.computeShader = &m_gtaoCS;
        ad.debugName = "GTAO";
        m_gtaoPipeline = m_device.CreateComputePipeline(ad);
        ad.computeShader = &m_aoBlurCS;
        ad.debugName = "GTAO.Blur";
        m_aoBlurPipeline = m_device.CreateComputePipeline(ad);
    }
    {
        ShaderCompileDesc ad;
        ad.path = "/Shaders/Lighting/ContactShadows.hlsl";
        ad.entryPoint = "ContactShadowsCS";
        ad.stage = ShaderStage::Compute;
        Result<ShaderBytecode> r = ShaderCompiler::Compile(ad);
        if (!r) {
            ZX_LOG_ERROR(LogSceneRenderer, "{}", r.GetError().message);
            return false;
        }
        m_contactCS = move(*r);
        ComputePipelineDesc pd;
        pd.computeShader = &m_contactCS;
        pd.debugName = "ContactShadows";
        m_contactPipeline = m_device.CreateComputePipeline(pd);
    }
    ComputePipelineDesc cpd;
    cpd.computeShader = &m_clusterCS;
    cpd.debugName = "ClusterLights";
    m_clusterPipeline = m_device.CreateComputePipeline(cpd);
    for (Buffer*& b : m_lightBuffers)
        b = m_device.CreateBuffer(BufferDesc{u64(MaxLights) * sizeof(GpuLight), BufferUsage::ShaderResource, MemoryType::Upload, 0, "Lights"});
    for (Buffer*& b : m_decalBuffers)
        b = m_device.CreateBuffer(BufferDesc{u64(MaxDecals) * sizeof(GpuDecal), BufferUsage::ShaderResource, MemoryType::Upload, 0, "Decals"});
    m_clusterBuffer = m_device.CreateBuffer(BufferDesc{u64(ClusterCount) * ClusterBytes,
                                                       BufferUsage::ShaderResource | BufferUsage::UnorderedAccess,
                                                       MemoryType::GpuOnly, 0, "LightClusters"});
    {
        ShaderCompileDesc vd;
        vd.path = "/Shaders/Forward/ForwardPBR.hlsl";
        vd.entryPoint = "ClearTileVS";
        vd.stage = ShaderStage::Vertex;
        Result<ShaderBytecode> r = ShaderCompiler::Compile(vd);
        if (!r) {
            ZX_LOG_ERROR(LogSceneRenderer, "{}", r.GetError().message);
            return false;
        }
        m_clearTileVS = move(*r);
        GraphicsPipelineDesc pd;
        pd.vertexShader = &m_clearTileVS;
        pd.rasterizer.cullMode = CullMode::None;
        pd.depthStencil.depthCompare = CompareOp::Always;
        pd.depthFormat = Format::D32_FLOAT;
        pd.debugName = "ShadowAtlas.ClearTile";
        m_clearTilePipeline = m_device.CreateGraphicsPipeline(pd);
        for (Buffer*& b : m_localFaceBuffers)
            b = m_device.CreateBuffer(BufferDesc{u64(MaxLocalFaces) * LocalFaceStride, BufferUsage::ShaderResource,
                                                 MemoryType::Upload, 0, "LocalShadowFaces"});
    }
    m_shadersOk = true;
    if (!InitGpuDriven())
        ZX_LOG_WARN(LogSceneRenderer, "GPU-driven rendering unavailable: CPU culling and draws");
    EnsureInstanceCapacity(1024);
    return m_instanceBuffers[0] && m_clusterPipeline && m_clusterBuffer;
}

void SceneRenderer::SetGBufferFormats(GraphicsPipelineDesc& pd) const
{
    pd.colorFormats[0] = m_colorFormat;              // emission / unlit
    pd.colorFormats[1] = Format::R8G8B8A8_SRGB;      // base color + material AO
    pd.colorFormats[2] = Format::R10G10B10A2_UNORM;  // normal + roughness (also read by SSR)
    pd.colorFormats[3] = Format::R8G8B8A8_UNORM;     // subsurface + metallic
    pd.colorCount = 4;
}

Pipeline* SceneRenderer::GetPipeline(u32 kind, PipelineVariant variant, u32 shader)
{
    const bool masked = kind == Masked || kind == MaskedTwoSided;
    const bool depthOnly = variant == VariantDepth || variant == VariantShadow;
    // Material graphs: their own pixel shaders once compiled (opaque depth passes need no pixel shader at all).
    if (shader != 0 && !(depthOnly && !masked)) {
        if (const MaterialShaderCache::Shaders* shaders = m_assets.Shaders().Get(shader)) {
            const u64 key = (u64(shader) << 16) | (u64(kind) << 8) | u64(variant);
            Pipeline*& gp = *m_graphPipelines.Insert(key, nullptr).first;
            if (variant == VariantShadow && gp &&
                (m_shadowPipelineBias[0] != m_settings.shadows.depthBias || m_shadowPipelineBias[1] != m_settings.shadows.slopeBias)) {
                m_device.Destroy(gp);
                gp = nullptr;
            }
            if (gp)
                return gp;
            Pipeline* standard = GetPipeline(kind, variant); // builds the matching fixed-function state
            if (!standard)
                return nullptr;
            const bool twoSided = kind == OpaqueTwoSided || kind == MaskedTwoSided || kind == TranslucentTwoSided;
            const bool translucent = kind == Translucent || kind == TranslucentTwoSided;
            GraphicsPipelineDesc pd;
            pd.rasterizer.cullMode = twoSided ? CullMode::None : CullMode::Back;
            pd.depthFormat = m_depthFormat;
            pd.depthStencil.depthWrite = variant != VariantMainNoWrite && variant != VariantMainNoWriteMRT && variant != VariantGBuffer && !translucent;
            if (variant == VariantShadow) {
                pd.rasterizer.cullMode = CullMode::None;
                pd.rasterizer.depthBias = i32(m_settings.shadows.depthBias);
                pd.rasterizer.slopeScaledDepthBias = m_settings.shadows.slopeBias;
            }
            if (depthOnly) {
                pd.vertexShader = &m_vsDepthAlpha;
                pd.pixelShader = &shaders->depthAlphaTest;
                pd.colorCount = 0;
            } else if (variant == VariantGBuffer) {
                pd.vertexShader = &m_vs;
                pd.pixelShader = masked ? &shaders->gbufferAlphaTest : &shaders->gbuffer;
                SetGBufferFormats(pd);
            } else {
                pd.vertexShader = &m_vs;
                const bool mrt = variant == VariantMainMRT || variant == VariantMainNoWriteMRT;
                pd.pixelShader = masked ? (mrt ? &shaders->mainAlphaTestNormals : &shaders->mainAlphaTest)
                                        : (mrt ? &shaders->mainNormals : &shaders->main);
                pd.colorFormats[0] = m_colorFormat;
                pd.colorFormats[1] = Format::R10G10B10A2_UNORM;
                pd.colorCount = mrt ? 2 : 1;
                if (translucent)
                    pd.blend[0] = BlendTarget::AlphaBlend();
            }
            const String name = String::Format("Forward.Graph{}.{}.{}", shader, u32(kind), u32(variant));
            pd.debugName = name.View();
            gp = m_device.CreateGraphicsPipeline(pd);
            if (gp)
                return gp;
        }
    }
    Pipeline*& p = m_pipelines[kind][variant];
    if (variant == VariantShadow && p &&
        (m_shadowPipelineBias[0] != m_settings.shadows.depthBias || m_shadowPipelineBias[1] != m_settings.shadows.slopeBias)) {
        m_device.Destroy(p); // bias changed (tweaking); deferred destruction
        p = nullptr;
    }
    if (p)
        return p;
    const bool twoSided = kind == OpaqueTwoSided || kind == MaskedTwoSided || kind == TranslucentTwoSided;
    const bool translucent = kind == Translucent || kind == TranslucentTwoSided;
    GraphicsPipelineDesc pd;
    pd.rasterizer.cullMode = twoSided ? CullMode::None : CullMode::Back;
    pd.depthFormat = m_depthFormat;
    pd.depthStencil.depthWrite = variant != VariantMainNoWrite && variant != VariantMainNoWriteMRT && variant != VariantGBuffer && !translucent;
    if (variant == VariantShadow) {
        // Thin and open geometry is common in levels: render both faces into the shadow map, biased.
        pd.rasterizer.cullMode = CullMode::None;
        pd.rasterizer.depthBias = i32(m_settings.shadows.depthBias);
        pd.rasterizer.slopeScaledDepthBias = m_settings.shadows.slopeBias;
        m_shadowPipelineBias[0] = m_settings.shadows.depthBias;
        m_shadowPipelineBias[1] = m_settings.shadows.slopeBias;
    }
    if (depthOnly) {
        pd.vertexShader = masked ? &m_vsDepthAlpha : &m_vsDepth;
        pd.pixelShader = masked ? &m_psDepthAlpha : nullptr;
        pd.colorCount = 0;
    } else if (variant == VariantGBuffer) {
        pd.vertexShader = &m_vs;
        pd.pixelShader = masked ? &m_psGBufferAlpha : &m_psGBuffer;
        SetGBufferFormats(pd);
    } else {
        pd.vertexShader = &m_vs;
        const bool mrt = variant == VariantMainMRT || variant == VariantMainNoWriteMRT;
        pd.pixelShader = masked ? (mrt ? &m_psAlphaNormals : &m_psAlpha) : (mrt ? &m_psNormals : &m_ps);
        pd.colorFormats[0] = m_colorFormat;
        pd.colorFormats[1] = Format::R10G10B10A2_UNORM; // normal + roughness (MRT variants)
        pd.colorCount = mrt ? 2 : 1;
        if (translucent)
            pd.blend[0] = BlendTarget::AlphaBlend();
    }
    static const char* kinds[KindCount] = {"Opaque", "Opaque2S", "Masked", "Masked2S", "Translucent", "Translucent2S"};
    static const char* variants[VariantCount] = {"Depth", "Main", "MainNoWrite", "Shadow", "MainMRT", "MainNoWriteMRT", "GBuffer"};
    const String name = String::Format("Forward.{}.{}", kinds[kind], variants[variant]);
    pd.debugName = name.View();
    p = m_device.CreateGraphicsPipeline(pd);
    return p;
}

void SceneRenderer::EnsureInstanceCapacity(u32 count)
{
    if (count <= m_instanceCapacity)
        return;
    u32 capacity = Max(m_instanceCapacity, 1024u);
    while (capacity < count)
        capacity *= 2;
    for (Buffer*& b : m_instanceBuffers) {
        m_device.Destroy(b); // deferred until the GPU is done with it
        b = m_device.CreateBuffer(
            BufferDesc{u64(capacity) * InstanceStride, BufferUsage::ShaderResource, MemoryType::Upload, 0, "Instances"});
    }
    m_instanceCapacity = capacity;
}

u32 SceneRenderer::GetMaterialSampler(u32 maxAnisotropy)
{
    u32 slot = 0;
    while (slot < 4 && (2u << slot) <= maxAnisotropy)
        ++slot;
    if (m_samplers[slot] == InvalidDescriptor) {
        SamplerDesc sd;
        const Filter f = slot == 0 ? Filter::Linear : Filter::Anisotropic;
        sd.minFilter = sd.magFilter = f;
        sd.mipFilter = Filter::Linear;
        sd.maxAnisotropy = 1u << slot;
        m_samplers[slot] = m_device.CreateSampler(sd);
    }
    return m_samplers[slot];
}

void SceneRenderer::SetupCascades(const RenderView& view, const SceneLighting& lighting)
{
    const ShadowSettings& ss = m_settings.shadows;
    const u32 count = ss.enabled ? Clamp(ss.cascades, 1u, MaxCascades) : 0;
    const Vec3 lightDir = Normalize(lighting.sunDirection);
    // (Re)create the per-cascade maps; any setting or light change invalidates the cache.
    if (count != m_cascadeCount || ss.resolution != m_shadowResolution || !ss.cache ||
        LengthSq(lightDir - m_shadowLightDir) > 1e-10f) {
        InvalidateShadows();
        m_shadowLightDir = lightDir;
    }
    if (ss.resolution != m_shadowResolution) {
        for (Cascade& c : m_cascades) {
            m_device.Destroy(c.map); // deferred
            c.map = nullptr;
        }
        m_shadowResolution = ss.resolution;
    }
    for (u32 c = 0; c < count; ++c)
        if (!m_cascades[c].map) {
            TextureDesc td;
            td.width = td.height = ss.resolution;
            td.format = Format::D32_FLOAT;
            td.usage = TextureUsage::DepthStencil | TextureUsage::ShaderResource;
            td.debugName = "SunShadowCascade";
            m_cascades[c].map = m_device.CreateTexture(td);
        }
    m_cascadeCount = count;
    m_viewConstants.cascadeCount = count;
    if (!count)
        return;

    const Mat4 invView = InverseAffine(view.view);
    const Vec3 right = invView.columns[0].XYZ(), up = invView.columns[1].XYZ(), forward = invView.columns[2].XYZ();
    const f32 tanY = Tan(view.fovY * 0.5f);
    const f32 tanX = tanY * f32(view.width) / f32(Max(view.height, 1u));
    const f32 nearZ = Max(view.nearPlane, 0.05f), farZ = Max(ss.distance, nearZ + 1.0f);
    const Vec3 lightUp = Abs(lightDir.y) > 0.99f ? Vec3(0, 0, 1) : Vec3::Up();
    const f32 res = f32(ss.resolution);
    const f32 backDistance = 150.0f; // casters behind the view slice (towards the sun) still cast into it

    // Desired cascade spheres for this frame.
    f32 prevSplit = nearZ;
    Sphere desired[MaxCascades];
    for (u32 c = 0; c < count; ++c) {
        const f32 t = f32(c + 1) / f32(count);
        const f32 uniform = nearZ + (farZ - nearZ) * t;
        const f32 logarithmic = nearZ * std::pow(farZ / nearZ, t);
        const f32 split = uniform + (logarithmic - uniform) * ss.splitLambda;
        // Bounding sphere of the frustum slice [prevSplit, split]: rotation-invariant -> stable cascades.
        Vec3 corners[8];
        Vec3 center = Vec3(0, 0, 0);
        for (u32 i = 0; i < 8; ++i) {
            const f32 d = (i & 4) ? split : prevSplit;
            corners[i] = view.position + forward * d + right * (((i & 1) ? 1.0f : -1.0f) * tanX * d) +
                         up * (((i & 2) ? 1.0f : -1.0f) * tanY * d);
            center += corners[i];
        }
        center = center / 8.0f;
        f32 radius = 0.0f;
        for (const Vec3& p : corners)
            radius = Max(radius, Length(p - center));
        desired[c] = Sphere{center, radius};
        m_viewConstants.cascadeSplits[c] = split;
        prevSplit = split;
    }

    // Decide which cascades to re-render: invalid ones, those whose slice left the cached bounds (at most
    // maxUpdatesPerFrame per frame, the nearest first) and those a moved caster touches (always: a stale shadow is
    // wrong, not just blurry). The rest keep last frame's map.
    auto touchedByMovedCaster = [&](const Cascade& k) {
        for (const Sphere& s : m_movedCasters) {
            const Vec3 d = s.center - k.center;
            const f32 along = Dot(d, lightDir); // the map spans [-(radius + back), radius] along the light
            const f32 reach = k.radius * 1.4143f + s.radius; // square map: its corners
            if (along > k.radius + s.radius || along < -(k.radius + backDistance) - s.radius)
                continue;
            if (LengthSq(d - lightDir * along) <= reach * reach)
                return true;
        }
        return false;
    };
    u32 updates = 0;
    for (u32 c = 0; c < count; ++c) {
        Cascade& k = m_cascades[c];
        const bool inside = k.valid && Length(desired[c].center - k.center) + desired[c].radius <= k.radius;
        const bool dirty = k.valid && touchedByMovedCaster(k);
        if (dirty && inside) { // same bounds, new content
            k.update = true;
            ++updates;
            continue;
        }
        k.update = (!inside && (updates < Max(1u, ss.maxUpdatesPerFrame) || !k.valid)) || dirty;
        if (!k.update)
            continue;
        ++updates;
        f32 radius = desired[c].radius * (1.0f + (ss.cache ? ss.cacheMargin : 0.0f));
        radius = std::ceil(radius * 16.0f) / 16.0f;
        const Vec3 center = desired[c].center;
        const Mat4 lightView = Mat4::LookTo(center - lightDir * (radius + backDistance), lightDir, lightUp);
        Mat4 lightProj = Mat4::OrthographicReversedZ(-radius, radius, -radius, radius, 0.0f, 2.0f * radius + backDistance);
        // Texel snapping: keep the world origin on a texel center so the map does not shimmer when refreshed.
        const Vec3 origin = (lightProj * lightView).TransformPoint(Vec3(0, 0, 0));
        const f32 tx = origin.x * res * 0.5f, ty = origin.y * res * 0.5f;
        lightProj = Mat4::Translation(Vec3((std::round(tx) - tx) * 2.0f / res, (std::round(ty) - ty) * 2.0f / res, 0.0f)) * lightProj;
        k.viewProj = lightProj * lightView;
        k.center = center;
        k.radius = radius;
        k.valid = true;
    }
    m_stats.cascadesUpdated = updates;
    // NDC -> uv (y down).
    const Mat4 toUv(Vec4(0.5f, 0, 0, 0), Vec4(0, -0.5f, 0, 0), Vec4(0, 0, 1, 0), Vec4(0.5f, 0.5f, 0, 1));
    for (u32 c = 0; c < count; ++c) {
        m_viewConstants.shadowMatrices[c] = toUv * m_cascades[c].viewProj;
        m_viewConstants.cascadeTexelWorld[c] = 2.0f * m_cascades[c].radius / res;
        m_viewConstants.cascadeDepthRange[c] = 2.0f * m_cascades[c].radius + backDistance; // OrthographicReversedZ range
        m_viewConstants.shadowMaps[c] = m_cascades[c].map ? m_cascades[c].map->GetSrvIndex() : InvalidDescriptor;
    }
    m_viewConstants.cameraForward = forward;
    m_viewConstants.shadowTexel = 1.0f / res;
    m_viewConstants.normalBias = ss.normalBias;
    m_viewConstants.shadowFilter = ss.filter;
    m_viewConstants.sunSourceTan = Tan(ss.sourceAngle * 0.5f * Pi / 180.0f);
    m_viewConstants.pcssMaxTexels = Max(ss.maxPenumbraTexels, 1.0f);
    m_viewConstants.cascadeBlend = ss.cascadeBlend;
}

void SceneRenderer::PrepareLocalShadows(Span<const LightDesc> lights, const RenderView& view, GpuLightData* gpuLights,
                                        Span<const u32> gpuIndexOfLight)
{
    const ShadowSettings& ss = m_settings.shadows;
    m_viewConstants.localShadowAtlas = InvalidDescriptor;
    if (!ss.localEnabled || !ss.enabled)
        return;
    // (Re)create the atlas.
    if (ss.localAtlasSize != m_localAtlasSize || ss.localTileSize != m_localTileSize || !m_localAtlas) {
        m_device.Destroy(m_localAtlas);
        TextureDesc td;
        td.width = td.height = ss.localAtlasSize;
        td.format = Format::D32_FLOAT;
        td.usage = TextureUsage::DepthStencil | TextureUsage::ShaderResource;
        td.debugName = "LocalShadowAtlas";
        m_localAtlas = m_device.CreateTexture(td);
        m_localAtlasSize = ss.localAtlasSize;
        m_localTileSize = ss.localTileSize;
        const u32 perRow = m_localAtlasSize / m_localTileSize;
        m_localSlots.Clear();
        m_localSlots.Resize(perRow * perRow);
    }
    if (!m_localAtlas)
        return;

    // Candidates: visible (uploaded) shadow-casting lights, most important first.
    struct Candidate
    {
        u32 light;
        f32 priority;
    };
    Array<Candidate> candidates;
    for (usize li = 0; li < lights.Size(); ++li)
        if (lights[li].castShadows && gpuIndexOfLight[li] != InvalidIndex) {
            const f32 d = Length(lights[li].position - view.position);
            candidates.PushBack(Candidate{u32(li), lights[li].range / Max(d, 1.0f)}); // nearest (and largest) first
        }
    Sort(candidates, [](const Candidate& a, const Candidate& b) { return a.priority > b.priority; });

    Buffer* faceBuffer = m_localFaceBuffers[m_device.GetFrameIndex() % FramesInFlight];
    u8* faceData = static_cast<u8*>(faceBuffer->GetMappedData());
    const u32 perRow = m_localAtlasSize / m_localTileSize;
    const f32 tileUv = f32(m_localTileSize) / f32(m_localAtlasSize);
    // Slightly wider frusta keep the PCF kernel inside the tile.
    const f32 margin = 1.0f + 3.0f / f32(m_localTileSize);
    u32 faces = 0, updates = 0;
    static const Vec3 cubeDirs[6] = {Vec3(1, 0, 0), Vec3(-1, 0, 0), Vec3(0, 1, 0), Vec3(0, -1, 0), Vec3(0, 0, 1), Vec3(0, 0, -1)};
    static const Vec3 cubeUps[6] = {Vec3(0, 1, 0), Vec3(0, 1, 0), Vec3(0, 0, -1), Vec3(0, 0, 1), Vec3(0, 1, 0), Vec3(0, 1, 0)};
    for (const Candidate& c : candidates) {
        const LightDesc& l = lights[c.light];
        const bool point = l.type == LightType::Point;
        const u32 faceCount = point ? 6u : 1u;
        if (faces + faceCount > Min(ss.maxLocalFaces, MaxLocalFaces))
            break;
        const Vec3 dir = SafeNormalize(l.direction, Vec3(0, -1, 0));
        const f32 halfFov = point ? Atan(margin) : Min(l.outerAngle * margin + 0.02f, DegToRad(80.0f));
        // Find (or allocate) the tiles of all faces first; skip the light if the atlas is full.
        u32 slots[6];
        bool ok = true;
        for (u32 f = 0; f < faceCount && ok; ++f) {
            u32 found = InvalidIndex, lru = InvalidIndex;
            for (u32 si = 0; si < m_localSlots.Size(); ++si) {
                const LocalSlot& sl = m_localSlots[si];
                if (sl.lightId == l.id && sl.face == f && sl.lastUsed + 1 >= m_frame) {
                    found = si;
                    break;
                }
                if (sl.lastUsed < m_frame && (lru == InvalidIndex || sl.lastUsed < m_localSlots[lru].lastUsed))
                    lru = si;
            }
            if (found == InvalidIndex && lru != InvalidIndex) {
                found = lru;
                m_localSlots[found] = LocalSlot{l.id, f, Vec3(), Vec3(), 0, false};
            }
            ok = found != InvalidIndex;
            slots[f] = found;
            if (ok)
                m_localSlots[found].lastUsed = m_frame; // reserve
        }
        if (!ok)
            break;
        // Re-render faces whose content is missing or stale (moved light), within the per-frame budget.
        const bool moved = !(LengthSq(m_localSlots[slots[0]].position - l.position) < 1e-8f &&
                             (point || LengthSq(m_localSlots[slots[0]].direction - dir) < 1e-8f));
        bool usable = true;
        for (u32 f = 0; f < faceCount; ++f) {
            LocalSlot& sl = m_localSlots[slots[f]];
            if (sl.valid && !moved)
                continue;
            if (updates >= ss.maxLocalUpdatesPerFrame) {
                usable &= sl.valid; // keep a slightly stale map, or go unshadowed this frame
                continue;
            }
            ++updates;
            const Vec3 fwd = point ? cubeDirs[f] : dir;
            const Vec3 up = point ? cubeUps[f] : (Abs(dir.y) > 0.99f ? Vec3(0, 0, 1) : Vec3::Up());
            LocalUpdate& u = m_localUpdates.AddDefaulted();
            u.slot = slots[f];
            u.viewProj = Mat4::PerspectiveReversedZ(2.0f * halfFov, 1.0f, 0.05f, Max(l.range, 0.1f)) * Mat4::LookTo(l.position, fwd, up);
            sl.position = l.position;
            sl.direction = dir;
            sl.valid = true;
        }
        if (!usable)
            continue;
        // Face records (matrix rows + atlas rect) for the shader.
        const u32 first = faces;
        for (u32 f = 0; f < faceCount; ++f) {
            const LocalSlot& sl = m_localSlots[slots[f]];
            const Vec3 fwd = point ? cubeDirs[f] : dir;
            const Vec3 up = point ? cubeUps[f] : (Abs(dir.y) > 0.99f ? Vec3(0, 0, 1) : Vec3::Up());
            const Mat4 vp = Mat4::PerspectiveReversedZ(2.0f * halfFov, 1.0f, 0.05f, Max(l.range, 0.1f)) *
                            Mat4::LookTo(sl.position, fwd, up);
            const Mat4 rows = Transpose(vp);
            u8* dst = faceData + usize(faces) * LocalFaceStride;
            MemCopy(dst, &rows, 64);
            const u32 tile = slots[f];
            const Vec4 rect(tileUv, tileUv, f32(tile % perRow) * tileUv, f32(tile / perRow) * tileUv);
            MemCopy(dst + 64, &rect, 16);
            ++faces;
        }
        GpuLightData& g = gpuLights[gpuIndexOfLight[c.light]];
        g.shadowFace = first;
        g.shadowTexel = 2.0f * Tan(halfFov) / f32(m_localTileSize);
    }
    m_stats.localShadowFaces = faces;
    m_stats.localShadowUpdates = updates;
    m_viewConstants.localShadowAtlas = faces ? m_localAtlas->GetSrvIndex() : InvalidDescriptor;
    m_viewConstants.localShadowFaces = faceBuffer->GetSrvIndex();
    m_viewConstants.localAtlasTexel = 1.0f / f32(m_localAtlasSize);
    m_viewConstants.localNormalBias = ss.localNormalBias;

    // Casters per updated face (culled against the face frustum; coarse LODs by distance to the light).
    for (LocalUpdate& u : m_localUpdates) {
        const Frustum fr = Frustum::FromMatrix(u.viewProj);
        const Vec3 lightPos = m_localSlots[u.slot].position;
        for (const InstanceRef& caster : m_casters) {
            const RenderInstance& inst = *caster.instance;
            const f32 scale = MaxScale(inst.world);
            for (const asset::CookedMeshSection& section : caster.mesh->sections) {
                const Sphere sp{inst.world.TransformPoint(section.bounds.Center()), Length(section.bounds.Extents()) * scale};
                if (!fr.Intersects(sp))
                    continue;
                const AssetId materialId = section.materialSlot < inst.materials.Size() ? inst.materials[section.materialSlot] : AssetId();
                const u32 material = m_assets.GetMaterialIndex(materialId);
                const MaterialInfo& info = m_assets.GetMaterialInfo(material);
                if (info.blend == MaterialBlend::Translucent)
                    continue;
                const bool masked = info.blend == MaterialBlend::Masked;
                // LOD: error must stay below ~1.5 texels at the section's distance from the light.
                const f32 dist = Max(Length(sp.center - lightPos) - sp.radius, 0.1f);
                const f32 texel = dist * 2.0f / f32(m_localTileSize);
                u32 lod = Min(m_settings.shadows.lodBias, section.lodCount - 1);
                while (lod + 1 < section.lodCount && caster.mesh->SectionLodAt(section, lod + 1).error * scale <= texel * 1.5f)
                    ++lod;
                const asset::CookedSectionLod& range = caster.mesh->SectionLodAt(section, lod);
                u.draws.PushBack(DrawItem{u64(masked) << 56 | material, caster.index, material, caster.mesh, range.indexOffset,
                                          range.indexCount, u8(masked ? Masked : Opaque), u8(lod), info.shader});
            }
        }
        Sort(u.draws, [](const DrawItem& a, const DrawItem& b) { return a.sortKey < b.sortKey; });
    }
}

void SceneRenderer::Prepare(Span<const RenderInstance> instances, const RenderView& view, const SceneLighting& lighting,
                            const SceneRenderSettings& settings)
{
    ZX_PROFILE_SCOPE("SceneRenderer::Prepare");
    m_settings = settings;
    m_stats = {};
    m_stats.instances = u32(instances.Size());
    m_opaque.Clear();
    m_translucent.Clear();
    m_velocityDraws.Clear();
    for (Array<DrawItem>& s : m_shadowDraws)
        s.Clear();
    m_casters.Clear();
    m_localUpdates.Clear();
    EnsureInstanceCapacity(u32(instances.Size()));
    Buffer* instanceBuffer = m_instanceBuffers[m_device.GetFrameIndex() % FramesInFlight];
    u8* instanceData = static_cast<u8*>(instanceBuffer->GetMappedData());
    ++m_frame;
    // Shadow cache key: which meshes are resident and how many instances there are (streaming invalidates it).
    {
        const RenderAssets::Stats as = m_assets.GetStats();
        const u64 key = (u64(as.meshes) << 32) ^ (u64(instances.Size()) << 8) ^ as.materials;
        if (key != m_shadowContentKey) {
            m_shadowContentKey = key;
            InvalidateShadows();
        }
    }
    m_movedCasters.Clear();
    for (u32 i : m_movedInstances) {
        if (i >= instances.Size() || !instances[i].castShadows)
            continue;
        const RenderInstance& inst = instances[i];
        const GpuMesh* mesh = m_assets.GetMesh(inst.mesh);
        if (!mesh)
            continue;
        m_movedCasters.PushBack(Sphere{inst.world.TransformPoint(mesh->sphere.center), mesh->sphere.radius * MaxScale(inst.world)});
        if (inst.prevWorld) // its old shadow must go too
            m_movedCasters.PushBack(Sphere{inst.prevWorld->TransformPoint(mesh->sphere.center), mesh->sphere.radius * MaxScale(*inst.prevWorld)});
    }
    m_movedInstances.Clear();
    SetupCascades(view, lighting);
    Frustum cascadeFrustums[MaxCascades];
    u32 cascadeMask = 0; // cascades re-rendered this frame
    for (u32 c = 0; c < m_cascadeCount; ++c)
        if (m_cascades[c].update) {
            cascadeFrustums[c] = Frustum::FromMatrix(m_cascades[c].viewProj);
            cascadeMask |= 1u << c;
        }

    const Frustum frustum = Frustum::FromMatrix(view.viewProj);
    const f32 invTanHalfFov = 1.0f / Tan(view.fovY * 0.5f);
    m_gpuFrame = settings.gpuDriven && settings.depthPrepass && m_cullPipeline && m_cullInitPipeline && m_hizPipeline;
    if (m_gpuFrame) {
        PrepareGpu(instances, view, frustum, invTanHalfFov, instanceData);
    } else {
        u32 instanceCount = 0;
        for (const RenderInstance& inst : instances) {
            const GpuMesh* mesh = m_assets.GetMesh(inst.mesh);
            if (!mesh) {
                ++m_stats.notResident;
                continue;
            }
            const f32 scale = MaxScale(inst.world);
            const Sphere worldSphere{inst.world.TransformPoint(mesh->sphere.center), mesh->sphere.radius * scale};

            // Instance row: world matrix rows + normal matrix rows (inverse transpose of the upper 3x3).
            const u32 index = instanceCount++;
            const Mat4 rows = Transpose(inst.world);
            const Mat4 inverse = Inverse(inst.world);
            u8* dst = instanceData + usize(index) * InstanceStride;
            MemCopy(dst, &rows, 64);
            // w of those rows: burn charring, glow, embers; then the burn sphere (world origin, radius).
            const f32 burnParams[3] = {inst.charring, inst.burnGlow, inst.embers};
            for (u32 r = 0; r < 3; ++r) {
                // Row r of the inverse-transpose = column r of the inverse.
                const Vec4 col(inverse.columns[r].XYZ(), burnParams[r]);
                MemCopy(dst + 64 + r * 16, &col, 16);
            }
            const Vec4 burn(inst.burnOrigin, inst.burnRadius);
            MemCopy(dst + 112, &burn, 16);
            // Last frame's world rows (velocity of moving objects).
            const Mat4 prevRows = Transpose(inst.prevWorld ? *inst.prevWorld : inst.world);
            MemCopy(dst + 128, &prevRows, 64);
            const bool moved = inst.prevWorld != nullptr;
            if (inst.castShadows)
                m_casters.PushBack(InstanceRef{index, mesh, &inst});

            const bool inView = frustum.Intersects(worldSphere);
            bool inShadow = false;
            for (u32 c = 0; c < m_cascadeCount && inst.castShadows; ++c)
                inShadow |= (cascadeMask & (1u << c)) && cascadeFrustums[c].Intersects(worldSphere);
            if (!inView && !inShadow) {
                ++m_stats.culled;
                continue;
            }

            // Sections are spatial chunks: cull and pick a LOD for each one.
            const bool wholeMeshVisible = inView && frustum.ContainsSphere(worldSphere);
            for (const asset::CookedMeshSection& section : mesh->sections) {
                const Sphere s{inst.world.TransformPoint(section.bounds.Center()), Length(section.bounds.Extents()) * scale};
                const bool sectionInView = inView && (wholeMeshVisible || mesh->sections.Size() == 1 || frustum.Intersects(s));
                u32 shadowMask = 0;
                for (u32 c = 0; c < m_cascadeCount && inst.castShadows; ++c)
                    if ((cascadeMask & (1u << c)) && cascadeFrustums[c].Intersects(s))
                        shadowMask |= 1u << c;
                if (!sectionInView && !shadowMask) {
                    ++m_stats.culledSections;
                    continue;
                }
                const f32 distance = Length(s.center - view.position);
                // Screen size: projected bounding-sphere radius as a fraction of the view height.
                const f32 screenSize = s.radius * invTanHalfFov / Max(distance - s.radius, 1e-3f);
                u32 lod = 0;
                if (settings.forcedLod >= 0) {
                    lod = Min(u32(settings.forcedLod), section.lodCount - 1);
                } else {
                    for (u32 l = 1; l < section.lodCount; ++l)
                        if (screenSize < mesh->SectionLodAt(section, l).screenSize * settings.lodBias)
                            lod = l;
                }
                const AssetId materialId = section.materialSlot < inst.materials.Size() ? inst.materials[section.materialSlot] : AssetId();
                const u32 material = m_assets.GetMaterialIndex(materialId);
                const MaterialInfo& info = m_assets.GetMaterialInfo(material);
                const bool masked = info.blend == MaterialBlend::Masked;
                const u32 depthBits = u32(Min(distance, 65535.0f) * 16.0f) & 0xFFFFF; // 20 bits

                if (shadowMask && info.blend != MaterialBlend::Translucent) {
                    for (u32 c = 0; c < m_cascadeCount; ++c) {
                        if (!(shadowMask & (1u << c)))
                            continue;
                        // Casters smaller than ~2 shadow texels contribute nothing visible.
                        const f32 texel = m_viewConstants.cascadeTexelWorld[c];
                        if (s.radius < texel)
                            continue;
                        // LOD by shadow-map resolution: the coarsest level whose error stays under ~1.5 texels,
                        // but never finer than the main view's LOD + lodBias.
                        u32 shadowLod = Min(lod + settings.shadows.lodBias, section.lodCount - 1);
                        while (shadowLod + 1 < section.lodCount &&
                               mesh->SectionLodAt(section, shadowLod + 1).error * scale <= texel * 1.5f)
                            ++shadowLod;
                        const asset::CookedSectionLod& range = mesh->SectionLodAt(section, shadowLod);
                        m_shadowDraws[c].PushBack(DrawItem{u64(masked) << 56 | material, index, material, mesh, range.indexOffset,
                                                           range.indexCount, u8(masked ? Masked : Opaque), u8(shadowLod), info.shader});
                        ++m_stats.shadowDraws;
                        m_stats.shadowTriangles += range.indexCount / 3;
                    }
                }
                if (!sectionInView)
                    continue;
                ++m_stats.lodHistogram[Min(lod, 7u)];
                const asset::CookedSectionLod& range = mesh->SectionLodAt(section, lod);
                DrawItem d;
                d.instance = index;
                d.material = material;
                d.mesh = mesh;
                d.indexOffset = range.indexOffset;
                d.indexCount = range.indexCount;
                d.lod = u8(lod);
                d.shader = info.shader;
                if (info.blend == MaterialBlend::Translucent) {
                    d.pipeline = info.doubleSided ? TranslucentTwoSided : Translucent;
                    d.sortKey = ~u64(depthBits); // back to front
                    m_translucent.PushBack(d);
                } else {
                    d.pipeline = u8(masked ? (info.doubleSided ? MaskedTwoSided : Masked) : (info.doubleSided ? OpaqueTwoSided : Opaque));
                    // Pipeline, then front to back, then material.
                    d.sortKey = (u64(d.pipeline) << 56) | (u64(depthBits) << 24) | (material & 0xFFFFFF);
                    m_opaque.PushBack(d);
                    if (moved)
                        m_velocityDraws.PushBack(d);
                }
                ++m_stats.draws;
                m_stats.triangles += range.indexCount / 3;
            }
        }
    }
    Sort(m_opaque, [](const DrawItem& a, const DrawItem& b) { return a.sortKey < b.sortKey; });
    Sort(m_translucent, [](const DrawItem& a, const DrawItem& b) { return a.sortKey < b.sortKey; });
    for (u32 c = 0; c < m_cascadeCount; ++c)
        Sort(m_shadowDraws[c], [](const DrawItem& a, const DrawItem& b) { return a.sortKey < b.sortKey; });

    // Punctual lights: frustum cull, pre-expose, upload; clustering runs on the GPU.
    const f32 exposure = ExposureFromEV100(lighting.exposureEV100);
    {
        Buffer* lightBuffer = m_lightBuffers[m_device.GetFrameIndex() % FramesInFlight];
        GpuLight* gpu = static_cast<GpuLight*>(lightBuffer->GetMappedData());
        u32 count = 0;
        Array<u32> gpuIndexOfLight;
        gpuIndexOfLight.Resize(lighting.lights.Size(), InvalidIndex);
        for (usize li = 0; li < lighting.lights.Size(); ++li) {
            const LightDesc& l = lighting.lights[li];
            if (count == MaxLights || l.range <= 0.0f || !frustum.Intersects(Sphere{l.position, l.range}))
                continue;
            // Invisible at this exposure (< 5 % of display white even at 1 m): e.g. lamps in daylight.
            if (l.intensity * Max(Max(l.color.x, l.color.y), l.color.z) * exposure < 0.05f)
                continue;
            gpuIndexOfLight[li] = count;
            GpuLight& g = gpu[count++];
            g = {};
            g.shadowFace = InvalidIndex;
            g.position = l.position;
            g.range = l.range;
            g.color = l.color * (l.intensity * exposure);
            g.type = l.type == LightType::Spot ? 1u : 0u;
            g.direction = SafeNormalize(l.direction, Vec3(0, -1, 0));
            const f32 cosOuter = Cos(l.outerAngle), cosInner = Max(Cos(l.innerAngle), cosOuter + 1e-3f);
            g.spotScale = 1.0f / (cosInner - cosOuter);
            g.spotOffset = -cosOuter * g.spotScale;
        }
        m_stats.lights = count;
        PrepareLocalShadows(lighting.lights, view, gpu, gpuIndexOfLight.View());
        m_viewConstants.lightBuffer = lightBuffer->GetSrvIndex();
        m_viewConstants.clusterBuffer = m_clusterBuffer->GetSrvIndex();
        m_viewConstants.lightCount = count;
        const f32 logRatio = std::log(ClusterFar / ClusterNear);
        m_viewConstants.clusterDepthScale = f32(ClusterGridZ) / logRatio;
        m_viewConstants.clusterDepthBias = -f32(ClusterGridZ) * std::log(ClusterNear) / logRatio;
        m_viewConstants.clusterTileScale = Vec2(f32(ClusterGridX) / f32(view.width), f32(ClusterGridY) / f32(view.height));
        m_clusterParams.view = view.view;
        m_clusterParams.lightBuffer = lightBuffer->GetSrvIndex();
        m_clusterParams.clusterBuffer = m_clusterBuffer->GetUavIndex();
        m_clusterParams.lightCount = count;
        m_clusterParams.nearZ = ClusterNear;
        m_clusterParams.farZ = ClusterFar;
        m_clusterParams.tanHalfY = Tan(view.fovY * 0.5f);
        m_clusterParams.tanHalfX = m_clusterParams.tanHalfY * f32(view.width) / f32(Max(view.height, 1u));
    }
    m_viewConstants.exposure = exposure;
    m_renderSize = UVec2(view.width, view.height);
    m_viewConstants.invRenderSize = Vec2(1.0f / f32(view.width), 1.0f / f32(view.height));
    {
        const AOSettings& ao = settings.ao;
        const u32 div = ao.halfResolution ? 2u : 1u;
        m_aoParams = {};
        m_aoParams.aoSize = Vec2(f32(Max(1u, view.width / div)), f32(Max(1u, view.height / div)));
        m_aoParams.invAoSize = Vec2(1.0f / m_aoParams.aoSize.x, 1.0f / m_aoParams.aoSize.y);
        m_aoParams.tanHalfY = Tan(view.fovY * 0.5f);
        m_aoParams.tanHalfX = m_aoParams.tanHalfY * f32(view.width) / f32(Max(view.height, 1u));
        m_aoParams.nearZ = view.nearPlane;
        m_aoParams.radius = ao.radius;
        m_aoParams.maxRadiusPixels = ao.maxRadiusPixels / f32(div);
        m_aoParams.intensity = ao.intensity;
        m_aoParams.sliceCount = Max(1u, ao.slices);
        m_aoParams.stepCount = Max(1u, ao.steps);
        m_aoParams.frame = u32(m_frame);
    }
    {
        const ContactShadowSettings& cs = settings.contactShadows;
        m_contactParams = {};
        m_contactParams.size = Vec2(f32(view.width), f32(view.height));
        m_contactParams.invSize = Vec2(1.0f / f32(view.width), 1.0f / f32(view.height));
        m_contactParams.tanHalfY = Tan(view.fovY * 0.5f);
        m_contactParams.tanHalfX = m_contactParams.tanHalfY * f32(view.width) / f32(Max(view.height, 1u));
        m_contactParams.nearZ = view.nearPlane;
        m_contactParams.rayLength = cs.length;
        m_contactParams.stepCount = Clamp(cs.steps, 1u, 64u);
        m_contactParams.thickness = cs.thickness;
        m_contactParams.frame = u32(m_frame);
        const Vec4 toSun = view.view * Vec4(-Normalize(lighting.sunDirection), 0.0f);
        m_contactParams.toSunView = Normalize(toSun.XYZ());
    }

    m_viewConstants.viewProj = view.viewProj;
    m_viewConstants.invViewProj = Inverse(view.viewProj);
    m_viewConstants.cameraPosition = view.position;
    m_viewConstants.lightDirection = Normalize(lighting.sunDirection);
    m_viewConstants.lightIntensity = lighting.sunIlluminance * exposure;
    m_viewConstants.lightColor = lighting.sunColor;
    m_viewConstants.instanceBuffer = instanceBuffer->GetSrvIndex();
    m_viewConstants.skyColor = lighting.skyColor;
    m_viewConstants.materialBuffer = m_assets.GetMaterialBuffer()->GetSrvIndex();
    m_viewConstants.materialParams = m_assets.GetMaterialParamBuffer()->GetSrvIndex();
    m_viewConstants.time = m_time;
    m_viewConstants.groundColor = lighting.groundColor;
    m_viewConstants.ambientIntensity = lighting.skyLuminance * exposure;
    m_skyIrradianceBuffer = lighting.skyIrradianceBuffer;
    m_viewConstants.skyIrradianceSH = lighting.skyIrradianceBuffer ? 1u : InvalidDescriptor;
    m_viewConstants.skyIrradianceScale = lighting.skyIrradianceScale;
    m_viewConstants.debugMode = settings.debugMode;
    m_viewConstants.materialSampler = GetMaterialSampler(settings.maxAnisotropy);
    m_viewConstants.skyCube = lighting.skyCube;
    m_viewConstants.skyCubeMips = lighting.skyCubeMips;
    m_viewConstants.probeBuffer = lighting.probeBuffer;
    m_viewConstants.probeCount = lighting.probeBuffer != InvalidDescriptor ? lighting.probeCount : 0u;
    m_viewConstants.reflectionExposure = lighting.reflectionExposure;
    m_viewConstants.ssrTexture = lighting.ssrTexture;
    m_viewConstants.cloudShadow = lighting.cloudShadow;
    m_viewConstants.cloudShadowInvExtent = 1.0f / Max(lighting.cloudShadowExtent, 1.0f);
    m_viewConstants.cloudShadowOrigin = lighting.cloudShadowOrigin;
    m_viewConstants.ssrMaxRoughness = lighting.ssrMaxRoughness;
    m_viewConstants.ssrViewProj = lighting.ssrViewProj;
    PrepareDecals(lighting.decals, Frustum::FromMatrix(view.viewProj));
}

void SceneRenderer::PrepareDecals(Span<const DecalDesc> decals, const Frustum& frustum)
{
    m_viewConstants.decalBuffer = InvalidDescriptor;
    m_viewConstants.decalCount = 0;
    if (decals.Empty())
        return;
    // Visible decals in sortOrder (stable), so later entries are blended on top.
    Array<u32> order;
    for (u32 i = 0; i < u32(decals.Size()) && order.Size() < MaxDecals; ++i) {
        const DecalDesc& d = decals[i];
        const f32 radius = Length(d.world.columns[0].XYZ()) + Length(d.world.columns[1].XYZ()) + Length(d.world.columns[2].XYZ());
        if (d.opacity > 0.0f && frustum.Intersects(Sphere{d.world.GetTranslation(), radius}))
            order.PushBack(i);
    }
    for (usize i = 1; i < order.Size(); ++i)
        for (usize j = i; j > 0 && decals[order[j]].sortOrder < decals[order[j - 1]].sortOrder; --j) {
            const u32 t = order[j];
            order[j] = order[j - 1];
            order[j - 1] = t;
        }
    Buffer* buffer = m_decalBuffers[m_device.GetFrameIndex() % FramesInFlight];
    GpuDecal* gpu = static_cast<GpuDecal*>(buffer->GetMappedData());
    for (usize i = 0; i < order.Size(); ++i) {
        const DecalDesc& d = decals[order[i]];
        const Mat4 inv = Inverse(d.world);
        GpuDecal g = {};
        for (u32 r = 0; r < 4; ++r)
            g.worldToDecal[r] = Vec4(inv.columns[0][r], inv.columns[1][r], inv.columns[2][r], inv.columns[3][r]);
        g.axisY = SafeNormalize(d.world.columns[1].XYZ(), Vec3::Up());
        g.axisX = SafeNormalize(d.world.columns[0].XYZ(), Vec3::Right());
        g.albedo = d.albedo;
        g.normal = d.normal;
        g.color = d.color;
        g.opacity = Saturate(d.opacity);
        g.roughness = d.roughness;
        g.affectRoughness = d.affectRoughness ? 1.0f : 0.0f;
        g.normalStrength = d.normalStrength;
        g.angleFade = Saturate(d.angleFade);
        g.emissive = d.emissive;
        gpu[i] = g;
    }
    m_viewConstants.decalBuffer = order.Empty() ? InvalidDescriptor : buffer->GetSrvIndex();
    m_viewConstants.decalCount = u32(order.Size());
    m_stats.decals = u32(order.Size());
}

void SceneRenderer::Draw(CommandList& cmd, const DrawItem& d, PipelineVariant variant, Pipeline*& bound)
{
    Pipeline* p = GetPipeline(d.pipeline, variant, d.shader);
    if (!p)
        return;
    if (p != bound) {
        cmd.SetPipeline(p);
        bound = p;
    }
    cmd.SetIndexBuffer(d.mesh->indexBuffer, d.mesh->index16 ? IndexFormat::UInt16 : IndexFormat::UInt32);
    const DrawPush push{d.instance, d.material, d.mesh->vertexBuffer->GetSrvIndex(), d.lod};
    cmd.SetPushConstants(push);
    cmd.DrawIndexed(d.indexCount, 1, d.indexOffset, 0, 0);
}

void SceneRenderer::AddPasses(RenderGraph& graph, RGTexture color, RGTexture depth, LinearColor clearColor,
                              Function<void(RenderGraph&, RGTexture, RGTexture, RGTexture)> afterOpaque)
{
    if (!m_shadersOk)
        return;
    // GPU-driven: culling of the main view (phase 1) and of the cascades that re-render, before anything draws.
    const bool gpu = m_gpuFrame && m_slotCount > 0;
    const bool occlusion = gpu && m_settings.occlusionCulling;
    RGBuffer gpuArgs, gpuLists, gpuCandidates;
    if (gpu) {
        gpuArgs = graph.ImportBuffer(m_indirectArgs, ResourceState::IndirectArgument);
        gpuLists = graph.ImportBuffer(m_visibleLists, ResourceState::ShaderResource);
        gpuCandidates = graph.ImportBuffer(m_candidates, ResourceState::UnorderedAccess);
        u32 viewMask = 3u;
        for (u32 c = 0; c < m_cascadeCount; ++c)
            if (m_cascades[c].update)
                viewMask |= 1u << (2 + c);
        AddCullInitPass(graph, gpuArgs, gpuCandidates, viewMask);
        AddCullPass(graph, 0, occlusion && m_hizValid ? 1u : 0u, m_cullViewProj, gpuArgs, gpuLists, gpuCandidates, 0, 0.0f, 0);
        for (u32 c = 0; c < m_cascadeCount; ++c)
            if (m_cascades[c].update) // casters only (item flag 1), skipping ones under ~1 shadow texel
                AddCullPass(graph, 2 + c, 0, m_cascades[c].viewProj, gpuArgs, gpuLists, gpuCandidates, 1u,
                            m_viewConstants.cascadeTexelWorld[c], m_settings.shadows.lodBias);
    }
    auto readIndirect = [gpu, gpuArgs, gpuLists](PassBuilder& b) {
        if (gpu) {
            b.ReadBuffer(gpuArgs, ResourceState::IndirectArgument);
            b.ReadBuffer(gpuLists);
        }
    };
    RGTexture shadowMaps[MaxCascades];
    for (u32 c = 0; c < m_cascadeCount; ++c) {
        shadowMaps[c] = graph.ImportTexture(m_cascades[c].map, ResourceState::ShaderResource);
        if (!m_cascades[c].update)
            continue;
        const RGTexture target = shadowMaps[c];
        graph.AddPass(
            Name(String::Format("SunShadow{}", c)).CStr(), PassType::Graphics,
            [target, readIndirect](PassBuilder& b) {
                b.DepthStencil(target, LoadOp::Clear, 0.0f);
                b.SideEffects(); // persistent cache: written for later frames
                readIndirect(b);
            },
            [this, c, gpu](PassContext& ctx) {
                ViewConstants cascadeView = m_viewConstants;
                cascadeView.viewProj = m_cascades[c].viewProj;
                ctx.cmd.SetConstants(1, &cascadeView, sizeof(cascadeView));
                if (gpu) {
                    DrawIndirect(ctx.cmd, 2 + c, VariantShadow);
                    return;
                }
                Pipeline* bound = nullptr;
                for (const DrawItem& d : m_shadowDraws[c])
                    Draw(ctx.cmd, d, VariantShadow, bound);
            });
    }
    RGTexture localAtlas;
    const bool hasLocalShadows = m_viewConstants.localShadowAtlas != InvalidDescriptor || !m_localUpdates.Empty();
    if (m_localAtlas && hasLocalShadows) {
        localAtlas = graph.ImportTexture(m_localAtlas, ResourceState::ShaderResource);
        if (!m_localUpdates.Empty()) {
            const RGTexture target = localAtlas;
            graph.AddPass(
                "LocalShadows", PassType::Graphics,
                [target](PassBuilder& b) {
                    b.DepthStencil(target, LoadOp::Load, 0.0f);
                    b.SideEffects(); // persistent cache
                },
                [this](PassContext& ctx) {
                    const u32 perRow = m_localAtlasSize / m_localTileSize;
                    ViewConstants faceView = m_viewConstants;
                    for (const LocalUpdate& u : m_localUpdates) {
                        const u32 x = (u.slot % perRow) * m_localTileSize, y = (u.slot / perRow) * m_localTileSize;
                        ctx.cmd.SetViewport(Viewport{f32(x), f32(y), f32(m_localTileSize), f32(m_localTileSize), 0.0f, 1.0f});
                        ctx.cmd.SetScissor(i32(x), i32(y), m_localTileSize, m_localTileSize);
                        ctx.cmd.SetPipeline(m_clearTilePipeline); // depth 0 = far (reversed-Z)
                        ctx.cmd.Draw(3);
                        faceView.viewProj = u.viewProj;
                        ctx.cmd.SetConstants(1, &faceView, sizeof(faceView));
                        Pipeline* bound = nullptr;
                        for (const DrawItem& d : u.draws)
                            Draw(ctx.cmd, d, VariantShadow, bound);
                    }
                });
        }
    }
    const bool prepass = m_settings.depthPrepass;
    if (prepass) {
        graph.AddPass(
            "DepthPrepass", PassType::Graphics,
            [depth, readIndirect](PassBuilder& b) {
                b.DepthStencil(depth, LoadOp::Clear, 0.0f);
                readIndirect(b);
            },
            [this, gpu](PassContext& ctx) {
                ctx.cmd.SetConstants(1, &m_viewConstants, sizeof(m_viewConstants));
                if (gpu) {
                    DrawIndirect(ctx.cmd, 0, VariantDepth);
                    return;
                }
                Pipeline* bound = nullptr;
                for (const DrawItem& d : m_opaque)
                    Draw(ctx.cmd, d, VariantDepth, bound);
            });
    }
    if (occlusion) {
        // Phase 2: a min-depth pyramid of what phase 1 drew, then the items hidden behind the previous frame's depth
        // are tested again against it - the ones that became visible are drawn now.
        AddHiZPass(graph, depth);
        AddCullPass(graph, 1, 2, m_cullViewProj, gpuArgs, gpuLists, gpuCandidates, 0, 0.0f, 0);
        graph.AddPass(
            "DepthPrepass2", PassType::Graphics,
            [depth, readIndirect](PassBuilder& b) {
                b.DepthStencil(depth, LoadOp::Load, 0.0f);
                readIndirect(b);
            },
            [this](PassContext& ctx) {
                ctx.cmd.SetConstants(1, &m_viewConstants, sizeof(m_viewConstants));
                DrawIndirect(ctx.cmd, 1, VariantDepth);
            });
    }
    // GTAO from the pre-pass depth (normals reconstructed from depth), then a depth-aware blur.
    RGTexture aoTexture;
    const bool hasAO = prepass && m_settings.ao.enabled && m_gtaoPipeline && m_aoBlurPipeline;
    if (hasAO) {
        TextureDesc ad;
        ad.width = u32(m_aoParams.aoSize.x);
        ad.height = u32(m_aoParams.aoSize.y);
        ad.format = Format::R8_UNORM;
        ad.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess;
        ad.debugName = "GTAO.Raw";
        const RGTexture raw = graph.CreateTexture(ad);
        ad.debugName = "GTAO";
        aoTexture = graph.CreateTexture(ad);
        const RGTexture blurred = aoTexture;
        graph.AddPass(
            "GTAO", PassType::Compute,
            [depth, raw](PassBuilder& b) {
                b.ReadTexture(depth);
                b.WriteTexture(raw);
            },
            [this, depth, raw](PassContext& ctx) {
                AOParams p = m_aoParams;
                p.depth = ctx.Texture(depth)->GetSrvIndex();
                p.output = ctx.Texture(raw)->GetUavIndex();
                ctx.cmd.SetPipeline(m_gtaoPipeline);
                ctx.cmd.SetPushConstants(p);
                ctx.cmd.Dispatch((u32(p.aoSize.x) + 7) / 8, (u32(p.aoSize.y) + 7) / 8);
            });
        graph.AddPass(
            "GTAOBlur", PassType::Compute,
            [depth, raw, blurred](PassBuilder& b) {
                b.ReadTexture(depth);
                b.ReadTexture(raw);
                b.WriteTexture(blurred);
            },
            [this, depth, raw, blurred](PassContext& ctx) {
                AOParams p = m_aoParams;
                p.depth = ctx.Texture(depth)->GetSrvIndex();
                p.input = ctx.Texture(raw)->GetSrvIndex();
                p.output = ctx.Texture(blurred)->GetUavIndex();
                ctx.cmd.SetPipeline(m_aoBlurPipeline);
                ctx.cmd.SetPushConstants(p);
                ctx.cmd.Dispatch((u32(p.aoSize.x) + 7) / 8, (u32(p.aoSize.y) + 7) / 8);
            });
    }

    // Contact shadows of the sun (render resolution, jittered; TAA resolves the noise).
    RGTexture contactTexture;
    const bool hasContact = prepass && m_settings.contactShadows.enabled && m_contactPipeline && m_viewConstants.lightIntensity > 0.0f;
    if (hasContact) {
        TextureDesc cd;
        cd.width = m_renderSize.x;
        cd.height = m_renderSize.y;
        cd.format = Format::R8_UNORM;
        cd.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess;
        cd.debugName = "ContactShadows";
        contactTexture = graph.CreateTexture(cd);
        graph.AddPass(
            "ContactShadows", PassType::Compute,
            [depth, contactTexture](PassBuilder& b) {
                b.ReadTexture(depth);
                b.WriteTexture(contactTexture);
            },
            [this, depth, contactTexture](PassContext& ctx) {
                ContactParams p = m_contactParams;
                p.depth = ctx.Texture(depth)->GetSrvIndex();
                p.output = ctx.Texture(contactTexture)->GetUavIndex();
                ctx.cmd.SetPipeline(m_contactPipeline);
                ctx.cmd.SetPushConstants(p);
                ctx.cmd.Dispatch((m_renderSize.x + 7) / 8, (m_renderSize.y + 7) / 8);
            });
    }

    // Light clustering (compute).
    const RGBuffer clusters = graph.ImportBuffer(m_clusterBuffer, ResourceState::ShaderResource);
    const bool hasLights = m_viewConstants.lightCount > 0;
    if (hasLights) {
        graph.AddPass(
            "LightClusters", PassType::Compute, [clusters](PassBuilder& b) { b.WriteBuffer(clusters); },
            [this](PassContext& ctx) {
                ctx.cmd.SetPipeline(m_clusterPipeline);
                ctx.cmd.SetConstants(0, &m_clusterParams, sizeof(m_clusterParams));
                ctx.cmd.Dispatch((ClusterCount + 63) / 64);
            });
    }
    const u32 cascadeCount = m_cascadeCount;
    {
        LightingInputs& li = m_lightingInputs;
        li = LightingInputs{};
        li.lightBuffer = hasLights ? m_viewConstants.lightBuffer : InvalidDescriptor;
        li.clusterBuffer = hasLights ? m_viewConstants.clusterBuffer : InvalidDescriptor;
        li.lightCount = hasLights ? m_viewConstants.lightCount : 0u;
        li.clusterDepthScale = m_viewConstants.clusterDepthScale;
        li.clusterDepthBias = m_viewConstants.clusterDepthBias;
        li.clusterTileScale = m_viewConstants.clusterTileScale;
        li.clusters = clusters;
        li.cascadeCount = cascadeCount;
        li.cascadeSplits = m_viewConstants.cascadeSplits;
        for (u32 c = 0; c < cascadeCount; ++c) {
            li.shadowMatrices[c] = m_viewConstants.shadowMatrices[c];
            li.shadowMaps[c] = m_viewConstants.shadowMaps[c];
            li.shadowTextures[c] = shadowMaps[c];
        }
    }
    // Forward passes: view constants in b2, sky irradiance SH (GPU-written) bound directly as the b1 constant buffer.
    Buffer* skySH = m_skyIrradianceBuffer;
    const RGBuffer skySHHandle = skySH ? graph.ImportBuffer(skySH, ResourceState::ConstantBuffer) : RGBuffer();
    auto bindView = [this, skySH, aoTexture, hasAO, contactTexture, hasContact](PassContext& ctx) {
        m_viewConstants.aoTexture = hasAO ? ctx.Texture(aoTexture)->GetSrvIndex() : InvalidDescriptor;
        m_viewConstants.contactShadows = hasContact ? ctx.Texture(contactTexture)->GetSrvIndex() : InvalidDescriptor;
        ctx.cmd.SetConstants(1, &m_viewConstants, sizeof(m_viewConstants));
        if (skySH)
            ctx.cmd.SetConstantBuffer(0, skySH->GetGpuAddress());
    };
    const bool readLocal = m_localAtlas && hasLocalShadows;
    auto readShadows = [shadowMaps, cascadeCount, clusters, hasLights, skySHHandle, skySH, aoTexture, hasAO, localAtlas,
                        readLocal, contactTexture, hasContact](PassBuilder& b) {
        if (hasAO)
            b.ReadTexture(aoTexture);
        if (hasContact)
            b.ReadTexture(contactTexture);
        if (readLocal)
            b.ReadTexture(localAtlas);
        for (u32 c = 0; c < cascadeCount; ++c)
            b.ReadTexture(shadowMaps[c]);
        if (hasLights)
            b.ReadBuffer(clusters);
        if (skySH)
            b.ReadBuffer(skySHHandle, ResourceState::ConstantBuffer);
    };
    // Normal + roughness for screen-space reflections (cleared: a = 0 marks "no opaque surface").
    RGTexture normals;
    const bool deferred = m_settings.deferred && prepass && m_settings.debugMode == 0 && m_deferredPipeline;
    m_stats.deferred = deferred;
    const bool mrt = m_settings.normalOutput || deferred; // the deferred G-buffer's B target is that texture
    if (mrt) {
        TextureDesc nd;
        nd.width = m_renderSize.x;
        nd.height = m_renderSize.y;
        nd.format = Format::R10G10B10A2_UNORM;
        nd.usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource;
        nd.debugName = "NormalRoughness";
        normals = graph.CreateTexture(nd);
    }
    RGTexture gbufferA, gbufferC;
    if (deferred) {
        TextureDesc gd;
        gd.width = m_renderSize.x;
        gd.height = m_renderSize.y;
        gd.usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource;
        gd.format = Format::R8G8B8A8_SRGB;
        gd.debugName = "GBufferA";
        gbufferA = graph.CreateTexture(gd);
        gd.format = Format::R8G8B8A8_UNORM;
        gd.debugName = "GBufferC";
        gbufferC = graph.CreateTexture(gd);
        graph.AddPass(
            "GBuffer", PassType::Graphics,
            [color, depth, clearColor, normals, gbufferA, gbufferC, readIndirect, readShadows](PassBuilder& b) {
                b.RenderTarget(color, LoadOp::Clear, clearColor);
                b.RenderTarget(gbufferA, LoadOp::Clear, LinearColor(0, 0, 0, 0));
                b.RenderTarget(normals, LoadOp::Clear, LinearColor(0, 0, 0, 0));
                b.RenderTarget(gbufferC, LoadOp::Clear, LinearColor(0, 0, 0, 0));
                b.DepthStencil(depth, LoadOp::Load, 0.0f);
                readIndirect(b);
                readShadows(b); // bindView binds them (the G-buffer shader ignores them)
            },
            [this, bindView, gpu, occlusion](PassContext& ctx) {
                bindView(ctx);
                if (gpu) {
                    DrawIndirect(ctx.cmd, 0, VariantGBuffer);
                    if (occlusion)
                        DrawIndirect(ctx.cmd, 1, VariantGBuffer);
                    return;
                }
                Pipeline* bound = nullptr;
                for (const DrawItem& d : m_opaque)
                    Draw(ctx.cmd, d, VariantGBuffer, bound);
            });
        const UVec2 size = m_renderSize;
        graph.AddPass(
            "DeferredLighting", PassType::Graphics,
            [color, depth, normals, gbufferA, gbufferC, readShadows](PassBuilder& b) {
                b.RenderTarget(color, LoadOp::Load);
                b.ReadTexture(depth);
                b.ReadTexture(gbufferA);
                b.ReadTexture(normals);
                b.ReadTexture(gbufferC);
                readShadows(b);
            },
            [this, bindView, depth, normals, gbufferA, gbufferC, size](PassContext& ctx) {
                m_viewConstants.gbufferA = ctx.Texture(gbufferA)->GetSrvIndex();
                m_viewConstants.gbufferB = ctx.Texture(normals)->GetSrvIndex();
                m_viewConstants.gbufferC = ctx.Texture(gbufferC)->GetSrvIndex();
                m_viewConstants.sceneDepth = ctx.Texture(depth)->GetSrvIndex();
                bindView(ctx);
                ctx.cmd.SetViewport(Viewport{0, 0, f32(size.x), f32(size.y), 0, 1});
                ctx.cmd.SetScissor(0, 0, size.x, size.y);
                ctx.cmd.SetPipeline(m_deferredPipeline);
                ctx.cmd.Draw(3);
            });
    }
    if (!deferred) {
        graph.AddPass(
            "ForwardOpaque", PassType::Graphics,
            [color, depth, prepass, clearColor, readShadows, mrt, normals, readIndirect](PassBuilder& b) {
                b.RenderTarget(color, LoadOp::Clear, clearColor);
                if (mrt)
                    b.RenderTarget(normals, LoadOp::Clear, LinearColor(0, 0, 0, 0));
                b.DepthStencil(depth, prepass ? LoadOp::Load : LoadOp::Clear, 0.0f);
                readShadows(b);
                readIndirect(b);
            },
            [this, prepass, bindView, mrt, gpu, occlusion](PassContext& ctx) {
                bindView(ctx);
                const PipelineVariant variant = prepass ? (mrt ? VariantMainNoWriteMRT : VariantMainNoWrite) : (mrt ? VariantMainMRT : VariantMain);
                if (gpu) {
                    DrawIndirect(ctx.cmd, 0, variant);
                    if (occlusion)
                        DrawIndirect(ctx.cmd, 1, variant);
                    return;
                }
                Pipeline* bound = nullptr;
                for (const DrawItem& d : m_opaque)
                    Draw(ctx.cmd, d, variant, bound);
            });
    }
    // Velocity of the objects that moved (motion blur, TAA); everything else keeps the "use the camera" sentinel.
    m_velocityTexture = RGTexture();
    if (m_motion && !m_velocityDraws.Empty() && m_velocityPipeline) {
        TextureDesc vd;
        vd.width = m_renderSize.x;
        vd.height = m_renderSize.y;
        vd.format = Format::R16G16_FLOAT;
        vd.usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource;
        vd.debugName = "Velocity";
        const RGTexture velocity = graph.CreateTexture(vd);
        m_velocityTexture = velocity;
        struct VelocityParams
        {
            Mat4 viewProj, viewProjNoJitter, prevViewProj;
            u32 instanceBuffer, instanceStride, pad[2];
        } vp = {m_viewConstants.viewProj, m_viewProjNoJitter, m_prevViewProjNoJitter, m_viewConstants.instanceBuffer, InstanceStride, {}};
        const UVec2 size = m_renderSize;
        graph.AddPass(
            "Velocity", PassType::Graphics,
            [velocity, depth](PassBuilder& b) {
                b.RenderTarget(velocity, LoadOp::Clear, LinearColor(-1000.0f, -1000.0f, 0, 0));
                b.DepthStencil(depth, LoadOp::Load, 0.0f, true);
            },
            [this, vp, size](PassContext& ctx) {
                ctx.cmd.SetViewport(Viewport{0, 0, f32(size.x), f32(size.y), 0, 1});
                ctx.cmd.SetScissor(0, 0, size.x, size.y);
                ctx.cmd.SetPipeline(m_velocityPipeline);
                ctx.cmd.SetConstants(0, &vp, sizeof(vp));
                for (const DrawItem& d : m_velocityDraws) {
                    ctx.cmd.SetIndexBuffer(d.mesh->indexBuffer, d.mesh->index16 ? IndexFormat::UInt16 : IndexFormat::UInt32);
                    const DrawPush push{d.instance, d.material, d.mesh->vertexBuffer->GetSrvIndex(), d.lod};
                    ctx.cmd.SetPushConstants(push);
                    ctx.cmd.DrawIndexed(d.indexCount, 1, d.indexOffset, 0, 0);
                }
            });
    }
    if (gpu) {
        // The pyramid built this frame tests the next frame's phase 1.
        m_hizViewProj = m_cullViewProj;
        m_hizValid = occlusion;
        if (occlusion)
            m_hizCurrent ^= 1u;
    }
    if (afterOpaque)
        afterOpaque(graph, color, depth, normals);
    if (!m_translucent.Empty()) {
        graph.AddPass(
            "ForwardTranslucent", PassType::Graphics,
            [color, depth, readShadows](PassBuilder& b) {
                b.RenderTarget(color, LoadOp::Load);
                b.DepthStencil(depth, LoadOp::Load, 0.0f, true);
                readShadows(b);
            },
            [this, bindView](PassContext& ctx) {
                bindView(ctx);
                Pipeline* bound = nullptr;
                for (const DrawItem& d : m_translucent)
                    Draw(ctx.cmd, d, VariantMainNoWrite, bound);
            });
    }
}

} // namespace zx::render
