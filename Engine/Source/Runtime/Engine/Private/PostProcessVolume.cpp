#include "Engine/PostProcessVolume.h"
#include "Scene/Hierarchy.h"

namespace zx {

// Each value is editable only when its override box is ticked (EditCondition: drawn as a checkbox before the label).
ZX_REFLECT_STRUCT_BEGIN(PostProcessVolume)
    ZX_FIELD(enabled, Meta::Category("Volume"))
    ZX_FIELD(unbound, Meta::Category("Volume"), Meta::Tooltip("Affects the whole world; the box is ignored"))
    ZX_FIELD(extent, Meta::Category("Volume"), Meta::Tooltip("Half size of the box in meters (scaled by the transform)"))
    ZX_FIELD(blendRadius, Meta::Category("Volume"), Meta::Range(0, 50), Meta::Units("m"),
             Meta::Tooltip("Distance outside the box over which the volume fades out"))
    ZX_FIELD(blendWeight, Meta::Category("Volume"), Meta::Range(0, 1))
    ZX_FIELD(priority, Meta::Category("Volume"), Meta::Tooltip("Higher priority volumes are applied last and win"))
    ZX_FIELD(overrideExposureCompensation, Meta::Hidden())
    ZX_FIELD(exposureCompensation, Meta::Category("Exposure"), Meta::EditCondition("overrideExposureCompensation"), Meta::Range(-6, 6),
             Meta::Units("EV"))
    ZX_FIELD(overrideBloomIntensity, Meta::Hidden())
    ZX_FIELD(bloomIntensity, Meta::Category("Bloom"), Meta::EditCondition("overrideBloomIntensity"), Meta::Range(0, 0.5))
    ZX_FIELD(overrideContrast, Meta::Hidden())
    ZX_FIELD(contrast, Meta::Category("Color Grading"), Meta::EditCondition("overrideContrast"), Meta::Range(0.5, 2))
    ZX_FIELD(overrideSaturation, Meta::Hidden())
    ZX_FIELD(saturation, Meta::Category("Color Grading"), Meta::EditCondition("overrideSaturation"), Meta::Range(0, 2))
    ZX_FIELD(overrideTemperature, Meta::Hidden())
    ZX_FIELD(temperature, Meta::Category("Color Grading"), Meta::EditCondition("overrideTemperature"), Meta::Range(2000, 12000),
             Meta::Units("K"), Meta::Tooltip("White balance: lower = the image turns blue, higher = warmer"))
    ZX_FIELD(overrideColorFilter, Meta::Hidden())
    ZX_FIELD(colorFilter, Meta::Category("Color Grading"), Meta::EditCondition("overrideColorFilter"), Meta::Color())
    ZX_FIELD(overrideVignette, Meta::Hidden())
    ZX_FIELD(vignette, Meta::Category("Color Grading"), Meta::EditCondition("overrideVignette"), Meta::Range(0, 1))
    ZX_FIELD(overrideDepthOfField, Meta::Hidden())
    ZX_FIELD(depthOfField, Meta::Category("Depth of Field"), Meta::EditCondition("overrideDepthOfField"))
    ZX_FIELD(overrideFocusDistance, Meta::Hidden())
    ZX_FIELD(focusDistance, Meta::Category("Depth of Field"), Meta::EditCondition("overrideFocusDistance"), Meta::Range(0.1, 1000),
             Meta::Units("m"))
    ZX_FIELD(overrideFStop, Meta::Hidden())
    ZX_FIELD(fStop, Meta::Category("Depth of Field"), Meta::EditCondition("overrideFStop"), Meta::Range(0.7, 32),
             Meta::Tooltip("Aperture: smaller numbers blur more"))
    ZX_FIELD(overrideMaxBlur, Meta::Hidden())
    ZX_FIELD(maxBlur, Meta::Category("Depth of Field"), Meta::EditCondition("overrideMaxBlur"), Meta::Range(1, 64), Meta::Units("px"))
    ZX_FIELD(overrideMotionBlur, Meta::Hidden())
    ZX_FIELD(motionBlur, Meta::Category("Motion Blur"), Meta::EditCondition("overrideMotionBlur"))
    ZX_FIELD(overrideMotionBlurAmount, Meta::Hidden())
    ZX_FIELD(motionBlurAmount, Meta::Category("Motion Blur"), Meta::EditCondition("overrideMotionBlurAmount"), Meta::Range(0, 1),
             Meta::Tooltip("Shutter: fraction of the camera's motion during one frame"))
    ZX_FIELD(overrideMotionBlurMax, Meta::Hidden())
    ZX_FIELD(motionBlurMax, Meta::Category("Motion Blur"), Meta::EditCondition("overrideMotionBlurMax"), Meta::Range(1, 128),
             Meta::Units("px"))
    ZX_FIELD(overrideColorLut, Meta::Hidden())
    ZX_FIELD(colorLut, Meta::Category("Color Grading"), Meta::EditCondition("overrideColorLut"),
             Meta::Tooltip("256 x 16 lookup table image (Unreal layout), e.g. StarterContent/LUTs/LUT_Cinematic.png"))
    ZX_FIELD(colorLutIntensity, Meta::Category("Color Grading"), Meta::Range(0, 1))
ZX_REFLECT_STRUCT_END()

f32 PostProcessVolumeWeight(const PostProcessVolume& volume, const Mat4& world, Vec3 position)
{
    if (!volume.enabled || volume.blendWeight <= 0.0f)
        return 0.0f;
    const f32 weight = Saturate(volume.blendWeight);
    if (volume.unbound)
        return weight;
    // Distance from the box in world meters: local point, clamped to the box, back to world.
    const Vec3 local = Inverse(world).TransformPoint(position);
    const Vec3 e(Abs(volume.extent.x), Abs(volume.extent.y), Abs(volume.extent.z));
    const Vec3 closest(Clamp(local.x, -e.x, e.x), Clamp(local.y, -e.y, e.y), Clamp(local.z, -e.z, e.z));
    const f32 distance = Length(world.TransformPoint(closest) - position);
    if (distance <= 0.0f)
        return weight;
    if (volume.blendRadius <= 0.0f || distance >= volume.blendRadius)
        return 0.0f;
    return weight * (1.0f - distance / volume.blendRadius);
}

void ApplyPostProcessVolume(const PostProcessVolume& v, f32 w, render::PostSettings& s)
{
    if (w <= 0.0f)
        return;
    auto blend = [w](f32& target, f32 value) { target = Lerp(target, value, w); };
    auto flip = [w](bool& target, bool value) {
        if (w >= 0.5f)
            target = value;
    };
    if (v.overrideExposureCompensation)
        blend(s.exposureCompensation, v.exposureCompensation);
    if (v.overrideBloomIntensity)
        blend(s.bloomIntensity, v.bloomIntensity);
    if (v.overrideContrast)
        blend(s.contrast, v.contrast);
    if (v.overrideSaturation)
        blend(s.saturation, v.saturation);
    if (v.overrideTemperature)
        blend(s.temperature, v.temperature);
    if (v.overrideColorFilter)
        s.colorFilter = Lerp(s.colorFilter, v.colorFilter, w);
    if (v.overrideVignette)
        blend(s.vignette, v.vignette);
    if (v.overrideDepthOfField)
        flip(s.depthOfField, v.depthOfField);
    if (v.overrideFocusDistance)
        blend(s.focusDistance, v.focusDistance);
    if (v.overrideFStop)
        blend(s.fStop, v.fStop);
    if (v.overrideMaxBlur)
        blend(s.maxBlur, v.maxBlur);
    if (v.overrideMotionBlur)
        flip(s.motionBlur, v.motionBlur);
    if (v.overrideMotionBlurAmount)
        blend(s.motionBlurAmount, v.motionBlurAmount);
    if (v.overrideMotionBlurMax)
        blend(s.motionBlurMax, v.motionBlurMax);
}

String FindColorLut(ecs::World& world, Vec3 position, f32& intensity)
{
    String best;
    i32 bestPriority = 0;
    intensity = 0.0f;
    world.Query<const PostProcessVolume>().Each([&](ecs::Entity e, const PostProcessVolume& v) {
        if (!v.overrideColorLut || v.colorLut.Empty())
            return;
        const scene::WorldTransform* wt = world.Get<const scene::WorldTransform>(e);
        const f32 w = PostProcessVolumeWeight(v, wt ? wt->matrix : Mat4::Identity(), position);
        if (w <= 0.0f || (!best.Empty() && v.priority < bestPriority))
            return;
        best = v.colorLut;
        bestPriority = v.priority;
        intensity = Saturate(v.colorLutIntensity) * w;
    });
    return best;
}

render::PostSettings EvaluatePostProcessVolumes(ecs::World& world, Vec3 position, const render::PostSettings& base)
{
    struct Active
    {
        const PostProcessVolume* volume;
        f32 weight;
        u32 order; // creation order breaks priority ties (stable)
    };
    Active active[64];
    u32 count = 0, order = 0;
    world.Query<const PostProcessVolume>().Each([&](ecs::Entity e, const PostProcessVolume& v) {
        const scene::WorldTransform* wt = world.Get<const scene::WorldTransform>(e);
        const f32 w = PostProcessVolumeWeight(v, wt ? wt->matrix : Mat4::Identity(), position);
        if (w > 0.0f && count < 64)
            active[count++] = Active{&v, w, order};
        ++order;
    });
    // Insertion sort by priority (few volumes).
    for (u32 i = 1; i < count; ++i)
        for (u32 j = i; j > 0 && (active[j].volume->priority < active[j - 1].volume->priority ||
                                  (active[j].volume->priority == active[j - 1].volume->priority && active[j].order < active[j - 1].order));
             --j) {
            const Active t = active[j];
            active[j] = active[j - 1];
            active[j - 1] = t;
        }
    render::PostSettings s = base;
    for (u32 i = 0; i < count; ++i)
        ApplyPostProcessVolume(*active[i].volume, active[i].weight, s);
    return s;
}

} // namespace zx
