#include "UI/UIRenderer.h"
#include "Core/Log/Log.h"
#include "Core/Profiler/Profiler.h"
#include "RHI/ShaderCompiler.h"

ZX_DEFINE_LOG_CATEGORY_STATIC(LogUIRenderer, Info);

namespace zx::ui {

using namespace rhi;

namespace {

struct UIPush
{
    Vec2 invViewport;
    u32 primitives;
    u32 clips;
    u32 atlas;
    u32 primOffset;
    f32 atlasInvSize;
    f32 textGamma;
};

/// Pixels the GPU rasterizes for a primitive (mirrors the quad expansion in UI.hlsl; lines/triangles approximated).
f32 RasterizedArea(const GpuPrimitive& p, const GpuClip& clip)
{
    const u32 type = p.header & 0xF;
    const Rect c(Vec2(clip.rect.x, clip.rect.y), Vec2(clip.rect.z, clip.rect.w));
    auto clippedArea = [&](Rect r) {
        r = r.Intersect(c);
        return Max(r.Width(), 0.0f) * Max(r.Height(), 0.0f);
    };
    if (type == u32(PrimitiveType::Line)) {
        const f32 len = Length(Vec2(p.rect.z - p.rect.x, p.rect.w - p.rect.y));
        return (len + p.data.x + 2.0f) * (Max(p.data.x, 1.0f) + 2.0f);
    }
    if (type == u32(PrimitiveType::Triangle))
        return 0.5f * Abs(Cross(Vec2(p.rect.z - p.rect.x, p.rect.w - p.rect.y), Vec2(p.data.x - p.rect.x, p.data.y - p.rect.y)));
    f32 expand = 0.0f;
    if (type == u32(PrimitiveType::Rect))
        expand = 1.0f;
    else if (type == u32(PrimitiveType::Shadow))
        expand = p.blur + 1.0f;
    const Rect outer(Vec2(p.rect.x, p.rect.y) - Vec2(expand), Vec2(p.rect.z, p.rect.w) + Vec2(expand));
    f32 area = clippedArea(outer);
    if ((type == u32(PrimitiveType::Rect) || type == u32(PrimitiveType::Shadow)) && p.extra > 0.0f)
        area -= clippedArea(Rect(Vec2(p.rect.x, p.rect.y) + Vec2(p.extra), Vec2(p.rect.z, p.rect.w) - Vec2(p.extra)));
    return Max(area, 0.0f);
}

} // namespace

UIRenderer::UIRenderer(Device& device) : m_device(device) {}

UIRenderer::~UIRenderer()
{
    m_device.Destroy(m_pipeline);
    m_device.Destroy(m_atlas);
    for (FrameBuffers& fb : m_frames) {
        m_device.Destroy(fb.prims);
        m_device.Destroy(fb.clips);
    }
}

bool UIRenderer::Init(Format targetFormat)
{
    ShaderCompileDesc sd;
    sd.path = "/Shaders/UI/UI.hlsl";
    sd.entryPoint = "VSMain";
    sd.stage = ShaderStage::Vertex;
    Result<ShaderBytecode> vs = ShaderCompiler::Compile(sd);
    sd.entryPoint = "PSMain";
    sd.stage = ShaderStage::Pixel;
    Result<ShaderBytecode> ps = ShaderCompiler::Compile(sd);
    if (!vs || !ps) {
        ZX_LOG_ERROR(LogUIRenderer, "{}", !vs ? vs.GetError().message : ps.GetError().message);
        return false;
    }
    GraphicsPipelineDesc pd;
    pd.vertexShader = &*vs;
    pd.pixelShader = &*ps;
    pd.rasterizer.cullMode = CullMode::None;
    pd.colorFormats[0] = targetFormat;
    pd.colorCount = 1;
    pd.blend[0] = BlendTarget::Premultiplied();
    pd.depthFormat = Format::Unknown;
    pd.depthStencil.depthTest = false;
    pd.depthStencil.depthWrite = false;
    pd.debugName = "UI";
    m_pipeline = m_device.CreateGraphicsPipeline(pd);

    TextureDesc td;
    td.dimension = TextureDimension::Texture2DArray;
    td.width = td.height = FontSystem::AtlasPageSize;
    td.depthOrArraySize = FontSystem::MaxAtlasPages;
    td.format = Format::R8_UNORM;
    td.usage = TextureUsage::ShaderResource;
    td.debugName = "UIGlyphAtlas";
    m_atlas = m_device.CreateTexture(td);
    return m_pipeline && m_atlas;
}

void UIRenderer::EnsureCapacity(FrameBuffers& fb, u32 prims, u32 clips)
{
    if (prims > fb.primCapacity || !fb.prims) {
        m_device.Destroy(fb.prims);
        fb.primCapacity = Max(NextPowerOfTwo(Max(prims, 1024u)), fb.primCapacity);
        fb.prims = m_device.CreateBuffer(BufferDesc{u64(fb.primCapacity) * sizeof(GpuPrimitive), BufferUsage::ShaderResource,
                                                    MemoryType::Upload, 0, "UIPrimitives"});
    }
    if (clips > fb.clipCapacity || !fb.clips) {
        m_device.Destroy(fb.clips);
        fb.clipCapacity = Max(NextPowerOfTwo(Max(clips, 256u)), fb.clipCapacity);
        fb.clips = m_device.CreateBuffer(BufferDesc{u64(fb.clipCapacity) * sizeof(GpuClip), BufferUsage::ShaderResource,
                                                    MemoryType::Upload, 0, "UIClips"});
    }
}

void UIRenderer::Prepare(CommandList& cmd, FontSystem& fonts, Span<const DrawList* const> lists)
{
    ZX_PROFILE_SCOPE("UI.Prepare");
    m_stats = Stats();
    // Atlas: upload dirty row bands of each page.
    for (u32 page = 0; page < fonts.AtlasPageCount(); ++page) {
        u32 minY, maxY;
        fonts.GetDirtyRows(page, minY, maxY);
        if (maxY <= minY)
            continue;
        SubresourceData data;
        data.data = fonts.AtlasPagePixels(page) + usize(minY) * FontSystem::AtlasPageSize;
        data.rowPitch = FontSystem::AtlasPageSize;
        cmd.UploadTextureRegion(m_atlas, 0, page, 0, minY, FontSystem::AtlasPageSize, maxY - minY, data);
        fonts.ClearDirty(page);
        m_stats.uploadedRows += maxY - minY;
    }
    cmd.Barrier(m_atlas, ResourceState::ShaderResource);

    u32 prims = 0, clips = 0;
    for (const DrawList* dl : lists) {
        prims += dl->PrimitiveCount();
        clips += u32(dl->Clips().Size());
    }
    FrameBuffers& fb = m_frames[m_device.GetFrameIndex() % FramesInFlight];
    EnsureCapacity(fb, prims, clips);
    m_current = &fb;
    m_primCount = prims;
    m_stats.primitives = prims;
    m_stats.clips = clips;
    if (!fb.prims || !fb.clips) {
        m_primCount = 0;
        return;
    }
    auto* dstPrims = static_cast<GpuPrimitive*>(fb.prims->GetMappedData());
    auto* dstClips = static_cast<GpuClip*>(fb.clips->GetMappedData());
    u32 primBase = 0, clipBase = 0;
    for (const DrawList* dl : lists) {
        const Span<const GpuClip> c = dl->Clips();
        MemCopy(dstClips + clipBase, c.Data(), c.SizeBytes());
        const Span<const GpuPrimitive> p = dl->Primitives();
        for (const GpuPrimitive& prim : p)
            m_stats.coveredPixels += RasterizedArea(prim, c[prim.header >> 16]);
        if (clipBase == 0) {
            MemCopy(dstPrims + primBase, p.Data(), p.SizeBytes());
        } else {
            // Rebase clip indices into the combined clip buffer.
            for (usize i = 0; i < p.Size(); ++i) {
                GpuPrimitive prim = p[i];
                const u32 clip = (prim.header >> 16) + clipBase;
                prim.header = (prim.header & 0xFFFF) | (clip << 16);
                dstPrims[primBase + i] = prim;
            }
        }
        primBase += u32(p.Size());
        clipBase += u32(c.Size());
        ZX_ASSERT(clipBase <= 0xFFFF, "Too many UI clip rects");
    }
}

void UIRenderer::Draw(CommandList& cmd, UVec2 targetSize)
{
    if (!m_current || m_primCount == 0 || targetSize.x == 0 || targetSize.y == 0)
        return;
    cmd.SetPipeline(m_pipeline);
    cmd.SetViewport(Viewport{0, 0, f32(targetSize.x), f32(targetSize.y), 0, 1});
    cmd.SetScissor(0, 0, targetSize.x, targetSize.y);
    UIPush push;
    push.invViewport = Vec2(1.0f / f32(targetSize.x), 1.0f / f32(targetSize.y));
    push.primitives = m_current->prims->GetSrvIndex();
    push.clips = m_current->clips->GetSrvIndex();
    push.atlas = m_atlas->GetSrvIndex();
    push.primOffset = 0;
    push.atlasInvSize = 1.0f / f32(FontSystem::AtlasPageSize);
    push.textGamma = m_textGamma;
    cmd.SetPushConstants(push);
    cmd.Draw(24, m_primCount); // up to 4 quads per primitive (hollow rings), see UI.hlsl
}

void UIRenderer::AddPass(render::RenderGraph& graph, render::RGTexture target, UVec2 targetSize, Span<const render::RGTexture> sampled)
{
    Array<render::RGTexture> reads;
    reads.Append(sampled);
    graph.AddPass(
        "UI", render::PassType::Graphics,
        [target, reads = move(reads)](render::PassBuilder& b) {
            for (render::RGTexture t : reads)
                b.ReadTexture(t);
            b.RenderTarget(target, LoadOp::Load);
        },
        [this, targetSize](render::PassContext& ctx) { Draw(ctx.cmd, targetSize); });
}

} // namespace zx::ui
