#pragma once

// Behaviour Tree editor: the .zbt node canvas - the blackboard on the left, the tree (top-down, children ordered left to
// right as they run) in the middle, the selected node's settings on the right. While playing, the nodes of the
// watched AI light up: running (amber), last succeeded (green), last failed (red).

#include "Engine/BehaviorTree.h"
#include "Core/Math/Geometry.h"
#include "Scene/ECS/World.h"

namespace zx::editor {

class Editor;

struct BTEditorState
{
    bool open = false;
    String path; // physical
    BehaviorTreeAsset tree;
    Array<String> problems;
    bool dirty = false;
    bool capturesKeys = false;
    bool frameRequested = false;
    Array<u32> selection;
    Vec2 pan;
    f32 zoom = 1.0f;

    enum class Drag : u8
    {
        None,
        Nodes,
        Box,
        Link,
        Pan,
    };
    Drag drag = Drag::None;
    Vec2 dragStart, rightPress;
    bool dragMoved = false;
    String beforeDrag;
    u32 linkParent = 0;

    Array<String> undo, redo; // .zbt snapshots
    String editField;
    String addSearch;
    bool addFocus = false;
    Vec2 addPosition;
    i32 addParent = -1; // the new node becomes this node's child (dropped link)
    ecs::Entity debugEntity;
};

void OpenBTEditor(Editor& ed, StringView physicalPath);
void DrawBTEditorPanel(Editor& ed);
/// A new tree with a Selector root (returns false if it can't be written).
bool CreateBehaviorTreeFile(StringView physicalPath);

} // namespace zx::editor
