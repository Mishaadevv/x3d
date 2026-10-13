#pragma once

// Temporal anti-aliasing: sub-pixel projection jitter (Halton 2,3) + history resolve (see Shaders/Post/TAA.hlsl).
//
//   const Vec2 jitter = taa.NextJitter(width, height);              // before building the view
//   view.projection = TemporalAA::ApplyJitter(projection, jitter);
//   RGTexture resolved = taa.AddPass(graph, sceneColor, depth, unjitteredViewProj, exposure);
//   post.AddPasses(graph, resolved, ...);

#include "Core/Math/Math.h"
#include "Render/RenderGraph.h"

namespace zx::render {

struct TAASettings
{
    bool enabled = true;
    f32 blend = 0.1f;        // weight of the current frame
    bool catmullRom = true;  // sharper history fetch (5 taps) vs bilinear
    u32 jitterSamples = 8;
};

class TemporalAA
{
public:
    explicit TemporalAA(rhi::Device& device);
    ~TemporalAA();
    ZX_NON_COPYABLE(TemporalAA)

    bool Init();

    /// Jitter for this frame in NDC units (call once per frame).
    Vec2 NextJitter(u32 width, u32 height, const TAASettings& settings);
    static Mat4 ApplyJitter(const Mat4& projection, Vec2 jitterNdc);

    /// Resolves `color` against the history; returns the new (persistent) history texture to feed post-processing.
    RGTexture AddPass(RenderGraph& graph, RGTexture color, RGTexture depth, const Mat4& viewProjNoJitter, f32 exposure,
                      const TAASettings& settings);
    /// Drops the history (camera cut, resolution change).
    void Reset() { m_historyValid = false; }

private:
    struct Params
    {
        Mat4 invViewProj;
        Mat4 prevViewProj;
        u32 color;
        u32 depth;
        u32 history;
        u32 output;
        Vec2 size;
        Vec2 invSize;
        f32 historyScale;
        f32 blend;
        u32 catmullRom;
        f32 pad;
    };

    rhi::Device& m_device;
    rhi::ShaderBytecode m_cs;
    rhi::Pipeline* m_pipeline = nullptr;
    rhi::Texture* m_history[2] = {};
    UVec2 m_size;
    u32 m_current = 0;
    bool m_historyValid = false;
    Mat4 m_prevViewProj;
    f32 m_prevExposure = 1.0f;
    u32 m_jitterIndex = 0;
};

} // namespace zx::render
