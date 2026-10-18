#pragma once

// ZGraph: visual scripting (nodes and wires, like Unreal Blueprints) compiled into ZeqouScript bytecode.
//
// A .zgraph file (JSON) holds variables, nodes (type, position, literal pin values, settings) and links. A Script
// component can point at a .zgraph instead of a .zs: ScriptSystem turns the graph into ZeqouScript source
// (CompileGraph) and compiles that, so graphs get everything scripts have - per-entity props in Details (exposed
// variables), hot reload while playing, the same engine API. Errors point at the node they come from.
//
//   Events      Start, Update, Key Pressed / Released, Every N Seconds, UI Click / Changed, Trigger / Collision,
//               Custom Event - each starts a chain of white execution wires.
//   Flow        Branch, Sequence, For Loop, Do Once, Flip Flop, Delay.
//   Actions     run in order along the wires (Set Position, Print, Play Sound, Set Text, Set Variable, ...).
//   Pure        values, computed where they're used (math, compare, vectors, Get Position, Find Entity, ...).

#include "Core/Containers/Array.h"
#include "Core/Containers/HashMap.h"
#include "Core/Math/Vector.h"
#include "Core/String/String.h"

namespace zx {

enum class GraphPinType : u8
{
    Exec,
    Any,
    Number,
    Bool,
    String,
    Vec3,
    Entity,
};

enum class GraphNodeKind : u8
{
    Event,  // starts a chain (no exec input)
    Action, // exec in -> statement -> exec out
    Flow,   // exec in -> several exec outs (special code)
    Pure,   // no exec pins: an expression
};

struct GraphPinDef
{
    const char* name = nullptr;  // "target", "then", "value"
    GraphPinType type = GraphPinType::Any;
    const char* defaultValue = ""; // literal used when nothing is linked (inputs); "self" = the script's entity
    const char* expr = "";         // outputs: the expression ({input} / {@setting} / {$tmp} expanded)
};

struct GraphSettingDef
{
    const char* name = nullptr; // "variable", "key", "event"
    const char* defaultValue = "";
};

struct GraphNodeDef
{
    const char* id;       // "action.setPosition"
    const char* title;    // "Set Position"
    const char* category; // "Transform"
    GraphNodeKind kind;
    GraphPinDef inputs[6];
    GraphPinDef outputs[5];
    GraphSettingDef settings[2];
    const char* code;    // Action: the statement; Event: the function ("fn update(dt)"); Pure: unused (outputs carry expr)
    const char* tooltip;

    u32 InputCount() const;
    u32 OutputCount() const;
    const GraphPinDef* Input(StringView name) const;
    const GraphPinDef* Output(StringView name) const;
};

/// Every node type, in menu order.
Span<const GraphNodeDef> GraphNodeLibrary();
const GraphNodeDef* FindGraphNode(StringView id);
const char* GraphPinTypeName(GraphPinType t);
/// Can an output of type `from` feed an input of type `to` (numbers / bools / vectors convert to text).
bool GraphPinsCompatible(GraphPinType from, GraphPinType to);

struct GraphVariable
{
    String name;
    GraphPinType type = GraphPinType::Number;
    String value = "0";   // literal
    bool exposed = false; // a prop: editable per entity in Details
    String tooltip;
};

struct GraphNode
{
    u32 id = 0;
    String type; // GraphNodeDef::id
    Vec2 position;
    HashMap<String, String> values;   // literal input values by pin name
    HashMap<String, String> settings; // by setting name
    String comment;
};

struct GraphLink
{
    u32 fromNode = 0;
    String fromPin;
    u32 toNode = 0;
    String toPin;
};

struct Graph
{
    String name = "Graph";
    Array<GraphVariable> variables;
    Array<GraphNode> nodes;
    Array<GraphLink> links;

    GraphNode* FindNode(u32 id);
    const GraphNode* FindNode(u32 id) const;
    u32 NextId() const;
    /// The link into an input pin (inputs take one wire), or null.
    const GraphLink* LinkInto(u32 node, StringView pin) const;
    /// The links out of an output pin.
    void LinksFrom(u32 node, StringView pin, Array<const GraphLink*>& out) const;
    /// The type a pin carries here (Get / Set Variable take their variable's type).
    GraphPinType PinType(const GraphNode& node, const GraphPinDef& pin) const;
    const GraphVariable* FindVariable(StringView name) const;
};

bool ParseGraph(StringView json, Graph& out, String* error = nullptr);
String WriteGraph(const Graph& graph);

struct GraphCompileResult
{
    String source;          // ZeqouScript
    Array<String> errors;   // "Node 'Set Text' (#12): ..."
    Array<String> warnings; // unconnected nodes and the like
    Array<u32> errorNodes;  // nodes with errors (the editor outlines them)
    Array<u32> lineNodes;   // node id of every source line (0 = none), 1-based lines -> index line - 1
};

/// Graph -> ZeqouScript source. False when the graph has errors (the source is still produced for inspection).
/// `trace`: every event and statement first calls __trace(nodeId) (the editor's graph debugger).
bool CompileGraph(const Graph& graph, GraphCompileResult& out, bool trace = false);
/// "file.zgraph:LINE:COL: message" from the script compiler -> the node it belongs to.
String DescribeGraphError(const Graph& graph, const GraphCompileResult& result, u32 line, StringView message);

/// A small example graph (Spinner): turns its entity, logs on Space - Starter Content and tests.
Graph MakeExampleGraph();
/// The Third Person template's mini game (Engine/Private/ZGraphDemo.cpp): coins, a gate that needs them, the HUD
/// with a pause menu.
Graph MakeCoinGraph();
Graph MakeGateGraph();
Graph MakeGameHUDGraph();

} // namespace zx
