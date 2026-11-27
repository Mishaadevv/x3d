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

namespace {

/// Two floors with a gap between them (x in [-17, -3] and [3, 17]); with `walkway` a strip over the gap at z in [6, 10]
/// joins them the long way round.
void TwoFloorsApart(Soup& soup, bool walkway)
{
    soup.Floor(-17, -10, -3, 10, 0);
    soup.Floor(3, -10, 17, 10, 0);
    if (walkway)
        soup.Floor(-3, 6, 3, 10, 0);
}

bool BuildWithLinks(NavMesh& nav, const Soup& soup, Span<const OffMeshLink> links)
{
    BuildSettings s;
    s.bounds = AABB(Vec3(-20, -1, -12), Vec3(20, 4, 12));
    return nav.Build(soup.v.View(), soup.i.View(), s, nullptr, {}, links);
}

u32 JumpCount(const Array<u8>& jumps)
{
    u32 n = 0;
    for (u8 j : jumps)
        n += j != 0 ? 1 : 0;
    return n;
}

f32 MeshArea(const NavMesh& nav)
{
    f32 area = 0.0f;
    for (const Poly& p : nav.Polys())
        area += (p.corners[2].x - p.corners[0].x) * (p.corners[2].z - p.corners[0].z);
    return area;
}

} // namespace

ZX_TEST(NavMesh, TilesMatchASingleBuild)
{
    Soup soup;
    soup.Floor(-20, -20, 20, 20, 0);
    soup.Box(Vec3(-12, 0, -0.5f), Vec3(12, 2, 0.5f)); // a wall across several tiles
    soup.Box(Vec3(5, 0, 6), Vec3(9, 0.3f, 14));       // a step on a tile corner
    const AABB bounds(Vec3(-22, -1, -22), Vec3(22, 4, 22));
    NavMesh single, tiled;
    ZX_REQUIRE(soup.Build(single, bounds));
    BuildSettings s;
    s.bounds = bounds;
    s.tileSize = 8.0f;
    BuildStats stats;
    ZX_REQUIRE(tiled.Build(soup.v.View(), soup.i.View(), s, &stats));
    ZX_EXPECT_EQ(tiled.TileCount(), 36u); // 44 m / 8 m -> 6 x 6
    ZX_EXPECT_EQ(stats.tilesBuilt, 36u);
    ZX_EXPECT_EQ(single.TileCount(), 1u);
    // The same walkable area (the erosion near tile edges sees the neighbors' cells).
    ZX_EXPECT_NEAR(MeshArea(tiled), MeshArea(single), MeshArea(single) * 0.002f);
    // Paths cross tile edges as they cross open floor. The polygon A* picks a corridor through smaller rectangles, so a
    // path may be a few percent longer (never shorter than the single build's).
    const Vec3 pairs[][2] = {{Vec3(1, 0, -8), Vec3(1.5f, 0, 8)}, {Vec3(-18, 0, -18), Vec3(18, 0, 18)}, {Vec3(-15, 0, 15), Vec3(7, 0.3f, 10)}};
    for (const auto& p : pairs) {
        Array<Vec3> a, b;
        ZX_REQUIRE(single.FindPath(p[0], p[1], a));
        ZX_REQUIRE(tiled.FindPath(p[0], p[1], b));
        ZX_EXPECT(PathLength(b) > PathLength(a) - 0.05f);
        ZX_EXPECT(PathLength(b) < PathLength(a) * 1.03f + 0.1f);
    }
    ZX_EXPECT(tiled.Raycast(Vec3(-18, 0, -18), Vec3(-2, 0, -14))); // straight over several tile edges
    // Tiles survive saving.
    Array<u8> bytes;
    tiled.Serialize(bytes);
    NavMesh copy;
    ZX_REQUIRE(copy.Deserialize(bytes.View()));
    ZX_EXPECT_EQ(copy.TileCount(), 36u);
    ZX_EXPECT_EQ(copy.Polys().Size(), tiled.Polys().Size());
    ZX_EXPECT_EQ(copy.Links().Size(), tiled.Links().Size());
}

ZX_TEST(NavMesh, TileSeamsLinkOnlyTheSameFloor)
{
    Soup soup;
    soup.Floor(-10, -10, 10, 10, 0);
    soup.Floor(-8, -3, 8, 3, 4.0f); // a bridge overhead across several tiles
    BuildSettings s;
    s.bounds = AABB(Vec3(-12, -1, -12), Vec3(12, 8, 12));
    s.tileSize = 6.0f;
    NavMesh nav;
    ZX_REQUIRE(nav.Build(soup.v.View(), soup.i.View(), s));
    Array<Vec3> path;
    ZX_REQUIRE(nav.FindPath(Vec3(-7, 4, 0), Vec3(7, 4, 0), path)); // along the bridge
    for (const Vec3& p : path)
        ZX_EXPECT_NEAR(p.y, 4.0f, 0.3f);
    ZX_REQUIRE(nav.FindPath(Vec3(-7, 0, 0), Vec3(7, 0, 0), path)); // below it
    for (const Vec3& p : path)
        ZX_EXPECT(p.y < 0.5f);
    ZX_EXPECT(!nav.FindPath(Vec3(-7, 0, 0), Vec3(7, 4, 0), path)); // no way up
}

