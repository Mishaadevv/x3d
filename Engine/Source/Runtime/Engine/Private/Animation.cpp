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
    ZX_FIELD(rootMotion, Meta::Category("Root Motion"),
             Meta::Tooltip("While playing, the root bone's horizontal movement in the clips moves the character (Character Controller "
                           "or the top-level entity); the model stays on the entity"))
    ZX_FIELD(rootBone, Meta::Tooltip("Bone that carries the motion; empty = the skeleton's root"))
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

/// The part of `v` across `up` (root motion moves along the ground; falls and jumps stay in the animation).
Vec3 Horizontal(Vec3 v, Vec3 up) { return v - up * Dot(v, up); }

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

bool AnimationSystem::StateClips(const AnimGraph& g, const AnimStateMachine::State& st, const Animator& a, State& s, Array<ClipWeight>& out)
{
    out.Clear();
    if (!st.HasMotion())
        return false;
    if (st.blend.Empty()) {
        const asset::AnimationClip* clip = ClipByName(a, st.clip);
        if (clip)
            out.PushBack(ClipWeight{clip, 1.0f});
        return clip != nullptr;
    }
    if (st.Is2D()) {
        // 2D blend space: every sample with weight.
        BlendSpace2DWeights(st.blend.View(), ParamValue(s, g, st.blendParam), ParamValue(s, g, st.blendParamY), s.weights);
        f32 total = 0.0f;
        for (usize i = 0; i < st.blend.Size(); ++i)
            if (s.weights[i] > 1e-3f)
                if (const asset::AnimationClip* clip = ClipByName(a, st.blend[i].clip)) {
                    out.PushBack(ClipWeight{clip, s.weights[i]});
                    total += s.weights[i];
                }
        for (ClipWeight& c : out)
            c.weight /= Max(total, 1e-6f);
        return !out.Empty();
    }
    // 1D blend space: the two samples around the parameter.
    const f32 v = ParamValue(s, g, st.blendParam);
    usize i = 0;
    while (i + 1 < st.blend.Size() && st.blend[i + 1].value <= v)
        ++i;
    const usize j = Min(i + 1, st.blend.Size() - 1);
    const f32 span = st.blend[j].value - st.blend[i].value;
    const f32 w = j != i && span > 1e-6f ? Clamp((v - st.blend[i].value) / span, 0.0f, 1.0f) : 0.0f;
    const asset::AnimationClip* c0 = ClipByName(a, st.blend[i].clip);
    const asset::AnimationClip* c1 = ClipByName(a, st.blend[j].clip);
    if (!c0 || !c1)
        return false;
    out.PushBack(ClipWeight{c0, 1.0f - w});
    if (w > 0.0f)
        out.PushBack(ClipWeight{c1, w});
    return true;
}

void AnimationSystem::StateMorphs(const AnimGraph& g, const AnimStateMachine& m, i32 stateIndex, f32 phase, const Animator& a, State& s,
                                  Array<f32>& out)
{
    out.Clear();
    const AnimStateMachine::State& st = m.states[usize(stateIndex)];
    Array<ClipWeight> clips;
    if (!StateClips(g, st, a, s, clips))
        return;
    f32 sample[64];
    for (const ClipWeight& c : clips) {
        if (c.clip->morphTracks.Empty())
            continue;
        const asset::MorphTrack& t = c.clip->morphTracks[0];
        const u32 n = Min(t.targets, 64u);
        if (out.Size() < n)
            out.Resize(n, 0.0f);
        asset::anim::SampleMorphWeights(t, c.clip->duration, phase * c.clip->duration, st.loop, Span<f32>(sample, n));
        for (u32 i = 0; i < n; ++i)
            out[i] += sample[i] * c.weight;
    }
}

void AnimationSystem::StateRootMotion(const AnimGraph& g, const AnimStateMachine& m, i32 stateIndex, f32 from, f32 to, const Animator& a,
                                      const asset::Skeleton& skeleton, State& s, Vec3& offset, Vec3& delta)
{
    offset = delta = Vec3();
    const AnimStateMachine::State& st = m.states[usize(stateIndex)];
    Array<ClipWeight> clips;
    if (s.rootBone < 0 || !StateClips(g, st, a, s, clips))
        return;
    const u32 root = u32(s.rootBone);
    for (const ClipWeight& c : clips) {
        const f32 d = c.clip->duration;
        offset += Horizontal(asset::anim::SampleBonePosition(*c.clip, skeleton, root, from * d) -
                                 asset::anim::SampleBonePosition(*c.clip, skeleton, root, 0.0f),
                             s.rootUp) *
                  c.weight;
        delta += Horizontal(asset::anim::RootMotionDelta(*c.clip, skeleton, root, from * d, to * d, st.loop), s.rootUp) * c.weight;
    }
}

