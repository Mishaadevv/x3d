#pragma once

// Skeletons and animation clips (imported from glTF skins / animations, cooked as their own assets) and the pure math
// that turns a clip into a pose:
//
//   Array<Transform> local(skeleton.BoneCount());
//   anim::SampleClip(clip, skeleton, time, /*loop*/ true, local.View());   // bone local transforms (bind pose where untracked)
//   Array<Mat4> model(skeleton.BoneCount());
//   anim::LocalToModel(skeleton, local.View(), model.View());              // skeleton space
//   palette[b] = model[b] * skeleton.inverseBind[b];                       // skinning matrices
//
// Engine conventions (left-handed, Y-up, meters). Bones are ordered so parents precede children.

#include "Core/Containers/Array.h"
#include "Core/Containers/Span.h"
#include "Core/Math/Math.h"
#include "Core/String/String.h"

namespace zx::asset {

struct Skeleton
{
    String name;
    Array<String> boneNames;
    Array<i32> parents;       // parent bone (-1: root); parents precede children
    Array<Transform> bindPose; // rest local transforms
    Array<Mat4> inverseBind;   // skeleton space -> bone space at bind time

    u32 BoneCount() const { return u32(boneNames.Size()); }
    i32 FindBone(StringView bone) const;
};

/// Keys of one bone. Each channel has its own times (seconds, ascending); an empty channel keeps the bind pose.
struct AnimationTrack
{
    enum StepFlags : u8
    {
        StepPosition = 1,
        StepRotation = 2,
        StepScale = 4,
    };
    u32 bone = 0;
    u8 stepMask = 0; // channels with STEP interpolation (others: linear / nlerp)
    Array<f32> positionTimes;
    Array<Vec3> positions;
    Array<f32> rotationTimes;
    Array<Quat> rotations;
    Array<f32> scaleTimes;
    Array<Vec3> scales;
};

/// Morph target weights over time (glTF "weights" channel of the skinned mesh): `targets` values per key.
struct MorphTrack
{
    u32 targets = 0;
    bool step = false;
    Array<f32> times;
    Array<f32> weights; // times.Size() * targets
};

struct AnimationClip
{
    String name;
    f32 duration = 0.0f;
    u32 boneCount = 0; // bones of the skeleton it was made for
    Array<AnimationTrack> tracks;
    Array<MorphTrack> morphTracks; // usually none or one (the skinned mesh's weights)
};

namespace anim {

/// Bone local transforms of `clip` at `time` (wrapped into the clip when looping, clamped otherwise). Bones without a
/// track (or channels without keys) take the skeleton's bind pose. `out` has skeleton.BoneCount() entries.
void SampleClip(const AnimationClip& clip, const Skeleton& skeleton, f32 time, bool loop, Span<Transform> out);
/// Weights of a morph track at `time` (wrapped when looping, clamped otherwise); `out` gets track.targets values.
void SampleMorphWeights(const MorphTrack& track, f32 duration, f32 time, bool loop, Span<f32> out);
/// Linear blend of two poses (rotations nlerp along the shorter arc): out = a * (1 - t) + b * t.
void BlendPoses(Span<const Transform> a, Span<const Transform> b, f32 t, Span<Transform> out);
/// Local position of one bone of `clip` at `time` (clamped into the clip; bind pose without a position track).
Vec3 SampleBonePosition(const AnimationClip& clip, const Skeleton& skeleton, u32 bone, f32 time);
/// How far `bone` moves between the clip times `from` and `to` (to >= from; looping: `to` may run past the end, every
/// cycle adds the clip's start-to-end displacement).
Vec3 RootMotionDelta(const AnimationClip& clip, const Skeleton& skeleton, u32 bone, f32 from, f32 to, bool loop);
/// One bone of BlendPoses.
Transform BlendTransforms(const Transform& a, const Transform& b, f32 t);
/// Additive animation: the change from `reference` to `pose` (local space; pose = delta * reference) and that change
/// applied to `base` with `weight` (0 = base unchanged).
Transform AdditiveDelta(const Transform& reference, const Transform& pose);
Transform ApplyAdditive(const Transform& base, const Transform& delta, f32 weight);
/// Skeleton-space matrices from local transforms.
void LocalToModel(const Skeleton& skeleton, Span<const Transform> local, Span<Mat4> out);
/// Skinning matrices: model[b] * inverseBind[b], optionally pre-multiplied by `skinToMesh`.
void SkinningMatrices(const Skeleton& skeleton, Span<const Mat4> model, const Mat4& skinToMesh, Span<Mat4> out);
/// Rotation of `bone` in skeleton space (local rotations chained to the root; bone scale ignored).
Quat GlobalRotation(const Skeleton& skeleton, Span<const Transform> local, u32 bone);
/// Two-bone IK (upper a -> lower b -> end c, e.g. hip -> knee -> foot): rotates a and b so that c reaches `target`
/// (skeleton space; clamped to the chain's reach). The bend keeps its current plane; `bendHint` (skeleton space)
/// orients a fully straight chain. Only the local rotations of a and b change.
void SolveTwoBoneIK(const Skeleton& skeleton, Span<Transform> local, u32 a, u32 b, u32 c, Vec3 target, Vec3 bendHint);
/// Applies `rotation` (skeleton space) on top of `bone`'s current global rotation.
void RotateGlobal(const Skeleton& skeleton, Span<Transform> local, u32 bone, const Quat& rotation);
/// Packs four bone indices (each <= 255) and weights (normalized and quantized to unorm8 summing to 255).
void PackSkin(const u32 joints[4], const f32 weights[4], u32& packedJoints, u32& packedWeights);

} // namespace anim

} // namespace zx::asset
