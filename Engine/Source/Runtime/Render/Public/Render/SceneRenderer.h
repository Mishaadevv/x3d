#pragma once

// Static mesh rendering: CPU frustum culling and LOD selection per mesh section (chunk), per-frame GPU instance
// buffer, cascaded sun shadows (atlas), depth pre-pass + forward PBR (opaque, alpha-tested) + sorted translucent pass.
//
//   renderer.Prepare(instances, view, lighting, settings);   // after RenderAssets::Update
//   renderer.AddPasses(graph, sceneColor, sceneDepth, clearColor);

#include "Core/Containers/Function.h"
#include "Render/RenderAssets.h"
#include "Render/RenderGraph.h"

#include <cmath>
#include <cstddef>

namespace zx::render {

struct RenderInstance
{
    Mat4 world;
    u64 id = 0; // stable identity (entity): pairs this frame's instance with last frame's
    // Motion (per-object motion blur, TAA): last frame's world matrix when it differs from world (the instance moved).
    // Null = did not move / did not exist. Must stay valid until AddPasses executes.
    const Mat4* prevWorld = nullptr;
    /// Skinned meshes: skinning matrices of this frame (mesh space), empty = static / bind pose. Must stay valid until
    /// AddPasses executes.
    Span<const Mat4> skinMatrices;
    /// Morph target weights (the mesh's target order), empty = none applied. Must stay valid until AddPasses executes.
    Span<const f32> morphWeights;
    AssetId mesh;
    /// Material per mesh slot (missing/invalid -> default material). Must stay valid until AddPasses executes.
    Span<const AssetId> materials;
    bool castShadows = true;
    // Fire (Flammable objects): charred within burnRadius of burnOrigin (world), glowing edge and embers.
    Vec3 burnOrigin;
    f32 burnRadius = 0.0f; // 0 = not burnt
    f32 charring = 0.0f;   // 0..1 how black
    f32 burnGlow = 0.0f;   // 0..1 glowing edge of the flames
    f32 embers = 0.0f;     // 0..1 embers in the charred part
};

struct RenderView
{
    Mat4 view;
    Mat4 projection;
    Mat4 viewProj;
    Vec3 position;
    f32 fovY = 1.0f; // radians
    u32 width = 1;   // render resolution
    u32 height = 1;
    f32 nearPlane = 0.05f;
};

enum class LightType : u8
{
    Point,
    Spot,
};

/// Punctual light in physical units.
struct LightDesc
{
    LightType type = LightType::Point;
    Vec3 position;
    Vec3 direction = Vec3(0, -1, 0); // spot: where it shines
    Vec3 color = Vec3(1, 1, 1);      // linear, normalized hue
    f32 intensity = 100.0f;          // candela (lumens / 4pi for an isotropic point light)
    f32 range = 10.0f;               // meters; the falloff is windowed to zero here
    f32 innerAngle = 0.4f;           // spot half-angles, radians
    f32 outerAngle = 0.6f;
    bool castShadows = false;        // gets tiles in the local shadow atlas (nearest lights first)
    u32 id = 0;                      // stable identity (e.g. entity) - keeps atlas tiles and caches across frames
};

inline f32 LumensToCandela(f32 lumens) { return lumens / (4.0f * Pi); }

/// Projected decal: a -1..1 box (world = box transform including its size) whose local -Y is the projection direction.
struct DecalDesc
{
    Mat4 world = Mat4::Identity();
    u32 albedo = rhi::InvalidDescriptor; // texture SRV: rgb color, a coverage (invalid = solid color)
    u32 normal = rhi::InvalidDescriptor; // BC5 normal map SRV
    Vec3 color = Vec3(1, 1, 1);
    f32 opacity = 1.0f;
    f32 roughness = 0.5f;
    bool affectRoughness = false;
    f32 normalStrength = 1.0f;
    f32 angleFade = 0.5f; // 0 = hard cut at 90 degrees, 1 = fades from facing to perpendicular
    f32 emissive = 0.0f;
    i32 sortOrder = 0; // higher draws on top
};

/// Dynamic GI probes as the shaders see them (Shaders/Common/DDGI.hlsli DDGIVolume, 80 bytes; GlobalIllumination fills it).
struct GIVolumeConstants
{
    IVec3 originIndex;
    u32 irradiance = rhi::InvalidDescriptor; // invalid: no GI (the open sky lights everything)
    Vec3 spacing = Vec3(1.0f);
    u32 distance = rhi::InvalidDescriptor;
    UVec3 counts;
    f32 intensity = 1.0f;
    f32 normalBias = 0.0f;
    f32 viewBias = 0.0f;
    Vec2 invIrradianceSize;
    Vec2 invDistanceSize;
    f32 maxDistance = 0.0f;
    u32 probeData = rhi::InvalidDescriptor; // probe offsets (relocation)
};
static_assert(sizeof(GIVolumeConstants) == 80);

/// Scene lighting in physical units; the renderer pre-exposes everything (exposure = 1 / (1.2 * 2^EV100)).
struct SceneLighting
{
    Vec3 sunDirection = Normalize(Vec3(-0.3f, -1.0f, 0.2f)); // direction light travels
    Vec3 sunColor = Vec3(1.0f, 0.96f, 0.9f);
    f32 sunIlluminance = 100000.0f; // lux (clear-sky noon ~100k, overcast ~10k, full moon ~0.25)
    Vec3 skyColor = Vec3(0.45f, 0.6f, 0.9f);
    Vec3 groundColor = Vec3(0.18f, 0.16f, 0.14f);
    f32 skyLuminance = 8000.0f; // cd/m^2 of the (hemispherical) ambient term
    f32 exposureEV100 = 15.0f;  // sunny-16 daylight ~15, interiors ~7, night ~0
    Span<const LightDesc> lights;
    /// Sky irradiance as SH9 (9 x float4 in a >= 256-byte buffer, see SkyRenderer). Null = hemispherical
    /// skyColor/groundColor * skyLuminance instead.
    rhi::Buffer* skyIrradianceBuffer = nullptr;
    Vec3 skyIrradianceScale = Vec3(0, 0, 0); // multiplies the SH (sun illuminance x color x exposure applied by Sky)
    // Reflections (ReflectionRenderer::Prepare fills these): specular image-based lighting.
    u32 skyCube = rhi::InvalidDescriptor; // prefiltered sky cube; invalid = the SH sky along the reflection
    f32 skyCubeMips = 0.0f;               // mip count - 1
    u32 probeBuffer = rhi::InvalidDescriptor;
    u32 probeCount = 0;
    f32 reflectionExposure = 1.0f;        // current exposure / the cubes' reference exposure
    u32 ssrTexture = rhi::InvalidDescriptor; // last frame's screen-space reflections
    Mat4 ssrViewProj = Mat4::Identity();
    f32 ssrMaxRoughness = 0.0f;
    /// Decals projected onto the scene (the forward pass applies them before lighting). Valid until AddPasses.
    Span<const DecalDesc> decals;
    /// Cloud shadows (CloudRenderer): fraction of sunlight reaching the ground over a square of the world.
    u32 cloudShadow = rhi::InvalidDescriptor;
    Vec2 cloudShadowOrigin; // world xz of the map's corner
    f32 cloudShadowExtent = 1.0f;
    /// Night: the hemispherical skyColor / groundColor x skyLuminance ambient is added on top of the SH sky with this
    /// weight (the physical sky goes dark after sunset; this is the moon- and starlight around).
    f32 ambientFloor = 0.0f;
    /// Dynamic GI probes (GlobalIllumination::Prepare): the diffuse ambient comes from them where they cover the scene.
    GIVolumeConstants gi;
};

inline f32 ExposureFromEV100(f32 ev100) { return 1.0f / (1.2f * std::exp2(ev100)); }

struct ShadowSettings
{
    bool enabled = true;
    u32 cascades = 3;        // 1..4
    u32 resolution = 1024;   // per cascade (the atlas is cascades x 1 tiles)
    f32 distance = 60.0f;    // meters covered by the last cascade
    f32 splitLambda = 0.8f;  // 0 = uniform, 1 = logarithmic splits
    f32 depthBias = -1.0f;   // rasterizer bias (reversed-Z: negative pushes depth away from the light)
    f32 slopeBias = -2.0f;
    f32 normalBias = 1.5f;   // receiver offset along the normal, in shadow texels
    u32 filter = 1;          // 0 = hard, 1 = 4 taps (2x2 bilinear PCF), 2 = 9 taps, 3 = PCSS (contact-hardening)
    f32 sourceAngle = 1.5f;  // PCSS: apparent sun diameter in degrees (the real sun: 0.53; larger = softer far shadows)
    f32 maxPenumbraTexels = 16.0f; // PCSS: filter radius cap
    u32 lodBias = 1;         // shadow casters use this many coarser LODs than the main view
    f32 cascadeBlend = 0.1f; // fraction of each cascade cross-faded into the next
    // Local (point/spot) shadows: fixed-size tiles in a persistent atlas; a point light uses 6 tiles.
    bool localEnabled = true;
    u32 localAtlasSize = 2048;
    u32 localTileSize = 256;
    u32 maxLocalFaces = 24;            // shadowed faces per frame (nearest lights first)
    u32 maxLocalUpdatesPerFrame = 12;  // faces re-rendered per frame (moved lights, new tiles)
    f32 localNormalBias = 1.5f;        // in shadow texels
    /// Static scene + static sun: re-render a cascade only when the view slice leaves its (enlarged) bounds.
    bool cache = true;
    f32 cacheMargin = 0.15f; // extra cascade radius that lets the camera move before a refresh
    u32 maxUpdatesPerFrame = 2;
    // Virtual shadow maps for the sun (surfaces; fog, water and particles keep the cascades): a clipmap of levels with
    // 16k x 16k virtual texels each, texel size doubling per level; only the 128x128 pages the screen needs are
    // rendered into a pool and cached until something moves.
    bool virtualShadows = false;
    u32 vsmLevels = 12;
    f32 vsmTexel = 0.0006f;       // level 0 texel, meters (finer levels serve pixels near the camera)
    f32 vsmResolution = 1.0f;     // < 1: coarser texels than the pixel footprint (Lite)
    f32 vsmDistance = 250.0f;     // meters
    u32 vsmPoolPages = 1024;      // 128x128 D32 pages (1024 = a 4096 x 4096 atlas, 64 MB)
    u32 vsmPagesPerFrame = 48;    // render budget
    f32 vsmNormalBias = 1.5f;     // texels
};

struct AOSettings
{
    bool enabled = true;        // needs the depth pre-pass
    bool halfResolution = true; // Lite
    f32 radius = 1.5f;          // meters
    u32 slices = 2;
    u32 steps = 4;
    f32 intensity = 1.5f;       // exponent applied to the visibility
    f32 maxRadiusPixels = 48.0f;
};

/// Screen-space contact shadows for the sun: a short ray march through the depth buffer (needs the depth pre-pass).
struct ContactShadowSettings
{
    bool enabled = true;
    f32 length = 0.3f;     // meters near the camera (grows with distance up to 2x)
    u32 steps = 8;
    f32 thickness = 0.03f; // assumed depth of the occluders, meters per meter of view distance
};

/// ZNano virtual geometry (Asset/Nano.h): meshes cooked with clusters are drawn cluster by cluster at the detail the
/// screen needs (GPU-driven frames; otherwise they use their fallback LOD chains).
struct NanoSettings
{
    bool enabled = true;
    f32 pixelError = 1.0f;          // LOD error allowed on screen, pixels (Lite: 2)
    f32 shadowPixelError = 2.0f;    // in shadow texels
    u32 triangleBudget = 3000000;   // main view; over it the error threshold rises (shadows and phase 2 get half)
};

struct SceneRenderSettings
{
    bool depthPrepass = true;
    /// > 1 keeps detailed LODs longer, < 1 switches earlier (Lite profile uses 0.75).
    f32 lodBias = 1.0f;
    /// Forces a LOD (-1 = automatic).
    i32 forcedLod = -1;
    /// 0 lit, 1 base color, 2 normals, 3 roughness/metallic, 4 LOD colors, 5 shadow cascades, 6 light clusters, 7 AO.
    u32 debugMode = 0;
    /// Material texture filtering: 1 = trilinear, 2..16 = anisotropic (Lite profile uses 4).
    u32 maxAnisotropy = 16;
    /// The opaque pass also writes normal + roughness (R10G10B10A2) for screen-space reflections.
    bool normalOutput = false;
    /// Deferred path (needs the depth pre-pass): opaque surfaces into a G-buffer, then one lighting pass per pixel.
    /// Debug views use the forward path. Translucent surfaces are always forward.
    bool deferred = false;
    /// GPU-driven opaque / masked geometry: culling, LOD selection and draw lists on the GPU (DrawIndexedIndirect).
    bool gpuDriven = true;
    /// Two-phase HiZ occlusion culling (GPU-driven only); off for one-off views such as probe captures.
    bool occlusionCulling = true;
    ShadowSettings shadows;
    AOSettings ao;
    ContactShadowSettings contactShadows;
    NanoSettings nano;
};

class SceneRenderer
{
public:
    static constexpr u32 MaxCascades = 4;

