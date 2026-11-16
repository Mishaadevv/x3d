#include "Asset/Retarget.h"

#include <cmath>

namespace zx::asset::anim {

namespace {

bool IsAlnum(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); }
bool IsUpper(char c) { return c >= 'A' && c <= 'Z'; }
bool IsLower(char c) { return c >= 'a' && c <= 'z'; }
char Lower(char c) { return IsUpper(c) ? char(c - 'A' + 'a') : c; }

StringView StripNamespace(StringView name)
{
    usize cut = 0;
    for (usize i = 0; i < name.Size(); ++i)
        if (name[i] == ':' || name[i] == '|')
            cut = i + 1;
    return name.SubStr(cut);
}

enum class Side : u8
{
    None,
    Left,
    Right,
};

/// Splits a bone name into its side and the lower-case remainder without separators, namespace and rig prefixes:
/// "mixamorig:LeftUpLeg" -> Left "upleg", "thigh_l" -> Left "thigh", "Bip01 R Calf" -> Right "calf".
void SplitBoneName(StringView name, Side& side, String& core, bool& ik)
{
    name = StripNamespace(name);
    side = Side::None;
    ik = false;
    // Tokens between separators.
    Array<StringView> tokens;
    for (usize i = 0; i < name.Size();) {
        while (i < name.Size() && !IsAlnum(name[i]))
            ++i;
        const usize start = i;
        while (i < name.Size() && IsAlnum(name[i]))
            ++i;
        if (i > start)
            tokens.PushBack(name.SubStr(start, i - start));
    }
    static const char* const prefixes[] = {"mixamorig", "bip01", "bip001", "bip", "def", "org", "mch", "valvebiped", "b",
                                           "j", "jnt", "bn", "bone", "cc", "base", "rig", "skel", "sk"};
    String lowered;
    Array<String> words;
    for (StringView t : tokens) {
        // Sides glued to a word: "LThigh", "ThighR" (case tells them apart from words starting / ending with l or r).
        if (side == Side::None && t.Size() > 2) {
            if ((t[0] == 'L' || t[0] == 'R') && IsUpper(t[1]) && IsLower(t[2])) {
                side = t[0] == 'L' ? Side::Left : Side::Right;
                t = t.DropLeft(1);
            } else if ((t[t.Size() - 1] == 'L' || t[t.Size() - 1] == 'R') && IsLower(t[t.Size() - 2])) {
                side = t[t.Size() - 1] == 'L' ? Side::Left : Side::Right;
                t = t.DropRight(1);
            }
        }
        lowered.Clear();
        for (char c : t)
            lowered += Lower(c);
        if (lowered == "l" || lowered == "left") {
            side = Side::Left;
            continue;
        }
        if (lowered == "r" || lowered == "right") {
            side = Side::Right;
            continue;
        }
        // "LeftUpLeg", "HandRight".
        for (const auto& [word, s] : {std::pair<StringView, Side>{"left", Side::Left}, {"right", Side::Right}}) {
            if (side != Side::None)
                break;
            if (lowered.View().StartsWith(word)) {
                side = s;
                lowered = String(lowered.View().DropLeft(word.Size()));
            } else if (lowered.View().EndsWith(word)) {
                side = s;
                lowered = String(lowered.View().DropRight(word.Size()));
            }
        }
        if (lowered == "ik")
            ik = true;
        if (!lowered.Empty())
            words.PushBack(lowered);
    }
    // Rig prefixes ("Bip01", "DEF", "CC_Base") go when something else remains.
    while (words.Size() > 1) {
        bool prefix = false;
        for (const char* p : prefixes)
            prefix = prefix || words[0] == p;
        if (!prefix)
            break;
        words.RemoveAt(0);
    }
    core.Clear();
    for (const String& w : words)
        core += w;
}

bool Has(const String& s, const char* word) { return s.Contains(StringView(word)); }

/// Same rest transform (Quat IsNearlyEqual goes through acos and cannot resolve tiny angles).
bool SameRest(const Transform& a, const Transform& b)
{
    return Length(a.position - b.position) <= 1e-4f * Max(1.0f, Length(a.position)) && Abs(Dot(a.rotation, b.rotation)) >= 1.0f - 1e-6f &&
           Length(a.scale - b.scale) <= 1e-4f;
}

/// Bind pose in skeleton space (local transforms chained to the roots).
void ModelPose(const Skeleton& sk, Span<const Transform> local, Span<Transform> out)
{
    for (u32 b = 0; b < sk.BoneCount(); ++b) {
        const i32 p = sk.parents[b];
        out[b] = p >= 0 ? out[u32(p)] * local[b] : local[b];
    }
}

bool IsAncestor(const Skeleton& sk, i32 ancestor, i32 bone)
{
    for (i32 b = bone; b >= 0; b = sk.parents[u32(b)])
        if (b == ancestor)
            return true;
    return false;
}

struct Rig
{
    i32 slot[u32(HumanBone::Count)];
    Array<i32> spine; // hips (exclusive) -> neck (exclusive), parents first
    Array<HumanBone> kind;
};

Rig FindRig(const Skeleton& sk)
{
    Rig r;
    for (i32& s : r.slot)
        s = -1;
    r.kind.Resize(sk.BoneCount(), HumanBone::None);
    for (u32 b = 0; b < sk.BoneCount(); ++b) {
        const HumanBone h = ClassifyBone(sk.boneNames[b].View());
        r.kind[b] = h;
        if (h != HumanBone::None && h != HumanBone::Spine && r.slot[u32(h)] < 0)
            r.slot[u32(h)] = i32(b);
    }
    const i32 hips = r.slot[u32(HumanBone::Hips)];
    i32 top = r.slot[u32(HumanBone::Neck)];
    if (top < 0)
        top = r.slot[u32(HumanBone::Head)];
    if (hips >= 0 && top >= 0 && IsAncestor(sk, hips, top)) {
        for (i32 b = sk.parents[u32(top)]; b >= 0 && b != hips; b = sk.parents[u32(b)])
            r.spine.PushBack(b);
        for (usize i = 0; i < r.spine.Size() / 2; ++i)
        {
            const i32 t = r.spine[i];
            r.spine[i] = r.spine[r.spine.Size() - 1 - i];
            r.spine[r.spine.Size() - 1 - i] = t;
        }
    } else {
        for (u32 b = 0; b < sk.BoneCount(); ++b)
            if (r.kind[b] == HumanBone::Spine)
                r.spine.PushBack(i32(b));
    }
    // The root slot only counts above the hips.
    if (const i32 root = r.slot[u32(HumanBone::Root)]; root >= 0 && hips >= 0 && !IsAncestor(sk, root, hips))
        r.slot[u32(HumanBone::Root)] = -1;
    return r;
}

/// Orientation of the body in skeleton space (from the hips / head / legs or arms); false without those bones.
bool BodyFrame(const Rig& r, Span<const Transform> model, Quat& out)
{
    auto at = [&](HumanBone h) { return r.slot[u32(h)]; };
    const i32 hips = at(HumanBone::Hips);
    i32 top = at(HumanBone::Head) >= 0 ? at(HumanBone::Head) : at(HumanBone::Neck);
    if (top < 0 && !r.spine.Empty())
        top = r.spine.Back();
    i32 left = at(HumanBone::LeftUpperLeg), right = at(HumanBone::RightUpperLeg);
    if (left < 0 || right < 0) {
        left = at(HumanBone::LeftUpperArm);
        right = at(HumanBone::RightUpperArm);
    }
    if (hips < 0 || top < 0 || left < 0 || right < 0)
        return false;
    const Vec3 up = model[u32(top)].position - model[u32(hips)].position;
    Vec3 side = model[u32(right)].position - model[u32(left)].position;
    if (LengthSq(up) < 1e-10f || LengthSq(side) < 1e-10f)
        return false;
    const Vec3 u = Normalize(up);
    side = side - u * Dot(side, u);
    if (LengthSq(side) < 1e-10f)
        return false;
    out = Quat::LookRotation(Normalize(Cross(Normalize(side), u)), u);
    return true;
}

/// Hip height above the feet (skeleton units); 0 when it cannot be measured.
f32 HipHeight(const Rig& r, Span<const Transform> model, const Quat& frame)
{
    const i32 hips = r.slot[u32(HumanBone::Hips)];
    const i32 lf = r.slot[u32(HumanBone::LeftFoot)], rf = r.slot[u32(HumanBone::RightFoot)];
    if (hips < 0 || lf < 0 || rf < 0)
        return 0.0f;
    const Vec3 feet = (model[u32(lf)].position + model[u32(rf)].position) * 0.5f;
    return Dot(model[u32(hips)].position - feet, frame.GetUp());
}

} // namespace

