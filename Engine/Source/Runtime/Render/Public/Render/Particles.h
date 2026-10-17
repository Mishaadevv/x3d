#pragma once

// Particles: camera-facing quads pulled from an upload buffer (64 bytes each, sorted back to front by the caller),
// procedural shapes (soft puff, flame, spark - no textures), premultiplied blending so additive (glowing) and alpha
// (smoke) particles share one draw, soft edges where they meet geometry (scene depth read as a texture), lit by the
// sun and sky unless emissive.
//
//   particles.AddPass(graph, sceneColor, sceneDepth, view, sortedParticles, lighting, time);   // after the fog

#include "Render/RenderGraph.h"
#include "Render/SceneRenderer.h"

namespace zx::render {

enum ParticleFlags : u32
{
    ParticleEmissive = 1, // glows (not lit by the sun and sky)
    ParticleAdditive = 2, // adds light instead of covering what is behind
};

/// Packs a shape and a flipbook grid into GpuParticle::shape.
constexpr u32 PackParticleShape(u32 shape, u32 columns = 0, u32 rows = 0) { return shape | (columns << 8) | (rows << 16); }

enum class ParticleShape : u32
{
    Soft,  // round puff (smoke, dust, fog)
    Flame, // flickering teardrop
    Spark, // bright core, stretched along the velocity
    Heat,  // invisible: bends the light behind it (heat haze over fires)
};

/// One particle as the GPU reads it.
struct GpuParticle
{
    Vec3 position;
    f32 size = 1.0f;      // quad width (m)
    Vec4 color;           // linear rgb (emissive: multiplied by the intensity reference), a = opacity
    f32 rotation = 0.0f;  // radians
    u32 flags = 0;        // ParticleFlags
    u32 texture = 0xFFFFFFFFu; // flipbook atlas (SRV) or invalid: procedural shape
    u32 shape = 0;        // ParticleShape | flipbook columns << 8 | rows << 16
    Vec3 velocity;        // m/s (sparks stretch along it)
    f32 seed = 0.0f;      // 0..1: varies the procedural shape; flipbooks: position in the animation
};
static_assert(sizeof(GpuParticle) == 64);

/// GPU-simulated emitter (Shaders/Particles/ParticleSim.hlsl), one row per emitter slot. Particles remember the slot and
/// its generation: a row that is inactive or has another generation kills them.
struct GpuEmitter
{
    enum Flags : u32
    {
        Active = 1,
        WorldSpace = 2,
        Bounce = 4, // collide with the depth buffer and bounce
        Die = 8,    // collide with the depth buffer and disappear
    };
    Mat4 world;
    Vec3 acceleration;
    f32 drag = 0.0f;
    Vec3 startColor; // linear rgb, emissive intensity applied
    f32 startAlpha = 1.0f;
    Vec3 endColor;
    f32 endAlpha = 0.0f;
    Vec3 boxSize;
    f32 radius = 0.0f;
    f32 startSize = 0.0f, endSize = 0.0f, sizeVariation = 0.0f, spin = 0.0f;
    f32 speed = 0.0f, speedVariation = 0.0f, lifetime = 1.0f, lifetimeVariation = 0.0f;
    f32 coneAngle = 0.0f, turbulence = 0.0f, fadeIn = 0.0f, bounciness = 0.0f;
    f32 dt = 0.0f;   // seconds to advance this tick (0: frozen)
    f32 time = 0.0f; // emitter time (turbulence)
    f32 flipbookFps = 0.0f, flipbookFrames = 1.0f;
    u32 spawnCount = 0;  // new particles this tick
    u32 spawnOffset = 0; // filled by the renderer
    u32 seed = 0;        // varies per tick
    u32 generation = 0;
    u32 emitShape = 0;   // EmitterShape: point, sphere, box, cone
    u32 packedShape = 0; // GpuParticle::shape
    u32 renderFlags = 0; // ParticleFlags
    u32 texture = 0xFFFFFFFFu;
    u32 flags = 0;
    u32 pad[7] = {};
};
static_assert(sizeof(GpuEmitter) == 256 && offsetof(GpuEmitter, spawnCount) == 192 && offsetof(GpuEmitter, flags) == 224);

/// GPU particles of one frame.
struct GpuParticleFrame
{
    Span<const GpuEmitter> emitters; // indexed by slot
    u64 tick = 0;                    // simulation tick: a tick already simulated only draws
    u64 resetGeneration = 0;         // a change empties the pool
};

class ParticleRenderer
{
public:
    static constexpr u32 MaxParticles = 65536;
    static constexpr f32 EmissiveNits = 4000.0f; // luminance of an emissive particle with color 1 (cd/m^2)

    explicit ParticleRenderer(rhi::Device& device);
    ~ParticleRenderer();
    ZX_NON_COPYABLE(ParticleRenderer)

    bool Init(rhi::Format colorFormat);

    /// Volumetric fog of this frame: smoke is lit by its lighting volume (sun shadows, point lights) and fogged.
    struct FogVolumes
    {
        RGTexture lighting, integrated; // invalid: no volumetric fog
        f32 distance = 0.0f;
        u32 slices = 64;
    };
    /// Draws `particles` (back to front) over `color`, depth-tested against `depth` in the shader. Heat haze particles
    /// distort the scene: the result is then a new texture - use the returned one from here on.
    /// GPU-simulated particles (`gpu`) are simulated first and drawn before the CPU ones (unsorted).
    RGTexture AddPass(RenderGraph& graph, RGTexture color, RGTexture depth, const RenderView& view, Span<const GpuParticle> particles,
                 const SceneLighting& lighting, f32 exposure, f32 time, const FogVolumes& fog = {}, const GpuParticleFrame* gpu = nullptr);

    static constexpr u32 GpuCapacity = 262144; // pool of GPU-simulated particles
    static constexpr u32 MaxGpuEmitters = 256;

private:
    rhi::Device& m_device;
    rhi::ShaderBytecode m_vs, m_ps, m_heatPS, m_distortVS, m_distortPS;
    rhi::Pipeline* m_pipeline = nullptr;
    rhi::Pipeline* m_heatPipeline = nullptr;
    rhi::Pipeline* m_distortPipeline = nullptr;
    rhi::Format m_colorFormat = rhi::Format::R16G16B16A16_FLOAT;
    rhi::Buffer* m_buffers[rhi::FramesInFlight] = {};
    u32 m_count = 0;
    // GPU simulation.
    bool EnsureGpuSimulation();
    rhi::ShaderBytecode m_initCS, m_emitCS, m_simulateCS, m_argsCS;
    rhi::Pipeline* m_initPipeline = nullptr;
    rhi::Pipeline* m_emitPipeline = nullptr;
    rhi::Pipeline* m_simulatePipeline = nullptr;
    rhi::Pipeline* m_argsPipeline = nullptr;
    rhi::Buffer* m_simState[2] = {}; // used in turn: survivors of one are compacted into the other
    rhi::Buffer* m_simCounters = nullptr;
    rhi::Buffer* m_simRender = nullptr;
    rhi::Buffer* m_simArgs = nullptr;
    rhi::Buffer* m_simEmitters = nullptr; // GPU copy of this tick's emitter rows
    rhi::Buffer* m_emitterBuffers[rhi::FramesInFlight] = {};
    bool m_gpuSimFailed = false;
    bool m_gpuNeedsInit = true;
    bool m_gpuHasParticles = false;
    u32 m_parity = 0;
    u64 m_lastTick = ~0ull;
    u64 m_resetGeneration = ~0ull;
};

} // namespace zx::render
