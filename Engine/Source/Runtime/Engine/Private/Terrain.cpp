#include "Engine/Terrain.h"
#include "Asset/CookedAssets.h"
#include "Asset/MeshData.h"
#include "Core/Log/Log.h"
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
    ZX_FIELD(height, Meta::Range(0, 2000), Meta::Units("m"), Meta::Tooltip("From the lowest to the highest possible point"))
    ZX_FIELD(seed, Meta::Range(0, 100000), Meta::Tooltip("Another seed = another landscape"))
    ZX_FIELD(featureSize, Meta::Range(1, 5000), Meta::Units("m"), Meta::Tooltip("Width of the largest hills"))
    ZX_FIELD(octaves, Meta::Range(1, 10), Meta::Tooltip("Layers of ever finer detail"))
    ZX_FIELD(roughness, Meta::Range(0, 1), Meta::Tooltip("How strong the finer layers are"))
    ZX_FIELD(ridges, Meta::Range(0, 1), Meta::Tooltip("0 = rolling hills, 1 = sharp ridged mountains"))
    ZX_FIELD(flatCenter, Meta::Range(0, 2000), Meta::Units("m"), Meta::Tooltip("Radius of a flat area in the middle (0 = none)"))
    ZX_FIELD(material, Meta::AssetType("Material"), Meta::Tooltip("Surface material (M_Terrain in Starter Content: grass, rock on slopes, snow on peaks)"))
    ZX_FIELD(collision, Meta::Tooltip("Physics collision (a height field) and Character Controllers walk on it"))
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

/// The render mesh, cooked directly (a regular grid needs no cache optimization): square chunks of cells become
/// sections with their own bounds, so the renderer culls what is off screen.
asset::CookedMesh CookTerrainMesh(const Terrain& t, const Array<f32>& h)
{
    const u32 n = TerrainSamples(t);
    const f32 s = TerrainSpacing(t), half = t.size * 0.5f;
    asset::CookedMesh out;
    out.name = "Terrain";
    out.vertices.ResizeUninitialized(usize(n) * n);
    auto at = [&](i32 x, i32 z) { return h[usize(Clamp(z, 0, i32(n) - 1)) * n + usize(Clamp(x, 0, i32(n) - 1))]; };
    for (u32 z = 0; z < n; ++z)
        for (u32 x = 0; x < n; ++x) {
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
            out.vertices[usize(z) * n + x] = asset::PackVertex(v);
            out.bounds.Expand(v.position);
        }
    // At most 8 x 8 chunks (few draw calls), at least 32 cells wide.
    const u32 cells = n - 1, chunk = Max(32u, (cells + 7) / 8);
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
    return h;
}

asset::AssetId TerrainMeshId(const ecs::World& world, ecs::Entity e)
{
    return Guid::FromName(TerrainNamespace, String::Format("{}", GetEntityGuid(world, e)).View());
}

void TerrainSystem::Update(ecs::World& world, asset::AssetManager& assets, render::RenderAssets* renderAssets)
{
    Array<ecs::Entity> terrains;
    world.Query<const Terrain>().Each([&](ecs::Entity e, const Terrain&) { terrains.PushBack(e); });
    for (ecs::Entity e : terrains) {
        EnsureEntityGuid(world, e);
        const asset::AssetId id = TerrainMeshId(world, e);
        const Terrain t = *world.Get<const Terrain>(e);
        const u64 hash = TerrainShapeHash(t);
        Built* built = m_built.Find(id);
        const f64 now = Clock::NowSeconds();
        // While a value is being dragged in Details, rebuild a few times per second (the last value always lands).
        if (!built || (built->hash != hash && now - built->time > 0.12)) {
            const f64 start = now;
            Array<f32> heights;
            GenerateTerrainHeights(t, heights);
            asset::CookedMesh cooked = CookTerrainMesh(t, heights);
            assets.AddMemoryMesh(id, move(cooked));
            if (renderAssets)
                renderAssets->ReloadMesh(id);
            m_built.InsertOrAssign(id, Built{hash, id, Clock::NowSeconds()});
            ++m_builds;
            ZX_LOG_INFO(LogTerrain, "Terrain: {} x {} samples built in {:.1f} ms", TerrainSamples(t), TerrainSamples(t),
                        (Clock::NowSeconds() - start) * 1000.0);
        }
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

} // namespace zx
