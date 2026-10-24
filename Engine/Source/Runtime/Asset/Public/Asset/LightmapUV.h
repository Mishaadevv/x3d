#pragma once

// Lightmap UVs (UV1) for static meshes that come without them - the cooker's "Generate Lightmap UVs":
//   1. charts: triangles grow into connected charts (shared positions) while their normals stay within a cone around
//      the chart's first (largest) triangle - walls, floors and the facets of round shapes each become a chart
//   2. each chart is projected onto its plane and turned to the rotation with the smallest bounding rectangle
//   3. charts are packed into the unit square (tallest first, shelves) with a gutter of a few texels at the mesh's
//      reference resolution, so filtering never bleeds between charts
// Vertices on chart borders are duplicated (each chart has its own UV1 there). Proportions are preserved: one UV unit
// is the same number of meters everywhere in the mesh, so a baker gets even texel density.

#include "Asset/MeshData.h"

namespace zx::asset::mesh {

struct LightmapUVOptions
{
    f32 chartAngle = 50.0f;     // degrees: max deviation of a triangle's normal from its chart's (seed) normal
    f32 texelsPerMeter = 8.0f;  // reference density (the gutter is sized for it; bakes may go up to 2x coarser)
    u32 minResolution = 32;
    u32 maxResolution = 1024;   // UV1 is half precision: finer would lose sub-texel accuracy
    u32 gutterTexels = 3;       // empty texels between charts at the reference resolution
};

struct LightmapUVResult
{
    u32 charts = 0;
    u32 resolution = 0;         // reference resolution (texels per side) the gutters were sized for
    f32 coverage = 0.0f;        // fraction of the square the charts fill
    u32 addedVertices = 0;      // duplicated along chart borders
};

/// Generates UV1 for LOD0 of `mesh` (call before splitting / LODs). Sets MeshAttributes::UV1.
LightmapUVResult GenerateLightmapUVs(MeshData& mesh, const LightmapUVOptions& options = {});

} // namespace zx::asset::mesh