    SceneRenderer(rhi::Device& device, RenderAssets& assets);
    ~SceneRenderer();
    ZX_NON_COPYABLE(SceneRenderer)

    bool Init(rhi::Format colorFormat, rhi::Format depthFormat);

    /// Seconds for animated materials (material graph Time node).
    void SetTime(f32 seconds) { m_time = seconds; }
    void Prepare(Span<const RenderInstance> instances, const RenderView& view, const SceneLighting& lighting,
                 const SceneRenderSettings& settings);
    /// Shadow passes, depth pre-pass (optional), opaque/masked forward pass and translucent pass. `afterOpaque` runs
    /// between the opaque and translucent passes (normals: the normal + roughness target when normalOutput is set).
    void AddPasses(RenderGraph& graph, RGTexture color, RGTexture depth, LinearColor clearColor,
                   Function<void(RenderGraph&, RGTexture color, RGTexture depth, RGTexture normals)> afterOpaque = nullptr);
    /// Forces all shadow cascades to re-render next frame (scene content changed).
    void InvalidateShadows();
    /// The view jumped (camera cut): last frame's depth says nothing about this one.
    void ResetOcclusion() { m_hizValid = false; }

    /// The lights and sun shadows of this frame for passes that light other things the same way (volumetric fog).
    /// Valid after AddPasses, within the same render graph.
    struct LightingInputs
    {
        u32 lightBuffer = rhi::InvalidDescriptor;   // GpuLight x lightCount (pre-exposed)
        u32 clusterBuffer = rhi::InvalidDescriptor; // light indices per cluster
        u32 lightCount = 0;
        f32 clusterDepthScale = 0.0f, clusterDepthBias = 0.0f;
        Vec2 clusterTileScale;                      // clusters per render-resolution pixel
        u32 cascadeCount = 0;
        Mat4 shadowMatrices[MaxCascades];           // world -> shadow uv + depth
        Vec4 cascadeSplits;                         // view-space far distance per cascade
        u32 shadowMaps[MaxCascades] = {rhi::InvalidDescriptor, rhi::InvalidDescriptor, rhi::InvalidDescriptor, rhi::InvalidDescriptor};
        RGTexture shadowTextures[MaxCascades];      // read them in your pass setup
        RGBuffer clusters;
    };
    const LightingInputs& GetLightingInputs() const { return m_lightingInputs; }
    /// This frame's point / spot lights (GpuLight buffer SRV, count), valid after Prepare.
    u32 PreparedLightBuffer() const { return m_viewConstants.lightCount ? m_viewConstants.lightBuffer : rhi::InvalidDescriptor; }
    u32 PreparedLightCount() const { return m_viewConstants.lightCount; }

