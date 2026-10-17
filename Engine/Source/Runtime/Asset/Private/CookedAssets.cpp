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
    case AssetType::Skeleton: return "Skeleton";
    case AssetType::Animation: return "Animation";
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

u32 PackMorphNormal(Vec3 d)
{
    auto q = [](f32 x) { return u32(i32(Round(Clamp(x * 0.5f, -1.0f, 1.0f) * 511.0f)) & 0x3FF); };
    return q(d.x) | (q(d.y) << 10) | (q(d.z) << 20);
}

Vec3 UnpackMorphNormal(u32 p)
{
    auto u = [](u32 v) { return f32(i32(v << 22) >> 22) / 511.0f * 2.0f; };
    return Vec3(u(p), u(p >> 10), u(p >> 20));
}

CookedMesh CookMesh(MeshData& mesh, bool generateLods)
{
    if (generateLods) {
        mesh::SplitSections(mesh);
        mesh::GenerateLods(mesh);
    }
    mesh::OptimizeMesh(mesh);
    mesh.ComputeBounds();
    for (const MorphTarget& m : mesh.morphs) // fully applied targets stay inside the bounds (culling)
        for (usize v = 0; v < m.positions.Size() && v < mesh.vertices.Size(); ++v)
            mesh.bounds.Expand(mesh.vertices[v].position + m.positions[v]);

    CookedMesh out;
    out.name = mesh.name;
    out.bounds = mesh.bounds;
    out.vertices.ResizeUninitialized(mesh.vertices.Size());
    for (usize i = 0; i < mesh.vertices.Size(); ++i)
        out.vertices[i] = PackVertex(mesh.vertices[i]);
    if ((mesh.attributes & MeshAttributes::Skin) != MeshAttributes::None) {
        out.skin.ResizeUninitialized(mesh.vertices.Size() * 2);
        for (usize i = 0; i < mesh.vertices.Size(); ++i) {
            const MeshVertex& v = mesh.vertices[i];
            out.skin[i * 2] = v.joints;
            out.skin[i * 2 + 1] = v.weights;
            for (u32 k = 0; k < 4; ++k)
                if ((v.weights >> (k * 8)) & 0xFFu)
                    out.boneCount = Max(out.boneCount, ((v.joints >> (k * 8)) & 0xFFu) + 1);
        }
    }
    out.index16 = mesh.vertices.Size() <= 0xFFFF;
    // Morph targets (dense, target-major).
    if (!mesh.morphs.Empty()) {
        const usize n = mesh.vertices.Size();
        out.morphDeltas.ResizeUninitialized(mesh.morphs.Size() * n * 4);
        for (usize t = 0; t < mesh.morphs.Size(); ++t) {
            const MorphTarget& m = mesh.morphs[t];
            out.morphNames.PushBack(m.name.Empty() ? String::Format("Morph{}", t) : m.name);
            out.morphDefaults.PushBack(m.defaultWeight);
            for (usize v = 0; v < n; ++v) {
                const Vec3 dp = v < m.positions.Size() ? m.positions[v] : Vec3();
                const Vec3 dn = v < m.normals.Size() ? m.normals[v] : Vec3();
                u32* d = &out.morphDeltas[(t * n + v) * 4];
                MemCopy(d, &dp, 12);
                d[3] = PackMorphNormal(dn);
            }
        }
    }

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
        for (u32 i = 0; i < s0.indexCount; ++i) {
            const u32 v = lod0.indices[s0.indexOffset + i];
            cs.bounds.Expand(mesh.vertices[v].position);
            for (const MorphTarget& m : mesh.morphs)
                if (v < m.positions.Size())
                    cs.bounds.Expand(mesh.vertices[v].position + m.positions[v]);
        }
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
    case TextureFormat::BC5:
    case TextureFormat::BC6H:
    case TextureFormat::BC7:
    case TextureFormat::BC7_SRGB: return 16;
    case TextureFormat::RGBA16F: return 8;
    case TextureFormat::RGBA32F: return 16;
    default: return 4;
    }
}

