#pragma once

// Physical sky (Hillaire 2020): transmittance + multiple-scattering LUTs (computed once), sky-view LUT (per frame),
// sky and sun disk drawn behind the scene. The same atmosphere model runs on the CPU to color the sun light that
// reaches the ground (sunsets turn the direct light orange).
// HDRI sky: a lat-long HDR panorama replaces the atmosphere - drawn behind the scene at full resolution and resampled
// into the sky-view LUT (sun clamped), so the sky lighting (SH), fog, water and reflections use it unchanged.

#include "Render/RenderGraph.h"
#include "Render/SceneRenderer.h"

namespace zx::render {

namespace atmosphere {
/// Transmittance from a point `altitudeKm` above the ground towards `dirToSun` (Earth-like atmosphere).
Vec3 Transmittance(Vec3 dirToSun, f32 altitudeKm = 0.0f);
/// Direction to the sun from elevation/azimuth in radians (azimuth 0 = +Z, 90 deg = +X).
Vec3 SunDirection(f32 elevation, f32 azimuth);
} // namespace atmosphere

struct FogSettings
{
    bool enabled = true;
    f32 density = 0.0015f;   // extinction (1/m) at baseHeight
    f32 falloff = 0.06f;     // height falloff (1/m)
    f32 baseHeight = 0.0f;
    f32 maxDistance = 3000.0f;
    Vec3 albedo = Vec3(0.9f, 0.95f, 1.0f);
    f32 sunAnisotropy = 0.6f; // Henyey-Greenstein g
};

/// HDR panorama replacing the physical sky (see HdriSky in Engine/SceneRendering.h).
struct HdriSkySettings
{
    u32 texture = rhi::InvalidDescriptor; // SRV of the lat-long texture; invalid = physical sky
    u32 width = 0;                        // texture width in texels (mip selection)
    f32 intensity = 1.0f;                 // luminance (cd/m2) of a texel value of 1
    f32 rotation = 0.0f;                  // radians around +Y (panorama +Z turns towards +X)
    Vec3 clampDirection = Vec3(0, 1, 0);  // world direction of the sun cone the sky lighting clamps
    f32 clampCos = 2.0f;                  // cos of the cone radius (> 1 = no clamp)
    f32 clampLuminance = 0.0f;            // texel luminance allowed inside the cone
    u64 key = 0;                          // must change whenever anything above changes

    bool IsActive() const { return texture != rhi::InvalidDescriptor; }
};

/// Panorama direction (asset::LatLongDirection) -> world for a sky rotated by `rotation` radians around +Y.
Vec3 HdriToWorld(Vec3 panoramaDirection, f32 rotation);

struct SkySettings
{
    f32 sunIlluminanceTop = 120000.0f; // lux outside the atmosphere
    Vec3 sunColor = Vec3(1, 1, 1);
    f32 sunAngularRadius = 0.0093f;     // radians (2x the real sun, reads better on screen)
    f32 cameraAltitude = 0.2f;          // km above the ground at world y = 0
    FogSettings fog;
    HdriSkySettings hdri;
};

class SkyRenderer
{
public:
    explicit SkyRenderer(rhi::Device& device);
    ~SkyRenderer();
    ZX_NON_COPYABLE(SkyRenderer)

    bool Init(rhi::Format colorFormat, rhi::Format depthFormat);

    /// Fills `lighting.sunIlluminance/sunColor` with the light that reaches the ground for a sun shining from
    /// `dirToSun` (lighting.sunDirection is set to -dirToSun).
    static void ApplySun(SceneLighting& lighting, Vec3 dirToSun, const SkySettings& settings);

    /// LUT updates and the sky irradiance (SH9) - call before the scene passes; fills lighting.skyIrradiance*.
    void AddPrePasses(RenderGraph& graph, const RenderView& view, Vec3 dirToSun, f32 exposure, const SkySettings& settings,
                      SceneLighting& lighting);
    /// Sky + sun disk where the depth is still cleared. Call after the opaque passes.
    void AddSkyPass(RenderGraph& graph, RGTexture color, RGTexture depth);
    /// Exponential height fog over the scene and the sky (after AddSkyPass). Uses the sun/sky of `lighting`.
    void AddFogPass(RenderGraph& graph, RGTexture color, RGTexture depth, const SceneLighting& lighting, const SkySettings& settings);

    /// Sky-view LUT (lat-long, luminance per unit sun illuminance) for image-based lighting.
    rhi::Texture* GetSkyViewLut() const { return m_skyView; }
    /// This frame's sky-view LUT in the graph (valid after AddPrePasses) and the factor turning its values into
    /// pre-exposed radiance (sun color x illuminance x exposure); used by reflections.
    RGTexture SkyViewTexture() const { return m_rgSkyView; }
    Vec3 SkyRadianceScale() const { return m_radianceScale * m_params.exposure; }
    /// The HDRI sky of the last AddPrePasses (inactive = physical sky).
    const HdriSkySettings& Hdri() const { return m_hdri; }

private:
    struct Params
    {
        Mat4 invViewProj;
        Vec3 cameraPosition;
        f32 exposure;
        Vec3 sunDirection;
        f32 sunIlluminance;
        Vec3 sunColor;
        f32 sunAngularRadius;
        u32 transmittanceLut;
        u32 multiScatterLut;
        u32 skyViewLut;
        u32 outputUav;
        f32 cameraAltitude;
        u32 hdri;
        f32 hdriRotation;
        f32 hdriLod;      // background mip
        Vec3 hdriClampDir;
        f32 hdriClampCos;
        f32 hdriClampLum;
        f32 hdriLutLod;   // sky-view LUT resampling mip
        f32 pad[2];
    };

    rhi::Device& m_device;
    rhi::ShaderBytecode m_transmittanceCS, m_multiScatterCS, m_skyViewCS, m_hdriViewCS, m_irradianceCS, m_skyVS, m_skyPS, m_fogPS;
    rhi::Pipeline* m_hdriViewPipeline = nullptr;
    HdriSkySettings m_hdri;
    Vec3 m_radianceScale = Vec3(1, 1, 1); // sky-view LUT value -> radiance (before exposure)
    u64 m_lastHdriKey = 0;
    rhi::Pipeline* m_fogPipeline = nullptr;
    rhi::Pipeline* m_irradiancePipeline = nullptr;
    rhi::Buffer* m_irradianceSH = nullptr; // 9 x float4
    Params m_params = {};
    RGTexture m_rgTransmittance, m_rgSkyView;
    RGBuffer m_rgSH;
    rhi::Pipeline* m_transmittancePipeline = nullptr;
    rhi::Pipeline* m_multiScatterPipeline = nullptr;
    rhi::Pipeline* m_skyViewPipeline = nullptr;
    rhi::Pipeline* m_skyPipeline = nullptr;
    rhi::Texture* m_transmittance = nullptr;
    rhi::Texture* m_multiScatter = nullptr;
    rhi::Texture* m_skyView = nullptr;
    bool m_lutsReady = false;
    bool m_skyValid = false;
    Vec3 m_lastSunDirection;
    f32 m_lastAltitude = 0.0f;
};

} // namespace zx::render
