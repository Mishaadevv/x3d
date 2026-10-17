#include "Asset/Animation.h"

namespace zx::asset {

i32 Skeleton::FindBone(StringView bone) const
{
    for (usize i = 0; i < boneNames.Size(); ++i)
        if (boneNames[i].View() == bone)
            return i32(i);
    return -1;
}

namespace anim {

namespace {

/// Key pair around `t` in ascending `times`: indices and the blend factor between them.
void FindKeys(Span<const f32> times, f32 t, usize& k0, usize& k1, f32& f)
{
    const usize n = times.Size();
    if (n == 1 || t <= times[0]) {
        k0 = k1 = 0;
        f = 0.0f;
        return;
    }
    if (t >= times[n - 1]) {
        k0 = k1 = n - 1;
        f = 0.0f;
        return;
    }
    usize lo = 0, hi = n - 1; // times[lo] <= t < times[hi]
    while (hi - lo > 1) {
        const usize mid = (lo + hi) / 2;
        if (times[mid] <= t)
            lo = mid;
        else
            hi = mid;
    }
    k0 = lo;
    k1 = hi;
    const f32 span = times[hi] - times[lo];
    f = span > 1e-9f ? (t - times[lo]) / span : 0.0f;
}

Quat NlerpShortest(const Quat& a, Quat b, f32 t)
{
    if (a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w < 0.0f)
        b = Quat(-b.x, -b.y, -b.z, -b.w);
    return Normalize(Quat(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t));
}

} // namespace

void SampleClip(const AnimationClip& clip, const Skeleton& skeleton, f32 time, bool loop, Span<Transform> out)
{
    const u32 bones = skeleton.BoneCount();
    for (u32 b = 0; b < bones && b < out.Size(); ++b)
        out[b] = skeleton.bindPose[b];
    if (clip.duration > 0.0f)
        time = loop ? time - clip.duration * Floor(time / clip.duration) : Clamp(time, 0.0f, clip.duration);
    else
        time = 0.0f;
    for (const AnimationTrack& tr : clip.tracks) {
        if (tr.bone >= bones || tr.bone >= out.Size())
            continue;
        Transform& x = out[tr.bone];
        usize k0, k1;
        f32 f;
        if (!tr.positions.Empty() && tr.positions.Size() == tr.positionTimes.Size()) {
            FindKeys(tr.positionTimes.View(), time, k0, k1, f);
            x.position = (tr.stepMask & AnimationTrack::StepPosition) ? tr.positions[k0] : Lerp(tr.positions[k0], tr.positions[k1], f);
        }
        if (!tr.rotations.Empty() && tr.rotations.Size() == tr.rotationTimes.Size()) {
            FindKeys(tr.rotationTimes.View(), time, k0, k1, f);
            x.rotation = (tr.stepMask & AnimationTrack::StepRotation) ? tr.rotations[k0] : NlerpShortest(tr.rotations[k0], tr.rotations[k1], f);
        }
        if (!tr.scales.Empty() && tr.scales.Size() == tr.scaleTimes.Size()) {
            FindKeys(tr.scaleTimes.View(), time, k0, k1, f);
            x.scale = (tr.stepMask & AnimationTrack::StepScale) ? tr.scales[k0] : Lerp(tr.scales[k0], tr.scales[k1], f);
        }
    }
}

void BlendPoses(Span<const Transform> a, Span<const Transform> b, f32 t, Span<Transform> out)
{
    const usize n = Min(Min(a.Size(), b.Size()), out.Size());
    for (usize i = 0; i < n; ++i)
        out[i] = Transform(Lerp(a[i].position, b[i].position, t), NlerpShortest(a[i].rotation, b[i].rotation, t), Lerp(a[i].scale, b[i].scale, t));
}

void LocalToModel(const Skeleton& skeleton, Span<const Transform> local, Span<Mat4> out)
{
    const u32 bones = Min(Min(skeleton.BoneCount(), u32(local.Size())), u32(out.Size()));
    for (u32 b = 0; b < bones; ++b) {
        const Mat4 m = local[b].ToMatrix();
        const i32 p = skeleton.parents[b];
        out[b] = p >= 0 ? out[u32(p)] * m : m;
    }
}

void SkinningMatrices(const Skeleton& skeleton, Span<const Mat4> model, const Mat4& skinToMesh, Span<Mat4> out)
{
    const u32 bones = Min(Min(skeleton.BoneCount(), u32(model.Size())), u32(out.Size()));
    for (u32 b = 0; b < bones; ++b)
        out[b] = skinToMesh * model[b] * skeleton.inverseBind[b];
}

void PackSkin(const u32 joints[4], const f32 weights[4], u32& packedJoints, u32& packedWeights)
{
    f32 sum = 0.0f;
    for (u32 i = 0; i < 4; ++i)
        sum += Max(weights[i], 0.0f);
    u32 q[4] = {};
    u32 total = 0;
    for (u32 i = 0; i < 4; ++i) {
        q[i] = sum > 0.0f ? u32(Max(weights[i], 0.0f) / sum * 255.0f + 0.5f) : (i == 0 ? 255u : 0u);
        total += q[i];
    }
    // Rounding drift goes to the largest weight so the four always sum to exactly 255.
    u32 largest = 0;
    for (u32 i = 1; i < 4; ++i)
        if (q[i] > q[largest])
            largest = i;
    q[largest] = u32(i32(q[largest]) + 255 - i32(total));
    packedJoints = 0;
    packedWeights = 0;
    for (u32 i = 0; i < 4; ++i) {
        packedJoints |= (Min(joints[i], 255u) & 0xFFu) << (i * 8);
        packedWeights |= (q[i] & 0xFFu) << (i * 8);
    }
}

} // namespace anim

} // namespace zx::asset
