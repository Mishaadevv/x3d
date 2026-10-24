#pragma once

// Behaviour Trees (M18): AI decisions as a tree of small tasks, ticked for every entity with a Behavior Tree component.
//
//   .zbt file (JSON, Content-relative, hot-reloaded): nodes with children; the first node is the root.
//     Composites: Selector (first child that doesn't fail; re-checks higher-priority children every tick and aborts a
//                 running lower one when one of them succeeds or runs - "chase when the player shows up"),
//                 Sequence (children in order; Condition children before the running one are re-checked every tick,
//                 a failing one aborts the sequence), Parallel (all children; succeeds when `count` succeed).
//     Decorators (one child): Inverter, Succeeder, Repeat (count, 0 = forever), Cooldown (seconds after it finished).
//     Tasks: Wait (seconds, deviation), MoveTo (key = vec3 or entity, radius; uses the Nav Mesh Agent), Stop,
//            Condition (key op value: set / notset / == != < > <= >=), SetValue (key value), ClearValue (key),
//            RandomPoint (key, radius: a reachable point around the entity), NextWaypoint (key, group: the children
//            of the entity named `group`, in order), FaceTarget (key), Event (name: calls fn onAIEvent(name) in the
//            entity's scripts), Log (text).
//   Blackboard: per-entity values (bool, number, vec3, string, entity), defaults in the file, scripts read and write
//     them (entity.bbGet(key) / entity.bbSet(key, value)).
//   Sight: the component's sightRange / sightAngle look for the entity named `targetName` (line of sight through the
//     physics when it runs) and write target (entity), targetVisible (bool), targetLastSeen (vec3).

#include "Core/Containers/Array.h"
#include "Core/Containers/HashMap.h"
#include "Core/Math/Math.h"
#include "Core/Reflection/Reflection.h"
#include "Core/String/String.h"
#include "Scene/ECS/World.h"

namespace zx {

class NavigationSystem;
class PhysicsSystem;
class ScriptSystem;

enum class BTNodeType : u8
{
    Selector,
    Sequence,
    Parallel,
    Inverter,
    Succeeder,
    Repeat,
    Cooldown,
    Wait,
    MoveTo,
    Stop,
    Condition,
    SetValue,
    ClearValue,
    RandomPoint,
    NextWaypoint,
    FaceTarget,
    Event,
    Log,
    Count
};

const char* BTNodeTypeName(BTNodeType t);
bool BTNodeTypeFromName(StringView name, BTNodeType& out);
bool BTIsComposite(BTNodeType t);
bool BTIsDecorator(BTNodeType t);

enum class BBType : u8
{
    None,
    Bool,
    Number,
    Vec3,
    String,
    Entity
};

struct BBValue
{
    BBType type = BBType::None;
    f64 number = 0.0; // also bool (0 / 1)
    Vec3 vec;
    String text;
    ecs::Entity entity;

    static BBValue Bool(bool b);
    static BBValue Number(f64 n);
    static BBValue Vector(Vec3 v);
    static BBValue Text(StringView s);
    static BBValue Of(ecs::Entity e);
    bool IsSet() const { return type != BBType::None; }
    bool Truthy() const;
};

struct BTNode
{
    BTNodeType type = BTNodeType::Sequence;
    String name;            // shown in the editor / debug output
    Array<u32> children;
    // Parameters (which ones matter depends on the type).
    String key;             // blackboard key
    String op;              // Condition operator
    BBValue value;          // Condition / SetValue operand
    f32 seconds = 1.0f;     // Wait / Cooldown
    f32 deviation = 0.0f;   // Wait: +- random seconds
    f32 radius = 0.5f;      // MoveTo acceptance / RandomPoint radius
    i32 count = 0;          // Repeat (0 = forever) / Parallel success count (0 = all)
    String text;            // Event name / Log text / NextWaypoint group
    Vec2 editorPosition;    // node position in the editor canvas
};

struct BehaviorTreeAsset
{
    String name;
    Array<BTNode> nodes;    // nodes[0] is the root
    Array<Pair<String, BBValue>> defaults; // blackboard defaults
};

bool ParseBehaviorTree(StringView json, BehaviorTreeAsset& out, String* error = nullptr);
String WriteBehaviorTree(const BehaviorTreeAsset& tree);
/// Patrol between waypoints (children of "Waypoints"), chase the target ("Player") while it is seen, look around where
/// it was last seen, then go back to patrolling.
BehaviorTreeAsset MakeGuardTree();

enum class BTStatus : u8
{
    Idle,
    Running,
    Success,
    Failure
};

struct BehaviorTree
{
    String tree;                // .zbt, Content-relative
    bool enabled = true;
    // Sight
    String targetName = "Player";
    f32 sightRange = 20.0f;     // meters
    f32 sightAngle = 120.0f;    // degrees (full cone)
    f32 eyeHeight = 1.6f;
    // Runtime (not saved).
    struct NodeState
    {
        BTStatus status = BTStatus::Idle;
        u32 child = 0;          // running child (composites) / iterations (Repeat)
        f32 timer = 0.0f;       // Wait remaining / Cooldown until
        u32 aux = 0;            // Parallel successes, MoveTo repaths
        Vec3 point;             // MoveTo target in flight
    };
    Array<NodeState> states;
    HashMap<String, BBValue> blackboard;
    i32 loadedVersion = -1;     // tree version the states belong to
    u32 waypoint = 0;           // NextWaypoint progress
    f32 clock = 0.0f;
};
ZX_DECLARE_TYPE(BehaviorTree);

class BehaviorSystem
{
public:
    String contentRoot;             // BehaviorTree::tree paths are relative to it
    NavigationSystem* navigation = nullptr;
    PhysicsSystem* physics = nullptr; // line of sight (null: no occlusion)
    ScriptSystem* scripts = nullptr;  // Event -> onAIEvent

    void Tick(ecs::World& world, f32 dt);
    /// Clears the runtime state of every tree (Play starts fresh).
    void Reset(ecs::World& world);
    /// Drops cached files (editor saved a tree); they reload on the next tick.
    void Reload(StringView path = {});
    const BehaviorTreeAsset* Load(StringView path);

    /// Status of the nodes of `e`'s tree this tick (debug views), empty if none.
    static Span<const BehaviorTree::NodeState> NodeStates(const ecs::World& world, ecs::Entity e);

    /// Events fired by Event nodes this frame (entity, name); the host passes them to scripts.
    struct AIEvent
    {
        ecs::Entity entity;
        String name;
    };
    Span<const AIEvent> Events() const { return m_events.View(); }

private:
    struct File
    {
        BehaviorTreeAsset tree;
        bool ok = false;
        i32 version = 0;
    };
    HashMap<String, File> m_files;
    i32 m_nextVersion = 1;
    Array<AIEvent> m_events;
    u32 m_rng = 12345;
    f32 Random01();

    struct Context;
    BTStatus Run(Context& c, u32 node);
    void Abort(Context& c, u32 node);
    void UpdateSight(ecs::World& world, ecs::Entity e, BehaviorTree& bt);
};

} // namespace zx
