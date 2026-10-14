#include "Render/Clouds.h"
#include "Core/Log/Log.h"
#include "Core/Jobs/JobSystem.h"
#include "RHI/ShaderCompiler.h"

#include <cmath>

namespace zx::render {

using namespace rhi;

ZX_DEFINE_LOG_CATEGORY_STATIC(LogClouds, Info);

namespace {

// Keep in sync with CloudParams (Shaders/Sky/Clouds.hlsl).
struct CloudParams
{
    Mat4 invViewProj;
    Mat4 prevViewProj;
    Vec3 cameraPosition;
    f32 bottom;
    Vec3 dirToSun;
    f32 thickness;
    Vec3 sunColor;
    f32 coverage;
    Vec3 skyColor;
    f32 density;
    Vec3 albedo;
    f32 detail;
    Vec2 wind;
    Vec2 outputSize;
    u32 shapeNoise;
    u32 detailNoise;
    u32 output;
    u32 history;
    u32 current;
    u32 resolved;
    u32 depthTexture;
    u32 frame;
    f32 historyWeight;
    f32 shadowStrength;
    u32 shadowOutput;
    u32 shadowSize;
    Vec2 shadowOrigin;
    f32 shadowExtent;
    f32 pad;
};

// --- Tiling noise volumes (computed once on the CPU: a long first-frame dispatch can trip the GPU watchdog on iGPUs).

Vec3 Hash33(Vec3 p)
{
    const Vec3 d(Dot(p, Vec3(127.1f, 311.7f, 74.7f)), Dot(p, Vec3(269.5f, 183.3f, 246.1f)), Dot(p, Vec3(113.5f, 271.9f, 124.6f)));
    auto fr = [](f32 x) { return x - std::floor(x); };
    return Vec3(fr(std::sin(d.x) * 43758.5453f), fr(std::sin(d.y) * 43758.5453f), fr(std::sin(d.z) * 43758.5453f));
}

f32 Wrap(f32 v, f32 period) { return v - period * std::floor(v / period); }

/// Tiling Worley (cellular) noise: 1 at the cell points, 0 far from them.
f32 Worley(Vec3 p, f32 period)
{
    const Vec3 cell(std::floor(p.x), std::floor(p.y), std::floor(p.z));
    const Vec3 f = p - cell;
    f32 best = 1.0f;
    for (i32 z = -1; z <= 1; ++z)
        for (i32 y = -1; y <= 1; ++y)
            for (i32 x = -1; x <= 1; ++x) {
                const Vec3 o{f32(x), f32(y), f32(z)};
                const Vec3 c(Wrap(cell.x + o.x, period), Wrap(cell.y + o.y, period), Wrap(cell.z + o.z, period));
                const Vec3 d = o + Hash33(c) - f;
                best = Min(best, Dot(d, d));
            }
    return 1.0f - Saturate(std::sqrt(best));
}

f32 GradientNoise(Vec3 p, f32 period)
{
    const Vec3 i(std::floor(p.x), std::floor(p.y), std::floor(p.z));
    const Vec3 f = p - i;
    auto fade = [](f32 t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); };
    const Vec3 u(fade(f.x), fade(f.y), fade(f.z));
    f32 n[8];
    for (u32 k = 0; k < 8; ++k) {
        const Vec3 o(f32(k & 1), f32((k >> 1) & 1), f32((k >> 2) & 1));
        const Vec3 g = Hash33(Vec3(Wrap(i.x + o.x, period), Wrap(i.y + o.y, period), Wrap(i.z + o.z, period))) * 2.0f - Vec3(1.0f);
        n[k] = Dot(g, f - o);
    }
    const f32 x0 = Lerp(Lerp(n[0], n[1], u.x), Lerp(n[2], n[3], u.x), u.y), x1 = Lerp(Lerp(n[4], n[5], u.x), Lerp(n[6], n[7], u.x), u.y);
    return Lerp(x0, x1, u.z) * 0.5f + 0.5f;
}

/// Shape (billowy Perlin-Worley, Schneider 2015) or detail (Worley fbm) noise, size^3 bytes.
Array<u8> MakeNoise(u32 size, bool shape)
{
    Array<u8> out;
    out.Resize(usize(size) * size * size);
    jobs::ParallelFor(size, 1, [&](u32 begin, u32 end) {
        for (u32 z = begin; z < end; ++z)
            for (u32 y = 0; y < size; ++y)
                for (u32 x = 0; x < size; ++x) {
                    const Vec3 p = (Vec3(f32(x), f32(y), f32(z)) + Vec3(0.5f)) / f32(size);
                    const f32 worley = Worley(p * 4.0f, 4.0f) * 0.625f + Worley(p * 8.0f, 8.0f) * 0.25f + Worley(p * 16.0f, 16.0f) * 0.125f;
                    f32 v = worley;
                    if (shape) {
                        const f32 perlin = GradientNoise(p * 4.0f, 4.0f) * 0.625f + GradientNoise(p * 8.0f, 8.0f) * 0.25f +
                                           GradientNoise(p * 16.0f, 16.0f) * 0.125f;
                        v = Saturate((perlin - (worley - 1.0f)) / Max(1.0f - (worley - 1.0f), 1e-4f)); // remap(perlin, worley - 1, 1)
                    }
                    out[(usize(z) * size + y) * size + x] = u8(Saturate(v) * 255.0f + 0.5f);
                }
    });
    return out;
}

} // namespace

