#include "Core/Jobs/JobSystem.h"
#include "Core/Testing/Test.h"
#include "Scene/Hierarchy.h"

using namespace zx;
using namespace zx::scene;

namespace {
bool Near(Vec3 a, Vec3 b) { return Length(a - b) < 1e-4f; }
} // namespace

ZX_TEST(Hierarchy, ParentChildTransforms)
{
    World w;
    InstallHierarchyHooks(w);
    const Entity root = CreateNode(w, "Root", Transform(Vec3(10, 0, 0)));
    const Entity arm = CreateNode(w, "Arm", Transform(Vec3(0, 2, 0), Quat::FromAxisAngle(Vec3::Up(), HalfPi)), root);
    const Entity hand = CreateNode(w, "Hand", Transform(Vec3(0, 0, 1), Quat(), Vec3(2.0f)), arm);
    ZX_EXPECT(GetParent(w, hand) == arm);
    ZX_EXPECT_EQ(GetChildren(w, root).Size(), usize(1));
    ZX_EXPECT(IsAncestorOf(w, root, hand));
    ZX_EXPECT(!IsAncestorOf(w, hand, root));

    UpdateWorldTransforms(w);
    // Arm is rotated 90 degrees around Y: its +Z maps to world +X.
    ZX_EXPECT(Near(w.Get<const WorldTransform>(hand)->GetPosition(), Vec3(11, 2, 0)));
    ZX_EXPECT(Near(ComputeWorldMatrix(w, hand).GetTranslation(), Vec3(11, 2, 0)));

    // Cycle prevention.
    ZX_EXPECT(!SetParent(w, root, hand));
    ZX_EXPECT(!SetParent(w, root, root));
    ZX_EXPECT(GetParent(w, root).IsNull());
}

ZX_TEST(Hierarchy, ReparentKeepWorld)
{
    World w;
    InstallHierarchyHooks(w);
    const Entity a = CreateNode(w, "A", Transform(Vec3(5, 0, 0)));
    const Entity b = CreateNode(w, "B", Transform(Vec3(0, 0, 3), Quat::FromAxisAngle(Vec3::Up(), Pi)));
    const Entity c = CreateNode(w, "C", Transform(Vec3(1, 1, 1)), a);
    ZX_EXPECT(Near(ComputeWorldMatrix(w, c).GetTranslation(), Vec3(6, 1, 1)));
    ZX_EXPECT(SetParent(w, c, b, true));
    ZX_EXPECT(Near(ComputeWorldMatrix(w, c).GetTranslation(), Vec3(6, 1, 1)));
    ZX_EXPECT(GetChildren(w, a).Empty());
    ZX_EXPECT_EQ(GetChildren(w, b).Size(), usize(1));
    ZX_EXPECT(SetParent(w, c, {}, true)); // detach
    ZX_EXPECT(GetChildren(w, b).Empty());
    ZX_EXPECT(Near(w.Get<const Transform>(c)->position, Vec3(6, 1, 1)));
}

ZX_TEST(Hierarchy, DestroyKeepsLinksConsistent)
{
    World w;
    InstallHierarchyHooks(w);
    const Entity root = CreateNode(w, "Root");
    const Entity mid = CreateNode(w, "Mid", {}, root);
    const Entity leaf1 = CreateNode(w, "Leaf1", {}, mid);
    const Entity leaf2 = CreateNode(w, "Leaf2", {}, mid);
    w.Destroy(leaf1);
    ZX_EXPECT_EQ(GetChildren(w, mid).Size(), usize(1));
    w.Destroy(mid); // orphans leaf2
    ZX_EXPECT(GetChildren(w, root).Empty());
    ZX_EXPECT(w.IsAlive(leaf2) && GetParent(w, leaf2).IsNull());

    const Entity a = CreateNode(w, "A");
    for (i32 i = 0; i < 10; ++i) {
        const Entity child = CreateNode(w, {}, {}, a);
        CreateNode(w, {}, {}, child);
    }
    const u32 before = w.EntityCount();
    DestroyHierarchy(w, a);
    ZX_EXPECT_EQ(w.EntityCount(), before - 21);
}

ZX_TEST(Hierarchy, ManyRootsParallel)
{
    jobs::Initialize({.workerCount = 4});
    {
        World w;
        InstallHierarchyHooks(w);
        Array<Entity> leaves;
        for (i32 r = 0; r < 200; ++r) {
            Entity parent = CreateNode(w, {}, Transform(Vec3(f32(r), 0, 0)));
            for (i32 d = 0; d < 5; ++d)
                parent = CreateNode(w, {}, Transform(Vec3(0, 1, 0)), parent);
            leaves.PushBack(parent);
        }
        UpdateWorldTransforms(w);
        bool ok = true;
        for (i32 r = 0; r < 200; ++r)
            ok &= Near(w.Get<const WorldTransform>(leaves[r])->GetPosition(), Vec3(f32(r), 5, 0));
        ZX_EXPECT(ok);
    }
    jobs::Shutdown();
}
