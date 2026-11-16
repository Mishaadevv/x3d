#include "Engine/Impostor.h"
#include "Asset/AssetManager.h"
#include "Core/Containers/HashMap.h"
#include "Core/HAL/PlatformFile.h"
#include "Core/Image/Image.h"
#include "Core/Image/TextureTools.h"
#include "Core/Log/Log.h"
#include "Core/Math/Color.h"
#include "Core/Serialization/Binary.h"

ZX_DEFINE_LOG_CATEGORY_STATIC(LogImpostor, Info);

namespace zx {

namespace {

constexpr u32 ImpostorMagic = 0x504D4958; // "XIMP"
constexpr u32 ImpostorVersion = 1;
constexpr u32 Supersample = 2;

struct BakeTriangle
{
    Vec3 p[3];
    Vec3 n[3];
    Vec2 uv[3];
    Vec4 tint[3];          // material base color x vertex color (linear)
    const Image* texture = nullptr; // sRGB RGBA8
    Vec2 uvScale = Vec2(1.0f), uvOffset;
    bool mask = false;
    f32 cutoff = 0.5f;
};

Vec4 SampleTexture(const Image& img, Vec2 uv)
{
    // Bilinear, repeat (the views are supersampled; leaves need the smooth alpha edge).
    const f32 fx = (uv.x - std::floor(uv.x)) * f32(img.width) - 0.5f, fy = (uv.y - std::floor(uv.y)) * f32(img.height) - 0.5f;
    const i32 x0 = i32(std::floor(fx)), y0 = i32(std::floor(fy));
    const f32 tx = fx - f32(x0), ty = fy - f32(y0);
    auto texel = [&](i32 x, i32 y) {
        x = ((x % i32(img.width)) + i32(img.width)) % i32(img.width);
        y = ((y % i32(img.height)) + i32(img.height)) % i32(img.height);
        const u8* p = img.Row(u32(y)) + usize(x) * 4;
        return Vec4(SRGB8ToLinear(p[0]), SRGB8ToLinear(p[1]), SRGB8ToLinear(p[2]), f32(p[3]) / 255.0f);
    };
    return Lerp(Lerp(texel(x0, y0), texel(x0 + 1, y0), tx), Lerp(texel(x0, y0 + 1), texel(x0 + 1, y0 + 1), tx), ty);
}

} // namespace

Vec3 ImpostorFrameDirection(u32 x, u32 y, u32 frames)
{
    const f32 u = (f32(x) + 0.5f) / f32(frames) * 2.0f - 1.0f, v = (f32(y) + 0.5f) / f32(frames) * 2.0f - 1.0f;
    const f32 ox = (u + v) * 0.5f, oz = (u - v) * 0.5f;
    return Normalize(Vec3(ox, Max(1.0f - Abs(ox) - Abs(oz), 0.0f), oz));
}

void ImpostorFrameOf(Vec3 dir, u32 frames, u32& x, u32& y)
{
    dir.y = Max(dir.y, 0.0f);
    const f32 s = Abs(dir.x) + Abs(dir.y) + Abs(dir.z);
    const f32 ox = s > 1e-6f ? dir.x / s : 0.0f, oz = s > 1e-6f ? dir.z / s : 0.0f;
    const f32 u = ox + oz, v = ox - oz;
    x = u32(Clamp(i32(std::floor((u * 0.5f + 0.5f) * f32(frames))), 0, i32(frames) - 1));
    y = u32(Clamp(i32(std::floor((v * 0.5f + 0.5f) * f32(frames))), 0, i32(frames) - 1));
}

void ImpostorFrameAxes(Vec3 dir, Vec3& right, Vec3& up)
{
    const Vec3 r = Cross(Vec3(0.0f, 1.0f, 0.0f), dir);
    right = LengthSq(r) > 1e-6f ? Normalize(r) : Vec3(1.0f, 0.0f, 0.0f);
    up = Cross(dir, right);
}

bool BakeImpostor(asset::AssetManager& assets, Span<const FoliagePart> parts, ImpostorAsset& out, u32 frames, u32 frameSize)
{
    // The model's triangles in model space with what shades them.
    HashMap<asset::AssetId, UniquePtr<Image>> images;
    auto imageOf = [&](asset::AssetId id) -> const Image* {
        if (!id.IsValid())
            return nullptr;
        if (UniquePtr<Image>* found = images.Find(id))
            return found->Get();
        // The cooked texture's top mip (block-compressed ones decoded): works for any source, embedded images too.
        UniquePtr<Image> img;
        const asset::CookedTexture* tex = assets.GetTexture(id);
        if (!tex) {
            assets.Load(id);
            assets.WaitIdle();
            tex = assets.GetTexture(id);
        }
        if (tex && !tex->mips.Empty()) {
            using TF = asset::TextureFormat;
            if (tex->format == TF::RGBA8 || tex->format == TF::RGBA8_SRGB) {
                img = MakeUnique<Image>(Image::Create(tex->width, tex->height, ImageFormat::RGBA8));
                MemCopy(img->pixels.Data(), tex->mips[0].Data(), Min(img->pixels.Size(), tex->mips[0].Size()));
            } else if (tex->format == TF::BC1 || tex->format == TF::BC1_SRGB || tex->format == TF::BC3 || tex->format == TF::BC3_SRGB ||
                       tex->format == TF::BC7 || tex->format == TF::BC7_SRGB) {
                const texture::BlockFormat bf = tex->format == TF::BC1 || tex->format == TF::BC1_SRGB   ? texture::BlockFormat::BC1
                                                : tex->format == TF::BC3 || tex->format == TF::BC3_SRGB ? texture::BlockFormat::BC3
                                                                                                        : texture::BlockFormat::BC7;
                img = MakeUnique<Image>(texture::DecompressBlocks(tex->mips[0].View(), tex->width, tex->height, bf));
            }
        }
        const Image* raw = img.Get();
        images.InsertOrAssign(id, move(img));
        return raw;
    };
    Array<BakeTriangle> tris;
    f32 minY = Infinity, maxY = -Infinity;
    for (const FoliagePart& part : parts) {
        const asset::CookedMesh* mesh = assets.GetMesh(part.mesh);
        if (!mesh || mesh->vertices.Empty())
            return false;
        const Mat4 m = part.local.ToMatrix();
        for (const asset::CookedMeshSection& sec : mesh->sections) {
            const asset::MaterialAsset* mat =
                sec.materialSlot < part.materials.Size() ? assets.GetMaterial(part.materials[sec.materialSlot]) : nullptr;
            const asset::CookedSectionLod& lod = mesh->SectionLod(sec, 0);
            for (u32 i = 0; i + 2 < lod.indexCount; i += 3) {
                BakeTriangle t;
                for (u32 k = 0; k < 3; ++k) {
                    const asset::MeshVertex v = asset::UnpackVertex(mesh->vertices[mesh->Index(lod.indexOffset + i + k)]);
                    t.p[k] = m.TransformPointAffine(v.position);
                    t.n[k] = SafeNormalize(m.TransformVector(v.normal), Vec3(0.0f, 1.0f, 0.0f));
                    t.uv[k] = v.uv0;
                    const Vec4 c(f32(v.color & 0xFF) / 255.0f, f32((v.color >> 8) & 0xFF) / 255.0f, f32((v.color >> 16) & 0xFF) / 255.0f,
                                 f32(v.color >> 24) / 255.0f);
                    t.tint[k] = (mat ? mat->baseColor : Vec4(1.0f)) * c;
                    minY = Min(minY, t.p[k].y);
                    maxY = Max(maxY, t.p[k].y);
                }
                if (mat) {
                    t.texture = imageOf(mat->baseColorTexture);
                    t.uvScale = mat->uvScale;
                    t.uvOffset = mat->uvOffset;
                    t.mask = mat->alphaMode == asset::AlphaMode::Mask;
                    t.cutoff = mat->alphaCutoff;
                }
                tris.PushBack(t);
            }
        }
    }
    if (tris.Empty())
        return false;
    out.frames = frames;
    out.frameSize = frameSize;
    out.centerY = (minY + maxY) * 0.5f;
    const Vec3 center(0.0f, out.centerY, 0.0f);
    f32 radius = 1e-3f;
    for (const BakeTriangle& t : tris)
        for (const Vec3& p : t.p)
            radius = Max(radius, Length(p - center));
    out.radius = radius * 1.02f;

    const u32 atlas = frames * frameSize, res = frameSize * Supersample;
    Image albedo = Image::Create(atlas, atlas, ImageFormat::RGBA8);
    Image normal = Image::Create(atlas, atlas, ImageFormat::RGBA8);
    Array<f32> depth;
    Array<Vec4> color; // rgb linear, a = covered
    Array<Vec3> nrm;
    depth.Resize(usize(res) * res);
    color.Resize(usize(res) * res);
    nrm.Resize(usize(res) * res);
    for (u32 fy = 0; fy < frames; ++fy)
        for (u32 fx = 0; fx < frames; ++fx) {
            const Vec3 dir = ImpostorFrameDirection(fx, fy, frames);
            Vec3 right, up;
            ImpostorFrameAxes(dir, right, up);
            for (usize i = 0; i < depth.Size(); ++i) {
                depth[i] = -Infinity;
                color[i] = Vec4();
                nrm[i] = Vec3();
            }
            const f32 scale = f32(res) * 0.5f / out.radius;
            for (const BakeTriangle& t : tris) {
                Vec3 s[3]; // screen x, y (pixels, y down), depth towards the viewer
                for (u32 k = 0; k < 3; ++k) {
                    const Vec3 d = t.p[k] - center;
                    s[k] = Vec3(f32(res) * 0.5f + Dot(d, right) * scale, f32(res) * 0.5f - Dot(d, up) * scale, Dot(d, dir));
                }
                const f32 area = (s[1].x - s[0].x) * (s[2].y - s[0].y) - (s[2].x - s[0].x) * (s[1].y - s[0].y);
                if (Abs(area) < 1e-8f)
                    continue;
                const i32 x0 = Max(i32(std::floor(Min(s[0].x, Min(s[1].x, s[2].x)))), 0);
                const i32 x1 = Min(i32(std::ceil(Max(s[0].x, Max(s[1].x, s[2].x)))), i32(res) - 1);
                const i32 y0 = Max(i32(std::floor(Min(s[0].y, Min(s[1].y, s[2].y)))), 0);
                const i32 y1 = Min(i32(std::ceil(Max(s[0].y, Max(s[1].y, s[2].y)))), i32(res) - 1);
                for (i32 py = y0; py <= y1; ++py)
                    for (i32 px = x0; px <= x1; ++px) {
                        const f32 cx = f32(px) + 0.5f, cy = f32(py) + 0.5f;
                        const f32 w0 = ((s[1].x - cx) * (s[2].y - cy) - (s[2].x - cx) * (s[1].y - cy)) / area;
                        const f32 w1 = ((s[2].x - cx) * (s[0].y - cy) - (s[0].x - cx) * (s[2].y - cy)) / area;
                        const f32 w2 = 1.0f - w0 - w1;
                        if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f)
                            continue;
                        const f32 z = w0 * s[0].z + w1 * s[1].z + w2 * s[2].z;
                        const usize idx = usize(py) * res + usize(px);
                        if (z <= depth[idx])
                            continue;
                        Vec4 c = t.tint[0] * w0 + t.tint[1] * w1 + t.tint[2] * w2;
                        if (t.texture) {
                            const Vec2 uv = (t.uv[0] * w0 + t.uv[1] * w1 + t.uv[2] * w2) * t.uvScale + t.uvOffset;
                            c = c * SampleTexture(*t.texture, uv);
                        }
                        if (t.mask && c.w < t.cutoff)
                            continue;
                        Vec3 n = SafeNormalize(t.n[0] * w0 + t.n[1] * w1 + t.n[2] * w2, dir);
                        if (Dot(n, dir) < 0.0f)
                            n = -n; // leaves are two-sided; a hidden back face never wins the depth test
                        depth[idx] = z;
                        color[idx] = Vec4(c.x, c.y, c.z, 1.0f);
                        nrm[idx] = Vec3(Dot(n, right), Dot(n, up), Dot(n, dir));
                    }
            }
            // Down to the frame: coverage -> alpha, average of the covered samples.
            for (u32 y = 0; y < frameSize; ++y)
                for (u32 x = 0; x < frameSize; ++x) {
                    Vec3 c, n;
                    u32 covered = 0;
                    for (u32 sy = 0; sy < Supersample; ++sy)
                        for (u32 sx = 0; sx < Supersample; ++sx) {
                            const usize idx = usize(y * Supersample + sy) * res + (x * Supersample + sx);
                            if (color[idx].w > 0.0f) {
                                c += color[idx].XYZ();
                                n += nrm[idx];
                                ++covered;
                            }
                        }
                    u8* a = albedo.Row(fy * frameSize + y) + usize(fx * frameSize + x) * 4;
                    u8* b = normal.Row(fy * frameSize + y) + usize(fx * frameSize + x) * 4;
                    if (covered) {
                        c = c / f32(covered);
                        n = SafeNormalize(n, Vec3(0.0f, 0.0f, 1.0f));
                    } else {
                        n = Vec3(0.0f, 0.0f, 1.0f);
                    }
                    for (u32 k = 0; k < 3; ++k)
                        a[k] = u8(Clamp(LinearToSRGB(Clamp(c[k], 0.0f, 1.0f)) * 255.0f + 0.5f, 0.0f, 255.0f));
                    a[3] = u8(covered * 255u / (Supersample * Supersample));
                    b[0] = u8(Clamp(n.x * 127.5f + 127.5f, 0.0f, 255.0f));
                    b[1] = u8(Clamp(n.y * 127.5f + 127.5f, 0.0f, 255.0f));
                    b[2] = u8(Clamp(n.z * 127.5f + 127.5f, 0.0f, 255.0f));
                    b[3] = 255;
                }
        }
    // Empty texels take the color of their covered neighbors (filtering and mips don't darken the edges).
    for (u32 pass = 0; pass < 8; ++pass) {
        Image next = albedo;
        Image nextN = normal;
        bool any = false;
        for (u32 y = 0; y < atlas; ++y)
            for (u32 x = 0; x < atlas; ++x) {
                const u8* a = albedo.Row(y) + usize(x) * 4;
                if (a[3] != 0 || (pass > 0 && (a[0] | a[1] | a[2]) != 0))
                    continue;
                u32 sum[3] = {0, 0, 0}, sumN[3] = {0, 0, 0}, count = 0;
                for (i32 dy = -1; dy <= 1; ++dy)
                    for (i32 dx = -1; dx <= 1; ++dx) {
                        const i32 nx = i32(x) + dx, ny = i32(y) + dy;
                        if (nx < 0 || ny < 0 || nx >= i32(atlas) || ny >= i32(atlas) || u32(nx) / frameSize != x / frameSize ||
                            u32(ny) / frameSize != y / frameSize)
                            continue;
                        const u8* q = albedo.Row(u32(ny)) + usize(nx) * 4;
                        if (q[3] == 0 && (q[0] | q[1] | q[2]) == 0)
                            continue;
                        const u8* qn = normal.Row(u32(ny)) + usize(nx) * 4;
                        for (u32 k = 0; k < 3; ++k) {
                            sum[k] += q[k];
                            sumN[k] += qn[k];
                        }
                        ++count;
                    }
                if (!count)
                    continue;
                u8* o = next.Row(y) + usize(x) * 4;
                u8* on = nextN.Row(y) + usize(x) * 4;
                for (u32 k = 0; k < 3; ++k) {
                    o[k] = u8(Max(sum[k] / count, 1u)); // non-zero: marks it filled
                    on[k] = u8(sumN[k] / count);
                }
                any = true;
            }
        albedo = move(next);
        normal = move(nextN);
        if (!any)
            break;
    }
    asset::TextureCookOptions co;
    co.srgb = true;
    co.preserveAlphaCoverage = true;
    co.alphaCutoff = 0.5f;
    Result<asset::CookedTexture> a = asset::CookTexture(albedo, co);
    asset::TextureCookOptions no;
    no.srgb = false;
    no.normalMap = true;
    Result<asset::CookedTexture> n = asset::CookTexture(normal, no);
    if (!a.IsOk() || !n.IsOk())
        return false;
    out.albedo = move(a.Value());
    out.normal = move(n.Value());
    out.albedo.name = "ImpostorAlbedo";
    out.normal.name = "ImpostorNormal";
    out.albedo.wrapU = out.albedo.wrapV = out.normal.wrapU = out.normal.wrapV = asset::TextureWrap::Clamp;
    return true;
}