bool IsBlockCompressed(TextureFormat f)
{
    return (f >= TextureFormat::BC1 && f <= TextureFormat::BC5) || (f >= TextureFormat::BC6H && f <= TextureFormat::BC7_SRGB);
}

namespace {
constexpr StringView CompressionNames[] = {"None", "Auto", "BC1", "BC3", "BC4", "BC5", "BC7"};
} // namespace

StringView TextureCompressionName(TextureCompression c)
{
    return u32(c) < sizeof(CompressionNames) / sizeof(CompressionNames[0]) ? CompressionNames[u32(c)] : StringView("Auto");
}

TextureCompression ParseTextureCompression(StringView name)
{
    for (u32 i = 0; i < sizeof(CompressionNames) / sizeof(CompressionNames[0]); ++i)
        if (name.EqualsIgnoreCase(CompressionNames[i]))
            return TextureCompression(i);
    return TextureCompression::Auto;
}

Vec3 LatLongDirection(f32 u, f32 v)
{
    const f32 theta = v * Pi, phi = (u - 0.5f) * 2.0f * Pi;
    return Vec3(Sin(theta) * Sin(phi), Cos(theta), Sin(theta) * Cos(phi));
}

Vec2 LatLongUV(Vec3 d)
{
    const f32 phi = std::atan2(d.x, d.z);
    const f32 theta = std::acos(Clamp(d.y, -1.0f, 1.0f));
    return Vec2(phi / (2.0f * Pi) + 0.5f, theta / Pi);
}

