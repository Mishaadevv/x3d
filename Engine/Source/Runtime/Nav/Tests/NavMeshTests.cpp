// Navigation mesh: walkable floors shrink by the agent radius, paths go around obstacles on the shortest route and stay
// off them, steps up to agentClimb connect while walls, cliffs, steep slopes and separate floors do not, and the binary
// form round-trips.

#include "Core/Testing/Test.h"
#include "Nav/NavMesh.h"

using namespace zx;
using namespace zx::nav;

namespace {

struct Soup
{
    Array<Vec3> v;
    Array<u32> i;

    /// Quad with the given outward normal (winding fixed up so Cross(b - a, c - a) points along it).
    void Quad(Vec3 a, Vec3 b, Vec3 c, Vec3 d, Vec3 n)
    {
        const u32 base = u32(v.Size());
        v.PushBack(a);
        v.PushBack(b);
        v.PushBack(c);
        v.PushBack(d);
        const bool flip = Dot(Cross(b - a, c - a), n) < 0.0f;
        const u32 t[6] = {0, 1, 2, 0, 2, 3};
        for (u32 k = 0; k < 6; k += 3) {
            i.PushBack(base + t[k]);
            i.PushBack(base + (flip ? t[k + 2] : t[k + 1]));
            i.PushBack(base + (flip ? t[k + 1] : t[k + 2]));
        }
    }
    void Floor(f32 x0, f32 z0, f32 x1, f32 z1, f32 y)
    {
        Quad(Vec3(x0, y, z0), Vec3(x1, y, z0), Vec3(x1, y, z1), Vec3(x0, y, z1), Vec3(0, 1, 0));
    }
    void Box(Vec3 mn, Vec3 mx)
    {
        Quad(Vec3(mn.x, mx.y, mn.z), Vec3(mx.x, mx.y, mn.z), Vec3(mx.x, mx.y, mx.z), Vec3(mn.x, mx.y, mx.z), Vec3(0, 1, 0));
        Quad(Vec3(mn.x, mn.y, mn.z), Vec3(mx.x, mn.y, mn.z), Vec3(mx.x, mn.y, mx.z), Vec3(mn.x, mn.y, mx.z), Vec3(0, -1, 0));
        Quad(Vec3(mn.x, mn.y, mn.z), Vec3(mn.x, mx.y, mn.z), Vec3(mn.x, mx.y, mx.z), Vec3(mn.x, mn.y, mx.z), Vec3(-1, 0, 0));
        Quad(Vec3(mx.x, mn.y, mn.z), Vec3(mx.x, mx.y, mn.z), Vec3(mx.x, mx.y, mx.z), Vec3(mx.x, mn.y, mx.z), Vec3(1, 0, 0));
        Quad(Vec3(mn.x, mn.y, mn.z), Vec3(mx.x, mn.y, mn.z), Vec3(mx.x, mx.y, mn.z), Vec3(mn.x, mx.y, mn.z), Vec3(0, 0, -1));
        Quad(Vec3(mn.x, mn.y, mx.z), Vec3(mx.x, mn.y, mx.z), Vec3(mx.x, mx.y, mx.z), Vec3(mn.x, mx.y, mx.z), Vec3(0, 0, 1));
    }
    bool Build(NavMesh& nav, AABB bounds, BuildStats* stats = nullptr) const
    {
        BuildSettings s;
        s.bounds = bounds;
        return nav.Build(v.View(), i.View(), s, stats);
    }
};

f32 PathLength(const Array<Vec3>& p)
{
    f32 len = 0.0f;
    for (usize k = 1; k < p.Size(); ++k)
        len += Distance(p[k - 1], p[k]);
    return len;
}

} // namespace

