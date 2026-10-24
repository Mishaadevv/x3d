#pragma once

// ZeqouScript in the engine: the Script component and the system that runs scripts while the game plays.
//
//   ScriptSystem scripts;
//   scripts.SetContentRoot(projectContentDir);           // Script::file is relative to it ('/' separators)
//   scripts.Begin(world);                                // Play: one instance per Script component, start()
//   scripts.Tick(world, dt, &input);                     // every frame: update(dt), hot reload of changed files
//   scripts.End();                                       // Stop
//
// Globals a script sees: entity / transform (its own entity), character (its CharacterController), input, time,
// audio, physics, find(name), plus the standard library. Physics callbacks: collisionEnter(other, speed),
// collisionExit(other), triggerEnter(other), triggerExit(other).
// Language reference: docs/ZeqouScript.md.

#include "Asset/CookedAssets.h"
#include "Core/Containers/Function.h"
#include "Core/Containers/HashMap.h"
#include "Core/Memory/Pointers.h"
#include "Core/Reflection/Reflection.h"
#include "Core/Serialization/Json.h"
#include "Engine/BehaviorTree.h"
#include "Scene/ECS/World.h"
#include "Script/ZeqouScript.h"

namespace zx {

class Localization;

class InputState;
class InputActionState;
class AudioSystem;
class PhysicsSystem;
class FireSystem;
class ParticleSystem;
class GameUISystem;
struct UIEvent;
class SoftBodySystem;
class NavigationSystem;
class AnimationSystem;
struct AnimEvent;
struct PhysicsEvent;

/// Runs a .zs file on the entity. `properties` holds the values of the script's `prop` fields that differ from the
/// defaults, as a JSON object ({"speed": 120, "tint": [1, 0, 0]}).
struct Script
{
    String file;
    String properties;
};
ZX_DECLARE_TYPE(Script);

/// Property values of a Script component (the defaults, overridden by Script::properties).
JsonValue ParseScriptProperties(const Script& script);
/// Stores `value` for `name` (removed again when it equals the default).
void SetScriptProperty(Script& script, const script::Property& prop, const script::Value& value);
script::Value JsonToScriptValue(const JsonValue& json, const script::Property& prop);
JsonValue ScriptValueToJson(const script::Value& value);

/// A starter file for "New Script" (a class named after the file, a speed prop, start/update).
String ScriptTemplate(StringView name);

class ScriptSystem
{
public:
    ScriptSystem();
    ~ScriptSystem();
    ZX_NON_COPYABLE(ScriptSystem)

    void SetContentRoot(StringView physicalDir);
    StringView ContentRoot() const { return m_contentRoot.View(); }

    /// The compiled script for a content-relative file (cached; recompiled when the file changes on disk).
    /// nullptr when it can't be read or doesn't compile (error says why, with line:column).
    const script::Program* GetProgram(StringView file, String* error = nullptr);
    /// Re-checks file times now (otherwise twice per second while ticking).
    void CheckForChanges();

    void Begin(ecs::World& world);
    /// Starts scripts added since the last tick, calls update(dt) on all running ones, applies deferred destroys.
    void Tick(ecs::World& world, f32 dt, const InputState* input);
    void End();
    bool IsRunning() const { return m_running; }
    u32 InstanceCount() const { return u32(m_instances.Size()); }
    script::VM& GetVM() { return *m_vm; }