const char* HumanBoneName(HumanBone bone)
{
    static const char* const names[] = {"None", "Root", "Hips", "Spine", "Neck", "Head", "LeftShoulder", "LeftUpperArm",
                                        "LeftLowerArm", "LeftHand", "RightShoulder", "RightUpperArm", "RightLowerArm",
                                        "RightHand", "LeftUpperLeg", "LeftLowerLeg", "LeftFoot", "LeftToes",
                                        "RightUpperLeg", "RightLowerLeg", "RightFoot", "RightToes"};
    static_assert(sizeof(names) / sizeof(names[0]) == usize(HumanBone::Count));
    return u32(bone) < u32(HumanBone::Count) ? names[u32(bone)] : "None";
}

String NormalizeBoneName(StringView name)
{
    String out;
    for (char c : StripNamespace(name))
        if (IsAlnum(c))
            out += Lower(c);
    return out;
}

HumanBone ClassifyBone(StringView name)
{
    Side side;
    String c;
    bool ik;
    SplitBoneName(name, side, c, ik);
    if (ik || c.Empty())
        return HumanBone::None;
    for (const char* skip : {"thumb", "index", "middle", "ring", "pinky", "little", "finger", "twist", "roll", "nub",
                             "eye", "jaw", "tongue", "teeth", "brow", "lid", "weapon", "prop", "pole", "target"})
        if (Has(c, skip))
            return HumanBone::None;
    if (c.View().EndsWith("end") || c.View().EndsWith("end1") || c.View().EndsWith("tip"))
        return HumanBone::None;
    if (side == Side::None) {
        if (Has(c, "pelvis") || Has(c, "hip"))
            return HumanBone::Hips;
        if (Has(c, "root"))
            return HumanBone::Root;
        if (Has(c, "neck"))
            return HumanBone::Neck;
        if (Has(c, "head"))
            return HumanBone::Head;
        if (Has(c, "spine") || Has(c, "chest") || Has(c, "torso") || Has(c, "abdomen") || Has(c, "waist"))
            return HumanBone::Spine;
        return HumanBone::None;
    }
    const bool left = side == Side::Left;
    auto pick = [&](HumanBone l, HumanBone r) { return left ? l : r; };
    // "Leg_Upper", "ArmLower": the qualifier may come after the limb.
    if (Has(c, "leg") && Has(c, "upper"))
        return pick(HumanBone::LeftUpperLeg, HumanBone::RightUpperLeg);
    if (Has(c, "leg") && Has(c, "lower"))
        return pick(HumanBone::LeftLowerLeg, HumanBone::RightLowerLeg);
    if (Has(c, "arm") && Has(c, "upper"))
        return pick(HumanBone::LeftUpperArm, HumanBone::RightUpperArm);
    if (Has(c, "arm") && Has(c, "lower"))
        return pick(HumanBone::LeftLowerArm, HumanBone::RightLowerArm);
    if (Has(c, "upleg") || Has(c, "thigh") || Has(c, "upperleg") || Has(c, "hip"))
        return pick(HumanBone::LeftUpperLeg, HumanBone::RightUpperLeg);
    if (Has(c, "forearm") || Has(c, "lowerarm") || Has(c, "elbow"))
        return pick(HumanBone::LeftLowerArm, HumanBone::RightLowerArm);
    if (Has(c, "calf") || Has(c, "shin") || Has(c, "lowerleg") || Has(c, "knee") || Has(c, "leg"))
        return pick(HumanBone::LeftLowerLeg, HumanBone::RightLowerLeg);
    if (Has(c, "toe") || Has(c, "ball"))
        return pick(HumanBone::LeftToes, HumanBone::RightToes);
    if (Has(c, "foot") || Has(c, "ankle"))
        return pick(HumanBone::LeftFoot, HumanBone::RightFoot);
    if (Has(c, "clavicle") || Has(c, "shoulder") || Has(c, "collar"))
        return pick(HumanBone::LeftShoulder, HumanBone::RightShoulder);
    if (Has(c, "upperarm") || Has(c, "arm"))
        return pick(HumanBone::LeftUpperArm, HumanBone::RightUpperArm);
    if (Has(c, "hand") || Has(c, "wrist") || Has(c, "palm"))
        return pick(HumanBone::LeftHand, HumanBone::RightHand);
    return HumanBone::None;
}