bool BakeImpostorFromFile(StringView modelPath, ImpostorAsset& out, UniquePtr<asset::AssetManager>& cooker)
{
    if (!cooker) {
        asset::AssetManagerSettings s;
        s.ddcDirectory = platform::GetTempDirectory() + "/zx_impostor_ddc";
        s.writeMetaFiles = false;
        cooker.Reset(new asset::AssetManager(s));
    }
    const asset::AssetId id = cooker->LoadPath(modelPath);
    cooker->WaitIdle();
    const asset::PrefabAsset* prefab = id.IsValid() ? cooker->GetPrefab(id) : nullptr;
    Array<FoliagePart> parts;
    if (!prefab || !FoliagePartsFromPrefab(*prefab, parts) || parts.Empty()) {
        ZX_LOG_WARN(LogImpostor, "Impostor: {} could not be loaded", modelPath);
        return false;
    }
    for (const FoliagePart& p : parts) {
        cooker->Load(p.mesh);
        for (const asset::AssetId& m : p.materials)
            cooker->Load(m);
    }
    cooker->WaitIdle();
    if (!BakeImpostor(*cooker, parts.View(), out)) {
        ZX_LOG_WARN(LogImpostor, "Impostor: {} could not be baked", modelPath);
        return false;
    }
    return true;
}

