#include "Asset/CookedAssets.h"
#include "Asset/MeshOptimizer.h"
#include "Core/Math/Packing.h"
#include "Core/Serialization/Binary.h"

namespace zx::asset {

const char* AssetTypeName(AssetType type)
{
    switch (type) {
    case AssetType::Texture: return "Texture";
    case AssetType::Mesh: return "Mesh";
    case AssetType::Material: return "Material";
    case AssetType::Prefab: return "Prefab";
    default: return "Unknown";
    }
}

// --- Mesh -------------------------------------------------------------------------------------------------------------

PackedVertex PackVertex(const MeshVertex& v)
{
    PackedVertex p;
    p.position = v.position;
    p.normal = PackNormalOct16(v.normal);
    p.tangent = (PackNormalOct16(v.tangent.XYZ()) & ~1u) | (v.tangent.w < 0.0f ? 1u : 0u);
    p.uv0 = v.uv0;
    p.uv1 = PackHalf2(v.uv1);
    p.color = v.color;
    return p;
}

MeshVertex UnpackVertex(const PackedVertex& p)
{
    MeshVertex v;
    v.position = p.position;
    v.normal = UnpackNormalOct16(p.normal);
    v.tangent = Vec4(UnpackNormalOct16(p.tangent & ~1u), (p.tangent & 1u) ? -1.0f : 1.0f);
    v.uv0 = p.uv0;
    v.uv1 = UnpackHalf2(p.uv1);
    v.color = p.color;
    return v;
}

u32 CookedMesh::Index(u32 i) const
{
    if (index16) {
        u16 v;
        MemCopy(&v, indexData.Data() + usize(i) * 2, 2);
        return v;
    }
    u32 v;
    MemCopy(&v, indexData.Data() + usize(i) * 4, 4);
    return v;
}

u32 CookedMesh::TriangleCount(u32 lod) const
{
    u32 n = 0;
    for (const CookedMeshSection& s : sections)
        n += SectionLod(s, lod).indexCount / 3;
    return n;
}

CookedMesh CookMesh(MeshData& mesh, bool generateLods)
{
    if (generateLods) {
        mesh::SplitSections(mesh);
        mesh::GenerateLods(mesh);
    }
    mesh::OptimizeMesh(mesh);
    mesh.ComputeBounds();

    CookedMesh out;
    out.name = mesh.name;
    out.bounds = mesh.bounds;
    out.vertices.ResizeUninitialized(mesh.vertices.Size());
    for (usize i = 0; i < mesh.vertices.Size(); ++i)
        out.vertices[i] = PackVertex(mesh.vertices[i]);
    out.index16 = mesh.vertices.Size() <= 0xFFFF;

    // Material slots: distinct source material indices in order of first appearance in LOD0.
    auto slotOf = [&](u32 material) {
        for (u32 s = 0; s < out.slotSourceMaterial.Size(); ++s)
            if (out.slotSourceMaterial[s] == material)
                return s;
        out.slotSourceMaterial.PushBack(material);
        return u32(out.slotSourceMaterial.Size() - 1);
    };
    // Index buffer = all LODs concatenated; each LOD0 section gets its chain of non-empty LOD ranges.
    Array<u32> lodBase;
    u32 indexCount = 0;
    for (const MeshLod& lod : mesh.lods) {
        lodBase.PushBack(indexCount);
        indexCount += u32(lod.indices.Size());
    }
    const MeshLod& lod0 = mesh.lods[0];
    for (usize si = 0; si < lod0.sections.Size(); ++si) {
        const MeshSection& s0 = lod0.sections[si];
        CookedMeshSection cs;
        cs.materialSlot = slotOf(s0.materialIndex);
        cs.firstLod = u32(out.sectionLods.Size());
        for (u32 i = 0; i < s0.indexCount; ++i)
            cs.bounds.Expand(mesh.vertices[lod0.indices[s0.indexOffset + i]].position);
        cs.lodCount = 0;
        for (usize l = 0; l < mesh.lods.Size(); ++l) {
            if (si >= mesh.lods[l].sections.Size())
                break;
            const MeshSection& s = mesh.lods[l].sections[si];
            if (l > 0 && s.indexCount == 0)
                break;
            out.sectionLods.PushBack(CookedSectionLod{lodBase[l] + s.indexOffset, s.indexCount, l == 0 ? 1.0f : s.screenSize, s.error});
            ++cs.lodCount;
        }
        out.lodCount = Max(out.lodCount, cs.lodCount);
        out.sections.PushBack(cs);
    }
    out.materialSlotCount = u32(out.slotSourceMaterial.Size());
    out.indexData.ResizeUninitialized(usize(indexCount) * (out.index16 ? 2 : 4));
    usize at = 0;
    for (const MeshLod& lod : mesh.lods)
        for (u32 i : lod.indices) {
            if (out.index16) {
                const u16 v = u16(i);
                MemCopy(out.indexData.Data() + at, &v, 2);
                at += 2;
            } else {
                MemCopy(out.indexData.Data() + at, &i, 4);
                at += 4;
            }
        }
    return out;
}

// --- Texture ----------------------------------------------------------------------------------------------------------

u32 TextureFormatBlockBytes(TextureFormat f)
{
    switch (f) {
    case TextureFormat::BC1:
    case TextureFormat::BC1_SRGB:
    case TextureFormat::BC4: return 8;
    case TextureFormat::BC3:
    case TextureFormat::BC3_SRGB:
    case TextureFormat::BC5: return 16;
    case TextureFormat::RGBA16F: return 8;
    case TextureFormat::RGBA32F: return 16;
    default: return 4;
    }
}

bool IsBlockCompressed(TextureFormat f) { return f >= TextureFormat::BC1 && f <= TextureFormat::BC5; }

u64 TextureMipSize(TextureFormat f, u32 width, u32 height)
{
    if (IsBlockCompressed(f))
        return u64(Max(1u, (width + 3) / 4)) * Max(1u, (height + 3) / 4) * TextureFormatBlockBytes(f);
    return u64(width) * height * TextureFormatBlockBytes(f);
}

u64 CookedTexture::MemorySize() const
{
    u64 n = 0;
    for (const Array<u8>& m : mips)
        n += m.Size();
    return n;
}

Result<CookedTexture> CookTexture(const Image& source, const TextureCookOptions& options)
{
    if (!source.IsValid())
        return Error::Make(ErrorCode::InvalidArgument, "CookTexture: empty image");
    CookedTexture out;
    if (source.format == ImageFormat::RGBA32F) {
        // HDR: half floats, single mip until BC6H arrives (M10).
        out.format = TextureFormat::RGBA16F;
        out.width = source.width;
        out.height = source.height;
        Array<u8>& mip = out.mips.AddDefaulted();
        mip.ResizeUninitialized(usize(source.width) * source.height * 8);
        const f32* src = reinterpret_cast<const f32*>(source.pixels.Data());
        u16* dst = reinterpret_cast<u16*>(mip.Data());
        for (usize i = 0; i < usize(source.width) * source.height * 4; ++i)
            dst[i] = FloatToHalf(src[i]);
        return out;
    }
    Image rgba8 = source;
    rgba8.ConvertTo(ImageFormat::RGBA8);

    Array<Image> levels;
    const bool needsMips = options.generateMips || (options.maxSize && Max(source.width, source.height) > options.maxSize);
    if (needsMips) {
        texture::MipOptions mo;
        mo.filter = options.mipFilter;
        mo.srgb = options.srgb;
        mo.normalMap = options.normalMap;
        mo.preserveAlphaCoverage = options.preserveAlphaCoverage;
        mo.alphaCutoff = options.alphaCutoff;
        levels = texture::GenerateMips(rgba8, mo);
    } else {
        levels.PushBack(move(rgba8));
    }
    // Size cap: drop top mips.
    u32 first = 0;
    while (options.maxSize && first + 1 < levels.Size() && Max(levels[first].width, levels[first].height) > options.maxSize)
        ++first;
    if (!options.generateMips)
        levels.Resize(first + 1);

    TextureCompression c = options.compression;
    if (c == TextureCompression::Auto)
        c = options.normalMap ? TextureCompression::BC5
                              : (levels[first].HasTransparency() ? TextureCompression::BC3 : TextureCompression::BC1);
    // D3D12 requires block-aligned top-level dimensions for BCn.
    if (c != TextureCompression::None && (levels[first].width % 4 != 0 || levels[first].height % 4 != 0))
        c = TextureCompression::None;
    texture::BlockFormat block = texture::BlockFormat::BC1;
    switch (c) {
    case TextureCompression::BC1:
        out.format = options.srgb ? TextureFormat::BC1_SRGB : TextureFormat::BC1;
        block = texture::BlockFormat::BC1;
        break;
    case TextureCompression::BC3:
        out.format = options.srgb ? TextureFormat::BC3_SRGB : TextureFormat::BC3;
        block = texture::BlockFormat::BC3;
        break;
    case TextureCompression::BC4:
        out.format = TextureFormat::BC4;
        block = texture::BlockFormat::BC4;
        break;
    case TextureCompression::BC5:
        out.format = TextureFormat::BC5;
        block = texture::BlockFormat::BC5;
        break;
    default: out.format = options.srgb ? TextureFormat::RGBA8_SRGB : TextureFormat::RGBA8; break;
    }
    out.width = levels[first].width;
    out.height = levels[first].height;
    for (usize l = first; l < levels.Size(); ++l) {
        if (c == TextureCompression::None)
            out.mips.PushBack(move(levels[l].pixels));
        else
            out.mips.PushBack(texture::CompressBlocks(levels[l], block));
    }
    return out;
}

// --- Serialization ----------------------------------------------------------------------------------------------------

namespace {

constexpr u32 ContainerMagic = 0x53415A58; // "XZAS"

void WriteHeader(BinaryWriter& w, AssetType type)
{
    w.Write<u32>(ContainerMagic);
    w.Write<u8>(u8(type));
    w.Write<u16>(u16(CookerVersion));
}

bool ReadHeader(BinaryReader& r, AssetType expected)
{
    return r.Read<u32>() == ContainerMagic && r.Read<u8>() == u8(expected) && r.Read<u16>() == CookerVersion && !r.HasError();
}

void WriteGuid(BinaryWriter& w, const Guid& g)
{
    w.Write<u64>(g.high);
    w.Write<u64>(g.low);
}

Guid ReadGuid(BinaryReader& r)
{
    Guid g;
    g.high = r.Read<u64>();
    g.low = r.Read<u64>();
    return g;
}

template <typename T>
void WritePod(BinaryWriter& w, const T& v)
{
    w.WriteBytes(&v, sizeof(T));
}

template <typename T>
void ReadPod(BinaryReader& r, T& v)
{
    r.ReadBytes(&v, sizeof(T));
}

} // namespace

AssetType PeekAssetType(ByteSpan data)
{
    BinaryReader r(data);
    if (r.Read<u32>() != ContainerMagic)
        return AssetType::Unknown;
    const u8 type = r.Read<u8>();
    if (r.Read<u16>() != CookerVersion || r.HasError() || type >= u8(AssetType::Count))
        return AssetType::Unknown;
    return AssetType(type);
}

void Serialize(const CookedMesh& m, Array<u8>& out)
{
    BinaryWriter w(out);
    WriteHeader(w, AssetType::Mesh);
    w.WriteString(m.name);
    WritePod(w, m.bounds);
    w.WriteBool(m.index16);
    w.Write<u32>(m.materialSlotCount);
    w.WriteArray(m.slotSourceMaterial.View());
    w.Write<u32>(m.lodCount);
    w.WriteArray(m.sections.View());
    w.WriteArray(m.sectionLods.View());
    w.WriteArray(m.vertices.View());
    w.WriteArray(m.indexData.View());
}

bool Deserialize(ByteSpan data, CookedMesh& m)
{
    BinaryReader r(data);
    if (!ReadHeader(r, AssetType::Mesh))
        return false;
    m.name = r.ReadString();
    ReadPod(r, m.bounds);
    m.index16 = r.ReadBool();
    m.materialSlotCount = r.Read<u32>();
    r.ReadArray(m.slotSourceMaterial);
    m.lodCount = r.Read<u32>();
    r.ReadArray(m.sections);
    r.ReadArray(m.sectionLods);
    r.ReadArray(m.vertices);
    r.ReadArray(m.indexData);
    if (r.HasError())
        return false;
    for (const CookedMeshSection& s : m.sections)
        if (s.lodCount == 0 || u64(s.firstLod) + s.lodCount > m.sectionLods.Size())
            return false;
    for (const CookedSectionLod& l : m.sectionLods)
        if (u64(l.indexOffset) + l.indexCount > m.IndexCount())
            return false;
    return true;
}

void Serialize(const CookedTexture& t, Array<u8>& out)
{
    BinaryWriter w(out);
    WriteHeader(w, AssetType::Texture);
    w.WriteString(t.name);
    w.Write<u8>(u8(t.format));
    w.Write<u32>(t.width);
    w.Write<u32>(t.height);
    w.Write<u8>(u8(t.wrapU));
    w.Write<u8>(u8(t.wrapV));
    w.Write<u32>(u32(t.mips.Size()));
    for (const Array<u8>& mip : t.mips)
        w.WriteArray(mip.View());
}

bool Deserialize(ByteSpan data, CookedTexture& t)
{
    BinaryReader r(data);
    if (!ReadHeader(r, AssetType::Texture))
        return false;
    t.name = r.ReadString();
    t.format = TextureFormat(r.Read<u8>());
    t.width = r.Read<u32>();
    t.height = r.Read<u32>();
    t.wrapU = TextureWrap(r.Read<u8>());
    t.wrapV = TextureWrap(r.Read<u8>());
    const u32 mipCount = r.Read<u32>();
    if (r.HasError() || mipCount > 16 || t.format > TextureFormat::RGBA32F)
        return false;
    t.mips.Clear();
    for (u32 m = 0; m < mipCount; ++m) {
        Array<u8>& mip = t.mips.AddDefaulted();
        r.ReadArray(mip);
        if (mip.Size() != TextureMipSize(t.format, Max(1u, t.width >> m), Max(1u, t.height >> m)))
            return false;
    }
    return !r.HasError();
}

void Serialize(const MaterialAsset& m, Array<u8>& out)
{
    BinaryWriter w(out);
    WriteHeader(w, AssetType::Material);
    w.WriteString(m.name);
    w.Write<u8>(u8(m.shadingModel));
    w.Write<u8>(u8(m.alphaMode));
    w.Write<f32>(m.alphaCutoff);
    w.WriteBool(m.doubleSided);
    WritePod(w, m.baseColor);
    w.Write<f32>(m.metallic);
    w.Write<f32>(m.roughness);
    WritePod(w, m.emissive);
    w.Write<f32>(m.normalScale);
    w.Write<f32>(m.occlusionStrength);
    WritePod(w, m.uvScale);
    WritePod(w, m.uvOffset);
    w.Write<f32>(m.uvRotation);
    for (const AssetId* id : {&m.baseColorTexture, &m.metallicRoughnessTexture, &m.normalTexture, &m.occlusionTexture,
                              &m.emissiveTexture})
        WriteGuid(w, *id);
}

bool Deserialize(ByteSpan data, MaterialAsset& m)
{
    BinaryReader r(data);
    if (!ReadHeader(r, AssetType::Material))
        return false;
    m.name = r.ReadString();
    m.shadingModel = ShadingModel(r.Read<u8>());
    m.alphaMode = AlphaMode(r.Read<u8>());
    m.alphaCutoff = r.Read<f32>();
    m.doubleSided = r.ReadBool();
    ReadPod(r, m.baseColor);
    m.metallic = r.Read<f32>();
    m.roughness = r.Read<f32>();
    ReadPod(r, m.emissive);
    m.normalScale = r.Read<f32>();
    m.occlusionStrength = r.Read<f32>();
    ReadPod(r, m.uvScale);
    ReadPod(r, m.uvOffset);
    m.uvRotation = r.Read<f32>();
    for (AssetId* id : {&m.baseColorTexture, &m.metallicRoughnessTexture, &m.normalTexture, &m.occlusionTexture,
                        &m.emissiveTexture})
        *id = ReadGuid(r);
    return !r.HasError();
}

void Serialize(const PrefabAsset& p, Array<u8>& out)
{
    BinaryWriter w(out);
    WriteHeader(w, AssetType::Prefab);
    w.WriteString(p.name);
    w.WriteVarU64(p.nodes.Size());
    for (const PrefabNode& n : p.nodes) {
        w.WriteString(n.name);
        WritePod(w, n.local.position);
        WritePod(w, n.local.rotation);
        WritePod(w, n.local.scale);
        w.Write<i32>(n.parent);
        WriteGuid(w, n.mesh);
        w.WriteVarU64(n.materials.Size());
        for (const AssetId& m : n.materials)
            WriteGuid(w, m);
    }
}

bool Deserialize(ByteSpan data, PrefabAsset& p)
{
    BinaryReader r(data);
    if (!ReadHeader(r, AssetType::Prefab))
        return false;
    p.name = r.ReadString();
    const u64 count = r.ReadVarU64();
    if (count > (1u << 24))
        return false;
    p.nodes.Clear();
    for (u64 i = 0; i < count && !r.HasError(); ++i) {
        PrefabNode& n = p.nodes.AddDefaulted();
        n.name = r.ReadString();
        ReadPod(r, n.local.position);
        ReadPod(r, n.local.rotation);
        ReadPod(r, n.local.scale);
        n.parent = r.Read<i32>();
        n.mesh = ReadGuid(r);
        const u64 materials = r.ReadVarU64();
        if (materials > 4096)
            return false;
        for (u64 m = 0; m < materials; ++m)
            n.materials.PushBack(ReadGuid(r));
        if (n.parent >= i32(i))
            return false;
    }
    return !r.HasError();
}

} // namespace zx::asset
