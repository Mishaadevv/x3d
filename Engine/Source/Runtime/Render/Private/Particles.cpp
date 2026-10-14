#include "Render/Particles.h"
#include "Core/Log/Log.h"
#include "RHI/ShaderCompiler.h"

namespace zx::render {

using namespace rhi;

ZX_DEFINE_LOG_CATEGORY_STATIC(LogParticles, Info);

namespace {

struct ParticleParams
{
    Mat4 viewProj;
    Vec3 cameraRight;
    u32 particles;
    Vec3 cameraUp;
    u32 depth;
    Vec3 cameraPosition;
    f32 nearPlane;
    Vec3 ambient;   // pre-exposed sky light
    f32 time;
    Vec3 sunLight;  // pre-exposed sun illuminance / pi
    f32 emissiveScale;
    Vec3 dirToSun;
    f32 pad0;
    Vec3 cameraForward;
    f32 pad1;
};

} // namespace

ParticleRenderer::ParticleRenderer(Device& device) : m_device(device) {}

ParticleRenderer::~ParticleRenderer()
{
    m_device.Destroy(m_pipeline);
    for (Buffer* b : m_buffers)
        m_device.Destroy(b);
}

bool ParticleRenderer::Init(Format colorFormat)
{
    struct Job
    {
        ShaderBytecode* out;
        StringView entry;
        ShaderStage stage;
    } jobs[] = {{&m_vs, "ParticleVS", ShaderStage::Vertex}, {&m_ps, "ParticlePS", ShaderStage::Pixel}};
    for (const Job& j : jobs) {
        ShaderCompileDesc d;
        d.path = "/Shaders/Particles/Particles.hlsl";
        d.entryPoint = j.entry;
        d.stage = j.stage;
        Result<ShaderBytecode> r = ShaderCompiler::Compile(d);
        if (!r) {
            ZX_LOG_ERROR(LogParticles, "{}", r.GetError().message);
            return false;
        }
        *j.out = move(*r);
    }
    GraphicsPipelineDesc pd;
    pd.vertexShader = &m_vs;
    pd.pixelShader = &m_ps;
    pd.rasterizer.cullMode = CullMode::None;
    pd.colorFormats[0] = colorFormat;
    pd.colorCount = 1;
    // Premultiplied alpha: color = src + dst * (1 - a); additive particles output a = 0.
    BlendTarget blend = BlendTarget::AlphaBlend();
    blend.srcColor = BlendFactor::One;
    blend.dstColor = BlendFactor::InvSrcAlpha;
    blend.writeMask = 0x7; // keep the scene alpha
    pd.blend[0] = blend;
    pd.debugName = "Particles";
    m_pipeline = m_device.CreateGraphicsPipeline(pd);
    for (u32 i = 0; i < FramesInFlight; ++i)
        m_buffers[i] = m_device.CreateBuffer(
            BufferDesc{u64(MaxParticles) * sizeof(GpuParticle), BufferUsage::ShaderResource, MemoryType::Upload, 0, "Particles"});
    return m_pipeline != nullptr;
}

void ParticleRenderer::AddPass(RenderGraph& graph, RGTexture color, RGTexture depth, const RenderView& view, Span<const GpuParticle> particles,
                               const SceneLighting& lighting, f32 exposure, f32 time)
{
    if (particles.Empty() || !m_pipeline)
        return;
    Buffer* buffer = m_buffers[m_device.GetFrameIndex() % FramesInFlight];
    if (!buffer)
        return;
    m_count = Min(u32(particles.Size()), MaxParticles);
    // The farthest particles are first: when capped, drop the nearest instead of popping holes in the middle.
    MemCopy(buffer->GetMappedData(), particles.Data(), usize(m_count) * sizeof(GpuParticle));

    ParticleParams p = {};
    p.viewProj = view.viewProj;
    const Mat4 invView = Inverse(view.view);
    p.cameraRight = SafeNormalize(invView.TransformVector(Vec3(1, 0, 0)), Vec3(1, 0, 0));
    p.cameraUp = SafeNormalize(invView.TransformVector(Vec3(0, 1, 0)), Vec3(0, 1, 0));
    p.cameraForward = SafeNormalize(invView.TransformVector(Vec3(0, 0, 1)), Vec3(0, 0, 1));
    p.cameraPosition = view.position;
    p.nearPlane = view.nearPlane;
    p.ambient = lighting.skyColor * (lighting.skyLuminance * exposure);
    p.sunLight = lighting.sunColor * (lighting.sunIlluminance * exposure / Pi);
    p.dirToSun = -lighting.sunDirection;
    p.emissiveScale = EmissiveNits * exposure;
    p.time = time;
    p.particles = buffer->GetSrvIndex();
    const UVec2 size(view.width, view.height);
    const u32 count = m_count;
    graph.AddPass(
        "Particles", PassType::Graphics,
        [=](PassBuilder& b) {
            b.ReadTexture(depth);
            b.RenderTarget(color, LoadOp::Load);
        },
        [=, this](PassContext& ctx) {
            ParticleParams params = p;
            params.depth = ctx.Texture(depth)->GetSrvIndex();
            ctx.cmd.SetViewport(Viewport{0, 0, f32(size.x), f32(size.y), 0, 1});
            ctx.cmd.SetScissor(0, 0, size.x, size.y);
            ctx.cmd.SetPipeline(m_pipeline);
            ctx.cmd.SetConstants(0, &params, sizeof(params));
            ctx.cmd.Draw(count * 6);
        });
}

} // namespace zx::render
