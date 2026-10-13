#include "Core/Containers/Algorithm.h"
#include "Core/Testing/Test.h"
#include "Engine/ComponentRegistry.h"
#include "Engine/SceneRendering.h"
#include "Engine/SceneSerialization.h"
#include "Engine/SceneUndo.h"

using namespace zx;
using ecs::Entity;
using ecs::World;

namespace {

/// Canonical text of the whole scene (every entity with a Transform, sorted by guid) for equality checks.
String Canonical(World& w)
{
    Array<Entity> roots, all;
    CollectRoots(w, roots);
    for (Entity r : roots)
        CollectHierarchy(w, r, all);
    Array<String> lines;
    for (Entity e : all)
        lines.PushBack(ToJsonString(SerializeEntity(w, e), JsonWriteOptions{false}));
    Sort(lines, [](const String& a, const String& b) { return a.View().Compare(b.View()) < 0; });
    String out;
    for (const String& l : lines)
        out += l + "\n";
    return out;
}

Entity Named(World& w, StringView name)
{
    Entity found;
    w.Query<const scene::EntityName>().Each([&](Entity e, const scene::EntityName& n) {
        if (n.value == name)
            found = e;
    });
    return found;
}

void BuildSample(World& w)
{
    const Entity root = scene::CreateNode(w, "Root", Transform(Vec3(1, 2, 3)));
    const Entity a = scene::CreateNode(w, "A", Transform(Vec3(0, 1, 0), Quat::FromAxisAngle(Vec3::Up(), 0.5f), Vec3(2.0f)), root);
    const Entity b = scene::CreateNode(w, "B", Transform(Vec3(4, 0, 0)), root);
    scene::CreateNode(w, "C", Transform(Vec3(0, 0, 9)), root);
    PointLight light;
    light.color = Vec3(1.0f, 0.5f, 0.25f);
    light.lumens = 1234.0f;
    w.Add(a, light);
    MeshRenderer mr;
    mr.mesh = Guid(1, 2);
    mr.materials.PushBack(Guid(3, 4));
    mr.visible = false;
    w.Add(b, mr);
    scene::CreateNode(w, "Leaf", Transform(Vec3(0, -1, 0)), b);
}

} // namespace

ZX_TEST(SceneSerialization, RoundTrip)
{
    RegisterEngineComponents();
    World w;
    scene::InstallHierarchyHooks(w);
    BuildSample(w);
    const JsonValue doc = SaveScene(w, {});
    ZX_EXPECT_EQ(doc.Get("entities").Size(), usize(5));
    ZX_EXPECT(doc.Get("entities")[0].Get("components").Get("PointLight").IsNull()); // root has no light

    JsonValue parsed;
    ZX_EXPECT(ParseJson(ToJsonString(doc).View(), parsed));
    World w2;
    scene::InstallHierarchyHooks(w2);
    Array<String> warnings;
    ZX_EXPECT(LoadScene(parsed, w2, {}, nullptr, &warnings));
    ZX_EXPECT(warnings.Empty());
    ZX_EXPECT_EQ(Canonical(w2), Canonical(w));

    const Entity root = Named(w2, "Root");
    const Span<const Entity> children = scene::GetChildren(w2, root);
    ZX_EXPECT_EQ(children.Size(), usize(3));
    ZX_EXPECT(children[0] == Named(w2, "A") && children[1] == Named(w2, "B") && children[2] == Named(w2, "C"));
    const PointLight* light = w2.Get<const PointLight>(Named(w2, "A"));
    ZX_EXPECT(light && light->lumens == 1234.0f && light->color.y == 0.5f);
    const MeshRenderer* mr = w2.Get<const MeshRenderer>(Named(w2, "B"));
    ZX_EXPECT(mr && mr->mesh == Guid(1, 2) && mr->materials.Size() == 1 && !mr->visible);
    ZX_EXPECT(w2.Has<scene::WorldTransform>(Named(w2, "Leaf")));
    ZX_EXPECT(scene::GetParent(w2, Named(w2, "Leaf")) == Named(w2, "B"));

    // Unknown components are reported, not fatal.
    parsed["entities"][0]["components"].Set("Nonexistent", JsonValue::MakeObject());
    World w3;
    warnings.Clear();
    ZX_EXPECT(LoadScene(parsed, w3, {}, nullptr, &warnings));
    ZX_EXPECT(!warnings.Empty());

    // Version / format checks.
    parsed.Set("version", i64(99));
    ZX_EXPECT(!LoadScene(parsed, w3, {}));

    ClearScene(w2);
    ZX_EXPECT_EQ(w2.EntityCount(), 0u);
}

