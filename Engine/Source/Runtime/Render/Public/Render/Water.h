#pragma once

// Water: FFT ocean simulation + surface rendering.
//
//   Simulation  per kind of water (wind speed, fetch, choppiness, direction): three cascades of waves (swell, waves,
//               ripples), each a JONSWAP spectrum with directional spreading built on the CPU, evolved and
//               inverse-FFT'd on the GPU every frame into displacement + slope textures with whitecap foam that
//               builds up on breaking crests and fades (Shaders/Water/Ocean.hlsl).
//   Surface     a mesh generated in the vertex shader (rings around the camera for large water, a grid for small),
//               displaced by the cascades and shaded with screen-space reflections over the prefiltered sky, the
//               shadowed sun, light through the crests, absorption, refraction, caustics and foam
//               (Shaders/Water/Water.hlsl).
//
//   water.AddPasses(graph, sceneColor, sceneDepth, view, surfaces, lighting, lightingInputs, sky, time, exposure);
//
// The scene color and depth are copied first (refraction, thickness, reflections), then the water draws over the scene
// with depth writes, so fog, TAA and post-processing treat it like any opaque surface.

#include "Render/RenderGraph.h"
#include "Render/SceneRenderer.h"

namespace zx::render {

class SkyRenderer;

/// One rectangular body of water (local XZ plane, centered on the transform; waves are in world space).
struct WaterSurface
{
    Mat4 world = Mat4::Identity();
    Vec2 size = Vec2(50.0f, 50.0f); // meters
    // Waves: the sea state the wind raises (JONSWAP): speed and the distance it blew over open water.
    f32 windSpeed = 8.0f;       // m/s
    f32 fetch = 50.0f;          // km (pools ~0.02, lakes ~3, oceans 100+)
    f32 windDirection = 0.5f;   // radians around +Y in world space (0 = towards +Z)
    f32 waveAmount = 1.0f;      // height multiplier
    f32 choppiness = 1.0f;      // 0 = round swells, 1+ = sharp crests
    // Look.
    Vec3 shallowColor = Vec3(0.10f, 0.55f, 0.55f);
    Vec3 deepColor = Vec3(0.01f, 0.07f, 0.13f);
    Vec3 scatterColor = Vec3(0.05f, 0.45f, 0.38f); // light through the crests
    f32 clarity = 4.0f;    // meters until the water is mostly opaque
    f32 refraction = 0.04f;
    f32 foam = 1.0f;       // whitecaps on breaking crests
    f32 shoreFoam = 0.8f;  // meters of foam along shores / around objects
    f32 roughness = 0.05f; // sun glint sharpness
    f32 caustics = 1.0f;   // light patterns on what lies below
    bool reflections = true; // screen-space reflections of the scene (else only the sky)
};

class WaterRenderer
{
public:
    explicit WaterRenderer(rhi::Device& device);
    ~WaterRenderer();
    ZX_NON_COPYABLE(WaterRenderer)

    /// `lite`: 64x64 cascades and a coarser mesh.
    bool Init(rhi::Format colorFormat, rhi::Format depthFormat, bool lite);

    /// Simulates and draws `surfaces` into color/depth. `sky` may be null (reflections then use the flat sky color).
    /// `inputs` provides the sun shadow cascades (may be null).
    void AddPasses(RenderGraph& graph, RGTexture color, RGTexture depth, const RenderView& view, Span<const WaterSurface> surfaces,
                   const SceneLighting& lighting, const SceneRenderer::LightingInputs* inputs, const SkyRenderer* sky, f32 time, f32 exposure);

    u32 Resolution() const { return m_n; }
    static constexpr u32 MaxSimulations = 2;
    static constexpr u32 Cascades = 3;

private:
    struct Simulation
    {
        u64 key = 0;
        f32 patchSize[Cascades] = {};
        rhi::Buffer* h0[Cascades] = {};
        rhi::Texture* specA[Cascades] = {};
        rhi::Texture* specB[Cascades] = {};
        rhi::Texture* displacement[Cascades] = {};
        rhi::Texture* derivatives[Cascades] = {};
        f32 lastTime = -1.0f;
        f32 choppiness = 1.0f;
        bool used = false;
    };
    void Build(Simulation& sim, const WaterSurface& s, u64 key);
    void Destroy(Simulation& sim);

    rhi::Device& m_device;
    rhi::ShaderBytecode m_copyCS, m_vs, m_ps, m_evolveCS, m_fftCS, m_assembleCS, m_mipCS, m_bakeCS;
    rhi::Pipeline* m_copyPipeline = nullptr;
    rhi::Pipeline* m_pipeline = nullptr;
    rhi::Pipeline* m_evolve = nullptr;
    rhi::Pipeline* m_fft = nullptr;
    rhi::Pipeline* m_assemble = nullptr;
    rhi::Pipeline* m_mip = nullptr;
    rhi::Pipeline* m_bake = nullptr;
    rhi::Texture* m_causticTexture = nullptr; // baked once: seamless caustic web
    rhi::Texture* m_foamTexture = nullptr;    // and foam bubbles
    bool m_patternsBaked = false;
    bool m_lite = false;
    u32 m_n = 128;
    u32 m_grid = 128;
    u32 m_rings = 96, m_segments = 192;
    Simulation m_sims[MaxSimulations];
    Array<WaterSurface> m_surfaces;
    Array<u32> m_surfaceSim;
};

/// Spectrum key of a surface (surfaces with the same key share one simulation).
u64 WaterSpectrumKey(const WaterSurface& s);

} // namespace zx::render
