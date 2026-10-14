#pragma once

// Terrain v0: a square height field centered on the entity, generated from fractal noise (rolling hills to ridged
// mountains, optional flat area in the middle for building). TerrainSystem turns it into a mesh drawn through the
// entity's Mesh Renderer (added and kept in sync automatically, so picking, shadows, outlines and editor tools work as
// for any mesh); PhysicsSystem collides with it as a physics::HeightField.

#include "Asset/AssetManager.h"
#include "Core/Containers/HashMap.h"
#include "Core/Reflection/Reflection.h"
#include "Scene/ECS/World.h"

namespace zx {

namespace render {
class RenderAssets;
}

struct Terrain
{
    f32 size = 200.0f;      // meters (square)
    i32 resolution = 257;   // samples per side
    f32 height = 30.0f;     // meters from the lowest to the highest possible point
    i32 seed = 1;
    f32 featureSize = 90.0f; // meters: width of the largest hills
    i32 octaves = 6;
    f32 roughness = 0.5f;    // amplitude kept per octave (more = rougher detail)
    f32 ridges = 0.25f;      // 0 = rolling hills, 1 = sharp ridged mountains
    f32 flatCenter = 20.0f;  // meters: radius of a flat area in the middle
    asset::AssetId material;
    bool collision = true;
};
ZX_DECLARE_TYPE(Terrain);

/// Samples per side after clamping (9..1025) and the spacing between them.
u32 TerrainSamples(const Terrain& t);
f32 TerrainSpacing(const Terrain& t);
/// Heights in meters (row-major, row = +Z, column = +X; sample (0, 0) at local (-size/2, -size/2)).
void GenerateTerrainHeights(const Terrain& t, Array<f32>& heights);
/// Hash of everything that changes the generated shape.
u64 TerrainShapeHash(const Terrain& t);
/// The id under which the terrain's mesh is registered (stable per entity guid).
asset::AssetId TerrainMeshId(const ecs::World& world, ecs::Entity e);

/// Builds terrain meshes when a Terrain appears or its shape changes; keeps its Mesh Renderer pointing at the mesh.
class TerrainSystem
{
public:
    /// Call once per frame before rendering. `renderAssets` (may be null) drops the old GPU copy of a rebuilt mesh.
    void Update(ecs::World& world, asset::AssetManager& assets, render::RenderAssets* renderAssets);
    void Clear() { m_built.Clear(); }
    u32 BuildCount() const { return m_builds; }

private:
    struct Built
    {
        u64 hash = 0;
        asset::AssetId mesh;
        f64 time = 0.0; // when it was built
    };
    HashMap<asset::AssetId, Built> m_built; // by mesh id
    u32 m_builds = 0;
};

} // namespace zx
