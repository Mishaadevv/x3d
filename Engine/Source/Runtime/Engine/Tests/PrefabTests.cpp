#include "Core/Containers/Algorithm.h"
#include "Core/FileSystem/FileSystem.h"
#include "Core/HAL/PlatformFile.h"
#include "Core/String/Path.h"
#include "Core/Testing/Test.h"
#include "Engine/ComponentRegistry.h"
#include "Engine/Prefab.h"
#include "Engine/SceneRendering.h"

using namespace zx;
using ecs::Entity;
using ecs::World;

namespace {

String TempContent()
{
    const String dir = Path::Join(Path::Normalize(platform::GetTempDirectory()), "zx_prefab_test");
    platform::DeleteDirectoryTree(dir);
    platform::CreateDirectoryTree(dir);
    return dir;
}

String Canonical(World& w)
{
    Array<Entity> roots, all;
    CollectRoots(w, roots);
    for (Entity r : roots)
        CollectHierarchy(w, r, all);
    Array<String> lines;
    for (Entity e : all) {
        JsonValue j = SerializeEntity(w, e);
        lines.PushBack(ToJsonString(j, JsonWriteOptions{false}));
    }
    Sort(lines, [](const String& a, const String& b) { return a.View().Compare(b.View()) < 0; });
    String out;
    for (const String& l : lines)
        out += l + "\n";
    return out;
}

Entity Child(World& w, Entity parent, StringView name)
{
    for (Entity c : scene::GetChildren(w, parent))
        if (const scene::EntityName* n = w.Get<const scene::EntityName>(c); n && n->value == name)
            return c;
    return {};
}

/// Lamp: root "Lamp" with children "Bulb" (point light) and "Base".
Entity BuildLamp(World& w)
{
    const Entity lamp = scene::CreateNode(w, "Lamp", Transform(Vec3(3, 0, 0)));
    const Entity bulb = scene::CreateNode(w, "Bulb", Transform(Vec3(0, 2, 0)), lamp);
    PointLight l;
    l.lumens = 500.0f;
    w.Add(bulb, l);
    scene::CreateNode(w, "Base", Transform(Vec3(0, 0.1f, 0)), lamp);
    return lamp;
}

} // namespace

ZX_TEST(Prefab, InstancesOverridesAndRoundTrip)
{
    RegisterEngineComponents();
    const String content = TempContent();
    PrefabLibrary library(nullptr, content);
    World w;
    scene::InstallHierarchyHooks(w);

    const Entity source = BuildLamp(w);
    const Guid sourceGuid = EnsureEntityGuid(w, source);
    ZX_REQUIRE(CreatePrefabFromEntity(w, source, "Props/Lamp.zprefab", library, nullptr));
    ZX_EXPECT(platform::FileExists(content + "/Props/Lamp.zprefab"));
    const Entity a = FindEntityByGuid(w, sourceGuid); // same guid, rebuilt as an instance
    ZX_REQUIRE(w.IsAlive(a) && w.Has<PrefabInstance>(a));
    ZX_EXPECT(w.Has<PrefabLink>(Child(w, a, "Bulb")));
    ZX_EXPECT(w.Get<const Transform>(a)->position == Vec3(3, 0, 0)); // placement kept

    const Entity b = InstantiatePrefabFile(w, library, "Props/Lamp.zprefab", Transform(Vec3(-5, 0, 1)));
    ZX_REQUIRE(w.IsAlive(b));
    const Guid bGuid = GetEntityGuid(w, b);
    ZX_EXPECT_EQ(scene::GetChildren(w, b).Size(), usize(2));
    ZX_EXPECT(w.Get<const PointLight>(Child(w, b, "Bulb"))->lumens == 500.0f);

    // Overrides on instance B: brighter bulb, base removed, an extra child, bulb lost its light colour change.
    w.Get<PointLight>(Child(w, b, "Bulb"))->lumens = 1500.0f;
    scene::DestroyHierarchy(w, Child(w, b, "Base"));
    scene::CreateNode(w, "Shade", Transform(Vec3(0, 2.2f, 0)), b);
    Array<String> overrides;
    GetPrefabOverrides(w, Child(w, b, "Bulb"), library, overrides);
    ZX_EXPECT(overrides.Contains(String("PointLight.lumens")));
    ZX_EXPECT(!overrides.Contains(String("Transform.position")));

    // The scene file stores B's bulb as a small delta, not as a full light.
    const JsonValue doc = SaveScene(w, SceneIOOptions{nullptr, {}, &library});
    const String text = ToJsonString(doc);
    ZX_EXPECT(text.View().Contains("removedFromPrefab"));
    World w2;
    scene::InstallHierarchyHooks(w2);
    Array<String> warnings;
    ZX_EXPECT(LoadScene(doc, w2, SceneIOOptions{nullptr, {}, &library}, nullptr, &warnings));
    ZX_EXPECT(warnings.Empty());
    ZX_EXPECT_EQ(Canonical(w2), Canonical(w));

    // Edit the prefab through instance A (dimmer bulb + new child) and apply: B keeps its brighter bulb override,
    // gets the new child, and still has no base.
    w.Get<PointLight>(Child(w, a, "Bulb"))->color = Vec3(1.0f, 0.5f, 0.2f);
    scene::CreateNode(w, "Switch", Transform(Vec3(0.2f, 0.5f, 0)), a);
    ZX_REQUIRE(ApplyPrefabInstance(w, a, library, nullptr));
    const Entity b2 = FindEntityByGuid(w, bGuid);
    ZX_REQUIRE(w.IsAlive(b2));
    const PointLight* bulbB = w.Get<const PointLight>(Child(w, b2, "Bulb"));
    ZX_REQUIRE(bulbB != nullptr);
    ZX_EXPECT_EQ(bulbB->lumens, 1500.0f);
    ZX_EXPECT(bulbB->color.y == 0.5f);
    ZX_EXPECT(w.IsAlive(Child(w, b2, "Switch")));
    ZX_EXPECT(!w.IsAlive(Child(w, b2, "Base")));
    ZX_EXPECT(w.IsAlive(Child(w, b2, "Shade")));

    // Revert drops B's overrides (only the placement stays).
    const Entity b3 = RefreshPrefabInstance(w, b2, library, true);
    ZX_EXPECT(w.Get<const PointLight>(Child(w, b3, "Bulb"))->lumens == 500.0f);
    ZX_EXPECT(w.IsAlive(Child(w, b3, "Base")));
    ZX_EXPECT(!w.IsAlive(Child(w, b3, "Shade")));
    ZX_EXPECT(w.Get<const Transform>(b3)->position == Vec3(-5, 0, 1));
    platform::DeleteDirectoryTree(content);
}

