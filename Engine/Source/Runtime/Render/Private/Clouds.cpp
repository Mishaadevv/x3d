#include "Render/Clouds.h"
#include "Core/Log/Log.h"
#include "Render/Sky.h"
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
    u32 weatherMap;
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
    f32 cloudType;
    f32 scale;
    f32 evolution;
    f32 time;
    u32 steps;
    u32 lightSteps;
    f32 ambient;
    f32 silverLining;
    Vec3 lightningPos;
    f32 lightning;
    u32 skyView;
    Vec3 skyScale;
    f32 hazeDistance;
    f32 weatherScale;
    Vec2 weatherOffset;
    u32 invalidDepth;
    f32 maxDistance;
    Vec2 subTexel;
    Vec2 pad3;
    u32 genOutput;
    u32 genSize;
    u32 genSliceBegin;
    u32 genSeed;
    u32 genSource;
    u32 genSourceMip;
    u32 pad1, pad2;
};

u32 HashU32(u32 x)
{
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

f32 Hash01(u32 x) { return f32(HashU32(x) >> 8) / 16777216.0f; }

} // namespace

CloudRenderer::CloudRenderer(Device& device) : m_device(device) {}

CloudRenderer::~CloudRenderer()
{
    for (Pipeline* p : {m_cloudsPipeline, m_temporalPipeline, m_shadowPipeline, m_compositePipeline, m_shapePipeline, m_detailPipeline,
                        m_weatherPipeline, m_mipPipeline})
        m_device.Destroy(p);
    for (Texture* t : {m_shapeNoise, m_detailNoise, m_weather, m_raw, m_resolved[0], m_resolved[1], m_shadow})
        m_device.Destroy(t);
}

bool CloudRenderer::Init(Format colorFormat, bool lite)
{
    m_lite = lite;
    struct Job
    {
        ShaderBytecode* out;
        StringView entry;
        ShaderStage stage;
    } shaderJobs[] = {{&m_cloudsCS, "CloudsCS", ShaderStage::Compute},         {&m_temporalCS, "TemporalCS", ShaderStage::Compute},
                      {&m_shadowCS, "ShadowCS", ShaderStage::Compute},         {&m_compositeVS, "CompositeVS", ShaderStage::Vertex},
                      {&m_compositePS, "CompositePS", ShaderStage::Pixel},     {&m_shapeCS, "ShapeNoiseCS", ShaderStage::Compute},
                      {&m_detailCS, "DetailNoiseCS", ShaderStage::Compute},    {&m_weatherCS, "WeatherCS", ShaderStage::Compute},
                      {&m_mipCS, "NoiseMipCS", ShaderStage::Compute}};
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
    m_shapePipeline = compute(m_shapeCS, "CloudShapeNoise");
    m_detailPipeline = compute(m_detailCS, "CloudDetailNoise");
    m_weatherPipeline = compute(m_weatherCS, "CloudWeather");
    m_mipPipeline = compute(m_mipCS, "CloudNoiseMip");
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

    TextureDesc nd;
    nd.dimension = TextureDimension::Texture3D;
    nd.format = Format::R8G8B8A8_UNORM;
    nd.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess;
    nd.width = nd.height = nd.depthOrArraySize = ShapeSize;
    nd.mipLevels = ShapeMips;
    nd.debugName = "CloudShapeNoise";
    m_shapeNoise = m_device.CreateTexture(nd);
    nd.width = nd.height = nd.depthOrArraySize = DetailSize;
    nd.mipLevels = DetailMips;
    nd.debugName = "CloudDetailNoise";
    m_detailNoise = m_device.CreateTexture(nd);
    TextureDesc wd;
    wd.width = wd.height = WeatherSize;
    wd.format = Format::R8G8B8A8_UNORM;
    wd.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess;
    wd.debugName = "CloudWeather";
    m_weather = m_device.CreateTexture(wd);
    TextureDesc sd;
    sd.width = sd.height = ShadowSize;
    sd.format = Format::R16G16B16A16_FLOAT;
    sd.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess;
    sd.debugName = "CloudShadows";
    m_shadow = m_device.CreateTexture(sd);
    return m_cloudsPipeline && m_temporalPipeline && m_shadowPipeline && m_compositePipeline && m_shapePipeline && m_detailPipeline &&
           m_weatherPipeline && m_mipPipeline && m_shapeNoise && m_detailNoise && m_weather && m_shadow;
}

