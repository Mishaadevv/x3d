// Navigation in a scene: a Nav Mesh Bounds over a terrain builds a mesh from its height field, a Nav Mesh Agent walks
// to its destination on it (feet on the ground, facing where it goes) and reports arrival; unreachable destinations
// report a partial path; the baked file round-trips.

#include "Core/HAL/PlatformFile.h"
#include "Core/String/Path.h"
#include "Core/Testing/Test.h"
#include "Engine/ComponentRegistry.h"
#include "Engine/Navigation.h"
#include "Engine/Terrain.h"
#include "Scene/Hierarchy.h"

using namespace zx;

namespace {

void MakeLevel(ecs::World& w)
{
    RegisterEngineComponents();
    const ecs::Entity ground = scene::CreateNode(w, "Ground", Transform(Vec3(0, 2, 0)));
    Terrain t;
    t.size = 40.0f;
    t.resolution = 41;
    t.hills = false;
    w.Add(ground, move(t));
    const ecs::Entity bounds = scene::CreateNode(w, "Nav", Transform(Vec3(0, 2, 0)));
    NavMeshBounds b;
    b.size = Vec3(30, 10, 30);
    w.Add(bounds, b);
    scene::UpdateWorldTransforms(w);
}

} // namespace

ZX_TEST(Navigation, AgentWalksToItsDestination)
{
    ecs::World w;
    MakeLevel(w);
    NavigationSystem nav;
    NavBuildReport report;
    ZX_REQUIRE(nav.Build(w, nullptr, &report));
    ZX_EXPECT_EQ(report.volumes, 1u);
    ZX_EXPECT(report.triangles > 100u);
    Vec3 on;
    ZX_REQUIRE(nav.NearestPoint(Vec3(3, 3, 3), 2.0f, on));
    ZX_EXPECT_NEAR(on.y, 2.0f, 0.25f);

    const ecs::Entity agent = scene::CreateNode(w, "Agent", Transform(Vec3(-10, 2, -10)));
    w.Add(agent, NavMeshAgent{});
    scene::UpdateWorldTransforms(w);
    ZX_REQUIRE(nav.SetDestination(w, agent, Vec3(10, 2, 8)));
    bool movedEast = false;
    for (u32 i = 0; i < 60 * 20; ++i) {
        nav.Tick(w, 1.0f / 60.0f);
        scene::UpdateWorldTransforms(w);
        const NavMeshAgent& a = *w.Get<const NavMeshAgent>(agent);
        if (i == 60) {
            // Walking north-east at full speed, facing that way.
            ZX_EXPECT_NEAR(Length(a.velocity), 3.5f, 0.2f);
            const Vec3 forward = w.Get<const Transform>(agent)->rotation * Vec3(0, 0, 1);
            ZX_EXPECT(Dot(forward, Normalize(a.velocity)) > 0.95f);
            movedEast = w.Get<const Transform>(agent)->position.x > -9.0f;
        }
        if (a.status == NavAgentStatus::Arrived)
            break;
    }
    ZX_EXPECT(movedEast);
    const NavMeshAgent& a = *w.Get<const NavMeshAgent>(agent);
    ZX_EXPECT(a.status == NavAgentStatus::Arrived);
    const Vec3 p = w.Get<const Transform>(agent)->position;
    ZX_EXPECT(Distance(Vec2(p.x, p.z), Vec2(10, 8)) < 0.3f);
    ZX_EXPECT_NEAR(p.y, 2.0f, 0.25f);
    ZX_EXPECT_EQ(NavigationSystem::RemainingDistance(w, agent), 0.0f);
}

