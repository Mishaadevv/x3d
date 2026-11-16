#include "Nav/NavMesh.h"
#include "Core/Containers/Algorithm.h"
#include "Core/Containers/HashMap.h"
#include "Core/Serialization/Binary.h"
#include "Core/Time/Time.h"

#include <cmath>

namespace zx::nav {

namespace {

constexpr u32 None = ~0u;
constexpr u8 AreaNone = 0, AreaWalkable = 1;
// Direction d: 0 = -x, 1 = +z, 2 = +x, 3 = -z.
constexpr i32 DirX[4] = {-1, 0, 1, 0};
constexpr i32 DirZ[4] = {0, 1, 0, -1};

struct HSpan
{
    u16 smin, smax; // cellHeight units above bounds.min.y
    u8 area;
    u32 next;       // next span up in the column
};

struct Heightfield
{
    i32 w = 0, h = 0;
    Vec3 origin;      // the bounds' min corner (shared by every tile: cell edges land on the same floats)
    i32 ox = 0, oz = 0; // the field's first cell in the bounds' grid
    f32 cs = 0.3f, ch = 0.2f;
    f32 X(i32 x) const { return origin.x + f32(x + ox) * cs; }
    f32 Z(i32 z) const { return origin.z + f32(z + oz) * cs; }
    i32 CellX(f32 wx) const { return i32(std::floor((wx - origin.x) / cs)) - ox; }
    i32 CellZ(f32 wz) const { return i32(std::floor((wz - origin.z) / cs)) - oz; }
    Array<u32> columns; // first span of each column (lowest)
    Array<HSpan> spans;

