#pragma once

// Dynamic global illumination (M17, "GI-1 DDGI-lite"): light bounces off walls, floors and objects in real time.
//
//   1. Global distance field: the static scene is voxelized on the GPU into nested 128^3 levels around the camera
//      (finest 0.25 m), with the color and normal of the surfaces; jump flooding turns the voxels into distances.
//      A level is rebuilt when the camera leaves its center region or the static scene changes (rate limited);
//      moving and skinned objects are left out.
//   2. Probes (DDGI): a grid around the camera; each probe shoots rays through the distance field, lights what they
//      hit (sun with soft SDF shadows, point / spot lights, the probes themselves = more bounces) or sees the sky,
//      and keeps octahedral maps of irradiance and visibility, blended over frames.
//   3. Shading (ForwardPBR LightSurface): the diffuse ambient comes from the 8 probes around a pixel instead of the
//      open sky - interiors get dark corners and color bleeding, sunlit floors light the ceilings.
//
//   gi.Prepare(instances, view, settings, lighting);  // before SceneRenderer::Prepare: sets lighting.gi
//   gi.AddPasses(graph, lighting, ...);               // after the scene: rebuilds / traces / updates (used next frame)
//
// Baked GI ("GI-0"): StartBake runs the same probes over a fixed grid around the whole static scene - every probe,
// 256 rays, an exact running average over many iterations - and reads the result back (TakeBake). SetBaked loads it;
// in GIMode::Baked the surfaces then take their diffuse ambient from those probes with no tracing at all (moving
// objects too: they are the light probes for dynamic objects).

#include "Core/Containers/Array.h"
#include "Render/RenderGraph.h"
#include "Render/SceneRenderer.h"
#include "Render/Sky.h"

namespace zx::render {

enum class GIMode : u8
{
    Dynamic, // probes traced every frame around the camera
    Baked,   // the scene's baked probes (no GPU time); falls back to Dynamic when the scene has none
};

struct GISettings
{
    bool enabled = true;
    GIMode mode = GIMode::Dynamic;
    f32 voxelSize = 0.25f;       // finest distance field level (meters per voxel)
    u32 levels = 3;              // nested levels, each twice as coarse (1..4)
    UVec3 probeCounts = UVec3(24, 12, 24);
    f32 probeSpacing = 1.5f;     // meters
    u32 raysPerProbe = 64;       // 32..256 (multiples of 64 are fastest)
    u32 probeStride = 2;         // one of every N probes is updated per frame
    f32 hysteresis = 0.97f;      // weight of the history per update (higher: smoother, slower)
    f32 intensity = 1.0f;        // multiplies the indirect diffuse light
    f32 bounce = 0.95f;          // fraction of the bounced light carried into the next bounce
    f32 maxRayDistance = 40.0f;  // meters
    f32 sunShadowDistance = 60.0f; // meters a hit point looks towards the sun for occluders
    // Screen-space GI (needs the normal + roughness target: deferred path or SSR).
    bool screenSpace = false;
    u32 screenRays = 4;          // per half-resolution pixel and frame
    f32 screenDistance = 2.5f;   // meters a screen ray marches
    f32 screenHistory = 0.88f;   // weight of last frame's result
};

/// A finished bake: probes over a fixed grid (pre-exposed with `exposure`), saved next to the scene as .zgi.
struct BakedGI
{
    IVec3 originIndex;
    UVec3 counts;
    f32 spacing = 1.0f;
    f32 maxDistance = 40.0f;
    f32 exposure = 1.0f;
    Array<u8> irradiance; // RGBA16F, (counts.x * counts.y * 8) x (counts.z * 8), tightly packed rows
    Array<u8> distance;   // RG16F, 16 x 16 per probe
    Array<u8> probeData;  // RGBA16F, one texel per probe (relocation offset)
    bool IsValid() const;
};
bool SaveBakedGI(StringView path, const BakedGI& data);
bool LoadBakedGI(StringView path, BakedGI& data);

struct GIBakeSettings
{
    f32 spacing = 1.0f;      // meters (grows when the scene would need more than maxProbes)
    u32 maxProbes = 24000;
    u32 raysPerProbe = 256;
    u32 iterations = 48;     // traced updates averaged together
};

class GlobalIllumination
{
public:
    static constexpr u32 SdfResolution = 128;
    static constexpr u32 MaxLevels = 4;

