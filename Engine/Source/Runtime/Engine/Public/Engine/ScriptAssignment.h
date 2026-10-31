#pragma once

#include "Engine/SceneUndo.h"

namespace zx {

/// Assign a content-relative or physical .zs/.zgraph file to an entity as one undo step.
/// Paths outside the content directory, missing files and non-script assets are rejected.
enum class ScriptAssignmentResult : u8
{
    Assigned,
    AlreadyAssigned,
    InvalidEntity,
    InvalidPath,
    MissingFile,
};

ScriptAssignmentResult AssignScript(ecs::World& world, SceneUndoStack& undo, ecs::Entity entity,
                                    StringView contentRoot, StringView path);
/// Resolves and validates a script path; returns the content-relative path, or an empty string.
String ResolveScriptPath(StringView contentRoot, StringView path);

} // namespace zx