CloudRenderer::CloudRenderer(Device& device) : m_device(device) {}

CloudRenderer::~CloudRenderer()
{
    for (Pipeline* p : {m_cloudsPipeline, m_temporalPipeline, m_shadowPipeline, m_compositePipeline})
        m_device.Destroy(p);
    for (Texture* t : {m_shapeNoise, m_detailNoise, m_raw, m_resolved[0], m_resolved[1], m_shadow})
        m_device.Destroy(t);
}

bool CloudRenderer::Init(Format colorFormat)
{
    struct Job
    {
        ShaderBytecode* out;
        StringView entry;
        ShaderStage stage;
    } shaderJobs[] = {{&m_cloudsCS, "CloudsCS", ShaderStage::Compute},       {&m_temporalCS, "TemporalCS", ShaderStage::Compute},
                      {&m_shadowCS, "ShadowCS", ShaderStage::Compute},       {&m_compositeVS, "CompositeVS", ShaderStage::Vertex},
                      {&m_compositePS, "CompositePS", ShaderStage::Pixel}};
    for (const Job& j : shaderJobs) {
        ShaderCompileDesc d;
        d.path = "/Shaders/Sky/Clouds.hlsl";
        d.entryPoint = j.entry;
        d.stage = j.stage;
        Result<ShaderBytecode> r = ShaderCompiler::Compile(d);
        if (!r) {
            ZX_LOG_ERROR(LogClouds, "{}", r.GetError().message);
            return false;
        }
        *j.out = move(*r);
    }
    auto compute = [&](ShaderBytecode& cs, StringView name) {
        ComputePipelineDesc cd;
        cd.computeShader = &cs;
        cd.debugName = name;
        return m_device.CreateComputePipeline(cd);
    };
    m_cloudsPipeline = compute(m_cloudsCS, "Clouds");
    m_temporalPipeline = compute(m_temporalCS, "CloudsTemporal");
    m_shadowPipeline = compute(m_shadowCS, "CloudShadows");
    GraphicsPipelineDesc pd;
    pd.vertexShader = &m_compositeVS;
    pd.pixelShader = &m_compositePS;
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
    pd.debugName = "CloudsComposite";
    m_compositePipeline = m_device.CreateGraphicsPipeline(pd);

    auto noiseVolume = [&](u32 size, bool shape, StringView name) {
        const Array<u8> voxels = MakeNoise(size, shape);
        TextureDesc nd;
        nd.dimension = TextureDimension::Texture3D;
        nd.format = Format::R8_UNORM;
        nd.usage = TextureUsage::ShaderResource;
        nd.width = nd.height = nd.depthOrArraySize = size;
        nd.debugName = name;
        SubresourceData data;
        data.data = voxels.Data();
        data.rowPitch = size;
        data.slicePitch = u64(size) * size;
        return m_device.CreateTexture(nd, Span<const SubresourceData>(&data, 1));
    };
    m_shapeNoise = noiseVolume(64, true, "CloudShapeNoise");
    m_detailNoise = noiseVolume(32, false, "CloudDetailNoise");
    TextureDesc sd;
    sd.width = sd.height = ShadowSize;
    sd.format = Format::R16G16B16A16_FLOAT;
    sd.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess;
    sd.debugName = "CloudShadows";
    m_shadow = m_device.CreateTexture(sd);
    return m_cloudsPipeline && m_temporalPipeline && m_shadowPipeline && m_compositePipeline && m_shapeNoise &&
           m_detailNoise && m_shadow;
}

