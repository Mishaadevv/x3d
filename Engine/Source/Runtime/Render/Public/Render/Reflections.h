#pragma once

// Reflections: specular image-based lighting from the sky (a prefiltered cubemap of the sky-view LUT), reflection
// probes (cubemaps captured in the scene, one face per frame, GGX-prefiltered into the mips, box-projected and blended
// by priority in the forward pass) and screen-space reflections (traced at half resolution after the opaque pass and
// used by the next frame's forward pass over the probes).
//
//   reflections.Prepare(probes, view, exposure, settings, environmentKey, lighting);  // before the scene
//   RenderView face; if (reflections.CaptureView(face)) { ...render the scene from `face` into faceColor/faceDepth... }
//   scene.AddPasses(graph, color, depth, clear, [&](auto& g, auto c, auto d, auto n) { reflections.AddSsrPass(g, c, d, n, view); });
//   reflections.AddUpdatePasses(graph, skyLut, skyScale, faceColor, faceDepth, exposure);  // after the scene

#include "Render/RenderGraph.h"
#include "Render/SceneRenderer.h"

namespace zx::render {

struct ReflectionProbeDesc
{
    u32 id = 0;           // stable identity (entity)
    Vec3 position;        // capture point
    Vec3 boxMin, boxMax;  // influence box (world, axis-aligned); also the parallax box
    f32 blendDistance = 1.0f; // meters inside the box over which the probe fades in
    f32 intensity = 1.0f;
    i32 priority = 0;     // higher first; then smaller boxes first
    bool boxProjection = true;
    u64 key = 0;          // a change triggers a recapture (placement, scene content)
};

struct ReflectionSettings
{
    bool sky = true;             // sky cube for specular ambient (else the SH sky along the reflection)
    bool probes = true;
    u32 probeResolution = 128;   // per cube face
    u32 maxProbes = 16;
    bool ssr = false;            // needs SceneRenderSettings::normalOutput
    bool ssrHalfResolution = true;
    u32 ssrSteps = 32;
    f32 ssrMaxDistance = 40.0f;  // meters
    f32 ssrThickness = 0.25f;    // meters (grows with distance)
    f32 ssrMaxRoughness = 0.6f;
};

class ReflectionRenderer
{
public:
    static constexpr u32 SkyResolution = 64;

    explicit ReflectionRenderer(rhi::Device& device);
    ~ReflectionRenderer();
    ZX_NON_COPYABLE(ReflectionRenderer)

    bool Init();

    /// Updates the probe set (new probes get cubes, changed ones are queued for capture), uploads the probe table and
    /// fills the reflection bindings of `lighting`. `environmentKey` changes when the sun or sky changes (sky cube update
    /// and recapture of every probe).
    void Prepare(Span<const ReflectionProbeDesc> probes, const RenderView& view, f32 exposure, const ReflectionSettings& settings,
                 u64 environmentKey, SceneLighting& lighting);
    /// The cube face to capture this frame (square view, 90 degrees), if a probe is being captured.
    bool CaptureView(RenderView& faceView) const;
    /// Screen-space reflections of this frame (after the opaque pass).
    void AddSsrPass(RenderGraph& graph, RGTexture color, RGTexture depth, RGTexture normals, const RenderView& view);
    /// Sky cube update, the captured face (faceColor/faceDepth, rendered with `exposure`) into the probe, prefiltering.
    /// Call after every pass that reads the cubes. skyLut may be invalid (no sky: the probes see black around them).
    void AddUpdatePasses(RenderGraph& graph, RGTexture skyLut, Vec3 skyScale, RGTexture faceColor, RGTexture faceDepth, f32 exposure);

    /// Recapture every probe (scene content changed).
    void RecaptureAll();
    u32 ProbeCount() const { return u32(m_slots.Size()); }
    u32 CapturedProbes() const;
    bool IsCapturing() const { return m_capture >= 0; }

    /// Exposure the cubes are stored at (EV100 10): keeps sunlit and dark scenes within half-float range.
    static f32 ReferenceExposure();

private:
    struct Slot
    {
        ReflectionProbeDesc desc;
        rhi::Texture* cube = nullptr;
        u64 capturedKey = 0;
        bool valid = false;
        bool seen = false;
    };
    struct Params
    {
        Mat4 invViewProj;
        Mat4 viewProj;
        u32 input;
        u32 depth;
        u32 output;
        u32 normals;
        Vec3 skyScale;
        f32 exposureScale;
        u32 size;
        u32 face;
        f32 roughness;
        u32 samples;
        Vec3 cameraPosition;
        f32 nearZ;
        Vec2 targetSize;
        Vec2 sceneSize;
        u32 steps;
        f32 maxDistance;
        f32 thickness;
        f32 maxRoughness;
        u32 frame;
        u32 sky;
        Vec2 pad;
        Vec3 cameraForward;
        f32 pad2;
    };
    static_assert(sizeof(Params) == 256);

    rhi::Texture* CreateCube(u32 size, u32 mips, StringView name);
    void AddPrefilterPasses(RenderGraph& graph, RGTexture source, RGTexture target, rhi::Texture* targetTexture, StringView name);
    static u32 MipCount(u32 size); // down to 4 x 4

    rhi::Device& m_device;
    rhi::ShaderBytecode m_skyToCubeCS, m_copyFaceCS, m_prefilterCS, m_ssrCS;
    rhi::Pipeline* m_skyToCube = nullptr;
    rhi::Pipeline* m_copyFace = nullptr;
    rhi::Pipeline* m_prefilter = nullptr;
    rhi::Pipeline* m_ssr = nullptr;
    ReflectionSettings m_settings;
    // Sky.
    rhi::Texture* m_skyRaw = nullptr;
    rhi::Texture* m_skyCube = nullptr;
    bool m_skyValid = false;
    bool m_skyDirty = true;
    u64 m_environmentKey = 0;
    // Probes.
    Array<Slot> m_slots;
    rhi::Texture* m_captureRaw = nullptr; // shared by the capture in progress
    u32 m_captureSize = 0;
    i32 m_capture = -1;     // slot being captured
    u32 m_captureFace = 0;
    u64 m_captureKey = 0;
    rhi::Buffer* m_probeBuffers[rhi::FramesInFlight] = {};
    // SSR.
    rhi::Texture* m_ssrTextures[2] = {};
    UVec2 m_ssrSize;
    u32 m_ssrWrite = 0;
    bool m_ssrValid = false;
    Mat4 m_ssrViewProj = Mat4::Identity();
    u32 m_frame = 0;
};

} // namespace zx::render