void SerializeImpostor(const ImpostorAsset& imp, Array<u8>& out)
{
    BinaryWriter w(out);
    w.Write<u32>(ImpostorMagic);
    w.Write<u32>(ImpostorVersion);
    w.Write<u32>(imp.frames);
    w.Write<u32>(imp.frameSize);
    w.Write<f32>(imp.centerY);
    w.Write<f32>(imp.radius);
    Array<u8> bytes;
    asset::Serialize(imp.albedo, bytes);
    w.WriteArray(Span<const u8>(bytes.Data(), bytes.Size()));
    bytes.Clear();
    asset::Serialize(imp.normal, bytes);
    w.WriteArray(Span<const u8>(bytes.Data(), bytes.Size()));
}

bool DeserializeImpostor(ByteSpan data, ImpostorAsset& out)
{
    BinaryReader r(data);
    if (r.Read<u32>() != ImpostorMagic || r.Read<u32>() != ImpostorVersion)
        return false;
    out.frames = r.Read<u32>();
    out.frameSize = r.Read<u32>();
    out.centerY = r.Read<f32>();
    out.radius = r.Read<f32>();
    Array<u8> a, n;
    if (!r.ReadArray(a) || !r.ReadArray(n) || r.HasError() || out.frames == 0 || out.frames > 64)
        return false;
    return asset::Deserialize(a.View(), out.albedo) && asset::Deserialize(n.View(), out.normal);
}