void CloudRenderer::EnsureTargets(UVec2 size)
{
    if (size == m_size && m_raw)
        return;
    m_device.Destroy(m_raw);
    m_device.Destroy(m_resolved[0]);
    m_device.Destroy(m_resolved[1]);
    TextureDesc d;
    d.width = size.x;
    d.height = size.y;
    d.format = Format::R16G16B16A16_FLOAT;
    d.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess;
    d.debugName = "CloudsRaw";
    m_raw = m_device.CreateTexture(d);
    d.debugName = "CloudsResolved";
    m_resolved[0] = m_device.CreateTexture(d);
    m_resolved[1] = m_device.CreateTexture(d);
    m_size = size;
    m_historyValid = false;
}

void CloudRenderer::AddPasses(RenderGraph& graph, RGTexture color, RGTexture depth, const RenderView& view, const Mat4& viewProjNoJitter,
                              const SceneLighting& lighting, const CloudSettings& settings, f32 exposure, f32 time)
{
    if (!settings.enabled || !m_compositePipeline || settings.coverage <= 0.0f) {
        m_historyValid = false;
        m_shadowValid = false;
        return;
    }
    // Quarter resolution: soft clouds upsample well, and the temporal blend fills in the detail.
    EnsureTargets(UVec2(Max(view.width / 4, 1u), Max(view.height / 4, 1u)));
    if (!m_raw || !m_resolved[0] || !m_resolved[1])
        return;
    ++m_frame;

    CloudParams p = {};
    p.invViewProj = Inverse(viewProjNoJitter);
    p.prevViewProj = m_prevViewProj;
    m_prevViewProj = viewProjNoJitter;
    p.cameraPosition = view.position;
    p.bottom = settings.bottom;
    p.thickness = Max(settings.thickness, 10.0f);
    p.dirToSun = -lighting.sunDirection;
    p.sunColor = lighting.sunColor * (lighting.sunIlluminance * exposure);
    p.coverage = Clamp(settings.coverage, 0.0f, 1.0f);
    p.skyColor = lighting.skyColor * (lighting.skyLuminance * exposure);
    p.density = Max(settings.density, 0.0f);
    p.albedo = settings.albedo;
    p.detail = Clamp(settings.detail, 0.0f, 1.0f);
    const f32 windAngle = DegToRad(settings.windDirection);
    // Drift so far: wraps every 60 km so the noise coordinates keep their precision.
    const f32 drift = std::fmod(settings.windSpeed * time, 60000.0f);
    p.wind = Vec2(-Sin(windAngle), -Cos(windAngle)) * drift;
    p.outputSize = Vec2(f32(m_size.x), f32(m_size.y));
    p.shapeNoise = m_shapeNoise->GetSrvIndex();
    p.detailNoise = m_detailNoise->GetSrvIndex();
    p.frame = m_frame;
    p.shadowStrength = Clamp(settings.shadowStrength, 0.0f, 1.0f);
    p.shadowSize = ShadowSize;
    p.shadowExtent = ShadowExtent;
    // The shadow map follows the camera in whole texels (no swimming).
    const f32 texel = ShadowExtent / f32(ShadowSize);
    m_shadowOrigin = Vec2(Floor(view.position.x / texel) * texel - ShadowExtent * 0.5f, Floor(view.position.z / texel) * texel - ShadowExtent * 0.5f);
    p.shadowOrigin = m_shadowOrigin;

    const RGTexture shapeRG = graph.ImportTexture(m_shapeNoise, ResourceState::ShaderResource);
    const RGTexture detailRG = graph.ImportTexture(m_detailNoise, ResourceState::ShaderResource);
    m_rgShape = shapeRG;
    const RGTexture rawRG = graph.ImportTexture(m_raw, ResourceState::ShaderResource);
    Texture* history = m_resolved[m_current];
    Texture* resolved = m_resolved[m_current ^ 1];
    m_current ^= 1;
    const RGTexture historyRG = graph.ImportTexture(history, ResourceState::ShaderResource);
    const RGTexture resolvedRG = graph.ImportTexture(resolved, ResourceState::ShaderResource);
    p.output = m_raw->GetUavIndex();
    p.current = m_raw->GetSrvIndex();
    p.history = history->GetSrvIndex();
    p.resolved = resolved->GetUavIndex();
    p.historyWeight = m_historyValid ? 0.85f : 0.0f;
    p.shadowOutput = m_shadow->GetUavIndex();
    m_historyValid = true;
    const UVec2 half = m_size;

    graph.AddPass(
        "Clouds", PassType::Compute,
        [=](PassBuilder& b) {
            b.ReadTexture(shapeRG);
            b.ReadTexture(detailRG);
            b.WriteTexture(rawRG);
        },
        [=, this](PassContext& ctx) {
            ctx.cmd.SetPipeline(m_cloudsPipeline);
            ctx.cmd.SetConstants(0, &p, sizeof(p));
            ctx.cmd.Dispatch((half.x + 7) / 8, (half.y + 7) / 8, 1);
        });
    graph.AddPass(
        "CloudsTemporal", PassType::Compute,
        [=](PassBuilder& b) {
            b.ReadTexture(rawRG);
            b.ReadTexture(historyRG);
            b.WriteTexture(resolvedRG);
        },
        [=, this](PassContext& ctx) {
            ctx.cmd.SetPipeline(m_temporalPipeline);
            ctx.cmd.SetConstants(0, &p, sizeof(p));
            ctx.cmd.Dispatch((half.x + 7) / 8, (half.y + 7) / 8, 1);
        });
    const UVec2 size(view.width, view.height);
    CloudParams cp = p;
    cp.history = resolved->GetSrvIndex(); // the composite reads this frame's blended result
    graph.AddPass(
        "CloudsComposite", PassType::Graphics,
        [=](PassBuilder& b) {
            b.ReadTexture(resolvedRG);
            b.ReadTexture(depth);
            b.RenderTarget(color, LoadOp::Load);
        },
        [=, this](PassContext& ctx) {
            CloudParams params = cp;
            params.depthTexture = ctx.Texture(depth)->GetSrvIndex();
            ctx.cmd.SetViewport(Viewport{0, 0, f32(size.x), f32(size.y), 0, 1});
            ctx.cmd.SetScissor(0, 0, size.x, size.y);
            ctx.cmd.SetPipeline(m_compositePipeline);
            ctx.cmd.SetConstants(0, &params, sizeof(params));
            ctx.cmd.Draw(3);
        });
    static_assert(sizeof(CloudParams) <= sizeof(m_shadowParams));
    MemCopy(m_shadowParams, &p, sizeof(p));
    m_shadowPending = true;
}

void CloudRenderer::AddShadowPass(RenderGraph& graph)
{
    if (!m_shadowPending)
        return;
    m_shadowPending = false;
    CloudParams p;
    MemCopy(&p, m_shadowParams, sizeof(p));
    const RGTexture shapeRG = m_rgShape;
    const RGTexture shadowRG = graph.ImportTexture(m_shadow, ResourceState::ShaderResource);
    graph.AddPass(
        "CloudShadows", PassType::Compute,
        [=](PassBuilder& b) {
            b.ReadTexture(shapeRG);
            b.WriteTexture(shadowRG);
            b.SideEffects();
        },
        [=, this](PassContext& ctx) {
            ctx.cmd.SetPipeline(m_shadowPipeline);
            ctx.cmd.SetConstants(0, &p, sizeof(p));
            ctx.cmd.Dispatch(ShadowSize / 8, ShadowSize / 8, 1);
        });
    m_shadowValid = true;
}

} // namespace zx::render
