#pragma once

// Immediate-mode debug lines (depth-tested or overlay). Accumulate during the frame, Render() once.
//
//   debugDraw.Grid(Vec3::Zero(), 20.0f, 20, Color(80, 80, 80));
//   debugDraw.Box(bounds, Color(0, 255, 0));
//   debugDraw.Render(cmd, viewProj);   // inside a render pass with the formats given to Init()

#include "Core/Math/Math.h"
#include "RHI/RHI.h"

namespace zx::render {

class DebugDraw
{
public:
    explicit DebugDraw(rhi::Device& device) : m_device(device) {}
    ~DebugDraw();
    ZX_NON_COPYABLE(DebugDraw)

    bool Init(rhi::Format colorFormat, rhi::Format depthFormat, u32 maxLinesPerFrame = 65536);

    void Line(Vec3 a, Vec3 b, Color color, bool depthTest = true);
    void Box(const AABB& box, Color color, bool depthTest = true);
    void Box(const AABB& localBox, const Mat4& transform, Color color, bool depthTest = true);
    void Sphere(Vec3 center, f32 radius, Color color, u32 segments = 24, bool depthTest = true);
    void Circle(Vec3 center, Vec3 normal, f32 radius, Color color, u32 segments = 32, bool depthTest = true);
    /// Grid on the XZ plane.
    void Grid(Vec3 center, f32 size, u32 divisions, Color color, bool depthTest = true);
    /// RGB = XYZ axes of a transform.
    void Axes(const Mat4& transform, f32 length, bool depthTest = false);
    void Frustum(const Mat4& viewProjection, Color color, bool depthTest = true);
    void Arrow(Vec3 from, Vec3 to, Color color, bool depthTest = true);

    /// Draws everything queued and clears the queue. Must be called inside a matching render pass.
    void Render(rhi::CommandList& cmd, const Mat4& viewProjection);
    void Clear();
    u32 LineCount() const { return u32(m_depthLines.Size() + m_overlayLines.Size()); }

private:
    struct LineVertex
    {
        Vec3 position;
        u32 color; // sRGB RGBA8
    };
    rhi::Device& m_device;
    rhi::Pipeline* m_depthPipeline = nullptr;
    rhi::Pipeline* m_overlayPipeline = nullptr;
    rhi::Buffer* m_buffers[rhi::FramesInFlight] = {};
    u32 m_maxLines = 0;
    Array<LineVertex> m_depthLines;
    Array<LineVertex> m_overlayLines;
};

} // namespace zx::render
