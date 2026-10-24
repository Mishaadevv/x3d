#pragma once

// Terrain: a square height field centered on the entity: generated hills (fractal noise: rolling hills to ridged
// mountains, optional flat area in the middle) or flat, plus what is sculpted on top with the editor's Terrain tool
// (raise, lower, smooth, flatten, noise) and painted layers (rock, dirt, sand: vertex colors read by M_Terrain), both
// stored in the component (scenes, undo, prefabs). TerrainSystem turns it into a mesh drawn through the
// entity's Mesh Renderer (added and kept in sync automatically, so picking, shadows, outlines and editor tools work as
// for any mesh); PhysicsSystem collides with it as a physics::HeightField.

#include "Asset/AssetManager.h"
#include "Asset/CookedAssets.h"
#include "Core/Math/Geometry.h"
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
    bool hills = true;      // generated hills under the sculpting (off: flat ground to sculpt from scratch)
    f32 height = 30.0f;     // meters from the lowest to the highest possible point
    i32 seed = 1;
    f32 featureSize = 90.0f; // meters: width of the largest hills
    i32 octaves = 6;
    f32 roughness = 0.5f;    // amplitude kept per octave (more = rougher detail)
    f32 ridges = 0.25f;      // 0 = rolling hills, 1 = sharp ridged mountains
    f32 flatCenter = 20.0f;  // meters: radius of a flat area in the middle
    /// Where this terrain sits in a larger landscape (meters): neighbor tiles with the same settings and offsets one
    /// tile apart continue each other's hills without seams (open worlds are grids of terrain tiles).
    Vec2 noiseOffset;
    asset::AssetId material;
    bool collision = true;
    // Sculpted / painted (the Terrain tool; hidden in Details): base64 of i16 height offsets (2 cm steps) and of RGBA8
    // layer weights (rock, dirt, sand, grass) per sample; empty = none. A new random dataId marks every change.
    String sculpt;
    String paint;
    i32 dataId = 0;
};
ZX_DECLARE_TYPE(Terrain);

/// Samples per side after clamping (9..1025) and the spacing between them.
u32 TerrainSamples(const Terrain& t);
f32 TerrainSpacing(const Terrain& t);
/// Heights in meters (row-major, row = +Z, column = +X; sample (0, 0) at local (-size/2, -size/2)).
void GenerateTerrainHeights(const Terrain& t, Array<f32>& heights);
/// Generated heights plus the sculpted offsets (what is rendered and collided with).
void ComputeTerrainHeights(const Terrain& t, Array<f32>& heights);
/// Decodes the sculpt offsets / paint weights for `n` x `n` samples (resampled when stored for another resolution).
void DecodeTerrainSculpt(const Terrain& t, u32 n, Array<f32>& offsets);
void DecodeTerrainPaint(const Terrain& t, u32 n, Array<u8>& rgb);
void EncodeTerrainSculpt(Terrain& t, const Array<f32>& offsets);
void EncodeTerrainPaint(Terrain& t, const Array<u8>& rgb);
/// Generated height (meters, no sculpting) at local (x, z) - also outside the terrain (seamless normals at tile edges).
f32 GeneratedTerrainHeight(const Terrain& t, f32 localX, f32 localZ);
/// A coarse copy of the terrain (`samples` per side, skirts on every chunk): far-away stand-ins (World Partition HLOD).
asset::CookedMesh CookTerrainProxy(const Terrain& t, u32 samples);
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
    /// New terrains built per Update (streaming); 0 = all at once.
    void SetNewBuildsPerUpdate(u32 n) { m_newPerUpdate = n == 0 ? ~0u : n; }

    /// Live terrain data for editing tools: generated heights, sculpted offsets, paint (n x n samples, row = +Z).
    struct Data
    {
        u32 n = 0;
        f32 spacing = 1.0f;
        Array<f32> base;
        Array<f32> sculpt;
        Array<u8> paint; // RGBA per sample: rock, dirt, sand, grass
        u64 hash = 0;    // shape hash of `base`
        i32 dataId = 0;  // component dataId `sculpt` / `paint` were decoded from
        asset::AssetId mesh;
        f64 time = 0.0;  // when the mesh was built
        bool editing = false;
        bool dirty = false;
        // Edited samples since the last mesh update (inclusive; empty when x1 < x0).
        i32 dirtyX0 = 1 << 30, dirtyZ0 = 1 << 30, dirtyX1 = -1, dirtyZ1 = -1;
        // The mesh as on the GPU (for in-place updates).
        Array<asset::PackedVertex> vertices;
        Array<asset::CookedMeshSection> sections;
        AABB bounds;
        f32 Height(u32 x, u32 z) const { return base[usize(z) * n + x] + sculpt[usize(z) * n + x]; }
    };
    /// Starts editing a terrain (its data stays live until Commit; the component is not re-read meanwhile).
    Data* BeginEdit(ecs::World& world, ecs::Entity e);
    /// Data of a terrain (built by the last Update), or null.
    Data* Find(ecs::World& world, ecs::Entity e);
    /// The mesh needs rebuilding (rebuilt by Update, a few times per second while editing).
    void MarkDirty(Data& d)
    {
        d.dirty = true;
        d.dirtyX0 = d.dirtyZ0 = 0;
        d.dirtyX1 = d.dirtyZ1 = i32(d.n) - 1;
    }
    /// Only samples [x0, x1] x [z0, z1] changed (the mesh is updated in place while editing).
    void MarkDirty(Data& d, i32 x0, i32 z0, i32 x1, i32 z1)
    {
        d.dirty = true;
        d.dirtyX0 = Min(d.dirtyX0, x0);
        d.dirtyZ0 = Min(d.dirtyZ0, z0);
        d.dirtyX1 = Max(d.dirtyX1, x1);
        d.dirtyZ1 = Max(d.dirtyZ1, z1);
    }
    /// Writes the edited data into the component (new dataId) and ends editing.
    void Commit(ecs::World& world, ecs::Entity e);
    /// Surface height at world (x, z) over a terrain entity (false outside it).
    bool HeightAt(ecs::World& world, ecs::Entity e, Vec2 worldXZ, f32& height);
    /// Closest terrain hit along a ray (world space).
    bool Raycast(ecs::World& world, const Ray& ray, f32 maxDistance, ecs::Entity& hitTerrain, Vec3& hitPoint);

private:
    void Rebuild(ecs::World& world, ecs::Entity e, Data& d, asset::AssetManager& assets, render::RenderAssets* renderAssets);
    HashMap<asset::AssetId, Data> m_built; // by mesh id
    u32 m_builds = 0;
    u32 m_newPerUpdate = 2;
};

} // namespace zx