RetargetMap BuildRetargetMap(const Skeleton& source, const Skeleton& target)
{
    RetargetMap m;
    const u32 ns = source.BoneCount(), nt = target.BoneCount();
    m.source.Resize(nt, -1);
    m.restFix.Resize(nt, Quat());
    m.moves.Resize(nt, 0);
    m.sourceRest.Resize(ns);
    m.targetRest.Resize(nt);
    ModelPose(source, source.bindPose.View(), m.sourceRest.View());
    ModelPose(target, target.bindPose.View(), m.targetRest.View());

    // Same skeleton (an animation-only export of the same rig): nothing to do.
    if (ns == nt) {
        bool same = SameRest(source.toModel, target.toModel);
        for (u32 b = 0; b < nt && same; ++b)
            same = source.boneNames[b] == target.boneNames[b] && source.parents[b] == target.parents[b] &&
                   SameRest(source.bindPose[b], target.bindPose[b]);
        if (same) {
            for (u32 b = 0; b < nt; ++b)
                m.source[b] = i32(b);
            m.mapped = nt;
            m.identity = true;
            return m;
        }
    }

    const Rig rs = FindRig(source), rt = FindRig(target);
    // Humanoid slots.
    for (u32 h = u32(HumanBone::Root); h < u32(HumanBone::Count); ++h)
        if (h != u32(HumanBone::Spine) && rt.slot[h] >= 0 && rs.slot[h] >= 0)
            m.source[u32(rt.slot[h])] = rs.slot[h];
    // Spine chains by position along the chain (the last target bone follows the last source bone).
    if (!rs.spine.Empty())
        for (usize i = 0; i < rt.spine.Size(); ++i) {
            const usize k = usize(Floor(f32(i + 1) * f32(rs.spine.Size()) / f32(rt.spine.Size()) + 0.5f));
            m.source[u32(rt.spine[i])] = rs.spine[Clamp<usize>(k, 1, rs.spine.Size()) - 1];
        }
    // Everything else by name (fingers, extra bones both rigs have).
    for (u32 b = 0; b < nt; ++b) {
        if (m.source[b] >= 0 || rt.kind[b] != HumanBone::None)
            continue;
        const String key = NormalizeBoneName(target.boneNames[b].View());
        for (u32 s = 0; s < ns; ++s)
            if (rs.kind[s] == HumanBone::None && NormalizeBoneName(source.boneNames[s].View()) == key) {
                m.source[b] = i32(s);
                break;
            }
    }
    for (u32 b = 0; b < nt; ++b)
        m.mapped += m.source[b] >= 0 ? 1u : 0u;

    // Skeleton spaces: aligned through the model spaces (characters face +Z there while animated, whatever their rest
    // pose looks like), and sizes from the hip heights.
    m.align = Normalize(Inverse(target.toModel.rotation) * source.toModel.rotation);
    Quat fs, ft;
    const bool frames = BodyFrame(rs, m.sourceRest.View(), fs) && BodyFrame(rt, m.targetRest.View(), ft);
    const f32 hs = frames ? HipHeight(rs, m.sourceRest.View(), fs) : 0.0f;
    const f32 ht = frames ? HipHeight(rt, m.targetRest.View(), ft) : 0.0f;
    m.scale = hs > 1e-4f && ht > 1e-4f ? ht / hs : 1.0f;
    // Turns the target's rest body into the orientation of the source's rest body (a rig whose rest pose faces
    // sideways and whose clips turn it forward).
    const Quat body = frames ? Normalize(m.align * fs * Inverse(ft)) : Quat();

    // Moving bones: the hips and a root above them; without hips, the mapped roots.
    const i32 hipsT = rt.slot[u32(HumanBone::Hips)];
    for (u32 b = 0; b < nt; ++b) {
        if (m.source[b] < 0)
            continue;
        const bool moves = hipsT >= 0 ? (i32(b) == hipsT || (rt.kind[b] == HumanBone::Root && IsAncestor(target, i32(b), hipsT)))
                                      : target.parents[b] < 0;
        m.moves[b] = moves ? 1 : 0;
    }

    // Rest pose matching: every mapped bone first turns with the body, then so that it points from its joint to its
    // mapped child the way the source bone does (A-pose vs T-pose arms, different spine curves). Hips and roots only
    // turn with the body.
    for (u32 b = 0; b < nt; ++b) {
        const i32 s = m.source[b];
        if (s < 0)
            continue;
        m.restFix[b] = body;
        if (m.moves[b] || rt.kind[b] == HumanBone::Hips || rt.kind[b] == HumanBone::Root)
            continue;
        for (u32 c = b + 1; c < nt; ++c) {
            if (target.parents[c] != i32(b) || m.source[c] < 0 || m.source[c] == s || !IsAncestor(source, s, m.source[c]))
                continue;
            const Vec3 dt = body * (m.targetRest[c].position - m.targetRest[b].position);
            const Vec3 ds = m.align * (m.sourceRest[u32(m.source[c])].position - m.sourceRest[u32(s)].position);
            if (LengthSq(dt) > 1e-12f && LengthSq(ds) > 1e-12f)
                m.restFix[b] = Normalize(Quat::FromTo(Normalize(dt), Normalize(ds)) * body);
            break;
        }
    }
    return m;
}

