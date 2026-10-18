// Terrain: deterministic generation (seed, flat center, height range) and the physics height field built from it by
// PhysicsSystem (bodies rest on the surface the renderer draws).

#include "Core/HAL/PlatformFile.h"
#include "Core/String/Base64.h"
#include "Core/Testing/Test.h"
#include "Engine/PhysicsSystem.h"
#include "Engine/Terrain.h"
#include "Scene/Hierarchy.h"

using namespace zx;

namespace {

/// Height of the generated surface at local (x, z) (same triangles as the mesh and the height field).
f32 SurfaceHeight(const Terrain& t, const Array<f32>& h, f32 x, f32 z)
{
    const u32 n = TerrainSamples(t);
    const f32 s = TerrainSpacing(t);
    const f32 gx = (x + t.size * 0.5f) / s, gz = (z + t.size * 0.5f) / s;
    const u32 ix = Min(u32(gx), n - 2), iz = Min(u32(gz), n - 2);
    const f32 fx = gx - f32(ix), fz = gz - f32(iz);
    const f32 h00 = h[iz * n + ix], h10 = h[iz * n + ix + 1], h01 = h[(iz + 1) * n + ix], h11 = h[(iz + 1) * n + ix + 1];
    // Diagonal p00 - p11.
    return fx > fz ? h00 + (h10 - h00) * fx + (h11 - h10) * fz : h00 + (h01 - h00) * fz + (h11 - h01) * fx;
}

} // namespace

ZX_TEST(Terrain, GenerationIsDeterministicAndShaped)
{
    Terrain t;
    t.resolution = 65;
    Array<f32> a, b;
    GenerateTerrainHeights(t, a);
    GenerateTerrainHeights(t, b);
    ZX_REQUIRE(a.Size() == usize(65 * 65));
    ZX_EXPECT(a == b);
    f32 lo = Infinity, hi = -Infinity;
    for (f32 v : a) {
        lo = Min(lo, v);
        hi = Max(hi, v);
    }
    ZX_EXPECT(lo >= 0.0f && hi <= t.height);
    ZX_EXPECT(hi - lo > t.height * 0.2f); // actually hilly
    // Flat center: samples within the radius share one height.
    const f32 center = a[32 * 65 + 32];
    const f32 s = TerrainSpacing(t);
    for (u32 z = 0; z < 65; ++z)
        for (u32 x = 0; x < 65; ++x) {
            const f32 px = -t.size * 0.5f + f32(x) * s, pz = -t.size * 0.5f + f32(z) * s;
            if (px * px + pz * pz < t.flatCenter * t.flatCenter)
                ZX_EXPECT_NEAR(a[z * 65 + x], center, 1e-3f);
        }
    // Another seed, another landscape; the hash follows every shape field.
    Terrain other = t;
    other.seed = 2;
    GenerateTerrainHeights(other, b);
    ZX_EXPECT(a != b);
    ZX_EXPECT(TerrainShapeHash(t) != TerrainShapeHash(other));
    other = t;
    other.material = Guid::New(); // material only: same shape
    ZX_EXPECT(TerrainShapeHash(t) == TerrainShapeHash(other));
    // Resolution is clamped.
    other.resolution = 100000;
    ZX_EXPECT_EQ(TerrainSamples(other), 1025u);
}

