#pragma once

// Volumetric fog: light scattered in a 3D grid of view-aligned cells (froxels) covering the first `distance` meters.
// Each cell gets the height fog's density plus local Fog Volumes (boxes / spheres of fog, smoke, steam with drifting
// noise), and is lit by the sun through its cascaded shadows (light shafts, "god rays"), the sky, and every point /
// spot light (fires glow in the smoke around them). The cells are blended with the previous frame (jittered in depth),
// then integrated front to back and applied to the scene; the analytic height fog continues beyond the grid.
//
// The lit grid is also a lighting volume for translucent particles: smoke is lit by the same lights and shadows.

#include "Core/Containers/Array.h"
#include "Render/RenderGraph.h"
#include "Render/SceneRenderer.h"
#include "Render/Sky.h"

namespace zx::render {

struct VolumetricFogSettings
{
    bool enabled = true;
    f32 distance = 80.0f;      // meters covered by the grid (the height fog continues analytically beyond)
    f32 scattering = 1.0f;     // multiplies the height fog's density inside the grid
    f32 localLights = 1.0f;    // how strongly point / spot lights light the fog
    f32 history = 0.9f;        // weight of the previous frame (smooths the depth jitter)
    u32 tileSize = 12;         // pixels per cell (render resolution)
    u32 slices = 64;           // cells along the view
};

enum class FogVolumeShape : u32
{
    Box,    // the unit cube -0.5 .. 0.5 in volume space
    Sphere, // radius 0.5 in volume space
};

/// A local volume of fog or smoke. Density fades out towards the edge, broken up by drifting noise.
struct FogVolumeDesc
{
    Mat4 worldToVolume;          // world -> volume space (unit box / sphere)
    FogVolumeShape shape = FogVolumeShape::Box;
    f32 density = 0.2f;          // extinction (1/m)
    Vec3 albedo = Vec3(0.9f);    // fraction of the light scattered (smoke: dark)
    Vec3 emissive;               // light given off per meter of fog (cd/m2 per m: glowing smoke above a fire)
    f32 edgeFade = 0.3f;         // fraction of the size over which the density fades out
    f32 noiseScale = 0.6f;       // 1/m
    f32 noiseAmount = 0.6f;      // 0 = uniform, 1 = only the noise
    Vec3 wind = Vec3(0.3f, 0.05f, 0.1f); // m/s the noise drifts
};

class VolumetricFogRenderer
{
public:
    static constexpr u32 MaxVolumes = 64;

    explicit VolumetricFogRenderer(rhi::Device& device);
    ~VolumetricFogRenderer();
    ZX_NON_COPYABLE(VolumetricFogRenderer)

    bool Init(rhi::Format colorFormat);

    /// Injects, integrates and applies the fog over `color` (after the opaque scene, water and the far analytic fog).
    /// `viewProjNoJitter` places the cells; the lighting comes from the scene renderer's lights and sun shadows.
    void AddPasses(RenderGraph& graph, RGTexture color, RGTexture depth, const RenderView& view, const Mat4& viewProjNoJitter,
                   const SceneLighting& lighting, const SceneRenderer::LightingInputs& lights, const FogSettings& fog,
                   const VolumetricFogSettings& settings, Span<const FogVolumeDesc> volumes, f32 exposure, f32 time,
                   const SkyRenderer* sky = nullptr);
    /// Drops the history (camera cut, settings change).
    void ResetHistory() { m_historyValid = false; }

    /// For particles this frame (valid after AddPasses when IsActive): the lighting volume (rgb = light scattered
    /// towards the camera per unit of scattering) and the integrated fog (rgb = in-scatter, a = transmittance),
    /// both indexed by (screen uv, sqrt(view depth / distance)).
    bool IsActive() const { return m_active; }
    RGTexture LightingVolume() const { return m_rgLighting; }
    RGTexture IntegratedVolume() const { return m_rgIntegrated; }
    f32 Distance() const { return m_distance; }
    u32 Slices() const { return m_grid.z; }

private:
    void EnsureTextures(UVec3 grid);

    rhi::Device& m_device;
    rhi::ShaderBytecode m_injectCS, m_integrateCS, m_applyVS, m_applyPS;
    rhi::Pipeline* m_injectPipeline = nullptr;
    rhi::Pipeline* m_integratePipeline = nullptr;
    rhi::Pipeline* m_applyPipeline = nullptr;
    rhi::Texture* m_scatter[2] = {}; // ping-pong: this frame / history
    rhi::Texture* m_lighting = nullptr;
    rhi::Texture* m_integrated = nullptr;
    rhi::Buffer* m_volumeBuffers[rhi::FramesInFlight] = {};
    UVec3 m_grid;
    u32 m_current = 0;
    u64 m_frame = 0;
    Mat4 m_prevViewProj = Mat4::Identity();
    Vec3 m_prevPosition, m_prevForward = Vec3(0, 0, 1);
    f32 m_distance = 80.0f;
    bool m_historyValid = false;
    bool m_active = false;
    RGTexture m_rgLighting, m_rgIntegrated;
};

} // namespace zx::render