ZX_TEST(Prefab, NestedPrefabs)
{
    RegisterEngineComponents();
    const String content = TempContent();
    PrefabLibrary library(nullptr, content);
    World w;
    scene::InstallHierarchyHooks(w);

    const Guid lampGuid = EnsureEntityGuid(w, BuildLamp(w));
    ZX_REQUIRE(CreatePrefabFromEntity(w, FindEntityByGuid(w, lampGuid), "Lamp.zprefab", library, nullptr));
    const Entity lamp = FindEntityByGuid(w, lampGuid);
    // A room containing a lamp instance (with its own override) becomes a prefab too.
    const Entity room = scene::CreateNode(w, "Room");
    scene::CreateNode(w, "Floor", Transform::Identity(), room);
    const Entity lampInRoom = InstantiatePrefabFile(w, library, "Lamp.zprefab", Transform(Vec3(1, 0, 1)), room);
    ZX_REQUIRE(w.IsAlive(Child(w, lampInRoom, "Bulb")));
    w.Get<PointLight>(Child(w, lampInRoom, "Bulb"))->lumens = 900.0f;
    ZX_REQUIRE(CreatePrefabFromEntity(w, room, "Room.zprefab", library, nullptr));

    const Entity r1 = InstantiatePrefabFile(w, library, "Room.zprefab", Transform(Vec3(0, 0, 10)));
    ZX_REQUIRE(w.IsAlive(r1));
    const Entity innerLamp = Child(w, r1, "Lamp");
    ZX_REQUIRE(w.IsAlive(innerLamp) && w.Has<PrefabInstance>(innerLamp));
    ZX_EXPECT_EQ(w.Get<const PointLight>(Child(w, innerLamp, "Bulb"))->lumens, 900.0f); // the room's override

    // Scene override on the nested bulb survives a save/load round trip.
    ZX_REQUIRE(w.IsAlive(Child(w, innerLamp, "Bulb")));
    w.Get<PointLight>(Child(w, innerLamp, "Bulb"))->range = 3.0f;
    const JsonValue doc = SaveScene(w, SceneIOOptions{nullptr, {}, &library});
    World w2;
    scene::InstallHierarchyHooks(w2);
    Array<String> warnings;
    ZX_EXPECT(LoadScene(doc, w2, SceneIOOptions{nullptr, {}, &library}, nullptr, &warnings));
    ZX_EXPECT(warnings.Empty());
    ZX_EXPECT_EQ(Canonical(w2), Canonical(w));

    // Deleting a member of the nested lamp (and the room's floor) is remembered across save/load too.
    scene::DestroyHierarchy(w, Child(w, innerLamp, "Base"));
    scene::DestroyHierarchy(w, Child(w, r1, "Floor"));
    {
        const JsonValue doc2 = SaveScene(w, SceneIOOptions{nullptr, {}, &library});
        World w3;
        scene::InstallHierarchyHooks(w3);
        ZX_EXPECT(LoadScene(doc2, w3, SceneIOOptions{nullptr, {}, &library}));
        ZX_EXPECT_EQ(Canonical(w3), Canonical(w));
        const Entity r1b = FindEntityByGuid(w3, GetEntityGuid(w, r1));
        ZX_EXPECT(!w3.IsAlive(Child(w3, Child(w3, r1b, "Lamp"), "Base")));
        ZX_EXPECT(!w3.IsAlive(Child(w3, r1b, "Floor")));
    }

    // Changing the inner prefab reaches the lamp inside the room instance.
    w.Get<PointLight>(Child(w, lamp, "Bulb"))->castShadows = true;
    const Guid r1Guid = GetEntityGuid(w, r1);
    ZX_REQUIRE(ApplyPrefabInstance(w, lamp, library, nullptr));
    const Entity reloadedRoom = RefreshPrefabInstance(w, FindEntityByGuid(w, r1Guid), library);
    const PointLight* bulb = w.Get<const PointLight>(Child(w, Child(w, reloadedRoom, "Lamp"), "Bulb"));
    ZX_REQUIRE(bulb != nullptr);
    ZX_EXPECT(bulb->castShadows);
    ZX_EXPECT_EQ(bulb->lumens, 900.0f);
    ZX_EXPECT_EQ(bulb->range, 3.0f);
    ZX_EXPECT(!w.IsAlive(Child(w, Child(w, reloadedRoom, "Lamp"), "Base")));
    platform::DeleteDirectoryTree(content);
}