    /// Camera matrices without jitter (this frame and last) for the velocity of moving objects.
    /// Indices (into the next Prepare's instances) of the instances whose world matrix changed since last frame.
    /// Cached sun cascades they touch (at the old or the new place) are re-rendered. Not called = nothing moves.
    void SetMovedInstances(Span<const u32> indices) 
    {
        m_movedInstances.Clear();
        m_movedInstances.Append(indices);
    }
    void SetMotionMatrices(const Mat4& viewProjNoJitter, const Mat4& prevViewProjNoJitter)
    {
        m_viewProjNoJitter = viewProjNoJitter;
        m_prevViewProjNoJitter = prevViewProjNoJitter;
        m_motion = true;
    }
    /// Screen-space motion of objects that moved this frame (RG16F, uv units, sentinel < -100 = not a moving object:
    /// use the camera's reprojection); invalid when nothing moved. Valid after AddPasses, within the same graph.
    RGTexture VelocityTexture() const { return m_velocityTexture; }

    struct Stats
    {
        u32 instances = 0;
        u32 culled = 0;         // instances
        u32 culledSections = 0; // chunks of visible instances
        u32 notResident = 0;
        u32 draws = 0;
        u64 triangles = 0;
        u32 shadowDraws = 0;
        u64 shadowTriangles = 0;
        u32 cascadesUpdated = 0;
        u32 localShadowFaces = 0;   // shadowed point/spot faces this frame
        u32 localShadowUpdates = 0; // faces re-rendered this frame
        u32 lights = 0; // after frustum culling
        u32 decals = 0; // after frustum culling
        u32 lodHistogram[8] = {};
        // GPU-driven path (draws / triangles above count only the CPU-drawn parts then).
        bool gpuDriven = false;
        bool deferred = false;
        u32 gpuItems = 0;      // (instance, section) items culled on the GPU
        u32 gpuGroups = 0;     // mesh section + material combinations
        u32 indirectCalls = 0; // DrawIndexedIndirect calls (per pass)
        // Virtual geometry (the GPU numbers are from a frame or two ago).
        u32 nanoInstances = 0;
        u64 nanoSourceTriangles = 0; // LOD0 triangles of the nano instances
        u32 nanoTested = 0;          // clusters the main view tests
        u32 nanoBins = 0;
        u32 nanoClusters = 0;        // drawn in the main view
        u64 nanoTriangles = 0;
        u64 nanoShadowTriangles = 0; // the last cascade rendered
        f32 nanoErrorScale = 1.0f;   // > 1: over budget, coarser than asked
        bool nanoOverflow = false;
        // Virtual shadow maps.
        bool virtualShadows = false;
        u32 vsmMappedPages = 0;
        u32 vsmRequestedPages = 0;
        u32 vsmRenderedPages = 0;   // this frame
        u32 vsmPendingPages = 0;    // requested but waiting for the render budget
        u32 vsmDraws = 0;
        bool vsmBuilding = false;   // pages for a turned sun are being rendered (the old ones still show)
    };
    const Stats& GetStats() const { return m_stats; }

private:
    struct DrawItem
    {
        u64 sortKey;
        u32 instance;
        u32 material;
        const GpuMesh* mesh;
        u32 indexOffset;
        u32 indexCount;
        u8 pipeline; // PipelineKind
        u8 lod;
        u32 shader = 0; // material graph shader (MaterialShaderCache id), 0 = standard
        u32 vertexBuffer = InvalidIndex;     // skinned: the instance's posed vertices (SRV) instead of the mesh's
        u32 prevVertexBuffer = InvalidIndex; // skinned: last frame's posed vertices (velocity)
    };
    enum PipelineKind : u8
    {
        Opaque,
        OpaqueTwoSided,
        Masked,
        MaskedTwoSided,
        Translucent,
        TranslucentTwoSided,
        KindCount
    };
    enum PipelineVariant : u8
    {
        VariantDepth,       // depth pre-pass
        VariantMain,        // forward with depth write
        VariantMainNoWrite, // forward after the pre-pass
        VariantShadow,      // shadow caster (biased, two-sided)
        VariantMainMRT,        // VariantMain + normal/roughness target
        VariantMainNoWriteMRT, // VariantMainNoWrite + normal/roughness target
        VariantGBuffer,        // deferred: emission + three G-buffer targets, after the pre-pass
        VariantCount
    };

