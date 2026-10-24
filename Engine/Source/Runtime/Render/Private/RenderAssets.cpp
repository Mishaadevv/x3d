#include "Render/RenderAssets.h"
#include "Asset/Nano.h"
#include "Core/Log/Log.h"
#include "Core/String/Path.h"
#include "Core/Time/Time.h"

ZX_DEFINE_LOG_CATEGORY_STATIC(LogRenderAssets, Info);

namespace zx::render {

using namespace rhi;
using asset::AssetState;

namespace {

constexpr u32 MaterialStride = 96; // keep in sync with ForwardPBR.hlsl
constexpr u32 MaterialGraphFlag = 8;
constexpr u32 ParamBufferBytes = 1u << 20; // material graph parameter blocks
constexpr u32 MaterialUnlit = 1, MaterialAlphaTest = 2, MaterialDoubleSided = 4;

Format ToRhiFormat(asset::TextureFormat f)
{
    switch (f) {
    case asset::TextureFormat::RGBA8: return Format::R8G8B8A8_UNORM;
    case asset::TextureFormat::RGBA8_SRGB: return Format::R8G8B8A8_SRGB;
    case asset::TextureFormat::BC1: return Format::BC1_UNORM;
    case asset::TextureFormat::BC1_SRGB: return Format::BC1_SRGB;
    case asset::TextureFormat::BC3: return Format::BC3_UNORM;
    case asset::TextureFormat::BC3_SRGB: return Format::BC3_SRGB;
    case asset::TextureFormat::BC4: return Format::BC4_UNORM;
    case asset::TextureFormat::BC5: return Format::BC5_UNORM;
    case asset::TextureFormat::RGBA16F: return Format::R16G16B16A16_FLOAT;
    case asset::TextureFormat::RGBA32F: return Format::R32G32B32A32_FLOAT;
    case asset::TextureFormat::BC6H: return Format::BC6H_UF16;
    case asset::TextureFormat::BC7: return Format::BC7_UNORM;
    case asset::TextureFormat::BC7_SRGB: return Format::BC7_SRGB;
    }
    return Format::Unknown;
}

struct GpuMaterialRow
{
    Vec4 baseColor;
    Vec3 emissive;
    f32 metallic;
    f32 roughness;
    f32 normalScale;
    f32 occlusionStrength;
    f32 alphaCutoff;
    u32 baseColorTexture;
    u32 metallicRoughnessTexture;
    u32 normalTexture;
    u32 occlusionTexture;
    u32 emissiveTexture;
    u32 flags;
    f32 uvRotation;
    u32 paramOffset; // material graphs
    Vec2 uvScale;
    Vec2 uvOffset;
};
static_assert(sizeof(GpuMaterialRow) == MaterialStride);

} // namespace

RenderAssets::RenderAssets(Device& device, asset::AssetManager& assets) : m_device(device), m_assets(assets) {}

RenderAssets::~RenderAssets()
{
    for (auto& kv : m_meshes) {
        m_device.Destroy(kv.second->mesh.vertexBuffer);
        m_device.Destroy(kv.second->mesh.skinBuffer);
        m_device.Destroy(kv.second->mesh.morphBuffer);
        m_device.Destroy(kv.second->mesh.indexBuffer);
        m_device.Destroy(kv.second->mesh.nanoBuffer);
    }
    m_device.Destroy(m_nanoPool);
    if (m_nanoFile.IsOpen()) {
        m_nanoFile.Close();
        platform::DeleteFileAt(m_nanoFilePath);
    }
    for (auto& kv : m_textures)
        m_device.Destroy(kv.second.texture);
    m_device.Destroy(m_materialBuffer);
    m_device.Destroy(m_paramBuffer);
}

bool RenderAssets::Init(u32 maxMaterials)
{
    m_maxMaterials = maxMaterials;
    m_materialBuffer = m_device.CreateBuffer(
        BufferDesc{u64(maxMaterials) * MaterialStride, BufferUsage::ShaderResource, MemoryType::GpuOnly, 0, "MaterialTable"});
    if (!m_materialBuffer)
        return false;
    m_materialRows.ResizeZeroed(usize(maxMaterials) * MaterialStride);
    m_paramBuffer = m_device.CreateBuffer(BufferDesc{ParamBufferBytes, BufferUsage::ShaderResource, MemoryType::GpuOnly, 0, "MaterialParams"});
    if (!m_paramBuffer)
        return false;
    m_paramBytes.ResizeZeroed(ParamBufferBytes);
    m_materialInfos.Resize(maxMaterials);
    // Default material (index 0): light grey, rough dielectric - shown while a material streams in.
    asset::MaterialAsset def;
    def.baseColor = Vec4(0.7f, 0.7f, 0.7f, 1.0f);
    def.roughness = 0.8f;
    bool pending = false;
    WriteMaterialRow(0, def, pending);
    return true;
}

u32 RenderAssets::GetTextureSrv(AssetId id)
{
    if (!id.IsValid())
        return InvalidDescriptor;
    TextureEntry* e = m_textures.Find(id);
    if (!e) {
        e = &m_textures.InsertOrAssign(id, TextureEntry{});
        e->requested = true;
        m_assets.Load(id);
        m_pendingTextures.PushBack(id);
    }
    return e->texture ? e->texture->GetSrvIndex() : InvalidDescriptor;
}

rhi::Texture* RenderAssets::GetTexture(AssetId id)
{
    if (GetTextureSrv(id) == InvalidDescriptor)
        return nullptr;
    return m_textures.Find(id)->texture;
}

const asset::HdrInfo* RenderAssets::GetTextureHdrInfo(AssetId id)
{
    if (GetTextureSrv(id) == InvalidDescriptor)
        return nullptr;
    const TextureEntry* e = m_textures.Find(id);
    return e->hdr.valid ? &e->hdr : nullptr;
}

void RenderAssets::WriteMaterialRow(u32 index, const asset::MaterialAsset& m, bool& texturesPending)
{
    GpuMaterialRow row = {};
    row.baseColor = m.baseColor;
    row.emissive = m.emissive;
    row.metallic = m.metallic;
    row.roughness = m.roughness;
    row.normalScale = m.normalScale;
    row.occlusionStrength = m.occlusionStrength;
    row.alphaCutoff = m.alphaCutoff;
    auto tex = [&](AssetId id) {
        const u32 srv = GetTextureSrv(id);
        if (id.IsValid() && srv == InvalidDescriptor) {
            const TextureEntry* e = m_textures.Find(id);
            texturesPending |= !(e && e->failed);
        }
        return srv;
    };
    row.baseColorTexture = tex(m.baseColorTexture);
    row.metallicRoughnessTexture = tex(m.metallicRoughnessTexture);
    row.normalTexture = tex(m.normalTexture);
    row.occlusionTexture = tex(m.occlusionTexture);
    row.emissiveTexture = tex(m.emissiveTexture);
    row.flags = (m.shadingModel == asset::ShadingModel::Unlit ? MaterialUnlit : 0) |
                (m.alphaMode == asset::AlphaMode::Mask ? MaterialAlphaTest : 0) | (m.doubleSided ? MaterialDoubleSided : 0);
    row.uvRotation = m.uvRotation;
    row.uvScale = m.uvScale;
    row.uvOffset = m.uvOffset;
    MemCopy(m_materialRows.Data() + usize(index) * MaterialStride, &row, MaterialStride);
    MaterialInfo& info = m_materialInfos[index];
    info.blend = m.alphaMode == asset::AlphaMode::Mask    ? MaterialBlend::Masked
                 : m.alphaMode == asset::AlphaMode::Blend ? MaterialBlend::Translucent
                                                          : MaterialBlend::Opaque;
    info.doubleSided = m.doubleSided;
    m_dirtyBegin = Min(m_dirtyBegin, index);
    m_dirtyEnd = Max(m_dirtyEnd, index + 1);
}

u32 RenderAssets::AllocateMaterialIndex(AssetId id, bool load)
{
    if (const MaterialEntry* e = m_materials.Find(id))
        return e->index;
    if (m_materialCount >= m_maxMaterials) {
        ZX_LOG_WARN(LogRenderAssets, "Material table full ({} entries)", m_maxMaterials);
        return 0;
    }
    MaterialEntry& e = m_materials.InsertOrAssign(id, MaterialEntry{});
    e.index = m_materialCount++;
    // Default look until loaded.
    MemCopy(m_materialRows.Data() + usize(e.index) * MaterialStride, m_materialRows.Data(), MaterialStride);
    m_dirtyEnd = Max(m_dirtyEnd, e.index + 1);
    m_dirtyBegin = Min(m_dirtyBegin, e.index);
    if (load) {
        m_assets.Load(id);
        m_pendingMaterials.PushBack(id);
    }
    return e.index;
}

u32 RenderAssets::GetMaterialIndex(AssetId id)
{
    if (!id.IsValid())
        return 0;
    return AllocateMaterialIndex(id, true);
}

u32 RenderAssets::SetGraphMaterial(AssetId id, const GraphMaterialDesc& desc)
{
    if (!id.IsValid())
        return 0;
    // An id first requested as a regular material (scene loaded before the graph registered) switches to the graph.
    m_pendingMaterials.Remove(id);
    const u32 index = AllocateMaterialIndex(id, false);
    if (index == 0)
        return 0;
    GraphEntry* g = m_graphs.Find(id);
    if (!g) {
        g = &m_graphs.InsertOrAssign(id, GraphEntry{});
        g->index = index;
    }
    const u32 size = AlignUp(Max<u32>(u32(desc.params.Size()), 16u), 16u);
    if (size > g->paramCapacity) {
        // Bump allocation; a grown block moves (the old space is not reused).
        if (m_paramUsed + size > ParamBufferBytes) {
            ZX_LOG_WARN(LogRenderAssets, "Material parameter buffer full");
            return index;
        }
        g->paramOffset = m_paramUsed;
        g->paramCapacity = size;
        m_paramUsed += size;
    }
    g->params = desc.params;
    g->textures = desc.textures;
    WriteGraphParams(*g);

    // Row: neutral look for the standard shader (used until the graph shader is ready) + the graph's block offset.
    GpuMaterialRow row = {};
    row.baseColor = Vec4(0.6f, 0.6f, 0.6f, 1.0f);
    row.roughness = 0.6f;
    row.normalScale = 1.0f;
    row.occlusionStrength = 1.0f;
    row.alphaCutoff = desc.alphaCutoff;
    row.baseColorTexture = row.metallicRoughnessTexture = row.normalTexture = row.occlusionTexture = row.emissiveTexture = InvalidDescriptor;
    row.flags = MaterialGraphFlag | (desc.unlit ? MaterialUnlit : 0) | (desc.blend == MaterialBlend::Masked ? MaterialAlphaTest : 0) |
                (desc.doubleSided ? MaterialDoubleSided : 0);
    row.paramOffset = g->paramOffset;
    row.uvScale = Vec2(1.0f, 1.0f);
    MemCopy(m_materialRows.Data() + usize(index) * MaterialStride, &row, MaterialStride);
    m_dirtyBegin = Min(m_dirtyBegin, index);
    m_dirtyEnd = Max(m_dirtyEnd, index + 1);
    MaterialInfo& info = m_materialInfos[index];
    info.blend = desc.blend;
    info.doubleSided = desc.doubleSided;
    info.shader = desc.shaderCode.Empty() ? 0 : m_shaders.Request(desc.shaderHash, desc.shaderCode, desc.blend == MaterialBlend::Masked);
    if (MaterialEntry* e = m_materials.Find(id))
        e->resolved = true;
    return index;
}

void RenderAssets::SetGraphMaterialParams(AssetId id, Span<const u8> params)
{
    GraphEntry* g = m_graphs.Find(id);
    if (!g || params.Size() > g->paramCapacity)
        return;
    g->params.Clear();
    g->params.Append(params);
    WriteGraphParams(*g);
}

void RenderAssets::WriteGraphParams(GraphEntry& g)
{
    u8* dst = m_paramBytes.Data() + g.paramOffset;
    MemCopy(dst, g.params.Data(), g.params.Size());
    g.texturesPending = false;
    for (const Pair<u32, AssetId>& t : g.textures) {
        if (t.first + 4 > g.params.Size())
            continue;
        const u32 srv = GetTextureSrv(t.second);
        if (t.second.IsValid() && srv == InvalidDescriptor) {
            const TextureEntry* e = m_textures.Find(t.second);
            g.texturesPending |= !(e && e->failed);
        }
        MemCopy(dst + t.first, &srv, 4);
    }
    if (m_paramDirtyEnd <= m_paramDirtyBegin) {
        m_paramDirtyBegin = g.paramOffset;
        m_paramDirtyEnd = g.paramOffset + g.paramCapacity;
    } else {
        m_paramDirtyBegin = Min(m_paramDirtyBegin, g.paramOffset);
        m_paramDirtyEnd = Max(m_paramDirtyEnd, g.paramOffset + g.paramCapacity);
    }
}

const GpuMesh* RenderAssets::GetMesh(AssetId id)
{
    if (!id.IsValid())
        return nullptr;
    UniquePtr<MeshEntry>* found = m_meshes.Find(id);
    MeshEntry* e = found ? found->Get() : nullptr;
    if (!e) {
        e = m_meshes.InsertOrAssign(id, MakeUnique<MeshEntry>()).Get();
        e->requested = true;
        m_assets.Load(id);
        m_pendingMeshes.PushBack(id);
    }
    return e->resident ? &e->mesh : nullptr;
}

void RenderAssets::ReloadMesh(AssetId id)
{
    UniquePtr<MeshEntry>* found = m_meshes.Find(id);
    if (!found)
        return;
    MeshEntry* e = found->Get();
    ++m_meshGeneration; // cached GpuMesh pointers die here
    m_vertexUpdates.RemoveIf([&](const VertexUpdate& u) { return u.buffer == e->mesh.vertexBuffer; });
    m_device.Destroy(e->mesh.vertexBuffer);
    m_device.Destroy(e->mesh.skinBuffer);
    e->mesh.skinBuffer = nullptr;
    m_device.Destroy(e->mesh.morphBuffer);
    e->mesh.morphBuffer = nullptr;
    m_device.Destroy(e->mesh.indexBuffer);
    ReleaseNanoPages(e->mesh);
    m_device.Destroy(e->mesh.nanoBuffer);
    e->mesh.nanoBuffer = nullptr;
    m_meshes.Remove(id);
    for (usize i = 0; i < m_pendingMeshes.Size(); ++i)
        if (m_pendingMeshes[i] == id) {
            m_pendingMeshes.RemoveAtSwap(i);
            break;
        }
    ++m_contentVersion;
}

bool RenderAssets::TryUploadMesh(AssetId id, MeshEntry& e, u64& budget)
{
    const AssetState state = m_assets.GetState(id);
    if (state == AssetState::Failed) {
        e.failed = true;
        return true;
    }
    const asset::CookedMesh* m = state == AssetState::Loaded ? m_assets.GetMesh(id) : nullptr;
    if (!m)
        return false;
    const u64 bytes = m->MemorySize();
    if (bytes > budget && budget != m_uploadBudget)
        return false; // wait for the next frame (always allow one item per frame)
    budget -= Min(budget, bytes);
    GpuMesh& g = e.mesh;
    g.sections = m->sections;
    g.lightmapUVs = m->lightmapUVs && m->lightmapResolution > 0;
    g.lightmapResolution = m->lightmapResolution;
    g.sectionLods = m->sectionLods;
    g.lodCount = m->lodCount;
    g.index16 = m->index16;
    const bool nano = !m->nano.Empty() && PrepareNanoMesh(*m, g);
    if (nano) {
        // Clusters draw the detail; the vertex / index buffers get the fallback chain from LOD1 on (sections without
        // one keep LOD0), compacted to the vertices it uses.
        Array<u32> remap;
        remap.Resize(m->vertices.Size(), InvalidIndex);
        Array<asset::PackedVertex> vertices;
        Array<u32> indices;
        Array<asset::CookedSectionLod> lods;
        u32 lodCount = 1;
        for (asset::CookedMeshSection& s : g.sections) {
            const u32 first = s.lodCount > 1 ? 1 : 0;
            const u32 firstLod = u32(lods.Size());
            for (u32 l = first; l < s.lodCount; ++l) {
                const asset::CookedSectionLod& src = m->SectionLod(s, l);
                const u32 offset = u32(indices.Size());
                for (u32 i = 0; i < src.indexCount; ++i) {
                    const u32 v = m->Index(src.indexOffset + i);
                    if (remap[v] == InvalidIndex) {
                        remap[v] = u32(vertices.Size());
                        vertices.PushBack(m->vertices[v]);
                    }
                    indices.PushBack(remap[v]);
                }
                lods.PushBack(asset::CookedSectionLod{offset, src.indexCount, l == first ? 1.0f : src.screenSize, src.error});
            }
            s.firstLod = firstLod;
            s.lodCount -= first;
            lodCount = Max(lodCount, s.lodCount);
        }
        g.sectionLods = move(lods);
        g.lodCount = lodCount;
        g.index16 = vertices.Size() <= 0xFFFF;
        Array<u8> indexData;
        indexData.ResizeUninitialized(indices.Size() * (g.index16 ? 2 : 4));
        for (usize i = 0; i < indices.Size(); ++i) {
            if (g.index16) {
                const u16 v = u16(indices[i]);
                MemCopy(indexData.Data() + i * 2, &v, 2);
            } else {
                MemCopy(indexData.Data() + i * 4, &indices[i], 4);
            }
        }
        g.vertexBuffer = m_device.CreateBuffer(
            BufferDesc{vertices.SizeBytes(), BufferUsage::ShaderResource, MemoryType::GpuOnly, 0, m->name.View()}, vertices.Data());
        g.indexBuffer = m_device.CreateBuffer(
            BufferDesc{indexData.Size(), BufferUsage::Index | BufferUsage::ShaderResource, MemoryType::GpuOnly, 0, m->name.View()},
            indexData.Data());
        g.vertexCount = u32(vertices.Size());
    } else {
        // Initial data goes through the device's staging copy (load-time path; an async copy queue comes later).
        g.vertexBuffer = m_device.CreateBuffer(
            BufferDesc{m->vertices.SizeBytes(), BufferUsage::ShaderResource, MemoryType::GpuOnly, 0, m->name.View()},
            m->vertices.Data());
        g.indexBuffer = m_device.CreateBuffer(
            BufferDesc{m->indexData.Size(), BufferUsage::Index | BufferUsage::ShaderResource, MemoryType::GpuOnly, 0, m->name.View()},
            m->indexData.Data());
        g.vertexCount = u32(m->vertices.Size());
    }
    if (m->IsSkinned())
        g.skinBuffer = m_device.CreateBuffer(
            BufferDesc{m->skin.SizeBytes(), BufferUsage::ShaderResource, MemoryType::GpuOnly, 0, m->name.View()}, m->skin.Data());
    if (m->MorphCount() > 0) {
        g.morphBuffer = m_device.CreateBuffer(
            BufferDesc{m->morphDeltas.SizeBytes(), BufferUsage::ShaderResource, MemoryType::GpuOnly, 0, m->name.View()}, m->morphDeltas.Data());
        g.morphCount = m->MorphCount();
        g.morphNames = m->morphNames;
    }
    g.boneCount = m->boneCount;
    g.bounds = m->bounds;
    g.sphere = Sphere::FromAABB(m->bounds);
    g.materialSlots = m->materialSlotCount;
    g.bytes = bytes;
    g.cpuPositions.Reserve(m->vertices.Size());
    for (const asset::PackedVertex& v : m->vertices)
        g.cpuPositions.PushBack(v.position);
    for (const asset::CookedMeshSection& s : m->sections) {
        const asset::CookedSectionLod& lod = m->SectionLod(s, 0);
        g.cpuSections.PushBack(UVec2(u32(g.cpuIndices.Size()), lod.indexCount));
        for (u32 i = 0; i < lod.indexCount; ++i)
            g.cpuIndices.PushBack(m->Index(lod.indexOffset + i));
    }
    if (nano)
        g.bytes = g.vertexBuffer->GetSize() + g.indexBuffer->GetSize() + g.nanoBuffer->GetSize();
    e.resident = g.vertexBuffer && g.indexBuffer;
    e.failed = !e.resident;
    return true;
}

bool RenderAssets::PrepareNanoMesh(const asset::CookedMesh& m, GpuMesh& g)
{
    namespace nano = asset::nano;
    if (!m_nanoFile.IsOpen()) {
        // One scratch file per run holds every nano mesh's pages (written once, read back as they stream in).
        const String dir = Path::Join(platform::GetTempDirectory(), "ZeqouX3D");
        platform::CreateDirectoryTree(dir);
        // Leftovers of runs that crashed (files other running instances hold open can't be deleted).
        Array<platform::DirectoryEntry> entries;
        if (platform::ListDirectory(dir, entries))
            for (const platform::DirectoryEntry& e : entries)
                if (!e.isDirectory && e.name.View().StartsWith("NanoPages-"))
                    platform::DeleteFileAt(Path::Join(dir, e.name.View()));
        m_nanoFilePath = Path::Join(dir, String::Format("NanoPages-{}.bin", Clock::NowTicks()).View());
        if (!m_nanoFile.Open(m_nanoFilePath, platform::FileMode::ReadWrite)) {
            ZX_LOG_ERROR(LogRenderAssets, "Can't create the cluster page file {}", m_nanoFilePath);
            return false;
        }
        m_nanoFileSize = 0;
    }
    if (!m_nanoPool) {
        m_nanoSlotCount = u32(Max<u64>(m_nanoPoolBytes / nano::PageBytes, 64));
        m_nanoPool = m_device.CreateBuffer(BufferDesc{u64(m_nanoSlotCount) * nano::PageBytes, BufferUsage::ShaderResource,
                                                      MemoryType::GpuOnly, 0, "Nano.PagePool"});
        if (!m_nanoPool)
            return false;
        for (u32 s = m_nanoSlotCount; s-- > 0;)
            m_nanoFreeSlots.PushBack(s);
        m_nanoScratch.Resize(nano::PageBytes);
    }
    nano::GpuPages pages;
    nano::BuildGpuPages(m, pages);
    if (!m_nanoFile.Seek(m_nanoFileSize) || m_nanoFile.Write(pages.data.Data(), pages.data.Size()) != pages.data.Size()) {
        ZX_LOG_ERROR(LogRenderAssets, "Can't write cluster pages of {}", m.name);
        return false;
    }
    g.nanoBuffer = m_device.CreateBuffer(BufferDesc{pages.headers.SizeBytes(), BufferUsage::ShaderResource, MemoryType::GpuOnly, 0,
                                                    m.name.View()},
                                         pages.headers.Data());
    if (!g.nanoBuffer)
        return false;
    g.nanoFileOffset = m_nanoFileSize;
    m_nanoFileSize += pages.data.Size();
    g.nanoClusters = u32(m.nano.clusters.Size());
    g.nanoLeafTriangles = m.nano.leafTriangles;
    g.nanoPages = u32(pages.pageFirstCluster.Size());
    g.nanoPinnedPages = Min(pages.pinnedPages, g.nanoPages);
    g.nanoPageFirstCluster = move(pages.pageFirstCluster);
    g.nanoResidentPages = 0;
    g.nanoSlots.Clear();
    const Sphere meshSphere = Sphere::FromAABB(m.bounds);
    f32 reach = meshSphere.radius;
    g.nanoParentErrors.Reserve(m.nano.clusters.Size());
    for (const asset::NanoClusterHeader& c : m.nano.clusters) {
        reach = Max(reach, Length(c.bounds.XYZ() - meshSphere.center) + c.bounds.w);
        reach = Max(reach, Length(c.selfSphere.XYZ() - meshSphere.center) + c.selfSphere.w);
        if (c.parentError < asset::NanoRootError)
            reach = Max(reach, Length(c.parentSphere.XYZ() - meshSphere.center) + c.parentSphere.w);
        g.nanoParentErrors.PushBack(c.parentError);
    }
    g.nanoSphere = Sphere{meshSphere.center, reach};
    m_nanoMeshes.PushBack(&g);
    return true;
}

void RenderAssets::RequestNanoClusters(const GpuMesh* mesh, u32 clusters)
{
    if (!mesh || !mesh->nanoBuffer)
        return;
    GpuMesh& g = const_cast<GpuMesh&>(*mesh); // residency is RenderAssets' own bookkeeping
    g.nanoWantedPages = Max(g.nanoWantedPages, g.NanoPagesFor(clusters));
    g.nanoLastWanted = m_frame;
}

void RenderAssets::ReleaseNanoPages(GpuMesh& g)
{
    for (u32 slot : g.nanoSlots)
        m_nanoRetired.PushBack(RetiredSlot{slot, m_frame});
    g.nanoSlots.Clear();
    g.nanoResidentPages = 0;
    m_nanoMeshes.Remove(&g);
}

void RenderAssets::UpdateNanoStreaming(CommandList& cmd)
{
    namespace nano = asset::nano;
    ++m_frame;
    m_nanoStreamed = 0;
    // Slots evicted FramesInFlight + 1 updates ago are no longer read by the GPU.
    for (usize i = 0; i < m_nanoRetired.Size();) {
        if (m_frame - m_nanoRetired[i].frame > FramesInFlight + 1) {
            m_nanoFreeSlots.PushBack(m_nanoRetired[i].slot);
            m_nanoRetired.RemoveAtSwap(i);
        } else {
            ++i;
        }
    }
    if (m_nanoMeshes.Empty())
        return;
    // What each mesh wants: its roots always, plus what the views asked for last frame. Pinned pages first, then the
    // rest round-robin by how far each mesh is from its goal.
    auto want = [](const GpuMesh* g) { return Max(g->nanoPinnedPages, Min(g->nanoWantedPages, g->nanoPages)); };
    u32 missing = 0;
    for (const GpuMesh* g : m_nanoMeshes)
        missing += want(g) > g->nanoResidentPages ? want(g) - g->nanoResidentPages : 0;
    const u32 toLoad = Min(missing, m_nanoPagesPerFrame);
    // Make room: evict the last pages of meshes holding more than they want, least recently wanted first.
    while (m_nanoFreeSlots.Size() + m_nanoRetired.Size() < toLoad) { // retired slots free up in a few frames
        GpuMesh* victim = nullptr;
        for (GpuMesh* g : m_nanoMeshes)
            if (g->nanoResidentPages > want(g) && (!victim || g->nanoLastWanted < victim->nanoLastWanted))
                victim = g;
        if (!victim) {
            // The pool is full of wanted pages: share it. The mesh furthest behind its goal takes the finest page of the
            // one closest to its own (pages are coarse first, so the tails matter least).
            const GpuMesh* needy = nullptr;
            f32 needyShare = 1.0f;
            for (const GpuMesh* g : m_nanoMeshes)
                if (g->nanoResidentPages < want(g)) {
                    const f32 share = f32(g->nanoResidentPages) / f32(want(g));
                    if (share < needyShare) {
                        needy = g;
                        needyShare = share;
                    }
                }
            f32 victimShare = 0.0f;
            for (GpuMesh* g : m_nanoMeshes)
                if (g != needy && g->nanoResidentPages > g->nanoPinnedPages) {
                    const f32 share = f32(g->nanoResidentPages - 1) / f32(Max(want(g), 1u));
                    if (share > victimShare) {
                        victim = g;
                        victimShare = share;
                    }
                }
            if (!needy || !victim || victimShare <= needyShare + 0.05f)
                break; // balanced: detail stays where it is
        }
        m_nanoRetired.PushBack(RetiredSlot{victim->nanoSlots.Back(), m_frame});
        victim->nanoSlots.PopBack();
        --victim->nanoResidentPages;
    }
    bool uploaded = false;
    for (u32 round = 0; m_nanoStreamed < toLoad && !m_nanoFreeSlots.Empty(); ++round) {
        bool progress = false;
        for (GpuMesh* g : m_nanoMeshes) {
            if (m_nanoStreamed >= toLoad || m_nanoFreeSlots.Empty())
                break;
            // One page per mesh per round (each mesh's pinned pages come first in its order).
            if (g->nanoResidentPages >= want(g))
                continue;
            const u32 page = g->nanoResidentPages;
            if (m_nanoFile.ReadAt(m_nanoScratch.Data(), nano::PageBytes, g->nanoFileOffset + u64(page) * nano::PageBytes) != nano::PageBytes) {
                ZX_LOG_ERROR(LogRenderAssets, "Can't read a cluster page");
                g->nanoWantedPages = g->nanoResidentPages; // stop asking
                continue;
            }
            const u32 slot = m_nanoFreeSlots.Back();
            m_nanoFreeSlots.PopBack();
            cmd.UploadBuffer(m_nanoPool, u64(slot) * nano::PageBytes, m_nanoScratch.Data(), nano::PageBytes);
            g->nanoSlots.PushBack(slot);
            ++g->nanoResidentPages;
            ++m_nanoStreamed;
            progress = uploaded = true;
        }
        if (!progress)
            break;
    }
    if (uploaded)
        cmd.Barrier(m_nanoPool, ResourceState::ShaderResource);
    m_nanoWantedTotal = 0;
    for (GpuMesh* g : m_nanoMeshes) {
        m_nanoWantedTotal += want(g);
        g->nanoWantedPages = 0; // the views ask again every frame
    }
}

bool RenderAssets::TryUploadTexture(AssetId id, TextureEntry& e, u64& budget)
{
    const AssetState state = m_assets.GetState(id);
    if (state == AssetState::Failed) {
        e.failed = true;
        return true;
    }
    const asset::CookedTexture* t = state == AssetState::Loaded ? m_assets.GetTexture(id) : nullptr;
    if (!t)
        return false;
    const u64 bytes = t->MemorySize();
    if (bytes > budget && budget != m_uploadBudget)
        return false;
    budget -= Min(budget, bytes);
    TextureDesc desc;
    desc.width = t->width;
    desc.height = t->height;
    desc.mipLevels = u32(t->mips.Size());
    desc.format = ToRhiFormat(t->format);
    desc.usage = TextureUsage::ShaderResource;
    desc.debugName = t->name.View();
    Array<SubresourceData> subresources;
    for (u32 m = 0; m < t->mips.Size(); ++m)
        subresources.PushBack(SubresourceData{t->mips[m].Data(), GetRowPitch(desc.format, Max(1u, desc.width >> m)), 0});
    e.texture = m_device.CreateTexture(desc, subresources.View());
    e.bytes = bytes;
    e.hdr = t->hdr;
    e.failed = e.texture == nullptr;
    m_assets.ReleaseCpuData(id);
    return true;
}

bool RenderAssets::UpdateMeshVertices(AssetId id, u32 firstVertex, Span<const asset::PackedVertex> vertices, const AABB& bounds,
                                      Span<const asset::CookedMeshSection> sections)
{
    UniquePtr<MeshEntry>* found = m_meshes.Find(id);
    MeshEntry* e = found ? found->Get() : nullptr;
    if (!e || !e->resident || !e->mesh.vertexBuffer || u64(firstVertex) + vertices.Size() > e->mesh.vertexCount ||
        sections.Size() != e->mesh.sections.Size())
        return false;
    GpuMesh& g = e->mesh;
    VertexUpdate& u = m_vertexUpdates.AddDefaulted();
    u.buffer = g.vertexBuffer;
    u.offset = u64(firstVertex) * sizeof(asset::PackedVertex);
    u.data.Append(vertices);
    for (usize i = 0; i < vertices.Size() && firstVertex + i < g.cpuPositions.Size(); ++i)
        g.cpuPositions[firstVertex + i] = vertices[i].position;
    for (usize i = 0; i < sections.Size(); ++i)
        g.sections[i].bounds = sections[i].bounds;
    g.bounds = bounds;
    g.sphere = Sphere::FromAABB(bounds);
    return true;
}

void RenderAssets::Update(CommandList& cmd)
{
    for (const VertexUpdate& u : m_vertexUpdates)
        cmd.UploadBuffer(u.buffer, u.offset, u.data.Data(), u.data.SizeBytes());
    m_vertexUpdates.Clear();
    u64 budget = m_uploadBudget;
    const usize pendingBefore = m_pendingTextures.Size() + m_pendingMeshes.Size() + m_pendingMaterials.Size();
    // Textures first (materials resolve against them), then meshes.
    for (usize i = 0; i < m_pendingTextures.Size();) {
        TextureEntry& e = *m_textures.Find(m_pendingTextures[i]);
        if (TryUploadTexture(m_pendingTextures[i], e, budget))
            m_pendingTextures.RemoveAtSwap(i);
        else
            ++i;
    }
    UpdateNanoStreaming(cmd); // last frame's requests (meshes that arrive below start next frame)
    for (usize i = 0; i < m_pendingMeshes.Size();) {
        MeshEntry& e = **m_meshes.Find(m_pendingMeshes[i]);
        if (TryUploadMesh(m_pendingMeshes[i], e, budget)) {
            m_assets.ReleaseCpuData(m_pendingMeshes[i]);
            m_pendingMeshes.RemoveAtSwap(i);
        } else {
            ++i;
        }
    }
    // Materials: write rows when the asset arrives, rewrite while their textures stream in.
    for (usize i = 0; i < m_pendingMaterials.Size();) {
        const AssetId id = m_pendingMaterials[i];
        MaterialEntry& e = *m_materials.Find(id);
        const AssetState state = m_assets.GetState(id);
        if (state == AssetState::Failed) {
            m_pendingMaterials.RemoveAtSwap(i);
            continue;
        }
        const asset::MaterialAsset* m = state == AssetState::Loaded ? m_assets.GetMaterial(id) : nullptr;
        if (!m) {
            ++i;
            continue;
        }
        bool pending = false;
        WriteMaterialRow(e.index, *m, pending);
        e.resolved = true;
        e.texturesPending = pending;
        if (!pending)
            m_pendingMaterials.RemoveAtSwap(i);
        else
            ++i;
    }
    // Material graphs: rewrite blocks whose textures are still streaming in.
    for (auto& kv : m_graphs)
        if (kv.second.texturesPending)
            WriteGraphParams(kv.second);
    const u32 readyShaders = m_shaders.ReadyCount();
    if (m_pendingTextures.Size() + m_pendingMeshes.Size() + m_pendingMaterials.Size() != pendingBefore || readyShaders != m_readyShaders ||
        m_paramDirtyEnd > m_paramDirtyBegin || m_dirtyEnd > m_dirtyBegin)
        ++m_contentVersion;
    m_readyShaders = readyShaders;
    if (m_paramDirtyEnd > m_paramDirtyBegin) {
        cmd.UploadBuffer(m_paramBuffer, m_paramDirtyBegin, m_paramBytes.Data() + m_paramDirtyBegin, m_paramDirtyEnd - m_paramDirtyBegin);
        cmd.Barrier(m_paramBuffer, ResourceState::ShaderResource);
        m_paramDirtyBegin = m_paramDirtyEnd = 0;
    }
    if (m_dirtyEnd > m_dirtyBegin) {
        cmd.UploadBuffer(m_materialBuffer, u64(m_dirtyBegin) * MaterialStride,
                         m_materialRows.Data() + usize(m_dirtyBegin) * MaterialStride,
                         u64(m_dirtyEnd - m_dirtyBegin) * MaterialStride);
        cmd.Barrier(m_materialBuffer, ResourceState::ShaderResource);
        m_dirtyBegin = m_maxMaterials;
        m_dirtyEnd = 0;
    }
}

RenderAssets::Stats RenderAssets::GetStats() const
{
    Stats s;
    for (const auto& kv : m_meshes)
        if (kv.second->resident) {
            ++s.meshes;
            s.meshBytes += kv.second->mesh.bytes;
        }
    for (const auto& kv : m_textures)
        if (kv.second.texture) {
            ++s.textures;
            s.textureBytes += kv.second.bytes;
        }
    s.materials = m_materialCount;
    s.pendingUploads = u32(m_pendingMeshes.Size() + m_pendingTextures.Size() + m_pendingMaterials.Size());
    s.nanoPoolPages = m_nanoSlotCount;
    for (const GpuMesh* g : m_nanoMeshes) {
        s.nanoResidentPages += g->nanoResidentPages;
    }
    s.nanoWantedPages = m_nanoWantedTotal;
    s.nanoStreamedPages = m_nanoStreamed;
    s.nanoFileBytes = m_nanoFileSize;
    return s;
}

} // namespace zx::render
