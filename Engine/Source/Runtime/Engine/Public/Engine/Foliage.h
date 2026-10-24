#pragma once

// Foliage (M18): many copies of one plant / rock model on an entity - forests, bushes, grass. A Foliage component holds
// the model's parts (mesh, materials, local transform: resolved from the model's prefab when planted) and its instances
// (position relative to the entity, yaw, scale), packed for the scene file. Every instance is drawn through the
// GPU-driven path (culling, LODs); instances beyond cullDistance from the viewer are skipped on the CPU.
//
// One Foliage entity per plant type per World Partition cell streams with the cell.
//
//   ScatterFoliage(rules, area, heightAt, seed, out);   // rule-based placement (open world generator)

#include "Asset/AssetManager.h"
#include "Core/Containers/Array.h"
#include "Core/Containers/Function.h"
#include "Core/Math/Math.h"
#include "Core/Reflection/Reflection.h"
#include "Scene/ECS/World.h"

namespace zx {

struct FoliagePart
{
    asset::AssetId mesh;
    Array<asset::AssetId> materials;
    Transform local; // within the model
};
ZX_DECLARE_TYPE(FoliagePart);

struct FoliageInstance
{
    Vec3 position; // relative to the entity
    f32 yaw = 0.0f;
    f32 scale = 1.0f;
};

struct Foliage
{
    String model;                 // Content-relative source of the parts (for tools; the parts are what is drawn)
    Array<FoliagePart> parts;
    f32 cullDistance = 600.0f;    // meters from the viewer
    bool castShadows = true;
    // Instances: base64 of 5 floats each (x, y, z, yaw, scale); a new dataId marks every change.
    String data;
    i32 dataId = 0;
    // Runtime (decoded instances; not saved).
    Array<FoliageInstance> instances;
    i32 decodedId = -1;
};
ZX_DECLARE_TYPE(Foliage);

void EncodeFoliage(Foliage& f, Span<const FoliageInstance> instances);
/// Decodes `data` into `instances` when dataId changed.
void DecodeFoliage(Foliage& f);
/// Parts of a model prefab (every node with a mesh, with its transform within the model). False until it is loaded.
bool FoliagePartsFromPrefab(const asset::PrefabAsset& prefab, Array<FoliagePart>& out);

/// Where a plant may grow.
struct FoliageRule
{
    f32 density = 0.002f;   // instances per square meter where it grows
    f32 maxSlope = 0.6f;    // rise over run (0.6 ~ 31 degrees)
    f32 minHeight = -1e9f;  // world height band
    f32 maxHeight = 1e9f;
    f32 clumping = 0.0f;    // 0 = everywhere alike, 1 = only in patches (forests, meadows)
    f32 clumpSize = 220.0f; // meters across a patch
    f32 minScale = 0.8f, maxScale = 1.25f;
    u32 seed = 1;
};

/// Rule-based placement over a rectangle (world xz `min`, `size`); `heightAt` returns the ground height (world y)
/// at world (x, z). Positions come out in world space.
void ScatterFoliage(const FoliageRule& rule, Vec2 min, Vec2 size, const Function<f32(f32, f32), 64>& heightAt, Array<FoliageInstance>& out);

} // namespace zx