    void AddSpan(i32 x, i32 z, u16 smin, u16 smax, u8 area, i32 mergeClimb)
    {
        u32& head = columns[usize(z * w + x)];
        // Merge with every overlapping span (they are sorted bottom-up and never overlap each other).
        u32 prev = None, cur = head;
        while (cur != None) {
            HSpan& s = spans[cur];
            if (s.smin > smax)
                break;
            if (s.smax < smin) {
                prev = cur;
                cur = s.next;
                continue;
            }
            // Overlap: the union keeps the walkable flag of whichever top is (about) the new top.
            if (i32(s.smax) - i32(smax) > mergeClimb)
                area = s.area;
            else if (Abs(i32(s.smax) - i32(smax)) <= mergeClimb)
                area = Max(area, s.area);
            smin = Min(smin, s.smin);
            smax = Max(smax, s.smax);
            const u32 next = s.next;
            if (prev == None)
                head = next;
            else
                spans[prev].next = next;
            cur = next; // the merged span is dropped from the list (its slot is not reused)
        }
        HSpan n{smin, smax, area, cur};
        spans.PushBack(n);
        const u32 id = u32(spans.Size() - 1);
        if (prev == None)
            head = id;
        else
            spans[prev].next = id;
    }
};

/// Clips a convex polygon (xz + y) against the half-plane axis(v) * sign <= limit * sign (axis 0 = x, 2 = z).
u32 ClipPoly(const Vec3* in, u32 n, Vec3* out, i32 axis, f32 limit, f32 sign)
{
    u32 m = 0;
    for (u32 i = 0, j = n - 1; i < n; j = i++) {
        const f32 di = (in[i][axis] - limit) * sign, dj = (in[j][axis] - limit) * sign;
        const bool inI = di <= 0.0f, inJ = dj <= 0.0f;
        if (inI != inJ) {
            const f32 t = dj / (dj - di);
            out[m++] = in[j] + (in[i] - in[j]) * t;
        }
        if (inI)
            out[m++] = in[i];
    }
    return m;
}

void RasterizeTriangle(Heightfield& hf, Vec3 a, Vec3 b, Vec3 c, u8 area, i32 mergeClimb)
{
    const f32 bminX = Min(a.x, Min(b.x, c.x)), bmaxX = Max(a.x, Max(b.x, c.x));
    const f32 bminZ = Min(a.z, Min(b.z, c.z)), bmaxZ = Max(a.z, Max(b.z, c.z));
    const f32 bminY = Min(a.y, Min(b.y, c.y)), bmaxY = Max(a.y, Max(b.y, c.y));
    const f32 fieldTop = 65535.0f * hf.ch;
    if (bmaxY < hf.origin.y || bminY - hf.origin.y > fieldTop)
        return;
    const i32 x0 = Max(0, hf.CellX(bminX));
    const i32 x1 = Min(hf.w - 1, hf.CellX(bmaxX));
    const i32 z0 = Max(0, hf.CellZ(bminZ));
    const i32 z1 = Min(hf.h - 1, hf.CellZ(bmaxZ));
    if (x0 > x1 || z0 > z1)
        return;
    Vec3 tri[3] = {a, b, c};
    Vec3 row[7], rowTmp[7], cell[7], cellTmp[7];
    for (i32 z = z0; z <= z1; ++z) {
        const f32 cz0 = hf.Z(z), cz1 = hf.Z(z + 1);
        u32 n = ClipPoly(tri, 3, rowTmp, 2, cz0, -1.0f); // z >= cz0
        if (n < 3)
            continue;
        n = ClipPoly(rowTmp, n, row, 2, cz1, 1.0f); // z <= cz1
        if (n < 3)
            continue;
        for (i32 x = x0; x <= x1; ++x) {
            const f32 cx0 = hf.X(x), cx1 = hf.X(x + 1);
            u32 m = ClipPoly(row, n, cellTmp, 0, cx0, -1.0f);
            if (m < 3)
                continue;
            m = ClipPoly(cellTmp, m, cell, 0, cx1, 1.0f);
            if (m < 3)
                continue;
            f32 ymin = cell[0].y, ymax = cell[0].y;
            for (u32 i = 1; i < m; ++i) {
                ymin = Min(ymin, cell[i].y);
                ymax = Max(ymax, cell[i].y);
            }
            ymin -= hf.origin.y;
            ymax -= hf.origin.y;
            if (ymax < 0.0f)
                continue;
            const i32 smin = Clamp(i32(std::floor(ymin / hf.ch)), 0, 65534);
            const i32 smax = Clamp(i32(std::ceil(ymax / hf.ch)), smin + 1, 65535);
            hf.AddSpan(x, z, u16(smin), u16(smax), area, mergeClimb);
        }
    }
}

/// One walkable cell: the top of a walkable span with room for the agent above it.
struct Cell
{
    u16 y;          // floor (cellHeight units)
    u16 ceiling;    // next span's bottom (or max)
    i32 x, z;
    u32 conn[4];    // neighbor cell per direction, None if not connected
    u32 dist;       // chamfer distance to the nearest edge (2 per cell)
    u32 rect;
};

/// Min-heap of (cost, poly) for A*.
struct HeapItem
{
    f32 f;
    u32 poly;
};

void HeapPush(Array<HeapItem>& heap, HeapItem item)
{
    heap.PushBack(item);
    usize i = heap.Size() - 1;
    while (i > 0) {
        const usize p = (i - 1) / 2;
        if (heap[p].f <= heap[i].f)
            break;
        swap(heap[p], heap[i]);
        i = p;
    }
}

HeapItem HeapPop(Array<HeapItem>& heap)
{
    const HeapItem top = heap[0];
    heap[0] = heap.Back();
    heap.PopBack();
    usize i = 0;
    for (;;) {
        const usize l = i * 2 + 1, r = l + 1;
        usize m = i;
        if (l < heap.Size() && heap[l].f < heap[m].f)
            m = l;
        if (r < heap.Size() && heap[r].f < heap[m].f)
            m = r;
        if (m == i)
            break;
        swap(heap[m], heap[i]);
        i = m;
    }
    return top;
}

/// Twice the signed area of (a, b, c) in xz; <= 0 when c is to the right of a->b (the funnel's convention).
f32 TriArea2(Vec3 a, Vec3 b, Vec3 c)
{
    const f32 ax = b.x - a.x, az = b.z - a.z, bx = c.x - a.x, bz = c.z - a.z;
    return bx * az - ax * bz;
}

bool NearlyEqualXZ(Vec3 a, Vec3 b) { return Abs(a.x - b.x) < 1e-4f && Abs(a.z - b.z) < 1e-4f; }

constexpr u32 NavMagic = 0x56414E5A; // "ZNAV"
constexpr u32 NavVersion = 2; // 2: tiles

} // namespace

void NavMesh::Clear()
{
    m_tiles.Clear();
    m_tilesX = m_tilesZ = 0;
    m_cellsX = m_cellsZ = m_tileCellsX = m_tileCellsZ = m_border = 0;
    m_polys.Clear();
    m_links.Clear();
    m_gridStart.Clear();
    m_gridItems.Clear();
    m_gridW = m_gridH = 0;
}

void NavMesh::BuildTile(u32 tx, u32 tz, Span<const Vec3> vertices, Span<const u32> indices, Span<const AABB> obstacles, BuildStats& stats)
{
    const BuildSettings& s = m_settings;
    Tile& tile = m_tiles[usize(tz) * m_tilesX + tx];
    tile = Tile{};
    Array<Poly>& polys = tile.polys;
    Array<Link>& links = tile.links;
    // The tile's core (its rectangles) and the area voxelized around it (cells of the whole bounds' grid).
    i32 coreX0, coreX1, coreZ0, coreZ1, ax0, ax1, az0, az1;
    TileCells(tx, tz, coreX0, coreX1, coreZ0, coreZ1, false);
    TileCells(tx, tz, ax0, ax1, az0, az1, true);

    // 1. Voxelize.
    Heightfield hf;
    hf.cs = s.cellSize;
    hf.ch = s.cellHeight;
    hf.origin = s.bounds.min;
    hf.ox = ax0;
    hf.oz = az0;
    hf.w = ax1 - ax0;
    hf.h = az1 - az0;
    if (hf.w <= 0 || hf.h <= 0 || u64(hf.w) * u64(hf.h) > 64ull * 1024 * 1024)
        return; // too large for one tile: use tiles
    // Core in the tile's cells.
    const i32 cx0 = coreX0 - ax0, cx1 = coreX1 - ax0, cz0 = coreZ0 - az0, cz1 = coreZ1 - az0;
    auto inCore = [&](i32 x, i32 z) { return x >= cx0 && x < cx1 && z >= cz0 && z < cz1; };
    hf.columns.Resize(usize(hf.w) * usize(hf.h), None);
    const i32 climb = i32(std::floor(s.agentClimb / s.cellHeight));
    const i32 agentH = i32(std::ceil(s.agentHeight / s.cellHeight));
    const i32 radius = i32(std::ceil(s.agentRadius / s.cellSize));
    const f32 walkableY = Cos(DegToRad(s.maxSlope));
    for (usize t = 0; t + 2 < indices.Size(); t += 3) {
        const Vec3 a = vertices[indices[t]], b = vertices[indices[t + 1]], c = vertices[indices[t + 2]];
        const Vec3 n = Cross(b - a, c - a);
        const f32 len = Length(n);
        if (len <= 1e-12f)
            continue;
        const u8 area = n.y / len >= walkableY ? AreaWalkable : AreaNone;
        RasterizeTriangle(hf, a, b, c, area, climb);
    }

    // 2. Filters (Recast order): low obstacles become walkable, then ledges and low ceilings are not.
    for (u32 col = 0; col < hf.columns.Size(); ++col) {
        u32 prev = None;
        bool prevWalkable = false;
        u8 prevArea = AreaNone;
        for (u32 i = hf.columns[col]; i != None; i = hf.spans[i].next) {
            HSpan& sp = hf.spans[i];
            const bool walkable = sp.area != AreaNone;
            if (!walkable && prevWalkable && i32(sp.smax) - i32(hf.spans[prev].smax) <= climb)
                sp.area = prevArea;
            prevWalkable = walkable;
            prevArea = sp.area;
            prev = i;
        }
    }
    // Obstacles carve the floor under them (where the agent can't pass beneath).
    for (const AABB& o : obstacles) {
        const i32 ox0 = Max(0, hf.CellX(o.min.x)), ox1 = Min(hf.w - 1, hf.CellX(o.max.x));
        const i32 oz0 = Max(0, hf.CellZ(o.min.z)), oz1 = Min(hf.h - 1, hf.CellZ(o.max.z));
        for (i32 z = oz0; z <= oz1; ++z)
            for (i32 x = ox0; x <= ox1; ++x)
                for (u32 i = hf.columns[usize(z * hf.w + x)]; i != None; i = hf.spans[i].next) {
                    const f32 top = hf.origin.y + f32(hf.spans[i].smax) * hf.ch;
                    if (top >= o.min.y - s.agentHeight && top <= o.max.y + s.agentClimb)
                        hf.spans[i].area = AreaNone;
                }
    }
    auto ceilingOf = [&](const HSpan& sp) { return sp.next != None ? i32(hf.spans[sp.next].smin) : 0xFFFF + agentH * 2; };
    Array<u8> keep;
    keep.Resize(hf.spans.Size(), 0);
    for (i32 z = 0; z < hf.h; ++z)
        for (i32 x = 0; x < hf.w; ++x)
            for (u32 i = hf.columns[usize(z * hf.w + x)]; i != None; i = hf.spans[i].next) {
                const HSpan& sp = hf.spans[i];
                if (sp.area == AreaNone)
                    continue;
                const i32 floor = sp.smax, ceiling = ceilingOf(sp);
                if (ceiling - floor < agentH)
                    continue; // the agent does not fit
                // Ledges: a neighbor whose reachable floor is more than a step below.
                i32 minDrop = 0xFFFF;
                bool ledge = false;
                for (i32 d = 0; d < 4 && !ledge; ++d) {
                    const i32 nx = x + DirX[d], nz = z + DirZ[d];
                    if (nx < 0 || nz < 0 || nx >= hf.w || nz >= hf.h) {
                        ledge = true;
                        break;
                    }
                    const u32 first = hf.columns[usize(nz * hf.w + nx)];
                    // The space below the neighbor's first span counts as a floor at -infinity.
                    i32 nfloor = -agentH * 4, nceil = first != None ? i32(hf.spans[first].smin) : 0xFFFF;
                    if (Min(ceiling, nceil) - Max(floor, nfloor) > agentH)
                        minDrop = Min(minDrop, nfloor - floor);
                    for (u32 j = first; j != None; j = hf.spans[j].next) {
                        nfloor = hf.spans[j].smax;
                        nceil = ceilingOf(hf.spans[j]);
                        if (Min(ceiling, nceil) - Max(floor, nfloor) > agentH)
                            minDrop = Min(minDrop, nfloor - floor);
                    }
                    ledge = minDrop < -climb;
                }
                if (!ledge)
                    keep[i] = 1;
            }

    // 3. Cells and their connections.
    Array<Cell> cells;
    Array<u32> columnFirst, columnCount; // cells per column (bottom-up)
    columnFirst.Resize(hf.columns.Size(), 0);
    columnCount.Resize(hf.columns.Size(), 0);
    u32 spanCount = 0;
    for (i32 z = 0; z < hf.h; ++z)
        for (i32 x = 0; x < hf.w; ++x) {
            const usize col = usize(z * hf.w + x);
            columnFirst[col] = u32(cells.Size());
            for (u32 i = hf.columns[col]; i != None; i = hf.spans[i].next) {
                ++spanCount;
                if (!keep[i])
                    continue;
                Cell c{};
                c.y = hf.spans[i].smax;
                c.ceiling = u16(Min(ceilingOf(hf.spans[i]), 0xFFFF));
                c.x = x;
                c.z = z;
                c.rect = None;
                cells.PushBack(c);
            }
            columnCount[col] = u32(cells.Size()) - columnFirst[col];
        }
    for (Cell& c : cells)
        for (i32 d = 0; d < 4; ++d) {
            c.conn[d] = None;
            const i32 nx = c.x + DirX[d], nz = c.z + DirZ[d];
            if (nx < 0 || nz < 0 || nx >= hf.w || nz >= hf.h)
                continue;
            const usize col = usize(nz * hf.w + nx);
            i32 best = 0x7FFFFFFF;
            for (u32 k = 0; k < columnCount[col]; ++k) {
                const u32 j = columnFirst[col] + k;
                const Cell& n = cells[j];
                const i32 dy = Abs(i32(n.y) - i32(c.y));
                const i32 gap = Min(i32(c.ceiling), i32(n.ceiling)) - Max(i32(c.y), i32(n.y));
                if (dy <= climb && gap >= agentH && dy < best) {
                    best = dy;
                    c.conn[d] = j;
                }
            }
        }

    // 4. Erode by the agent radius (two-pass chamfer distance to the edge of the walkable area, 2 = one cell).
    for (Cell& c : cells) {
        bool edge = false;
        for (i32 d = 0; d < 4; ++d)
            edge |= c.conn[d] == None;
        c.dist = edge ? 0 : 0xFFFF;
    }
    auto relax = [&](Cell& c, i32 d, i32 d2) {
        const u32 n = c.conn[d];
        if (n == None)
            return;
        c.dist = Min(c.dist, cells[n].dist + 2);
        const u32 nn = cells[n].conn[d2];
        if (nn != None)
            c.dist = Min(c.dist, cells[nn].dist + 3);
    };
    for (i32 z = 0; z < hf.h; ++z) // forward: -x, -z and the diagonals behind
        for (i32 x = 0; x < hf.w; ++x) {
            const usize col = usize(z * hf.w + x);
            for (u32 k = 0; k < columnCount[col]; ++k) {
                Cell& c = cells[columnFirst[col] + k];
                relax(c, 0, 3);
                relax(c, 3, 2);
            }
        }
    for (i32 z = hf.h - 1; z >= 0; --z) // backward: +x, +z and the diagonals ahead
        for (i32 x = hf.w - 1; x >= 0; --x) {
            const usize col = usize(z * hf.w + x);
            for (u32 k = 0; k < columnCount[col]; ++k) {
                Cell& c = cells[columnFirst[col] + k];
                relax(c, 2, 1);
                relax(c, 1, 0);
            }
        }
    Array<u8> alive;
    alive.Resize(cells.Size(), 0);
    for (usize i = 0; i < cells.Size(); ++i)
        alive[i] = cells[i].dist >= u32(radius * 2) ? 1 : 0;
    auto unlinkDead = [&] {
        for (usize i = 0; i < cells.Size(); ++i)
            for (i32 d = 0; d < 4; ++d)
                if (cells[i].conn[d] != None && (!alive[i] || !alive[cells[i].conn[d]]))
                    cells[i].conn[d] = None;
    };
    unlinkDead();

    // Drop small islands (flood fill over the connections; ones reaching the border may go on in a neighbor tile).
    {
        Array<u32> island, stack;
        island.Resize(cells.Size(), None);
        u32 islands = 0;
        for (usize i = 0; i < cells.Size(); ++i) {
            if (!alive[i] || island[i] != None)
                continue;
            Array<u32> members;
            bool reachesBorder = false;
            stack.PushBack(u32(i));
            island[i] = islands;
            while (!stack.Empty()) {
                const u32 c = stack.PopBackValue();
                members.PushBack(c);
                reachesBorder |= !inCore(cells[c].x, cells[c].z);
                for (i32 d = 0; d < 4; ++d) {
                    const u32 n = cells[c].conn[d];
                    if (n != None && island[n] == None) {
                        island[n] = islands;
                        stack.PushBack(n);
                    }
                }
            }
            if (members.Size() < s.minRegionCells && (!reachesBorder || m_border == 0))
                for (u32 m : members)
                    alive[m] = 0;
            ++islands;
        }
    }
    // Only the core becomes rectangles (the border belongs to the neighbor tiles).
    for (usize i = 0; i < cells.Size(); ++i)
        if (!inCore(cells[i].x, cells[i].z))
            alive[i] = 0;
    unlinkDead();
    u32 walkable = 0;
    for (u8 a : alive)
        walkable += a;

    // 5. Rectangles: grow along +x, then add rows along +z while every cell links up with the row before it.
    const u32 maxSide = Max(1u, s.maxRectCells);
    Array<u32> rowStarts;
    for (i32 z = 0; z < hf.h; ++z)
        for (i32 x = 0; x < hf.w; ++x) {
            const usize col = usize(z * hf.w + x);
            for (u32 k = 0; k < columnCount[col]; ++k) {
                const u32 start = columnFirst[col] + k;
                if (!alive[start] || cells[start].rect != None)
                    continue;
                const u32 id = u32(polys.Size());
                // Width of the first row.
                u32 w = 1;
                for (u32 c = start; w < maxSide;) {
                    const u32 n = cells[c].conn[2];
                    if (n == None || cells[n].rect != None)
                        break;
                    c = n;
                    ++w;
                }
                rowStarts.Clear();
                rowStarts.PushBack(start);
                // Further rows: each cell must be the +z neighbor of the cell below and link +x to the next one.
                while (rowStarts.Size() < maxSide) {
                    const u32 below = rowStarts.Back();
                    const u32 first = cells[below].conn[1];
                    if (first == None || cells[first].rect != None)
                        break;
                    bool ok = true;
                    u32 b = below, c = first;
                    for (u32 i = 1; i < w && ok; ++i) {
                        const u32 nb = cells[b].conn[2], nc = cells[c].conn[2];
                        ok = nb != None && nc != None && cells[nc].rect == None && cells[nb].conn[1] == nc;
                        b = nb;
                        c = nc;
                    }
                    if (!ok)
                        break;
                    rowStarts.PushBack(first);
                }
                const u32 rows = u32(rowStarts.Size());
                // Claim the cells.
                f32 minY = 1e30f, maxY = -1e30f;
                for (u32 r = 0; r < rows; ++r) {
                    u32 c = rowStarts[r];
                    for (u32 i = 0; i < w; ++i) {
                        cells[c].rect = id;
                        const f32 y = hf.origin.y + f32(cells[c].y) * hf.ch;
                        minY = Min(minY, y);
                        maxY = Max(maxY, y);
                        if (i + 1 < w)
                            c = cells[c].conn[2];
                    }
                }
                // Corners: the floors of the corner cells.
                auto cellY = [&](u32 c) { return hf.origin.y + f32(cells[c].y) * hf.ch; };
                u32 endFirst = rowStarts[0], endLast = rowStarts[rows - 1];
                for (u32 i = 1; i < w; ++i) {
                    endFirst = cells[endFirst].conn[2];
                    endLast = cells[endLast].conn[2];
                }
                Poly p;
                const f32 wx0 = hf.X(x), wx1 = hf.X(x + i32(w));
                const f32 wz0 = hf.Z(z), wz1 = hf.Z(z + i32(rows));
                p.corners[0] = Vec3(wx0, cellY(rowStarts[0]), wz0);
                p.corners[1] = Vec3(wx1, cellY(endFirst), wz0);
                p.corners[2] = Vec3(wx1, cellY(endLast), wz1);
                p.corners[3] = Vec3(wx0, cellY(rowStarts[rows - 1]), wz1);
                p.minY = minY;
                p.maxY = maxY;
                polys.PushBack(p);
            }
        }

    // 6. Links: runs of edge cells whose outside neighbors belong to the same rectangle.
    {
        // Index the cells of each rectangle's corner so edges can be walked: store the first cell.
        Array<u32> rectFirst;
        rectFirst.Resize(polys.Size(), None);
        for (u32 i = 0; i < cells.Size(); ++i)
            if (cells[i].rect != None && rectFirst[cells[i].rect] == None)
                rectFirst[cells[i].rect] = i; // scan order: the (x0, z0) cell comes first
        for (u32 pi = 0; pi < polys.Size(); ++pi) {
            Poly& p = polys[pi];
            p.firstLink = u32(links.Size());
            const u32 w = u32(std::lround((p.corners[1].x - p.corners[0].x) / hf.cs));
            const u32 rows = u32(std::lround((p.corners[3].z - p.corners[0].z) / hf.cs));
            // Walk an edge: `start` cell, step direction along the edge, outward direction.
            auto walkEdge = [&](u32 start, i32 along, u32 count, i32 out) {
                u32 runTo = None;
                Vec3 runA;
                u32 c = start;
                auto edgePoint = [&](u32 cell, bool far) {
                    // Point on the edge at the start (or end) of `cell`, y halfway to the neighbor's floor.
                    const Cell& e = cells[cell];
                    f32 ex = hf.X(e.x), ez = hf.Z(e.z);
                    if (out == 2)
                        ex += hf.cs;
                    if (out == 1)
                        ez += hf.cs;
                    const f32 t = far ? hf.cs : 0.0f;
                    if (along == 2)
                        ex += t;
                    if (along == 1)
                        ez += t;
                    f32 y = hf.origin.y + f32(e.y) * hf.ch;
                    if (e.conn[out] != None)
                        y = (y + hf.origin.y + f32(cells[e.conn[out]].y) * hf.ch) * 0.5f;
                    return Vec3(ex, y, ez);
                };
                u32 last = c;
                for (u32 i = 0; i < count; ++i) {
                    const u32 n = cells[c].conn[out];
                    const u32 to = n != None ? cells[n].rect : None;
                    if (to != runTo) {
                        if (runTo != None)
                            links.PushBack(Link{runTo, runA, edgePoint(last, true)});
                        runTo = to;
                        if (to != None)
                            runA = edgePoint(c, false);
                    }
                    last = c;
                    if (i + 1 < count)
                        c = cells[c].conn[along];
                }
                if (runTo != None)
                    links.PushBack(Link{runTo, runA, edgePoint(last, true)});
            };
            const u32 c00 = rectFirst[pi];
            u32 c10 = c00, c01 = c00;
            for (u32 i = 1; i < w; ++i)
                c10 = cells[c10].conn[2];
            for (u32 i = 1; i < rows; ++i)
                c01 = cells[c01].conn[1];
            walkEdge(c00, 2, w, 3);    // -z edge
            walkEdge(c01, 2, w, 1);    // +z edge
            walkEdge(c00, 1, rows, 0); // -x edge
            walkEdge(c10, 1, rows, 2); // +x edge
            p.linkCount = u32(links.Size()) - p.firstLink;
        }
    }

    // 7. Edge cells (stitching with the neighbor tiles): walkable core cells on the core's sides.
    for (usize i = 0; i < cells.Size(); ++i) {
        const Cell& c = cells[i];
        if (!alive[i] || c.rect == None)
            continue;
        const u32 gx = u32(c.x + ax0), gz = u32(c.z + az0);
        const EdgeCell xe{gz, c.y, c.ceiling, c.rect}, ze{gx, c.y, c.ceiling, c.rect};
        if (c.x == cx0)
            tile.edges[0].PushBack(xe);
        if (c.x == cx1 - 1)
            tile.edges[2].PushBack(xe);
        if (c.z == cz1 - 1)
            tile.edges[1].PushBack(ze);
        if (c.z == cz0)
            tile.edges[3].PushBack(ze);
    }
    stats.spans += spanCount;
    stats.walkableCells += walkable;
    ++stats.tilesBuilt;
}

void NavMesh::TileCells(u32 tx, u32 tz, i32& x0, i32& x1, i32& z0, i32& z1, bool withBorder) const
{
    const i32 b = withBorder ? m_border : 0;
    x0 = Max(i32(tx) * m_tileCellsX - b, 0);
    x1 = Min((i32(tx) + 1) * m_tileCellsX + b, m_cellsX);
    z0 = Max(i32(tz) * m_tileCellsZ - b, 0);
    z1 = Min((i32(tz) + 1) * m_tileCellsZ + b, m_cellsZ);
}

bool NavMesh::Build(Span<const Vec3> vertices, Span<const u32> indices, const BuildSettings& s, BuildStats* stats, Span<const AABB> obstacles)
{
    Stopwatch timer;
    Clear();
    m_settings = s;
    if (!s.bounds.IsValid() || s.cellSize <= 0.0f || s.cellHeight <= 0.0f)
        return false;
    const Vec3 size = s.bounds.Size();
    m_cellsX = Max(1, i32(std::ceil(size.x / s.cellSize)));
    m_cellsZ = Max(1, i32(std::ceil(size.z / s.cellSize)));
    if (s.tileSize > 0.0f) {
        m_tileCellsX = m_tileCellsZ = Max(16, i32(std::lround(s.tileSize / s.cellSize)));
        // Wide enough for the erosion and the ledge test next to the tile's edge to see what a single build would.
        m_border = i32(std::ceil(s.agentRadius / s.cellSize)) + 3;
    } else {
        m_tileCellsX = m_cellsX;
        m_tileCellsZ = m_cellsZ;
        m_border = 0;
    }
    m_tilesX = u32((m_cellsX + m_tileCellsX - 1) / m_tileCellsX);
    m_tilesZ = u32((m_cellsZ + m_tileCellsZ - 1) / m_tileCellsZ);
    m_tiles.Resize(usize(m_tilesX) * m_tilesZ);
    BuildStats st;
    for (u32 tz = 0; tz < m_tilesZ; ++tz)
        for (u32 tx = 0; tx < m_tilesX; ++tx)
            BuildTile(tx, tz, vertices, indices, obstacles, st);
    Assemble();
    if (stats) {
        *stats = st;
        stats->columnsX = u32(m_cellsX);
        stats->columnsZ = u32(m_cellsZ);
        stats->polys = u32(m_polys.Size());
        stats->links = u32(m_links.Size());
        stats->milliseconds = timer.ElapsedMilliseconds();
    }
    return !m_polys.Empty();
}

AABB NavMesh::TileBuildBounds(const AABB& area) const
{
    AABB out;
    if (m_tiles.Empty())
        return out;
    const f32 cs = m_settings.cellSize;
    const Vec3 o = m_settings.bounds.min;
    const i32 tx0 = Max(0, i32(std::floor((area.min.x - o.x) / (cs * f32(m_tileCellsX))))), tx1 = Min(i32(m_tilesX) - 1, i32(std::floor((area.max.x - o.x) / (cs * f32(m_tileCellsX)))));
    const i32 tz0 = Max(0, i32(std::floor((area.min.z - o.z) / (cs * f32(m_tileCellsZ))))), tz1 = Min(i32(m_tilesZ) - 1, i32(std::floor((area.max.z - o.z) / (cs * f32(m_tileCellsZ)))));
    if (tx0 > tx1 || tz0 > tz1 || area.max.y < m_settings.bounds.min.y || area.min.y > m_settings.bounds.max.y)
        return out;
    i32 x0, x1, z0, z1, x2, x3, z2, z3;
    TileCells(u32(tx0), u32(tz0), x0, x1, z0, z1, true);
    TileCells(u32(tx1), u32(tz1), x2, x3, z2, z3, true);
    out.min = Vec3(o.x + f32(x0) * cs, m_settings.bounds.min.y, o.z + f32(z0) * cs);
    out.max = Vec3(o.x + f32(x3) * cs, m_settings.bounds.max.y, o.z + f32(z3) * cs);
    return out;
}

bool NavMesh::RebuildTiles(Span<const Vec3> vertices, Span<const u32> indices, const AABB& area, Span<const AABB> obstacles, BuildStats* stats)
{
    Stopwatch timer;
    if (m_tiles.Empty())
        return false;
    const f32 cs = m_settings.cellSize;
    const Vec3 o = m_settings.bounds.min;
    const i32 tx0 = Max(0, i32(std::floor((area.min.x - o.x) / (cs * f32(m_tileCellsX))))), tx1 = Min(i32(m_tilesX) - 1, i32(std::floor((area.max.x - o.x) / (cs * f32(m_tileCellsX)))));
    const i32 tz0 = Max(0, i32(std::floor((area.min.z - o.z) / (cs * f32(m_tileCellsZ))))), tz1 = Min(i32(m_tilesZ) - 1, i32(std::floor((area.max.z - o.z) / (cs * f32(m_tileCellsZ)))));
    if (tx0 > tx1 || tz0 > tz1 || area.max.y < m_settings.bounds.min.y || area.min.y > m_settings.bounds.max.y)
        return false;
    BuildStats st;
    for (i32 tz = tz0; tz <= tz1; ++tz)
        for (i32 tx = tx0; tx <= tx1; ++tx)
            BuildTile(u32(tx), u32(tz), vertices, indices, obstacles, st);
    Assemble();
    if (stats) {
        *stats = st;
        stats->columnsX = u32(m_cellsX);
        stats->columnsZ = u32(m_cellsZ);
        stats->polys = u32(m_polys.Size());
        stats->links = u32(m_links.Size());
        stats->milliseconds = timer.ElapsedMilliseconds();
    }
    return st.tilesBuilt > 0;
}

void NavMesh::Assemble()
{
    m_polys.Clear();
    m_links.Clear();
    Array<u32> offset;
    offset.Resize(m_tiles.Size(), 0);
    u32 total = 0;
    for (usize t = 0; t < m_tiles.Size(); ++t) {
        offset[t] = total;
        total += u32(m_tiles[t].polys.Size());
    }
    // Stitches: walkable cells facing each other across a tile edge (within a step, room for the agent) link their
    // rectangles; consecutive cells of one pair of rectangles make one portal.
    struct Cross
    {
        u32 from;
        Link link;
    };
    Array<Cross> cross;
    const f32 cs = m_settings.cellSize, ch = m_settings.cellHeight;
    const Vec3 o = m_settings.bounds.min;
    const i32 climb = i32(std::floor(m_settings.agentClimb / ch));
    const i32 agentH = i32(std::ceil(m_settings.agentHeight / ch));
    struct Pair
    {
        u32 a, b;      // global polys
        u32 along;
        f32 y;         // portal height (between the two floors)
    };
    Array<Pair> pairs;
    for (u32 tz = 0; tz < m_tilesZ; ++tz)
        for (u32 tx = 0; tx < m_tilesX; ++tx)
            for (u32 dir : {2u, 1u}) { // +x and +z neighbors (each edge once)
                const u32 nx = tx + (dir == 2 ? 1 : 0), nz = tz + (dir == 1 ? 1 : 0);
                if (nx >= m_tilesX || nz >= m_tilesZ)
                    continue;
                const usize ta = usize(tz) * m_tilesX + tx, tb = usize(nz) * m_tilesX + nx;
                const Array<EdgeCell>& ea = m_tiles[ta].edges[dir];
                const Array<EdgeCell>& eb = m_tiles[tb].edges[dir == 2 ? 0 : 3];
                if (ea.Empty() || eb.Empty())
                    continue;
                // Facing cells by `along`.
                HashMap<u32, Array<u32>> byAlong;
                for (u32 j = 0; j < eb.Size(); ++j) {
                    Array<u32>* list = byAlong.Find(eb[j].along);
                    if (!list) {
                        byAlong.InsertOrAssign(eb[j].along, Array<u32>{});
                        list = byAlong.Find(eb[j].along);
                    }
                    list->PushBack(j);
                }
                pairs.Clear();
                for (const EdgeCell& c : ea) {
                    const Array<u32>* list = byAlong.Find(c.along);
                    if (!list)
                        continue;
                    i32 best = 0x7FFFFFFF;
                    u32 bestJ = None;
                    for (u32 j : *list) {
                        const EdgeCell& n = eb[j];
                        const i32 dy = Abs(i32(n.y) - i32(c.y));
                        const i32 gap = Min(i32(c.ceiling), i32(n.ceiling)) - Max(i32(c.y), i32(n.y));
                        if (dy <= climb && gap >= agentH && dy < best) {
                            best = dy;
                            bestJ = j;
                        }
                    }
                    if (bestJ != None)
                        pairs.PushBack(Pair{offset[ta] + c.poly, offset[tb] + eb[bestJ].poly, c.along, o.y + (f32(c.y) + f32(eb[bestJ].y)) * 0.5f * ch});
                }
                Sort(pairs, [](const Pair& l, const Pair& r) {
                    return l.a != r.a ? l.a < r.a : l.b != r.b ? l.b < r.b : l.along < r.along;
                });
                // The edge line: x (dir 2) or z (dir 1) of the shared side.
                const f32 line = dir == 2 ? o.x + f32((i32(tx) + 1) * m_tileCellsX) * cs : o.z + f32((i32(tz) + 1) * m_tileCellsZ) * cs;
                auto point = [&](u32 along, f32 y) {
                    return dir == 2 ? Vec3(line, y, o.z + f32(along) * cs) : Vec3(o.x + f32(along) * cs, y, line);
                };
                for (usize i = 0; i < pairs.Size();) {
                    usize j = i;
                    while (j + 1 < pairs.Size() && pairs[j + 1].a == pairs[i].a && pairs[j + 1].b == pairs[i].b && pairs[j + 1].along == pairs[j].along + 1)
                        ++j;
                    const Vec3 pa = point(pairs[i].along, pairs[i].y), pb = point(pairs[j].along + 1, pairs[j].y);
                    cross.PushBack(Cross{pairs[i].a, Link{pairs[i].b, pa, pb}});
                    cross.PushBack(Cross{pairs[i].b, Link{pairs[i].a, pa, pb}});
                    i = j + 1;
                }
            }
    Sort(cross, [](const Cross& l, const Cross& r) { return l.from < r.from; });
    m_polys.Reserve(total);
    usize ci = 0;
    for (usize t = 0; t < m_tiles.Size(); ++t) {
        const Tile& tile = m_tiles[t];
        for (u32 pi = 0; pi < tile.polys.Size(); ++pi) {
            Poly p = tile.polys[pi];
            const u32 global = offset[t] + pi;
            const u32 first = u32(m_links.Size());
            for (u32 l = 0; l < p.linkCount; ++l) {
                Link link = tile.links[p.firstLink + l];
                link.to += offset[t];
                m_links.PushBack(link);
            }
            while (ci < cross.Size() && cross[ci].from < global)
                ++ci;
            while (ci < cross.Size() && cross[ci].from == global)
                m_links.PushBack(cross[ci++].link);
            p.firstLink = first;
            p.linkCount = u32(m_links.Size()) - first;
            m_polys.PushBack(p);
        }
    }
    BuildGrid();
}

void NavMesh::BuildGrid()
{
    m_gridStart.Clear();
    m_gridItems.Clear();
    m_gridW = m_gridH = 0;
    if (m_polys.Empty())
        return;
    Vec2 mn(1e30f), mx(-1e30f);
    for (const Poly& p : m_polys) {
        mn = Min(mn, Vec2(p.corners[0].x, p.corners[0].z));
        mx = Max(mx, Vec2(p.corners[2].x, p.corners[2].z));
    }
    m_gridMin = mn;
    m_gridCell = 8.0f;
    m_gridW = u32(std::ceil((mx.x - mn.x) / m_gridCell)) + 1;
    m_gridH = u32(std::ceil((mx.y - mn.y) / m_gridCell)) + 1;
    auto range = [&](const Poly& p, u32& x0, u32& x1, u32& z0, u32& z1) {
        x0 = u32((p.corners[0].x - mn.x) / m_gridCell);
        z0 = u32((p.corners[0].z - mn.y) / m_gridCell);
        x1 = Min(m_gridW - 1, u32((p.corners[2].x - mn.x) / m_gridCell));
        z1 = Min(m_gridH - 1, u32((p.corners[2].z - mn.y) / m_gridCell));
    };
    Array<u32> counts;
    counts.Resize(usize(m_gridW) * m_gridH + 1, 0);
    for (const Poly& p : m_polys) {
        u32 x0, x1, z0, z1;
        range(p, x0, x1, z0, z1);
        for (u32 z = z0; z <= z1; ++z)
            for (u32 x = x0; x <= x1; ++x)
                ++counts[z * m_gridW + x];
    }
    m_gridStart.Resize(counts.Size(), 0);
    u32 sum = 0;
    for (usize i = 0; i < counts.Size(); ++i) {
        m_gridStart[i] = sum;
        sum += counts[i];
        counts[i] = 0;
    }
    m_gridItems.Resize(sum, 0);
    for (u32 pi = 0; pi < m_polys.Size(); ++pi) {
        u32 x0, x1, z0, z1;
        range(m_polys[pi], x0, x1, z0, z1);
        for (u32 z = z0; z <= z1; ++z)
            for (u32 x = x0; x <= x1; ++x) {
                const u32 b = z * m_gridW + x;
                m_gridItems[m_gridStart[b] + counts[b]++] = pi;
            }
    }
}

f32 NavMesh::HeightAt(u32 poly, f32 x, f32 z) const
{
    const Poly& p = m_polys[poly];
    const f32 sx = p.corners[1].x - p.corners[0].x, sz = p.corners[3].z - p.corners[0].z;
    const f32 u = Clamp((x - p.corners[0].x) / Max(sx, 1e-6f), 0.0f, 1.0f);
    const f32 v = Clamp((z - p.corners[0].z) / Max(sz, 1e-6f), 0.0f, 1.0f);
    const f32 y0 = Lerp(p.corners[0].y, p.corners[1].y, u), y1 = Lerp(p.corners[3].y, p.corners[2].y, u);
    return Lerp(y0, y1, v);
}

i32 NavMesh::FindNearestPoly(Vec3 p, f32 searchDistance, Vec3* onMesh) const
{
    if (m_polys.Empty())
        return -1;
    const f32 r = Max(searchDistance, 0.0f);
    const i32 x0 = Max(0, i32(std::floor((p.x - r - m_gridMin.x) / m_gridCell)));
    const i32 z0 = Max(0, i32(std::floor((p.z - r - m_gridMin.y) / m_gridCell)));
    const i32 x1 = Min(i32(m_gridW) - 1, i32(std::floor((p.x + r - m_gridMin.x) / m_gridCell)));
    const i32 z1 = Min(i32(m_gridH) - 1, i32(std::floor((p.z + r - m_gridMin.y) / m_gridCell)));
    i32 best = -1;
    f32 bestD = r * r;
    Vec3 bestP;
    for (i32 z = z0; z <= z1; ++z)
        for (i32 x = x0; x <= x1; ++x) {
            const u32 b = u32(z) * m_gridW + u32(x);
            for (u32 k = m_gridStart[b]; k < m_gridStart[b + 1]; ++k) {
                const u32 pi = m_gridItems[k];
                const Poly& poly = m_polys[pi];
                const f32 cx = Clamp(p.x, poly.corners[0].x, poly.corners[2].x);
                const f32 cz = Clamp(p.z, poly.corners[0].z, poly.corners[2].z);
                const Vec3 q(cx, HeightAt(pi, cx, cz), cz);
                Vec3 d = q - p;
                // A point a little above the floor (an agent's feet, a camera's pick) belongs to the floor below.
                if (d.y < 0.0f && d.y > -m_settings.agentHeight)
                    d.y *= 0.25f;
                const f32 dist = LengthSq(d);
                if (dist <= bestD) {
                    bestD = dist;
                    best = i32(pi);
                    bestP = q;
                }
            }
        }
    if (best >= 0 && onMesh)
        *onMesh = bestP;
    return best;
}

bool NavMesh::FindPath(Vec3 start, Vec3 end, Array<Vec3>& path, bool* partial) const
{
    path.Clear();
    if (partial)
        *partial = false;
    const f32 search = Max(m_settings.agentHeight * 2.0f, m_settings.agentRadius * 4.0f + 1.0f);
    Vec3 s, e;
    const i32 sp = FindNearestPoly(start, search, &s);
    if (sp < 0)
        return false;
    i32 ep = FindNearestPoly(end, search, &e);
    // A* over the polygons; a node sits where the path entered its polygon (the portal's closest point).
    const u32 n = u32(m_polys.Size());
    Array<f32> g;
    Array<u32> parent, parentLink;
    Array<Vec3> pos;
    Array<u8> closed;
    g.Resize(n, 1e30f);
    parent.Resize(n, None);
    parentLink.Resize(n, None);
    pos.Resize(n, Vec3());
    closed.Resize(n, 0);
    Array<HeapItem> open;
    g[u32(sp)] = 0.0f;
    pos[u32(sp)] = s;
    const Vec3 goal = ep >= 0 ? e : end;
    HeapPush(open, {Distance(s, goal), u32(sp)});
    u32 bestPoly = u32(sp);
    f32 bestH = Distance(s, goal);
    bool found = false;
    while (!open.Empty()) {
        const HeapItem it = HeapPop(open);
        const u32 cur = it.poly;
        if (closed[cur])
            continue;
        closed[cur] = 1;
        if (i32(cur) == ep) {
            found = true;
            break;
        }
        const f32 h = Distance(pos[cur], goal);
        if (h < bestH) {
            bestH = h;
            bestPoly = cur;
        }
        const Poly& p = m_polys[cur];
        for (u32 li = p.firstLink; li < p.firstLink + p.linkCount; ++li) {
            const Link& l = m_links[li];
            if (closed[l.to])
                continue;
            // Enter at the portal point closest to the straight line towards the goal (clamped segment midpoint).
            const Vec3 ab = l.b - l.a;
            const f32 len2 = Max(LengthSq(ab), 1e-8f);
            const f32 t = Clamp(Dot((pos[cur] + goal) * 0.5f - l.a, ab) / len2, 0.05f, 0.95f);
            const Vec3 q = l.a + ab * t;
            f32 cost = g[cur] + Distance(pos[cur], q);
            if (i32(l.to) == ep)
                cost += Distance(q, e);
            if (cost < g[l.to]) {
                g[l.to] = cost;
                parent[l.to] = cur;
                parentLink[l.to] = li;
                pos[l.to] = q;
                HeapPush(open, {cost + (i32(l.to) == ep ? 0.0f : Distance(q, goal)), l.to});
            }
        }
    }
    u32 last = found ? u32(ep) : bestPoly;
    if (!found) {
        if (partial)
            *partial = true;
        const Poly& p = m_polys[last];
        const f32 cx = Clamp(goal.x, p.corners[0].x, p.corners[2].x), cz = Clamp(goal.z, p.corners[0].z, p.corners[2].z);
        e = Vec3(cx, HeightAt(last, cx, cz), cz);
    }
    // Corridor (polygon links from the start).
    Array<u32> corridor;
    for (u32 c = last; c != u32(sp); c = parent[c])
        corridor.PushBack(parentLink[c]);
    // Portals oriented (left, right) for the funnel; start and end are degenerate portals.
    Array<Vec3> lefts, rights;
    lefts.PushBack(s);
    rights.PushBack(s);
    u32 from = u32(sp);
    for (usize i = corridor.Size(); i-- > 0;) {
        const Link& l = m_links[corridor[i]];
        const Poly& a = m_polys[from];
        const Poly& b = m_polys[l.to];
        const Vec3 ca = (a.corners[0] + a.corners[2]) * 0.5f, cb = (b.corners[0] + b.corners[2]) * 0.5f;
        const Vec3 d = cb - ca;
        // `a` of the link is right of `b` when, facing along d, it lies on the +x side for d = +z.
        const bool aRight = d.z * (l.a.x - l.b.x) - d.x * (l.a.z - l.b.z) > 0.0f;
        // Pull the portal in by the agent radius is unnecessary: erosion already keeps the mesh off the walls.
        lefts.PushBack(aRight ? l.b : l.a);
        rights.PushBack(aRight ? l.a : l.b);
        from = l.to;
    }
    lefts.PushBack(e);
    rights.PushBack(e);

    // Simple stupid funnel algorithm (Mononen).
    path.PushBack(s);
    Vec3 apex = s, left = lefts[0], right = rights[0];
    usize apexIndex = 0, leftIndex = 0, rightIndex = 0;
    for (usize i = 1; i < lefts.Size(); ++i) {
        const Vec3 l = lefts[i], r = rights[i];
        if (TriArea2(apex, right, r) <= 0.0f) { // the right side tightens
            if (NearlyEqualXZ(apex, right) || TriArea2(apex, left, r) > 0.0f) {
                right = r;
                rightIndex = i;
            } else { // crossed over the left: the left point becomes a corner
                path.PushBack(left);
                apex = left;
                apexIndex = leftIndex;
                left = right = apex;
                leftIndex = rightIndex = apexIndex;
                i = apexIndex;
                continue;
            }
        }
        if (TriArea2(apex, left, l) >= 0.0f) { // the left side tightens
            if (NearlyEqualXZ(apex, left) || TriArea2(apex, right, l) < 0.0f) {
                left = l;
                leftIndex = i;
            } else {
                path.PushBack(right);
                apex = right;
                apexIndex = rightIndex;
                left = right = apex;
                leftIndex = rightIndex = apexIndex;
                i = apexIndex;
                continue;
            }
        }
    }
    if (!NearlyEqualXZ(path.Back(), e) || Abs(path.Back().y - e.y) > 1e-3f)
        path.PushBack(e);
    // The corridor may pass a corner where four rectangles meet on the wrong side; drop corners the mesh can see past.
    for (usize i = 0; i + 2 < path.Size();) {
        usize j = path.Size() - 1;
        while (j > i + 1 && !Raycast(path[i], path[j]))
            --j;
        if (j > i + 1)
            path.RemoveAt(i + 1, j - i - 1);
        ++i;
    }
    return found;
}

bool NavMesh::Raycast(Vec3 start, Vec3 end, f32* hitT) const
{
    if (hitT)
        *hitT = 0.0f;
    i32 cur = FindNearestPoly(start, m_settings.agentHeight, nullptr);
    if (cur < 0)
        return false;
    const f32 dx = end.x - start.x, dz = end.z - start.z;
    constexpr f32 Eps = 1e-3f;
    f32 t = 0.0f;
    for (u32 guard = 0; guard < 4096; ++guard) {
        const Poly& p = m_polys[u32(cur)];
        // Exit of the segment from this rectangle (slab test in xz).
        f32 exit = 1.0f;
        if (dx > 1e-9f)
            exit = Min(exit, (p.corners[2].x - start.x) / dx);
        else if (dx < -1e-9f)
            exit = Min(exit, (p.corners[0].x - start.x) / dx);
        if (dz > 1e-9f)
            exit = Min(exit, (p.corners[2].z - start.z) / dz);
        else if (dz < -1e-9f)
            exit = Min(exit, (p.corners[0].z - start.z) / dz);
        exit = Max(exit, t);
        if (exit >= 1.0f) {
            if (hitT)
                *hitT = 1.0f;
            return true;
        }
        // The next rectangle holds the point just past the exit (near a corner, a diagonal neighbor of a neighbor).
        const f32 len = Sqrt(dx * dx + dz * dz);
        const f32 step = Eps / Max(len, 1e-6f);
        const f32 px = start.x + dx * (exit + step), pz = start.z + dz * (exit + step);
        auto contains = [&](u32 q) {
            const Poly& o = m_polys[q];
            return px >= o.corners[0].x - Eps && px <= o.corners[2].x + Eps && pz >= o.corners[0].z - Eps && pz <= o.corners[2].z + Eps;
        };
        i32 next = -1;
        for (u32 li = p.firstLink; li < p.firstLink + p.linkCount && next < 0; ++li)
            if (contains(m_links[li].to))
                next = i32(m_links[li].to);
        for (u32 li = p.firstLink; li < p.firstLink + p.linkCount && next < 0; ++li) {
            const Poly& n = m_polys[m_links[li].to];
            for (u32 lj = n.firstLink; lj < n.firstLink + n.linkCount && next < 0; ++lj)
                if (m_links[lj].to != u32(cur) && contains(m_links[lj].to))
                    next = i32(m_links[lj].to);
        }
        if (next < 0) {
            if (hitT)
                *hitT = exit;
            return false;
        }
        cur = next;
        t = exit;
    }
    return false;
}

void NavMesh::Serialize(Array<u8>& out) const
{
    BinaryWriter w(out);
    w.Write(NavMagic);
    w.Write(NavVersion);
    w.Write(m_settings);
    w.Write(m_cellsX);
    w.Write(m_cellsZ);
    w.Write(m_tileCellsX);
    w.Write(m_tileCellsZ);
    w.Write(m_border);
    w.Write(m_tilesX);
    w.Write(m_tilesZ);
    for (const Tile& t : m_tiles) {
        w.WriteArray(t.polys.View());
        w.WriteArray(t.links.View());
        for (const Array<EdgeCell>& e : t.edges)
            w.WriteArray(e.View());
    }
}

bool NavMesh::Deserialize(Span<const u8> data)
{
    Clear();
    BinaryReader r(data);
    if (r.Read<u32>() != NavMagic || r.Read<u32>() != NavVersion)
        return false;
    r.Read(m_settings);
    m_cellsX = r.Read<i32>();
    m_cellsZ = r.Read<i32>();
    m_tileCellsX = r.Read<i32>();
    m_tileCellsZ = r.Read<i32>();
    m_border = r.Read<i32>();
    m_tilesX = r.Read<u32>();
    m_tilesZ = r.Read<u32>();
    bool ok = !r.HasError() && m_tilesX > 0 && m_tilesZ > 0 && u64(m_tilesX) * m_tilesZ <= 1u << 20;
    if (ok) {
        m_tiles.Resize(usize(m_tilesX) * m_tilesZ);
        for (Tile& t : m_tiles) {
            r.ReadArray(t.polys);
            r.ReadArray(t.links);
            for (Array<EdgeCell>& e : t.edges)
                r.ReadArray(e);
            for (const Poly& p : t.polys)
                ok &= p.firstLink + p.linkCount <= t.links.Size();
            for (const Link& l : t.links)
                ok &= l.to < t.polys.Size();
            for (const Array<EdgeCell>& e : t.edges)
                for (const EdgeCell& c : e)
                    ok &= c.poly < t.polys.Size();
        }
        ok &= !r.HasError();
    }
    if (!ok) {
        Clear();
        return false;
    }
    Assemble();
    return true;
}

} // namespace zx::nav