HdrInfo AnalyzeHdrEnvironment(const Image& source)
{
    HdrInfo info;
    if (source.format != ImageFormat::RGBA32F || !source.IsValid())
        return info;
    // Work on an energy-preserving box-downsampled copy (<= 512 wide): enough to find the sun, fast to cook.
    u32 factor = 1;
    while (source.width / factor > 512 && source.height / (factor * 2) >= 4)
        factor *= 2;
    const u32 w = Max(1u, source.width / factor), h = Max(1u, source.height / factor);
    Array<Vec3> px;
    px.ResizeZeroed(usize(w) * h);
    for (u32 y = 0; y < h * factor && y < source.height; ++y) {
        const f32* row = reinterpret_cast<const f32*>(source.Row(y));
        for (u32 x = 0; x < w * factor && x < source.width; ++x) {
            Vec3 c(row[x * 4], row[x * 4 + 1], row[x * 4 + 2]);
            if (!(c.x == c.x) || !(c.y == c.y) || !(c.z == c.z))
                c = Vec3(0, 0, 0);
            px[usize(y / factor) * w + x / factor] += Max(c, Vec3(0, 0, 0)) / f32(factor * factor);
        }
    }
    auto lum = [](Vec3 c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; };
    const f32 dPhi = 2.0f * Pi / f32(w), dTheta = Pi / f32(h);
    Vec3 sum;
    f32 peak = 0.0f;
    usize peakIndex = 0;
    Array<f32> upper;
    for (u32 y = 0; y < h; ++y) {
        const f32 v = (f32(y) + 0.5f) / f32(h);
        const f32 dOmega = Sin(v * Pi) * dPhi * dTheta;
        for (u32 x = 0; x < w; ++x) {
            const Vec3 c = px[usize(y) * w + x];
            sum += c * dOmega;
            if (v < 0.52f) { // sky hemisphere (a little below the horizon for sunsets)
                const f32 l = lum(c);
                upper.PushBack(l);
                if (l > peak) {
                    peak = l;
                    peakIndex = usize(y) * w + x;
                }
            }
        }
    }
    info.valid = true;
    info.average = sum / (4.0f * Pi);
    if (upper.Empty() || peak <= 0.0f)
        return info;
    // Median sky luminance (nth element by partial selection on a copy).
    Array<f32> sorted = upper;
    const usize mid = sorted.Size() / 2;
    for (usize i = 0; i <= mid; ++i)
        for (usize j = i + 1; j < sorted.Size(); ++j)
            if (sorted[j] < sorted[i])
                swap(sorted[i], sorted[j]);
    const f32 median = sorted[mid];
    // A sun is a small region far brighter than the sky around it.
    if (peak < 50.0f * Max(median, 1e-6f))
        return info;
    const f32 threshold = Max(peak * 0.02f, median * 20.0f);
    const Vec3 peakDir = LatLongDirection((f32(peakIndex % w) + 0.5f) / f32(w), (f32(peakIndex / w) + 0.5f) / f32(h));
    const f32 searchCos = Cos(DegToRad(10.0f));
    Vec3 irradiance, weightedDir;
    f32 minCos = 1.0f;
    for (u32 y = 0; y < h; ++y) {
        const f32 v = (f32(y) + 0.5f) / f32(h);
        const f32 dOmega = Sin(v * Pi) * dPhi * dTheta;
        for (u32 x = 0; x < w; ++x) {
            const Vec3 c = px[usize(y) * w + x];
            const f32 l = lum(c);
            if (l <= threshold)
                continue;
            const Vec3 d = LatLongDirection((f32(x) + 0.5f) / f32(w), v);
            const f32 cosAngle = Dot(d, peakDir);
            if (cosAngle < searchCos)
                continue;
            const Vec3 excess = c * ((l - threshold) / l) * dOmega;
            irradiance += excess;
            weightedDir += d * lum(excess);
            minCos = Min(minCos, cosAngle);
        }
    }
    if (lum(irradiance) <= 0.0f)
        return info;
    info.hasSun = true;
    info.sunDirection = SafeNormalize(weightedDir, peakDir);
    info.sunIlluminance = irradiance;
    // Cone: the farthest bright texel plus a texel of margin.
    info.sunCosRadius = Cos(Min(std::acos(Clamp(minCos, -1.0f, 1.0f)) + dTheta * 1.5f, DegToRad(15.0f)));
    info.sunClamp = threshold;
    return info;
}

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
        // HDR: box-filtered mips (Kaiser lobes ring around the sun), BC6H when the size allows, else half floats.
        out.hdr = AnalyzeHdrEnvironment(source);
        Array<Image> levels;
        if (options.generateMips || (options.maxSize && Max(source.width, source.height) > options.maxSize)) {
            texture::MipOptions mo;
            mo.filter = texture::MipFilter::Box;
            levels = texture::GenerateMips(source, mo);
        } else {
            levels.PushBack(source);
        }
        u32 first = 0;
        while (options.maxSize && first + 1 < levels.Size() && Max(levels[first].width, levels[first].height) > options.maxSize)
            ++first;
        if (!options.generateMips)
            levels.Resize(first + 1);
        const bool bc6 = options.compression != TextureCompression::None && levels[first].width % 4 == 0 && levels[first].height % 4 == 0;
        out.format = bc6 ? TextureFormat::BC6H : TextureFormat::RGBA16F;
        out.width = levels[first].width;
        out.height = levels[first].height;
        for (usize l = first; l < levels.Size(); ++l) {
            if (bc6) {
                out.mips.PushBack(texture::CompressBlocks(levels[l], texture::BlockFormat::BC6H));
                continue;
            }
            Array<u8>& mip = out.mips.AddDefaulted();
            const usize count = usize(levels[l].width) * levels[l].height * 4;
            mip.ResizeUninitialized(count * 2);
            const f32* src = reinterpret_cast<const f32*>(levels[l].pixels.Data());
            u16* dst = reinterpret_cast<u16*>(mip.Data());
            for (usize i = 0; i < count; ++i)
                dst[i] = FloatToHalf(src[i]);
        }
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
                              : (levels[first].HasTransparency() ? TextureCompression::BC7 : TextureCompression::BC1);
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
    case TextureCompression::BC7:
        out.format = options.srgb ? TextureFormat::BC7_SRGB : TextureFormat::BC7;
        block = texture::BlockFormat::BC7;
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
    w.WriteArray(m.skin.View());
    w.Write<u32>(m.boneCount);
    w.WriteVarU64(m.morphNames.Size());
    for (const String& name : m.morphNames)
        w.WriteString(name);
    w.WriteArray(m.morphDefaults.View());
    w.WriteArray(m.morphDeltas.View());
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
    r.ReadArray(m.skin);
    m.boneCount = r.Read<u32>();
    const u64 morphs = r.ReadVarU64();
    if (morphs > 1024)
        return false;
    for (u64 i = 0; i < morphs && !r.HasError(); ++i)
        m.morphNames.PushBack(r.ReadString());
    r.ReadArray(m.morphDefaults);
    r.ReadArray(m.morphDeltas);
    if (r.HasError() || (!m.skin.Empty() && m.skin.Size() != m.vertices.Size() * 2))
        return false;
    if (m.morphDefaults.Size() != morphs || m.morphDeltas.Size() != morphs * m.vertices.Size() * 4)
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
    w.WriteBool(t.hdr.valid);
    if (t.hdr.valid) {
        WritePod(w, t.hdr.average);
        w.WriteBool(t.hdr.hasSun);
        WritePod(w, t.hdr.sunDirection);
        WritePod(w, t.hdr.sunIlluminance);
        w.Write<f32>(t.hdr.sunCosRadius);
        w.Write<f32>(t.hdr.sunClamp);
    }
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
    if (r.HasError() || mipCount > 16 || t.format > TextureFormat::BC7_SRGB)
        return false;
    t.mips.Clear();
    for (u32 m = 0; m < mipCount; ++m) {
        Array<u8>& mip = t.mips.AddDefaulted();
        r.ReadArray(mip);
        if (mip.Size() != TextureMipSize(t.format, Max(1u, t.width >> m), Max(1u, t.height >> m)))
            return false;
    }
    t.hdr = HdrInfo();
    t.hdr.valid = r.ReadBool();
    if (t.hdr.valid) {
        ReadPod(r, t.hdr.average);
        t.hdr.hasSun = r.ReadBool();
        ReadPod(r, t.hdr.sunDirection);
        ReadPod(r, t.hdr.sunIlluminance);
        t.hdr.sunCosRadius = r.Read<f32>();
        t.hdr.sunClamp = r.Read<f32>();
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
        WriteGuid(w, n.skeleton);
        WritePod(w, n.skinToNode);
        w.WriteVarU64(n.animations.Size());
        for (const AssetId& a : n.animations)
            WriteGuid(w, a);
        w.WriteVarU64(n.morphNames.Size());
        for (const String& name : n.morphNames)
            w.WriteString(name);
        w.WriteArray(n.morphWeights.View());
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
        n.skeleton = ReadGuid(r);
        ReadPod(r, n.skinToNode);
        const u64 animations = r.ReadVarU64();
        if (animations > 4096)
            return false;
        for (u64 a = 0; a < animations; ++a)
            n.animations.PushBack(ReadGuid(r));
        const u64 morphs = r.ReadVarU64();
        if (morphs > 1024)
            return false;
        for (u64 m = 0; m < morphs; ++m)
            n.morphNames.PushBack(r.ReadString());
        r.ReadArray(n.morphWeights);
        if (n.parent >= i32(i))
            return false;
    }
    return !r.HasError();
}