ZX_TEST(NavMesh, ObstaclesCarveAndRebuildOnlyTheirTiles)
{
    // A wall with one door in the middle.
    Soup soup;
    soup.Floor(-16, -16, 16, 16, 0);
    soup.Box(Vec3(-16, 0, -0.5f), Vec3(-1.2f, 2, 0.5f));
    soup.Box(Vec3(1.2f, 0, -0.5f), Vec3(16, 2, 0.5f));
    BuildSettings s;
    s.bounds = AABB(Vec3(-16, -1, -16), Vec3(16, 4, 16));
    s.tileSize = 8.0f;
    NavMesh nav;
    ZX_REQUIRE(nav.Build(soup.v.View(), soup.i.View(), s));
    const Vec3 a(0, 0, -8), b(0, 0, 8);
    Array<Vec3> path;
    ZX_REQUIRE(nav.FindPath(a, b, path));
    ZX_EXPECT(PathLength(path) < 16.5f); // through the door
    // A crate shut in the door: only its tiles rebuild, and the way is cut.
    const AABB crate(Vec3(-1.5f, 0, -1), Vec3(1.5f, 1.5f, 1));
    const AABB need = nav.TileBuildBounds(crate);
    ZX_EXPECT(need.IsValid() && need.min.x < crate.min.x && need.max.x > crate.max.x);
    BuildStats stats;
    ZX_REQUIRE(nav.RebuildTiles(soup.v.View(), soup.i.View(), crate, Span<const AABB>(&crate, 1), &stats));
    ZX_EXPECT(stats.tilesBuilt >= 1u && stats.tilesBuilt <= 4u);
    bool partial = false;
    ZX_EXPECT(!nav.FindPath(a, b, path, &partial));
    ZX_EXPECT(partial);
    ZX_EXPECT(path.Back().z < 0.0f); // led up to the crate's side
    // The crate is gone: the door is open again.
    ZX_REQUIRE(nav.RebuildTiles(soup.v.View(), soup.i.View(), crate));
    ZX_REQUIRE(nav.FindPath(a, b, path));
    ZX_EXPECT(PathLength(path) < 16.5f);
    // Outside the bounds: nothing to rebuild.
    ZX_EXPECT(!nav.RebuildTiles(soup.v.View(), soup.i.View(), AABB(Vec3(100, 0, 100), Vec3(101, 1, 101))));
}

