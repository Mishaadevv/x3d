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

enum class ParticleShape : u32
{
    Soft,  // round puff (smoke, dust, fog)
    Flame, // flickering teardrop
    Spark, // bright core, stretched along the velocity
};

/// One particle as the GPU reads it.
struct GpuParticle
{
    Vec3 position;
    f32 size = 1.0f;      // quad width (m)
    Vec4 color;           // linear rgb (emissive: multiplied by the intensity reference), a = opacity
    f32 rotation = 0.0f;  // radians
    f32 emissive = 0.0f;  // 0 = lit by sun and sky, 1 = glows
    f32 additive = 0.0f;  // 0 = covers what is behind (alpha), 1 = adds light
    ParticleShape shape = ParticleShape::Soft;
    Vec3 velocity;        // m/s (sparks stretch along it)
    f32 seed = 0.0f;      // 0..1, varies the procedural shape
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

    /// Draws `particles` (back to front) over `color`, depth-tested against `depth` in the shader.
    void AddPass(RenderGraph& graph, RGTexture color, RGTexture depth, const RenderView& view, Span<const GpuParticle> particles,
                 const SceneLighting& lighting, f32 exposure, f32 time);

private:
    rhi::Device& m_device;
    rhi::ShaderBytecode m_vs, m_ps;
    rhi::Pipeline* m_pipeline = nullptr;
    rhi::Buffer* m_buffers[rhi::FramesInFlight] = {};
    u32 m_count = 0;
};

} // namespace zx::render
