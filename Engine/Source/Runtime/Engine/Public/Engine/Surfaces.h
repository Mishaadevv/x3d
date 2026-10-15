#pragma once

// Surface types for footsteps and impacts, guessed from what an entity is made of: a Terrain is grass, Water is water,
// otherwise the name of its first material decides ("M_Grass", "M_Wood", "M_Snow", ...). Until physical materials
// exist this keeps sounds right without any setup.

#include "Asset/CookedAssets.h"
#include "Core/Containers/Function.h"
#include "Scene/ECS/World.h"

namespace zx {

/// "Grass", "Wood", "Snow", "Sand", "Carpet", "Metal", "Water" or "Concrete" (the default) from a material name.
StringView SurfaceFromName(StringView materialName);
/// The surface of an entity; `materialName` turns a material id into its name (null: Concrete for meshes).
StringView SurfaceOf(const ecs::World& world, ecs::Entity e, const Function<String(asset::AssetId)>& materialName);
/// The footstep sound set for a surface: StarterContent/Sounds/Footsteps/Footstep_<Concrete|Grass|Wood|Snow|Carpet>.
StringView FootstepSurface(StringView surface);

} // namespace zx