f32 AnimationSystem::EvaluateState(const AnimGraph& g, const AnimStateMachine& m, i32 stateIndex, f32 phase, bool additive, const Animator& a,
                                   const asset::Skeleton& skeleton, State& s, Span<Transform> out)
{
    const AnimStateMachine::State& st = m.states[usize(stateIndex)];
    const u32 bones = skeleton.BoneCount();
    // The clips at the same phase (cycles stay in step), blended by weight.
    Array<ClipWeight> clips;
    f32 length = 1.0f;
    if (!StateClips(g, st, a, s, clips)) {
        for (u32 b = 0; b < bones; ++b)
            out[b] = skeleton.bindPose[b];
    } else {
        f32 total = 0.0f, blendedLength = 0.0f;
        s.scratch.Resize(bones);
        for (const ClipWeight& c : clips) {
            total += c.weight;
            blendedLength += c.weight * c.clip->duration;
            if (total == c.weight) {
                asset::anim::SampleClip(*c.clip, skeleton, phase * c.clip->duration, st.loop, out);
            } else {
                asset::anim::SampleClip(*c.clip, skeleton, phase * c.clip->duration, st.loop, s.scratch.View());
                asset::anim::BlendPoses(out, s.scratch.View(), c.weight / Max(total, 1e-6f), out);
            }
        }
        length = Max(blendedLength / Max(total, 1e-6f), 1e-3f);
    }
    if (additive && st.HasMotion()) {
        // The change from the state's first frame (local space): pose = delta * reference.
        s.refPose.Resize(bones);
        EvaluateState(g, m, stateIndex, 0.0f, false, a, skeleton, s, s.refPose.View());
        for (u32 b = 0; b < bones; ++b)
            out[b] = asset::anim::AdditiveDelta(s.refPose[b], out[b]);
    } else if (additive) {
        for (u32 b = 0; b < bones; ++b)
            out[b] = Transform();
    }
    return length;
}

