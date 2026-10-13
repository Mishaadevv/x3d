#pragma once

// Static mesh rendering: CPU frustum culling and LOD selection per mesh section (chunk), per-frame GPU instance
// buffer, cascaded sun shadows (atlas), depth pre-pass + forward PBR (opaque, alpha-tested) + sorted translucent pass.
//
//   renderer.Prepare(instances, view, lighting, settings);   // after RenderAssets::Update
//   renderer.AddPasses(graph, sceneColor, sceneDepth, clearColor);

#include "Render/RenderAssets.h"
#include "Render/RenderGraph.h"

#include <cmath>

namespace zx::render {

struct RenderInstance
{
    Mat4 world;
    AssetId mesh;
    /// Material per mesh slot (missing/invalid -> default material). Must stay valid until AddPasses executes.
    Span<const AssetId> materials;
    bool castShadows = true;
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
    u32 filter = 1;          // 0 = hard, 1 = 4 taps (2x2 bilinear PCF), 2 = 9 taps
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
    ShadowSettings shadows;
    AOSettings ao;
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
    /// Shadow passes, depth pre-pass (optional), opaque/masked forward pass and translucent pass.
    void AddPasses(RenderGraph& graph, RGTexture color, RGTexture depth, LinearColor clearColor);
    /// Forces all shadow cascades to re-render next frame (scene content changed).
    void InvalidateShadows();

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
        u32 lodHistogram[8] = {};
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
        VariantCount
    };

    rhi::Pipeline* GetPipeline(u32 kind, PipelineVariant variant, u32 shader = 0);
    void Draw(rhi::CommandList& cmd, const DrawItem& d, PipelineVariant variant, rhi::Pipeline*& bound);
    void EnsureInstanceCapacity(u32 count);
    void SetupCascades(const RenderView& view, const SceneLighting& lighting);
    u32 GetMaterialSampler(u32 maxAnisotropy);

    rhi::Device& m_device;
    RenderAssets& m_assets;
    rhi::Format m_colorFormat = rhi::Format::Unknown;
    rhi::Format m_depthFormat = rhi::Format::Unknown;
    rhi::ShaderBytecode m_vs, m_vsDepth, m_vsDepthAlpha, m_ps, m_psAlpha, m_psDepthAlpha;
    rhi::Pipeline* m_pipelines[KindCount][VariantCount] = {};
    HashMap<u64, rhi::Pipeline*> m_graphPipelines; // (shader, kind, variant) -> pipeline
    f32 m_time = 0.0f;
    f32 m_shadowPipelineBias[2] = {0.0f, 0.0f}; // depth/slope bias the shadow pipelines were built with
    rhi::Buffer* m_instanceBuffers[rhi::FramesInFlight] = {};
    u32 m_instanceCapacity = 0;
    Array<DrawItem> m_opaque;
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
        u32 pad;
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
    } m_viewConstants = {};
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
    struct InstanceRef
    {
        u32 index;
        const GpuMesh* mesh;
        Mat4 world;
        f32 scale;
        Span<const AssetId> materials;
    };
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
    u32 m_cascadeCount = 0;
    u32 m_shadowResolution = 0;
    Vec3 m_shadowLightDir;
    u64 m_shadowContentKey = 0;
    u64 m_frame = 0;
    u32 m_samplers[5] = {rhi::InvalidDescriptor, rhi::InvalidDescriptor, rhi::InvalidDescriptor, rhi::InvalidDescriptor,
                         rhi::InvalidDescriptor}; // anisotropy 1, 2, 4, 8, 16
    Stats m_stats;
    bool m_shadersOk = false;
};

} // namespace zx::render
