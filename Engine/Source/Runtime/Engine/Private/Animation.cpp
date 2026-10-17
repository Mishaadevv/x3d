#include "Engine/Animation.h"
#include "Core/FileSystem/FileSystem.h"
#include <cmath>
#include "Core/Log/Log.h"
#include "Core/Profiler/Profiler.h"
#include "Core/String/Path.h"
#include "Engine/Gameplay.h"
#include "Scene/Hierarchy.h"

namespace zx {

ZX_DEFINE_LOG_CATEGORY_STATIC(LogAnimation, Info);

ZX_REFLECT_STRUCT_BEGIN(Animator)
    ZX_FIELD(clip, Meta::AssetType("Animation"), Meta::Tooltip("The clip that plays without a graph (one of the model's clips)"))
    ZX_FIELD(graph, Meta::Tooltip("AnimGraph (.zanimgraph, Content-relative) that picks and blends the clips from parameters, "
                                  "e.g. StarterContent/Characters/Locomotion.zanimgraph; empty = play Clip"))
    ZX_FIELD(playing)
    ZX_FIELD(loop)
    ZX_FIELD(previewInEditor, Meta::Tooltip("Animate while editing; off = bind pose until Play"))
    ZX_FIELD(speed, Meta::Range(0, 4))
    ZX_FIELD(blendTime, Meta::Range(0, 2), Meta::Units("s"), Meta::Tooltip("Cross-fade when the clip changes"))
    ZX_FIELD(skeleton, Meta::AssetType("Skeleton"), Meta::ReadOnly())
    ZX_FIELD(clips, Meta::AssetType("Animation"), Meta::ReadOnly(), Meta::Tooltip("Clips imported with the model"))
    ZX_FIELD(footIK, Meta::Category("Inverse Kinematics"), Meta::Tooltip("Feet follow slopes and steps while playing (grounded Character Controller)"))
    ZX_FIELD(footReach, Meta::Range(0, 2), Meta::Units("m"), Meta::Tooltip("How far the feet may move up / down"))
    ZX_FIELD(leftFoot, Meta::Tooltip("Foot bone; its parent and grandparent are the knee and the hip"))
    ZX_FIELD(rightFoot)
    ZX_FIELD(headBone, Meta::Tooltip("Turned towards entity.lookAt targets"))
    ZX_FIELD(footstepEvents, Meta::Tooltip("Scripts get onAnimEvent(\"Footstep\") when a foot touches down while moving"))
    ZX_FIELD(skinToNode, Meta::Hidden())
ZX_REFLECT_STRUCT_END()

namespace {

String Lower(StringView s)
{
    String out(s);
    for (usize i = 0; i < out.Size(); ++i)
        if (out[i] >= 'A' && out[i] <= 'Z')
            out[i] = char(out[i] - 'A' + 'a');
    return out;
}

f32 Smooth(f32 x) { return x * x * (3.0f - 2.0f * x); }

/// Character Controller state of the entity or the nearest ancestor that has one (the mesh is usually a child).
const CharacterState* FindCharacter(ecs::World& world, ecs::Entity e)
{
    for (u32 depth = 0; depth < 6 && world.IsAlive(e); ++depth) {
        if (const CharacterState* st = world.Get<const CharacterState>(e))
            return st;
        const scene::Parent* p = world.Get<const scene::Parent>(e);
        if (!p)
            break;
        e = p->entity;
    }
    return nullptr;
}

} // namespace

const AnimationSystem::GraphFile* AnimationSystem::LoadGraph(StringView path)
{
    if (GraphFile* g = m_graphs.Find(String(path)))
        return g->ok ? g : nullptr;
    GraphFile& g = m_graphs.InsertOrAssign(String(path), GraphFile{});
    String text, error;
    const String full = contentRoot.Empty() ? String(path) : Path::Join(contentRoot, path);
    if (!fs::ReadTextFile(full, text))
        ZX_LOG_WARN(LogAnimation, "AnimGraph not found: {}", full);
    else if (!ParseAnimGraph(text.View(), g.graph, &error))
        ZX_LOG_ERROR(LogAnimation, "{}: {}", path, error);
    else
        g.ok = true;
    return g.ok ? &g : nullptr;
}

const asset::AnimationClip* AnimationSystem::ClipByName(const Animator& a, StringView name)
{
    for (const asset::AssetId& id : a.clips)
        if (m_assets.GetName(id).View().EqualsIgnoreCase(name)) {
            const asset::AnimationClip* clip = m_assets.GetAnimation(id);
            if (!clip && m_assets.GetState(id) == asset::AssetState::Unknown)
                m_assets.Load(id);
            return clip;
        }
    return nullptr;
}

f32 AnimationSystem::ParamValue(const State& s, const AnimGraph& g, StringView name) const
{
    if (const f32* v = s.params.Find(Lower(name)))
        return *v;
    const i32 p = g.FindParam(name);
    return p >= 0 ? g.params[usize(p)].defaultValue : 0.0f;
}

f32 AnimationSystem::EvaluateState(const AnimGraph& g, i32 stateIndex, f32 phase, const Animator& a, const asset::Skeleton& skeleton, State& s,
                                   Span<Transform> out)
{
    const AnimGraph::State& st = g.states[usize(stateIndex)];
    const u32 bones = skeleton.BoneCount();
    auto bindPose = [&] {
        for (u32 b = 0; b < bones; ++b)
            out[b] = skeleton.bindPose[b];
    };
    if (st.blend.Empty()) {
        const asset::AnimationClip* clip = ClipByName(a, st.clip);
        if (!clip) {
            bindPose();
            return 1.0f;
        }
        asset::anim::SampleClip(*clip, skeleton, phase * clip->duration, st.loop, out);
        return Max(clip->duration, 1e-3f);
    }
    // 1D blend space: the two samples around the parameter, both at the same phase (their cycles stay in step).
    const f32 v = ParamValue(s, g, st.blendParam);
    usize i = 0;
    while (i + 1 < st.blend.Size() && st.blend[i + 1].value <= v)
        ++i;
    const usize j = Min(i + 1, st.blend.Size() - 1);
    const f32 span = st.blend[j].value - st.blend[i].value;
    const f32 w = j != i && span > 1e-6f ? Clamp((v - st.blend[i].value) / span, 0.0f, 1.0f) : 0.0f;
    const asset::AnimationClip* c0 = ClipByName(a, st.blend[i].clip);
    const asset::AnimationClip* c1 = ClipByName(a, st.blend[j].clip);
    if (!c0 || !c1) {
        bindPose();
        return 1.0f;
    }
    asset::anim::SampleClip(*c0, skeleton, phase * c0->duration, st.loop, out);
    if (w > 0.0f) {
        s.scratch.Resize(bones);
        asset::anim::SampleClip(*c1, skeleton, phase * c1->duration, st.loop, s.scratch.View());
        asset::anim::BlendPoses(out, s.scratch.View(), w, out);
    }
    return Max(c0->duration + (c1->duration - c0->duration) * w, 1e-3f);
}

void AnimationSystem::TickGraph(const AnimGraph& g, const Animator& a, const asset::Skeleton& skeleton, State& s, f32 dt, bool animate,
                                ecs::Entity e)
{
    const u32 bones = skeleton.BoneCount();
    if (s.graphState < 0 || s.graphState >= i32(g.states.Size())) {
        s.graphState = Clamp(g.entry, 0, i32(g.states.Size()) - 1);
        s.phase = 0.0f;
        s.fade = 1.0f;
    }
    if (!animate) {
        s.graphState = Clamp(g.entry, 0, i32(g.states.Size()) - 1);
        s.phase = 0.0f;
        s.fade = 1.0f;
        for (u32 b = 0; b < bones; ++b)
            s.pose[b] = skeleton.bindPose[b];
        return;
    }
    // Transitions (any-state ones too): the first whose conditions all hold fires; triggers it tests are consumed.
    for (const AnimGraph::Transition& t : g.transitions) {
        if ((t.from >= 0 && t.from != s.graphState) || t.to == s.graphState)
            continue;
        if (t.exitTime >= 0.0f && s.phase < t.exitTime)
            continue;
        bool pass = true;
        for (const AnimGraph::Condition& c : t.conditions) {
            const f32 v = ParamValue(s, g, c.param);
            switch (c.op) {
            case AnimGraph::Op::Greater: pass &= v > c.value; break;
            case AnimGraph::Op::Less: pass &= v < c.value; break;
            case AnimGraph::Op::IsTrue:
            case AnimGraph::Op::Triggered: pass &= v > 0.5f; break;
            case AnimGraph::Op::IsFalse: pass &= v <= 0.5f; break;
            }
        }
        if (!pass)
            continue;
        for (const AnimGraph::Condition& c : t.conditions)
            if (c.op == AnimGraph::Op::Triggered)
                s.params.InsertOrAssign(Lower(c.param), 0.0f);
        s.fromState = s.graphState;
        s.fromPhase = s.phase;
        s.graphState = t.to;
        s.phase = 0.0f;
        s.fade = t.duration > 0.0f ? 0.0f : 1.0f;
        s.fadeDuration = t.duration;
        break;
    }
    // Pose of the current state, advanced by its (blended) length; the previous one keeps playing while fading out.
    const AnimGraph::State& cur = g.states[usize(s.graphState)];
    const f32 length = EvaluateState(g, s.graphState, s.phase, a, skeleton, s, s.pose.View());
    const f32 before = s.phase;
    s.phase += dt * a.speed * cur.speed / length;
    const bool wrapped = cur.loop && s.phase >= 1.0f;
    s.phase = cur.loop ? s.phase - Floor(s.phase) : Min(s.phase, 1.0f);
    for (const AnimGraph::Event& ev : cur.events) // authored events the phase passed this tick
        if (wrapped ? (ev.time > before || ev.time <= s.phase) : (ev.time > before && ev.time <= s.phase))
            m_events.PushBack(AnimEvent{e, ev.name});
    if (s.fade < 1.0f && s.fromState >= 0) {
        s.fromPose.Resize(bones);
        const AnimGraph::State& from = g.states[usize(s.fromState)];
        const f32 fromLength = EvaluateState(g, s.fromState, s.fromPhase, a, skeleton, s, s.fromPose.View());
        s.fromPhase += dt * a.speed * from.speed / fromLength;
        s.fromPhase = from.loop ? s.fromPhase - Floor(s.fromPhase) : Min(s.fromPhase, 1.0f);
        asset::anim::BlendPoses(s.fromPose.View(), s.pose.View(), Smooth(s.fade), s.pose.View());
        s.fade = Min(1.0f, s.fade + dt / Max(s.fadeDuration, 1e-3f));
    }
    // Triggers nobody consumed this tick expire.
    for (const AnimGraph::Param& p : g.params)
        if (p.type == AnimGraph::ParamType::Trigger)
            s.params.InsertOrAssign(Lower(p.name), 0.0f);
}

void AnimationSystem::Tick(ecs::World& world, f32 dt, bool playing)
{
    ZX_PROFILE_SCOPE("AnimationSystem::Tick");
    dt = Clamp(dt, 0.0f, 0.1f);
    if (playing != m_wasPlaying) { // Play start / stop: every animator restarts from the scene state
        Clear();
        m_wasPlaying = playing;
    }
    m_events.Clear();
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

        const GraphFile* graph = a.graph.Empty() ? nullptr : LoadGraph(a.graph.View());
        if (graph) {
            if (s.graphPath != a.graph) {
                s.graphPath = a.graph;
                s.graphState = -1;
            }
            // A Character Controller (here or on an ancestor) drives the locomotion parameters.
            if (const CharacterState* ch = FindCharacter(world, e); ch && playing) {
                const f32 horizontal = Length(Vec3(ch->velocity.x, 0.0f, ch->velocity.z));
                s.params.InsertOrAssign("speed", horizontal);
                s.params.InsertOrAssign("verticalspeed", ch->velocity.y);
                // Short airborne moments (stepping down a stair) are not a jump; a jump leaves at once.
                f32& air = *s.params.Insert("_airtime", 0.0f).first;
                air = ch->grounded ? 0.0f : air + dt;
                s.params.InsertOrAssign("grounded", (ch->grounded || (air < 0.15f && ch->velocity.y < 1.0f)) ? 1.0f : 0.0f);
            }
            TickGraph(graph->graph, a, *skeleton, s, dt, animate, e);
        } else {
            // Single clip; changing it cross-fades from where the old one was.
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
            if (s.fade < 1.0f && animate)
                if (const asset::AnimationClip* from = m_assets.GetAnimation(s.fromClip)) {
                    s.fromPose.Resize(bones);
                    asset::anim::SampleClip(*from, *skeleton, s.fromTime, a.loop, s.fromPose.View());
                    asset::anim::BlendPoses(s.fromPose.View(), s.pose.View(), Smooth(s.fade), s.pose.View());
                }
        }
        if (animate)
            ApplyIK(world, e, a, *skeleton, s, dt, playing);
        asset::anim::LocalToModel(*skeleton, s.pose.View(), s.model.View());
        asset::anim::SkinningMatrices(*skeleton, s.model.View(), a.skinToNode.ToMatrix(), s.palette.View());
    });
    m_states.RemoveIf([](u64, const State& s) { return !s.alive; });
}

