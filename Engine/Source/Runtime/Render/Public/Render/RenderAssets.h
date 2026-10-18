#pragma once

// GPU side of assets: uploads cooked meshes/textures from the AssetManager on demand (with a per-frame byte budget)
// and maintains the GPU material table (bindless ByteAddressBuffer, see Shaders/Forward/ForwardPBR.hlsl).
//
// Everything is pull-based: GetMesh/GetMaterialIndex request loads and return null/the default material until the
// data is resident, so a scene can be drawn while it streams in.

#include "Asset/AssetManager.h"
#include "Core/HAL/PlatformFile.h"
#include "RHI/RHI.h"
#include "Render/MaterialShaderCache.h"

namespace zx::render {

using asset::AssetId;

struct GpuMesh
{
    rhi::Buffer* vertexBuffer = nullptr;
    rhi::Buffer* indexBuffer = nullptr;
    rhi::Buffer* skinBuffer = nullptr; // skinned meshes: packed bone indices + weights per vertex (GPU skinning input)
    rhi::Buffer* morphBuffer = nullptr; // morph target offsets (CookedMesh::morphDeltas)
    u32 morphCount = 0;
    Array<String> morphNames;
    u32 vertexCount = 0;
    u32 boneCount = 0;
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
    // ZNano virtual geometry (CookedMesh::nano). The cluster headers stay resident (page-relative offsets, see
    // asset::nano::BuildGpuPages); the clusters' vertices and triangles stream in pages through RenderAssets' page pool,
    // coarse levels first, so the resident part is always a prefix of pages. The vertex / index buffers above then
    // hold only the fallback LOD chain from LOD1 down (CPU path, point / spot shadows, velocity, translucent parts).
    rhi::Buffer* nanoBuffer = nullptr; // headers
    u32 nanoClusters = 0;
    u32 nanoLeafTriangles = 0;
    Sphere nanoSphere;           // mesh space: holds every cluster's and parent group's sphere
    Array<f32> nanoParentErrors; // per cluster, descending: the clusters an instance can need are a prefix
    u32 nanoPages = 0;
    u32 nanoPinnedPages = 1;     // the roots: always resident before the mesh draws as clusters
    Array<u32> nanoPageFirstCluster;
    u64 nanoFileOffset = 0;      // page i at nanoFileOffset + i * PageBytes in RenderAssets' page file
    u32 nanoResidentPages = 0;   // pages [0, resident) are in the pool at nanoSlots
    Array<u32> nanoSlots;
    u32 nanoWantedPages = 0;     // requested this frame (RenderAssets::RequestNanoClusters)
    u64 nanoLastWanted = 0;
    bool NanoReady() const { return nanoBuffer && nanoResidentPages >= nanoPinnedPages; }
    u32 NanoResidentClusters() const { return nanoResidentPages < nanoPages ? nanoPageFirstCluster[nanoResidentPages] : nanoClusters; }
    /// Clusters whose own error is below this have no resident children: they are drawn instead (-1: all resident).
    f32 NanoResidentError() const
    {
        const u32 resident = NanoResidentClusters();
        return resident < nanoClusters && resident > 0 ? nanoParentErrors[resident - 1] : -1.0f;
    }
    /// Pages that hold the first `clusters` clusters.
    u32 NanoPagesFor(u32 clusters) const
    {
        u32 lo = 0, hi = nanoPages; // first page starting at or after `clusters`
        while (lo < hi) {
            const u32 mid = (lo + hi) / 2;
            if (nanoPageFirstCluster[mid] < clusters)
                lo = mid + 1;
            else
                hi = mid;
        }
        return lo;
    }
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