    rhi::Pipeline* GetPipeline(u32 kind, PipelineVariant variant, u32 shader = 0, bool nano = false);
    void SetGBufferFormats(rhi::GraphicsPipelineDesc& pd) const;
    // --- GPU-driven path (SceneRendererGpu.cpp) ---
    static constexpr u32 GpuViewCount = 2 + MaxCascades; // main phase 1, main phase 2, cascades
    static constexpr u32 MaxGpuLods = 4;
    struct DrawGroup // one mesh section with one material
    {
        const GpuMesh* mesh = nullptr;
        u32 section = 0;
        AssetId materialId;
        u32 material = 0;
        u8 pipeline = Opaque;
        u32 shader = 0;
        u32 lodCount = 1;
        bool translucent = false;
        u32 count = 0;     // items this frame
        u32 dense = 0;     // index in this frame's group table
        u32 firstSlot = 0; // this frame
        u32 listBase = 0;  // this frame
        u64 lastFrame = 0;
    };
    struct DrawTemplate // a mesh with its material list: the group of each section
    {
        const GpuMesh* mesh = nullptr;
        Array<AssetId> materials;
        Array<u32> groups;
        bool hasTranslucent = false;
    };
    struct Bucket // slots drawn by one DrawIndexedIndirect: same pipeline, shader and index buffer
    {
        u8 pipeline = Opaque;
        u32 shader = 0;
        const GpuMesh* mesh = nullptr;
        u32 firstSlot = 0;
        u32 slotCount = 0;
    };
    bool InitGpuDriven();
    void PrepareGpu(Span<const RenderInstance> instances, const RenderView& view, const struct Frustum& frustum, f32 invTanHalfFov,
                    u8* instanceData);
    u32 TemplateFor(const GpuMesh* mesh, Span<const AssetId> materials);
    void AddCullPass(RenderGraph& graph, u32 viewIndex, u32 mode, const Mat4& viewProj, RGBuffer args, RGBuffer lists, RGBuffer candidates,
                     u32 requiredFlags, f32 minRadius, u32 lodOffset);
    void DrawIndirect(rhi::CommandList& cmd, u32 viewIndex, PipelineVariant variant);
    void AddCullInitPass(RenderGraph& graph, RGBuffer args, RGBuffer candidates, u32 viewMask);
    void AddHiZPass(RenderGraph& graph, RGTexture depth);
    void EnsureGpuBuffers();
    void Draw(rhi::CommandList& cmd, const DrawItem& d, PipelineVariant variant, rhi::Pipeline*& bound);
    void EnsureInstanceCapacity(u32 count);
    // --- Virtual geometry (SceneRendererNano.cpp) ---
    static constexpr u32 NanoRegions = 3; // index / record regions: main phase 1, main phase 2, shadows (one at a time)
    static constexpr u32 VsmMaxLevels = 16;
    static constexpr u32 NanoViewCount = GpuViewCount + VsmMaxLevels; // + one view per virtual shadow level
    struct NanoRef
    {
        u32 instance;   // into Prepare's instances
        u32 row;
        u32 templ;
        const GpuMesh* mesh;
        u32 sectionBase = 0; // into this frame's section -> bin table
        Sphere sphere;       // world, holds every cluster sphere
        f32 scale = 1.0f;
    };
    struct NanoBucket // bins drawn by one DrawIndexedIndirect: same pipeline and shader
    {
        u8 pipeline = Opaque;
        u32 shader = 0;
        u32 firstBin = 0;
        u32 binCount = 0;
    };
    struct NanoView
    {
        u32 itemsOffset = 0; // bytes into the tables
        u32 itemCount = 0;
        u32 threads = 0;     // candidate clusters
        bool ortho = false;
        f32 pixelsPerUnit = 1.0f;
        f32 threshold = 1.0f;
        u32 vsmLevel = InvalidIndex; // virtual shadow level: clusters replicate into the pages they touch
        u32 vsmMapOffset = 0;        // bytes into the VSM table: window page -> render page
    };
    struct NanoBuffers
    {
        RGBuffer args, counters, visible, indices, candidates;
    };
    bool InitNano();
    void PrepareNano(Span<const RenderInstance> instances, const RenderView& view, const struct Frustum& frustum, f32 invTanHalfFov,
                     const struct Frustum* cascadeFrustums);
    void EnsureNanoBuffers(u32 binCount, u32 candidates);
    void AddNanoCullPass(RenderGraph& graph, u32 view, u32 mode, const Mat4& viewProj, const NanoBuffers& buffers);
    void AddNanoStatsPass(RenderGraph& graph, const NanoBuffers& buffers);
    void DrawNano(rhi::CommandList& cmd, u32 view, PipelineVariant variant);
    // --- Virtual shadow maps (SceneRendererVsm.cpp) ---
    struct VsmPage
    {
        u32 phys = 0;
        u64 lastRequested = 0;
        bool rendered = false; // holds valid depth
        bool dirty = false;    // something moved through it (stale until re-rendered)
    };
    struct VsmRenderPage
    {
        u32 level;
        i32 x, y;  // absolute page coordinates in light space
        u32 phys;
        Mat4 viewProj;
        Array<DrawItem> draws; // regular casters (clusters come from the nano pass of the level)
    };
    bool InitVsm();
    void PrepareVsm(const RenderView& view, const SceneLighting& lighting);
    void AddVsmPasses(RenderGraph& graph, RGTexture atlas, const NanoBuffers* nanoBuffers);
    void AddVsmMarkPass(RenderGraph& graph, RGTexture depth);
    rhi::Pipeline* GetVsmPipeline(bool masked, u32 shader);
    static Vec2 VsmLight(const Vec4 rows[3], Vec3 world) { return Vec2(Dot(rows[0].XYZ(), world), Dot(rows[1].XYZ(), world)); }
    f32 VsmPageWorld(u32 level) const { return m_vsmTexel0 * f32(1u << level) * 128.0f; }
    void SetupCascades(const RenderView& view, const SceneLighting& lighting);
    u32 GetMaterialSampler(u32 maxAnisotropy);

