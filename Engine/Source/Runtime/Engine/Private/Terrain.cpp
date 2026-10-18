#include "Engine/Terrain.h"
#include "Asset/CookedAssets.h"
#include "Asset/MeshData.h"
#include "Core/Log/Log.h"
#include "Core/Math/Random.h"
#include "Core/String/Base64.h"
#include "Scene/Hierarchy.h"
#include "Core/Time/Time.h"
#include <bit>
#include "Engine/SceneRendering.h"
#include "Engine/SceneSerialization.h"
#include "Render/RenderAssets.h"

ZX_DEFINE_LOG_CATEGORY_STATIC(LogTerrain, Info);

namespace zx {

ZX_REFLECT_STRUCT_BEGIN(Terrain)
    ZX_FIELD(size, Meta::Range(4, 4000), Meta::Units("m"), Meta::Tooltip("Width of the square terrain, centered on the entity"))
    ZX_FIELD(resolution, Meta::Range(9, 1025), Meta::Tooltip("Height samples per side (more = finer detail, more triangles)"))
    ZX_FIELD(hills, Meta::Tooltip("Generated hills under the sculpting; off = flat ground to sculpt from scratch (Terrain tool, T)"))
    ZX_FIELD(height, Meta::Range(0, 2000), Meta::Units("m"), Meta::Tooltip("From the lowest to the highest possible point"))
    ZX_FIELD(seed, Meta::Range(0, 100000), Meta::Tooltip("Another seed = another landscape"))
    ZX_FIELD(featureSize, Meta::Range(1, 5000), Meta::Units("m"), Meta::Tooltip("Width of the largest hills"))
    ZX_FIELD(octaves, Meta::Range(1, 10), Meta::Tooltip("Layers of ever finer detail"))
    ZX_FIELD(roughness, Meta::Range(0, 1), Meta::Tooltip("How strong the finer layers are"))
    ZX_FIELD(ridges, Meta::Range(0, 1), Meta::Tooltip("0 = rolling hills, 1 = sharp ridged mountains"))
    ZX_FIELD(flatCenter, Meta::Range(0, 2000), Meta::Units("m"), Meta::Tooltip("Radius of a flat area in the middle (0 = none)"))
    ZX_FIELD(material, Meta::AssetType("Material"), Meta::Tooltip("Surface material (M_Terrain in Starter Content: grass, rock on slopes, snow on peaks)"))
    ZX_FIELD(collision, Meta::Tooltip("Physics collision (a height field) and Character Controllers walk on it"))
    ZX_FIELD(sculpt, Meta::Hidden())
    ZX_FIELD(paint, Meta::Hidden())
    ZX_FIELD(dataId, Meta::Hidden())
ZX_REFLECT_STRUCT_END()

namespace {

u32 HashInt(i32 x, i32 z, u32 seed)
{
    u32 h = u32(x) * 0x8DA6B343u ^ u32(z) * 0xD8163841u ^ seed * 0xCB1AB31Fu;
    h ^= h >> 13;
    h *= 0x5BD1E995u;
    h ^= h >> 15;
    return h;
}

/// 2D gradient noise in about [-1, 1].
f32 GradientNoise(f32 x, f32 z, u32 seed)
{
    const f32 fx = Floor(x), fz = Floor(z);
    const i32 ix = i32(fx), iz = i32(fz);
    const f32 tx = x - fx, tz = z - fz;
    auto grad = [&](i32 cx, i32 cz, f32 dx, f32 dz) {
        const f32 a = f32(HashInt(cx, cz, seed) & 0xFFFFu) * (TwoPi / 65536.0f);
        return Cos(a) * dx + Sin(a) * dz;
    };
    const f32 n00 = grad(ix, iz, tx, tz), n10 = grad(ix + 1, iz, tx - 1.0f, tz);
    const f32 n01 = grad(ix, iz + 1, tx, tz - 1.0f), n11 = grad(ix + 1, iz + 1, tx - 1.0f, tz - 1.0f);
    const f32 ux = tx * tx * tx * (tx * (tx * 6.0f - 15.0f) + 10.0f), uz = tz * tz * tz * (tz * (tz * 6.0f - 15.0f) + 10.0f);
    return Lerp(Lerp(n00, n10, ux), Lerp(n01, n11, ux), uz) * 1.41f;
}

/// Terrain shape 0..1 at local (x, z) meters.
f32 Shape(const Terrain& t, f32 x, f32 z)
{
    const u32 seed = u32(t.seed);
    const f32 f = 1.0f / Max(t.featureSize, 0.01f);
    f32 smooth = 0.0f, ridged = 0.0f, amp = 1.0f, total = 0.0f, freq = f;
    for (i32 o = 0; o < Clamp(t.octaves, 1, 10); ++o) {
        // Each octave is rotated a little and shifted so the grids don't line up.
        const f32 rx = x * 0.8f - z * 0.6f, rz = x * 0.6f + z * 0.8f;
        x = rx + 17.3f;
        z = rz - 9.1f;
        const f32 n = GradientNoise(x * freq, z * freq, seed + u32(o) * 101u);
        smooth += n * amp;
        const f32 r = 1.0f - Abs(n);
        ridged += r * r * amp;
        total += amp;
        amp *= Clamp(t.roughness, 0.0f, 1.0f);
        freq *= 2.0f;
    }
    smooth = Saturate(0.5f + 0.6f * smooth / total);
    ridged = Saturate(ridged / total);
    return Lerp(smooth, ridged, Saturate(t.ridges));
}

/// One vertex of the terrain grid (heights `h`, n x n, spacing `s`).
asset::PackedVertex TerrainVertex(const Terrain& t, const Array<f32>& h, const Array<u8>& paint, u32 n, f32 s, u32 x, u32 z)
{
    const f32 half = s * f32(n - 1) * 0.5f;
    auto at = [&](i32 xx, i32 zz) { return h[usize(Clamp(zz, 0, i32(n) - 1)) * n + usize(Clamp(xx, 0, i32(n) - 1))]; };
    asset::MeshVertex v;
    v.position = Vec3(-half + f32(x) * s, at(i32(x), i32(z)), -half + f32(z) * s);
    const f32 dx = (at(i32(x) + 1, i32(z)) - at(i32(x) - 1, i32(z))) / (2.0f * s);
    const f32 dz = (at(i32(x), i32(z) + 1) - at(i32(x), i32(z) - 1)) / (2.0f * s);
    v.normal = Normalize(Vec3(-dx, 1.0f, -dz));
    // Tangent along +X (u), bitangent towards -Z (v grows that way): w = +1.
    const Vec3 tangent = Normalize(Vec3(1.0f, dx, 0.0f) - v.normal * Dot(Vec3(1.0f, dx, 0.0f), v.normal));
    v.tangent = Vec4(tangent, 1.0f);
    v.uv0 = Vec2(v.position.x, -v.position.z) * 0.25f; // a texture tile every 4 m
    v.uv1 = Vec2(f32(x) / f32(n - 1), 1.0f - f32(z) / f32(n - 1));
    // Painted layer weights (rock, dirt, sand) for M_Terrain, stored inverted (unpainted = white: materials that
    // multiply by the vertex color stay as they are); none without a material.
    const usize pi = (usize(z) * n + x) * 4;
    v.color = 0xFFFFFFFFu;
    if (t.material.IsValid() && paint.Size() >= pi + 4)
        v.color = u32(255 - paint[pi]) | (u32(255 - paint[pi + 1]) << 8) | (u32(255 - paint[pi + 2]) << 16) | (u32(255 - paint[pi + 3]) << 24);
    return asset::PackVertex(v);
}

/// Cells per side of a culling chunk (section): at most 8 x 8 chunks (few draw calls), at least 32 cells wide.
u32 TerrainChunk(u32 n) { return Max(32u, (n - 1 + 7) / 8); }

/// Section bounds from the vertices (sections are the chunks in z-then-x order).
void TerrainSectionBounds(const Array<asset::PackedVertex>& vertices, u32 n, Array<asset::CookedMeshSection>& sections, AABB& bounds)
{
    const u32 cells = n - 1, chunk = TerrainChunk(n);
    bounds = AABB();
    usize si = 0;
    for (u32 cz = 0; cz < cells; cz += chunk)
        for (u32 cx = 0; cx < cells; cx += chunk, ++si) {
            AABB b;
            for (u32 z = cz; z <= Min(cz + chunk, cells); ++z)
                for (u32 x = cx; x <= Min(cx + chunk, cells); ++x)
                    b.Expand(vertices[usize(z) * n + x].position);
            if (si < sections.Size())
                sections[si].bounds = b;
            bounds.Expand(b.min);
            bounds.Expand(b.max);
        }
}

/// The render mesh, cooked directly (a regular grid needs no cache optimization): square chunks of cells become
/// sections with their own bounds, so the renderer culls what is off screen.
asset::CookedMesh CookTerrainMesh(const Terrain& t, const Array<f32>& h, const Array<u8>& paint)
{
    const u32 n = TerrainSamples(t);
    const f32 s = TerrainSpacing(t);
    asset::CookedMesh out;
    out.name = "Terrain";
    out.vertices.ResizeUninitialized(usize(n) * n);
    for (u32 z = 0; z < n; ++z)
        for (u32 x = 0; x < n; ++x) {
            out.vertices[usize(z) * n + x] = TerrainVertex(t, h, paint, n, s, x, z);
            out.bounds.Expand(out.vertices[usize(z) * n + x].position);
        }
    const u32 cells = n - 1, chunk = TerrainChunk(n);
    Array<u32> indices;
    indices.Reserve(usize(cells) * cells * 6);
    for (u32 cz = 0; cz < cells; cz += chunk)
        for (u32 cx = 0; cx < cells; cx += chunk) {
            asset::CookedMeshSection section;
            section.firstLod = u32(out.sectionLods.Size());
            const u32 first = u32(indices.Size());
            for (u32 z = cz; z < Min(cz + chunk, cells); ++z)
                for (u32 x = cx; x < Min(cx + chunk, cells); ++x) {
                    const u32 p00 = z * n + x, p10 = p00 + 1, p01 = p00 + n, p11 = p01 + 1;
                    // Same diagonal as the physics height field (p00 - p11): what you see is what you collide with.
                    for (u32 i : {p00, p11, p10, p00, p01, p11}) {
                        indices.PushBack(i);
                        section.bounds.Expand(out.vertices[i].position);
                    }
                }
            out.sectionLods.PushBack(asset::CookedSectionLod{first, u32(indices.Size()) - first, 1.0f, 0.0f});
            out.sections.PushBack(section);
        }
    out.lodCount = 1;
    out.materialSlotCount = 1;
    out.slotSourceMaterial.PushBack(0);
    out.index16 = out.vertices.Size() <= 0xFFFF;
    out.indexData.ResizeUninitialized(indices.Size() * (out.index16 ? 2 : 4));
    if (out.index16) {
        u16* dst = reinterpret_cast<u16*>(out.indexData.Data());
        for (usize i = 0; i < indices.Size(); ++i)
            dst[i] = u16(indices[i]);
    } else {
        MemCopy(out.indexData.Data(), indices.Data(), indices.Size() * 4);
    }
    return out;
}

const Guid TerrainNamespace(0x7A54657272616E31ull, 0x5A5833445465726Eull);

} // namespace

u32 TerrainSamples(const Terrain& t) { return u32(Clamp(t.resolution, 9, 1025)); }

f32 TerrainSpacing(const Terrain& t) { return Max(t.size, 0.01f) / f32(TerrainSamples(t) - 1); }

void GenerateTerrainHeights(const Terrain& t, Array<f32>& heights)
{
    const u32 n = TerrainSamples(t);
    const f32 s = TerrainSpacing(t), half = t.size * 0.5f;
    heights.Resize(usize(n) * n);
    const f32 center = Shape(t, 0.0f, 0.0f);
    const f32 flat = Max(t.flatCenter, 0.0f);
    const f32 blend = Max(flat * 0.6f, s * 3.0f);
    for (u32 z = 0; z < n; ++z)
        for (u32 x = 0; x < n; ++x) {
            const f32 px = -half + f32(x) * s, pz = -half + f32(z) * s;
            if (!t.hills) {
                heights[usize(z) * n + x] = 0.0f;
                continue;
            }
            f32 v = Shape(t, px, pz);
            if (flat > 0.0f) {
                const f32 r = Sqrt(px * px + pz * pz);
                v = Lerp(center, v, SmoothStep(flat, flat + blend, r));
            }
            heights[usize(z) * n + x] = v * t.height;
        }
}

u64 TerrainShapeHash(const Terrain& t)
{
    u64 h = 0xCBF29CE484222325ull;
    auto mix = [&](u64 v) { h = HashCombine(h, v); };
    auto bits = [](f32 f) { return u64(std::bit_cast<u32>(f)); };
    mix(bits(t.size));
    mix(u64(t.resolution));
    mix(bits(t.height));
    mix(u64(u32(t.seed)));
    mix(bits(t.featureSize));
    mix(u64(t.octaves));
    mix(bits(t.roughness));
    mix(bits(t.ridges));
    mix(bits(t.flatCenter));
    mix(t.hills ? 1u : 0u);
    return h;
}

asset::AssetId TerrainMeshId(const ecs::World& world, ecs::Entity e)
{
    return Guid::FromName(TerrainNamespace, String::Format("{}", GetEntityGuid(world, e)).View());
}

namespace {

/// Bilinear resample of an m x m grid of `channels` floats into n x n.
template <typename T>
void Resample(const Array<T>& src, u32 m, u32 channels, u32 n, Array<T>& dst)
{
    dst.Resize(usize(n) * n * channels);
    for (u32 z = 0; z < n; ++z)
        for (u32 x = 0; x < n; ++x) {
            const f32 fx = f32(x) / f32(n - 1) * f32(m - 1), fz = f32(z) / f32(n - 1) * f32(m - 1);
            const u32 x0 = Min(u32(fx), m - 1), z0 = Min(u32(fz), m - 1), x1 = Min(x0 + 1, m - 1), z1 = Min(z0 + 1, m - 1);
            const f32 tx = fx - f32(x0), tz = fz - f32(z0);
            for (u32 c = 0; c < channels; ++c) {
                auto at = [&](u32 xx, u32 zz) { return f32(src[(usize(zz) * m + xx) * channels + c]); };
                const f32 v = Lerp(Lerp(at(x0, z0), at(x1, z0), tx), Lerp(at(x0, z1), at(x1, z1), tx), tz);
                dst[(usize(z) * n + x) * channels + c] = T(v);
            }
        }
}

u32 GridSide(usize count)
{
    const u32 m = u32(Sqrt(f32(count)) + 0.5f);
    return usize(m) * m == count ? m : 0u;
}

constexpr f32 SculptStep = 0.02f; // m per i16 unit

} // namespace

void DecodeTerrainSculpt(const Terrain& t, u32 n, Array<f32>& offsets)
{
    offsets.Clear();
    offsets.Resize(usize(n) * n, 0.0f);
    Array<u8> bytes;
    if (t.sculpt.Empty() || !Base64Decode(t.sculpt.View(), bytes) || bytes.Size() % 2)
        return;
    const u32 m = GridSide(bytes.Size() / 2);
    if (m < 2)
        return;
    Array<f32> grid;
    grid.Resize(usize(m) * m);
    for (usize i = 0; i < grid.Size(); ++i)
        grid[i] = f32(i16(u16(bytes[i * 2]) | (u16(bytes[i * 2 + 1]) << 8))) * SculptStep;
    if (m == n)
        offsets = move(grid);
    else
        Resample(grid, m, 1, n, offsets);
}

void DecodeTerrainPaint(const Terrain& t, u32 n, Array<u8>& rgb)
{
    rgb.Clear();
    Array<u8> bytes;
    if (t.paint.Empty() || !Base64Decode(t.paint.View(), bytes))
        return;
    // RGBA (rock, dirt, sand, grass); older scenes stored RGB (3 * m^2 is never 4 * k^2, so the sizes don't collide).
    u32 channels = 4, m = bytes.Size() % 4 == 0 ? GridSide(bytes.Size() / 4) : 0u;
    if (m < 2 && bytes.Size() % 3 == 0) {
        channels = 3;
        m = GridSide(bytes.Size() / 3);
    }
    if (m < 2)
        return;
    if (channels == 3) {
        Array<u8> rgba;
        rgba.Resize(usize(m) * m * 4, 0);
        for (usize i = 0; i < usize(m) * m; ++i)
            for (u32 c = 0; c < 3; ++c)
                rgba[i * 4 + c] = bytes[i * 3 + c];
        bytes = move(rgba);
    }
    if (m == n)
        rgb = move(bytes);
    else
        Resample(bytes, m, 4, n, rgb);
}

void EncodeTerrainSculpt(Terrain& t, const Array<f32>& offsets)
{
    bool any = false;
    Array<u8> bytes;
    bytes.Resize(offsets.Size() * 2);
    for (usize i = 0; i < offsets.Size(); ++i) {
        const i32 q = i32(Clamp(Round(offsets[i] / SculptStep), -32767.0f, 32767.0f));
        any |= q != 0;
        bytes[i * 2] = u8(u16(i16(q)) & 0xFF);
        bytes[i * 2 + 1] = u8(u16(i16(q)) >> 8);
    }
    t.sculpt = any ? Base64Encode(ByteSpan(bytes.Data(), bytes.Size())) : String();
}

void EncodeTerrainPaint(Terrain& t, const Array<u8>& rgb)
{
    bool any = false;
    for (u8 v : rgb)
        any |= v != 0;
    t.paint = any ? Base64Encode(ByteSpan(rgb.Data(), rgb.Size())) : String();
}

void ComputeTerrainHeights(const Terrain& t, Array<f32>& heights)
{
    GenerateTerrainHeights(t, heights);
    if (t.sculpt.Empty())
        return;
    Array<f32> offsets;
    DecodeTerrainSculpt(t, TerrainSamples(t), offsets);
    for (usize i = 0; i < heights.Size() && i < offsets.Size(); ++i)
        heights[i] += offsets[i];
}

void TerrainSystem::Rebuild(ecs::World& world, ecs::Entity e, Data& d, asset::AssetManager& assets, render::RenderAssets* renderAssets)
{
    const Terrain& t = *world.Get<const Terrain>(e);
    const f64 start = Clock::NowSeconds();
    Array<f32> heights = d.base;
    for (usize i = 0; i < heights.Size(); ++i)
        heights[i] += d.sculpt[i];
    // While a tool edits it: only the touched rows, written into the resident GPU mesh (no reload: no flicker, no
    // reflection recapture, a fraction of the work).
    if (d.editing && renderAssets && d.dirtyX1 >= d.dirtyX0 && d.vertices.Size() == heights.Size()) {
        const u32 x0 = u32(Max(d.dirtyX0 - 1, 0)), x1 = Min(u32(d.dirtyX1 + 1), d.n - 1);
        const u32 z0 = u32(Max(d.dirtyZ0 - 1, 0)), z1 = Min(u32(d.dirtyZ1 + 1), d.n - 1);
        for (u32 z = z0; z <= z1; ++z)
            for (u32 x = x0; x <= x1; ++x)
                d.vertices[usize(z) * d.n + x] = TerrainVertex(t, heights, d.paint, d.n, d.spacing, x, z);
        TerrainSectionBounds(d.vertices, d.n, d.sections, d.bounds);
        const usize first = usize(z0) * d.n, count = usize(z1 - z0 + 1) * d.n;
        if (renderAssets->UpdateMeshVertices(d.mesh, u32(first), Span<const asset::PackedVertex>(d.vertices.Data() + first, count), d.bounds,
                                             d.sections.View())) {
            d.time = Clock::NowSeconds();
            d.dirty = false;
            d.dirtyX0 = d.dirtyZ0 = 1 << 30;
            d.dirtyX1 = d.dirtyZ1 = -1;
            return;
        }
    }
    asset::CookedMesh cooked = CookTerrainMesh(t, heights, d.paint);
    d.vertices = cooked.vertices;
    d.sections = cooked.sections;
    d.bounds = cooked.bounds;
    assets.AddMemoryMesh(d.mesh, move(cooked));
    if (renderAssets)
        renderAssets->ReloadMesh(d.mesh);
    d.time = Clock::NowSeconds();
    d.dirty = false;
    d.dirtyX0 = d.dirtyZ0 = 1 << 30;
    d.dirtyX1 = d.dirtyZ1 = -1;
    ++m_builds;
    if (!d.editing)
        ZX_LOG_INFO(LogTerrain, "Terrain: {} x {} samples built in {:.1f} ms", d.n, d.n, (Clock::NowSeconds() - start) * 1000.0);
}

void TerrainSystem::Update(ecs::World& world, asset::AssetManager& assets, render::RenderAssets* renderAssets)
{
    Array<ecs::Entity> terrains;
    world.Query<const Terrain>().Each([&](ecs::Entity e, const Terrain&) { terrains.PushBack(e); });
    for (ecs::Entity e : terrains) {
        EnsureEntityGuid(world, e);
        const asset::AssetId id = TerrainMeshId(world, e);
        const Terrain& t = *world.Get<const Terrain>(e);
        const u64 hash = TerrainShapeHash(t);
        Data* d = m_built.Find(id);
        const f64 now = Clock::NowSeconds();
        if (!d) {
            m_built.InsertOrAssign(id, Data{});
            d = m_built.Find(id);
            d->mesh = id;
            d->hash = ~hash; // build now
            d->dataId = t.dataId - 1;
        }
        // The generated shape: while a value is being dragged in Details, a few times per second (the last value lands).
        if (d->hash != hash && (d->base.Empty() || now - d->time > 0.12)) {
            d->hash = hash;
            d->n = TerrainSamples(t);
            d->spacing = TerrainSpacing(t);
            GenerateTerrainHeights(t, d->base);
            if (d->sculpt.Size() != d->base.Size())
                d->dataId = t.dataId - 1; // resolution changed: resample what was sculpted
            d->dirty = true;
        }
        // Sculpted / painted data from the component (scene load, undo, redo) unless a tool is editing it.
        if (!d->editing && d->dataId != t.dataId) {
            d->dataId = t.dataId;
            DecodeTerrainSculpt(t, d->n, d->sculpt);
            DecodeTerrainPaint(t, d->n, d->paint);
            d->dirty = true;
        }
        if (d->dirty) // every frame while sculpting: the ground follows the brush
            Rebuild(world, e, *d, assets, renderAssets);
        // The Mesh Renderer draws it (and makes picking, shadows and editor tools work as for any mesh).
        MeshRenderer* mr = world.Get<MeshRenderer>(e);
        if (!mr) {
            world.Add(e, MeshRenderer{});
            mr = world.Get<MeshRenderer>(e);
        }
        if (mr->mesh != id)
            mr->mesh = id;
        const bool wantMaterial = t.material.IsValid();
        if (wantMaterial && (mr->materials.Size() != 1 || mr->materials[0] != t.material)) {
            mr->materials.Clear();
            mr->materials.PushBack(t.material);
        } else if (!wantMaterial && !mr->materials.Empty()) {
            mr->materials.Clear();
        }
        if (mr->collision != t.collision)
            mr->collision = t.collision;
    }
}

TerrainSystem::Data* TerrainSystem::Find(ecs::World& world, ecs::Entity e)
{
    if (!world.IsAlive(e) || !world.Has<Terrain>(e))
        return nullptr;
    Data* d = m_built.Find(TerrainMeshId(world, e));
    return d && !d->base.Empty() && d->sculpt.Size() == d->base.Size() ? d : nullptr;
}

TerrainSystem::Data* TerrainSystem::BeginEdit(ecs::World& world, ecs::Entity e)
{
    Data* d = Find(world, e);
    if (!d)
        return nullptr;
    d->editing = true;
    if (d->paint.Size() != d->base.Size() * 4)
        d->paint.Resize(d->base.Size() * 4, 0);
    return d;
}

void TerrainSystem::Commit(ecs::World& world, ecs::Entity e)
{
    Data* d = Find(world, e);
    Terrain* t = world.IsAlive(e) ? world.Get<Terrain>(e) : nullptr;
    if (!d || !t)
        return;
    EncodeTerrainSculpt(*t, d->sculpt);
    EncodeTerrainPaint(*t, d->paint);
    static Random rng(0x7e44a1u);
    i32 id;
    do {
        id = i32(rng.NextU32() & 0x7FFFFFFF);
    } while (id == 0 || id == t->dataId);
    t->dataId = id;
    d->dataId = id;
    d->editing = false; // the GPU mesh is already up to date (updated in place while editing)
}

bool TerrainSystem::HeightAt(ecs::World& world, ecs::Entity e, Vec2 worldXZ, f32& height)
{
    const Data* d = Find(world, e);
    if (!d)
        return false;
    const Mat4 m = scene::ComputeWorldMatrix(world, e);
    const Vec3 local = InverseAffine(m).TransformPointAffine(Vec3(worldXZ.x, 0.0f, worldXZ.y));
    const f32 half = d->spacing * f32(d->n - 1) * 0.5f;
    const f32 gx = (local.x + half) / d->spacing, gz = (local.z + half) / d->spacing;
    if (gx < 0.0f || gz < 0.0f || gx > f32(d->n - 1) || gz > f32(d->n - 1))
        return false;
    const u32 x0 = Min(u32(gx), d->n - 2), z0 = Min(u32(gz), d->n - 2);
    const f32 tx = gx - f32(x0), tz = gz - f32(z0);
    const f32 h = Lerp(Lerp(d->Height(x0, z0), d->Height(x0 + 1, z0), tx), Lerp(d->Height(x0, z0 + 1), d->Height(x0 + 1, z0 + 1), tx), tz);
    height = m.TransformPointAffine(Vec3(local.x, h, local.z)).y;
    return true;
}

bool TerrainSystem::Raycast(ecs::World& world, const Ray& ray, f32 maxDistance, ecs::Entity& hitTerrain, Vec3& hitPoint)
{
    bool hit = false;
    f32 best = maxDistance;
    world.Query<const Terrain>().Each([&](ecs::Entity e, const Terrain&) {
        const Data* d = Find(world, e);
        if (!d)
            return;
        // In the terrain's space, clipped to its box (heights min..max), marching half a cell at a time.
        const Mat4 m = scene::ComputeWorldMatrix(world, e);
        const Mat4 inv = InverseAffine(m);
        const Vec3 o = inv.TransformPointAffine(ray.origin);
        const Vec3 far = inv.TransformPointAffine(ray.origin + ray.direction * best);
        const f32 len = Length(far - o);
        if (len <= 1e-6f)
            return;
        const Vec3 dir = (far - o) / len;
        f32 lo = d->Height(0, 0), hi = lo;
        for (usize i = 0; i < d->base.Size(); ++i) {
            const f32 h = d->base[i] + d->sculpt[i];
            lo = Min(lo, h);
            hi = Max(hi, h);
        }
        const f32 half = d->spacing * f32(d->n - 1) * 0.5f;
        const AABB box(Vec3(-half, lo - 0.01f, -half), Vec3(half, hi + 0.01f, half));
        f32 t0 = 0.0f, t1 = len;
        for (u32 a = 0; a < 3; ++a) {
            if (Abs(dir[a]) < 1e-8f) {
                if (o[a] < box.min[a] || o[a] > box.max[a])
                    return;
                continue;
            }
            f32 ta = (box.min[a] - o[a]) / dir[a], tb = (box.max[a] - o[a]) / dir[a];
            if (ta > tb) {
                const f32 tmp = ta;
                ta = tb;
                tb = tmp;
            }
            t0 = Max(t0, ta);
            t1 = Min(t1, tb);
        }
        if (t0 > t1)
            return;
        auto height = [&](f32 x, f32 z) {
            const f32 gx = Clamp((x + half) / d->spacing, 0.0f, f32(d->n - 1)), gz = Clamp((z + half) / d->spacing, 0.0f, f32(d->n - 1));
            const u32 x0 = Min(u32(gx), d->n - 2), z0 = Min(u32(gz), d->n - 2);
            const f32 tx = gx - f32(x0), tz = gz - f32(z0);
            return Lerp(Lerp(d->Height(x0, z0), d->Height(x0 + 1, z0), tx), Lerp(d->Height(x0, z0 + 1), d->Height(x0 + 1, z0 + 1), tx), tz);
        };
        const f32 step = d->spacing * 0.5f;
        f32 prevT = t0;
        Vec3 p = o + dir * t0;
        if (p.y < height(p.x, p.z))
            return; // starts below the surface
        for (f32 t = t0 + step;; t += step) {
            const f32 tc = Min(t, t1);
            p = o + dir * tc;
            if (p.y < height(p.x, p.z)) {
                f32 a = prevT, b = tc;
                for (u32 k = 0; k < 14; ++k) {
                    const f32 mid = (a + b) * 0.5f;
                    const Vec3 q = o + dir * mid;
                    if (q.y < height(q.x, q.z))
                        b = mid;
                    else
                        a = mid;
                }
                const Vec3 wp = m.TransformPointAffine(o + dir * b);
                const f32 dist = Length(wp - ray.origin);
                if (dist < best) {
                    best = dist;
                    hitPoint = wp;
                    hitTerrain = e;
                    hit = true;
                }
                return;
            }
            if (tc >= t1)
                return;
            prevT = tc;
        }
    });
    return hit;
}

} // namespace zx