    GlobalIllumination(rhi::Device& device, RenderAssets& assets);
    ~GlobalIllumination();
    ZX_NON_COPYABLE(GlobalIllumination)

    bool Init();

    /// Places the levels and the probe grid around the camera and decides what to rebuild this frame. Sets
    /// lighting.gi to the probes (as last updated) when they are ready; otherwise leaves it off.
    void Prepare(Span<const RenderInstance> instances, Vec3 cameraPosition, f32 exposure, const GISettings& settings,
                 SceneLighting& lighting);
    /// Rebuilds a distance field level if needed, traces the probes' rays and updates them. `lightBuffer` / `lightCount`:
    /// this frame's point / spot lights (SceneRenderer::PreparedLightBuffer), `sky`: the sky's irradiance SH (or null).
    /// Screen-space GI after the opaque pass (`normals`: octahedral normal + roughness; invalid = skipped). Read by the
    /// lighting of the next frame (Prepare sets lighting.ssgiTexture).
    void AddScreenPass(RenderGraph& graph, RGTexture color, RGTexture depth, RGTexture normals, const RenderView& view);
    /// `inputs`: the scene's sun cascades of this frame (hits inside them use the shadow maps).
    void AddPasses(RenderGraph& graph, const SceneLighting& lighting, const SceneRenderer::LightingInputs& inputs, u32 lightBuffer,
                   u32 lightCount, const SkyRenderer* sky);
    /// Forgets the probes and the distance field (scene switch).
    void Reset();

    // --- Baking ---
    /// Starts baking the static scene of the next frames (the grid is fitted to it on the next Prepare).
    void StartBake(const GIBakeSettings& settings);
    void CancelBake();
    bool IsBaking() const { return m_bake.state != BakeState::Idle && m_bake.state != BakeState::Done; }
    f32 BakeProgress() const;
    /// Moves a finished bake out (once); false while none is ready.
    bool TakeBake(BakedGI& out);
    /// Probes used in GIMode::Baked (null = none). Uploaded on the next AddPasses.
    void SetBaked(const BakedGI* data);
    bool HasBaked() const { return m_bakedValid || m_bakedPending; }

    struct Stats
    {
        bool active = false;
        u32 probes = 0;
        u32 probesUpdated = 0; // this frame
        u32 rays = 0;          // this frame
        u32 levelRebuilt = ~0u; // this frame (~0 = none)
        u32 voxelizedSections = 0;
        u64 voxelizedTriangles = 0;
        f32 finestVoxel = 0.0f;
        bool baked = false;     // lighting from the baked probes
    };
    const Stats& GetStats() const { return m_stats; }

private:
    struct Level
    {
        Vec3 min;         // world corner of the built field
        bool built = false;
        u64 contentKey = 0;
        u64 builtFrame = 0;
    };
    struct VoxelDraw
    {
        Vec4 rows[3];
        u32 vertexBuffer, indexBuffer, firstIndex, triangleCount, index16, material;
    };

    enum class BakeState : u8
    {
        Idle,
        Pending,  // fit the grid on the next Prepare
        Running,  // iterations
        Readback, // copies issued, waiting for the GPU
        Done,
    };
    struct Bake
    {
        BakeState state = BakeState::Idle;
        GIBakeSettings settings;
        Vec3 center;
        IVec3 origin;
        UVec3 counts;
        f32 spacing = 1.0f;
        u32 iteration = 0;
        u64 copyFrame = 0;
        u64 pitches[3] = {};
        rhi::Buffer* readback[3] = {};
        BakedGI result;
    };

