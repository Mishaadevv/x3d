#pragma once

// Volumetric clouds: a raymarched layer of drifting 3D noise lit by the sun and the sky (see Shaders/Sky/Clouds.hlsl),
// drawn over the sky at quarter resolution with temporal accumulation. A top-down map of the sunlight the clouds let
// through shadows the ground and the volumetric fog (one frame late: clouds move slowly).
//
//   clouds.AddPasses(graph, sceneColor, sceneDepth, view, viewProjNoJitter, lighting, settings, exposure, time);
//   lighting.cloudShadow = clouds.ShadowMap(); ...   // next frame's scene and fog

#include "Render/RenderGraph.h"
#include "Render/SceneRenderer.h"

namespace zx::render {

struct CloudSettings
{
    bool enabled = false;
    f32 coverage = 0.35f;       // 0 = clear sky, 1 = overcast
    f32 bottom = 1500.0f;       // m above the ground
    f32 thickness = 2500.0f;    // m
    f32 density = 0.03f;        // extinction (1/m) in the thick of a cloud
    f32 detail = 0.35f;         // edges eroded by fine noise
    f32 windSpeed = 15.0f;      // m/s
    f32 windDirection = 60.0f;  // degrees around +Y (0 = towards +Z)
    Vec3 albedo = Vec3(1.0f);
    f32 shadowStrength = 0.75f; // how dark cloud shadows are on the ground
};

class CloudRenderer
{
public:
    static constexpr u32 ShadowSize = 256;
    static constexpr f32 ShadowExtent = 3000.0f; // m around the camera

    explicit CloudRenderer(rhi::Device& device);
    ~CloudRenderer();
    ZX_NON_COPYABLE(CloudRenderer)

    bool Init(rhi::Format colorFormat);

    /// Raymarches the clouds, blends them with last frame and composites them over the sky pixels of `color`. Nothing
    /// (and no shadows) when disabled.
    void AddPasses(RenderGraph& graph, RGTexture color, RGTexture depth, const RenderView& view, const Mat4& viewProjNoJitter,
                   const SceneLighting& lighting, const CloudSettings& settings, f32 exposure, f32 time);

    /// Refreshes the cloud shadow map for the next frame. Call last in the frame: this frame's scene and fog read the
    /// map without declaring it (it must not be in the write state while they run).
    void AddShadowPass(RenderGraph& graph);

    /// Cloud shadows written by the previous frame (SRV, r = fraction of sunlight reaching the ground), the world xz of
    /// the map's corner and its size in meters; invalid when clouds are off.
    u32 ShadowMap() const { return m_shadowValid ? m_shadow->GetSrvIndex() : rhi::InvalidDescriptor; }
    Vec2 ShadowOrigin() const { return m_shadowOrigin; }

private:
    void EnsureTargets(UVec2 size);

    rhi::Device& m_device;
    rhi::ShaderBytecode m_cloudsCS, m_temporalCS, m_shadowCS, m_compositeVS, m_compositePS;
    rhi::Pipeline* m_cloudsPipeline = nullptr;
    rhi::Pipeline* m_temporalPipeline = nullptr;
    rhi::Pipeline* m_shadowPipeline = nullptr;
    rhi::Pipeline* m_compositePipeline = nullptr;
    rhi::Texture* m_shapeNoise = nullptr;
    rhi::Texture* m_detailNoise = nullptr;
    rhi::Texture* m_raw = nullptr;           // this frame's raymarch (half resolution)
    rhi::Texture* m_resolved[2] = {};        // temporal ping-pong
    rhi::Texture* m_shadow = nullptr;
    UVec2 m_size;
    u32 m_current = 0;
    u32 m_frame = 0;
    bool m_historyValid = false;
    bool m_shadowValid = false;
    Mat4 m_prevViewProj = Mat4::Identity();
    Vec2 m_shadowOrigin;
    bool m_shadowPending = false;
    RGTexture m_rgShape; // this frame's handle of the shape noise (one import per graph: its state is tracked there)
    u8 m_shadowParams[512] = {}; // CloudParams of this frame for the shadow pass
};

} // namespace zx::render