void Serialize(const Skeleton& s, Array<u8>& out)
{
    BinaryWriter w(out);
    WriteHeader(w, AssetType::Skeleton);
    w.WriteString(s.name);
    w.WriteVarU64(s.boneNames.Size());
    for (usize b = 0; b < s.boneNames.Size(); ++b) {
        w.WriteString(s.boneNames[b]);
        w.Write<i32>(s.parents[b]);
        WritePod(w, s.bindPose[b].position);
        WritePod(w, s.bindPose[b].rotation);
        WritePod(w, s.bindPose[b].scale);
        WritePod(w, s.inverseBind[b]);
    }
}

bool Deserialize(ByteSpan data, Skeleton& s)
{
    BinaryReader r(data);
    if (!ReadHeader(r, AssetType::Skeleton))
        return false;
    s.name = r.ReadString();
    const u64 bones = r.ReadVarU64();
    if (bones > 4096)
        return false;
    for (u64 b = 0; b < bones && !r.HasError(); ++b) {
        s.boneNames.PushBack(r.ReadString());
        const i32 parent = r.Read<i32>();
        if (parent >= i32(b))
            return false; // parents precede children
        s.parents.PushBack(parent);
        Transform t;
        ReadPod(r, t.position);
        ReadPod(r, t.rotation);
        ReadPod(r, t.scale);
        s.bindPose.PushBack(t);
        Mat4 inv;
        ReadPod(r, inv);
        s.inverseBind.PushBack(inv);
    }
    return !r.HasError();
}

