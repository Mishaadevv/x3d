#include "Engine/GameRuntime.h"
#include "Engine/Animation.h"
#include "Engine/SoftBody.h"
#include "Engine/GameUI.h"
#include "Engine/Fire.h"
#include "Engine/GameModule.h"
#include "Engine/Gameplay.h"
#include "Engine/InputActions.h"
#include "Engine/Particles.h"
#include "Engine/PhysicsSystem.h"
#include "Engine/Scripting.h"
#include "Render/RenderAssets.h"
#include "Scene/Hierarchy.h"

namespace zx {

namespace {
MeshBoundsFn MeshBounds(render::RenderAssets* assets)
{
    return [assets](asset::AssetId id, AABB& out) {
        const render::GpuMesh* mesh = assets ? assets->GetMesh(id) : nullptr;
        if (!mesh)
            return false;
        out = mesh->bounds;
        return true;
    };
}
} // namespace

void BeginGame(ecs::World& world, GameSystems& s)
{
    if (GameplayClock* clock = world.GetSingleton<GameplayClock>())
        *clock = GameplayClock{};
    if (s.gameUI)
        s.gameUI->Reset();
    if (s.scripts)
        s.scripts->Begin(world);
    if (s.game)
        s.game->Begin(world);
    if (s.inputActionState)
        *s.inputActionState = InputActionState();
    if (s.audio) {
        s.audio->Begin(world);
        PhysicsSystem* physics = s.physics;
        s.audio->velocityOf = [physics](ecs::Entity e) { return physics && physics->IsRunning() ? physics->GetVelocity(e) : Vec3(); };
    }
    if (s.physics && s.Enabled("physics")) {
        scene::UpdateWorldTransforms(world);
        s.physics->Begin(world, s.renderAssets);
    }
    if (s.fire) {
        s.fire->Clear();
        render::RenderAssets* assets = s.renderAssets;
        s.fire->meshBounds = [assets](asset::AssetId id, AABB& out) {
            const render::GpuMesh* mesh = assets ? assets->GetMesh(id) : nullptr;
            if (!mesh)
                return false;
            out = mesh->bounds;
            return true;
        };
    }
}

f32 GameTimeScale(const GameSystems& s) { return s.scripts && s.Enabled("scripts") ? Clamp(s.scripts->timeScale, 0.0f, 10.0f) : 1.0f; }

void TickGame(ecs::World& world, GameSystems& s, f32 realDt, const InputState* input, const ListenerPose& listener, bool tickAudio)
{
    // Scripts may slow down or pause the game (time.scale); UI and audio keep running in real time.
    const f32 dt = realDt * GameTimeScale(s);
    const bool physicsRunning = s.physics && s.physics->IsRunning();
    TickGameplay(world, dt);
    if (s.inputActionState && s.inputActions)
        s.inputActionState->Update(*s.inputActions, input);
    if (s.scripts && s.Enabled("scripts")) {
        s.scripts->actions = s.inputActionState;
        s.scripts->physics = physicsRunning ? s.physics : nullptr;
        s.scripts->particles = s.particles;
        s.scripts->animation = s.animation;
        s.scripts->softBodies = s.softBodies;
        if (s.animation) // last frame's footsteps and authored events
            s.scripts->DispatchAnimEvents(world, s.animation->Events());
        s.scripts->gameUI = s.gameUI;
        if (s.gameUI) // button clicks, slider / toggle changes since the last frame
            s.scripts->DispatchUIEvents(world, s.gameUI->Events());
        s.scripts->context.realDt = realDt;
        s.scripts->fire = s.Enabled("fire") ? s.fire : nullptr;
        s.scripts->Tick(world, dt, input);
    }
    if (s.game) {
        GameTickContext context;
        context.input = input;
        context.actions = s.inputActionState;
        context.audio = s.audio;
        context.physics = physicsRunning ? s.physics : nullptr;
        s.game->Tick(world, dt, context);
    }
    // Characters move as the scripts asked (character.move / jump), sweeping through the physics world; then the
    // physics steps (their capsules and kinematic bodies follow, teleports apply).
    scene::UpdateWorldTransforms(world);
    if (s.Enabled("characters")) {
        if (physicsRunning)
            TickCharacters(world, dt, *s.physics);
        else
            TickCharacters(world, dt, MeshBounds(s.renderAssets));
        scene::UpdateWorldTransforms(world);
    }
    // Soft bodies (vehicles) against the world as it is before this frame's physics step; they push rigid bodies.
    if (s.softBodies && s.Enabled("physics")) {
        s.softBodies->Tick(world, dt, physicsRunning ? s.physics : nullptr);
        scene::UpdateWorldTransforms(world);
    }
    if (physicsRunning) {
        s.physics->Tick(world, dt);
        scene::UpdateWorldTransforms(world);
        // Collision / trigger callbacks of this frame's steps.
        if (s.scripts && s.Enabled("scripts"))
            s.scripts->DispatchPhysicsEvents(world, s.physics->Events());
        if (s.game)
            s.game->DispatchPhysicsEvents(s.physics->Events());
        if (s.audio)
            s.audio->PlayImpacts(world, s.physics->Events()); // Impact Sound components
    }
    // Fire spreads and burns (before the particles tick: it emits flames and smoke into them).
    if (s.fire && s.particles && s.Enabled("fire")) {
        s.fire->physics = physicsRunning ? s.physics : nullptr;
        s.fire->Tick(world, dt, *s.particles);
    }
    // UI click sounds.
    if (s.gameUI) {
        if (s.audio)
            for (const UIEvent& ev : s.gameUI->Events())
                if (const UIButton* b = world.IsAlive(ev.entity) ? world.Get<const UIButton>(ev.entity) : nullptr;
                    b && ev.type == UIEvent::Type::Click && !b->clickSound.Empty())
                    s.audio->PlayOneShot(b->clickSound, 0.6f, false, Vec3(0.0f));
        s.gameUI->ClearEvents();
    }
    if (s.audio && tickAudio)
        s.audio->Tick(world, realDt, listener);
}

void EndGame(GameSystems& s)
{
    if (s.scripts)
        s.scripts->End();
    if (s.game)
        s.game->End();
    if (s.audio)
        s.audio->End();
    if (s.physics)
        s.physics->End();
    if (s.fire)
        s.fire->Clear();
    if (s.softBodies)
        s.softBodies->Clear();
    if (s.gameUI)
        s.gameUI->Reset();
}

ListenerPose GameListener(ecs::World& world, const ListenerPose& fallback)
{
    ListenerPose pose = fallback;
    if (const scene::WorldTransform* wt = world.Get<const scene::WorldTransform>(FindGameCamera(world))) {
        pose.position = wt->GetPosition();
        pose.forward = SafeNormalize(wt->matrix.TransformVector(Vec3::Forward()), Vec3::Forward());
        pose.up = SafeNormalize(wt->matrix.TransformVector(Vec3::Up()), Vec3::Up());
    }
    return pose;
}

} // namespace zx
