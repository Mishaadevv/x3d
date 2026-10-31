#include "Core/FileSystem/FileSystem.h"
#include "Core/HAL/PlatformFile.h"
#include "Core/String/Path.h"
#include "Core/Testing/Test.h"
#include "Engine/ComponentRegistry.h"
#include "Engine/ScriptAssignment.h"
#include "Engine/Scripting.h"
#include "Scene/Hierarchy.h"

using namespace zx;

ZX_TEST(ScriptAssignment, ValidateAssignReplaceAndUndo)
{
    RegisterEngineComponents();
    const String root = Path::Join(platform::GetTempDirectory(), String::Format("zx_script_assignment_{}", Guid::New()));
    ZX_REQUIRE(platform::CreateDirectoryTree(root + "/Scripts"));
    ZX_REQUIRE(fs::WriteTextFile(root + "/Scripts/First.zs", "script First {}"));
    ZX_REQUIRE(fs::WriteTextFile(root + "/Scripts/Second.zgraph", "{}"));
    ecs::World world;
    scene::InstallHierarchyHooks(world);
    ecs::Entity e = scene::CreateNode(world, "Target", Transform::Identity());
    SceneUndoStack undo(world);
    const Guid guid = EnsureEntityGuid(world, e);

    ZX_EXPECT(AssignScript(world, undo, e, root, "../outside.zs") == ScriptAssignmentResult::InvalidPath);
    ZX_EXPECT(AssignScript(world, undo, e, root, "Scripts/Missing.zs") == ScriptAssignmentResult::MissingFile);
    ZX_EXPECT(AssignScript(world, undo, e, root, "Scripts/First.txt") == ScriptAssignmentResult::InvalidPath);
    ZX_EXPECT(AssignScript(world, undo, ecs::NullEntity, root, "Scripts/First.zs") == ScriptAssignmentResult::InvalidEntity);
    ZX_EXPECT(!world.Has<Script>(e));
    ZX_EXPECT(!undo.CanUndo());

    ZX_EXPECT(AssignScript(world, undo, e, root, root + "/Scripts/First.zs") == ScriptAssignmentResult::Assigned);
    ZX_REQUIRE(world.Has<Script>(e));
    ZX_EXPECT(world.Get<Script>(e)->file == "Scripts/First.zs");
    world.Get<Script>(e)->properties = "{\"speed\":2}";
    ZX_EXPECT(AssignScript(world, undo, e, root, "Scripts/First.zs") == ScriptAssignmentResult::AlreadyAssigned);
    ZX_EXPECT(world.Get<Script>(e)->properties == "{\"speed\":2}");
    ZX_EXPECT_EQ(undo.Count(), usize(1));
    ZX_EXPECT(AssignScript(world, undo, e, root, "Scripts/Second.zgraph") == ScriptAssignmentResult::Assigned);
    ZX_EXPECT(world.Get<Script>(e)->properties.Empty());
    ZX_EXPECT_EQ(undo.Count(), usize(2));
    ZX_EXPECT(undo.Undo());
    e = FindEntityByGuid(world, guid);
    ZX_REQUIRE(e && world.Has<Script>(e));
    ZX_EXPECT(world.Get<Script>(e)->file == "Scripts/First.zs");
    ZX_EXPECT(world.Get<Script>(e)->properties == "{\"speed\":2}");
    ZX_EXPECT(undo.Undo());
    e = FindEntityByGuid(world, guid);
    ZX_REQUIRE(e);
    ZX_EXPECT(!world.Has<Script>(e));
    ZX_EXPECT(undo.Redo());
    e = FindEntityByGuid(world, guid);
    ZX_REQUIRE(e && world.Has<Script>(e));
    ZX_EXPECT(world.Get<Script>(e)->file == "Scripts/First.zs");
    platform::DeleteDirectoryTree(root);
}
