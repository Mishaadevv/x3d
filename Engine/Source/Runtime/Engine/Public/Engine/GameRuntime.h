#pragma once

// The game loop shared by Play-in-Editor and the packaged game (ZXGame): which systems start, in which order they
// tick each frame, and how they stop. The owner keeps the systems; this only drives them.
//
//   GameSystems s{.scripts = &scripts, .physics = &physics, ...};
//   BeginGame(world, s);
//   each frame: TickGame(world, s, dt, &input, GameListener(world, fallback));
//   EndGame(s);

#include "Core/Containers/Function.h"
#include "Engine/AudioSystem.h"
#include "Scene/ECS/World.h"

namespace zx {

class ScriptSystem;
class GameModuleSystem;
class PhysicsSystem;
class ParticleSystem;
class FireSystem;
class AnimationSystem;
class InputActionState;
class InputState;
struct InputActionMap;
namespace render {
class RenderAssets;
}

class SoftBodySystem;
class GameUISystem;
class WaterSystem;
class NavigationSystem;
class BehaviorSystem;

struct GameSystems
{
    ScriptSystem* scripts = nullptr;
    GameModuleSystem* game = nullptr;
    AudioSystem* audio = nullptr;
    PhysicsSystem* physics = nullptr;
    ParticleSystem* particles = nullptr;
    FireSystem* fire = nullptr;
    render::RenderAssets* renderAssets = nullptr; // mesh bounds (characters without physics, fire)
    const InputActionMap* inputActions = nullptr;
    InputActionState* inputActionState = nullptr;
    /// Project Features (Features window); null = everything on.
    Function<bool(StringView)> featureEnabled;
    AnimationSystem* animation = nullptr; // Animators (scripts set graph parameters / play clips)
    SoftBodySystem* softBodies = nullptr; // soft-body vehicles (simulated with the physics)
    GameUISystem* gameUI = nullptr;
    WaterSystem* water = nullptr;         // wave heights, buoyancy, splashes
    NavigationSystem* navigation = nullptr; // nav meshes + Nav Mesh Agents (scripts: entity.navTo)
    BehaviorSystem* behavior = nullptr;     // Behavior Tree components (AI decisions, sight)

    bool Enabled(StringView feature) const { return !featureEnabled || featureEnabled(feature); }
};

/// Starts the game on the current world: gameplay clock, scripts, C++ module, audio, physics bodies, fire.
void BeginGame(ecs::World& world, GameSystems& s);
/// One frame: gameplay components, input actions, scripts, C++ module, characters, physics (+ collision callbacks
/// and impact sounds), fire, audio. Particles and terrains are ticked by the owner (they also run while editing).
void TickGame(ecs::World& world, GameSystems& s, f32 dt, const InputState* input, const ListenerPose& listener, bool tickAudio = true);
/// The game's time scale (scripts set time.scale: 0 = paused); hosts scale animation / particles with it.
f32 GameTimeScale(const GameSystems& s);
/// Stops every system started by BeginGame.
void EndGame(GameSystems& s);

/// Listener at the active game camera, else `fallback`.
ListenerPose GameListener(ecs::World& world, const ListenerPose& fallback);

} // namespace zx
