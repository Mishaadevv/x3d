#include "Render/Sky.h"
#include "Core/Log/Log.h"
#include "RHI/ShaderCompiler.h"

#include <cmath>

ZX_DEFINE_LOG_CATEGORY_STATIC(LogSky, Info);

namespace zx::render {

using namespace rhi;

namespace atmosphere {

namespace {
// Keep in sync with Shaders/Sky/Atmosphere.hlsli (km).
constexpr f32 Bottom = 6360.0f, Top = 6460.0f;
const Vec3 RayleighScattering = Vec3(5.802f, 13.558f, 33.1f) * 1e-3f;
constexpr f32 RayleighScale = 8.0f;
constexpr f32 MieExtinction = 4.440e-3f, MieScale = 1.2f;
const Vec3 OzoneAbsorption = Vec3(0.650f, 1.881f, 0.085f) * 1e-3f;

Vec3 Extinction(f32 h)
{
    const f32 ozone = Max(0.0f, 1.0f - Abs(h - 25.0f) / 15.0f);
    return RayleighScattering * std::exp(-h / RayleighScale) + Vec3(MieExtinction * std::exp(-h / MieScale)) + OzoneAbsorption * ozone;
}

f32 RaySphere(Vec3 o, Vec3 d, f32 radius)
{
    const f32 b = Dot(o, d), c = Dot(o, o) - radius * radius, disc = b * b - c;
    if (disc < 0.0f)
        return -1.0f;
    const f32 s = std::sqrt(disc), t0 = -b - s, t1 = -b + s;
    return t0 >= 0.0f ? t0 : (t1 >= 0.0f ? t1 : -1.0f);
}
} // namespace

Vec3 Transmittance(Vec3 dirToSun, f32 altitudeKm)
{
    const Vec3 origin(0.0f, Bottom + Max(altitudeKm, 0.0f) + 1e-3f, 0.0f);
    const Vec3 dir = Normalize(dirToSun);
    if (RaySphere(origin, dir, Bottom) > 0.0f)
        return Vec3(0, 0, 0); // below the horizon
    const f32 tMax = RaySphere(origin, dir, Top);
    constexpr u32 Steps = 64;
    const f32 dt = tMax / Steps;
    Vec3 depth(0, 0, 0);
    for (u32 i = 0; i < Steps; ++i)
        depth += Extinction(Length(origin + dir * ((f32(i) + 0.5f) * dt)) - Bottom) * dt;
    return Vec3(std::exp(-depth.x), std::exp(-depth.y), std::exp(-depth.z));
}

Vec3 SunDirection(f32 elevation, f32 azimuth)
{
    return Vec3(Cos(elevation) * Sin(azimuth), Sin(elevation), Cos(elevation) * Cos(azimuth));
}

} // namespace atmosphere

Vec3 HdriToWorld(Vec3 d, f32 rotation)
{
    const f32 s = Sin(rotation), c = Cos(rotation);
    return Vec3(d.x * c + d.z * s, d.y, -d.x * s + d.z * c);
}

SkyRenderer::SkyRenderer(Device& device) : m_device(device) {}

SkyRenderer::~SkyRenderer()
{
    for (Pipeline* p : {m_transmittancePipeline, m_multiScatterPipeline, m_skyViewPipeline, m_hdriViewPipeline, m_irradiancePipeline, m_skyPipeline,
                        m_fogPipeline})
        m_device.Destroy(p);
    m_device.Destroy(m_irradianceSH);
    for (Texture* t : {m_transmittance, m_multiScatter, m_skyView})
        m_device.Destroy(t);
}

bool SkyRenderer::Init(Format colorFormat, Format depthFormat)
{
    struct Job
    {
        ShaderBytecode* out;
        StringView entry;
        ShaderStage stage;
    } jobs[] = {
        {&m_transmittanceCS, "TransmittanceCS", ShaderStage::Compute},
        {&m_multiScatterCS, "MultiScatterCS", ShaderStage::Compute},
        {&m_skyViewCS, "SkyViewCS", ShaderStage::Compute},
        {&m_hdriViewCS, "SkyViewFromHdriCS", ShaderStage::Compute},
        {&m_irradianceCS, "SkyIrradianceCS", ShaderStage::Compute},
        {&m_skyVS, "SkyVS", ShaderStage::Vertex},
        {&m_skyPS, "SkyPS", ShaderStage::Pixel},
        {&m_fogPS, "FogPS", ShaderStage::Pixel},
    };
    for (const Job& j : jobs) {
        ShaderCompileDesc d;
        d.path = "/Shaders/Sky/Sky.hlsl";
        d.entryPoint = j.entry;
        d.stage = j.stage;
        Result<ShaderBytecode> r = ShaderCompiler::Compile(d);
        if (!r) {
            ZX_LOG_ERROR(LogSky, "{}", r.GetError().message);
            return false;
        }
        *j.out = move(*r);
    }
    auto compute = [&](const ShaderBytecode& cs, StringView name) {
        ComputePipelineDesc cd;
        cd.computeShader = &cs;
        cd.debugName = name;
        return m_device.CreateComputePipeline(cd);
    };
    m_transmittancePipeline = compute(m_transmittanceCS, "Sky.Transmittance");
    m_multiScatterPipeline = compute(m_multiScatterCS, "Sky.MultiScatter");
    m_skyViewPipeline = compute(m_skyViewCS, "Sky.SkyView");
    m_hdriViewPipeline = compute(m_hdriViewCS, "Sky.SkyViewFromHdri");
    m_irradiancePipeline = compute(m_irradianceCS, "Sky.Irradiance");
    m_irradianceSH = m_device.CreateBuffer(BufferDesc{256, BufferUsage::Constant | BufferUsage::ShaderResource | BufferUsage::UnorderedAccess,
                                                      MemoryType::GpuOnly, 0, "SkyIrradianceSH"});
    GraphicsPipelineDesc pd;
    pd.vertexShader = &m_skyVS;
    pd.pixelShader = &m_skyPS;
    pd.rasterizer.cullMode = CullMode::None;
    pd.depthStencil.depthWrite = false; // GreaterEqual against cleared depth (0): background only
    pd.depthFormat = depthFormat;
    pd.colorFormats[0] = colorFormat;
    pd.colorCount = 1;
    pd.debugName = "Sky";
    m_skyPipeline = m_device.CreateGraphicsPipeline(pd);
    // Fog: premultiplied over the scene (src * 1 + dst * transmittance), no depth test.
    GraphicsPipelineDesc fd;
    fd.vertexShader = &m_skyVS;
    fd.pixelShader = &m_fogPS;
    fd.rasterizer.cullMode = CullMode::None;
    fd.depthStencil.depthTest = false;
    fd.depthStencil.depthWrite = false;
    fd.colorFormats[0] = colorFormat;
    fd.colorCount = 1;
    fd.blend[0].enable = true;
    fd.blend[0].srcColor = BlendFactor::One;
    fd.blend[0].dstColor = BlendFactor::SrcAlpha;
    fd.blend[0].srcAlpha = BlendFactor::Zero;
    fd.blend[0].dstAlpha = BlendFactor::One;
    fd.debugName = "HeightFog";
    m_fogPipeline = m_device.CreateGraphicsPipeline(fd);

    auto lut = [&](u32 w, u32 h, StringView name) {
        TextureDesc td;
        td.width = w;
        td.height = h;
        td.format = Format::R16G16B16A16_FLOAT;
        td.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess;
        td.debugName = name;
        return m_device.CreateTexture(td);
    };
    m_transmittance = lut(256, 64, "TransmittanceLUT");
    m_multiScatter = lut(32, 32, "MultiScatterLUT");
    m_skyView = lut(192, 108, "SkyViewLUT");
    return m_fogPipeline && m_transmittancePipeline && m_multiScatterPipeline && m_skyViewPipeline && m_hdriViewPipeline && m_irradiancePipeline &&
           m_irradianceSH &&
           m_skyPipeline && m_transmittance &&
           m_multiScatter && m_skyView;
}

void SkyRenderer::ApplySun(SceneLighting& lighting, Vec3 dirToSun, const SkySettings& settings)
{
    const Vec3 dir = Normalize(dirToSun);
    const Vec3 T = atmosphere::Transmittance(dir, settings.cameraAltitude) * settings.sunColor;
    // Luminous illuminance ~ Rec.709 luminance of the transmitted spectrum; color normalized to the brightest channel.
    const f32 luminance = 0.2126f * T.x + 0.7152f * T.y + 0.0722f * T.z;
    lighting.sunDirection = -dir;
    lighting.sunIlluminance = settings.sunIlluminanceTop * luminance;
    // sunColor * sunIlluminance == top illuminance * transmittance per channel; the illuminance stays photometric.
    lighting.sunColor = luminance > 1e-6f ? T / luminance : Vec3(0, 0, 0);
}

void SkyRenderer::AddPrePasses(RenderGraph& graph, const RenderView& view, Vec3 dirToSun, f32 exposure,
                               const SkySettings& settings, SceneLighting& lighting)
{
    Params& p = m_params;
    p = {};
    p.invViewProj = Inverse(view.viewProj);
    p.cameraPosition = view.position;
    p.exposure = exposure;
    p.sunDirection = Normalize(dirToSun);
    p.sunIlluminance = settings.sunIlluminanceTop;
    p.sunColor = settings.sunColor;
    p.sunAngularRadius = settings.sunAngularRadius;
    p.transmittanceLut = m_transmittance->GetSrvIndex();
    p.multiScatterLut = m_multiScatter->GetSrvIndex();
    p.skyViewLut = m_skyView->GetSrvIndex();
    p.cameraAltitude = settings.cameraAltitude + Max(view.position.y, 0.0f) * 1e-3f;
    p.skyBrightness = Max(settings.skyBrightness, 0.0f);
    p.starLuminance = Max(settings.starLuminance, 0.0f);
    p.moonDirection = SafeNormalize(settings.moonDirection, Vec3(0, 1, 0));
    p.moonIlluminance = Max(settings.moonIlluminance, 0.0f);
    p.nightLuminance = Max(settings.nightLuminance, 0.0f);
    // HDRI sky: the panorama scaled by its intensity replaces the atmosphere (sunColor x sunIlluminance = scale).
    m_hdri = settings.hdri;
    const bool hdri = m_hdri.IsActive();
    p.hdri = hdri ? m_hdri.texture : InvalidDescriptor;
    m_radianceScale = hdri ? Vec3(m_hdri.intensity) : settings.sunColor * settings.sunIlluminanceTop;
    if (hdri) {
        p.sunColor = Vec3(1, 1, 1);
        p.sunIlluminance = m_hdri.intensity;
        p.hdriRotation = m_hdri.rotation;
        const f32 width = f32(Max(m_hdri.width, 1u));
        // Background: about one texel per pixel; LUT: one texel per LUT column.
        const f32 pixelAngle = view.fovY / f32(Max(view.height, 1u));
        p.hdriLod = Max(0.0f, Log2((2.0f * Pi / width) > 0.0f ? pixelAngle / (2.0f * Pi / width) : 1.0f));
        p.hdriLutLod = Max(0.0f, Log2(width / 192.0f));
        // The resampled sun spreads over ~2 LUT texels: widen the clamp cone accordingly.
        const f32 lutTexel = 2.0f * Pi / 192.0f;
        p.hdriClampDir = m_hdri.clampDirection;
        p.hdriClampCos = m_hdri.clampCos > 1.0f ? 2.0f : Cos(Min(std::acos(Clamp(m_hdri.clampCos, -1.0f, 1.0f)) + 2.0f * lutTexel, Pi));
        p.hdriClampLum = m_hdri.clampLuminance;
    }

    const RGTexture transmittance = graph.ImportTexture(m_transmittance, ResourceState::ShaderResource);
    const RGTexture multiScatter = graph.ImportTexture(m_multiScatter, ResourceState::ShaderResource);
    const RGTexture skyView = graph.ImportTexture(m_skyView, ResourceState::ShaderResource);
    const RGBuffer sh = graph.ImportBuffer(m_irradianceSH, ResourceState::ConstantBuffer); // read as a root CBV
    m_rgTransmittance = transmittance;
    m_rgSkyView = skyView;
    m_rgSH = sh;
    if (!m_lutsReady) {
        // Static atmosphere: the transmittance and multiple-scattering LUTs are computed once.
        m_lutsReady = true;
        graph.AddPass(
            "SkyTransmittanceLUT", PassType::Compute,
            [transmittance](PassBuilder& b) {
                b.WriteTexture(transmittance);
                b.SideEffects();
            },
            [this](PassContext& ctx) {
                Params q = m_params;
                q.outputUav = m_transmittance->GetUavIndex();
                ctx.cmd.SetPipeline(m_transmittancePipeline);
                ctx.cmd.SetConstants(0, &q, sizeof(q));
                ctx.cmd.Dispatch(256 / 8, 64 / 8);
            });
        graph.AddPass(
            "SkyMultiScatterLUT", PassType::Compute,
            [transmittance, multiScatter](PassBuilder& b) {
                b.ReadTexture(transmittance);
                b.WriteTexture(multiScatter);
                b.SideEffects();
            },
            [this](PassContext& ctx) {
                Params q = m_params;
                q.outputUav = m_multiScatter->GetUavIndex();
                ctx.cmd.SetPipeline(m_multiScatterPipeline);
                ctx.cmd.SetConstants(0, &q, sizeof(q));
                ctx.cmd.Dispatch(32 / 8, 32 / 8);
            });
    }
    // The sky-view LUT and its SH only depend on the sun direction and the camera altitude: refresh on change.
    const u64 hdriKey = hdri ? (m_hdri.key | 1u) : 0u;
    const bool skyChanged = !m_skyValid || hdriKey != m_lastHdriKey ||
                            (!hdri && (LengthSq(p.sunDirection - m_lastSunDirection) > 1e-10f || Abs(p.cameraAltitude - m_lastAltitude) > 0.01f));
    m_skyValid = true;
    m_lastSunDirection = p.sunDirection;
    m_lastAltitude = p.cameraAltitude;
    m_lastHdriKey = hdriKey;
    if (skyChanged) {
    graph.AddPass(
        hdri ? "SkyViewFromHdri" : "SkyViewLUT", PassType::Compute,
        [transmittance, multiScatter, skyView](PassBuilder& b) {
            b.ReadTexture(transmittance);
            b.ReadTexture(multiScatter);
            b.WriteTexture(skyView);
            b.SideEffects();
        },
        [this, hdri](PassContext& ctx) {
            Params q = m_params;
            q.outputUav = m_skyView->GetUavIndex();
            ctx.cmd.SetPipeline(hdri ? m_hdriViewPipeline : m_skyViewPipeline);
            ctx.cmd.SetConstants(0, &q, sizeof(q));
            ctx.cmd.Dispatch((192 + 7) / 8, (108 + 7) / 8);
        });
    graph.AddPass(
        "SkyIrradianceSH", PassType::Compute,
        [skyView, sh](PassBuilder& b) {
            b.ReadTexture(skyView);
            b.WriteBuffer(sh);
            b.SideEffects();
        },
        [this](PassContext& ctx) {
            Params q = m_params;
            q.outputUav = m_irradianceSH->GetUavIndex();
            ctx.cmd.SetPipeline(m_irradiancePipeline);
            ctx.cmd.SetConstants(0, &q, sizeof(q));
            ctx.cmd.Dispatch(1);
        });
    }
    lighting.skyIrradianceBuffer = m_irradianceSH;
    lighting.skyIrradianceScale = m_radianceScale * exposure;
}

void SkyRenderer::AddSkyPass(RenderGraph& graph, RGTexture color, RGTexture depth)
{
    const RGTexture transmittance = m_rgTransmittance, skyView = m_rgSkyView;
    graph.AddPass(
        "Sky", PassType::Graphics,
        [color, depth, transmittance, skyView](PassBuilder& b) {
            b.ReadTexture(transmittance);
            b.ReadTexture(skyView);
            b.RenderTarget(color, LoadOp::Load);
            b.DepthStencil(depth, LoadOp::Load, 0.0f, true);
        },
        [this](PassContext& ctx) {
            ctx.cmd.SetPipeline(m_skyPipeline);
            ctx.cmd.SetConstants(0, &m_params, sizeof(m_params));
            ctx.cmd.Draw(3);
        });
}

void SkyRenderer::AddFogPass(RenderGraph& graph, RGTexture color, RGTexture depth, const SceneLighting& lighting,
                             const SkySettings& settings)
{
    if (!settings.fog.enabled)
        return;
    struct FogParams
    {
        f32 density, falloff, baseHeight, maxDistance;
        Vec3 albedo;
        f32 sunG;
        Vec3 sunColor;
        f32 skyScale;
        u32 depth;
        f32 startDistance;
        u32 pad[2];
    } fp = {};
    const FogSettings& f = settings.fog;
    fp.density = f.density;
    fp.falloff = Max(f.falloff, 1e-5f);
    fp.baseHeight = f.baseHeight;
    fp.maxDistance = f.maxDistance;
    fp.startDistance = Max(f.startDistance, 0.0f);
    fp.albedo = f.albedo;
    fp.sunG = f.sunAnisotropy;
    fp.sunColor = lighting.sunColor * (lighting.sunIlluminance * m_params.exposure);
    fp.skyScale = lighting.skyIrradianceScale.x;
    const RGBuffer sh = m_rgSH;
    const RGTexture skyView = m_rgSkyView;
    graph.AddPass(
        "HeightFog", PassType::Graphics,
        [color, depth, sh, skyView](PassBuilder& b) {
            b.ReadTexture(depth);
            if (skyView.IsValid())
                b.ReadTexture(skyView);
            b.ReadBuffer(sh, ResourceState::ConstantBuffer);
            b.RenderTarget(color, LoadOp::Load);
        },
        [this, fp, depth](PassContext& ctx) {
            FogParams p = fp;
            p.depth = ctx.Texture(depth)->GetSrvIndex();
            ctx.cmd.SetPipeline(m_fogPipeline);
            ctx.cmd.SetConstants(0, &m_params, sizeof(m_params));
            ctx.cmd.SetConstantBuffer(1, m_irradianceSH->GetGpuAddress());
            ctx.cmd.SetPushConstants(p);
            ctx.cmd.Draw(3);
        });
}

} // namespace zx::render