void RetargetPose(const RetargetMap& map, const Skeleton& source, Span<const Transform> sourceLocal, const Skeleton& target,
                  Span<Transform> out)
{
    const u32 ns = source.BoneCount(), nt = target.BoneCount();
    if (map.identity) {
        for (u32 b = 0; b < nt && b < out.Size(); ++b)
            out[b] = b < sourceLocal.Size() ? sourceLocal[b] : target.bindPose[b];
        return;
    }
    Array<Transform> sm, tm;
    sm.Resize(ns);
    tm.Resize(nt);
    ModelPose(source, sourceLocal, sm.View());
    const Quat alignInv = Inverse(map.align);
    for (u32 b = 0; b < nt; ++b) {
        const i32 p = target.parents[b];
        Transform local = target.bindPose[b];
        if (const i32 s = map.source[b]; s >= 0) {
            const Quat delta = sm[u32(s)].rotation * Inverse(map.sourceRest[u32(s)].rotation);
            const Quat global = Normalize(map.align * delta * alignInv * map.restFix[b] * map.targetRest[b].rotation);
            const Quat parent = p >= 0 ? tm[u32(p)].rotation : Quat();
            local.rotation = Normalize(Inverse(parent) * global);
            if (map.moves[b]) {
                const Vec3 pos = map.targetRest[b].position +
                                 map.align * (sm[u32(s)].position - map.sourceRest[u32(s)].position) * map.scale;
                local.position = p >= 0 ? tm[u32(p)].InverseTransformPoint(pos) : pos;
            }
        }
        tm[b] = p >= 0 ? tm[u32(p)] * local : local;
        if (b < out.Size())
            out[b] = local;
    }
}

