#pragma once

// GPU side of ZUI: glyph atlas texture, per-frame primitive buffers, one instanced draw.
//
//   uiRenderer.Prepare(*cmd, fonts, lists);          // outside a render pass: atlas uploads + primitive buffers
//   ... begin a render pass on the target (LoadOp::Load to draw over the scene) ...
//   uiRenderer.Draw(*cmd, targetSize);
//
// or, with a render graph: Prepare() before graph.Execute() and AddPass(graph, backBuffer, size).

#include "Core/Containers/Span.h"
#include "RHI/RHI.h"
#include "Render/RenderGraph.h"
#include "UI/DrawList.h"

namespace zx::ui {

class UIRenderer
{
public:
    explicit UIRenderer(rhi::Device& device);
    ~UIRenderer();
    ZX_NON_COPYABLE(UIRenderer)

    bool Init(rhi::Format targetFormat);

    /// Uploads dirty atlas rows and copies the primitives of `lists` (drawn in order) into this frame's buffers.
    void Prepare(rhi::CommandList& cmd, FontSystem& fonts, Span<const DrawList* const> lists);
    /// Draws the prepared primitives into the current render pass.
    void Draw(rhi::CommandList& cmd, UVec2 targetSize);
    /// Render-graph pass drawing over `target`. `sampled`: graph textures shown through Image() (read by the pass).
    void AddPass(render::RenderGraph& graph, render::RGTexture target, UVec2 targetSize, Span<const render::RGTexture> sampled = {});

    /// Coverage exponent applied to text (< 1 = slightly bolder; compensates display-space blending).
    void SetTextGamma(f32 gamma) { m_textGamma = gamma; }

    struct Stats
    {
        u32 primitives = 0;
        u32 clips = 0;
        u32 uploadedRows = 0;
        f32 coveredPixels = 0.0f; // sum of rasterized quad areas (overdraw = coveredPixels / target area)
    };
    const Stats& GetStats() const { return m_stats; }

private:
    struct FrameBuffers
    {
        rhi::Buffer* prims = nullptr;
        rhi::Buffer* clips = nullptr;
        u32 primCapacity = 0;
        u32 clipCapacity = 0;
    };
    void EnsureCapacity(FrameBuffers& fb, u32 prims, u32 clips);

    rhi::Device& m_device;
    rhi::Pipeline* m_pipeline = nullptr;
    rhi::Texture* m_atlas = nullptr;
    FrameBuffers m_frames[rhi::FramesInFlight];
    FrameBuffers* m_current = nullptr;
    u32 m_primCount = 0;
    f32 m_textGamma = 0.85f;
    Stats m_stats;
};

} // namespace zx::ui