    rhi::Device& m_device;
    RenderAssets& m_assets;
    rhi::Format m_colorFormat = rhi::Format::Unknown;
    rhi::Format m_depthFormat = rhi::Format::Unknown;
    rhi::ShaderBytecode m_vsNano, m_vsNanoDepth, m_vsNanoDepthAlpha; // ZX_NANO: cluster indices
    rhi::Pipeline* m_nanoPipelines[KindCount][VariantCount] = {};
    f32 m_nanoShadowPipelineBias[2] = {0.0f, 0.0f};
    rhi::ShaderBytecode m_vs, m_vsDepth, m_vsDepthAlpha, m_ps, m_psAlpha, m_psDepthAlpha, m_psNormals, m_psAlphaNormals;
    rhi::ShaderBytecode m_psGBuffer, m_psGBufferAlpha, m_deferredVS, m_deferredPS;
    rhi::Pipeline* m_deferredPipeline = nullptr; // lighting of the G-buffer (fullscreen, additive)
    rhi::Pipeline* m_pipelines[KindCount][VariantCount] = {};
    HashMap<u64, rhi::Pipeline*> m_graphPipelines; // (shader, kind, variant) -> pipeline
    f32 m_time = 0.0f;
    f32 m_shadowPipelineBias[2] = {0.0f, 0.0f}; // depth/slope bias the shadow pipelines were built with
    rhi::Buffer* m_instanceBuffers[rhi::FramesInFlight] = {};
    u32 m_instanceCapacity = 0;
    Array<DrawItem> m_opaque;
    Array<DrawItem> m_velocityDraws; // opaque sections of instances that moved
    rhi::ShaderBytecode m_velocityVS, m_velocityPS;
    rhi::Pipeline* m_velocityPipeline = nullptr;
    Mat4 m_viewProjNoJitter = Mat4::Identity(), m_prevViewProjNoJitter = Mat4::Identity();
    RGTexture m_velocityTexture;
    bool m_motion = false; // SetMotionMatrices was called (the main view; probe captures don't need velocity)
    Array<DrawItem> m_translucent;
    Array<DrawItem> m_shadowDraws[MaxCascades];
    SceneRenderSettings m_settings;

