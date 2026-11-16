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
//   Hearing: noises (BehaviorSystem::ReportNoise; scripts noise(position, range[, tag]) / entity.makeNoise(range[, tag]);
//     the target's footsteps: its Character speed x footstepNoise meters) are heard within their range x `hearing`, half
//     as far through walls (physics). The loudest one heard writes noiseHeard (bool, true until the tree clears it),
//     noiseLocation (vec3), noiseTag (string) and noiseInstigator (entity, when known).

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

/// `validate` = false accepts trees being edited (decorators without a child); child indices are always checked.
bool ParseBehaviorTree(StringView json, BehaviorTreeAsset& out, String* error = nullptr, bool validate = true);
/// Problems that keep a tree from running as drawn (empty decorators, unconnected nodes), one line each.
void CheckBehaviorTree(const BehaviorTreeAsset& tree, Array<String>& problems);
String WriteBehaviorTree(const BehaviorTreeAsset& tree);
/// Tidy top-down layout of the editor positions: leaves side by side, parents centered over their children; nodes not
/// under the root are placed in a row below.
void LayoutBehaviorTree(BehaviorTreeAsset& tree, Vec2 nodeSize = Vec2(200.0f, 58.0f), Vec2 gap = Vec2(24.0f, 56.0f));
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
    // Hearing
    f32 hearing = 1.0f;         // multiplier of every noise's range (0 = deaf)
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
    f32 footstepNoise = 1.5f;         // meters a target's footsteps carry per m/s of its Character speed (0 = silent)

    /// A sound the AI can hear (heard by the trees on their next Tick, then forgotten).
    struct Noise
    {
        Vec3 position;
        f32 range = 10.0f;      // meters it carries (in the open, for hearing 1)
        ecs::Entity instigator; // who made it (not heard by itself)
        String tag;             // "footstep", "gunshot", ... (blackboard noiseTag)
    };
    void ReportNoise(Vec3 position, f32 range, ecs::Entity instigator = {}, StringView tag = {});
    /// Noises heard by the last Tick (debug views; reported ones wait in m_noises until the next).
    Span<const Noise> LastNoises() const { return m_lastNoises.View(); }

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
    Array<Noise> m_noises, m_lastNoises;
    u32 m_rng = 12345;
    f32 Random01();

    struct Context;
    BTStatus Run(Context& c, u32 node);
    void Abort(Context& c, u32 node);
    /// Returns the target entity (found by name, even when not seen).
    ecs::Entity UpdateSight(ecs::World& world, ecs::Entity e, BehaviorTree& bt);
    void UpdateHearing(ecs::World& world, ecs::Entity e, BehaviorTree& bt, ecs::Entity target);
};

} // namespace zx
