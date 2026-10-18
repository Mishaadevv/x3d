#pragma once

// ZNano: virtual geometry (M15) - meshes as clusters of up to 128 triangles in a LOD hierarchy (a DAG, like Unreal's
// Nanite), so the renderer draws exactly the detail the screen needs, cluster by cluster:
//
//   leaves     LOD0 triangles cut into spatially compact clusters (Morton order of the triangle centers);
//   levels     groups of 4 neighboring clusters are merged and simplified to half (their outer borders locked, so
//              neighbors that pick a different level still meet without cracks) and cut into new clusters;
//   errors     every cluster knows the error of its own level (selfError around selfSphere) and of the level that
//              replaces it (parentError around parentSphere). A view draws a cluster when its own error projects to
//              at most the pixel threshold and its parent's error to more - one consistent cut through the DAG,
//              evaluated per cluster in parallel. Errors grow monotonically up the hierarchy, spheres contain their
//              children's, so the cut never draws two levels of the same surface.
//
// Clusters are sorted by parentError (largest first): for an instance the renderer only tests the prefix whose parent
// error can still matter at the instance's nearest point (a binary search on the CPU).

#include "Asset/CookedAssets.h"
#include "Asset/MeshData.h"

namespace zx::asset::nano {

using asset::NanoClusterHeader;
using asset::NanoMesh;
using ClusterHeader = NanoClusterHeader;
inline constexpr u32 MaxClusterTriangles = 128;
inline constexpr u32 MaxClusterVertices = 255; // local indices are u8
inline constexpr f32 RootError = NanoRootError;

struct BuildOptions
{
    u32 groupSize = 4;          // clusters merged per simplification group
    f32 minReduction = 0.85f;   // a level that keeps more than this share of the triangles ends the hierarchy
};

/// Builds the cluster DAG of every section's LOD0 of `mesh` (vertices and indices as cooked). False if the mesh has
/// nothing to cluster.
bool Build(const CookedMesh& mesh, NanoMesh& out, const BuildOptions& options = {});

/// Regular LOD chains for every section, cut from the DAG at a few error levels (fractions of the section radius), so
/// the paths that don't draw clusters (CPU rendering, point / spot shadows, velocity, translucent sections) stay cheap.
/// Appends the indices to the mesh's index data; at most `maxLods` levels per section including LOD0.
void AppendFallbackLods(CookedMesh& mesh, u32 maxLods = 4, f32 referenceHeight = 1080.0f);

/// Projected size in pixels of an error `error` at sphere `sphere` seen from `eye` (mesh space), with
/// `pixelsPerUnit` = viewport height / (2 tan(fov / 2)). The GPU uses the same formula.
inline f32 ProjectedError(f32 error, const Vec4& sphere, Vec3 eye, f32 pixelsPerUnit)
{
    if (error >= NanoRootError)
        return NanoRootError;
    const f32 d = Max(Length(sphere.XYZ() - eye) - sphere.w, 1e-3f);
    return error * pixelsPerUnit / d;
}

/// The LOD cut on the CPU (tests, tools): clusters whose own error is small enough and whose parent's isn't.
inline void SelectCut(const NanoMesh& m, Vec3 eye, f32 pixelsPerUnit, f32 thresholdPixels, Array<u32>& out)
{
    out.Clear();
    for (u32 i = 0; i < m.clusters.Size(); ++i) {
        const NanoClusterHeader& c = m.clusters[i];
        if (ProjectedError(c.selfError, c.selfSphere, eye, pixelsPerUnit) <= thresholdPixels &&
            ProjectedError(c.parentError, c.parentSphere, eye, pixelsPerUnit) > thresholdPixels)
            out.PushBack(i);
    }
}

/// A procedural rock: an icosphere subdivided `subdivisions` times (20 * 4^n triangles: 7 = 327k, 8 = 1.3M) with
/// layered noise on the radius (about 1 m). Stress scenes and tests for virtual geometry.
MeshData MakeDetailedRock(u32 subdivisions, u32 seed = 1);

/// Cook policy: static meshes (no skin, no morphs) with at least this many LOD0 triangles become virtual geometry when
/// their .zmeta says "virtualGeometry": "auto" (the default); true / false force it.
inline constexpr u32 AutoTriangleThreshold = 50000;

} // namespace zx::asset::nano