AnimationClip RetargetClip(const AnimationClip& clip, const Skeleton& source, const Skeleton& target, const RetargetMap& map,
                           f32 sampleRate)
{
    AnimationClip out;
    out.name = clip.name;
    out.duration = clip.duration;
    out.boneCount = target.BoneCount();
    out.morphTracks = clip.morphTracks;
    const u32 nt = target.BoneCount();
    const u32 frames = clip.duration > 0.0f ? u32(std::ceil(clip.duration * Max(sampleRate, 1.0f))) + 1 : 1;
    Array<i32> trackOf;
    trackOf.Resize(nt, -1);
    for (u32 b = 0; b < nt; ++b)
        if (map.source[b] >= 0) {
            trackOf[b] = i32(out.tracks.Size());
            AnimationTrack& t = out.tracks.AddDefaulted();
            t.bone = b;
            t.rotationTimes.Reserve(frames);
            t.rotations.Reserve(frames);
        }
    Array<Transform> src, dst;
    src.Resize(source.BoneCount());
    dst.Resize(nt);
    for (u32 f = 0; f < frames; ++f) {
        const f32 time = frames > 1 ? Min(f32(f) / Max(sampleRate, 1.0f), clip.duration) : 0.0f;
        SampleClip(clip, source, time, false, src.View());
        RetargetPose(map, source, src.View(), target, dst.View());
        for (u32 b = 0; b < nt; ++b) {
            if (trackOf[b] < 0)
                continue;
            AnimationTrack& t = out.tracks[usize(trackOf[b])];
            Quat q = dst[b].rotation;
            if (!t.rotations.Empty() && Dot(t.rotations.Back(), q) < 0.0f)
                q = -q; // keep neighbouring keys on one hemisphere
            t.rotationTimes.PushBack(time);
            t.rotations.PushBack(q);
            if (map.moves[b]) {
                t.positionTimes.PushBack(time);
                t.positions.PushBack(dst[b].position);
            }
        }
    }
    return out;
}

} // namespace zx::asset::anim
