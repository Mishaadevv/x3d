#pragma once

// Water surfaces: a vertex-shader grid displaced by Gerstner waves, shaded with sky reflection (Fresnel), sun glints,
// screen-space refraction, depth-based absorption and scattering, and foam on wave crests and along shores.
//
//   water.AddPasses(graph, sceneColor, sceneDepth, view, surfaces, lighting, sky);   // after the sky, before the fog
//
// The scene color and depth are copied first (refraction / thickness), then the water draws over the scene with
// depth writes, so fog, TAA and post-processing treat it like any opaque surface.

#include "Render/RenderGraph.h"
#include "Render/SceneRenderer.h"

namespace zx::render {

class SkyRenderer;

/// One rectangular body of water (local XZ plane, centered on the transform; waves move along world space).
struct WaterSurface
{
    Mat4 world = Mat4::Identity();
    Vec2 size = Vec2(50.0f, 50.0f); // meters
    f32 waveHeight = 0.35f;         // amplitude of the largest wave (m)
    f32 waveLength = 8.0f;          // wavelength of the largest wave (m)
    f32 waveSpeed = 1.0f;           // multiplier of the physical (deep water) speed
    f32 windDirection = 0.5f;       // radians around +Y (0 = +X)
    f32 choppiness = 0.6f;          // 0 = round sine waves, 1 = sharp crests
    Vec3 shallowColor = Vec3(0.10f, 0.55f, 0.55f);
    Vec3 deepColor = Vec3(0.01f, 0.07f, 0.13f);
    f32 clarity = 4.0f;    // meters until the water is mostly opaque
    f32 refraction = 0.04f; // screen-space distortion strength
    f32 foam = 1.0f;        // foam amount (0 = none)
    f32 shoreFoam = 0.8f;   // meters of foam along shores / around objects
    f32 roughness = 0.04f;  // sun glint sharpness
};

class WaterRenderer
{
public:
    explicit WaterRenderer(rhi::Device& device);
    ~WaterRenderer();
    ZX_NON_COPYABLE(WaterRenderer)

    /// `lite` uses a coarser grid.
    bool Init(rhi::Format colorFormat, rhi::Format depthFormat, bool lite);

    /// Draws `surfaces` into color/depth. `sky` may be null (reflections then use the flat sky color of `lighting`).
    void AddPasses(RenderGraph& graph, RGTexture color, RGTexture depth, const RenderView& view, Span<const WaterSurface> surfaces,
                   const SceneLighting& lighting, const SkyRenderer* sky, f32 time, f32 exposure);

    u32 GridResolution() const { return m_grid; }

private:
    rhi::Device& m_device;
    rhi::ShaderBytecode m_copyCS, m_vs, m_ps;
    rhi::Pipeline* m_copyPipeline = nullptr;
    rhi::Pipeline* m_pipeline = nullptr;
    u32 m_grid = 128;
    Array<WaterSurface> m_surfaces;
};

} // namespace zx::render