    void EnsureResources(const GISettings& settings);
    void AddBakeReadback(RenderGraph& graph);
    void FinishBakeReadback();
    void DestroyResources();
    void AddLevelBuild(RenderGraph& graph, u32 level);

    rhi::Device& m_device;
    RenderAssets& m_assets;
    rhi::ShaderBytecode m_clearCS, m_voxelizeCS, m_jumpFloodCS, m_finalizeCS, m_traceCS, m_irradianceCS, m_distanceCS, m_cubeCS;
    rhi::Pipeline* m_clearPipeline = nullptr;
    rhi::Pipeline* m_voxelizePipeline = nullptr;
    rhi::Pipeline* m_jumpFloodPipeline = nullptr;
    rhi::Pipeline* m_finalizePipeline = nullptr;
    rhi::Pipeline* m_tracePipeline = nullptr;
    rhi::Pipeline* m_irradiancePipeline = nullptr;
    rhi::Pipeline* m_distancePipeline = nullptr;
    rhi::Pipeline* m_cubePipeline = nullptr;
    rhi::ShaderBytecode m_ssgiTraceCS, m_ssgiTemporalCS;
    rhi::Pipeline* m_ssgiTracePipeline = nullptr;
    rhi::Pipeline* m_ssgiTemporalPipeline = nullptr;
    rhi::Texture* m_ssgiTrace = nullptr;
    rhi::Texture* m_ssgi[2] = {};   // accumulated results: [m_ssgiRead] = last frame's
    UVec2 m_ssgiSize;
    u32 m_ssgiRead = 0;
    bool m_ssgiValid = false;
    bool m_ssgiEnabled = false;     // this frame's settings
    Mat4 m_ssgiViewProj = Mat4::Identity();
    u64 m_ssgiFrame = 0;
    // Distance field: three textures with the levels stacked along z, two seed buffers for jump flooding.
    rhi::Texture* m_distance = nullptr;
    rhi::Texture* m_albedo = nullptr;
    rhi::Texture* m_normal = nullptr;
    rhi::Buffer* m_seeds[2] = {};
    // Probes.
    rhi::Texture* m_irradiance = nullptr;
    rhi::Texture* m_probeDistance = nullptr;
    rhi::Buffer* m_rays = nullptr;
    rhi::Buffer* m_flags = nullptr; // per probe: traced this frame (classification)
    rhi::Texture* m_probeData = nullptr; // per slot: offset from the grid point (relocation)
    rhi::Texture* m_cubes[6] = {}; // ambient cube per probe slot: +X -X +Y -Y +Z -Z (bounces of the rays)
    UVec3 m_probeCounts;
    u32 m_levelCount = 0;
    u32 m_rayCount = 0;
    f32 m_voxel = 0.0f;
    Level m_levels[MaxLevels];
    // This frame.
    GISettings m_settings;
    bool m_active = false;
    bool m_probesValid = false; // the probe maps hold data (shading may read them)
    u32 m_rebuild = ~0u;        // level to rebuild this frame
    Vec3 m_rebuildMin;
    u64 m_rebuildKey = 0;
    Array<VoxelDraw> m_voxelDraws;
    IVec3 m_originIndex;      // this frame's grid
    IVec3 m_updatedOrigin;    // the grid of the last update (what the probe maps hold)
    f32 m_exposure = 1.0f, m_prevExposure = 1.0f;
    u64 m_frame = 0;
    Mat4 m_rotation = Mat4::Identity();
    u64 m_seed = 0x2545F4914F6CDD1Dull;
    Stats m_stats;
    Bake m_bake;
    // Baked probes (GIMode::Baked).
    BakedGI m_bakedData;          // waiting for upload
    bool m_bakedPending = false;
    bool m_bakedValid = false;
    rhi::Texture* m_bakedTextures[3] = {}; // irradiance, distance, probe data
    GIVolumeConstants m_bakedConstants;
    f32 m_bakedExposure = 1.0f;
};

} // namespace zx::render
