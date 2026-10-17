#pragma once

// Animator component (plays animation clips on the skinned Mesh Renderer of its entity) and the AnimationSystem that
// poses every animator once per frame:
//
//   AnimationSystem animation(assets);
//   animation.Tick(world, dt, playing);          // editor: animators with Preview In Editor run while editing too
//   Span<const Mat4> palette = animation.Palette(entity); // skinning matrices (mesh space) for the renderer
//
// Clips come with the imported model (glTF animations made for its skin). Changing Animator::clip cross-fades over
// Blend Time. Scripts and C++ switch clips by name (AnimationSystem::Play).

#include "Asset/Animation.h"
#include "Asset/AssetManager.h"
#include "Core/Containers/HashMap.h"
#include "Core/Reflection/Reflection.h"
#include "Scene/ECS/World.h"

namespace zx {

struct Animator
{
    asset::AssetId skeleton;     // the skin of the model (set on import)
    asset::AssetId clip;         // what plays: an Animation asset made for that skeleton
    Array<asset::AssetId> clips; // the clips imported with the model
    bool playing = true;
    bool loop = true;
    bool previewInEditor = true; // animate while editing (off: bind pose until Play)
    f32 speed = 1.0f;
    f32 blendTime = 0.25f;       // cross-fade when the clip changes (seconds)
    Transform skinToNode;        // skeleton space -> this entity (set on import)
};

ZX_DECLARE_TYPE(Animator);

class AnimationSystem
{
public:
    explicit AnimationSystem(asset::AssetManager& assets) : m_assets(assets) {}

    /// Advances and poses every animator: all of them while `playing`, otherwise those with Preview In Editor (the
    /// rest show the bind pose). Skeletons and clips are requested on first use.
    void Tick(ecs::World& world, f32 dt, bool playing);
    /// Skinning matrices of the entity's mesh this frame (mesh space; empty until its skeleton is loaded).
    Span<const Mat4> Palette(ecs::Entity e) const;
    /// Switches the entity's clip to the one named `clip` among Animator::clips (cross-fading). False if unknown.
    bool Play(ecs::World& world, ecs::Entity e, StringView clip);
    /// Name of a clip asset ("Run"), empty while unknown.
    String ClipName(asset::AssetId clip) const { return m_assets.GetName(clip); }
    /// Seconds into the current clip.
    f32 Time(ecs::Entity e) const;
    /// Drops all runtime state (Play start / stop, scene change).
    void Clear() { m_states.Clear(); }
    u32 AnimatorCount() const { return u32(m_states.Size()); }

private:
    struct State
    {
        asset::AssetId clip;
        f32 time = 0.0f;
        asset::AssetId fromClip; // cross-fade source
        f32 fromTime = 0.0f;
        f32 fade = 1.0f;         // 1 = fully on `clip`
        f32 fadeDuration = 0.0f;
        Array<Transform> pose, fromPose;
        Array<Mat4> model, palette;
        bool alive = false;
    };
    asset::AssetManager& m_assets;
    HashMap<u64, State> m_states; // entity -> state
    bool m_wasPlaying = false;
};

} // namespace zx