f32 AnimationSystem::TickMachine(const AnimGraph& g, const AnimStateMachine& m, bool additive, MachineRun& r, const Animator& a,
                                 const asset::Skeleton& skeleton, State& s, f32 dt, ecs::Entity e, Span<Transform> out, bool baseLayer)
{
    const bool rootMotion = baseLayer && s.rootBone >= 0;
    const u32 bones = skeleton.BoneCount();
    if (r.state < 0 || r.state >= i32(m.states.Size())) {
        r = MachineRun{};
        r.state = Clamp(m.entry, 0, i32(m.states.Size()) - 1);
    }
    // Transitions (any-state ones too): the first whose conditions all hold fires; triggers it tests are consumed.
    for (const AnimStateMachine::Transition& t : m.transitions) {
        if ((t.from >= 0 && t.from != r.state) || t.to == r.state)
            continue;
        if (t.exitTime >= 0.0f && r.phase < t.exitTime)
            continue;
        bool pass = true;
        for (const AnimStateMachine::Condition& c : t.conditions) {
            const f32 v = ParamValue(s, g, c.param);
            switch (c.op) {
            case AnimStateMachine::Op::Greater: pass &= v > c.value; break;
            case AnimStateMachine::Op::Less: pass &= v < c.value; break;
            case AnimStateMachine::Op::IsTrue:
            case AnimStateMachine::Op::Triggered: pass &= v > 0.5f; break;
            case AnimStateMachine::Op::IsFalse: pass &= v <= 0.5f; break;
            }
        }
        if (!pass)
            continue;
        for (const AnimStateMachine::Condition& c : t.conditions)
            if (c.op == AnimStateMachine::Op::Triggered)
                s.params.InsertOrAssign(Lower(c.param), 0.0f);
        r.fromState = r.state;
        r.fromPhase = r.phase;
        r.state = t.to;
        r.phase = 0.0f;
        r.fade = t.duration > 0.0f ? 0.0f : 1.0f;
        r.fadeDuration = t.duration;
        break;
    }
    // Pose of the current state, advanced by its (blended) length; the previous one keeps playing while fading out.
    const AnimStateMachine::State& cur = m.states[usize(r.state)];
    const f32 length = EvaluateState(g, m, r.state, r.phase, additive, a, skeleton, s, out);
    const f32 before = r.phase;
    r.phase += dt * a.speed * cur.speed / length;
    Vec3 rootOffset, rootDelta;
    if (rootMotion)
        StateRootMotion(g, m, r.state, before, r.phase, a, skeleton, s, rootOffset, rootDelta);
    if (baseLayer)
        StateMorphs(g, m, r.state, before, a, s, s.morphs);
    const bool wrapped = cur.loop && r.phase >= 1.0f;
    r.phase = cur.loop ? r.phase - Floor(r.phase) : Min(r.phase, 1.0f);
    for (const AnimStateMachine::Event& ev : cur.events) // authored events the phase passed this tick
        if (wrapped ? (ev.time > before || ev.time <= r.phase) : (ev.time > before && ev.time <= r.phase))
            m_events.PushBack(AnimEvent{e, ev.name});
    f32 presence = cur.HasMotion() ? 1.0f : 0.0f;
    if (r.fade < 1.0f && r.fromState >= 0) {
        const AnimStateMachine::State& from = m.states[usize(r.fromState)];
        const f32 t = Smooth(r.fade);
        if (from.HasMotion()) {
            s.fromPose.Resize(bones);
            const f32 fromLength = EvaluateState(g, m, r.fromState, r.fromPhase, additive, a, skeleton, s, s.fromPose.View());
            const f32 fromBefore = r.fromPhase;
            r.fromPhase += dt * a.speed * from.speed / fromLength;
            if (rootMotion) { // the motion blends like the pose
                Vec3 fromOffset, fromDelta;
                StateRootMotion(g, m, r.fromState, fromBefore, r.fromPhase, a, skeleton, s, fromOffset, fromDelta);
                rootOffset = Lerp(fromOffset, rootOffset, t);
                rootDelta = Lerp(fromDelta, rootDelta, t);
            }
            if (baseLayer) { // morph weights blend like the pose
                StateMorphs(g, m, r.fromState, fromBefore, a, s, s.fromMorphs);
                const usize n = Max(s.morphs.Size(), s.fromMorphs.Size());
                s.morphs.Resize(n, 0.0f);
                s.fromMorphs.Resize(n, 0.0f);
                for (usize i = 0; i < n; ++i)
                    s.morphs[i] = Lerp(s.fromMorphs[i], s.morphs[i], t);
            }
            r.fromPhase = from.loop ? r.fromPhase - Floor(r.fromPhase) : Min(r.fromPhase, 1.0f);
            if (cur.HasMotion())
                asset::anim::BlendPoses(s.fromPose.View(), out, t, out);
            else
                for (u32 b = 0; b < bones; ++b)
                    out[b] = s.fromPose[b];
        }
        // A silent state on either side fades the layer instead of blending towards the bind pose.
        presence = cur.HasMotion() && from.HasMotion() ? 1.0f : cur.HasMotion() ? t : from.HasMotion() ? 1.0f - t : 0.0f;
        r.fade = Min(1.0f, r.fade + dt / Max(r.fadeDuration, 1e-3f));
    }
    if (rootMotion) {
        s.rootOffset = rootOffset;
        s.rootDelta = rootDelta;
    }
    return presence;
}