ZX_TEST(Terrain, BodiesRestOnTheSurface)
{
    ecs::World world;
    scene::InstallHierarchyHooks(world);
    Terrain t;
    t.size = 64.0f;
    t.resolution = 65;
    t.height = 8.0f;
    t.featureSize = 20.0f;
    t.flatCenter = 0.0f;
    const ecs::Entity terrain = scene::CreateNode(world, "Terrain", Transform(Vec3(0, -2, 0)));
    world.Add(terrain, t);
    Array<f32> heights;
    GenerateTerrainHeights(t, heights);
    Array<ecs::Entity> balls;
    const Vec2 spots[] = {Vec2(3.3f, -7.1f), Vec2(-12.4f, 5.2f), Vec2(20.5f, 18.0f), Vec2(-25.0f, -22.6f)};
    for (Vec2 p : spots) {
        const ecs::Entity b = scene::CreateNode(world, "Ball", Transform(Vec3(p.x, 15.0f, p.y)));
        world.Add(b, SphereCollider{});
        RigidBody rb;
        rb.angularDamping = 2.0f; // stay roughly where they land
        rb.linearDamping = 1.0f;
        world.Add(b, rb);
        balls.PushBack(b);
    }
    scene::UpdateWorldTransforms(world);
    PhysicsSystem p;
    p.Begin(world, nullptr);
    ZX_EXPECT(p.BodyOf(terrain).IsValid());
    for (u32 i = 0; i < 240; ++i) {
        scene::UpdateWorldTransforms(world);
        p.Tick(world, 1.0f / 60.0f);
    }
    scene::UpdateWorldTransforms(world);
    for (ecs::Entity b : balls) {
        const Vec3 pos = world.Get<Transform>(b)->position;
        const f32 ground = SurfaceHeight(t, heights, pos.x, pos.z) - 2.0f;
        ZX_EXPECT(pos.y > ground + 0.2f); // never below the surface (radius 0.5, slopes lift the center less)
        ZX_EXPECT(pos.y < ground + 1.0f); // and resting on it
    }
    // Raycasts hit the terrain where the mesh is.
    physics::RaycastHit hit;
    ZX_REQUIRE(p.World()->Raycast(Ray(Vec3(1.7f, 50, 2.9f), Vec3(0, -1, 0)), 100.0f, hit));
    ZX_EXPECT_NEAR(hit.point.y, SurfaceHeight(t, heights, 1.7f, 2.9f) - 2.0f, 1e-3f);
    p.End();
}

ZX_TEST(Terrain, SculptAndPaintRoundTrip)
{
    Terrain t;
    t.hills = false; // flat: the heights are the sculpt
    t.size = 64.0f;
    t.resolution = 33;
    const u32 n = TerrainSamples(t);
    Array<f32> offsets;
    offsets.Resize(usize(n) * n, 0.0f);
    offsets[usize(16) * n + 16] = 7.25f;
    offsets[usize(3) * n + 30] = -2.5f;
    EncodeTerrainSculpt(t, offsets);
    ZX_EXPECT(!t.sculpt.Empty());
    Array<f32> heights;
    ComputeTerrainHeights(t, heights);
    ZX_EXPECT_NEAR(heights[usize(16) * n + 16], 7.25f, 0.011f); // 2 cm steps
    ZX_EXPECT_NEAR(heights[usize(3) * n + 30], -2.5f, 0.011f);
    ZX_EXPECT_NEAR(heights[0], 0.0f, 1e-6f);
    // Painting.
    Array<u8> rgb;
    rgb.Resize(usize(n) * n * 4, 0);
    rgb[(usize(5) * n + 5) * 4 + 1] = 200; // dirt
    rgb[(usize(6) * n + 5) * 4 + 3] = 150; // grass
    EncodeTerrainPaint(t, rgb);
    Array<u8> back;
    DecodeTerrainPaint(t, n, back);
    ZX_REQUIRE(back.Size() == rgb.Size());
    ZX_EXPECT_EQ(u32(back[(usize(5) * n + 5) * 4 + 1]), 200u);
    ZX_EXPECT_EQ(u32(back[(usize(6) * n + 5) * 4 + 3]), 150u);
    // Scenes from 0.13.14 stored RGB: read as RGBA with no grass.
    {
        Array<u8> legacy;
        legacy.Resize(usize(n) * n * 3, 0);
        legacy[(usize(5) * n + 5) * 3 + 2] = 99; // sand
        Terrain old = t;
        old.paint = Base64Encode(ByteSpan(legacy.Data(), legacy.Size()));
        Array<u8> upgraded;
        DecodeTerrainPaint(old, n, upgraded);
        ZX_REQUIRE(upgraded.Size() == usize(n) * n * 4);
        ZX_EXPECT_EQ(u32(upgraded[(usize(5) * n + 5) * 4 + 2]), 99u);
        ZX_EXPECT_EQ(u32(upgraded[(usize(5) * n + 5) * 4 + 3]), 0u);
    }
    // Nothing painted / sculpted: empty strings.
    Array<u8> none;
    none.Resize(rgb.Size(), 0);
    EncodeTerrainPaint(t, none);
    ZX_EXPECT(t.paint.Empty());
    // Another resolution: the sculpt is resampled (a 1 m bump at the center stays at the center).
    offsets.Clear();
    offsets.Resize(usize(n) * n, 0.0f);
    offsets[usize(16) * n + 16] = 4.0f;
    EncodeTerrainSculpt(t, offsets);
    t.resolution = 65;
    Array<f32> fine;
    DecodeTerrainSculpt(t, TerrainSamples(t), fine);
    ZX_EXPECT_NEAR(fine[usize(32) * 65 + 32], 4.0f, 0.02f);
    ZX_EXPECT_NEAR(fine[usize(33) * 65 + 32], 2.0f, 0.05f); // halfway to the neighbor
}

