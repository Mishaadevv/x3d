#pragma once

// Editor selection outline: the selected meshes are drawn into a mask (testing against the scene depth), then a soft
// outline is blended around the mask over the final (tonemapped) image. Parts hidden behind other geometry get a
// dimmer outline, like UE/Unity.
//
//   outline.Init(Format::R8G8B8A8_UNORM);
//   outline.AddPasses(graph, items, viewProjNoJitter, nearPlane, sceneDepth, renderSize, output, outputSize, settings);

#include "Render/RenderAssets.h"
#include "Render/RenderGraph.h"

namespace zx::render {

struct OutlineItem
{
    const GpuMesh* mesh = nullptr;
    Mat4 world = Mat4::Identity();
};

struct OutlineSettings
{
    Color color = Color(0xF5, 0x9E, 0x1B); // UE-like orange (sRGB)
    f32 thickness = 2.0f;                  // pixels
    f32 hiddenOpacity = 0.35f;
};

class SelectionOutline
{
public:
    explicit SelectionOutline(rhi::Device& device) : m_device(device) {}
    ~SelectionOutline();
    ZX_NON_COPYABLE(SelectionOutline)

    bool Init(rhi::Format outputFormat);

    /// `sceneDepth` is the (reversed-Z) depth at `renderSize`; `output` is the final image at `outputSize`.
    void AddPasses(RenderGraph& graph, Span<const OutlineItem> items, const Mat4& viewProj, f32 nearPlane, RGTexture sceneDepth,
                   UVec2 renderSize, RGTexture output, UVec2 outputSize, const OutlineSettings& settings);

private:
    rhi::Device& m_device;
    rhi::Pipeline* m_maskPipeline = nullptr;
    rhi::Pipeline* m_outlinePipeline = nullptr;
    Array<OutlineItem> m_items; // copy for the pass lambda (valid until the graph executes)
};

} // namespace zx::render
