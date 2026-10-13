#include "Render/TemporalAA.h"
#include "Core/Log/Log.h"
#include "RHI/ShaderCompiler.h"

ZX_DEFINE_LOG_CATEGORY_STATIC(LogTAA, Info);

namespace zx::render {

using namespace rhi;

namespace {
f32 Halton(u32 index, u32 base)
{
    f32 f = 1.0f, r = 0.0f;
    for (u32 i = index; i > 0; i /= base) {
        f /= f32(base);
        r += f * f32(i % base);
    }
    return r;
}
} // namespace

TemporalAA::TemporalAA(Device& device) : m_device(device) {}

TemporalAA::~TemporalAA()
{
    m_device.Destroy(m_pipeline);
    for (Texture* t : m_history)
        m_device.Destroy(t);
}

bool TemporalAA::Init()
{
    ShaderCompileDesc d;
    d.path = "/Shaders/Post/TAA.hlsl";
    d.entryPoint = "ResolveCS";
    d.stage = ShaderStage::Compute;
    Result<ShaderBytecode> r = ShaderCompiler::Compile(d);
    if (!r) {
        ZX_LOG_ERROR(LogTAA, "{}", r.GetError().message);
        return false;
    }
    m_cs = move(*r);
    ComputePipelineDesc cd;
    cd.computeShader = &m_cs;
    cd.debugName = "TAA.Resolve";
    m_pipeline = m_device.CreateComputePipeline(cd);
    return m_pipeline != nullptr;
}

Vec2 TemporalAA::NextJitter(u32 width, u32 height, const TAASettings& settings)
{
    if (!settings.enabled)
        return Vec2(0.0f, 0.0f);
    const u32 i = (m_jitterIndex++ % Max(settings.jitterSamples, 1u)) + 1; // skip index 0 (= no offset)
    // Pixel offset in [-0.5, 0.5] -> NDC.
    return Vec2((Halton(i, 2) - 0.5f) * 2.0f / f32(width), (Halton(i, 3) - 0.5f) * 2.0f / f32(height));
}

Mat4 TemporalAA::ApplyJitter(const Mat4& projection, Vec2 jitterNdc)
{
    // Perspective: clip.w = view z, so adding j * w to clip.xy shifts NDC by j (third column, x/y rows).
    Mat4 p = projection;
    p.columns[2].x += jitterNdc.x;
    p.columns[2].y += jitterNdc.y;
    return p;
}

RGTexture TemporalAA::AddPass(RenderGraph& graph, RGTexture color, RGTexture depth, const Mat4& viewProjNoJitter, f32 exposure,
                              const TAASettings& settings)
{
    const TextureDesc& cd = graph.GetDesc(color);
    if (cd.width != m_size.x || cd.height != m_size.y) {
        for (Texture*& t : m_history) {
            m_device.Destroy(t);
            TextureDesc hd;
            hd.width = cd.width;
            hd.height = cd.height;
            hd.format = Format::R16G16B16A16_FLOAT;
            hd.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess;
            hd.debugName = "TAAHistory";
            t = m_device.CreateTexture(hd);
        }
        m_size = UVec2(cd.width, cd.height);
        m_historyValid = false;
    }
    const u32 write = m_current ^ 1;
    const RGTexture history = graph.ImportTexture(m_history[m_current], ResourceState::ShaderResource);
    const RGTexture output = graph.ImportTexture(m_history[write], ResourceState::ShaderResource);
    Params p = {};
    p.invViewProj = Inverse(viewProjNoJitter);
    p.prevViewProj = m_historyValid ? m_prevViewProj : viewProjNoJitter;
    p.size = Vec2(f32(cd.width), f32(cd.height));
    p.invSize = Vec2(1.0f / p.size.x, 1.0f / p.size.y);
    p.historyScale = m_historyValid ? exposure / m_prevExposure : 1.0f;
    p.blend = settings.blend;
    p.catmullRom = settings.catmullRom ? 1u : 0u;
    const bool hasHistory = m_historyValid && settings.enabled;
    graph.AddPass(
        "TAA", PassType::Compute,
        [color, depth, history, output, hasHistory](PassBuilder& b) {
            b.ReadTexture(color);
            b.ReadTexture(depth);
            if (hasHistory)
                b.ReadTexture(history);
            b.WriteTexture(output);
        },
        [this, p, color, depth, history, output, hasHistory](PassContext& ctx) {
            Params q = p;
            q.color = ctx.Texture(color)->GetSrvIndex();
            q.depth = ctx.Texture(depth)->GetSrvIndex();
            q.history = hasHistory ? ctx.Texture(history)->GetSrvIndex() : InvalidDescriptor;
            q.output = ctx.Texture(output)->GetUavIndex();
            ctx.cmd.SetPipeline(m_pipeline);
            ctx.cmd.SetConstants(0, &q, sizeof(q));
            ctx.cmd.Dispatch((m_size.x + 7) / 8, (m_size.y + 7) / 8);
        });
    m_prevViewProj = viewProjNoJitter;
    m_prevExposure = exposure;
    m_historyValid = settings.enabled;
    m_current = write;
    return output;
}

} // namespace zx::render
