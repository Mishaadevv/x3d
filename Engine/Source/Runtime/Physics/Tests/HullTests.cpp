// Convex hulls: quickhull (faces merged into polygons, vertex limit), mass properties, GJK, and hulls in the world:
// resting on the ground and on each other, spheres / capsules on hulls, mesh contacts, raycasts.

#include "Core/Math/Random.h"
#include "Core/Testing/Test.h"
#include "Physics/PhysicsWorld.h"
#include "../Private/ConvexHull.h"

using namespace zx;
using namespace zx::physics;

namespace {

constexpr f32 Dt = 1.0f / 60.0f;

void Run(PhysicsWorld& w, f32 seconds)
{
    for (u32 i = 0; i < u32(seconds / Dt); ++i)
        w.Step(Dt);
}

Array<Vec3> CubePoints(f32 h, bool withInterior)
{
    Array<Vec3> p;
    for (u32 i = 0; i < 8; ++i)
        p.PushBack(Vec3((i & 1) ? h : -h, (i & 2) ? h : -h, (i & 4) ? h : -h));
    if (withInterior) {
        Random rng(3);
        for (u32 i = 0; i < 50; ++i)
            p.PushBack(Vec3(rng.Range(-h, h), rng.Range(-h, h), rng.Range(-h, h)) * 0.9f);
    }
    return p;
}

BodyId Ground(PhysicsWorld& w)
{
    BodyDesc d;
    d.shapes.PushBack(ShapeDesc::Box(Vec3(50.0f, 0.5f, 50.0f)));
    d.position = Vec3(0.0f, -0.5f, 0.0f);
    return w.CreateBody(d);
}

BodyId DynamicHull(PhysicsWorld& w, HullId hull, Vec3 pos, Quat rot = {})
{
    BodyDesc d;
    d.motion = MotionType::Dynamic;
    d.position = pos;
    d.rotation = rot;
    d.shapes.PushBack(ShapeDesc::Hull(hull));
    return w.CreateBody(d);
}

} // namespace

ZX_TEST(Hull, CubeFacesVolumeInertia)
{
    ConvexHull h;
    const Array<Vec3> pts = CubePoints(1.0f, true);
    ZX_REQUIRE(h.Build(pts.View()));
    ZX_EXPECT_EQ(h.vertices.Size(), usize(8)); // interior points dropped
    ZX_EXPECT_EQ(h.faces.Size(), usize(6));    // coplanar triangles merged into quads
    for (const ConvexHull::Face& f : h.faces)
        ZX_EXPECT_EQ(u32(f.count), 4u);
    ZX_EXPECT_EQ(h.edges.Size() / 2, usize(12));
    ZX_EXPECT_EQ(h.directions.Size(), usize(3));
    ZX_EXPECT_NEAR(h.volume, 8.0f, 1e-3f);
    ZX_EXPECT(Length(h.centroid) < 1e-4f);
    // Box inertia (unit density, side 2): m (b^2 + c^2) / 12 = 8 * 8 / 12.
    ZX_EXPECT_NEAR(h.inertia.columns[0].x, 8.0f * 8.0f / 12.0f, 1e-2f);
    ZX_EXPECT_NEAR(h.inertia.columns[1].y, 8.0f * 8.0f / 12.0f, 1e-2f);
    ZX_EXPECT(Abs(h.inertia.columns[0].y) < 1e-3f);
}

ZX_TEST(Hull, SphereCloudIsReducedAndClose)
{
    Array<Vec3> pts;
    Random rng(9);
    for (u32 i = 0; i < 2000; ++i)
        pts.PushBack(rng.OnUnitSphere());
    ConvexHull h;
    ZX_REQUIRE(h.Build(pts.View()));
    ZX_EXPECT(h.vertices.Size() <= usize(ConvexHull::MaxVertices));
    const f32 sphere = 4.0f / 3.0f * Pi;
    ZX_EXPECT(h.volume > sphere * 0.6f && h.volume <= sphere * 1.001f);
    // Flat input is rejected.
    Array<Vec3> flat = {Vec3(0, 0, 0), Vec3(1, 0, 0), Vec3(0, 0, 1), Vec3(1, 0, 1)};
    ConvexHull f;
    ZX_EXPECT(!f.Build(flat.View()));
}

ZX_TEST(Hull, GjkDistance)
{
    const Array<Vec3> cube = CubePoints(1.0f, false);
    const Vec3 p(3.0f, 0.5f, 0.0f);
    const GjkResult g = Gjk(cube.View(), Span<const Vec3>(&p, 1));
    ZX_EXPECT_NEAR(g.distance, 2.0f, 1e-3f);
    ZX_EXPECT(Distance(g.pointA, Vec3(1.0f, 0.5f, 0.0f)) < 1e-3f);
    // Corner region.
    const Vec3 q(2.0f, 2.0f, 2.0f);
    ZX_EXPECT_NEAR(Gjk(cube.View(), Span<const Vec3>(&q, 1)).distance, Sqrt(3.0f), 1e-3f);
    // Segment above the top face (lying horizontally).
    const Vec3 seg[2] = {Vec3(-0.5f, 1.5f, 0.2f), Vec3(0.6f, 1.5f, 0.2f)};
    const GjkResult gs = Gjk(cube.View(), Span<const Vec3>(seg, 2));
    ZX_EXPECT_NEAR(gs.distance, 0.5f, 1e-3f);
    ZX_EXPECT_NEAR(gs.pointB.y, 1.5f, 1e-3f);
    // Overlap.
    const Vec3 inside(0.2f, 0.1f, 0.0f);
    ZX_EXPECT_NEAR(Gjk(cube.View(), Span<const Vec3>(&inside, 1)).distance, 0.0f, 1e-4f);
}

