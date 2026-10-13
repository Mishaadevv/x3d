#pragma once

// GPU side of assets: uploads cooked meshes/textures from the AssetManager on demand (with a per-frame byte budget)
// and maintains the GPU material table (bindless ByteAddressBuffer, see Shaders/Forward/ForwardPBR.hlsl).
//
// Everything is pull-based: GetMesh/GetMaterialIndex request loads and return null/the default material until the
// data is resident, so a scene can be drawn while it streams in.

#include "Asset/AssetManager.h"
#include "RHI/RHI.h"
#include "Render/MaterialShaderCache.h"

namespace zx::render {

using asset::AssetId;

struct GpuMesh
{
    rhi::Buffer* vertexBuffer = nullptr;
    rhi::Buffer* indexBuffer = nullptr;
    bool index16 = false;
    Array<asset::CookedMeshSection> sections;
    Array<asset::CookedSectionLod> sectionLods;
    u32 lodCount = 1;
    const asset::CookedSectionLod& SectionLodAt(const asset::CookedMeshSection& s, u32 lod) const
    {
        return sectionLods[s.firstLod + Min(lod, s.lodCount - 1)];
    }
    AABB bounds;
    Sphere sphere;
    u32 materialSlots = 0;
    u64 bytes = 0;
    // LOD0 triangles kept on the CPU for ray queries (picking, editor tools, gameplay raycasts).
    Array<Vec3> cpuPositions;
    Array<u32> cpuIndices;
    Array<UVec2> cpuSections; // per section: (first index, index count) into cpuIndices
};

enum class MaterialBlend : u8
{
    Opaque,
    Masked,
    Translucent,
};

/// CPU-side material properties needed to choose pipelines and sort draws.
struct MaterialInfo
{
    MaterialBlend blend = MaterialBlend::Opaque;
    bool doubleSided = false;
    u32 shader = 0; // MaterialShaderCache id of a material graph (0 = standard metallic-roughness shader)
};

/// A material graph instance as the renderer sees it (see Render/MaterialGraph.h for the source).
struct GraphMaterialDesc
{
    u64 shaderHash = 0;
    String shaderCode;
    MaterialBlend blend = MaterialBlend::Opaque;
    bool doubleSided = false;
    bool unlit = false;
    f32 alphaCutoff = 0.5f;
    Array<u8> params;                         // parameter block (textures as 0xFFFFFFFF placeholders)
    Array<Pair<u32, AssetId>> textures;       // block offset -> texture asset
};

class RenderAssets
{
public:
    RenderAssets(rhi::Device& device, asset::AssetManager& assets);
    ~RenderAssets();
    ZX_NON_COPYABLE(RenderAssets)

    bool Init(u32 maxMaterials = 4096);

    /// Uploads newly loaded assets (within the byte budget) and the dirty part of the material table.
    void Update(rhi::CommandList& cmd);
    void SetUploadBudget(u64 bytesPerFrame) { m_uploadBudget = bytesPerFrame; }
    /// Changes whenever something that renders differently arrived or changed (meshes, textures, materials, compiled
    /// material shaders) - reflection probes recapture on it.
    u64 ContentVersion() const { return m_contentVersion; }

    /// Null until resident.
    const GpuMesh* GetMesh(AssetId id);
    /// Index into the material table; 0 (default material) until the asset is loaded.
    u32 GetMaterialIndex(AssetId id);
    const MaterialInfo& GetMaterialInfo(u32 index) const { return m_materialInfos[index]; }
    /// Bindless SRV index of the texture, or InvalidDescriptor if not resident yet.
    u32 GetTextureSrv(AssetId id);
    /// The resident texture (requests the load like GetTextureSrv); null until uploaded.
    rhi::Texture* GetTexture(AssetId id);
    /// What the cooker measured on an HDR texture (environment average, sun); null until resident or for LDR textures.
    const asset::HdrInfo* GetTextureHdrInfo(AssetId id);

    rhi::Buffer* GetMaterialBuffer() const { return m_materialBuffer; }

    // --- Material graphs ---
    /// Creates or replaces the graph material `id` (row, parameter block, shader). Meshes referencing `id` use it at
    /// once (with the standard shader until the graph's shader is compiled). Returns the material index.
    u32 SetGraphMaterial(AssetId id, const GraphMaterialDesc& desc);
    /// Updates only parameter values (same layout): no recompilation, visible next frame.
    void SetGraphMaterialParams(AssetId id, Span<const u8> params);
    bool IsGraphMaterial(AssetId id) const { return m_graphs.Contains(id); }
    rhi::Buffer* GetMaterialParamBuffer() const { return m_paramBuffer; }
    MaterialShaderCache& Shaders() { return m_shaders; }
    const MaterialShaderCache& Shaders() const { return m_shaders; }

    struct Stats
    {
        u32 meshes = 0;
        u32 textures = 0;
        u32 materials = 0;
        u64 meshBytes = 0;
        u64 textureBytes = 0;
        u32 pendingUploads = 0;
    };
    Stats GetStats() const;

private:
    struct TextureEntry
    {
        rhi::Texture* texture = nullptr;
        bool requested = false;
        bool failed = false;
        u64 bytes = 0;
        asset::HdrInfo hdr;
    };
    struct MaterialEntry
    {
        u32 index = 0;
        bool resolved = false;      // asset loaded and row written
        bool texturesPending = false; // some texture not resident yet
    };
    struct MeshEntry
    {
        GpuMesh mesh;
        bool resident = false;
        bool requested = false;
        bool failed = false;
    };

    struct GraphEntry
    {
        u32 index = 0;
        u32 paramOffset = 0;
        u32 paramCapacity = 0;
        Array<u8> params;
        Array<Pair<u32, AssetId>> textures;
        bool texturesPending = false;
    };

    void WriteMaterialRow(u32 index, const asset::MaterialAsset& m, bool& texturesPending);
    u32 AllocateMaterialIndex(AssetId id, bool load);
    /// Copies the graph's parameters (with resolved texture SRVs) into the parameter buffer mirror.
    void WriteGraphParams(GraphEntry& g);
    bool TryUploadMesh(AssetId id, MeshEntry& e, u64& budget);
    bool TryUploadTexture(AssetId id, TextureEntry& e, u64& budget);

    rhi::Device& m_device;
    asset::AssetManager& m_assets;
    HashMap<AssetId, MeshEntry> m_meshes;
    HashMap<AssetId, TextureEntry> m_textures;
    HashMap<AssetId, MaterialEntry> m_materials;
    Array<AssetId> m_pendingMeshes;
    Array<AssetId> m_pendingTextures;
    Array<AssetId> m_pendingMaterials;

    rhi::Buffer* m_materialBuffer = nullptr;
    Array<u8> m_materialRows; // CPU mirror
    Array<MaterialInfo> m_materialInfos;
    u32 m_materialCount = 1; // 0 = default material
    u32 m_maxMaterials = 0;
    u32 m_dirtyBegin = 0, m_dirtyEnd = 1;

    MaterialShaderCache m_shaders;
    HashMap<AssetId, GraphEntry> m_graphs;
    rhi::Buffer* m_paramBuffer = nullptr;
    Array<u8> m_paramBytes; // CPU mirror
    u32 m_paramUsed = 0;
    u32 m_paramDirtyBegin = 0, m_paramDirtyEnd = 0;
    u64 m_uploadBudget = 16ull << 20;
    u64 m_contentVersion = 1;
    u32 m_readyShaders = 0;
};

} // namespace zx::render
