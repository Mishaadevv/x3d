// Terrain: deterministic generation (seed, flat center, height range) and the physics height field built from it by
// PhysicsSystem (bodies rest on the surface the renderer draws).

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
