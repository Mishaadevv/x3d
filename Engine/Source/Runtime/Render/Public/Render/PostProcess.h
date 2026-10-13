#pragma once

// Post-processing: luminance histogram + auto exposure (EV100, read back to drive the next frames' pre-exposure),
// dual-filter bloom, tonemapping (ACES fit / AgX) with basic grading, FXAA and upscale to the output.

#include "Render/RenderGraph.h"

namespace zx::render {

enum class Tonemapper : u8
{
    ACES,
    AgX,
};

struct PostSettings
{
    bool autoExposure = true;
    f32 exposureCompensation = 1.0f; // EV, + = brighter (auto exposure aims at the bright half of the histogram)
    f32 minEV100 = -2.0f;
    f32 maxEV100 = 16.0f;
    f32 adaptationSpeed = 1.5f; // 1/s
    bool bloom = true;
    f32 bloomIntensity = 0.04f;
    u32 bloomLevels = 5;
    Tonemapper tonemapper = Tonemapper::ACES; // ACES keeps skies and water saturated (AgX: softer, desaturated look)
    f32 contrast = 1.05f;
    f32 saturation = 1.05f;
    f32 temperature = 6500.0f; // white balance, Kelvin
    f32 vignette = 0.25f;
    bool fxaa = true;
    /// 0 final, 1 HDR without tonemapping, 2 depth, 3 luminance heatmap (EV), 4 bloom only.
    u32 viewMode = 0;
};

class PostProcess
{
public:
    explicit PostProcess(rhi::Device& device);
    ~PostProcess();
    ZX_NON_COPYABLE(PostProcess)

    bool Init(rhi::Format outputFormat);

    /// sceneColor: linear HDR pre-exposed with `exposureEV100`. Writes the final image into `output`.
    void AddPasses(RenderGraph& graph, RGTexture sceneColor, RGTexture sceneDepth, RGTexture output, f32 exposureEV100,
                   f32 deltaTime, const PostSettings& settings);

    /// Latest adapted EV100 computed on the GPU (a couple of frames old); valid once HasAutoExposure().
    f32 GetAutoExposureEV100() const { return m_autoEV; }
    bool HasAutoExposure() const { return m_hasAutoEV; }

private:
    struct Params
    {
        u32 input;
        u32 output;
        u32 histogram;
        u32 bloom;
        Vec2 inputTexel;
        Vec2 outputSize;
        f32 exposure;
        f32 targetEVBias;
        f32 minEV;
        f32 maxEV;
        f32 adaptation;
        f32 bloomIntensity;
        f32 contrast;
        f32 saturation;
        Vec3 whiteBalance;
        f32 vignette;
        u32 tonemapper;
        u32 viewMode;
        u32 depth;
        f32 pad;
    };
    static_assert(sizeof(Params) == 96);

    rhi::Device& m_device;
    rhi::ShaderBytecode m_histogramCS, m_exposureCS, m_vs, m_bloomDownPS, m_bloomUpPS, m_tonemapPS, m_fxaaPS;
    rhi::Pipeline* m_histogramPipeline = nullptr;
    rhi::Pipeline* m_exposurePipeline = nullptr;
    rhi::Pipeline* m_bloomDown = nullptr;
    rhi::Pipeline* m_bloomUp = nullptr; // additive
    rhi::Pipeline* m_tonemap = nullptr;
    rhi::Pipeline* m_fxaa = nullptr;
    rhi::Pipeline* m_tonemapToOutput = nullptr;
    rhi::Buffer* m_histogram = nullptr; // 64 bins + adapted EV
    rhi::Buffer* m_readback[rhi::FramesInFlight] = {};
    bool m_readbackValid[rhi::FramesInFlight] = {};
    u64 m_frame = 0;
    f32 m_autoEV = 0.0f;
    bool m_hasAutoEV = false;
};

} // namespace zx::render