void CloudRenderer::AddMips(CommandList& cmd, Texture* texture, u32 size, u32 mips)
{
    cmd.SetPipeline(m_mipPipeline);
    for (u32 mip = 1; mip < mips; ++mip) {
        cmd.Barrier(texture, ResourceState::UnorderedAccess);
        CloudParams g = {};
        g.genOutput = texture->GetUavIndex(mip);
        g.genSource = texture->GetUavIndex(mip - 1);
        g.genSize = size >> mip;
        cmd.SetConstants(0, &g, sizeof(g));
        cmd.Dispatch(Max(g.genSize / 4, 1u), Max(g.genSize / 4, 1u), Max(g.genSize / 4, 1u));
    }
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
                              const SceneLighting& lighting, const CloudSettings& settings, const SkyRenderer* sky, f32 exposure, f32 time)
{
    m_flash = 0.0f;
    if (!settings.enabled || !m_compositePipeline || settings.coverage <= 0.0f) {
        m_historyValid = false;
        m_shadowValid = false;
        return;
    }
    const RGTexture shapeRG = graph.ImportTexture(m_shapeNoise, ResourceState::ShaderResource);
    const RGTexture detailRG = graph.ImportTexture(m_detailNoise, ResourceState::ShaderResource);
    const RGTexture weatherRG = graph.ImportTexture(m_weather, ResourceState::ShaderResource);
    m_rgShape = shapeRG;
    m_rgWeather = weatherRG;

    CloudParams p = {};
    p.coverage = Clamp(settings.coverage, 0.0f, 1.0f);
    p.cloudType = Clamp(settings.cloudType, 0.0f, 1.0f);

    // Generation: the noise volumes in slices over the first frames (a long dispatch can trip the GPU watchdog on
    // integrated GPUs), the weather map whenever coverage / type / seed change.
    ++m_genFrame;
    if (!m_detailReady) {
        m_detailReady = true;
        CloudParams g = {};
        g.genOutput = m_detailNoise->GetUavIndex();
        g.genSize = DetailSize;
        graph.AddPass(
            "CloudDetailNoise", PassType::Compute, [=](PassBuilder& b) { b.WriteTexture(detailRG); },
            [=, this](PassContext& ctx) {
                ctx.cmd.SetPipeline(m_detailPipeline);
                ctx.cmd.SetConstants(0, &g, sizeof(g));
                ctx.cmd.Dispatch(DetailSize / 4, DetailSize / 4, DetailSize / 4);
                AddMips(ctx.cmd, m_detailNoise, DetailSize, DetailMips);
            });
    }
    if (m_shapeSlices < ShapeSize && m_genFrame > 2) {
        const u32 slices = Min(4u, ShapeSize - m_shapeSlices);
        CloudParams g = {};
        g.genOutput = m_shapeNoise->GetUavIndex();
        g.genSize = ShapeSize;
        g.genSliceBegin = m_shapeSlices;
        m_shapeSlices += slices;
        const bool last = m_shapeSlices >= ShapeSize;
        graph.AddPass(
            "CloudShapeNoise", PassType::Compute, [=](PassBuilder& b) { b.WriteTexture(shapeRG); },
            [=, this](PassContext& ctx) {
                ctx.cmd.SetPipeline(m_shapePipeline);
                ctx.cmd.SetConstants(0, &g, sizeof(g));
                ctx.cmd.Dispatch(ShapeSize / 4, ShapeSize / 4, (slices + 3) / 4);
                if (last)
                    AddMips(ctx.cmd, m_shapeNoise, ShapeSize, ShapeMips);
            });
    }
    const u64 weatherKey = u64(p.coverage * 1000.0f) | (u64(p.cloudType * 1000.0f) << 16) | (u64(settings.seed) << 32);
    if (weatherKey != m_weatherKey && m_genFrame > 1) {
        m_weatherKey = weatherKey;
        CloudParams g = p;
        g.genOutput = m_weather->GetUavIndex();
        g.genSize = WeatherSize;
        g.genSeed = settings.seed * 1000u;
        graph.AddPass(
            "CloudWeather", PassType::Compute, [=](PassBuilder& b) { b.WriteTexture(weatherRG); },
            [=, this](PassContext& ctx) {
                ctx.cmd.SetPipeline(m_weatherPipeline);
                ctx.cmd.SetConstants(0, &g, sizeof(g));
                ctx.cmd.Dispatch(WeatherSize / 8, WeatherSize / 8, 1);
            });
    }
    if (!IsReady()) {
        m_historyValid = false;
        m_shadowValid = false;
        return;
    }

    // Lite: quarter resolution; Full: half (the temporal blend fills in the detail).
    const u32 div = m_lite ? 4u : 2u;
    EnsureTargets(UVec2(Max(view.width / div, 1u), Max(view.height / div, 1u)));
    if (!m_raw || !m_resolved[0] || !m_resolved[1])
        return;
    ++m_frame;

    p.invViewProj = Inverse(viewProjNoJitter);
    p.prevViewProj = m_prevViewProj;
    m_prevViewProj = viewProjNoJitter;
    p.cameraPosition = view.position;
    p.bottom = Max(settings.bottom, 0.0f);
    p.thickness = Max(settings.thickness, 50.0f);
    p.dirToSun = -lighting.sunDirection;
    p.sunColor = lighting.sunColor * (lighting.sunIlluminance * exposure);
    p.skyColor = lighting.skyColor * (lighting.skyLuminance * exposure);
    p.density = Max(settings.density, 0.0f);
    p.albedo = settings.albedo;
    p.detail = Clamp(settings.detail, 0.0f, 1.0f);
    p.scale = Clamp(settings.scale, 0.2f, 5.0f);
    p.evolution = Max(settings.evolution, 0.0f);
    p.time = time;
    p.steps = m_lite ? 32u : 64u;
    p.lightSteps = m_lite ? 3u : 6u;
    p.maxDistance = m_lite ? 35000.0f : 60000.0f;
    p.ambient = Max(settings.ambient, 0.0f);
    p.silverLining = Max(settings.silverLining, 0.0f);
    const f32 windAngle = DegToRad(settings.windDirection);
    // Drift so far: wraps every tile of the weather map so the coordinates keep their precision.
    const f32 drift = std::fmod(settings.windSpeed * time, WeatherExtent * 3.0f);
    p.wind = Vec2(-Sin(windAngle), -Cos(windAngle)) * drift;
    p.outputSize = Vec2(f32(m_size.x), f32(m_size.y));
    p.shapeNoise = m_shapeNoise->GetSrvIndex();
    p.detailNoise = m_detailNoise->GetSrvIndex();
    p.weatherMap = m_weather->GetSrvIndex();
    p.frame = m_frame;
    // Halton(2, 3) positions inside the texel, 8 frames.
    auto halton = [](u32 i, u32 b) {
        f32 f = 1.0f, r = 0.0f;
        for (; i > 0; i /= b) {
            f /= f32(b);
            r += f * f32(i % b);
        }
        return r;
    };
    p.subTexel = Vec2(halton(m_frame % 8 + 1, 2), halton(m_frame % 8 + 1, 3));
    p.shadowStrength = Clamp(settings.shadowStrength, 0.0f, 1.0f);
    p.shadowSize = ShadowSize;
    p.shadowExtent = ShadowExtent;
    p.weatherScale = 1.0f / (WeatherExtent * p.scale);
    p.weatherOffset = Vec2(0.37f, 0.61f); // the camera doesn't start right at the tile's seam
    p.hazeDistance = 45000.0f;
    p.skyView = InvalidDescriptor;
    const RGTexture skyView = sky ? sky->SkyViewTexture() : RGTexture{};
    if (skyView.IsValid())
        p.skyScale = sky->SkyRadianceScale();
    // Lightning: random strikes in 0.25 s slots, a bright flicker deep in the clouds near the camera.
    if (settings.lightning > 0.0f) {
        const u32 slot = u32(Max(time, 0.0f) * 4.0f);
        if (Hash01(slot * 7919u + settings.seed) < settings.lightning * 0.07f) {
            const f32 age = time * 4.0f - f32(slot);
            const f32 flicker = 0.6f + 0.4f * Sin(age * 60.0f);
            m_flash = std::exp(-age * 5.0f) * flicker;
            const f32 a = Hash01(slot * 31u + 5u) * 2.0f * Pi, r = 1500.0f + Hash01(slot * 17u + 3u) * 7000.0f;
            p.lightningPos = view.position + Vec3(Cos(a) * r, 0.0f, Sin(a) * r);
            p.lightningPos.y = p.bottom + p.thickness * (0.15f + 0.3f * Hash01(slot * 13u + 1u));
            p.lightning = m_flash * lighting.sunIlluminance * exposure * 0.08f + 30.0f * exposure * m_flash;
        }
    }
    // The shadow map follows the camera in whole texels (no swimming).
    const f32 texel = ShadowExtent / f32(ShadowSize);
    m_shadowOrigin = Vec2(Floor(view.position.x / texel) * texel - ShadowExtent * 0.5f, Floor(view.position.z / texel) * texel - ShadowExtent * 0.5f);
    p.shadowOrigin = m_shadowOrigin;

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
    p.historyWeight = m_historyValid ? 1.0f : 0.0f;
    p.shadowOutput = m_shadow->GetUavIndex();
    m_historyValid = true;
    const UVec2 half = m_size;

    graph.AddPass(
        "Clouds", PassType::Compute,
        [=](PassBuilder& b) {
            b.ReadTexture(shapeRG);
            b.ReadTexture(detailRG);
            b.ReadTexture(weatherRG);
            b.ReadTexture(depth);
            if (skyView.IsValid())
                b.ReadTexture(skyView);
            b.WriteTexture(rawRG);
        },
        [=, this](PassContext& ctx) {
            CloudParams params = p;
            params.depthTexture = ctx.Texture(depth)->GetSrvIndex();
            if (skyView.IsValid())
                params.skyView = ctx.Texture(skyView)->GetSrvIndex();
            ctx.cmd.SetPipeline(m_cloudsPipeline);
            ctx.cmd.SetConstants(0, &params, sizeof(params));
            ctx.cmd.Dispatch((half.x / 2 + 8) / 8, (half.y / 2 + 8) / 8, 1); // one pixel of each 2x2 block
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
    CloudParams sp = p;
    sp.invalidDepth = 1;
    MemCopy(m_shadowParams, &sp, sizeof(sp));
    m_shadowPending = true;
}

void CloudRenderer::AddShadowPass(RenderGraph& graph)
{
    if (!m_shadowPending)
        return;
    m_shadowPending = false;
    CloudParams p;
    MemCopy(&p, m_shadowParams, sizeof(p));
    const RGTexture shapeRG = m_rgShape, weatherRG = m_rgWeather;
    const RGTexture shadowRG = graph.ImportTexture(m_shadow, ResourceState::ShaderResource);
    graph.AddPass(
        "CloudShadows", PassType::Compute,
        [=](PassBuilder& b) {
            b.ReadTexture(shapeRG);
            b.ReadTexture(weatherRG);
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