ImpostorIds RegisterImpostor(asset::AssetManager& assets, const ImpostorAsset& imp, StringView key)
{
    const Guid base = Guid::FromName(Guid(0x494D504F53544F52ull, 0x5A58334450617274ull), key);
    ImpostorIds ids;
    ids.mesh = Guid::FromName(base, "mesh");
    ids.material = Guid::FromName(base, "material");
    ids.albedo = Guid::FromName(base, "albedo");
    ids.normal = Guid::FromName(base, "normal");
    if (assets.GetState(ids.mesh) == asset::AssetState::Loaded)
        return ids;
    assets.AddMemoryTexture(ids.albedo, imp.albedo);
    assets.AddMemoryTexture(ids.normal, imp.normal);
    asset::MaterialAsset m;
    m.name = String::Format("Impostor {}", key);
    m.alphaMode = asset::AlphaMode::Mask;
    m.alphaCutoff = 0.5f;
    m.roughness = 1.0f;
    m.baseColorTexture = ids.albedo;
    m.normalTexture = ids.normal;
    assets.AddMemoryMaterial(ids.material, move(m));
    // The quad: corners at +-radius around the sphere's center (the shader turns it towards the camera). Indices wind
    // clockwise as the camera sees the quad (front faces).
    asset::CookedMesh q;
    q.name = String::Format("Impostor {}", key);
    const f32 r = imp.radius, c = imp.centerY;
    const Vec3 corners[4] = {Vec3(-r, c + r, 0.0f), Vec3(r, c + r, 0.0f), Vec3(r, c - r, 0.0f), Vec3(-r, c - r, 0.0f)};
    const Vec2 uvs[4] = {Vec2(0, 0), Vec2(1, 0), Vec2(1, 1), Vec2(0, 1)};
    for (u32 i = 0; i < 4; ++i) {
        asset::MeshVertex v;
        v.position = corners[i];
        v.normal = Vec3(0.0f, 0.0f, 1.0f);
        v.tangent = Vec4(1.0f, 0.0f, 0.0f, 1.0f);
        v.uv0 = uvs[i];
        q.vertices.PushBack(asset::PackVertex(v));
    }
    const u16 indices[6] = {0, 2, 1, 0, 3, 2};
    q.index16 = true;
    q.indexData.ResizeUninitialized(sizeof(indices));
    MemCopy(q.indexData.Data(), indices, sizeof(indices));
    q.bounds = AABB(Vec3(-r, c - r, -r), Vec3(r, c + r, r)); // the quad turns: cull with the whole sphere
    asset::CookedSectionLod lod;
    lod.indexCount = 6;
    q.sectionLods.PushBack(lod);
    asset::CookedMeshSection sec;
    sec.bounds = q.bounds;
    q.sections.PushBack(sec);
    q.materialSlotCount = 1;
    q.slotSourceMaterial.PushBack(0);
    assets.AddMemoryMesh(ids.mesh, move(q));
    return ids;
}

void MakeImpostorFoliage(const ImpostorIds& ids, const ImpostorAsset& imp, Foliage& f)
{
    FoliagePart part;
    part.mesh = ids.mesh;
    part.materials.PushBack(ids.material);
    f.parts.Clear();
    f.parts.PushBack(part);
    f.castShadows = false;
    f.windSway = 0.0f;
    f.collision = FoliageCollision::None;
    f.impostorFrames = imp.frames;
    f.impostorCenterY = imp.centerY;
}

bool WantsImpostor(const Foliage& f) { return !f.parts.Empty() && FoliageCollisionOf(f) != FoliageCollision::None; }

} // namespace zx
