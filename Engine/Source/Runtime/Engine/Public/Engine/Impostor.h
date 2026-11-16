#pragma once

// Impostors: a plant seen from far away as one camera-facing quad. BakeImpostor renders the model on the CPU from
// Frames x Frames directions over the upper hemisphere (hemi-octahedral layout) into an albedo atlas (alpha = coverage)
// and a normal atlas (each view's own space: x = the view's right axis, y = its up axis, z = towards the viewer). The
// forward vertex shader turns the quad towards the camera and picks the frame of the view direction (Foliage
// impostorFrames / impostorCenterY -> instance row +192: x = half2(-frames, centerY)), so the pixel shader, lighting,
// fog and alpha test are the ordinary ones.
//
//   ImpostorAsset imp;
//   BakeImpostor(assets, foliage.parts, imp);                      // or DeserializeImpostor (HLOD files)
//   const ImpostorIds ids = RegisterImpostor(assets, imp, "Pine_3"); // quad mesh, material, atlases as memory assets
//   MakeImpostorFoliage(ids, imp, foliage);                         // the Foliage draws the plants as impostors

#include "Asset/CookedAssets.h"
#include "Engine/Foliage.h"

namespace zx {

namespace asset {
class AssetManager;
}

struct ImpostorAsset
{
    u32 frames = 8;      // frames per side of the atlas
    u32 frameSize = 64;  // texels per frame side
    f32 centerY = 0.0f;  // model space: the bounding sphere the views are framed on
    f32 radius = 1.0f;
    asset::CookedTexture albedo; // sRGB color, alpha = coverage (alpha tested at 0.5)
    asset::CookedTexture normal; // RG = the view-space normal's x, y
};

struct ImpostorIds
{
    asset::AssetId mesh, material, albedo, normal;
};

/// Direction (model space, towards the viewer, y >= 0) frame (x, y) of a frames x frames hemi-octahedral layout shows.
Vec3 ImpostorFrameDirection(u32 x, u32 y, u32 frames);
/// The frame whose direction is closest to `dir` (model space, towards the viewer; below the horizon counts as on it).
void ImpostorFrameOf(Vec3 dir, u32 frames, u32& x, u32& y);
/// A frame's image axes for its direction (the vertex shader uses the same): right = Y x dir, up = dir x right.
void ImpostorFrameAxes(Vec3 dir, Vec3& right, Vec3& up);

/// Renders the parts' meshes (loaded, CPU data kept) with their materials (base color texture from its source image,
/// alpha masks, two-sided leaves). False when a mesh isn't loaded or the model is empty.
bool BakeImpostor(asset::AssetManager& assets, Span<const FoliagePart> parts, ImpostorAsset& out, u32 frames = 8, u32 frameSize = 64);
/// Bakes a model file (physical path) with a cooker of its own (`cooker` is created on first use and may be reused for
/// more models; its cache is in the temp folder): renderers drop the CPU copies the bake needs.
bool BakeImpostorFromFile(StringView modelPath, ImpostorAsset& out, UniquePtr<asset::AssetManager>& cooker);
void SerializeImpostor(const ImpostorAsset& imp, Array<u8>& out);
bool DeserializeImpostor(ByteSpan data, ImpostorAsset& out);
/// Registers the quad mesh, material and atlases (ids derived from `key`: the same key gives the same ids, registered
/// once).
ImpostorIds RegisterImpostor(asset::AssetManager& assets, const ImpostorAsset& imp, StringView key);
/// Turns `f` into impostors of its plants (parts = the quad; no shadows, wind or collision).
void MakeImpostorFoliage(const ImpostorIds& ids, const ImpostorAsset& imp, Foliage& f);
/// Plants big enough to be seen from far away (trees, rocks): the ones HLODs keep as impostors.
bool WantsImpostor(const Foliage& f);

} // namespace zx
