#pragma once

// Animation retargeting: plays a clip made for one skeleton on another (different bone names, proportions, bone axes,
// rest pose or up axis).
//
//   RetargetMap map = anim::BuildRetargetMap(sourceSkeleton, targetSkeleton);
//   AnimationClip clip = anim::RetargetClip(sourceClip, sourceSkeleton, targetSkeleton, map); // keys for the target
//
// Bones are matched as humanoid slots recognized from common naming schemes (Mixamo "mixamorig:LeftUpLeg", Unreal
// "thigh_l", Blender "UpperLeg.L", 3ds Max "Bip01 L Thigh", ...); the spine chain between the hips and the neck is
// matched by position along the chain, so 2 spine bones drive 4 and the other way round. Other bones (fingers, props)
// match by normalized name. Rotations are transferred as the change from the source's rest pose in skeleton space,
// with the target's rest pose first turned to the source's (an A-pose rig plays T-pose clips with the arms where the
// animator put them). The two skeleton spaces are aligned through Skeleton::toModel (each skeleton's place in its
// model, where characters face +Z), so a Z-up rig plays Y-up clips and a rig whose rest pose faces sideways still walks
// forward.
// The hips (and a mapped root bone) also move, scaled by the ratio of the hip heights; every other bone keeps the
// target's bone lengths.

#include "Asset/Animation.h"

namespace zx::asset::anim {

enum class HumanBone : u8
{
    None,
    Root, // above the hips (often carries root motion)
    Hips,
    Spine, // any bone of the chain between the hips and the neck (spine, chest, upper chest)
    Neck,
    Head,
    LeftShoulder,
    LeftUpperArm,
    LeftLowerArm,
    LeftHand,
    RightShoulder,
    RightUpperArm,
    RightLowerArm,
    RightHand,
    LeftUpperLeg,
    LeftLowerLeg,
    LeftFoot,
    LeftToes,
    RightUpperLeg,
    RightLowerLeg,
    RightFoot,
    RightToes,
    Count,
};

const char* HumanBoneName(HumanBone bone);
/// Humanoid slot of a bone name (None for fingers, twist / end bones and anything unrecognized).
HumanBone ClassifyBone(StringView name);
/// Lower-case alphanumerics of a name without its namespace ("mixamorig:Left_Hand.001" -> "lefthand001"): the key
/// for matching bones that are not humanoid slots.
String NormalizeBoneName(StringView name);

struct RetargetMap
{
    Array<i32> source;  // per target bone: the source bone driving it (-1: keeps its rest pose)
    Array<Quat> restFix; // per target bone (skeleton space): turns its rest pose onto the source's (body, then direction)
    Array<u8> moves;    // per target bone: 1 = its position follows the source (hips, root)
    Array<Transform> sourceRest, targetRest; // rest poses in skeleton space
    Quat align;         // source skeleton space -> target skeleton space (through the model spaces)
    f32 scale = 1.0f;   // target hip height / source hip height
    u32 mapped = 0;     // target bones with a source
    bool identity = false; // same bones and rest pose: the source's clips play unchanged
};

RetargetMap BuildRetargetMap(const Skeleton& source, const Skeleton& target);
/// Target local pose from a source local pose (both with their skeleton's bone count).
void RetargetPose(const RetargetMap& map, const Skeleton& source, Span<const Transform> sourceLocal, const Skeleton& target,
                  Span<Transform> out);
/// The clip resampled at `sampleRate` for the target skeleton (rotation keys for mapped bones, position keys for the
/// moving ones; morph tracks copied). Name and duration stay; boneCount is the target's.
AnimationClip RetargetClip(const AnimationClip& clip, const Skeleton& source, const Skeleton& target, const RetargetMap& map,
                           f32 sampleRate = 30.0f);

} // namespace zx::asset::anim
