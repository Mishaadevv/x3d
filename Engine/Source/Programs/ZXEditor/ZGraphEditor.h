#pragma once

// ZGraph editor: the node canvas for script graphs (.zgraph) - variables on the left, nodes and wires in the middle,
// the selected node's settings and the generated code on the right.

#include "Engine/ZGraph.h"
#include "Core/Containers/HashMap.h"
#include "Core/Math/Geometry.h"
#include "Scene/ECS/World.h"

namespace zx::editor {

class Editor;

struct ZGraphEditorState
{
    bool open = false;
    String path; // physical
    Graph graph;
    GraphCompileResult compiled;
    bool dirty = false;
    bool capturesKeys = false; // the canvas has the keyboard this frame (scene shortcuts are suspended)
    bool frameRequested = false;
    bool showCode = false;
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
    u32 linkNode = 0;
    String linkPin;
    bool linkFromOutput = false;

    Array<String> undo, redo; // .zgraph snapshots
    String clipboard;
    String editField; // the text field being edited (one undo step per edit)
    String addSearch;
    bool addFocus = false;
    Vec2 addPosition;
    bool addHasLink = false;
    u32 addLinkNode = 0;
    String addLinkPin;
    bool addLinkFromOutput = false;

    // Debugging (while playing): which nodes ran when, breakpoints, the entity being watched.
    Array<u32> breakpoints;
    HashMap<u32, f64> hits; // node -> time it last ran
    ecs::Entity debugEntity;
    u32 breakNode = 0;      // paused at this breakpoint
    bool breakPending = false;
    f64 frozenAt = 0.0; // paused: highlights keep this moment
};

void OpenZGraphEditor(Editor& ed, StringView physicalPath);
void DrawZGraphEditorPanel(Editor& ed);
/// ScriptSystem::onGraphTrace: a node of `file` ran on `entity`.
void ZGraphTrace(Editor& ed, StringView file, ecs::Entity entity, u32 node);
/// After the game ticked: a breakpoint that was hit pauses Play.
void ZGraphAfterTick(Editor& ed);
/// A new graph file with On Start / On Update (returns false if it can't be written).
bool CreateZGraphFile(StringView physicalPath);

} // namespace zx::editor
