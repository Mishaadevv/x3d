#include "Engine/ScriptAssignment.h"
#include "Engine/Scripting.h"
#include "Core/HAL/PlatformFile.h"
#include "Core/String/Path.h"

namespace zx {

String ResolveScriptPath(StringView contentRoot, StringView path)
{
    if (contentRoot.Empty() || path.Empty() || (!Path::HasExtension(path, ".zs") && !Path::HasExtension(path, ".zgraph")))
        return {};
    const String root = platform::GetAbsolutePath(contentRoot);
    const String absolute = Path::IsAbsolute(path) ? Path::Normalize(path) : Path::Join(root, path);
    if (!Path::IsUnder(absolute, root))
        return {};
    return Path::MakeRelative(absolute, root);
}

ScriptAssignmentResult AssignScript(ecs::World& world, SceneUndoStack& undo, ecs::Entity entity,
                                    StringView contentRoot, StringView path)
{
    if (!world.IsAlive(entity))
        return ScriptAssignmentResult::InvalidEntity;
    const String relative = ResolveScriptPath(contentRoot, path);
    if (relative.Empty())
        return ScriptAssignmentResult::InvalidPath;
    if (!platform::FileExists(Path::Join(contentRoot, relative)))
        return ScriptAssignmentResult::MissingFile;
    const Script* previous = world.Get<Script>(entity);
    if (previous && previous->file == relative)
        return ScriptAssignmentResult::AlreadyAssigned;
    undo.Begin("Assign Script");
    undo.Record(entity);
    world.Set(entity, Script{relative, String()});
    undo.End();
    return ScriptAssignmentResult::Assigned;
}

} // namespace zx
