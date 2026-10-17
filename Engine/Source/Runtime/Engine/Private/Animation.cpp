#include "Engine/Animation.h"
#include "Core/Profiler/Profiler.h"

namespace zx {

ZX_REFLECT_STRUCT_BEGIN(Animator)
    ZX_FIELD(clip, Meta::AssetType("Animation"), Meta::Tooltip("The clip that plays (one of the model's clips)"))
    ZX_FIELD(playing)
    ZX_FIELD(loop)
    ZX_FIELD(previewInEditor, Meta::Tooltip("Animate while editing; off = bind pose until Play"))
    ZX_FIELD(speed, Meta::Range(0, 4))
    ZX_FIELD(blendTime, Meta::Range(0, 2), Meta::Units("s"), Meta::Tooltip("Cross-fade when the clip changes"))
    ZX_FIELD(skeleton, Meta::AssetType("Skeleton"), Meta::ReadOnly())
    ZX_FIELD(clips, Meta::AssetType("Animation"), Meta::ReadOnly(), Meta::Tooltip("Clips imported with the model"))
    ZX_FIELD(skinToNode, Meta::Hidden())
ZX_REFLECT_STRUCT_END()

void AnimationSystem::Tick(ecs::World& world, f32 dt, bool playing)
{
    ZX_PROFILE_SCOPE("AnimationSystem::Tick");
    dt = Clamp(dt, 0.0f, 0.1f);
    if (playing != m_wasPlaying) { // Play start / stop: every animator restarts from the scene state
        m_states.Clear();
        m_wasPlaying = playing;
    }
    for (auto& [key, s] : m_states)
        s.alive = false;
    world.Query<const Animator>().Each([&](ecs::Entity e, const Animator& a) {
        State& s = *m_states.Insert(e.Pack(), State{}).first;
        s.alive = true;
        const asset::Skeleton* skeleton = a.skeleton.IsValid() ? m_assets.GetSkeleton(a.skeleton) : nullptr;
        if (!skeleton) {
            if (a.skeleton.IsValid() && m_assets.GetState(a.skeleton) == asset::AssetState::Unknown)
                m_assets.Load(a.skeleton);
            s.palette.Clear();
            return;
        }
        const u32 bones = skeleton->BoneCount();
        s.pose.Resize(bones);
        s.model.Resize(bones);
        s.palette.Resize(bones);
        const bool animate = playing || a.previewInEditor;
        // Clip change: cross-fade from where the old one was.
        if (a.clip != s.clip) {
            if (s.clip.IsValid() && a.blendTime > 0.0f && animate) {
                s.fromClip = s.clip;
                s.fromTime = s.time;
                s.fade = 0.0f;
                s.fadeDuration = a.blendTime;
            } else {
                s.fade = 1.0f;
            }
            s.clip = a.clip;
            s.time = 0.0f;
        }
        const asset::AnimationClip* clip = nullptr;
        if (s.clip.IsValid()) {
            clip = m_assets.GetAnimation(s.clip);
            if (!clip && m_assets.GetState(s.clip) == asset::AssetState::Unknown)
                m_assets.Load(s.clip);
        }
        if (animate && a.playing) {
            const f32 step = dt * a.speed;
            s.time += step;
            s.fromTime += step;
            if (s.fade < 1.0f)
                s.fade = Min(1.0f, s.fade + dt / Max(s.fadeDuration, 1e-3f));
        }
        if (!animate) {
            s.time = 0.0f;
            s.fade = 1.0f;
        }
        if (clip && animate)
            asset::anim::SampleClip(*clip, *skeleton, s.time, a.loop, s.pose.View());
        else
            for (u32 b = 0; b < bones; ++b)
                s.pose[b] = skeleton->bindPose[b];
        if (s.fade < 1.0f && animate) {
            if (const asset::AnimationClip* from = m_assets.GetAnimation(s.fromClip)) {
                s.fromPose.Resize(bones);
                asset::anim::SampleClip(*from, *skeleton, s.fromTime, a.loop, s.fromPose.View());
                // Smoothstep weight: no velocity jump at either end of the fade.
                const f32 w = s.fade * s.fade * (3.0f - 2.0f * s.fade);
                asset::anim::BlendPoses(s.fromPose.View(), s.pose.View(), w, s.pose.View());
            }
        }
        asset::anim::LocalToModel(*skeleton, s.pose.View(), s.model.View());
        asset::anim::SkinningMatrices(*skeleton, s.model.View(), a.skinToNode.ToMatrix(), s.palette.View());
    });
    m_states.RemoveIf([](u64, const State& s) { return !s.alive; });
}

Span<const Mat4> AnimationSystem::Palette(ecs::Entity e) const
{
    const State* s = m_states.Find(e.Pack());
    return s ? s->palette.View() : Span<const Mat4>();
}

f32 AnimationSystem::Time(ecs::Entity e) const
{
    const State* s = m_states.Find(e.Pack());
    return s ? s->time : 0.0f;
}

bool AnimationSystem::Play(ecs::World& world, ecs::Entity e, StringView clip)
{
    Animator* a = world.Get<Animator>(e);
    if (!a)
        return false;
    for (const asset::AssetId& id : a->clips)
        if (m_assets.GetName(id).View().EqualsIgnoreCase(clip)) {
            a->clip = id;
            return true;
        }
    return false;
}

} // namespace zx
