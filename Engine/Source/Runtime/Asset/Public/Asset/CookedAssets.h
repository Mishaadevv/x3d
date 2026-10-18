#pragma once

// Cooked (runtime-ready) asset formats and the cookers that produce them from imported data.
//
//   Mesh     -> CookedMesh     (packed 36-byte vertices, 16/32-bit indices, LODs, sections, bounds)
//   Texture  -> CookedTexture  (mips, BCn compressed, optional size cap for the Lite profile)
//   Material -> MaterialAsset  (PBR parameters + texture asset ids)
//   Scene    -> PrefabAsset    (node hierarchy referencing mesh/material assets)
//   Skin     -> Skeleton       (bones, bind pose, inverse bind matrices; Asset/Animation.h)
//   Anim     -> AnimationClip  (keyed bone tracks)
//
// All cooked formats serialize to a small binary container: magic + type + version + payload.

#include "Asset/SceneImport.h"
#include "Core/Hash/Guid.h"
#include "Core/Image/TextureTools.h"

namespace zx::asset {

using AssetId = Guid;

enum class AssetType : u8
{
    Unknown,
    Texture,
    Mesh,
    Material,
    Prefab,
    Skeleton,
    Animation,
    Count
};

const char* AssetTypeName(AssetType type);

// --- Mesh -------------------------------------------------------------------------------------------------------------

/// GPU vertex layout (read with ByteAddressBuffer in shaders, see Shaders/Common/Mesh.hlsli).
struct PackedVertex
{
    Vec3 position;
    u32 normal;  // octahedral snorm16x2
    u32 tangent; // octahedral snorm16x2, bitangent sign in bit 0
    Vec2 uv0;    // f32 (tiling UVs need the precision)
    u32 uv1;     // half2
    u32 color;   // RGBA8 linear
};
static_assert(sizeof(PackedVertex) == 36);

PackedVertex PackVertex(const MeshVertex& v);
MeshVertex UnpackVertex(const PackedVertex& p);

/// One level of a section's LOD chain.
struct CookedSectionLod
{
    u32 indexOffset = 0; // into the whole index buffer
    u32 indexCount = 0;
    /// Used when the section's projected radius / view height is below this (1 for LOD0).
    f32 screenSize = 1.0f;
    f32 error = 0.0f; // meters
};

/// A spatial chunk drawn with one material, with its own bounds and LOD chain.
struct CookedMeshSection
{
    AABB bounds;
    u32 materialSlot = 0;
    u32 firstLod = 0; // into CookedMesh::sectionLods
    u32 lodCount = 1;
    u32 pad = 0;
};

// --- ZNano virtual geometry (Asset/Nano.h builds it) ------------------------------------------------------------------

inline constexpr f32 NanoRootError = 3.0e38f; // parentError of the coarsest clusters

/// GPU layout (Shaders/Forward/Nano.hlsl reads it): 80 bytes.
struct NanoClusterHeader
{
    Vec4 bounds;       // center (mesh space) + radius: culling
    Vec4 selfSphere;   // LOD sphere of this cluster's level
    Vec4 parentSphere; // LOD sphere of the level replacing it
    f32 selfError = 0.0f;   // mesh units (0 for LOD0)
    f32 parentError = NanoRootError;
    u32 triangleStart = 0;  // into NanoMesh::triangles
    u32 triangleCount = 0;
    u32 vertexStart = 0;    // into NanoMesh::vertexRemap
    u32 vertexCount = 0;
    u32 section = 0;        // mesh section (material slot through it)
    u32 level = 0;          // 0 = LOD0
};
static_assert(sizeof(NanoClusterHeader) == 80);

struct NanoMesh
{
    Array<NanoClusterHeader> clusters; // sorted by parentError, largest first
    Array<u32> triangles;          // per triangle: local indices a | b << 8 | c << 16
    Array<u32> vertexRemap;        // per cluster vertex: index into the mesh's vertices
    u32 levels = 0;
    u32 leafTriangles = 0;