ZX_TEST(NavMesh, FloorShrinksByTheAgentRadius)
{
    Soup soup;
    soup.Floor(-10, -10, 10, 10, 0);
    NavMesh nav;
    BuildStats stats;
    ZX_REQUIRE(soup.Build(nav, AABB(Vec3(-12, -1, -12), Vec3(12, 4, 12)), &stats));
    ZX_EXPECT(stats.polys >= 1u);
    // Every polygon stays at least the agent radius (minus a cell of rounding) inside the floor, at floor height.
    for (const Poly& p : nav.Polys()) {
        ZX_EXPECT(p.corners[0].x >= -10.0f + 0.4f - 0.31f);
        ZX_EXPECT(p.corners[2].x <= 10.0f - 0.4f + 0.31f);
        ZX_EXPECT_NEAR(p.corners[0].y, 0.0f, 0.21f);
    }
    // The floor is one walkable area: almost all of its 400 m2 minus the eroded rim.
    f32 area = 0.0f;
    for (const Poly& p : nav.Polys())
        area += (p.corners[2].x - p.corners[0].x) * (p.corners[2].z - p.corners[0].z);
    ZX_EXPECT(area > 18.0f * 18.0f && area < 20.0f * 20.0f);
    Vec3 on;
    ZX_EXPECT(nav.FindNearestPoly(Vec3(3, 0.5f, -2), 1.0f, &on) >= 0);
    ZX_EXPECT_NEAR(on.y, 0.0f, 0.21f);
    ZX_EXPECT(nav.FindNearestPoly(Vec3(30, 0, 0), 2.0f) < 0);
    // A straight path across open floor is a single segment.
    Array<Vec3> path;
    ZX_REQUIRE(nav.FindPath(Vec3(-8, 0, -8), Vec3(8, 0, 7), path));
    ZX_EXPECT_EQ(path.Size(), 2u);
    ZX_EXPECT(nav.Raycast(Vec3(-8, 0, -8), Vec3(8, 0, 7)));
    f32 t = 0.0f;
    ZX_EXPECT(!nav.Raycast(Vec3(0, 0, 0), Vec3(20, 0, 0), &t));
    ZX_EXPECT(t > 0.45f && t < 0.5f); // leaves the mesh about 9.6 m along
}

ZX_TEST(NavMesh, PathGoesAroundAWall)
{
    Soup soup;
    soup.Floor(-10, -10, 10, 10, 0);
    soup.Box(Vec3(-6, 0, -0.5f), Vec3(6, 2, 0.5f)); // a wall across the middle, open at both ends
    NavMesh nav;
    ZX_REQUIRE(soup.Build(nav, AABB(Vec3(-12, -1, -12), Vec3(12, 4, 12))));
    Array<Vec3> path;
    const Vec3 a(1, 0, -5), b(1.5f, 0, 5);
    ZX_REQUIRE(nav.FindPath(a, b, path));
    ZX_EXPECT(path.Size() >= 3u); // turns around a wall end
    const f32 len = PathLength(path);
    // Shortest way round the +x end (6.4 m with the radius): from (1, -5) to the corner and on to (1.5, 5).
    const f32 corner = Distance(Vec3(1, 0, -5), Vec3(6.4f, 0, -0.9f)) + 1.8f + Distance(Vec3(6.4f, 0, 0.9f), Vec3(1.5f, 0, 5));
    ZX_EXPECT(len > Distance(a, b) + 1.0f);
    ZX_EXPECT(len < corner + 1.0f);
    // No point along the path cuts through the wall (inflated by most of the agent radius).
    for (usize k = 1; k < path.Size(); ++k)
        for (u32 t = 0; t <= 50; ++t) {
            const Vec3 p = Lerp(path[k - 1], path[k], f32(t) / 50.0f);
            const bool inside = p.x > -6.3f && p.x < 6.3f && p.z > -0.8f && p.z < 0.8f;
            ZX_EXPECT(!inside);
        }
}