    /// Sound for scripts (audio.play, entity.playSound); null = scripts play nothing.
    AudioSystem* audio = nullptr;
    /// The project's Input Actions for input.action / actionPressed / actionReleased / axis (null = all released).
    const InputActionState* actions = nullptr;
    /// Rigid bodies and queries for entity.velocity / addForce and the physics global (null = no physics).
    PhysicsSystem* physics = nullptr;
    /// Particle Emitters for entity.emit(n) (null = no bursts).
    ParticleSystem* particles = nullptr;
    /// Animators for entity.animParam / animTrigger / playAnim / animState (null = no animation control).
    AnimationSystem* animation = nullptr;
    SoftBodySystem* softBodies = nullptr; // vehicles: entity.carInput / carSpeed / ...
    NavigationSystem* navigation = nullptr; // Nav Mesh Agents: entity.navTo / navStop / navStatus / navRemaining
    /// Game UI for ui.cursor / ui.overUI (null = no UI).
    GameUISystem* gameUI = nullptr;
    /// tr(key, ...) and lang (null = tr returns the key).
    Localization* localization = nullptr;
    /// time.scale set by scripts (0 = paused); the host scales the game's dt with it. Reset to 1 by Begin.
    f32 timeScale = 1.0f;
    /// Fire for entity.ignite / extinguish / burning and the fire global (null = nothing burns).
    FireSystem* fire = nullptr;
    /// Calls collisionEnter / collisionExit / triggerEnter / triggerExit on the scripts of both entities of each event.
    void DispatchPhysicsEvents(ecs::World& world, Span<const PhysicsEvent> events);
    /// Calls onAnimEvent(name) on the scripts of each event's entity and its ancestors (the player above its mesh).
    void DispatchAnimEvents(ecs::World& world, Span<const AnimEvent> events);
    /// Behaviour tree Event nodes: fn onAIEvent(name) on the entity and its ancestors.
    void DispatchAIEvents(ecs::World& world, Span<const BehaviorSystem::AIEvent> events);
    /// Calls onUIClick(name) / onUIChanged(name, value) on the widget's scripts and its ancestors' (the canvas).
    void DispatchUIEvents(ecs::World& world, Span<const UIEvent> events);

    /// Script graphs compile with __trace(node) calls, reported here (the editor's graph debugger).
    bool traceGraphs = false;
    Function<void(StringView file, ecs::Entity entity, u32 node)> onGraphTrace;
    /// game.get / set / add: values shared by every script while playing (score, keys found...); cleared by Begin.
    const script::Value* GameValue(StringView key) const { return m_gameValues.Find(key); }
    /// Entities running `file` (content-relative).
    void InstancesOf(StringView file, Array<ecs::Entity>& out) const;
    /// A field (var / prop) of the script `file` running on `entity`, as text. False if there's none.
    bool ReadField(ecs::Entity entity, StringView file, StringView field, String& out) const;

    /// log()/print() output and errors (file:line: message).
    Function<void(StringView message, bool isError)> onLog;
    /// Material id -> name, for surface types (hit.surface, character.surface); set by the host.
    Function<String(asset::AssetId)> materialName;

private:
    struct CachedProgram;
    struct RunningScript;
    CachedProgram* Load(StringView file);
    void Report(StringView message, bool isError);
    void StartInstances(ecs::World& world);
    void RebindGlobals(RunningScript& rs);

    String m_contentRoot;
    UniquePtr<script::VM> m_vm;
    script::NativeType* m_entityType = nullptr;
    script::NativeType* m_characterType = nullptr;
    script::NativeType* m_hitType = nullptr;
    HashMap<String, UniquePtr<CachedProgram>> m_programs;
    Array<UniquePtr<RunningScript>> m_instances;
    Array<script::Program*> m_retired; // replaced by hot reload, freed when no instance uses them
    f64 m_checkTimer = 0.0;
    bool m_running = false;
    RunningScript* m_current = nullptr; // the script being called (for __trace)
    HashMap<String, script::Value> m_gameValues;
    struct MoveTween
    {
        ecs::Entity entity;
        Vec3 from, to;
        f32 duration = 0.0f, elapsed = 0.0f;
    };
    Array<MoveTween> m_moves; // entity.moveTo

public:
    // State the native bindings read (valid during Tick).
    struct Context
    {
        ecs::World* world = nullptr;
        const InputState* input = nullptr;
        f32 dt = 0.0f;
        f32 realDt = 0.0f; // unscaled by time.scale
        f64 elapsed = 0.0;
        u64 frame = 0;
        Array<ecs::Entity> destroyQueue;
        struct Hit
        {
            ecs::Entity entity;
            Vec3 point, normal;
            f32 distance;
        };
        Array<Hit> hits; // raycast results handed to scripts this frame
    } context;
};

} // namespace zx
