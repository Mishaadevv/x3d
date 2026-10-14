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

f32 CircleOfConfusion(f32 depthMeters, f32 focusDistance, f32 fStop, f32 fovY, f32 imageHeight)
{
    // Thin lens: CoC = f^2 / (N (S1 - f)) * (1 - S1 / S2), in mm on a 24 mm high sensor.
    const f32 f = 12.0f / std::tan(Max(fovY, 0.01f) * 0.5f); // focal length, mm
    const f32 s1 = Max(focusDistance * 1000.0f, f * 1.01f);
    const f32 scale = f * f / (Max(fStop, 0.5f) * (s1 - f)) / 24.0f * imageHeight;
    return scale * (1.0f - s1 / Max(depthMeters * 1000.0f, 1e-3f));
}

PostProcess::PostProcess(Device& device) : m_device(device) {}

PostProcess::~PostProcess()
{
    for (Pipeline* p : {m_histogramPipeline, m_exposurePipeline, m_bloomDown, m_bloomUp, m_tonemap, m_fxaa, m_tonemapToOutput, m_dofPrepare,
                        m_dofBlur, m_dofComposite, m_motionBlur})
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
        {&m_dofPrepareCS, "DofPrepareCS", ShaderStage::Compute}, {&m_dofBlurCS, "DofBlurCS", ShaderStage::Compute},
        {&m_dofCompositeCS, "DofCompositeCS", ShaderStage::Compute}, {&m_motionBlurCS, "MotionBlurCS", ShaderStage::Compute},
    };
    for (const Job& j : jobs) {
        ShaderCompileDesc d;
        const bool dof = j.out == &m_dofPrepareCS || j.out == &m_dofBlurCS || j.out == &m_dofCompositeCS || j.out == &m_motionBlurCS;
        d.path = dof ? "/Shaders/Post/DepthOfField.hlsl" : "/Shaders/Post/PostProcess.hlsl";
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
    cd.computeShader = &m_dofPrepareCS;
    cd.debugName = "Post.DofPrepare";
    m_dofPrepare = m_device.CreateComputePipeline(cd);
    cd.computeShader = &m_dofBlurCS;
    cd.debugName = "Post.DofBlur";
    m_dofBlur = m_device.CreateComputePipeline(cd);
    cd.computeShader = &m_dofCompositeCS;
    cd.debugName = "Post.DofComposite";
    m_dofComposite = m_device.CreateComputePipeline(cd);
    cd.computeShader = &m_motionBlurCS;
    cd.debugName = "Post.MotionBlur";
    m_motionBlur = m_device.CreateComputePipeline(cd);

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
    return m_histogramPipeline && m_exposurePipeline && m_bloomDown && m_bloomUp && m_tonemap && m_fxaa && m_histogram && m_dofPrepare &&
           m_dofBlur && m_dofComposite && m_motionBlur;
}

RGTexture PostProcess::AddDepthOfField(RenderGraph& graph, RGTexture color, RGTexture depth, const DofParams& base)
{
    const TextureDesc& cd = graph.GetDesc(color);
    TextureDesc hd;
    hd.width = Max(1u, (cd.width + 1) / 2);
    hd.height = Max(1u, (cd.height + 1) / 2);
    hd.format = Format::R16G16B16A16_FLOAT;
    hd.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess;
    hd.debugName = "DofHalf";
    const RGTexture half = graph.CreateTexture(hd);
    hd.debugName = "DofBlurred";
    const RGTexture blurred = graph.CreateTexture(hd);
    TextureDesc fd = hd;
    fd.width = cd.width;
    fd.height = cd.height;
    fd.debugName = "DofResult";
    const RGTexture result = graph.CreateTexture(fd);
    const Vec2 halfSize(f32(hd.width), f32(hd.height)), fullSize(f32(cd.width), f32(cd.height));

    graph.AddPass(
        "DofPrepare", PassType::Compute,
        [=](PassBuilder& b) {
            b.ReadTexture(color);
            b.ReadTexture(depth);
            b.WriteTexture(half);
        },
        [this, base, color, depth, half, halfSize](PassContext& ctx) {
            DofParams p = base;
            p.color = ctx.Texture(color)->GetSrvIndex();
            p.depth = ctx.Texture(depth)->GetSrvIndex();
            p.output = ctx.Texture(half)->GetUavIndex();
            p.size = halfSize;
            p.invSize = Vec2(1.0f / halfSize.x, 1.0f / halfSize.y);
            ctx.cmd.SetPipeline(m_dofPrepare);
            ctx.cmd.SetConstants(0, &p, sizeof(p));
            ctx.cmd.Dispatch((u32(halfSize.x) + 7) / 8, (u32(halfSize.y) + 7) / 8);
        });
    graph.AddPass(
        "DofBlur", PassType::Compute,
        [=](PassBuilder& b) {
            b.ReadTexture(half);
            b.WriteTexture(blurred);
        },
        [this, base, half, blurred, halfSize](PassContext& ctx) {
            DofParams p = base;
            p.input = ctx.Texture(half)->GetSrvIndex();
            p.output = ctx.Texture(blurred)->GetUavIndex();
            p.size = halfSize;
            p.invSize = Vec2(1.0f / halfSize.x, 1.0f / halfSize.y);
            ctx.cmd.SetPipeline(m_dofBlur);
            ctx.cmd.SetConstants(0, &p, sizeof(p));
            ctx.cmd.Dispatch((u32(halfSize.x) + 7) / 8, (u32(halfSize.y) + 7) / 8);
        });
    graph.AddPass(
        "DofComposite", PassType::Compute,
        [=](PassBuilder& b) {
            b.ReadTexture(color);
            b.ReadTexture(depth);
            b.ReadTexture(blurred);
            b.WriteTexture(result);
        },
        [this, base, color, depth, blurred, result, fullSize](PassContext& ctx) {
            DofParams p = base;
            p.color = ctx.Texture(color)->GetSrvIndex();
            p.depth = ctx.Texture(depth)->GetSrvIndex();
            p.input = ctx.Texture(blurred)->GetSrvIndex();
            p.output = ctx.Texture(result)->GetUavIndex();
            p.size = fullSize;
            p.invSize = Vec2(1.0f / fullSize.x, 1.0f / fullSize.y);
            ctx.cmd.SetPipeline(m_dofComposite);
            ctx.cmd.SetConstants(0, &p, sizeof(p));
            ctx.cmd.Dispatch((u32(fullSize.x) + 7) / 8, (u32(fullSize.y) + 7) / 8);
        });
    return result;
}