void AnimationSystem::ApplyIK(ecs::World& world, ecs::Entity e, const Animator& a, const asset::Skeleton& skeleton, State& s, f32 dt,
                              bool playing)
{
    using namespace asset::anim;
    const scene::WorldTransform* wt = world.Get<const scene::WorldTransform>(e);
    if (!wt)
        return;
    // Rig bones (once per skeleton).
    if (s.rigSkeleton != a.skeleton) {
        s.rigSkeleton = a.skeleton;
        for (u32 side = 0; side < 2; ++side) {
            s.foot[side] = skeleton.FindBone(side == 0 ? a.leftFoot.View() : a.rightFoot.View());
            s.knee[side] = s.foot[side] >= 0 ? skeleton.parents[u32(s.foot[side])] : -1;
            s.hip[side] = s.knee[side] >= 0 ? skeleton.parents[u32(s.knee[side])] : -1;
            if (s.hip[side] < 0)
                s.foot[side] = -1;
        }
        s.head = skeleton.FindBone(a.headBone.View());
        s.pelvis = -1;
        for (u32 b = 0; b < skeleton.BoneCount() && s.pelvis < 0; ++b)
            if (skeleton.parents[b] < 0)
                s.pelvis = i32(b);
    }
    const Mat4 skelToWorld = wt->matrix * a.skinToNode.ToMatrix();
    const Mat4 worldToSkel = Inverse(skelToWorld);
    const Vec3 upWorld(0, 1, 0);
    const Vec3 upSkel = SafeNormalize(worldToSkel.TransformVector(upWorld), Vec3(0, 1, 0));
    const f32 rate = 1.0f - std::exp(-dt * 14.0f); // smoothing towards the targets

    // --- Foot IK and footsteps -------------------------------------------------------------------------------------
    ecs::Entity owner; // the entity with the Character Controller (the ground query ignores it)
    const CharacterState* ch = nullptr;
    for (ecs::Entity x = e; world.IsAlive(x) && !ch;) {
        ch = world.Get<const CharacterState>(x);
        if (ch)
            owner = x;
        const scene::Parent* p = world.Get<const scene::Parent>(x);
        if (!p)
            break;
        x = p->entity;
    }
    const bool legs = s.foot[0] >= 0 && s.foot[1] >= 0 && s.pelvis >= 0;
    const bool footActive = legs && a.footIK && playing && raycast && ch && ch->grounded;
    s.footIKWeight += ((footActive ? 1.0f : 0.0f) - s.footIKWeight) * rate;
    if (legs && (s.footIKWeight > 1e-3f || (a.footstepEvents && ch))) {
        s.model.Resize(skeleton.BoneCount());
        LocalToModel(skeleton, s.pose.View(), s.model.View());
        const Vec3 rootWorld = wt->matrix.GetTranslation(); // the character's feet
        Vec3 footWorld[2];
        for (u32 side = 0; side < 2; ++side)
            footWorld[side] = skelToWorld.TransformPoint(s.model[u32(s.foot[side])].GetTranslation());
        // Footsteps: a foot that comes down near the ground while the character moves.
        if (a.footstepEvents && ch && ch->grounded) {
            const f32 speed = Length(Vec3(ch->velocity.x, 0.0f, ch->velocity.z));
            for (u32 side = 0; side < 2; ++side) {
                const f32 height = footWorld[side].y - rootWorld.y;
                const bool down = height < 0.12f;
                if (down && !s.footDown[side] && speed > 0.5f)
                    m_events.PushBack(AnimEvent{e, String("Footstep")});
                s.footDown[side] = down ? true : (height > 0.18f ? false : s.footDown[side]);
            }
        }
        if (s.footIKWeight > 1e-3f) {
            // Ground under each foot relative to the character's feet (flat ground = 0).
            for (u32 side = 0; side < 2; ++side) {
                f32 offset = 0.0f;
                Vec3 normal = upWorld;
                if (footActive) {
                    const Vec3 from(footWorld[side].x, rootWorld.y + a.footReach, footWorld[side].z);
                    const Vec3 to(footWorld[side].x, rootWorld.y - a.footReach, footWorld[side].z);
                    Vec3 point, n;
                    if (raycast(from, to, owner, point, n) && n.y > 0.5f) {
                        offset = Clamp(point.y - rootWorld.y, -a.footReach, a.footReach);
                        normal = n;
                    }
                }
                s.footOffset[side] += (offset - s.footOffset[side]) * rate;
                s.footNormal[side] = SafeNormalize(Lerp(s.footNormal[side], normal, rate), upWorld);
            }
            // Hips drop so the lower foot can reach down; the other leg bends.
            const f32 pelvisTarget = Min(Min(s.footOffset[0], s.footOffset[1]), 0.0f);
            s.pelvisOffset += (pelvisTarget - s.pelvisOffset) * rate;
            const f32 w = s.footIKWeight;
            s.pose[u32(s.pelvis)].position += worldToSkel.TransformVector(upWorld * (s.pelvisOffset * w));
            LocalToModel(skeleton, s.pose.View(), s.model.View());
            const Vec3 bendHint = SafeNormalize(worldToSkel.TransformVector(wt->matrix.TransformVector(Vec3(0, 0, 1))), Vec3(0, 0, 1));
            for (u32 side = 0; side < 2; ++side) {
                const Vec3 current = skelToWorld.TransformPoint(s.model[u32(s.foot[side])].GetTranslation());
                const Vec3 targetWorld = current + upWorld * ((s.footOffset[side] - s.pelvisOffset) * w);
                SolveTwoBoneIK(skeleton, s.pose.View(), u32(s.hip[side]), u32(s.knee[side]), u32(s.foot[side]),
                               worldToSkel.TransformPoint(targetWorld), bendHint);
                // Sole on the slope.
                const Vec3 n = SafeNormalize(worldToSkel.TransformVector(s.footNormal[side]), upSkel);
                RotateGlobal(skeleton, s.pose.View(), u32(s.foot[side]), Slerp(Quat::Identity(), Quat::FromTo(upSkel, n), w));
            }
        }
    }

    // --- Look-at ----------------------------------------------------------------------------------------------------
    s.lookWeight += (s.lookTargetWeight - s.lookWeight) * (1.0f - std::exp(-dt * 6.0f));
    if (s.head >= 0 && s.lookWeight > 1e-3f) {
        s.model.Resize(skeleton.BoneCount());
        LocalToModel(skeleton, s.pose.View(), s.model.View());
        const Vec3 headPos = s.model[u32(s.head)].GetTranslation();
        const Vec3 toTarget = SafeNormalize(worldToSkel.TransformPoint(s.lookTarget) - headPos, Vec3(0, 0, 1));
        // The face looks where the body faces, turned by how the animation moved the head away from its bind pose.
        Array<Transform> bind;
        bind.Resize(skeleton.BoneCount());
        for (u32 b = 0; b < skeleton.BoneCount(); ++b)
            bind[b] = skeleton.bindPose[b];
        const Quat animated = GlobalRotation(skeleton, s.pose.View(), u32(s.head)) * Inverse(GlobalRotation(skeleton, bind.View(), u32(s.head)));
        const Vec3 bodyForward = SafeNormalize(worldToSkel.TransformVector(wt->matrix.TransformVector(Vec3(0, 0, 1))), Vec3(0, 0, 1));
        const Vec3 facing = SafeNormalize(animated * bodyForward, bodyForward);
        // At most 70 degrees from where the head faces.
        const f32 limit = DegToRad(70.0f);
        const f32 angle = std::acos(Clamp(Dot(facing, toTarget), -1.0f, 1.0f));
        const Quat full = Quat::FromTo(facing, toTarget);
        const Quat turn = angle > limit ? Slerp(Quat::Identity(), full, limit / angle) : full;
        RotateGlobal(skeleton, s.pose.View(), u32(s.head), Slerp(Quat::Identity(), turn, s.lookWeight));
    }
}