    bool Empty() const { return clusters.Empty(); }
    /// The GPU buffer: headers, then triangles, then the vertex remap (offsets in ClusterHeader are relative to
    /// their arrays; the shader adds TrianglesOffset() / RemapOffset()).
    u32 TrianglesOffset() const { return u32(clusters.Size() * sizeof(NanoClusterHeader)); }
    u32 RemapOffset() const { return TrianglesOffset() + u32(triangles.Size() * 4); }
    u64 GpuBytes() const { return u64(RemapOffset()) + vertexRemap.Size() * 4; }
    void WriteGpu(Array<u8>& out) const;
};


struct CookedMesh
{
    String name;
    Array<PackedVertex> vertices;
    Array<u8> indexData; // u16 or u32 indices of all LODs
    bool index16 = false;
    Array<CookedMeshSection> sections;
    Array<CookedSectionLod> sectionLods;
    u32 lodCount = 1; // longest chain
    AABB bounds;
    u32 materialSlotCount = 0;
    /// Source (importer) material index per slot.
    Array<u32> slotSourceMaterial;
    /// Skinned meshes: per vertex (same order as `vertices`) packed bone indices then packed weights (u8 x 4 each);
    /// empty for static meshes.
    Array<u32> skin;
    u32 boneCount = 0; // highest bone index used + 1
    /// Morph targets: names, default weights and offsets, target-major, 4 u32 per vertex (position delta as 3 floats,
    /// normal delta packed 10:10:10 snorm over [-2, 2]; see PackMorphNormal). Empty without morph targets.
    Array<String> morphNames;
    Array<f32> morphDefaults;
    Array<u32> morphDeltas;
    /// Virtual geometry: the cluster DAG (empty for regular meshes).
    NanoMesh nano;

    u32 IndexCount() const { return u32(indexData.Size() / (index16 ? 2 : 4)); }
    u32 Index(u32 i) const;
    /// Triangles drawn when every section uses `lod` (clamped to its chain).
    u32 TriangleCount(u32 lod) const;
    const CookedSectionLod& SectionLod(const CookedMeshSection& s, u32 lod) const
    {
        return sectionLods[s.firstLod + Min(lod, s.lodCount - 1)];
    }
    u64 MemorySize() const { return vertices.SizeBytes() + indexData.Size() + skin.SizeBytes() + morphDeltas.SizeBytes() + nano.GpuBytes(); }
    u32 MorphCount() const { return u32(morphNames.Size()); }
    bool IsSkinned() const { return !skin.Empty(); }
};

/// Normal offset of a morph target packed for the GPU (Shaders/Forward/Skinning.hlsl unpacks it).
u32 PackMorphNormal(Vec3 delta);
Vec3 UnpackMorphNormal(u32 packed);

/// Splits large sections into chunks and generates LODs (optional), optimizes for the vertex cache/fetch and
/// packs. `mesh` is modified.
/// Virtual geometry: Off, On, or Auto (static meshes with nano::AutoTriangleThreshold+ triangles).
enum class NanoMode : u8
{
    Auto,
    On,
    Off,
};
CookedMesh CookMesh(MeshData& mesh, bool generateLods = true, NanoMode nano = NanoMode::Off);

// --- Texture ----------------------------------------------------------------------------------------------------------

enum class TextureFormat : u8
{
    RGBA8,
    RGBA8_SRGB,
    BC1,
    BC1_SRGB,
    BC3,
    BC3_SRGB,
    BC4,
    BC5,
    RGBA16F,
    RGBA32F,
    BC6H,     // unsigned half-float RGB (HDR)
    BC7,
    BC7_SRGB,
};

u32 TextureFormatBlockBytes(TextureFormat f); // bytes per 4x4 block (BCn) or per pixel (uncompressed)
bool IsBlockCompressed(TextureFormat f);
u64 TextureMipSize(TextureFormat f, u32 width, u32 height);

enum class TextureCompression : u8
{
    None, // RGBA8 (HDR: RGBA16F)
    Auto, // BC1 opaque / BC7 with alpha (color), BC5 normal maps; HDR sources: BC6H
    BC1,
    BC3,
    BC4,
    BC5,
    BC7, // high quality color (opaque or alpha), 8 bpp
};

/// Name used in .zmeta files ("Auto", "BC7", "None"...) and back (unknown names -> Auto).
StringView TextureCompressionName(TextureCompression c);
TextureCompression ParseTextureCompression(StringView name);

/// What the cooker measured on an HDR image, read as a lat-long environment (u = 0.5 looks along +Z, v = 0 is +Y up).
/// Values are in the image's units (scale them by the sky intensity).
struct HdrInfo
{
    bool valid = false;
    Vec3 average;              // mean radiance over the sphere
    bool hasSun = false;       // a distinct sun disk was found
    Vec3 sunDirection = Vec3(0, 1, 0);
    Vec3 sunIlluminance;       // integral of the sun's radiance above sunClamp (RGB): lights the scene as a directional light
    f32 sunCosRadius = 1.0f;   // cone around sunDirection that holds the sun
    f32 sunClamp = 0.0f;       // luminance the sky lighting is clamped to inside the cone (the rest is the directional light)
};

/// Lat-long direction of texture coordinate (u, v) and back (see HdrInfo).
Vec3 LatLongDirection(f32 u, f32 v);
Vec2 LatLongUV(Vec3 direction);
/// Measures an RGBA32F lat-long environment (average radiance, sun).
HdrInfo AnalyzeHdrEnvironment(const Image& image);

struct TextureCookOptions
{
    bool srgb = true;
    bool normalMap = false;
    bool generateMips = true;
    TextureCompression compression = TextureCompression::Auto;
    texture::MipFilter mipFilter = texture::MipFilter::Kaiser;
    /// Largest allowed dimension (0 = unlimited); bigger sources drop their top mips (Lite profile uses 1024).
    u32 maxSize = 0;
    /// Alpha-tested textures: keep alpha coverage constant across mips.
    bool preserveAlphaCoverage = false;
    f32 alphaCutoff = 0.5f;
};

struct CookedTexture
{
    String name;
    TextureFormat format = TextureFormat::RGBA8;
    u32 width = 0;
    u32 height = 0;
    TextureWrap wrapU = TextureWrap::Repeat;
    TextureWrap wrapV = TextureWrap::Repeat;
    Array<Array<u8>> mips;
    HdrInfo hdr; // HDR sources only

