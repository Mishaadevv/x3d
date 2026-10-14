#include "Render/Water.h"
#include "Core/Log/Log.h"
#include "Render/Sky.h"
#include "RHI/ShaderCompiler.h"

namespace zx::render {

using namespace rhi;

ZX_DEFINE_LOG_CATEGORY_STATIC(LogWater, Info);

namespace {

constexpr u32 WaveCount = 8;

struct WaterParams
{
    Mat4 viewProj;
    Mat4 world;
    Vec3 cameraPosition;
    f32 time;
    Vec2 size;
    u32 grid;
    f32 nearPlane;
    Vec4 waves[WaveCount];
    Vec4 waveMotion[WaveCount];
    Vec3 shallowColor;
    f32 clarity;
    Vec3 deepColor;
    f32 refraction;
    Vec3 sunRadiance;
    f32 foam;
    Vec3 dirToSun;
    f32 shoreFoam;
    Vec3 ambient;
    f32 roughness;
    Vec3 skyScale;
    u32 skyView;
    u32 sceneColor;
    u32 sceneDepth;
    Vec2 invRenderSize;
};

struct CopyParams
{
    u32 srcColor, srcDepth, dstColor, dstDepth, width, height;
};

/// Eight Gerstner waves spread around the wind direction, from the longest (waveLength) to short chop, with
/// incommensurate lengths so the pattern never visibly repeats. Waves shorter than four grid cells only shade.
void SetupWaves(const WaterSurface& s, WaterParams& p, f32 cellSize)
{
    static const f32 LengthScale[WaveCount] = {1.0f, 0.73f, 0.53f, 0.41f, 0.29f, 0.21f, 0.15f, 0.11f};
    static const f32 AngleOffset[WaveCount] = {0.0f, 0.62f, -0.48f, 1.12f, -1.03f, 0.31f, -0.27f, 1.65f};
    static const f32 AmplitudeScale[WaveCount] = {1.0f, 0.68f, 0.48f, 0.36f, 0.25f, 0.17f, 0.12f, 0.08f};
    for (u32 i = 0; i < WaveCount; ++i) {
        const f32 length = Max(s.waveLength * LengthScale[i], 0.1f);
        const f32 k = TwoPi / length;
        const f32 amplitude = Max(s.waveHeight, 0.0f) * AmplitudeScale[i];
        const f32 angle = s.windDirection + AngleOffset[i];
        const f32 omega = Sqrt(9.81f * k) * s.waveSpeed; // deep-water dispersion
        // Steepness: the sum over all waves stays below 1 so crests never loop.
        const f32 q = amplitude > 0.0f ? Min(Clamp(s.choppiness, 0.0f, 1.0f) / (k * amplitude * f32(WaveCount)), 1.0f / (k * amplitude * f32(WaveCount))) : 0.0f;
        p.waves[i] = Vec4(Cos(angle), Sin(angle), k, amplitude);
        p.waveMotion[i] = Vec4(omega, q, length >= cellSize * 4.0f ? 1.0f : 0.0f, 0.0f);
    }
}

} // namespace

WaterRenderer::WaterRenderer(Device& device) : m_device(device) {}

WaterRenderer::~WaterRenderer()
{
    m_device.Destroy(m_copyPipeline);
    m_device.Destroy(m_pipeline);
}

bool WaterRenderer::Init(Format colorFormat, Format depthFormat, bool lite)
{
    m_grid = lite ? 96 : 160;
    struct Job
    {
        ShaderBytecode* out;
        StringView entry;
        ShaderStage stage;
    } jobs[] = {
        {&m_copyCS, "CopySceneCS", ShaderStage::Compute},
        {&m_vs, "WaterVS", ShaderStage::Vertex},
        {&m_ps, "WaterPS", ShaderStage::Pixel},
    };
    for (const Job& j : jobs) {
        ShaderCompileDesc d;
        d.path = "/Shaders/Water/Water.hlsl";
        d.entryPoint = j.entry;
        d.stage = j.stage;
        Result<ShaderBytecode> r = ShaderCompiler::Compile(d);
        if (!r) {
            ZX_LOG_ERROR(LogWater, "{}", r.GetError().message);
            return false;
        }
        *j.out = move(*r);
    }
    ComputePipelineDesc cd;
    cd.computeShader = &m_copyCS;
    cd.debugName = "Water.CopyScene";
    m_copyPipeline = m_device.CreateComputePipeline(cd);
    GraphicsPipelineDesc pd;
    pd.vertexShader = &m_vs;
    pd.pixelShader = &m_ps;
    pd.rasterizer.cullMode = CullMode::None; // seen from below too
    pd.depthFormat = depthFormat;
    pd.colorFormats[0] = colorFormat;
    pd.colorCount = 1;
    pd.debugName = "Water";
    m_pipeline = m_device.CreateGraphicsPipeline(pd);
    return m_copyPipeline && m_pipeline;
}

void WaterRenderer::AddPasses(RenderGraph& graph, RGTexture color, RGTexture depth, const RenderView& view,
                              Span<const WaterSurface> surfaces, const SceneLighting& lighting, const SkyRenderer* sky, f32 time,
                              f32 exposure)
{
    if (surfaces.Empty() || !m_pipeline)
        return;
    m_surfaces.Clear();
    m_surfaces.Append(surfaces);

    // Copies of the scene for refraction and thickness (the water then writes color and depth in place).
    TextureDesc cd;
    cd.width = view.width;
    cd.height = view.height;
    cd.format = Format::R16G16B16A16_FLOAT;
    cd.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess;
    cd.debugName = "WaterSceneColor";
    const RGTexture colorCopy = graph.CreateTexture(cd);
    cd.format = Format::R32_FLOAT;
    cd.debugName = "WaterSceneDepth";
    const RGTexture depthCopy = graph.CreateTexture(cd);
    const UVec2 size(view.width, view.height);
    graph.AddPass(
        "WaterCopy", PassType::Compute,
        [=](PassBuilder& b) {
            b.ReadTexture(color);
            b.ReadTexture(depth);
            b.WriteTexture(colorCopy);
            b.WriteTexture(depthCopy);
        },
        [=, this](PassContext& ctx) {
            CopyParams p{ctx.Texture(color)->GetSrvIndex(), ctx.Texture(depth)->GetSrvIndex(), ctx.Texture(colorCopy)->GetUavIndex(),
                         ctx.Texture(depthCopy)->GetUavIndex(), size.x, size.y};
            ctx.cmd.SetPipeline(m_copyPipeline);
            ctx.cmd.SetPushConstants(p);
            ctx.cmd.Dispatch((size.x + 7) / 8, (size.y + 7) / 8);
        });

    WaterParams base = {};
    base.viewProj = view.viewProj;
    base.cameraPosition = view.position;
    base.time = time;
    base.grid = m_grid;
    base.nearPlane = view.nearPlane;
    base.sunRadiance = lighting.sunColor * (lighting.sunIlluminance * exposure);
    base.dirToSun = -lighting.sunDirection;
    base.ambient = lighting.skyColor * (lighting.skyLuminance * exposure);
    base.invRenderSize = Vec2(1.0f / f32(view.width), 1.0f / f32(view.height));
    base.skyView = InvalidDescriptor;
    const RGTexture skyView = sky ? sky->SkyViewTexture() : RGTexture{};
    if (skyView.IsValid())
        base.skyScale = sky->SkyRadianceScale();
    graph.AddPass(
        "Water", PassType::Graphics,
        [=](PassBuilder& b) {
            b.ReadTexture(colorCopy);
            b.ReadTexture(depthCopy);
            if (skyView.IsValid())
                b.ReadTexture(skyView);
            b.RenderTarget(color, LoadOp::Load);
            b.DepthStencil(depth, LoadOp::Load);
        },
        [=, this](PassContext& ctx) {
            ctx.cmd.SetViewport(Viewport{0, 0, f32(size.x), f32(size.y), 0, 1});
            ctx.cmd.SetScissor(0, 0, size.x, size.y);
            ctx.cmd.SetPipeline(m_pipeline);
            for (const WaterSurface& s : m_surfaces) {
                WaterParams p = base;
                p.world = s.world;
                p.size = s.size;
                p.shallowColor = s.shallowColor;
                p.deepColor = s.deepColor;
                p.clarity = s.clarity;
                p.refraction = s.refraction;
                p.foam = s.foam;
                p.shoreFoam = s.shoreFoam;
                p.roughness = Clamp(s.roughness, 0.01f, 1.0f);
                SetupWaves(s, p, Max(s.size.x, s.size.y) / f32(m_grid));
                p.sceneColor = ctx.Texture(colorCopy)->GetSrvIndex();
                p.sceneDepth = ctx.Texture(depthCopy)->GetSrvIndex();
                if (skyView.IsValid())
                    p.skyView = ctx.Texture(skyView)->GetSrvIndex();
                ctx.cmd.SetConstants(0, &p, sizeof(p));
                ctx.cmd.Draw(m_grid * m_grid * 6);
            }
        });
}

} // namespace zx::render
