#pragma once

// Volumetric clouds (Shaders/Sky/Clouds.hlsl): a shell of clouds around the planet shaped by a weather map (where and
// which kind: stratus, cumulus, cumulonimbus) and 3D noise, lit by the sun with multiple scattering and by the sky,
// raymarched at reduced resolution with temporal accumulation and composited over the sky and far geometry. A top-down
// map of the sunlight the clouds let through shadows the ground and the volumetric fog (one frame late).
//
//   clouds.AddPasses(graph, sceneColor, sceneDepth, view, viewProjNoJitter, lighting, settings, sky, exposure, time);
//   lighting.cloudShadow = clouds.ShadowMap(); ...   // next frame's scene and fog
//
// The noise volumes are generated on the GPU in slices over the first frames (clouds appear once they are ready).

#include "Render/RenderGraph.h"
#include "Render/SceneRenderer.h"

namespace zx::render {

class SkyRenderer;

struct CloudSettings
{
    bool enabled = false;
    f32 coverage = 0.35f;       // 0 = clear sky, 1 = overcast
    f32 cloudType = 0.5f;       // 0 = flat stratus layers, 0.5 = cumulus heaps, 1 = towering storm clouds
    f32 bottom = 1500.0f;       // m above the ground
    f32 thickness = 3500.0f;    // m
    f32 density = 0.04f;        // extinction (1/m) in the thick of a cloud
    f32 detail = 0.6f;          // edges eroded by fine noise
    f32 scale = 1.0f;           // size of the cloud shapes
    f32 evolution = 2.0f;       // m/s: how fast the shapes churn
    f32 windSpeed = 15.0f;      // m/s
    f32 windDirection = 60.0f;  // degrees around +Y (0 = towards +Z)
    Vec3 albedo = Vec3(1.0f);
    f32 ambient = 1.0f;         // sky light in the clouds
    f32 silverLining = 1.0f;    // light scattered forward around the sun
    f32 shadowStrength = 0.75f; // how dark cloud shadows are on the ground
    f32 lightning = 0.0f;       // 0..1: how often lightning flashes inside the clouds
    u32 seed = 1;               // the weather map's pattern
};

class CloudRenderer
{
public:
    static constexpr u32 ShadowSize = 256;
    static constexpr f32 ShadowExtent = 4000.0f; // m around the camera
    static constexpr u32 ShapeSize = 128, DetailSize = 32, WeatherSize = 512;
    static constexpr u32 ShapeMips = 5, DetailMips = 3;
    static constexpr f32 WeatherExtent = 40000.0f; // m covered by one tile of the weather map

    explicit CloudRenderer(rhi::Device& device);
    ~CloudRenderer();
    ZX_NON_COPYABLE(CloudRenderer)

    /// `lite`: quarter resolution and fewer steps; else half resolution.
    bool Init(rhi::Format colorFormat, bool lite = true);

    /// Raymarches the clouds, blends them with last frame and composites them over the sky (and far geometry) of
    /// `color`. Nothing (and no shadows) when disabled or while the noise is still being generated.
    void AddPasses(RenderGraph& graph, RGTexture color, RGTexture depth, const RenderView& view, const Mat4& viewProjNoJitter,
                   const SceneLighting& lighting, const CloudSettings& settings, const SkyRenderer* sky, f32 exposure, f32 time);

    /// Refreshes the cloud shadow map for the next frame. Call last in the frame: this frame's scene and fog read the
    /// map without declaring it (it must not be in the write state while they run).
    void AddShadowPass(RenderGraph& graph);

    /// Cloud shadows written by the previous frame (SRV, r = fraction of sunlight reaching the ground), the world xz of
    /// the map's corner and its size in meters; invalid when clouds are off.
    u32 ShadowMap() const { return m_shadowValid ? m_shadow->GetSrvIndex() : rhi::InvalidDescriptor; }
    Vec2 ShadowOrigin() const { return m_shadowOrigin; }
    /// The noise volumes are complete (clouds render).
    bool IsReady() const { return m_shapeSlices >= ShapeSize && m_detailReady && m_weatherKey != ~0ull; }
    /// Brightness of the current lightning flash (0 = none) - hosts may flash the scene's light with it.
    f32 LightningFlash() const { return m_flash; }

private:
    void EnsureTargets(UVec2 size);
    void AddMips(rhi::CommandList& cmd, rhi::Texture* texture, u32 size, u32 mips);

    rhi::Device& m_device;
    rhi::ShaderBytecode m_cloudsCS, m_temporalCS, m_shadowCS, m_compositeVS, m_compositePS, m_shapeCS, m_detailCS, m_weatherCS, m_mipCS;
    rhi::Pipeline* m_cloudsPipeline = nullptr;
    rhi::Pipeline* m_temporalPipeline = nullptr;
    rhi::Pipeline* m_shadowPipeline = nullptr;
    rhi::Pipeline* m_compositePipeline = nullptr;
    rhi::Pipeline* m_shapePipeline = nullptr;
    rhi::Pipeline* m_detailPipeline = nullptr;
    rhi::Pipeline* m_weatherPipeline = nullptr;
    rhi::Pipeline* m_mipPipeline = nullptr;
    rhi::Texture* m_shapeNoise = nullptr;
    rhi::Texture* m_detailNoise = nullptr;
    rhi::Texture* m_weather = nullptr;
    rhi::Texture* m_raw = nullptr;           // this frame's raymarch
    rhi::Texture* m_resolved[2] = {};        // temporal ping-pong
    rhi::Texture* m_shadow = nullptr;
    u32 m_shapeSlices = 0;   // shape noise slices generated so far
    u32 m_genFrame = 0;      // frames since generation started (spreads the work)
    bool m_detailReady = false;
    u64 m_weatherKey = ~0ull;
    bool m_lite = true;
    UVec2 m_size;
    u32 m_current = 0;
    u32 m_frame = 0;
    bool m_historyValid = false;
    bool m_shadowValid = false;
    f32 m_flash = 0.0f;
    Mat4 m_prevViewProj = Mat4::Identity();
    Vec2 m_shadowOrigin;
    bool m_shadowPending = false;
    RGTexture m_rgShape, m_rgWeather; // this frame's handles (one import per graph: its state is tracked there)
    u8 m_shadowParams[1024] = {}; // CloudParams of this frame for the shadow pass
};

} // namespace zx::render