ZX_TEST(SceneSerialization, UndoRedoAllOperations)
{
    RegisterEngineComponents();
    World w;
    scene::InstallHierarchyHooks(w);
    BuildSample(w);
    SceneUndoStack undo(w);
    const String s0 = Canonical(w);

    // 1. Move.
    undo.Begin("Move");
    undo.Record(Named(w, "A"));
    w.Get<Transform>(Named(w, "A"))->position = Vec3(7, 7, 7);
    ZX_EXPECT(undo.End());
    const String s1 = Canonical(w);

    // 2. Create a child.
    undo.Begin("Create");
    const Entity created = scene::CreateNode(w, "New", Transform(Vec3(5, 5, 5)), Named(w, "C"));
    w.Add(created, SpotLight{});
    undo.RecordCreated(created);
    ZX_EXPECT(undo.End());
    const String s2 = Canonical(w);

    // 3. Delete a subtree (B + Leaf).
    undo.Begin("Delete");
    undo.RecordHierarchy(Named(w, "B"));
    scene::DestroyHierarchy(w, Named(w, "B"));
    ZX_EXPECT(undo.End());
    const String s3 = Canonical(w);

    // 4. Reparent A under C (first child -> last).
    undo.Begin("Reparent");
    undo.Record(Named(w, "A"));
    scene::SetParent(w, Named(w, "A"), Named(w, "C"));
    ZX_EXPECT(undo.End());
    const String s4 = Canonical(w);

    // 5. Remove a component, add another, rename.
    undo.Begin("Edit");
    undo.Record(Named(w, "A"));
    const Entity a = Named(w, "A");
    w.Remove<PointLight>(a);
    w.Add(a, MeshRenderer{});
    w.Get<scene::EntityName>(a)->value = "A2";
    ZX_EXPECT(undo.End());
    const String s5 = Canonical(w);

    // A transaction without changes is not pushed; nested transactions merge.
    undo.Begin("Nothing");
    undo.Record(Named(w, "C"));
    ZX_EXPECT(!undo.End());
    undo.Begin("Outer");
    undo.Begin("Inner");
    undo.Record(Named(w, "C"));
    w.Get<Transform>(Named(w, "C"))->scale = Vec3(3.0f);
    ZX_EXPECT(!undo.End());
    ZX_EXPECT(undo.End());
    ZX_EXPECT_EQ(undo.Count(), usize(6));
    ZX_EXPECT(undo.UndoLabel() == "Outer");
    const String s6 = Canonical(w);

    const String* states[] = {&s0, &s1, &s2, &s3, &s4, &s5, &s6};
    for (usize i = 6; i > 0; --i) {
        ZX_EXPECT(undo.Undo());
        ZX_EXPECT_EQ(Canonical(w), *states[i - 1]);
    }
    ZX_EXPECT(!undo.CanUndo());
    // Restored subtree keeps its sibling position (B between A and C).
    const Span<const Entity> children = scene::GetChildren(w, Named(w, "Root"));
    ZX_EXPECT(children.Size() == 3 && children[1] == Named(w, "B"));
    ZX_EXPECT(scene::GetParent(w, Named(w, "Leaf")) == Named(w, "B"));

    for (usize i = 1; i <= 6; ++i) {
        ZX_EXPECT(undo.Redo());
        ZX_EXPECT_EQ(Canonical(w), *states[i]);
    }
    ZX_EXPECT(!undo.CanRedo());

    // Dirty tracking and branch truncation.
    undo.MarkSaved();
    ZX_EXPECT(!undo.IsDirty());
    undo.Undo();
    ZX_EXPECT(undo.IsDirty());
    undo.Begin("Branch");
    undo.Record(Named(w, "Root"));
    w.Get<Transform>(Named(w, "Root"))->position = Vec3::Zero();
    ZX_EXPECT(undo.End());
    ZX_EXPECT(!undo.CanRedo());
    ZX_EXPECT(undo.IsDirty());
    undo.Undo();
    ZX_EXPECT(undo.IsDirty()); // saved state was on the discarded branch
}