RGTexture PostProcess::AddMotionBlur(RenderGraph& graph, RGTexture color, RGTexture depth, const DofParams& base)
{
    const TextureDesc& cd = graph.GetDesc(color);
    TextureDesc td;
    td.width = cd.width;
    td.height = cd.height;
    td.format = Format::R16G16B16A16_FLOAT;
    td.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess;
    td.debugName = "MotionBlur";
    const RGTexture result = graph.CreateTexture(td);
    const Vec2 size(f32(cd.width), f32(cd.height));
    graph.AddPass(
        "MotionBlur", PassType::Compute,
        [=](PassBuilder& b) {
            b.ReadTexture(color);
            b.ReadTexture(depth);
            b.WriteTexture(result);
        },
        [this, base, color, depth, result, size](PassContext& ctx) {
            DofParams p = base;
            p.color = ctx.Texture(color)->GetSrvIndex();
            p.depth = ctx.Texture(depth)->GetSrvIndex();
            p.output = ctx.Texture(result)->GetUavIndex();
            p.size = size;
            p.invSize = Vec2(1.0f / size.x, 1.0f / size.y);
            ctx.cmd.SetPipeline(m_motionBlur);
            ctx.cmd.SetConstants(0, &p, sizeof(p));
            ctx.cmd.Dispatch((u32(size.x) + 7) / 8, (u32(size.y) + 7) / 8);
        });
    return result;
}

void PostProcess::AddPasses(RenderGraph& graph, RGTexture sceneColor, RGTexture sceneDepth, RGTexture output,
                            f32 exposureEV100, f32 deltaTime, const PostSettings& settings, const PostView* view)
{
    // --- Depth of field, motion blur (before exposure/bloom: they work on the HDR image) ---
    if (view) {
        const TextureDesc& sd = graph.GetDesc(sceneColor);
        DofParams dp = {};
        dp.invViewProj = Inverse(view->viewProj);
        dp.prevViewProj = m_hasPrevView && !view->cut ? m_prevViewProj : view->viewProj;
        dp.fullInvSize = Vec2(1.0f / f32(sd.width), 1.0f / f32(sd.height));
        dp.nearZ = view->nearPlane;
        // cocScale * (1 - focus / z): the CoC of a point at infinity.
        dp.cocScale = CircleOfConfusion(1e9f, settings.focusDistance, settings.fStop, view->fovY, f32(sd.height));
        dp.focusDistance = Max(settings.focusDistance, 0.01f);
        dp.maxCoc = Clamp(settings.maxBlur, 1.0f, 64.0f);
        dp.radiusStep = Clamp(settings.dofQuality, 0.5f, 4.0f);
        dp.motionScale = settings.motionBlurAmount;
        dp.motionMax = settings.motionBlurMax;
        dp.samples = Clamp(settings.motionBlurSamples, 2u, 32u);
        dp.viewMode = settings.viewMode;
        dp.input = InvalidDescriptor;
        if (settings.depthOfField || settings.viewMode == 5)
            sceneColor = AddDepthOfField(graph, sceneColor, sceneDepth, dp);
        if (settings.motionBlur && settings.motionBlurAmount > 0.0f && m_hasPrevView && !view->cut)
            sceneColor = AddMotionBlur(graph, sceneColor, sceneDepth, dp);
        m_prevViewProj = view->viewProj;
        m_hasPrevView = true;
    }

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
    base.colorFilter = settings.colorFilter;
    base.lut = settings.colorLutIntensity > 0.0f ? settings.colorLut : InvalidDescriptor;
    base.lutIntensity = Saturate(settings.colorLutIntensity);

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
