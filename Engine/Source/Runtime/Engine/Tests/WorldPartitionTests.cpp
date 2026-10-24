// World Partition: spatial roots are saved into cell files by position, stream in around a source and out when it
// moves away, and saving with a cell unloaded keeps what that cell's file holds.

#include "Core/FileSystem/FileSystem.h"
#include "Core/HAL/PlatformFile.h"
#include "Core/String/Path.h"
#include "Core/Testing/Test.h"
#include "Engine/ComponentRegistry.h"
#include "Engine/SceneSerialization.h"
#include "Engine/WorldPartition.h"
#include "Scene/Hierarchy.h"

using namespace zx;
using ecs::Entity;
using ecs::World;

namespace {

u32 CountSpatial(World& w)
{
    Array<Entity> roots;
    CollectRoots(w, roots);
    u32 n = 0;
    for (Entity r : roots)
        n += IsSpatialRoot(w, r) ? 1u : 0u;
    return n;
}

Entity AddSpatial(World& w, StringView name, Vec3 position)
{
    Transform t;
    t.position = position;
    const Entity e = scene::CreateNode(w, name, t);
    w.Add(e, SpatiallyLoaded{});
    EnsureEntityGuid(w, e);
    return e;
}

} // namespace

ZX_TEST(WorldPartition, CellsStreamAroundTheSource)
{
    RegisterEngineComponents();
    fs::Initialize(1);
    const String dir = Path::Join(Path::Normalize(platform::GetTempDirectory()), "zx_wp_test");
    platform::DeleteDirectoryTree(dir);
    platform::CreateDirectoryTree(dir);
    WorldPartitionSettings settings;
    settings.enabled = true;
    settings.cellSize = 100.0f;
    settings.loadRange = 150.0f;
    {
        World w;
        const Entity always = scene::CreateNode(w, "Sun"); // not spatial: stays in the scene
        (void)always;
        AddSpatial(w, "A", Vec3(10, 0, 10));     // cell (0, 0)
        const Entity b = AddSpatial(w, "B", Vec3(60, 0, 20)); // cell (0, 0)
        scene::CreateNode(w, "B child", Transform(), b);      // travels with its root
        AddSpatial(w, "Far", Vec3(950, 0, 20));  // cell (9, 0)
        scene::UpdateWorldTransforms(w);
        WorldStreamer s;
        s.Configure(dir, settings, SceneIOOptions{});
        ZX_REQUIRE(s.SaveCells(w));
        ZX_EXPECT(platform::FileExists(Path::Join(dir, WorldCellFileName(IVec2(0, 0)))));
        ZX_EXPECT(platform::FileExists(Path::Join(dir, WorldCellFileName(IVec2(9, 0)))));
        ZX_EXPECT_EQ(s.GetStats().cellsOnDisk, 2u);
    }
    {
        // A fresh world: only the cells near the source come in.
        World w;
        WorldStreamer s;
        s.Configure(dir, settings, SceneIOOptions{});
        const Vec3 near[] = {Vec3(0, 0, 0)};
        s.LoadAllInRange(w, near);
        ZX_EXPECT(s.IsCellLoaded(IVec2(0, 0)));
        ZX_EXPECT(!s.IsCellLoaded(IVec2(9, 0)));
        ZX_EXPECT_EQ(CountSpatial(w), 2u);
        // Walk to the far cell: it loads, the first one unloads.
        const Vec3 far[] = {Vec3(950, 0, 0)};
        s.LoadAllInRange(w, far);
        s.Update(w, far, true, 1e9);
        ZX_EXPECT(s.IsCellLoaded(IVec2(9, 0)));
        ZX_EXPECT(!s.IsCellLoaded(IVec2(0, 0)));
        ZX_EXPECT_EQ(CountSpatial(w), 1u);
        // A new entity in the unloaded cell (0, 0): saving keeps A and B there.
        AddSpatial(w, "New", Vec3(30, 0, 70));
        scene::UpdateWorldTransforms(w);
        ZX_REQUIRE(s.SaveCells(w));
        World check;
        WorldStreamer s2;
        s2.Configure(dir, settings, SceneIOOptions{});
        s2.LoadAllInRange(check, near);
        ZX_EXPECT_EQ(CountSpatial(check), 3u);
    }
    fs::Shutdown();
    platform::DeleteDirectoryTree(dir);
}