void AnimationSystem::TickGraph(const AnimGraph& g, const Animator& a, const asset::Skeleton& skeleton, State& s, f32 dt, bool animate,
                                ecs::Entity e)
{
    const u32 bones = skeleton.BoneCount();
    if (s.machines.Size() != g.MachineCount()) {
        s.machines.Clear();
        s.machines.Resize(g.MachineCount());
        s.maskSkeleton = {};
    }
    if (!animate) {
        for (MachineRun& r : s.machines)
            r = MachineRun{};
        for (u32 b = 0; b < bones; ++b)
            s.pose[b] = skeleton.bindPose[b];
        return;
    }
    TickMachine(g, g, false, s.machines[0], a, skeleton, s, dt, e, s.pose.View(), true);
    if (s.rootBone >= 0) // the root stays over the entity; the motion moves the entity instead
        s.pose[u32(s.rootBone)].position -= s.rootOffset;

    // Layers on top: bone masks (a bone and everything below it) resolved once per skeleton.
    if (!g.layers.Empty() && (s.maskSkeleton != a.skeleton || s.layerMasks.Size() != g.layers.Size())) {
        s.maskSkeleton = a.skeleton;
        s.layerMasks.Clear();
        s.layerMasks.Resize(g.layers.Size());
        for (usize l = 0; l < g.layers.Size(); ++l) {
            Array<u8>& mask = s.layerMasks[l];
            mask.Resize(bones);
            for (u32 b = 0; b < bones; ++b) {
                bool in = g.layers[l].mask.Empty();
                for (const String& name : g.layers[l].mask)
                    in |= skeleton.boneNames[b].View().EqualsIgnoreCase(name);
                const i32 parent = skeleton.parents[b];
                mask[b] = in || (parent >= 0 && mask[u32(parent)]) ? 1 : 0;
            }
        }
    }
    s.layerPose.Resize(bones);
    for (usize l = 0; l < g.layers.Size(); ++l) {
        const AnimGraph::Layer& layer = g.layers[l];
        const f32 presence = TickMachine(g, layer, layer.additive, s.machines[l + 1], a, skeleton, s, dt, e, s.layerPose.View());
        f32 w = layer.weight * presence;
        if (!layer.weightParam.Empty())
            w *= Clamp(ParamValue(s, g, layer.weightParam), 0.0f, 1.0f);
        if (w <= 1e-4f)
            continue;
        const Span<const u8> mask = s.layerMasks[l].View();
        for (u32 b = 0; b < bones; ++b) {
            if (!mask[b])
                continue;
            if (layer.additive)
                s.pose[b] = asset::anim::ApplyAdditive(s.pose[b], s.layerPose[b], w);
            else
                s.pose[b] = asset::anim::BlendTransforms(s.pose[b], s.layerPose[b], w);
        }
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
        s.rootBone = -1;
        s.rootOffset = s.rootDelta = Vec3();
        s.morphs.Clear();
        if (a.rootMotion) {
            s.rootBone = a.rootBone.Empty() ? -1 : skeleton->FindBone(a.rootBone.View());
            for (u32 b = 0; b < bones && s.rootBone < 0 && a.rootBone.Empty(); ++b)
                if (skeleton->parents[b] < 0)
                    s.rootBone = i32(b);
            if (s.rootBone >= 0 && skeleton->parents[u32(s.rootBone)] >= 0)
                s.rootBone = -1; // only a root bone's position is in skeleton space
            if (const scene::WorldTransform* wt = world.Get<const scene::WorldTransform>(e))
                s.rootUp = SafeNormalize(Inverse(wt->matrix * a.skinToNode.ToMatrix()).TransformVector(Vec3(0, 1, 0)), Vec3(0, 1, 0));
        }

        const GraphFile* graph = a.graph.Empty() ? nullptr : LoadGraph(a.graph.View());
        if (graph) {
            if (s.graphPath != a.graph) {
                s.graphPath = a.graph;
                s.machines.Clear();
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
            const f32 timeBefore = s.time, fromBefore = s.fromTime;
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
            if (clip && animate && s.rootBone >= 0) {
                const u32 root = u32(s.rootBone);
                const f32 wrappedTime = a.loop && clip->duration > 0.0f ? s.time - clip->duration * Floor(s.time / clip->duration) : s.time;
                s.rootOffset = Horizontal(asset::anim::SampleBonePosition(*clip, *skeleton, root, wrappedTime) -
                                              asset::anim::SampleBonePosition(*clip, *skeleton, root, 0.0f),
                                          s.rootUp);
                s.rootDelta = Horizontal(asset::anim::RootMotionDelta(*clip, *skeleton, root, timeBefore, s.time, a.loop), s.rootUp);
            }
            if (s.fade < 1.0f && animate)
                if (const asset::AnimationClip* from = m_assets.GetAnimation(s.fromClip)) {
                    s.fromPose.Resize(bones);
                    asset::anim::SampleClip(*from, *skeleton, s.fromTime, a.loop, s.fromPose.View());
                    asset::anim::BlendPoses(s.fromPose.View(), s.pose.View(), Smooth(s.fade), s.pose.View());
                    if (s.rootBone >= 0) {
                        const u32 root = u32(s.rootBone);
                        const f32 t = Smooth(s.fade);
                        const f32 wrappedFrom =
                            a.loop && from->duration > 0.0f ? s.fromTime - from->duration * Floor(s.fromTime / from->duration) : s.fromTime;
                        const Vec3 offset = Horizontal(asset::anim::SampleBonePosition(*from, *skeleton, root, wrappedFrom) -
                                                           asset::anim::SampleBonePosition(*from, *skeleton, root, 0.0f),
                                                       s.rootUp);
                        const Vec3 delta = Horizontal(asset::anim::RootMotionDelta(*from, *skeleton, root, fromBefore, s.fromTime, a.loop), s.rootUp);
                        s.rootOffset = Lerp(offset, s.rootOffset, t);
                        s.rootDelta = Lerp(delta, s.rootDelta, t);
                    }
                }
            if (s.rootBone >= 0 && animate)
                s.pose[u32(s.rootBone)].position -= s.rootOffset;
            s.morphs.Clear();
            auto clipMorphs = [&](const asset::AnimationClip* c, f32 time, f32 weight) {
                if (!c || c->morphTracks.Empty() || weight <= 0.0f)
                    return;
                const asset::MorphTrack& t = c->morphTracks[0];
                const u32 n = Min(t.targets, 64u);
                f32 sample[64];
                asset::anim::SampleMorphWeights(t, c->duration, time, a.loop, Span<f32>(sample, n));
                if (s.morphs.Size() < n)
                    s.morphs.Resize(n, 0.0f);
                for (u32 i = 0; i < n; ++i)
                    s.morphs[i] += sample[i] * weight;
            };
            if (animate) {
                const f32 t = s.fade < 1.0f ? Smooth(s.fade) : 1.0f;
                clipMorphs(clip, s.time, t);
                if (t < 1.0f)
                    clipMorphs(m_assets.GetAnimation(s.fromClip), s.fromTime, 1.0f - t);
            }
        }
        if (playing && s.rootBone >= 0)
            ApplyRootMotion(world, e, a, s);
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

void AnimationSystem::ApplyRootMotion(ecs::World& world, ecs::Entity e, const Animator& a, const State& s)
{
    const scene::WorldTransform* wt = world.Get<const scene::WorldTransform>(e);
    if (!wt || LengthSq(s.rootDelta) < 1e-14f)
        return;
    Vec3 move = (wt->matrix * a.skinToNode.ToMatrix()).TransformVector(s.rootDelta);
    move.y = 0.0f;
    // A Character Controller (here or on an ancestor) walks it, sweeping through the world; otherwise the top-level
    // entity moves.
    ecs::Entity top = e;
    for (ecs::Entity x = e; world.IsAlive(x);) {
        if (CharacterState* ch = world.Get<CharacterState>(x)) {
            ch->rootMotion += move;
            return;
        }
        top = x;
        const scene::Parent* p = world.Get<const scene::Parent>(x);
        if (!p)
            break;
        x = p->entity;
    }
    if (Transform* t = world.Get<Transform>(top))
        t->position += move;
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
            s.machines.Clear();
}

void AnimationSystem::SetLookAt(ecs::Entity e, Vec3 target, f32 weight)
{
    State& s = *m_states.Insert(e.Pack(), State{}).first;
    s.alive = true;
    s.lookTarget = target;
    s.lookTargetWeight = Clamp(weight, 0.0f, 1.0f);
}

Span<const f32> AnimationSystem::MorphWeights(ecs::Entity e) const
{
    const State* s = m_states.Find(e.Pack());
    return s ? s->morphs.View() : Span<const f32>();
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

String AnimationSystem::CurrentState(ecs::Entity e, u32 layer) const
{
    const State* s = m_states.Find(e.Pack());
    if (!s || layer >= s->machines.Size() || s->machines[layer].state < 0)
        return String();
    const GraphFile* g = m_graphs.Find(s->graphPath);
    if (!g || !g->ok || layer >= g->graph.MachineCount())
        return String();
    const AnimStateMachine& m = g->graph.Machine(layer);
    const i32 state = s->machines[layer].state;
    return state < i32(m.states.Size()) ? m.states[usize(state)].name : String();
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