    struct ViewConstants
    {
        Mat4 viewProj;
        Vec3 cameraPosition;
        f32 time;
        Vec3 lightDirection;
        f32 lightIntensity;
        Vec3 lightColor;
        u32 instanceBuffer;
        Vec3 skyColor;
        u32 materialBuffer;
        Vec3 groundColor;
        f32 ambientIntensity;
        u32 debugMode;
        u32 materialSampler;
        u32 materialParams; // material graph parameter buffer
        f32 ambientFloor;   // hemispherical ambient added on top of the SH sky (SceneLighting::ambientFloor)
        // Shadows (keep in sync with ForwardPBR.hlsl).
        Mat4 shadowMatrices[MaxCascades]; // world -> shadow uv + depth
        Vec4 cascadeSplits;               // view-space far distance of each cascade
        Vec4 cascadeTexelWorld;           // world size of one shadow texel per cascade
        u32 shadowMaps[MaxCascades];      // bindless SRV per cascade
        Vec3 cameraForward;
        f32 shadowTexel; // 1 / resolution
        f32 normalBias;
        u32 cascadeCount; // 0 = no shadows
        u32 shadowFilter;
        f32 cascadeBlend;
        // Clustered lights.
        u32 lightBuffer;
        u32 clusterBuffer;
        u32 lightCount;
        f32 clusterDepthScale; // slice = log(viewZ) * scale + bias
        f32 clusterDepthBias;
        Vec2 clusterTileScale; // clusters per pixel (x, y)
        f32 exposure;
        Vec3 skyIrradianceScale;
        u32 skyIrradianceSH;
        u32 aoTexture;      // GTAO result (render resolution or half), or invalid
        Vec2 invRenderSize;
        u32 pad3;
        u32 localShadowAtlas;  // SRV or invalid
        u32 localShadowFaces;  // SRV: per face 4 matrix rows + uv rect
        f32 localAtlasTexel;   // 1 / atlas size
        f32 localNormalBias;
        // Reflections.
        u32 skyCube;
        f32 skyCubeMips;
        u32 probeBuffer;
        u32 probeCount;
        f32 reflectionExposure;
        u32 ssrTexture;
        f32 ssrMaxRoughness;
        f32 reflectionPad;
        Mat4 ssrViewProj;
        u32 decalBuffer;
        u32 decalCount;
        Vec2 decalPad;
        u32 cloudShadow; // SRV or invalid
        f32 cloudShadowInvExtent;
        Vec2 cloudShadowOrigin;
        // GPU-driven draws.
        u32 drawSlots;
        u32 visibleLists;
        u32 slotCount;
        u32 listCapacity;
        // Sun shadow softness (PCSS) and contact shadows.
        Vec4 cascadeDepthRange; // meters per unit of shadow depth, per cascade
        f32 sunSourceTan;       // tan of the sun's angular radius
        f32 pcssMaxTexels;
        u32 contactShadows;     // R8 render-size texture (1 = lit) or invalid
        f32 shadowPad;
        // Deferred lighting.
        Mat4 invViewProj;
        u32 gbufferA, gbufferB, gbufferC, sceneDepth;
        // Virtual shadow maps (Common/VirtualShadows.hlsli).
        Vec4 vsmLightRows[3]; // world -> light space (meters; z along the sun's rays)
        u32 vsmTable;         // SRV: level headers + page tables, invalid = cascades
        u32 vsmAtlas;
        u32 vsmLevels;
        f32 vsmTexel0;
        f32 vsmPixelScale;    // meters per pixel per meter of distance / resolution
        f32 vsmDepthScale;    // depth = 0.5 - z * scale
        f32 vsmNormalBias;
        f32 vsmMaxDistance;
        Vec2 vsmInvAtlasSize;
        u32 vsmPagesPerRow;
        f32 vsmPad;
        GIVolumeConstants gi; // Common/DDGI.hlsli
    } m_viewConstants = {};
    // HLSL constant buffer packing: a matrix starts on a 16-byte boundary.
    static_assert(offsetof(ViewConstants, ssrViewProj) % 16 == 0, "pad ViewConstants (and ViewData in ForwardPBR.hlsl)");
    static_assert(offsetof(ViewConstants, cascadeDepthRange) % 16 == 0, "pad ViewConstants (and ViewData in ForwardPBR.hlsl)");
    static_assert(offsetof(ViewConstants, invViewProj) % 16 == 0, "pad ViewConstants (and ViewData in ForwardPBR.hlsl)");
    static_assert(offsetof(ViewConstants, gi) % 16 == 0, "pad ViewConstants (and ViewData in ForwardPBR.hlsl)");
    // Local shadow atlas state.
    struct LocalSlot
    {
        u32 lightId = 0;
        u32 face = 0;
        Vec3 position;
        Vec3 direction;
        u64 lastUsed = 0;
        bool valid = false; // contains a rendered map for (lightId, face) at position/direction
    };
    struct LocalUpdate
    {
        u32 slot;
        Mat4 viewProj;
        Array<DrawItem> draws;
    };
    struct InstanceRef // shadow caster (local lights); instance points into Prepare's instances
    {
        u32 index;
        const GpuMesh* mesh;
        const RenderInstance* instance;
        u32 vertexBuffer = InvalidIndex; // skinned: posed vertices
        bool nano = false;               // drawn as clusters this frame
    };
    // GPU skinning: each skinned instance owns two vertex buffers used in turn (this frame's pose, last frame's).
    struct SkinJob
    {
        u32 instance;        // index into Prepare's instances
        u32 row;             // instance row (GPU-driven frames: assigned by PrepareGpu)
        rhi::Buffer* output; // this frame's posed vertices
        u32 sourceVertices, skin, paletteBase, vertexCount, boneCount; // skin = InvalidIndex: morph targets only
        u32 morphDeltas, morphBase, morphCount;                        // weights at morphBase in the weight buffer
        u32 current, previous; // SRVs (previous = current when last frame had no pose)
    };
    struct SkinState
    {
        rhi::Buffer* buffers[2] = {};
        u32 current = 0;
        u32 vertexCount = 0;
        u64 lastFrame = 0;
    };
    void PrepareSkinning(Span<const RenderInstance> instances);
    void AddSkinningPass(RenderGraph& graph);
    u32 SkinSlot(usize instance) const { return m_skinSlot.Empty() ? InvalidIndex : m_skinSlot[instance]; }

public:
    /// Posed vertex buffer (SRV) of a skinned instance this frame (after Prepare), or InvalidIndex.
    u32 SkinnedVertexBuffer(u64 instanceId) const
    {
        const SkinState* st = m_skinStates.Find(instanceId);
        return st && st->lastFrame == m_frame && st->buffers[st->current] ? st->buffers[st->current]->GetSrvIndex() : InvalidIndex;
    }

private:
    HashMap<u64, SkinState> m_skinStates; // instance id -> buffers
    Array<SkinJob> m_skinJobs;
    Array<u32> m_skinSlot; // per instance: index into m_skinJobs (empty when nothing is skinned)
    rhi::Buffer* m_paletteBuffers[rhi::FramesInFlight] = {};
    u64 m_paletteCapacity = 0;
    rhi::Buffer* m_morphWeightBuffers[rhi::FramesInFlight] = {};
    u64 m_morphWeightCapacity = 0;
    rhi::ShaderBytecode m_skinCS;
    rhi::Pipeline* m_skinPipeline = nullptr;
    rhi::Texture* m_localAtlas = nullptr;
    u32 m_localAtlasSize = 0;
    u32 m_localTileSize = 0;
    Array<LocalSlot> m_localSlots;
    Array<LocalUpdate> m_localUpdates;
    Array<InstanceRef> m_casters;
    rhi::Buffer* m_localFaceBuffers[rhi::FramesInFlight] = {};
    rhi::ShaderBytecode m_clearTileVS;
    rhi::Pipeline* m_clearTilePipeline = nullptr;
    void PrepareLocalShadows(Span<const LightDesc> lights, const RenderView& view, struct GpuLightData* gpuLights,
                             Span<const u32> gpuIndexOfLight);
    struct AOParams
    {
        u32 depth;
        u32 output;
        u32 input;
        u32 frame;
        Vec2 aoSize;
        Vec2 invAoSize;
        f32 tanHalfX;
        f32 tanHalfY;
        f32 nearZ;
        f32 radius;
        f32 maxRadiusPixels;
        f32 intensity;
        u32 sliceCount;
        u32 stepCount;
    } m_aoParams = {};
    struct ContactParams // ContactShadows.hlsl
    {
        u32 depth;
        u32 output;
        u32 frame;
        u32 stepCount;
        Vec2 size;
        Vec2 invSize;
        Vec3 toSunView;
        f32 rayLength;
        f32 tanHalfX;
        f32 tanHalfY;
        f32 nearZ;
        f32 thickness;
    } m_contactParams = {};
    rhi::ShaderBytecode m_contactCS;
    rhi::Pipeline* m_contactPipeline = nullptr;
    rhi::ShaderBytecode m_gtaoCS, m_aoBlurCS;
    rhi::Pipeline* m_gtaoPipeline = nullptr;
    rhi::Pipeline* m_aoBlurPipeline = nullptr;
    UVec2 m_renderSize;
    struct ClusterParams
    {
        Mat4 view;
        u32 lightBuffer;
        u32 clusterBuffer;
        u32 lightCount;
        f32 nearZ;
        f32 farZ;
        f32 tanHalfX;
        f32 tanHalfY;
        f32 pad;
    } m_clusterParams = {};
    rhi::ShaderBytecode m_clusterCS;
    rhi::Pipeline* m_clusterPipeline = nullptr;
    rhi::Buffer* m_lightBuffers[rhi::FramesInFlight] = {};
    rhi::Buffer* m_decalBuffers[rhi::FramesInFlight] = {};
    void PrepareDecals(Span<const DecalDesc> decals, const struct Frustum& frustum);
    rhi::Buffer* m_clusterBuffer = nullptr;
    rhi::Buffer* m_skyIrradianceBuffer = nullptr;
    struct Cascade
    {
        rhi::Texture* map = nullptr;
        Vec3 center;
        f32 radius = 0.0f;
        Mat4 viewProj;
        bool valid = false;
        bool update = false;
    };
    Cascade m_cascades[MaxCascades];
    LightingInputs m_lightingInputs;
    u32 m_cascadeCount = 0;
    u32 m_shadowResolution = 0;
    Vec3 m_shadowLightDir;
    u64 m_shadowContentKey = 0;
    Array<u32> m_movedInstances;
    Array<Sphere> m_movedCasters; // bounds of moved shadow casters, this and last frame's place
    u64 m_frame = 0;
    u32 m_samplers[5] = {rhi::InvalidDescriptor, rhi::InvalidDescriptor, rhi::InvalidDescriptor, rhi::InvalidDescriptor,
                         rhi::InvalidDescriptor}; // anisotropy 1, 2, 4, 8, 16
    Stats m_stats;
    bool m_shadersOk = false;

