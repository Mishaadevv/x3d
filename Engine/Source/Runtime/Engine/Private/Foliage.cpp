#include "Engine/Foliage.h"
#include "Core/Containers/Algorithm.h"
#include "Core/Hash/Hash.h"
#include "Core/Math/Random.h"
#include "Core/String/Base64.h"
#include "Core/String/Path.h"

#include <cmath>

namespace zx {

ZX_REFLECT_STRUCT_BEGIN(FoliagePart)
    ZX_FIELD(mesh, Meta::AssetType("Mesh"), Meta::ReadOnly())
    ZX_FIELD(materials, Meta::AssetType("Material"))
    ZX_FIELD(local)
ZX_REFLECT_STRUCT_END()

ZX_REFLECT_ENUM_BEGIN(FoliageCollision)
    ZX_ENUM_VALUE(Auto)
    ZX_ENUM_VALUE(None)
    ZX_ENUM_VALUE(Trunk)
    ZX_ENUM_VALUE(Hull)
ZX_REFLECT_ENUM_END()

ZX_REFLECT_STRUCT_BEGIN(Foliage)
    ZX_FIELD(model, Meta::ReadOnly(), Meta::Tooltip("The model the plants were made from"))
    ZX_FIELD(parts, Meta::Hidden())
    ZX_FIELD(cullDistance, Meta::Range(10, 5000), Meta::Units("m"), Meta::Tooltip("Plants farther than this from the camera are not drawn"))
    ZX_FIELD(castShadows)
    ZX_FIELD(windSway, Meta::Range(-1, 5), Meta::Tooltip("How much the plants bend in the wind (Wind Zones): 0 = rigid, 1 = a tree, "
                                                         "3 = grass; -1 = by the model's name (rocks rigid, grass flexible)"))
    ZX_FIELD(collision, Meta::Tooltip("How the plants collide while playing: Trunk = a capsule around the trunk (trees), Hull = the "
                                      "convex shape of the mesh (rocks), None = walk through (grass); Auto = by the model's name"))
    ZX_FIELD(data, Meta::Hidden())
    ZX_FIELD(dataId, Meta::Hidden())
ZX_REFLECT_STRUCT_END()

f32 FoliageWindSway(const Foliage& f)
{
    if (f.windSway >= 0.0f)
        return f.windSway;
    String name(Path::Stem(f.model));
    name.ToLowerInPlace();
    for (const char* rigid : {"rock", "stone", "boulder", "pebble", "cliff", "log", "stump", "mushroom", "dead"})
        if (name.Contains(StringView(rigid)))
            return name.Contains(StringView("dead")) ? 0.4f : 0.0f; // dead trees: bare, stiff branches
    for (const char* soft : {"grass", "fern", "flower", "clover", "plant", "reed", "petal", "wheat", "weed"})
        if (name.Contains(StringView(soft)))
            return 3.0f;
    if (name.Contains(StringView("bush")) || name.Contains(StringView("shrub")))
        return 1.5f;
    if (name.Contains(StringView("pine")) || name.Contains(StringView("fir")))
        return 0.8f;
    return 1.0f;
}

FoliageCollision FoliageCollisionOf(const Foliage& f)
{
    if (f.collision != FoliageCollision::Auto)
        return f.collision;
    String name(Path::Stem(f.model));
    name.ToLowerInPlace();
    for (const char* solid : {"rock", "stone", "boulder", "cliff", "stump", "log"})
        if (name.Contains(StringView(solid)))
            return FoliageCollision::Hull;
    for (const char* tree : {"tree", "pine", "fir", "oak", "birch", "palm", "spruce", "trunk"})
        if (name.Contains(StringView(tree)))
            return FoliageCollision::Trunk;
    return FoliageCollision::None;
}

bool EstimateFoliageTrunk(Span<const Vec3> positions, FoliageTrunk& out)
{
    if (positions.Empty())
        return false;
    f32 minY = positions[0].y, maxY = positions[0].y;
    for (Vec3 p : positions) {
        minY = Min(minY, p.y);
        maxY = Max(maxY, p.y);
    }
    const f32 height = maxY - minY;
    const f32 band = minY + Max(height * 0.08f, 1e-3f);
    Vec3 center;
    u32 n = 0;
    for (Vec3 p : positions)
        if (p.y <= band) {
            center += Vec3(p.x, 0.0f, p.z);
            ++n;
        }
    center = center / f32(Max(n, 1u));
    // Radius: the 75th percentile of the base ring (roots and stray vertices don't widen it).
    Array<f32> d;
    for (Vec3 p : positions)
        if (p.y <= band)
            d.PushBack(Length(Vec2(p.x - center.x, p.z - center.z)));
    Sort(d, [](f32 a, f32 b) { return a < b; });
    const f32 r = d.Empty() ? 0.0f : d[Min(d.Size() - 1, usize(f32(d.Size()) * 0.75f))];
    out.base = Vec3(center.x, minY, center.z);
    out.radius = Clamp(r, 0.04f, Max(height * 0.12f, 0.04f));
    out.height = height * 0.5f;
    return true;
}

void EncodeFoliage(Foliage& f, Span<const FoliageInstance> instances)
{
    Array<f32> floats;
    floats.ResizeUninitialized(instances.Size() * 5);
    for (usize i = 0; i < instances.Size(); ++i) {
        const FoliageInstance& in = instances[i];
        f32* d = &floats[i * 5];
        d[0] = in.position.x;
        d[1] = in.position.y;
        d[2] = in.position.z;
        d[3] = in.yaw;
        d[4] = in.scale;
    }
    f.data = Base64Encode(ByteSpan(reinterpret_cast<const u8*>(floats.Data()), floats.SizeBytes()));
    f.dataId = Max(1, i32(HashFnv1a64(f.data.View()) & 0x7FFFFFFF)); // the same plants keep their id
    f.instances.Clear();
    f.instances.Append(instances);
    f.decodedId = f.dataId;
}

void DecodeFoliage(Foliage& f)
{
    if (f.decodedId == f.dataId)
        return;
    f.decodedId = f.dataId;
    f.instances.Clear();
    Array<u8> bytes;
    if (!Base64Decode(f.data.View(), bytes))
        return;
    const usize n = bytes.Size() / (5 * sizeof(f32));
    const f32* d = reinterpret_cast<const f32*>(bytes.Data());
    f.instances.ResizeUninitialized(n);
    for (usize i = 0; i < n; ++i)
        f.instances[i] = FoliageInstance{Vec3(d[i * 5], d[i * 5 + 1], d[i * 5 + 2]), d[i * 5 + 3], d[i * 5 + 4]};
}

bool FoliagePartsFromPrefab(const asset::PrefabAsset& prefab, Array<FoliagePart>& out)
{
    out.Clear();
    Array<Mat4> global;
    global.Resize(prefab.nodes.Size());
    for (usize i = 0; i < prefab.nodes.Size(); ++i) {
        const asset::PrefabNode& n = prefab.nodes[i];
        global[i] = n.parent >= 0 ? global[usize(n.parent)] * n.local.ToMatrix() : n.local.ToMatrix();
        if (!n.mesh.IsValid())
            continue;
        FoliagePart p;
        p.mesh = n.mesh;
        p.materials = n.materials;
        p.local = Transform::FromMatrix(global[i]);
        out.PushBack(move(p));
    }
    return !out.Empty();
}

namespace {

f32 Hash01(i32 x, i32 z, u32 seed)
{
    u32 h = u32(x) * 73856093u ^ u32(z) * 19349663u ^ seed * 83492791u;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return f32(h & 0xFFFFFF) / 16777216.0f;
}

/// Smooth value noise 0..1 (patches of plants).
f32 ValueNoise(f32 x, f32 z, u32 seed)
{
    const i32 x0 = i32(std::floor(x)), z0 = i32(std::floor(z));
    const f32 fx = x - f32(x0), fz = z - f32(z0);
    const f32 ux = fx * fx * (3.0f - 2.0f * fx), uz = fz * fz * (3.0f - 2.0f * fz);
    const f32 a = Hash01(x0, z0, seed), b = Hash01(x0 + 1, z0, seed), c = Hash01(x0, z0 + 1, seed), d = Hash01(x0 + 1, z0 + 1, seed);
    return Lerp(Lerp(a, b, ux), Lerp(c, d, ux), uz);
}

} // namespace

void ScatterFoliage(const FoliageRule& rule, Vec2 min, Vec2 size, const Function<f32(f32, f32), 64>& heightAt, Array<FoliageInstance>& out)
{
    const f32 area = size.x * size.y;
    const u32 candidates = u32(Min(area * Max(rule.density, 0.0f), 200000.0f));
    // The generator is seeded by the rule and the rectangle: neighbors never repeat, a rebuild gives the same plants.
    Random rng(u64(rule.seed) * 0x9E3779B97F4A7C15ull ^ (u64(u32(i32(min.x))) << 32) ^ u64(u32(i32(min.y))));
    for (u32 i = 0; i < candidates; ++i) {
        const f32 x = min.x + rng.NextFloat() * size.x, z = min.y + rng.NextFloat() * size.y;
        if (rule.clumping > 0.0f) {
            const f32 patch = ValueNoise(x / rule.clumpSize, z / rule.clumpSize, rule.seed * 7u + 3u) * 0.7f +
                              ValueNoise(x / (rule.clumpSize * 0.37f), z / (rule.clumpSize * 0.37f), rule.seed * 13u + 5u) * 0.3f;
            // Keep a candidate with a probability that falls off outside the patches.
            const f32 keep = SmoothStep(0.35f + 0.25f * rule.clumping, 0.6f + 0.15f * rule.clumping, patch);
            if (rng.NextFloat() > Lerp(1.0f, keep, rule.clumping))
                continue;
        }
        const f32 y = heightAt(x, z);
        if (y < rule.minHeight || y > rule.maxHeight)
            continue;
        const f32 e = 1.5f;
        const f32 dx = (heightAt(x + e, z) - heightAt(x - e, z)) / (2.0f * e), dz = (heightAt(x, z + e) - heightAt(x, z - e)) / (2.0f * e);
        if (Sqrt(dx * dx + dz * dz) > rule.maxSlope)
            continue;
        FoliageInstance in;
        in.position = Vec3(x, y - 0.05f, z); // a little into the ground (no floating on slopes)
        in.yaw = rng.NextFloat() * TwoPi;
        in.scale = Lerp(rule.minScale, rule.maxScale, rng.NextFloat());
        out.PushBack(in);
    }
}

} // namespace zx
