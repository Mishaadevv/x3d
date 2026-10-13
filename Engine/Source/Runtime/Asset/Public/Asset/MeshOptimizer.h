#pragma once

// Mesh optimization (own implementations of well-known algorithms):
//  - post-transform vertex cache ordering (Tom Forsyth's linear-speed algorithm),
//  - vertex fetch ordering (vertices in first-use order),
//  - quadric-error-metric simplification (Garland-Heckbert edge collapse onto existing vertices, border/seam locks),
//  - LOD chain generation.

#include "Asset/MeshData.h"

namespace zx::asset::mesh {

/// Reorders triangles of a triangle list for the post-transform vertex cache. Works per index range.
void OptimizeVertexCache(Span<u32> indices, u32 vertexCount);
/// Average cache miss ratio (transformed vertices per triangle) for a FIFO cache of the given size. 0.5 is ideal.
f32 ComputeACMR(Span<const u32> indices, u32 vertexCount, u32 cacheSize = 16);

/// Optimizes every section of every LOD for the vertex cache, then reorders vertices in first-use order.
void OptimizeMesh(MeshData& mesh);

struct SimplifyOptions
{
    /// Target index count (the result may be larger if the error limit is hit first).
    u32 targetIndexCount = 0;
    /// Maximum error relative to the mesh extent (0.01 = 1% of the bounding radius).
    f32 maxError = 0.01f;
    /// Keep vertices on open borders fixed.
    bool lockBorders = false;
};

/// Simplifies a triangle list over `positions` (vertices with identical positions are treated as one, so UV and
/// normal seams survive). Writes the new index list and returns the achieved relative error.
f32 Simplify(Span<const u32> indices, Span<const Vec3> positions, const SimplifyOptions& options, Array<u32>& outIndices);

struct SplitOptions
{
    /// Sections with more triangles than this are split...
    u32 maxTriangles = 4096;
    /// ...as long as their bounds are larger than this (meters), so small dense props stay one draw.
    f32 minExtent = 2.0f;
};

/// Splits big LOD0 sections into spatially coherent chunks (median splits along the longest axis) so large meshes
/// (levels, terrain pieces) can be culled and LOD-ed per chunk. Call before GenerateLods. Returns sections added.
u32 SplitSections(MeshData& mesh, const SplitOptions& options = {});

struct LodOptions
{
    u32 maxLods = 4;              // including LOD0
    f32 reduction = 0.5f;         // triangle ratio between consecutive LODs
    u32 minTriangles = 32;        // per section: stop when a LOD would be smaller
    f32 maxError = 0.05f;         // relative error limit (fraction of the section extent) for the last LOD
    f32 referenceHeight = 1080.0f; // screen height used to turn errors into switch distances (1 pixel error)
};

/// Builds LOD1..N from LOD0: an independent chain per section with locked borders (no cracks between sections or
/// chunks). Fills screenSize/error of every LOD section.
void GenerateLods(MeshData& mesh, const LodOptions& options = {});

} // namespace zx::asset::mesh