void Serialize(const AnimationClip& c, Array<u8>& out)
{
    BinaryWriter w(out);
    WriteHeader(w, AssetType::Animation);
    w.WriteString(c.name);
    w.Write<f32>(c.duration);
    w.Write<u32>(c.boneCount);
    w.WriteVarU64(c.tracks.Size());
    for (const AnimationTrack& t : c.tracks) {
        w.Write<u32>(t.bone);
        w.Write<u8>(t.stepMask);
        w.WriteArray(t.positionTimes.View());
        w.WriteArray(t.positions.View());
        w.WriteArray(t.rotationTimes.View());
        w.WriteArray(t.rotations.View());
        w.WriteArray(t.scaleTimes.View());
        w.WriteArray(t.scales.View());
    }
    w.WriteVarU64(c.morphTracks.Size());
    for (const MorphTrack& t : c.morphTracks) {
        w.Write<u32>(t.targets);
        w.WriteBool(t.step);
        w.WriteArray(t.times.View());
        w.WriteArray(t.weights.View());
    }
}

bool Deserialize(ByteSpan data, AnimationClip& c)
{
    BinaryReader r(data);
    if (!ReadHeader(r, AssetType::Animation))
        return false;
    c.name = r.ReadString();
    c.duration = r.Read<f32>();
    c.boneCount = r.Read<u32>();
    const u64 tracks = r.ReadVarU64();
    if (tracks > 4096)
        return false;
    for (u64 i = 0; i < tracks && !r.HasError(); ++i) {
        AnimationTrack& t = c.tracks.AddDefaulted();
        t.bone = r.Read<u32>();
        t.stepMask = r.Read<u8>();
        r.ReadArray(t.positionTimes);
        r.ReadArray(t.positions);
        r.ReadArray(t.rotationTimes);
        r.ReadArray(t.rotations);
        r.ReadArray(t.scaleTimes);
        r.ReadArray(t.scales);
        if (t.positionTimes.Size() != t.positions.Size() || t.rotationTimes.Size() != t.rotations.Size() ||
            t.scaleTimes.Size() != t.scales.Size())
            return false;
    }
    const u64 morphTracks = r.ReadVarU64();
    if (morphTracks > 256)
        return false;
    for (u64 i = 0; i < morphTracks && !r.HasError(); ++i) {
        MorphTrack& t = c.morphTracks.AddDefaulted();
        t.targets = r.Read<u32>();
        t.step = r.ReadBool();
        r.ReadArray(t.times);
        r.ReadArray(t.weights);
        if (t.weights.Size() != t.times.Size() * t.targets)
            return false;
    }
    return !r.HasError();
}

} // namespace zx::asset
