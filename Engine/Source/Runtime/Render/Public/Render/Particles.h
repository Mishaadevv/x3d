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
    RGTexture AddPass(RenderGraph& graph, RGTexture color, RGTexture depth, const RenderView& view, Span<const GpuParticle> particles,
                 const SceneLighting& lighting, f32 exposure, f32 time, const FogVolumes& fog = {});

private:
    rhi::Device& m_device;
    rhi::ShaderBytecode m_vs, m_ps, m_heatPS, m_distortVS, m_distortPS;
    rhi::Pipeline* m_pipeline = nullptr;
    rhi::Pipeline* m_heatPipeline = nullptr;
    rhi::Pipeline* m_distortPipeline = nullptr;
    rhi::Format m_colorFormat = rhi::Format::R16G16B16A16_FLOAT;
    rhi::Buffer* m_buffers[rhi::FramesInFlight] = {};
    u32 m_count = 0;
};

} // namespace zx::render