ZX_TEST(NavMesh, OffMeshLinkJoinsFloorsTheAgentCannotWalkBetween)
{
    Soup soup;
    TwoFloorsApart(soup, false);
    // Without the link the gap is a wall: the path stops at its side.
    NavMesh nav;
    ZX_REQUIRE(BuildWithLinks(nav, soup, {}));
    Array<Vec3> path;
    Array<u8> jumps;
    bool partial = false;
    ZX_EXPECT(!nav.FindPath(Vec3(-10, 0, 0), Vec3(10, 0, 0), path, &partial, &jumps));
    ZX_EXPECT(partial);

    // A jump across the gap (the endpoints are in the air here: the build snaps them onto the mesh).
    OffMeshLink link;
    link.start = Vec3(-3.5f, 0.0f, 0.0f);
    link.end = Vec3(3.5f, 0.0f, 0.0f);
    const OffMeshLink links[1] = {link};
    ZX_REQUIRE(BuildWithLinks(nav, soup, Span<const OffMeshLink>(links, 1)));
    ZX_EXPECT_EQ(nav.OffMeshLinks().Size(), 1u);
    if (nav.OffMeshLinks().Empty())
        return;
    const OffMeshLink& built = nav.OffMeshLinks()[0];
    ZX_EXPECT(built.startPoly >= 0 && built.endPoly >= 0);
    ZX_EXPECT(built.start.x < -3.1f && built.start.x > -3.8f); // on the mesh, inside its rim (the agent radius in)
    ZX_EXPECT(built.end.x > 3.1f && built.end.x < 3.8f);

    // Straight across: on foot to where it begins, over the gap, then on foot again.
    ZX_REQUIRE(nav.FindPath(Vec3(-10, 0, 0), Vec3(10, 0, 0), path, &partial, &jumps));
    ZX_EXPECT(!partial);
    ZX_EXPECT_EQ(path.Size(), jumps.Size());
    ZX_EXPECT_EQ(JumpCount(jumps), 1u);
    if (JumpCount(jumps) != 1)
        return;
    usize jump = 0;
    while (jump < jumps.Size() && !jumps[jump])
        ++jump;
    ZX_REQUIRE(jump > 0);
    ZX_REQUIRE(jump + 1 < path.Size());
    ZX_EXPECT_NEAR(path[jump - 1].x, built.start.x, 0.05f);
    ZX_EXPECT_NEAR(path[jump].x, built.end.x, 0.05f);
    ZX_EXPECT_NEAR(path[jump - 1].z, 0.0f, 0.05f);
    ZX_EXPECT(PathLength(path) < 21.5f); // 20 m plus the walk to the rim

    // The binary form keeps the link (its endpoints come back on the mesh).
    Array<u8> bytes;
    nav.Serialize(bytes);
    NavMesh copy;
    ZX_REQUIRE(copy.Deserialize(bytes.View()));
    ZX_EXPECT_EQ(copy.OffMeshLinks().Size(), 1u);
    ZX_EXPECT(!copy.OffMeshLinks().Empty() && copy.OffMeshLinks()[0].endPoly >= 0);
    Array<Vec3> path2;
    Array<u8> jumps2;
    ZX_REQUIRE(copy.FindPath(Vec3(-10, 0, 0), Vec3(10, 0, 0), path2, nullptr, &jumps2));
    ZX_EXPECT_EQ(JumpCount(jumps2), 1u);

    // Rebuilding the tiles keeps the links from the arguments (a rebuilt mesh must not lose them).
    ZX_REQUIRE(nav.RebuildTiles(soup.v.View(), soup.i.View(), AABB(Vec3(-2, -1, -2), Vec3(2, 1, 2)), {}, nullptr,
                               Span<const OffMeshLink>(links, 1)));
    ZX_REQUIRE(nav.FindPath(Vec3(-10, 0, 0), Vec3(10, 0, 0), path, nullptr, &jumps));
    ZX_EXPECT_EQ(JumpCount(jumps), 1u);
}

ZX_TEST(NavMesh, OffMeshLinkCostDecidesWhetherToJump)
{
    Soup soup;
    TwoFloorsApart(soup, true);
    OffMeshLink link;
    link.start = Vec3(-3.0f, 0.0f, 0.0f);
    link.end = Vec3(3.0f, 0.0f, 0.0f);
    const OffMeshLink links[1] = {link};
    NavMesh nav;
    ZX_REQUIRE(BuildWithLinks(nav, soup, Span<const OffMeshLink>(links, 1)));
    Array<Vec3> path;
    Array<u8> jumps;
    // Free: straight over the gap (16 m) rather than around the walkway (about 23 m).
    ZX_REQUIRE(nav.FindPath(Vec3(-8, 0, 0), Vec3(8, 0, 0), path, nullptr, &jumps));
    ZX_EXPECT_EQ(JumpCount(jumps), 1u);
    ZX_EXPECT(PathLength(path) < 17.5f);
    // Worth 100 m of walking: the agents take the walkway around instead.
    OffMeshLink slow = link;
    slow.cost = 100.0f;
    const OffMeshLink slowLinks[1] = {slow};
    NavMesh walking;
    ZX_REQUIRE(BuildWithLinks(walking, soup, Span<const OffMeshLink>(slowLinks, 1)));
    ZX_REQUIRE(walking.FindPath(Vec3(-8, 0, 0), Vec3(8, 0, 0), path, nullptr, &jumps));
    ZX_EXPECT_EQ(JumpCount(jumps), 0u);
    ZX_EXPECT(PathLength(path) > 20.0f);
    // Down only: from the far floor there is no way back up over the link.
    OffMeshLink oneWay = link;
    oneWay.bidirectional = false;
    const OffMeshLink oneWayLinks[1] = {oneWay};
    NavMesh down;
    ZX_REQUIRE(BuildWithLinks(down, soup, Span<const OffMeshLink>(oneWayLinks, 1)));
    bool partial = false;
    // The link runs -x -> +x: crossing back is a walk around, straight there is allowed.
    ZX_REQUIRE(down.FindPath(Vec3(-8, 0, 0), Vec3(8, 0, 0), path, &partial, &jumps));
    ZX_EXPECT(!partial);
    ZX_EXPECT_EQ(JumpCount(jumps), 1u);
    ZX_REQUIRE(down.FindPath(Vec3(8, 0, 0), Vec3(-8, 0, 0), path, &partial, &jumps));
    ZX_EXPECT_EQ(JumpCount(jumps), 0u);
    ZX_EXPECT_GT(PathLength(path), 20.0f);
}

