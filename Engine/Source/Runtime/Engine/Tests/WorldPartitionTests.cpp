// World Partition: spatial roots are saved into cell files by position, stream in around a source and out when it
// moves away, and saving with a cell unloaded keeps what that cell's file holds. HLODs keep trees as impostors and leave
// out the cells whose real trees are loaded.

#include "Asset/AssetManager.h"
#include "Core/CoreInit.h"
#include "Core/FileSystem/FileSystem.h"
#include "Core/Threading/Thread.h"
#include "Engine/Foliage.h"
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

ZX_TEST(WorldPartition, HlodKeepsTreesAsImpostors)
{
    RegisterEngineComponents();
    fs::Initialize(1);
    const String dir = Path::Join(Path::Normalize(platform::GetTempDirectory()), "zx_wp_hlod_test");
    const String ddc = Path::Join(Path::Normalize(platform::GetTempDirectory()), "zx_wp_hlod_ddc");
    platform::DeleteDirectoryTree(dir);
    platform::CreateDirectoryTree(dir);
    {
        asset::AssetManagerSettings as;
        as.ddcDirectory = ddc;
        as.writeMetaFiles = false;
        asset::AssetManager assets(as);
        const SceneIOOptions io{&assets, FindEngineRootDirectory() + "/Engine/Content", nullptr};
        WorldPartitionSettings settings;
        settings.enabled = true;
        settings.cellSize = 100.0f;
        settings.loadRange = 150.0f;
        // Pines in cell (0, 0) (3 plants) and cell (3, 0) (2 plants): both in HLOD group (0, 0) of 4 x 4 cells.
        auto pines = [](World& w, Vec3 at, u32 count) {
            const Entity e = AddSpatial(w, "Pines", at);
            Foliage f;
            f.model = "Packs/StylizedNature/Models/Pine_3.gltf";
            FoliagePart part;
            part.mesh = Guid::New();
            f.parts.PushBack(part);
            Array<FoliageInstance> in;
            for (u32 i = 0; i < count; ++i)
                in.PushBack(FoliageInstance{Vec3(f32(i) * 5.0f, 0.0f, 0.0f), 0.0f, 1.0f});
            EncodeFoliage(f, in.View());
            w.Add(e, move(f));
        };
        {
            World w;
            pines(w, Vec3(20, 0, 20), 3);
            pines(w, Vec3(350, 0, 20), 2);
            scene::UpdateWorldTransforms(w);
            WorldStreamer s;
            s.Configure(dir, settings, io);
            ZX_REQUIRE(s.SaveCells(w));
            ZX_EXPECT_EQ(s.BuildHlods(), 1u);
        }
        Array<platform::DirectoryEntry> listed;
        platform::ListDirectory(Path::Join(dir, "impostors"), listed);
        Array<String> impostors;
        for (const platform::DirectoryEntry& d : listed)
            impostors.PushBack(d.name);
        ZX_REQUIRE(impostors.Size() == 1u);
        ZX_EXPECT(impostors[0].View().StartsWith("Pine_3_"));

        auto plantCount = [](World& w, u32& frames) {
            u32 n = 0;
            w.Query<const HlodProxy, const Foliage>().Each([&](const HlodProxy&, const Foliage& f) {
                Foliage copy = f;
                DecodeFoliage(copy);
                n += u32(copy.instances.Size());
                frames = f.impostorFrames;
            });
            return n;
        };
        World w;
        WorldStreamer s;
        s.Configure(dir, settings, io);
        // From far away: nothing loaded, every pine is an impostor (at its place in the world).
        const Vec3 far[] = {Vec3(-1000, 0, 20)};
        s.Update(w, far, true, 1e9);
        u32 frames = 0;
        ZX_EXPECT_EQ(plantCount(w, frames), 5u);
        ZX_EXPECT_EQ(frames, 8u);
        // Next to cell (0, 0): it loads with its real pines, and once it has settled its impostors go.
        const Vec3 near[] = {Vec3(-60, 0, 50)};
        s.LoadAllInRange(w, near);
        ZX_EXPECT(s.IsCellLoaded(IVec2(0, 0)));
        ZX_EXPECT(!s.IsCellLoaded(IVec2(3, 0)));
        Thread::SleepMs(1600);
        s.Update(w, near, true, 1e9);
        ZX_EXPECT_EQ(plantCount(w, frames), 2u);
        s.Reset();
    }
    fs::Shutdown();
    platform::DeleteDirectoryTree(dir);
    platform::DeleteDirectoryTree(ddc);
}
