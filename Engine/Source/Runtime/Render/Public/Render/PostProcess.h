#pragma once

// Post-processing: depth of field and camera motion blur (when a PostView is given), luminance histogram + auto
// exposure (EV100, read back to drive the next frames' pre-exposure), dual-filter bloom, tonemapping (ACES fit / AgX)
// with grading, FXAA and upscale to the output.

#include "Core/Math/Math.h"
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
    Vec3 colorFilter = Vec3(1.0f, 1.0f, 1.0f); // multiplies the image before tonemapping
    u32 colorLut = rhi::InvalidDescriptor;     // 256 x 16 color grading LUT (UE layout: 16 slices of 16 x 16), SRV
    f32 colorLutIntensity = 0.0f;              // 0..1 blend with the ungraded image
    bool fxaa = true;
    // Depth of field: physical camera (24 mm sensor height, focal length from the field of view).
    bool depthOfField = false;
    f32 focusDistance = 5.0f; // meters
    f32 fStop = 2.8f;         // aperture: smaller = shallower focus
    f32 maxBlur = 16.0f;      // largest blur radius, render-resolution pixels
    f32 dofQuality = 1.0f;    // spiral step (half-res pixels): larger = fewer samples
    // Camera motion blur (velocity from depth; moving objects get it with per-object velocity later).
    bool motionBlur = false;
    f32 motionBlurAmount = 0.5f; // shutter: fraction of the frame's motion
    f32 motionBlurMax = 32.0f;   // pixels
    u32 motionBlurSamples = 8;
    /// 0 final, 1 HDR without tonemapping, 2 depth, 3 luminance heatmap (EV), 4 bloom only, 5 depth of field focus.
    u32 viewMode = 0;
};

/// Camera of the frame (depth of field, motion blur). Matrices without TAA jitter.
struct PostView
{
    Mat4 viewProj = Mat4::Identity();
    f32 fovY = 1.0f; // radians
    f32 nearPlane = 0.05f;
    bool cut = false; // camera cut: no motion blur this frame
};

/// Signed circle of confusion in pixels for an image `imageHeight` pixels high (< 0 in front of the focus).
f32 CircleOfConfusion(f32 depthMeters, f32 focusDistance, f32 fStop, f32 fovY, f32 imageHeight);

class PostProcess
{
public:
    explicit PostProcess(rhi::Device& device);
    ~PostProcess();
    ZX_NON_COPYABLE(PostProcess)

    bool Init(rhi::Format outputFormat);

    /// sceneColor: linear HDR pre-exposed with `exposureEV100`. Writes the final image into `output`.
    /// With `view` (null = none) depth of field and motion blur run first, when enabled in the settings.
    void AddPasses(RenderGraph& graph, RGTexture sceneColor, RGTexture sceneDepth, RGTexture output, f32 exposureEV100,
                   f32 deltaTime, const PostSettings& settings, const PostView* view = nullptr);

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
        u32 lut;           // color grading LUT (SRV) or invalid
        Vec3 colorFilter;
        f32 lutIntensity;
    };
    static_assert(sizeof(Params) == 112);
    struct DofParams
    {
        Mat4 invViewProj;
        Mat4 prevViewProj;
        u32 color;
        u32 depth;
        u32 input;
        u32 output;
        Vec2 size;
        Vec2 invSize;
        Vec2 fullInvSize;
        f32 nearZ;
        f32 cocScale;
        f32 focusDistance;
        f32 maxCoc;
        f32 radiusStep;
        f32 motionScale;
        f32 motionMax;
        u32 samples;
        u32 viewMode;
        f32 pad;
    };
    RGTexture AddDepthOfField(RenderGraph& graph, RGTexture color, RGTexture depth, const DofParams& base);
    RGTexture AddMotionBlur(RenderGraph& graph, RGTexture color, RGTexture depth, const DofParams& base);

    rhi::Device& m_device;
    rhi::ShaderBytecode m_histogramCS, m_exposureCS, m_vs, m_bloomDownPS, m_bloomUpPS, m_tonemapPS, m_fxaaPS;
    rhi::Pipeline* m_histogramPipeline = nullptr;
    rhi::Pipeline* m_exposurePipeline = nullptr;
    rhi::Pipeline* m_bloomDown = nullptr;
    rhi::Pipeline* m_bloomUp = nullptr; // additive
    rhi::Pipeline* m_tonemap = nullptr;
    rhi::Pipeline* m_fxaa = nullptr;
    rhi::Pipeline* m_tonemapToOutput = nullptr;
    rhi::ShaderBytecode m_dofPrepareCS, m_dofBlurCS, m_dofCompositeCS, m_motionBlurCS;
    rhi::Pipeline* m_dofPrepare = nullptr;
    rhi::Pipeline* m_dofBlur = nullptr;
    rhi::Pipeline* m_dofComposite = nullptr;
    rhi::Pipeline* m_motionBlur = nullptr;
    Mat4 m_prevViewProj = Mat4::Identity();
    bool m_hasPrevView = false;
    rhi::Buffer* m_histogram = nullptr; // 64 bins + adapted EV
    rhi::Buffer* m_readback[rhi::FramesInFlight] = {};
    bool m_readbackValid[rhi::FramesInFlight] = {};
    u64 m_frame = 0;
    f32 m_autoEV = 0.0f;
    bool m_hasAutoEV = false;
};

} // namespace zx::render
