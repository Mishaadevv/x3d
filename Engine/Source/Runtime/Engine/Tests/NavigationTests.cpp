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