    u64 MemorySize() const;
};

Result<CookedTexture> CookTexture(const Image& image, const TextureCookOptions& options);

// --- Material ---------------------------------------------------------------------------------------------------------

enum class ShadingModel : u8
{
    DefaultLit,
    Unlit,
};

struct MaterialAsset
{
    String name;
    ShadingModel shadingModel = ShadingModel::DefaultLit;
    AlphaMode alphaMode = AlphaMode::Opaque;
    f32 alphaCutoff = 0.5f;
    bool doubleSided = false;
    Vec4 baseColor = Vec4(1, 1, 1, 1);
    f32 metallic = 0.0f;
    f32 roughness = 0.5f;
    Vec3 emissive = Vec3(0, 0, 0);
    f32 normalScale = 1.0f;
    f32 occlusionStrength = 1.0f;
    /// UV transform applied to all textures (KHR_texture_transform of the base color texture).
    Vec2 uvScale = Vec2(1, 1);
    Vec2 uvOffset = Vec2(0, 0);
    f32 uvRotation = 0.0f;
    AssetId baseColorTexture;
    AssetId metallicRoughnessTexture;
    AssetId normalTexture;
    AssetId occlusionTexture;
    AssetId emissiveTexture;
};

// --- Prefab -----------------------------------------------------------------------------------------------------------

struct PrefabNode
{
    String name;
    Transform local;
    i32 parent = -1;
    AssetId mesh;
    Array<AssetId> materials; // one per mesh material slot
    // Skinned mesh: its skeleton, the clips made for it and the skeleton -> node transform (see ImportedNode).
    AssetId skeleton;
    Mat4 skinToNode = Mat4::Identity();
    Array<AssetId> animations;
    // Mesh with morph targets: their names and default weights.
    Array<String> morphNames;
    Array<f32> morphWeights;
};

struct PrefabAsset
{
    String name;
    Array<PrefabNode> nodes; // parents always precede children
};

// --- Serialization ----------------------------------------------------------------------------------------------------

void Serialize(const CookedMesh& mesh, Array<u8>& out);
void Serialize(const CookedTexture& texture, Array<u8>& out);
void Serialize(const MaterialAsset& material, Array<u8>& out);
void Serialize(const PrefabAsset& prefab, Array<u8>& out);
void Serialize(const Skeleton& skeleton, Array<u8>& out);
void Serialize(const AnimationClip& clip, Array<u8>& out);
/// Reads the container header; returns Unknown for invalid data.
AssetType PeekAssetType(ByteSpan data);
bool Deserialize(ByteSpan data, CookedMesh& out);
bool Deserialize(ByteSpan data, CookedTexture& out);
bool Deserialize(ByteSpan data, MaterialAsset& out);
bool Deserialize(ByteSpan data, PrefabAsset& out);
bool Deserialize(ByteSpan data, Skeleton& out);
bool Deserialize(ByteSpan data, AnimationClip& out);

/// Bump when any cooked format or cooker output changes (part of every DDC key).
inline constexpr u32 CookerVersion = 8; // 6: ZNano clusters, 7: fallback LODs cut from the cluster DAG, 8: unique group errors

} // namespace zx::asset
