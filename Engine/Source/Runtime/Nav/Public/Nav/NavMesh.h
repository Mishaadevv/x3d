#pragma once

// Navigation mesh (M18), built from a triangle soup like Recast does it:
//   1. voxelize: every triangle is rasterized into columns of spans (cellSize across, cellHeight up); spans of triangles
//      flatter than maxSlope are walkable
//   2. filter: a span is walkable only if the agent fits above it (agentHeight), its edges are not ledges higher than
//      agentClimb, and it is at least agentRadius away from the walls (erosion)
//   3. polygons: walkable cells (connected to their neighbors within agentClimb) are merged greedily into rectangles -
//      convex polygons that follow the connections, so stacked floors and bridges stay apart
//   4. links: neighboring rectangles share portals (the overlapping part of their touching edges)
// Queries: FindPath = A* over the rectangles, then the funnel algorithm through the portals (the shortest path
// that stays on the mesh); NearestPoint snaps a position onto the mesh.

#include "Core/Containers/Array.h"
#include "Core/Containers/Span.h"
#include "Core/Math/Geometry.h"
#include "Core/Math/Math.h"

namespace zx::nav {

struct BuildSettings
{
    AABB bounds;                // world area to build
    f32 cellSize = 0.3f;        // meters across
    f32 cellHeight = 0.2f;      // meters up
    f32 agentHeight = 1.8f;
    f32 agentRadius = 0.4f;
    f32 agentClimb = 0.45f;     // highest step
    f32 maxSlope = 45.0f;       // degrees
    u32 maxRectCells = 24;      // rectangle side limit (cells): keeps them flat enough on slopes
    u32 minRegionCells = 6;     // smaller islands of walkable cells are dropped
};

struct BuildStats
{
    u32 columnsX = 0, columnsZ = 0;
    u32 spans = 0;
    u32 walkableCells = 0;
    u32 polys = 0;
    u32 links = 0;
    f64 milliseconds = 0.0;
};

/// A convex polygon of the mesh: a rectangle of cells, corners in world space (y from the cells under them).
struct Poly
{
    Vec3 corners[4];   // (x0, z0), (x1, z0), (x1, z1), (x0, z1)
    f32 minY = 0.0f, maxY = 0.0f;
    u32 firstLink = 0;
    u32 linkCount = 0;
};

struct Link
{
    u32 to;          // neighbor polygon
    Vec3 a, b;       // the portal (on the shared edge), in order along it
};

class NavMesh
{
public:
    /// Builds from world-space triangles (`indices` in triples). Replaces the previous mesh. Returns false if nothing
    /// walkable was found.
    bool Build(Span<const Vec3> vertices, Span<const u32> indices, const BuildSettings& settings, BuildStats* stats = nullptr);
    void Clear();
    bool IsEmpty() const { return m_polys.Empty(); }

    /// Polygon under / nearest to `p` within `searchDistance` (meters), -1 if none; `onMesh` gets the snapped point.
    i32 FindNearestPoly(Vec3 p, f32 searchDistance, Vec3* onMesh = nullptr) const;
    /// Shortest path on the mesh (corners where it turns, start and end included). False if unreachable (the path
    /// then leads as close as the mesh allows when `partial`).
    bool FindPath(Vec3 start, Vec3 end, Array<Vec3>& path, bool* partial = nullptr) const;
    /// Walks the mesh from `start` towards `end` (xz); true if the straight line stays on it. `hitT` gets the fraction
    /// of the way that was walkable.
    bool Raycast(Vec3 start, Vec3 end, f32* hitT = nullptr) const;
    /// Height of the mesh at (x, z) on polygon `poly` (bilinear over its corners).
    f32 HeightAt(u32 poly, f32 x, f32 z) const;

    Span<const Poly> Polys() const { return m_polys.View(); }
    Span<const Link> Links() const { return m_links.View(); }
    const BuildSettings& Settings() const { return m_settings; }

    /// Binary form (saved next to scenes as .znav).
    void Serialize(Array<u8>& out) const;
    bool Deserialize(Span<const u8> data);

private:
    void BuildGrid();
    BuildSettings m_settings;
    Array<Poly> m_polys;
    Array<Link> m_links;
    // Spatial grid of polygons (xz buckets) for nearest-polygon queries.
    f32 m_gridCell = 8.0f;
    Vec2 m_gridMin;
    u32 m_gridW = 0, m_gridH = 0;
    Array<u32> m_gridStart; // m_gridW * m_gridH + 1
    Array<u32> m_gridItems;
};

} // namespace zx::nav