ZX_TEST(NavMesh, StepsConnectButWallsAndCliffsDoNot)
{
    Soup soup;
    soup.Floor(-10, -10, 10, 10, 0);
    soup.Box(Vec3(2, 0, -10), Vec3(10, 0.3f, 10));    // a 30 cm step up to the east half
    soup.Box(Vec3(-10, 0, 4), Vec3(-2, 1.2f, 10));    // a 1.2 m block in the north-west corner (too high to climb)
    NavMesh nav;
    ZX_REQUIRE(soup.Build(nav, AABB(Vec3(-12, -1, -12), Vec3(12, 4, 12))));
    Array<Vec3> path;
    ZX_EXPECT(nav.FindPath(Vec3(-5, 0, -5), Vec3(6, 0.3f, 0), path));
    ZX_EXPECT_NEAR(path.Back().y, 0.3f, 0.21f);
    bool partial = false;
    ZX_EXPECT(!nav.FindPath(Vec3(-5, 0, -5), Vec3(-6, 1.2f, 7), path, &partial));
    ZX_EXPECT(partial);
    ZX_EXPECT(path.Back().y < 0.5f); // led to the foot of the block
}

ZX_TEST(NavMesh, SeparateFloorsAndSteepSlopes)
{
    Soup soup;
    soup.Floor(-10, -10, 10, 10, 0);
    soup.Floor(-4, -4, 4, 4, 4.0f); // a platform overhead (agents fit below and on it)
    // A 60 degree ramp from the ground up to the east edge: too steep.
    soup.Quad(Vec3(12, 0, -3), Vec3(16, 6.93f, -3), Vec3(16, 6.93f, 3), Vec3(12, 0, 3), Vec3(-0.866f, 0.5f, 0));
    NavMesh nav;
    ZX_REQUIRE(soup.Build(nav, AABB(Vec3(-12, -1, -12), Vec3(18, 8, 12))));
    // Both floors exist: under the platform and on it.
    Vec3 on;
    ZX_EXPECT(nav.FindNearestPoly(Vec3(0, 0.3f, 0), 0.5f, &on) >= 0);
    ZX_EXPECT_NEAR(on.y, 0.0f, 0.21f);
    ZX_EXPECT(nav.FindNearestPoly(Vec3(0, 4.3f, 0), 0.5f, &on) >= 0);
    ZX_EXPECT_NEAR(on.y, 4.0f, 0.21f);
    Array<Vec3> path;
    ZX_EXPECT(!nav.FindPath(Vec3(-8, 0, -8), Vec3(0, 4, 0), path));
    // The steep ramp has no polygons.
    for (const Poly& p : nav.Polys())
        ZX_EXPECT(p.corners[0].x < 12.0f);
}

ZX_TEST(NavMesh, SerializeRoundTrip)
{
    Soup soup;
    soup.Floor(-10, -10, 10, 10, 0);
    soup.Box(Vec3(-2, 0, -2), Vec3(2, 2, 2));
    NavMesh nav;
    ZX_REQUIRE(soup.Build(nav, AABB(Vec3(-12, -1, -12), Vec3(12, 4, 12))));
    Array<u8> bytes;
    nav.Serialize(bytes);
    NavMesh copy;
    ZX_REQUIRE(copy.Deserialize(bytes.View()));
    ZX_EXPECT_EQ(copy.Polys().Size(), nav.Polys().Size());
    ZX_EXPECT_EQ(copy.Links().Size(), nav.Links().Size());
    Array<Vec3> a, b;
    ZX_EXPECT(nav.FindPath(Vec3(-6, 0, -6), Vec3(6, 0, 6), a));
    ZX_EXPECT(copy.FindPath(Vec3(-6, 0, -6), Vec3(6, 0, 6), b));
    ZX_EXPECT_EQ(a.Size(), b.Size());
    bytes[0] ^= 0xFF;
    ZX_EXPECT(!copy.Deserialize(bytes.View()));
    ZX_EXPECT(copy.IsEmpty());
}