    /// Null until resident. A resident mesh's address stays valid until ReloadMesh (MeshGeneration changes then).
    const GpuMesh* GetMesh(AssetId id);
    u64 MeshGeneration() const { return m_meshGeneration; }
    /// Forgets a mesh (its GPU copy is freed once the GPU is done with it); the next GetMesh loads it again. For
    /// memory meshes that were replaced (generated terrain).
    void ReloadMesh(AssetId id);
    /// Rewrites vertices [firstVertex, firstVertex + count) of a resident mesh in place (uploaded by the next Update) and
    /// its bounds - for meshes edited live (terrain sculpting) without a reload (no flicker, no probe recapture).
    /// Returns false if the mesh is not resident (then reload it instead).
    bool UpdateMeshVertices(AssetId id, u32 firstVertex, Span<const asset::PackedVertex> vertices, const AABB& bounds,
                            Span<const asset::CookedMeshSection> sections);
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

    // --- Virtual geometry streaming ---
    /// The first `clusters` clusters of a nano mesh are needed this frame (by any view); pages stream in from the next
    /// Update, within the pool and the per-frame budget, evicting pages nobody asked for lately.
    void RequestNanoClusters(const GpuMesh* mesh, u32 clusters);
    /// The page pool (raw buffer: vertices of 36 bytes + packed triangles), invalid before the first nano mesh.
    u32 GetNanoPoolSrv() const { return m_nanoPool ? m_nanoPool->GetSrvIndex() : rhi::InvalidDescriptor; }
    /// Before the first nano mesh loads: GPU memory for streamed cluster pages.
    void SetNanoPoolBytes(u64 bytes) { m_nanoPoolBytes = bytes; }
    void SetNanoPagesPerFrame(u32 pages) { m_nanoPagesPerFrame = Max(pages, 1u); }
    const MaterialShaderCache& Shaders() const { return m_shaders; }

    struct Stats
    {
        u32 meshes = 0;
        u32 textures = 0;
        u32 materials = 0;
        u64 meshBytes = 0;
        u64 textureBytes = 0;
        u32 pendingUploads = 0;
        u32 nanoPoolPages = 0;     // pool capacity
        u32 nanoResidentPages = 0;
        u32 nanoWantedPages = 0;   // asked for last frame
        u32 nanoStreamedPages = 0; // loaded by the last Update
        u64 nanoFileBytes = 0;     // all pages of all nano meshes
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
    bool PrepareNanoMesh(const asset::CookedMesh& m, GpuMesh& g); // headers, pages into the page file
    void UpdateNanoStreaming(rhi::CommandList& cmd);
    void ReleaseNanoPages(GpuMesh& g);

    rhi::Device& m_device;
    asset::AssetManager& m_assets;
    HashMap<AssetId, UniquePtr<MeshEntry>> m_meshes; // stable addresses: GetMesh pointers survive later loads
    u64 m_meshGeneration = 0;
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
    struct VertexUpdate
    {
        rhi::Buffer* buffer = nullptr;
        u64 offset = 0;
        Array<asset::PackedVertex> data;
    };
    Array<VertexUpdate> m_vertexUpdates;
    u64 m_contentVersion = 1;
    // Virtual geometry streaming: fixed-size page slots in one pool buffer; pages come from a scratch file.
    rhi::Buffer* m_nanoPool = nullptr;
    u64 m_nanoPoolBytes = 160ull << 20;
    u32 m_nanoPagesPerFrame = 24;
    u32 m_nanoSlotCount = 0;
    Array<u32> m_nanoFreeSlots;
    struct RetiredSlot
    {
        u32 slot;
        u64 frame;
    };
    Array<RetiredSlot> m_nanoRetired; // reusable once the GPU frames that read them are done
    Array<GpuMesh*> m_nanoMeshes;
    platform::File m_nanoFile;
    String m_nanoFilePath;
    u64 m_nanoFileSize = 0;
    Array<u8> m_nanoScratch;
    u64 m_frame = 0;
    u32 m_nanoStreamed = 0;
    u32 m_nanoWantedTotal = 0;
    u32 m_readyShaders = 0;
};

} // namespace zx::render
