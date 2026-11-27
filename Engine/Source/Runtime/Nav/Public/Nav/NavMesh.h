#pragma once

// Navigation mesh (M18), built from a triangle soup like Recast does it:
//   1. voxelize: every triangle is rasterized into columns of spans (cellSize across, cellHeight up); spans of triangles
//      flatter than maxSlope are walkable
//   2. filter: a span is walkable only if the agent fits above it (agentHeight), its edges are not ledges higher than
//      agentClimb, and it is at least agentRadius away from the walls (erosion)
//   3. polygons: walkable cells (connected to their neighbors within agentClimb) are merged greedily into rectangles -
//      convex polygons that follow the connections, so stacked floors and bridges stay apart
//   4. links: neighboring rectangles share portals (the overlapping part of their touching edges)
// Tiles: with tileSize > 0 the bounds are cut into square tiles built one by one (each voxelized with a border as wide
// as the agent, so erosion near the tile edge matches a single build), then stitched: the walkable cells along
// facing tile edges link up like neighbor cells do (within a step, room for the agent). RebuildTiles rebuilds only the
// tiles an area touches (moving obstacles, changed level geometry) and stitches them back in.
// Obstacles: boxes that carve the floor under them (doors, crates) - passed to Build / RebuildTiles.
// Off-mesh links: ways the mesh does not hold (a jump, a drop, a ladder) - endpoints snapped onto the polygons they
// join, then used by the search like a portal that costs its length plus `cost`; the path marks where it crosses them
// (FindPath's `jumps`), because the agent is moved over the gap there instead of walking.
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
    f32 tileSize = 0.0f;        // meters per tile side (0 = one tile over the bounds)
};

struct BuildStats
{
    u32 columnsX = 0, columnsZ = 0;
    u32 spans = 0;
    u32 walkableCells = 0;
    u32 polys = 0;
    u32 links = 0;
    u32 tilesBuilt = 0;
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

/// A way off the mesh: a jump, a ladder, a drop. The agent walks to `start`, crosses to `end` (the game moves it over
/// the gap) and walks on. Points are world space; the build snaps them onto the mesh and fills in the polygons.
struct OffMeshLink
{
    Vec3 start;
    Vec3 end;
    bool bidirectional = true; // both ways (a drop is usually one-way: down only)
    f32 cost = 0.0f;           // extra meters of walking the crossing is worth (a slow ladder costs a few)
    i32 startPoly = -1, endPoly = -1; // set by the build, -1 when the endpoint is not on the mesh
};

class NavMesh
{
public:
    /// Builds from world-space triangles (`indices` in triples), carving `obstacles` (world boxes). Replaces the previous
    /// mesh. Returns false if nothing walkable was found.
    bool Build(Span<const Vec3> vertices, Span<const u32> indices, const BuildSettings& settings, BuildStats* stats = nullptr,
               Span<const AABB> obstacles = {}, Span<const OffMeshLink> links = {});
    /// The tiles `area` touches, with their borders: the geometry RebuildTiles needs (empty when nothing is built).
    AABB TileBuildBounds(const AABB& area) const;
    /// Rebuilds the tiles `area` touches from geometry covering TileBuildBounds(area) and stitches them back in. False
    /// when no tile was rebuilt (empty mesh, area outside the bounds).
    bool RebuildTiles(Span<const Vec3> vertices, Span<const u32> indices, const AABB& area, Span<const AABB> obstacles = {},
                      BuildStats* stats = nullptr, Span<const OffMeshLink> links = {});
    void Clear();
    bool IsEmpty() const { return m_polys.Empty(); }
    u32 TileCount() const { return m_tilesX * m_tilesZ; }

    /// Polygon under / nearest to `p` within `searchDistance` (meters), -1 if none; `onMesh` gets the snapped point.
    i32 FindNearestPoly(Vec3 p, f32 searchDistance, Vec3* onMesh = nullptr) const;
    /// Shortest path on the mesh (corners where it turns, start and end included), crossing off-mesh links where they
    /// are the way through. False if unreachable (the path then leads as close as the mesh allows when `partial`).
    /// `jumps` (one flag per path point) is 1 where the path leaves the mesh: the segment from the previous point to this
    /// one is an off-mesh link, and the caller moves the agent across it instead of walking.
    bool FindPath(Vec3 start, Vec3 end, Array<Vec3>& path, bool* partial = nullptr, Array<u8>* jumps = nullptr) const;
    /// Walks the mesh from `start` towards `end` (xz); true if the straight line stays on it. `hitT` gets the fraction
    /// of the way that was walkable.
    bool Raycast(Vec3 start, Vec3 end, f32* hitT = nullptr) const;
    /// Height of the mesh at (x, z) on polygon `poly` (bilinear over its corners).
    f32 HeightAt(u32 poly, f32 x, f32 z) const;

    Span<const Poly> Polys() const { return m_polys.View(); }
    Span<const Link> Links() const { return m_links.View(); }
    Span<const OffMeshLink> OffMeshLinks() const { return m_offLinks.View(); }
    const BuildSettings& Settings() const { return m_settings; }

    /// Binary form (saved next to scenes as .znav).
    void Serialize(Array<u8>& out) const;
    bool Deserialize(Span<const u8> data);

private:
    /// A walkable cell on a tile's edge (stitching): `along` = global cell index along the edge.
    struct EdgeCell
    {
        u32 along;
        u16 y, ceiling;
        u32 poly; // in the tile
    };
    struct Tile
    {
        Array<Poly> polys; // links local to the tile
        Array<Link> links;
        Array<EdgeCell> edges[4]; // -x, +z, +x, -z
    };
    void BuildTile(u32 tx, u32 tz, Span<const Vec3> vertices, Span<const u32> indices, Span<const AABB> obstacles, BuildStats& stats);
    /// Cell ranges [x0, x1) x [z0, z1) of a tile's core and of its voxelized area (core + border, inside the bounds).
    void TileCells(u32 tx, u32 tz, i32& x0, i32& x1, i32& z0, i32& z1, bool withBorder) const;
    /// All tiles into m_polys / m_links (offsets, stitches between neighbors), then the search grid and the off-mesh
    /// link endpoints on the polygons they connect (their indices move when tiles are rebuilt).
    void Assemble();
    void BuildGrid();
    void ResolveLinks();
    BuildSettings m_settings;
    i32 m_cellsX = 0, m_cellsZ = 0;          // cells over the bounds
    i32 m_tileCellsX = 0, m_tileCellsZ = 0;  // cells per tile side
    i32 m_border = 0;                        // cells voxelized around a tile
    u32 m_tilesX = 0, m_tilesZ = 0;
    Array<Tile> m_tiles;
    Array<Poly> m_polys;
    Array<Link> m_links;
    // Spatial grid of polygons (xz buckets) for nearest-polygon queries.
    f32 m_gridCell = 8.0f;
    Vec2 m_gridMin;
    u32 m_gridW = 0, m_gridH = 0;
    Array<u32> m_gridStart; // m_gridW * m_gridH + 1
    Array<u32> m_gridItems;
    // Off-mesh links as authored (endpoints snapped by the build) and the polygons they leave, for the search.
    Array<OffMeshLink> m_offLinks;
    Array<u32> m_linkStart; // m_polys.Size() + 1
    Array<u32> m_linkItems; // link index << 1 | crossed backwards
};

} // namespace zx::nav
