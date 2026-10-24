#pragma once

// CPU-side mesh representation used by importers, the mesh processing pipeline and the cooker.
// Engine conventions: left-handed, Y-up, +Z forward, meters, clockwise front faces, UV origin top-left.

#include "Core/Containers/Array.h"
#include "Core/Math/Math.h"
#include "Core/String/String.h"

namespace zx::asset {

struct MeshVertex
{
    Vec3 position;
    Vec3 normal;
    Vec4 tangent; // xyz + handedness sign in w (bitangent = cross(normal, tangent) * w)
    Vec2 uv0;
    Vec2 uv1;
    u32 color = 0xFFFFFFFF; // RGBA8, linear
    // Skinning (MeshAttributes::Skin): four bone indices (u8 each) and their weights (unorm8, summing to 255).
    u32 joints = 0;
    u32 weights = 0;
};

/// Index range drawn with one material.
struct MeshSection
{
    u32 indexOffset = 0;
    u32 indexCount = 0; // 0 in LOD > 0: no further reduction, the previous LOD is used
    u32 materialIndex = 0;
    /// LOD > 0: use this LOD when the section's projected bounding-sphere radius / view height (see
    /// SceneRenderer) is below this value.
    f32 screenSize = 1.0f;
    /// Geometric error of this LOD in meters (0 for LOD0).
    f32 error = 0.0f;
};

/// LOD0 holds the full mesh. LODs > 0 (generated) have exactly one section per LOD0 section, in the same order, so
/// every section has its own LOD chain (spatially split sections can be culled and LOD-ed independently).
struct MeshLod
{
    Array<u32> indices; // triangle list into MeshData::vertices
    Array<MeshSection> sections;
};

enum class MeshAttributes : u8
{
    None = 0,
    Normals = 1 << 0,
    Tangents = 1 << 1,
    UV0 = 1 << 2,
    UV1 = 1 << 3,
    Colors = 1 << 4,
    Skin = 1 << 5, // joints / weights (bone indices into the skeleton of the node's skin)
};
ZX_ENUM_FLAGS(MeshAttributes)

/// Morph target (blend shape): per-vertex offsets added with a weight (0..1), parallel to MeshData::vertices.
struct MorphTarget
{
    String name;
    f32 defaultWeight = 0.0f;
    Array<Vec3> positions; // position deltas
    Array<Vec3> normals;   // normal deltas (empty = none)
};

struct MeshData
{
    String name;
    Array<MeshVertex> vertices;
    Array<MorphTarget> morphs; // follow `vertices` through welding, reordering and merging
    Array<MeshLod> lods; // lods[0] = full detail
    MeshAttributes attributes = MeshAttributes::None;
    AABB bounds;
    u32 lightmapResolution = 0; // generated lightmap UVs: reference resolution their gutters fit (0 = none / imported UV1)

    /// Triangles drawn at the given LOD (sections without a reduction at that level count their previous LOD).
    u32 TriangleCount(u32 lod = 0) const;
    void ComputeBounds();
    /// Sanity check (indices in range, sections cover the index buffer). Returns an error description or empty.
    String Validate() const;
};

namespace mesh {

/// Area-and-angle weighted smooth normals over shared vertices (call after welding for smooth results).
void GenerateNormals(MeshData& mesh);
/// Per-vertex tangents from UV0 (accumulated per triangle, Gram-Schmidt orthogonalized). Needs normals and UV0.
void GenerateTangents(MeshData& mesh);
/// Reorders / drops vertices: vertex i of the result is the old vertex newToOld[i] (morph targets follow).
void PermuteVertices(MeshData& mesh, Span<const u32> newToOld);
/// Merges bit-identical vertices (with identical morph offsets) and remaps indices of all LODs.
void WeldVertices(MeshData& mesh);
/// Drops vertices no LOD references and compacts.
void RemoveUnusedVertices(MeshData& mesh);
/// Removes triangles with repeated indices or zero area.
u32 RemoveDegenerateTriangles(MeshData& mesh);
/// Converts right-handed, counter-clockwise-front data (glTF/OBJ) to engine space: mirrors X (positions, normals,
/// tangents with flipped handedness) and reverses the triangle winding.
void MirrorX(MeshData& mesh);
/// Applies a transform to positions/normals/tangents (flips winding if the determinant is negative).
void Transform(MeshData& mesh, const Mat4& m);
/// Appends `src` (all LOD0 sections) into `dst`, offsetting material indices by materialOffset.
void Append(MeshData& dst, const MeshData& src, u32 materialOffset = 0);

} // namespace mesh

} // namespace zx::asset
