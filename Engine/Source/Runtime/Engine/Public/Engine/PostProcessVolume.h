#pragma once

// Post Process Volume: overrides post-processing settings (exposure, color grading, bloom, depth of field, motion
// blur) while the camera is inside a box, or everywhere when Unbound. Volumes blend like in UE: sorted by priority
// (higher wins), each with a weight that fades to 0 over Blend Radius outside the box; only settings whose override
// box is ticked are touched.
//
//   render::PostSettings post = EvaluatePostProcessVolumes(world, cameraPosition, baseSettings);

#include "Core/Reflection/Reflection.h"
#include "Render/PostProcess.h"
#include "Scene/ECS/World.h"

namespace zx {

struct PostProcessVolume
{
    bool enabled = true;
    bool unbound = false;               // affects the whole world (the box is ignored)
    Vec3 extent = Vec3(5.0f, 5.0f, 5.0f); // half size of the box (local space, scaled by the transform)
    f32 blendRadius = 2.0f;             // meters outside the box over which the volume fades out
    f32 blendWeight = 1.0f;             // 0..1
    i32 priority = 0;                   // higher priority is applied later (wins)

    // Overrides: a value is used only when its override flag is set.
    bool overrideExposureCompensation = false;
    f32 exposureCompensation = 0.0f; // EV
    bool overrideBloomIntensity = false;
    f32 bloomIntensity = 0.04f;
    bool overrideContrast = false;
    f32 contrast = 1.05f;
    bool overrideSaturation = false;
    f32 saturation = 1.05f;
    bool overrideTemperature = false;
    f32 temperature = 6500.0f; // Kelvin
    bool overrideColorFilter = false;
    Vec3 colorFilter = Vec3(1.0f, 1.0f, 1.0f);
    bool overrideVignette = false;
    f32 vignette = 0.25f;
    bool overrideDepthOfField = false;
    bool depthOfField = true;
    bool overrideFocusDistance = false;
    f32 focusDistance = 5.0f; // meters
    bool overrideFStop = false;
    f32 fStop = 2.8f;
    bool overrideMaxBlur = false;
    f32 maxBlur = 16.0f; // pixels
    bool overrideMotionBlur = false;
    bool motionBlur = true;
    bool overrideMotionBlurAmount = false;
    f32 motionBlurAmount = 0.5f;
    bool overrideMotionBlurMax = false;
    f32 motionBlurMax = 32.0f; // pixels
};
ZX_DECLARE_TYPE(PostProcessVolume);

/// Weight (0..1) of a volume for a camera at `position`; `world` is the volume's world matrix.
f32 PostProcessVolumeWeight(const PostProcessVolume& volume, const Mat4& world, Vec3 position);
/// Applies one volume's overrides on top of `settings` with `weight` (numbers blend, switches flip at 0.5).
void ApplyPostProcessVolume(const PostProcessVolume& volume, f32 weight, render::PostSettings& settings);
/// `base` with every enabled volume affecting `position` blended over it in priority order.
render::PostSettings EvaluatePostProcessVolumes(ecs::World& world, Vec3 position, const render::PostSettings& base);

} // namespace zx
