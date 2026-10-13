#include "Render/RenderAssets.h"
#include "Core/Log/Log.h"

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
        m_device.Destroy(kv.second.mesh.vertexBuffer);
        m_device.Destroy(kv.second.mesh.indexBuffer);
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
    MeshEntry* e = m_meshes.Find(id);
    if (!e) {
        e = &m_meshes.InsertOrAssign(id, MeshEntry{});
        e->requested = true;
        m_assets.Load(id);
        m_pendingMeshes.PushBack(id);
    }
    return e->resident ? &e->mesh : nullptr;
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
    const u64 bytes = m->vertices.SizeBytes() + m->indexData.Size();
    if (bytes > budget && budget != m_uploadBudget)
        return false; // wait for the next frame (always allow one item per frame)
    budget -= Min(budget, bytes);
    GpuMesh& g = e.mesh;
    // Initial data goes through the device's staging copy (load-time path; an async copy queue comes later).
    g.vertexBuffer = m_device.CreateBuffer(
        BufferDesc{m->vertices.SizeBytes(), BufferUsage::ShaderResource, MemoryType::GpuOnly, 0, m->name.View()},
        m->vertices.Data());
    g.indexBuffer = m_device.CreateBuffer(
        BufferDesc{m->indexData.Size(), BufferUsage::Index | BufferUsage::ShaderResource, MemoryType::GpuOnly, 0, m->name.View()},
        m->indexData.Data());
    g.index16 = m->index16;
    g.sections = m->sections;
    g.sectionLods = m->sectionLods;
    g.lodCount = m->lodCount;
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
    e.resident = g.vertexBuffer && g.indexBuffer;
    e.failed = !e.resident;
    return true;
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

void RenderAssets::Update(CommandList& cmd)
{
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
    for (usize i = 0; i < m_pendingMeshes.Size();) {
        MeshEntry& e = *m_meshes.Find(m_pendingMeshes[i]);
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
        if (kv.second.resident) {
            ++s.meshes;
            s.meshBytes += kv.second.mesh.bytes;
        }
    for (const auto& kv : m_textures)
        if (kv.second.texture) {
            ++s.textures;
            s.textureBytes += kv.second.bytes;
        }
    s.materials = m_materialCount;
    s.pendingUploads = u32(m_pendingMeshes.Size() + m_pendingTextures.Size() + m_pendingMaterials.Size());
    return s;
}

} // namespace zx::render
