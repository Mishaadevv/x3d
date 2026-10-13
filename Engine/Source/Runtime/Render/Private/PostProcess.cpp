#include "Render/PostProcess.h"
#include "Core/Log/Log.h"
#include "RHI/ShaderCompiler.h"

#include <cmath>

ZX_DEFINE_LOG_CATEGORY_STATIC(LogPost, Info);

namespace zx::render {

using namespace rhi;

namespace {

constexpr u32 HistogramBins = 64;
constexpr u32 EVOffset = HistogramBins * 4;

/// Approximate sRGB-ish color of a blackbody (Tanner Helland's fit), 1000..40000 K.
Vec3 BlackbodyColor(f32 kelvin)
{
    const f32 t = Clamp(kelvin, 1000.0f, 40000.0f) / 100.0f;
    f32 r, g, b;
    if (t <= 66.0f) {
        r = 255.0f;
        g = 99.4708025861f * std::log(t) - 161.1195681661f;
        b = t <= 19.0f ? 0.0f : 138.5177312231f * std::log(t - 10.0f) - 305.0447927307f;
    } else {
        r = 329.698727446f * std::pow(t - 60.0f, -0.1332047592f);
        g = 288.1221695283f * std::pow(t - 60.0f, -0.0755148492f);
        b = 255.0f;
    }
    return Vec3(Clamp(r, 1.0f, 255.0f), Clamp(g, 1.0f, 255.0f), Clamp(b, 1.0f, 255.0f)) / 255.0f;
}

} // namespace

PostProcess::PostProcess(Device& device) : m_device(device) {}

PostProcess::~PostProcess()
{
    for (Pipeline* p : {m_histogramPipeline, m_exposurePipeline, m_bloomDown, m_bloomUp, m_tonemap, m_fxaa, m_tonemapToOutput})
        m_device.Destroy(p);
    m_device.Destroy(m_histogram);
    for (Buffer* b : m_readback)
        m_device.Destroy(b);
}

bool PostProcess::Init(Format outputFormat)
{
    struct Job
    {
        ShaderBytecode* out;
        StringView entry;
        ShaderStage stage;
    } jobs[] = {
        {&m_histogramCS, "HistogramCS", ShaderStage::Compute}, {&m_exposureCS, "ExposureCS", ShaderStage::Compute},
        {&m_vs, "FullscreenVS", ShaderStage::Vertex},          {&m_bloomDownPS, "BloomDownPS", ShaderStage::Pixel},
        {&m_bloomUpPS, "BloomUpPS", ShaderStage::Pixel},       {&m_tonemapPS, "TonemapPS", ShaderStage::Pixel},
        {&m_fxaaPS, "FxaaPS", ShaderStage::Pixel},
    };
    for (const Job& j : jobs) {
        ShaderCompileDesc d;
        d.path = "/Shaders/Post/PostProcess.hlsl";
        d.entryPoint = j.entry;
        d.stage = j.stage;
        Result<ShaderBytecode> r = ShaderCompiler::Compile(d);
        if (!r) {
            ZX_LOG_ERROR(LogPost, "{}", r.GetError().message);
            return false;
        }
        *j.out = move(*r);
    }
    ComputePipelineDesc cd;
    cd.computeShader = &m_histogramCS;
    cd.debugName = "Post.Histogram";
    m_histogramPipeline = m_device.CreateComputePipeline(cd);
    cd.computeShader = &m_exposureCS;
    cd.debugName = "Post.Exposure";
    m_exposurePipeline = m_device.CreateComputePipeline(cd);

    auto fullscreen = [&](const ShaderBytecode& ps, Format format, bool additive, StringView name) {
        GraphicsPipelineDesc pd;
        pd.vertexShader = &m_vs;
        pd.pixelShader = &ps;
        pd.rasterizer.cullMode = CullMode::None;
        pd.depthStencil.depthTest = false;
        pd.depthStencil.depthWrite = false;
        pd.colorFormats[0] = format;
        pd.colorCount = 1;
        if (additive) {
            pd.blend[0].enable = true;
            pd.blend[0].srcColor = BlendFactor::One;
            pd.blend[0].dstColor = BlendFactor::One;
            pd.blend[0].srcAlpha = BlendFactor::One;
            pd.blend[0].dstAlpha = BlendFactor::One;
        }
        pd.debugName = name;
        return m_device.CreateGraphicsPipeline(pd);
    };
    m_bloomDown = fullscreen(m_bloomDownPS, Format::R11G11B10_FLOAT, false, "Post.BloomDown");
    m_bloomUp = fullscreen(m_bloomUpPS, Format::R11G11B10_FLOAT, true, "Post.BloomUp");
    m_tonemap = fullscreen(m_tonemapPS, Format::R8G8B8A8_UNORM, false, "Post.Tonemap");
    m_fxaa = fullscreen(m_fxaaPS, outputFormat, false, "Post.FXAA");
    m_tonemapToOutput = fullscreen(m_tonemapPS, outputFormat, false, "Post.TonemapToOutput");

    const u8 zeros[256 + 16] = {}; // empty bins, EV 0 = "not adapted yet"
    m_histogram = m_device.CreateBuffer(BufferDesc{sizeof(zeros), BufferUsage::ShaderResource | BufferUsage::UnorderedAccess |
                                                                      BufferUsage::CopySource,
                                                   MemoryType::GpuOnly, 0, "ExposureHistogram"},
                                        zeros);
    for (Buffer*& b : m_readback)
        b = m_device.CreateBuffer(BufferDesc{16, BufferUsage::CopyDest, MemoryType::Readback, 0, "ExposureReadback"});
    return m_histogramPipeline && m_exposurePipeline && m_bloomDown && m_bloomUp && m_tonemap && m_fxaa && m_histogram;
}

void PostProcess::AddPasses(RenderGraph& graph, RGTexture sceneColor, RGTexture sceneDepth, RGTexture output,
                            f32 exposureEV100, f32 deltaTime, const PostSettings& settings)
{
    // Read back the EV computed FramesInFlight frames ago (its fence has been waited on by BeginFrame).
    const u32 slot = u32(m_frame % FramesInFlight);
    if (m_readbackValid[slot]) {
        f32 ev;
        MemCopy(&ev, m_readback[slot]->GetMappedData(), 4);
        if (std::isfinite(ev) && ev != 0.0f) {
            m_autoEV = ev;
            m_hasAutoEV = true;
        }
    }
    ++m_frame;

    const TextureDesc& sceneDesc = graph.GetDesc(sceneColor);
    const u32 width = sceneDesc.width, height = sceneDesc.height;
    Params base = {};
    base.histogram = m_histogram->GetUavIndex();
    base.bloom = InvalidDescriptor;
    base.exposure = 1.0f / (1.2f * std::exp2(exposureEV100));
    base.targetEVBias = settings.exposureCompensation;
    base.minEV = settings.minEV100;
    base.maxEV = settings.maxEV100;
    base.adaptation = 1.0f - std::exp(-Max(deltaTime, 0.0f) * settings.adaptationSpeed);
    base.bloomIntensity = settings.bloom ? settings.bloomIntensity : 0.0f;
    base.contrast = settings.contrast;
    base.saturation = settings.saturation;
    const Vec3 white = BlackbodyColor(6500.0f), target = BlackbodyColor(settings.temperature);
    Vec3 gains(white.x / target.x, white.y / target.y, white.z / target.z);
    gains = gains / (0.2126f * gains.x + 0.7152f * gains.y + 0.0722f * gains.z); // keep luminance
    base.whiteBalance = gains;
    base.vignette = settings.vignette;
    base.tonemapper = settings.tonemapper == Tonemapper::AgX ? 1u : 0u;
    base.viewMode = settings.viewMode;

    // --- Auto exposure ---
    if (settings.autoExposure) {
        const RGBuffer histogram = graph.ImportBuffer(m_histogram, ResourceState::UnorderedAccess);
        Buffer* readback = m_readback[slot];
        m_readbackValid[slot] = true;
        graph.AddPass(
            "ExposureHistogram", PassType::Compute,
            [sceneColor, histogram](PassBuilder& b) {
                b.ReadTexture(sceneColor);
                b.WriteBuffer(histogram);
                b.SideEffects();
            },
            [this, base, sceneColor, width, height](PassContext& ctx) {
                Params p = base;
                p.input = ctx.Texture(sceneColor)->GetSrvIndex();
                p.inputTexel = Vec2(1.0f / f32(width), 1.0f / f32(height));
                ctx.cmd.SetPipeline(m_histogramPipeline);
                ctx.cmd.SetPushConstants(p);
                ctx.cmd.Dispatch((width / 2 + 15) / 16, (height / 2 + 15) / 16);
            });
        graph.AddPass(
            "AutoExposure", PassType::Compute,
            [histogram](PassBuilder& b) {
                b.WriteBuffer(histogram);
                b.SideEffects();
            },
            [this, base, readback](PassContext& ctx) {
                ctx.cmd.SetPipeline(m_exposurePipeline);
                ctx.cmd.SetPushConstants(base);
                ctx.cmd.Dispatch(1);
                ctx.cmd.CopyBuffer(readback, 0, m_histogram, EVOffset, 4);
            });
    }

    // --- Bloom: downsample chain from half resolution, then additive upsampling ---
    RGTexture bloomLevels[8];
    u32 levels = 0;
    if (settings.bloom && settings.bloomLevels > 0) {
        u32 w = width, h = height;
        RGTexture source = sceneColor;
        for (u32 l = 0; l < Min(settings.bloomLevels, 8u) && w > 8 && h > 8; ++l) {
            w = Max(1u, w / 2);
            h = Max(1u, h / 2);
            TextureDesc td;
            td.width = w;
            td.height = h;
            td.format = Format::R11G11B10_FLOAT;
            td.usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource;
            td.debugName = "Bloom";
            const RGTexture dst = graph.CreateTexture(td);
            const TextureDesc& sd = graph.GetDesc(source);
            const Vec2 texel(1.0f / f32(sd.width), 1.0f / f32(sd.height));
            graph.AddPass(
                "BloomDown", PassType::Graphics,
                [source, dst](PassBuilder& b) {
                    b.ReadTexture(source);
                    b.RenderTarget(dst, LoadOp::DontCare);
                },
                [this, base, source, texel](PassContext& ctx) {
                    Params p = base;
                    p.input = ctx.Texture(source)->GetSrvIndex();
                    p.inputTexel = texel * 0.5f; // half-texel offsets around the destination texel center
                    ctx.cmd.SetPipeline(m_bloomDown);
                    ctx.cmd.SetPushConstants(p);
                    ctx.cmd.Draw(3);
                });
            bloomLevels[levels++] = dst;
            source = dst;
        }
        for (u32 l = levels - 1; l > 0; --l) {
            const RGTexture src = bloomLevels[l], dst = bloomLevels[l - 1];
            const TextureDesc& sd = graph.GetDesc(src);
            const Vec2 texel(0.5f / f32(sd.width), 0.5f / f32(sd.height));
            graph.AddPass(
                "BloomUp", PassType::Graphics,
                [src, dst](PassBuilder& b) {
                    b.ReadTexture(src);
                    b.RenderTarget(dst, LoadOp::Load);
                },
                [this, base, src, texel](PassContext& ctx) {
                    Params p = base;
                    p.input = ctx.Texture(src)->GetSrvIndex();
                    p.inputTexel = texel;
                    ctx.cmd.SetPipeline(m_bloomUp);
                    ctx.cmd.SetPushConstants(p);
                    ctx.cmd.Draw(3);
                });
        }
    }

    // --- Tonemap (render resolution, LDR with luma in alpha) ---
    TextureDesc ld;
    ld.width = width;
    ld.height = height;
    ld.format = Format::R8G8B8A8_UNORM;
    ld.usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource;
    ld.debugName = "Tonemapped";
    // Without FXAA the tonemap pass writes the output directly (upscaling with its bilinear fetch).
    const bool fxaa = settings.fxaa;
    const RGTexture ldr = fxaa ? graph.CreateTexture(ld) : output;
    const RGTexture bloom = levels ? bloomLevels[0] : RGTexture();
    graph.AddPass(
        "Tonemap", PassType::Graphics,
        [sceneColor, sceneDepth, bloom, ldr, levels](PassBuilder& b) {
            b.ReadTexture(sceneColor);
            b.ReadTexture(sceneDepth);
            if (levels)
                b.ReadTexture(bloom);
            b.RenderTarget(ldr, LoadOp::DontCare);
        },
        [this, base, sceneColor, sceneDepth, bloom, levels, width, height, fxaa](PassContext& ctx) {
            Params p = base;
            p.input = ctx.Texture(sceneColor)->GetSrvIndex();
            p.depth = ctx.Texture(sceneDepth)->GetSrvIndex();
            p.bloom = levels ? ctx.Texture(bloom)->GetSrvIndex() : InvalidDescriptor;
            p.inputTexel = Vec2(1.0f / f32(width), 1.0f / f32(height));
            ctx.cmd.SetPipeline(fxaa ? m_tonemap : m_tonemapToOutput);
            ctx.cmd.SetPushConstants(p);
            ctx.cmd.Draw(3);
        });

    // --- FXAA + upscale to the output ---
    if (!fxaa)
        return;
    graph.AddPass(
        "FXAA", PassType::Graphics,
        [ldr, output](PassBuilder& b) {
            b.ReadTexture(ldr);
            b.RenderTarget(output, LoadOp::DontCare);
        },
        [this, base, ldr, width, height](PassContext& ctx) {
            Params p = base;
            p.input = ctx.Texture(ldr)->GetSrvIndex();
            p.inputTexel = Vec2(1.0f / f32(width), 1.0f / f32(height));
            ctx.cmd.SetPipeline(m_fxaa);
            ctx.cmd.SetPushConstants(p);
            ctx.cmd.Draw(3);
        });
}

} // namespace zx::render