    // GPU-driven state.
    bool m_gpuFrame = false; // this frame draws opaque / masked geometry indirectly
    HashMap<u64, u32> m_templateIndex;
    Array<DrawTemplate> m_templates;
    HashMap<u64, u32> m_groupIndex;
    Array<DrawGroup> m_groups;
    Array<u32> m_activeGroups; // this frame, sorted by bucket
    Array<Bucket> m_buckets;
    u32 m_slotCount = 0, m_itemCount = 0, m_listCapacity = 0;
    struct InstanceCache // last frame's resolution of the instance at the same index
    {
        AssetId mesh;
        const GpuMesh* gpuMesh = nullptr;
        const AssetId* materials = nullptr;
        u32 materialCount = 0;
        u32 templateIndex = 0;
    };
    Array<InstanceCache> m_instanceCache;
    u64 m_instanceCacheGeneration = ~0ull; // RenderAssets::MeshGeneration the cache was built with
    rhi::Buffer* m_cullItems[rhi::FramesInFlight] = {};  // per frame (upload): cull items, group table, slot table
    rhi::Buffer* m_cullGroups[rhi::FramesInFlight] = {};
    rhi::Buffer* m_cullSlots[rhi::FramesInFlight] = {};
    u64 m_cullItemBytes = 0, m_cullGroupBytes = 0, m_cullSlotBytes = 0;
    rhi::Buffer* m_indirectArgs = nullptr;  // GpuViewCount regions of IndirectDrawArgs
    rhi::Buffer* m_visibleLists = nullptr;  // GpuViewCount regions of instance indices
    rhi::Buffer* m_candidates = nullptr;    // phase 1 -> phase 2 items
    u32 m_argsCapacity = 0, m_listsCapacity = 0, m_candidateCapacity = 0;
    rhi::Texture* m_hiz[2][2] = {};         // [frame parity][even / odd mips]
    UVec2 m_hizSize;
    u32 m_hizMips = 0;
    u32 m_hizCurrent = 0;
    bool m_hizValid = false;
    Mat4 m_hizViewProj = Mat4::Identity(); // the view m_hiz[!current] was built with
    Mat4 m_cullViewProj = Mat4::Identity();
    Vec3 m_cullCamera;
    f32 m_cullInvTanHalfFov = 1.0f;
    rhi::ShaderBytecode m_cullInitCS, m_cullCS, m_hizCS;
    // Virtual shadow map state.
    bool m_vsmActive = false;
    u32 m_vsmLevels = 0;
    f32 m_vsmTexel0 = 0.001f;
    // Two generations of pages: the one shown (m_vsmRows) and, while the sun has turned, the one being built for the
    // new direction; it replaces the shown one once all its needed pages are rendered.
    Vec4 m_vsmRows[3];
    Vec3 m_vsmLightDir;
    u32 m_vsmGen = 0;
    bool m_vsmBuilding = false;
    Vec4 m_vsmBuildRows[3];
    Vec3 m_vsmBuildDir;
    bool m_vsmBuildMarked = false;               // the GPU has marked pages for the build generation
    bool m_vsmInvalidate = false;
    i32 m_vsmOrigin[VsmMaxLevels][2] = {};
    i32 m_vsmBuildOrigin[VsmMaxLevels][2] = {};
    const Vec4* m_vsmRenderRows = m_vsmRows;     // the generation rendered this frame
    const i32 (*m_vsmRenderOrigin)[2] = m_vsmOrigin;
    HashMap<u64, VsmPage> m_vsmPages;
    Array<u32> m_vsmFree;
    Array<VsmRenderPage> m_vsmRender;
    u32 m_vsmRenderLevels = 0;                   // bit per level that renders pages this frame
    u32 m_vsmMapOffset[VsmMaxLevels] = {};       // bytes into the table: the level's window -> render page map
    u32 m_vsmRenderPagesOffset = 0;              // bytes into the table: per render page rect + atlas position
    rhi::Texture* m_vsmAtlas = nullptr;
    u32 m_vsmAtlasPagesX = 0, m_vsmAtlasPagesY = 0;
    rhi::Buffer* m_vsmTables[rhi::FramesInFlight] = {};
    u64 m_vsmTableBytes = 0;
    rhi::Buffer* m_vsmRequests = nullptr;
    rhi::Buffer* m_vsmReadback[rhi::FramesInFlight] = {};
    bool m_vsmReadbackValid[rhi::FramesInFlight] = {};
    i32 m_vsmReadOrigin[rhi::FramesInFlight][2][VsmMaxLevels][2] = {}; // [slot][shown / building][level]
    u32 m_vsmReadGen[rhi::FramesInFlight][2] = {};
    bool m_vsmReadBuild[rhi::FramesInFlight] = {};
    u32 m_vsmReadLevels[rhi::FramesInFlight] = {};
    f32 m_vsmReadTexel[rhi::FramesInFlight] = {};
    rhi::ShaderBytecode m_vsmMarkCS, m_vsmClearCS, m_vsNanoVsm, m_vsNanoVsmAlpha;
    rhi::Pipeline* m_vsmMarkPipeline = nullptr;
    rhi::Pipeline* m_vsmClearPipeline = nullptr;
    Mat4 m_vsmLevelViewProj[VsmMaxLevels]; // per rendering level: the rectangle around its pages (cluster culling)
    u64 m_vsmFrame = 0;
    HashMap<u64, rhi::Pipeline*> m_vsmPipelines; // (shader, masked) -> nano page pipeline
    f32 m_vsmPipelineBias[2] = {0.0f, 0.0f};
    // Virtual geometry state.
    bool m_nanoActive = false; // this frame draws clusters
    Array<NanoRef> m_nanoRefs;
    Array<NanoBucket> m_nanoBuckets;
    NanoView m_nanoViews[NanoViewCount];
    u32 m_nanoBinCount = 0, m_nanoBinCapacity = 0;
    u32 m_nanoInstancesOffset = 0, m_nanoSectionBinsOffset = 0, m_nanoBinsOffset = 0; // bytes into the tables
    u32 m_nanoIndexCapacity[NanoRegions] = {}, m_nanoIndexBase[NanoRegions] = {};
    u32 m_nanoVisibleCapacity = 0, m_nanoCandidateCapacity = 0;
    u64 m_nanoIndexBytes = 0;
    f32 m_nanoErrorScale = 1.0f, m_nanoShadowErrorScale = 1.0f; // budget feedback
    struct NanoReadback
    {
        u32 clusters = 0;
        u64 triangles = 0, shadowTriangles = 0;
        bool overflow = false;
    } m_nanoLast;
    rhi::Buffer* m_nanoTables[rhi::FramesInFlight] = {}; // upload: instances, section bins, bins, view items
    u64 m_nanoTableBytes = 0;
    rhi::Buffer* m_nanoArgs = nullptr;       // IndirectDrawArgs per (region, bin)
    rhi::Buffer* m_nanoCounters = nullptr;   // per view: visible records, indices, overflow flags
    rhi::Buffer* m_nanoVisible = nullptr;    // uint4 records per region
    rhi::Buffer* m_nanoIndices = nullptr;    // the cluster index buffer (all regions)
    rhi::Buffer* m_nanoCandidates = nullptr; // phase 1 -> phase 2
    rhi::Buffer* m_nanoReadback[rhi::FramesInFlight] = {};
    bool m_nanoReadbackValid[rhi::FramesInFlight] = {};
    rhi::ShaderBytecode m_nanoClearCS, m_nanoCullCS, m_nanoArgsCS, m_nanoExpandCS;
    rhi::Pipeline* m_nanoClearPipeline = nullptr;
    rhi::Pipeline* m_nanoCullPipeline = nullptr;
    rhi::Pipeline* m_nanoArgsPipeline = nullptr;
    rhi::Pipeline* m_nanoExpandPipeline = nullptr;
    rhi::Pipeline* m_cullInitPipeline = nullptr;
    rhi::Pipeline* m_cullPipeline = nullptr;
    rhi::Pipeline* m_hizPipeline = nullptr;
};

} // namespace zx::render