ZX_TEST(Navigation, UnreachableAndBakedFile)
{
    ecs::World w;
    MakeLevel(w);
    NavigationSystem nav;
    ZX_REQUIRE(nav.Build(w, nullptr));
    // Outside the volume: the path leads to its edge.
    Array<Vec3> path;
    bool partial = false;
    ZX_EXPECT(!nav.FindPath(Vec3(0, 2, 0), Vec3(40, 2, 0), path, &partial));
    ZX_EXPECT(partial);
    ZX_REQUIRE(!path.Empty());
    ZX_EXPECT(path.Back().x > 13.0f && path.Back().x < 15.0f);

    const String file = Path::Join(Path::Normalize(platform::GetTempDirectory()), "zx_nav_test.znav");
    ZX_REQUIRE(nav.Save(file.View()));
    NavigationSystem loaded;
    ZX_REQUIRE(loaded.Load(file.View()));
    ZX_EXPECT_EQ(loaded.Volumes().Size(), 1u);
    ZX_EXPECT_EQ(loaded.Volumes()[0].mesh.Polys().Size(), nav.Volumes()[0].mesh.Polys().Size());
    platform::DeleteFileAt(file.View());
}

ZX_TEST(Navigation, MovingObstaclesRebuildTheirTiles)
{
    ecs::World w;
    MakeLevel(w);
    w.Get<NavMeshBounds>(w.Query<const NavMeshBounds>().First())->tileSize = 8.0f;
    // A wall of obstacles across the level (past the volume's sides) with a 2 m door at x = 0.
    auto obstacle = [&](StringView name, Vec3 at, Vec3 size) {
        const ecs::Entity e = scene::CreateNode(w, name, Transform(at));
        NavMeshObstacle o;
        o.size = size;
        w.Add(e, o);
        return e;
    };
    obstacle("Wall West", Vec3(-14, 3, 0), Vec3(26, 2, 1));
    obstacle("Wall East", Vec3(14, 3, 0), Vec3(26, 2, 1));
    scene::UpdateWorldTransforms(w);
    NavigationSystem nav;
    NavBuildReport report;
    ZX_REQUIRE(nav.Build(w, nullptr, &report));
    ZX_EXPECT(report.tiles > 1u);
    const Vec3 a(0, 2, -10), b(0, 2, 10);
    Array<Vec3> path;
    ZX_REQUIRE(nav.FindPath(a, b, path));
    ZX_EXPECT(path.Size() == 2u); // straight through the door
    // An agent heads through it; a crate drops into the door.
    const ecs::Entity agent = scene::CreateNode(w, "Agent", Transform(a));
    w.Add(agent, NavMeshAgent{});
    scene::UpdateWorldTransforms(w);
    ZX_REQUIRE(nav.SetDestination(w, agent, b));
    nav.Tick(w, 1.0f / 60.0f);
    const ecs::Entity crate = obstacle("Crate", Vec3(0, 3, 0), Vec3(3, 2, 1.2f));
    scene::UpdateWorldTransforms(w);
    for (u32 i = 0; i < 30; ++i) { // 0.5 s: the rebuild waits a fraction of a second at most
        nav.Tick(w, 1.0f / 60.0f);
        scene::UpdateWorldTransforms(w);
    }
    ZX_EXPECT_EQ(nav.RebuildCount(), 1u);
    bool partial = false;
    ZX_EXPECT(!nav.FindPath(a, b, path, &partial));
    ZX_EXPECT(partial);
    for (u32 i = 0; i < 60 * 6; ++i) {
        nav.Tick(w, 1.0f / 60.0f);
        scene::UpdateWorldTransforms(w);
    }
    ZX_EXPECT(w.Get<const Transform>(agent)->position.z < 0.0f); // stopped at the crate
    ZX_EXPECT(w.Get<const NavMeshAgent>(agent)->status == NavAgentStatus::Partial);
    // The crate is pushed into the wall: the door opens, the agent walks on when sent again.
    w.Get<Transform>(crate)->position = Vec3(8, 3, 0);
    scene::UpdateWorldTransforms(w);
    for (u32 i = 0; i < 30; ++i)
        nav.Tick(w, 1.0f / 60.0f);
    ZX_EXPECT_EQ(nav.RebuildCount(), 2u);
    ZX_REQUIRE(nav.FindPath(a, b, path));
    ZX_REQUIRE(nav.SetDestination(w, agent, b));
    for (u32 i = 0; i < 60 * 8; ++i) {
        nav.Tick(w, 1.0f / 60.0f);
        scene::UpdateWorldTransforms(w);
    }
    ZX_EXPECT(w.Get<const NavMeshAgent>(agent)->status == NavAgentStatus::Arrived);
    // Disabling an obstacle counts as removing it; standing still costs nothing.
    w.Get<NavMeshObstacle>(crate)->enabled = false;
    for (u32 i = 0; i < 60; ++i)
        nav.Tick(w, 1.0f / 60.0f);
    ZX_EXPECT_EQ(nav.RebuildCount(), 3u);
    for (u32 i = 0; i < 60; ++i)
        nav.Tick(w, 1.0f / 60.0f);
    ZX_EXPECT_EQ(nav.RebuildCount(), 3u);
}

