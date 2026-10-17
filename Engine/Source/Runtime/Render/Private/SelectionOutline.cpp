#include "Render/SelectionOutline.h"
#include "Core/Log/Log.h"
#include "RHI/ShaderCompiler.h"

ZX_DEFINE_LOG_CATEGORY_STATIC(LogOutline, Info);

namespace zx::render {

using namespace rhi;

namespace {

struct MaskPush
{
    Mat4 worldViewProj;
    u32 vertexBuffer;
    u32 sceneDepth;
    Vec2 outputToDepth;
    f32 nearPlane;
    f32 pad[3];
};
static_assert(sizeof(MaskPush) == 96);

struct OutlinePush
{
    u32 mask;
    f32 radius;
    f32 hiddenAlpha;
    f32 pad;
    Vec4 color;
};

Pipeline* CreatePipeline(Device& device, StringView define, StringView vsEntry, StringView psEntry, GraphicsPipelineDesc pd)
{
    const ShaderDefine defines[] = {{define, "1"}};
    ShaderCompileDesc sd;
    sd.path = "/Shaders/Editor/SelectionOutline.hlsl";
    sd.defines = Span<const ShaderDefine>(defines, 1);
    sd.entryPoint = vsEntry;
    sd.stage = ShaderStage::Vertex;
    Result<ShaderBytecode> vs = ShaderCompiler::Compile(sd);
    sd.entryPoint = psEntry;
    sd.stage = ShaderStage::Pixel;
    Result<ShaderBytecode> ps = ShaderCompiler::Compile(sd);
    if (!vs || !ps) {
        ZX_LOG_ERROR(LogOutline, "{}", !vs ? vs.GetError().message : ps.GetError().message);
        return nullptr;
    }
    pd.vertexShader = &*vs;
    pd.pixelShader = &*ps;
    return device.CreateGraphicsPipeline(pd);
}

} // namespace

SelectionOutline::~SelectionOutline()
{
    m_device.Destroy(m_maskPipeline);
    m_device.Destroy(m_outlinePipeline);
}

bool SelectionOutline::Init(Format outputFormat)
{
    GraphicsPipelineDesc mask;
    mask.rasterizer.cullMode = CullMode::None;
    mask.depthStencil.depthTest = false;
    mask.depthStencil.depthWrite = false;
    mask.colorFormats[0] = Format::R8G8_UNORM;
    mask.colorCount = 1;
    mask.blend[0].enable = true; // max: "visible" wins over "hidden" where surfaces overlap
    mask.blend[0].srcColor = mask.blend[0].dstColor = BlendFactor::One;
    mask.blend[0].srcAlpha = mask.blend[0].dstAlpha = BlendFactor::One;
    mask.blend[0].colorOp = mask.blend[0].alphaOp = BlendOp::Max;
    mask.debugName = "SelectionOutline.Mask";
    m_maskPipeline = CreatePipeline(m_device, "ZX_MASK", "MaskVS", "MaskPS", mask);

    GraphicsPipelineDesc outline;
    outline.rasterizer.cullMode = CullMode::None;
    outline.depthStencil.depthTest = false;
    outline.depthStencil.depthWrite = false;
    outline.colorFormats[0] = outputFormat;
    outline.colorCount = 1;
    outline.blend[0] = BlendTarget::AlphaBlend();
    outline.debugName = "SelectionOutline.Composite";
    m_outlinePipeline = CreatePipeline(m_device, "ZX_OUTLINE", "OutlineVS", "OutlinePS", outline);
    return m_maskPipeline && m_outlinePipeline;
}

void SelectionOutline::AddPasses(RenderGraph& graph, Span<const OutlineItem> items, const Mat4& viewProj, f32 nearPlane,
                                 RGTexture sceneDepth, UVec2 renderSize, RGTexture output, UVec2 outputSize,
                                 const OutlineSettings& settings)
{
    m_items.Clear();
    for (const OutlineItem& it : items)
        if (it.mesh && it.mesh->vertexBuffer && it.mesh->indexBuffer)
            m_items.PushBack(it);
    if (m_items.Empty() || !m_maskPipeline || !m_outlinePipeline)
        return;

    TextureDesc md;
    md.width = outputSize.x;
    md.height = outputSize.y;
    md.format = Format::R8G8_UNORM;
    md.usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource;
    md.debugName = "SelectionMask";
    const RGTexture mask = graph.CreateTexture(md);
    const Vec2 outputToDepth(f32(renderSize.x) / f32(outputSize.x), f32(renderSize.y) / f32(outputSize.y));
    graph.AddPass(
        "SelectionMask", PassType::Graphics,
        [&](PassBuilder& b) {
            b.RenderTarget(mask, LoadOp::Clear, LinearColor(0, 0, 0, 0));
            b.ReadTexture(sceneDepth);
        },
        [this, sceneDepth, viewProj, nearPlane, outputToDepth, outputSize](PassContext& ctx) {
            ctx.cmd.SetViewport(Viewport{0, 0, f32(outputSize.x), f32(outputSize.y), 0, 1});
            ctx.cmd.SetScissor(0, 0, outputSize.x, outputSize.y);
            ctx.cmd.SetPipeline(m_maskPipeline);
            for (const OutlineItem& it : m_items) {
                const GpuMesh& m = *it.mesh;
                MaskPush p{};
                p.worldViewProj = viewProj * it.world;
                p.vertexBuffer = it.vertexBuffer != 0xFFFFFFFFu ? it.vertexBuffer : m.vertexBuffer->GetSrvIndex();
                p.sceneDepth = ctx.Texture(sceneDepth)->GetSrvIndex();
                p.outputToDepth = outputToDepth;
                p.nearPlane = nearPlane;
                ctx.cmd.SetPushConstants(p);
                ctx.cmd.SetIndexBuffer(m.indexBuffer, m.index16 ? IndexFormat::UInt16 : IndexFormat::UInt32);
                for (const asset::CookedMeshSection& s : m.sections) {
                    const asset::CookedSectionLod& lod = m.SectionLodAt(s, 0);
                    ctx.cmd.DrawIndexed(lod.indexCount, 1, lod.indexOffset, 0, 0);
                }
            }
        });

    // Screen rectangle of the selection (projected bounds + outline radius): the outline pass only shades that.
    i32 x0 = 0, y0 = 0, x1 = i32(outputSize.x), y1 = i32(outputSize.y);
    {
        Vec2 lo(1e30f), hi(-1e30f);
        bool behind = false;
        for (const OutlineItem& it : m_items) {
            const Mat4 m = viewProj * it.world;
            for (u32 i = 0; i < 8 && !behind; ++i) {
                const Vec4 c = m * Vec4(it.mesh->bounds.Corner(i), 1.0f);
                if (c.w <= 1e-4f) {
                    behind = true; // crosses the camera plane: keep the whole view
                    break;
                }
                const Vec2 ndc(c.x / c.w, c.y / c.w);
                const Vec2 px((ndc.x * 0.5f + 0.5f) * f32(outputSize.x), (0.5f - ndc.y * 0.5f) * f32(outputSize.y));
                lo = Min(lo, px);
                hi = Max(hi, px);
            }
        }
        if (!behind) {
            const f32 pad = Max(settings.thickness, 1.0f) + 2.0f;
            x0 = Clamp(i32(Floor(lo.x - pad)), 0, i32(outputSize.x));
            y0 = Clamp(i32(Floor(lo.y - pad)), 0, i32(outputSize.y));
            x1 = Clamp(i32(Ceil(hi.x + pad)), 0, i32(outputSize.x));
            y1 = Clamp(i32(Ceil(hi.y + pad)), 0, i32(outputSize.y));
        }
        if (x1 <= x0 || y1 <= y0)
            return; // off screen
    }

    OutlinePush op{};
    op.radius = Max(settings.thickness, 1.0f);
    op.hiddenAlpha = settings.hiddenOpacity;
    op.color = Vec4(f32(settings.color.r) / 255.0f, f32(settings.color.g) / 255.0f, f32(settings.color.b) / 255.0f,
                    f32(settings.color.a) / 255.0f);
    graph.AddPass(
        "SelectionOutline", PassType::Graphics,
        [&](PassBuilder& b) {
            b.ReadTexture(mask);
            b.RenderTarget(output, LoadOp::Load);
        },
        [this, mask, op, outputSize, x0, y0, x1, y1](PassContext& ctx) mutable {
            ctx.cmd.SetViewport(Viewport{0, 0, f32(outputSize.x), f32(outputSize.y), 0, 1});
            ctx.cmd.SetScissor(x0, y0, u32(x1 - x0), u32(y1 - y0));
            ctx.cmd.SetPipeline(m_outlinePipeline);
            op.mask = ctx.Texture(mask)->GetSrvIndex();
            ctx.cmd.SetPushConstants(op);
            ctx.cmd.Draw(3);
        });
}

} // namespace zx::render
