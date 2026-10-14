#pragma once

// Real-time smoke and fire simulated on the GPU (stable fluids on a 3D grid in a box, see Shaders/Volumetric/Fluid.hlsl)
// and raymarched over the scene: buoyant hot gas rises and curls into vortices, smoke drifts and thins out, hot regions
// glow like flames. Each volume keeps its own grids between frames (keyed by `id`).
//
//   fluids.AddPasses(graph, sceneColor, sceneDepth, view, viewProjNoJitter, lighting, volumes, exposure, dt, time);

#include "Core/Containers/HashMap.h"
#include "Render/RenderGraph.h"
#include "Render/SceneRenderer.h"

namespace zx::render {

struct FluidVolumeDesc
{
    u64 id = 0;                  // stable identity (entity): the simulation persists across frames
    Mat4 world = Mat4::Identity(); // unit box (-0.5 .. 0.5) -> world
    u32 resolution = 48;         // cells along the longest side
    bool simulate = true;        // false: frozen (still drawn)
    // Emitter (in box space, -0.5 .. 0.5).
    Vec3 sourcePosition = Vec3(0.0f, -0.4f, 0.0f);
    f32 sourceRadius = 0.12f;    // fraction of the longest side
    Vec3 sourceVelocity = Vec3(0.0f, 1.0f, 0.0f); // m/s
    f32 smokeRate = 1.0f;        // density added per second
    f32 heat = 1.0f;             // temperature at the source (0 = only smoke)
    f32 noise = 0.6f;            // turbulence of the source
    // Motion.
    f32 buoyancy = 4.0f;         // m/s^2 per unit temperature (hot gas rises)
    f32 weight = 0.3f;           // m/s^2 per unit density (smoke sinks)
    f32 vorticity = 6.0f;        // swirl strength
    f32 smokeFade = 0.25f;       // per second
    f32 coolRate = 1.6f;         // per second
    // Look.
    f32 density = 3.0f;          // extinction (1/m) of density 1
    Vec3 smokeColor = Vec3(0.2f);
    f32 fireIntensity = 2.0f;    // glow of the hottest gas (x 10 000 nits)
    f32 fireThreshold = 0.25f;   // temperature above which gas glows
};

class FluidRenderer
{
public:
    static constexpr u32 MaxVolumes = 4;

    explicit FluidRenderer(rhi::Device& device);
    ~FluidRenderer();
    ZX_NON_COPYABLE(FluidRenderer)

    bool Init(rhi::Format colorFormat);

    /// Steps every volume that simulates (dt seconds, clamped) and draws them all over `color` (depth-tested against
    /// `depth`). Volumes not passed for a while lose their grids.
    void AddPasses(RenderGraph& graph, RGTexture color, RGTexture depth, const RenderView& view, const Mat4& viewProjNoJitter,
                   const SceneLighting& lighting, Span<const FluidVolumeDesc> volumes, f32 exposure, f32 dt, f32 time);

private:
    struct State
    {
        UVec3 grid;
        rhi::Texture* velocity[2] = {};
        rhi::Texture* scalars[2] = {};
        rhi::Texture* pressure[2] = {};
        rhi::Texture* divergence = nullptr;
        rhi::Texture* curl = nullptr;
        u32 current = 0; // index of the up-to-date velocity / scalars
        u64 lastFrame = 0;
    };
    State* StateFor(const FluidVolumeDesc& v, UVec3 grid);
    void Release(State& s);

    rhi::Device& m_device;
    rhi::ShaderBytecode m_curlCS, m_advectVelocityCS, m_divergenceCS, m_jacobiCS, m_projectCS, m_advectScalarsCS, m_vs, m_ps;
    rhi::Pipeline* m_curl = nullptr;
    rhi::Pipeline* m_advectVelocity = nullptr;
    rhi::Pipeline* m_divergence = nullptr;
    rhi::Pipeline* m_jacobi = nullptr;
    rhi::Pipeline* m_project = nullptr;
    rhi::Pipeline* m_advectScalars = nullptr;
    rhi::Pipeline* m_draw = nullptr;
    HashMap<u64, State> m_states;
    u64 m_frame = 0;
};

} // namespace zx::render