ZX_TEST(Navigation, AgentCrossesAnOffMeshLink)
{
    ecs::World w;
    RegisterEngineComponents();
    // Two terraces 3 m apart, the gap in between: nothing walks across it.
    for (const f32 x : {-11.0f, 13.5f}) {
        const ecs::Entity e = scene::CreateNode(w, "Terrace", Transform(Vec3(x, 2, 0)));
        Terrain t;
        t.size = 20.0f;
        t.resolution = 21;
        t.hills = false;
        w.Add(e, move(t));
    }
    const ecs::Entity bounds = scene::CreateNode(w, "Nav", Transform(Vec3(0, 2, 0)));
    NavMeshBounds b;
    b.size = Vec3(60, 10, 30);
    w.Add(bounds, b);
    scene::UpdateWorldTransforms(w);
    NavigationSystem nav;
    NavBuildReport report;
    ZX_REQUIRE(nav.Build(w, nullptr, &report));
    ZX_EXPECT_EQ(report.volumes, 1u);
    const Vec3 a(-8, 2, 0), c(10, 2, 0);
    Array<Vec3> path;
    bool partial = false;
    ZX_EXPECT(!nav.FindPath(a, c, path, &partial));
    ZX_EXPECT(partial);

    // A jump over the gap: agents walk to its edge, are moved across and walk on.
    const ecs::Entity linkEntity = scene::CreateNode(w, "Jump", Transform(Vec3(-1.0f, 2, 0)));
    NavMeshLink link;
    link.start = Vec3(0, 0, 0);
    link.end = Vec3(5.0f, 0, 0);
    w.Add(linkEntity, link);
    scene::UpdateWorldTransforms(w);
    ZX_REQUIRE(nav.Build(w, nullptr, &report));
    ZX_EXPECT_EQ(report.links, 1u);

    const ecs::Entity agent = scene::CreateNode(w, "Agent", Transform(a));
    w.Add(agent, NavMeshAgent{});
    scene::UpdateWorldTransforms(w);
    ZX_REQUIRE(nav.SetDestination(w, agent, c));
    bool crossed = false, overGap = false;
    f32 lowestOverGap = 100.0f;
    for (u32 i = 0; i < 60 * 12; ++i) {
        nav.Tick(w, 1.0f / 60.0f);
        scene::UpdateWorldTransforms(w);
        const NavMeshAgent& ag = *w.Get<const NavMeshAgent>(agent);
        const Vec3 p = w.Get<const Transform>(agent)->position;
        if (ag.linkProgress >= 0.0f)
            crossed = true;
        // The middle of the gap has no floor: only the crossing can put the agent there, and it never sinks.
        if (p.x > 0.6f && p.x < 3.4f) {
            overGap = true;
            lowestOverGap = Min(lowestOverGap, p.y);
        }
        if (ag.status == NavAgentStatus::Arrived)
            break;
    }
    ZX_EXPECT(crossed);
    ZX_EXPECT(overGap);
    ZX_EXPECT(lowestOverGap > 1.9f); // over the gap, not falling into it
    const NavMeshAgent& done = *w.Get<const NavMeshAgent>(agent);
    ZX_EXPECT(done.status == NavAgentStatus::Arrived);
    const Vec3 p = w.Get<const Transform>(agent)->position;
    ZX_EXPECT(Distance(Vec2(p.x, p.z), Vec2(c.x, c.z)) < 0.4f);
    ZX_EXPECT_NEAR(p.y, 2.0f, 0.3f);
}