void AnimationSystem::SetTime(ecs::Entity e, f32 seconds)
{
    State& s = *m_states.Insert(e.Pack(), State{}).first;
    s.time = Max(seconds, 0.0f);
    s.fade = 1.0f;
}

void AnimationSystem::ReloadGraph(StringView path)
{
    m_graphs.RemoveIf([&](const String& key, const GraphFile&) { return key.View().EqualsIgnoreCase(path); });
    for (auto& [key, s] : m_states)
        if (s.graphPath.View().EqualsIgnoreCase(path))
            s.graphState = -1;
}

void AnimationSystem::SetLookAt(ecs::Entity e, Vec3 target, f32 weight)
{
    State& s = *m_states.Insert(e.Pack(), State{}).first;
    s.alive = true;
    s.lookTarget = target;
    s.lookTargetWeight = Clamp(weight, 0.0f, 1.0f);
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

void AnimationSystem::SetParam(ecs::Entity e, StringView name, f32 value)
{
    State& s = *m_states.Insert(e.Pack(), State{}).first;
    s.alive = true;
    s.params.InsertOrAssign(Lower(name), value);
}

f32 AnimationSystem::GetParam(ecs::Entity e, StringView name) const
{
    const State* s = m_states.Find(e.Pack());
    const f32* v = s ? s->params.Find(Lower(name)) : nullptr;
    return v ? *v : 0.0f;
}

String AnimationSystem::CurrentState(ecs::Entity e) const
{
    const State* s = m_states.Find(e.Pack());
    if (!s || s->graphState < 0)
        return String();
    const GraphFile* g = m_graphs.Find(s->graphPath);
    return g && g->ok && s->graphState < i32(g->graph.states.Size()) ? g->graph.states[usize(s->graphState)].name : String();
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