ZX_TEST(Hull, RestsAndStacks)
{
    PhysicsWorld w;
    Ground(w);
    // A hull cube behaves like a box: rests flat at its half height.
    const HullId cube = w.CreateConvexHull(CubePoints(0.5f, false).View());
    ZX_REQUIRE(cube != InvalidHull);
    const BodyId a = DynamicHull(w, cube, Vec3(0, 1.0f, 0), Quat::FromAxisAngle(Vec3(0, 1, 0), 0.3f));
    const BodyId b = DynamicHull(w, cube, Vec3(0.1f, 2.2f, 0));
    // A rock-like hull (random cloud) settles on the ground.
    Array<Vec3> rock;
    Random rng(5);
    for (u32 i = 0; i < 60; ++i)
        rock.PushBack(rng.OnUnitSphere() * Vec3(0.6f, 0.4f, 0.5f));
    const HullId rockHull = w.CreateConvexHull(rock.View());
    ZX_REQUIRE(rockHull != InvalidHull);
    const BodyId r = DynamicHull(w, rockHull, Vec3(3, 2, 0), Quat::FromAxisAngle(Normalize(Vec3(1, 1, 0)), 0.7f));
    Run(w, 4.0f);
    ZX_EXPECT_NEAR(w.GetPosition(a).y, 0.5f, 0.03f);
    ZX_EXPECT_NEAR(w.GetPosition(b).y, 1.5f, 0.05f);
    ZX_EXPECT(Length(w.GetLinearVelocity(r)) < 0.05f);
    ZX_EXPECT(w.GetBounds(r).min.y > -0.05f); // on the ground, not through it
    ZX_EXPECT(w.GetBounds(r).min.y < 0.05f);
}

ZX_TEST(Hull, SpheresCapsulesAndRays)
{
    PhysicsWorld w;
    // A static hull "table" (cube 2 x 2 x 2 at y = 1, top at 2).
    BodyDesc table;
    table.position = Vec3(0, 1, 0);
    table.shapes.PushBack(ShapeDesc::Hull(w.CreateConvexHull(CubePoints(1.0f, false).View())));
    w.CreateBody(table);
    BodyDesc ball;
    ball.motion = MotionType::Dynamic;
    ball.position = Vec3(0.2f, 3.0f, 0.1f);
    ball.shapes.PushBack(ShapeDesc::Sphere(0.25f));
    const BodyId s = w.CreateBody(ball);
    BodyDesc pill;
    pill.motion = MotionType::Dynamic;
    pill.position = Vec3(-0.2f, 3.5f, -0.6f); // clear of the ball
    pill.rotation = Quat::FromAxisAngle(Vec3(0, 0, 1), Pi * 0.5f); // lying down
    pill.shapes.PushBack(ShapeDesc::Capsule(0.2f, 0.4f));
    const BodyId c = w.CreateBody(pill);
    Run(w, 3.0f);
    ZX_EXPECT_NEAR(w.GetPosition(s).y, 2.25f, 0.03f);
    ZX_EXPECT_NEAR(w.GetPosition(c).y, 2.2f, 0.03f);
    // Lying flat (both ends supported): its axis stays horizontal.
    ZX_EXPECT(Abs((w.GetRotation(c) * Vec3(0, 1, 0)).y) < 0.1f);
    RaycastHit hit;
    ZX_REQUIRE(w.Raycast(Ray(Vec3(0.5f, 10, -0.5f), Vec3(0, -1, 0)), 100.0f, hit));
    ZX_EXPECT_NEAR(hit.distance, 8.0f, 1e-3f);
    ZX_EXPECT(Dot(hit.normal, Vec3(0, 1, 0)) > 0.999f);
}

ZX_TEST(Hull, OnTriangleMesh)
{
    PhysicsWorld w;
    // Ground as a static triangle mesh (two triangles).
    const Vec3 v[4] = {Vec3(-10, 0, -10), Vec3(10, 0, -10), Vec3(10, 0, 10), Vec3(-10, 0, 10)};
    const u32 idx[6] = {0, 2, 1, 0, 3, 2};
    BodyDesc ground;
    ground.shapes.PushBack(ShapeDesc::Mesh(w.CreateTriangleMesh(Span<const Vec3>(v, 4), Span<const u32>(idx, 6))));
    w.CreateBody(ground);
    const BodyId h = DynamicHull(w, w.CreateConvexHull(CubePoints(0.5f, false).View()), Vec3(0, 2, 0));
    Run(w, 3.0f);
    ZX_EXPECT_NEAR(w.GetPosition(h).y, 0.5f, 0.03f);
}

