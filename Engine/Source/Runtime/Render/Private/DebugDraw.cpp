#include "Render/DebugDraw.h"
#include "Core/Log/Log.h"
#include "RHI/ShaderCompiler.h"

ZX_DEFINE_LOG_CATEGORY_STATIC(LogDebugDraw, Info);

namespace zx::render {

using namespace rhi;

DebugDraw::~DebugDraw()
{
    m_device.Destroy(m_depthPipeline);
    m_device.Destroy(m_overlayPipeline);
    for (Buffer* b : m_buffers)
        m_device.Destroy(b);
}

bool DebugDraw::Init(Format colorFormat, Format depthFormat, u32 maxLinesPerFrame)
{
    ShaderCompileDesc sd;
    sd.path = "/Shaders/Debug/DebugDraw.hlsl";
    sd.entryPoint = "VSMain";
    sd.stage = ShaderStage::Vertex;
    Result<ShaderBytecode> vs = ShaderCompiler::Compile(sd);
    sd.entryPoint = "PSMain";
    sd.stage = ShaderStage::Pixel;
    Result<ShaderBytecode> ps = ShaderCompiler::Compile(sd);
    if (!vs || !ps) {
        ZX_LOG_ERROR(LogDebugDraw, "{}", !vs ? vs.GetError().message : ps.GetError().message);
        return false;
    }
    GraphicsPipelineDesc pd;
    pd.vertexShader = &*vs;
    pd.pixelShader = &*ps;
    pd.topology = PrimitiveTopology::LineList;
    pd.rasterizer.cullMode = CullMode::None;
    pd.colorFormats[0] = colorFormat;
    pd.colorCount = 1;
    pd.blend[0] = BlendTarget::AlphaBlend();
    pd.depthFormat = depthFormat;
    pd.depthStencil.depthWrite = false;
    pd.debugName = "DebugDraw.Depth";
    m_depthPipeline = m_device.CreateGraphicsPipeline(pd);
    pd.depthStencil.depthTest = false;
    pd.debugName = "DebugDraw.Overlay";
    m_overlayPipeline = m_device.CreateGraphicsPipeline(pd);
    m_maxLines = maxLinesPerFrame;
    for (u32 i = 0; i < FramesInFlight; ++i)
        m_buffers[i] = m_device.CreateBuffer(BufferDesc{u64(maxLinesPerFrame) * 2 * sizeof(LineVertex),
                                                        BufferUsage::ShaderResource, MemoryType::Upload, 0, "DebugDrawLines"});
    return m_depthPipeline && m_overlayPipeline;
}

void DebugDraw::Line(Vec3 a, Vec3 b, Color color, bool depthTest)
{
    Array<LineVertex>& lines = depthTest ? m_depthLines : m_overlayLines;
    const u32 c = color.ToPackedABGR();
    lines.PushBack(LineVertex{a, c});
    lines.PushBack(LineVertex{b, c});
}

void DebugDraw::Box(const AABB& b, Color color, bool depthTest) { Box(b, Mat4::Identity(), color, depthTest); }

void DebugDraw::Box(const AABB& b, const Mat4& m, Color color, bool depthTest)
{
    Vec3 c[8];
    for (u32 i = 0; i < 8; ++i)
        c[i] = m.TransformPointAffine(b.Corner(i));
    static const u8 edges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (const auto& e : edges)
        Line(c[e[0]], c[e[1]], color, depthTest);
}

void DebugDraw::Circle(Vec3 center, Vec3 normal, f32 radius, Color color, u32 segments, bool depthTest)
{
    Vec3 t, bt;
    OrthonormalBasis(SafeNormalize(normal, Vec3::Up()), t, bt);
    Vec3 prev = center + t * radius;
    for (u32 i = 1; i <= segments; ++i) {
        const f32 a = TwoPi * f32(i) / f32(segments);
        const Vec3 p = center + (t * Cos(a) + bt * Sin(a)) * radius;
        Line(prev, p, color, depthTest);
        prev = p;
    }
}

void DebugDraw::Sphere(Vec3 center, f32 radius, Color color, u32 segments, bool depthTest)
{
    Circle(center, Vec3::UnitX(), radius, color, segments, depthTest);
    Circle(center, Vec3::UnitY(), radius, color, segments, depthTest);
    Circle(center, Vec3::UnitZ(), radius, color, segments, depthTest);
}

void DebugDraw::Grid(Vec3 center, f32 size, u32 divisions, Color color, bool depthTest)
{
    const f32 half = size * 0.5f;
    const f32 step = size / f32(divisions);
    for (u32 i = 0; i <= divisions; ++i) {
        const f32 o = -half + step * f32(i);
        const bool major = i == divisions / 2;
        const Color c = major ? Color(u8(Min(255, color.r + 60)), u8(Min(255, color.g + 60)), u8(Min(255, color.b + 60)), color.a)
                              : color;
        Line(center + Vec3(o, 0, -half), center + Vec3(o, 0, half), c, depthTest);
        Line(center + Vec3(-half, 0, o), center + Vec3(half, 0, o), c, depthTest);
    }
}

void DebugDraw::Axes(const Mat4& m, f32 length, bool depthTest)
{
    const Vec3 o = m.GetTranslation();
    Line(o, o + Normalize(m.GetRight()) * length, Color(230, 60, 60), depthTest);
    Line(o, o + Normalize(m.GetUp()) * length, Color(60, 220, 60), depthTest);
    Line(o, o + Normalize(m.GetForward()) * length, Color(70, 110, 240), depthTest);
}

void DebugDraw::Frustum(const Mat4& viewProjection, Color color, bool depthTest)
{
    const Mat4 inv = Inverse(viewProjection);
    Vec3 c[8];
    for (u32 i = 0; i < 8; ++i) {
        // Reversed-Z: near plane at depth 1, far at depth ~0 (use a small epsilon for infinite projections).
        const Vec4 ndc((i & 1) ? 1.0f : -1.0f, (i & 2) ? 1.0f : -1.0f, (i & 4) ? 1e-4f : 1.0f, 1.0f);
        const Vec4 p = inv * ndc;
        c[i] = p.XYZ() / p.w;
    }
    static const u8 edges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (const auto& e : edges)
        Line(c[e[0]], c[e[1]], color, depthTest);
}

void DebugDraw::Arrow(Vec3 from, Vec3 to, Color color, bool depthTest)
{
    Line(from, to, color, depthTest);
    const Vec3 dir = to - from;
    const f32 len = Length(dir);
    if (len < 1e-5f)
        return;
    const Vec3 d = dir / len;
    Vec3 t, bt;
    OrthonormalBasis(d, t, bt);
    const f32 head = len * 0.15f;
    for (const Vec3& side : {t, -t, bt, -bt})
        Line(to, to - d * head + side * head * 0.4f, color, depthTest);
}

void DebugDraw::Clear()
{
    m_depthLines.Clear();
    m_overlayLines.Clear();
}

void DebugDraw::Render(CommandList& cmd, const Mat4& viewProjection)
{
    Buffer* buffer = m_buffers[m_device.GetFrameIndex() % FramesInFlight];
    const u32 maxVertices = m_maxLines * 2;
    const u32 depthCount = Min(u32(m_depthLines.Size()), maxVertices);
    const u32 overlayCount = Min(u32(m_overlayLines.Size()), maxVertices - depthCount);
    if (!buffer || depthCount + overlayCount == 0) {
        Clear();
        return;
    }
    auto* dst = static_cast<LineVertex*>(buffer->GetMappedData());
    MemCopy(dst, m_depthLines.Data(), usize(depthCount) * sizeof(LineVertex));
    MemCopy(dst + depthCount, m_overlayLines.Data(), usize(overlayCount) * sizeof(LineVertex));
    struct
    {
        Mat4 viewProj;
        u32 vertexBuffer;
        u32 baseVertex;
        u32 pad[2];
    } push = {viewProjection, buffer->GetSrvIndex(), 0, {0, 0}};
    if (depthCount) {
        cmd.SetPipeline(m_depthPipeline);
        cmd.SetPushConstants(push);
        cmd.Draw(depthCount);
    }
    if (overlayCount) {
        push.baseVertex = depthCount;
        cmd.SetPipeline(m_overlayPipeline);
        cmd.SetPushConstants(push);
        cmd.Draw(overlayCount);
    }
    Clear();
}

} // namespace zx::render