ZX_TEST(Terrain, EditingThroughTheSystem)
{
    ecs::World world;
    scene::InstallHierarchyHooks(world);
    asset::AssetManagerSettings settings;
    settings.ddcDirectory = platform::GetTempDirectory() + "/zx_terrain_test";
    settings.writeMetaFiles = false;
    asset::AssetManager assets(settings);
    const ecs::Entity e = scene::CreateNode(world, "Terrain", Transform(Vec3(10, 2, 0)));
    Terrain t;
    t.hills = false;
    t.size = 40.0f;
    t.resolution = 41; // 1 m cells
    world.Add(e, t);
    scene::UpdateWorldTransforms(world);
    TerrainSystem terrains;
    terrains.Update(world, assets, nullptr);
    f32 h = -1.0f;
    ZX_REQUIRE(terrains.HeightAt(world, e, Vec2(10, 0), h));
    ZX_EXPECT_NEAR(h, 2.0f, 1e-4f);
    ZX_EXPECT(!terrains.HeightAt(world, e, Vec2(40, 0), h)); // outside

    // A tool raises a bump, commits it into the component.
    TerrainSystem::Data* d = terrains.BeginEdit(world, e);
    ZX_REQUIRE(d != nullptr);
    d->sculpt[usize(20) * d->n + 20] = 5.0f;
    d->paint[(usize(20) * d->n + 20) * 4] = 255;
    terrains.MarkDirty(*d);
    terrains.Commit(world, e);
    const Terrain after = *world.Get<const Terrain>(e); // a copy: the component is replaced below
    ZX_EXPECT(!after.sculpt.Empty() && !after.paint.Empty() && after.dataId != 0);
    ZX_REQUIRE(terrains.HeightAt(world, e, Vec2(10, 0), h));
    ZX_EXPECT_NEAR(h, 7.0f, 0.02f);
    // A ray from above hits the bump; from the side it misses below the flat part.
    ecs::Entity hitTerrain;
    Vec3 hit;
    ZX_REQUIRE(terrains.Raycast(world, Ray(Vec3(10, 50, 0), Vec3(0, -1, 0)), 100.0f, hitTerrain, hit));
    ZX_EXPECT(hitTerrain == e);
    ZX_EXPECT_NEAR(hit.y, 7.0f, 0.02f);
    ZX_REQUIRE(terrains.Raycast(world, Ray(Vec3(0, 10, 0), Normalize(Vec3(1, -1, 0))), 100.0f, hitTerrain, hit));
    ZX_EXPECT_NEAR(hit.y, 2.0f, 0.05f); // lands on the flat ground at x = 8

    // Undo restores the old component data: the system re-reads it (no sculpt -> flat again).
    Terrain restored = after;
    restored.sculpt.Clear();
    restored.paint.Clear();
    restored.dataId = 0;
    *world.Get<Terrain>(e) = restored;
    terrains.Update(world, assets, nullptr);
    ZX_REQUIRE(terrains.HeightAt(world, e, Vec2(10, 0), h));
    ZX_EXPECT_NEAR(h, 2.0f, 1e-4f);

    // The physics height field includes the sculpt.
    *world.Get<Terrain>(e) = after;
    Array<f32> heights;
    ComputeTerrainHeights(after, heights);
    ZX_EXPECT_NEAR(heights[usize(20) * 41 + 20], 5.0f, 0.02f);
}
