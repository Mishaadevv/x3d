// ZNano cluster DAG builder (see Asset/Nano.h).

#include "Asset/Nano.h"
#include "Asset/MeshOptimizer.h"
#include "Core/Containers/Algorithm.h"
#include "Core/Containers/HashMap.h"
#include "Core/Hash/Hash.h"
#include "Core/Memory/Memory.h"

#include <bit>

namespace zx::asset::nano {

namespace {

struct BuildCluster
{
    Array<u32> indices; // mesh vertex indices, 3 per triangle
    Vec4 bounds;
    Vec4 selfSphere;
    Vec4 parentSphere;
    f32 selfError = 0.0f;
    f32 parentError = RootError;
    u32 section = 0;
    u32 level = 0;
};

u32 Part1By2(u32 x)
{
    x &= 0x3FF;
    x = (x | (x << 16)) & 0x030000FF;
    x = (x | (x << 8)) & 0x0300F00F;
    x = (x | (x << 4)) & 0x030C30C3;
    x = (x | (x << 2)) & 0x09249249;
    return x;
}

u32 Morton(Vec3 p, const AABB& box)
{
    const Vec3 size = Max(box.max - box.min, Vec3(1e-6f));
    const Vec3 q = Clamp((p - box.min) / size, Vec3(0.0f), Vec3(1.0f)) * 1023.0f;
    return Part1By2(u32(q.x)) | (Part1By2(u32(q.y)) << 1) | (Part1By2(u32(q.z)) << 2);
}

Vec4 SphereOfPoints(const Array<Vec3>& positions, Span<const u32> indices)
{
    AABB box;
    for (u32 i : indices)
        box.Expand(positions[i]);
    const Vec3 c = box.Center();
    f32 r2 = 0.0f;
    for (u32 i : indices)
        r2 = Max(r2, Dot(positions[i] - c, positions[i] - c));
    return Vec4(c, Sqrt(r2));
}

/// The smallest-ish sphere containing `spheres` (AABB center, then the farthest reach).
Vec4 SphereOfSpheres(Span<const Vec4> spheres)
{
    AABB box;
    for (const Vec4& s : spheres) {
        box.Expand(s.XYZ() - Vec3(s.w));
        box.Expand(s.XYZ() + Vec3(s.w));
    }
    const Vec3 c = box.Center();
    f32 r = 0.0f;
    for (const Vec4& s : spheres)
        r = Max(r, Length(s.XYZ() - c) + s.w);
    return Vec4(c, r);
}

/// Cuts a triangle list into clusters (<= 128 triangles, <= 255 vertices) in Morton order of the triangle centers.
void MakeClusters(Span<const u32> indices, const Array<Vec3>& positions, u32 section, u32 level, Array<BuildCluster>& out)
{
    const u32 triCount = u32(indices.Size() / 3);
    if (triCount == 0)
        return;
    AABB box;
    for (u32 i : indices)
        box.Expand(positions[i]);
    Array<u64> order; // morton << 32 | triangle
    order.ResizeUninitialized(triCount);
    for (u32 t = 0; t < triCount; ++t) {
        const Vec3 c = (positions[indices[t * 3]] + positions[indices[t * 3 + 1]] + positions[indices[t * 3 + 2]]) / 3.0f;
        order[t] = (u64(Morton(c, box)) << 32) | t;
    }
    Sort(order, [](u64 a, u64 b) { return a < b; });
    BuildCluster cur;
    Array<u32> localVerts; // unique vertices of the current cluster (small: linear search)
    auto flush = [&] {
        if (cur.indices.Empty())
            return;
        cur.section = section;
        cur.level = level;
        cur.bounds = SphereOfPoints(positions, cur.indices.View());
        out.PushBack(move(cur));
        cur = BuildCluster();
        localVerts.Clear();
    };
    for (u64 key : order) {
        const u32 t = u32(key & 0xFFFFFFFFu);
        u32 added = 0;
        for (u32 k = 0; k < 3; ++k)
            if (!localVerts.Contains(indices[t * 3 + k]))
                ++added;
        if (cur.indices.Size() / 3 >= MaxClusterTriangles || localVerts.Size() + added > MaxClusterVertices)
            flush();
        for (u32 k = 0; k < 3; ++k) {
            const u32 v = indices[t * 3 + k];
            if (!localVerts.Contains(v))
                localVerts.PushBack(v);
            cur.indices.PushBack(v);
        }
    }
    flush();
}

/// Groups of up to `groupSize` clusters that share the most border (welded vertices): the merged group's inner
/// borders unlock, so it simplifies well. Seeds go in Morton order; a group grows by the unassigned neighbor sharing
/// the most vertices with it.
void GroupClusters(const Array<BuildCluster>& level, const Array<u32>& weld, u32 groupSize, Array<Array<u32>>& groups)
{
    const u32 n = u32(level.Size());
    // (welded vertex, cluster) pairs -> cluster adjacency with shared vertex counts.
    Array<u64> pairs;
    Array<u32> ids;
    for (u32 c = 0; c < n; ++c) {
        ids.Clear();
        for (u32 v : level[c].indices)
            ids.PushBack(weld[v]);
        Sort(ids, [](u32 a, u32 b) { return a < b; });
        for (usize i = 0; i < ids.Size(); ++i)
            if (i == 0 || ids[i] != ids[i - 1])
                pairs.PushBack((u64(ids[i]) << 32) | c);
    }
    Sort(pairs, [](u64 a, u64 b) { return a < b; });
    Array<u64> edges; // min << 32 | max, once per shared vertex
    for (usize i = 0; i < pairs.Size();) {
        usize j = i;
        while (j < pairs.Size() && (pairs[j] >> 32) == (pairs[i] >> 32))
            ++j;
        for (usize a = i; a < j; ++a)
            for (usize b = a + 1; b < j; ++b) {
                const u32 ca = u32(pairs[a] & 0xFFFFFFFFu), cb = u32(pairs[b] & 0xFFFFFFFFu);
                edges.PushBack((u64(Min(ca, cb)) << 32) | Max(ca, cb));
            }
        i = j;
    }
    Sort(edges, [](u64 a, u64 b) { return a < b; });
    struct Link
    {
        u32 other;
        u32 weight;
    };
    Array<Array<Link>> adjacency;
    adjacency.Resize(n);
    for (usize i = 0; i < edges.Size();) {
        usize j = i;
        while (j < edges.Size() && edges[j] == edges[i])
            ++j;
        const u32 a = u32(edges[i] >> 32), b = u32(edges[i] & 0xFFFFFFFFu);
        adjacency[a].PushBack(Link{b, u32(j - i)});
        adjacency[b].PushBack(Link{a, u32(j - i)});
        i = j;
    }

    AABB box;
    for (const BuildCluster& c : level)
        box.Expand(c.bounds.XYZ());
    Array<u64> order;
    for (u32 i = 0; i < n; ++i)
        order.PushBack((u64(Morton(level[i].bounds.XYZ(), box)) << 32) | i);
    Sort(order, [](u64 a, u64 b) { return a < b; });
    Array<bool> assigned;
    assigned.Resize(n, false);
    Array<u32> weight; // shared vertices with the growing group
    weight.Resize(n, 0u);
    Array<u32> touched;
    groups.Clear();
    for (u64 key : order) {
        const u32 seed = u32(key & 0xFFFFFFFFu);
        if (assigned[seed])
            continue;
        Array<u32> group;
        group.PushBack(seed);
        assigned[seed] = true;
        touched.Clear();
        while (group.Size() < groupSize) {
            for (const Link& l : adjacency[group.Back()])
                if (!assigned[l.other]) {
                    if (weight[l.other] == 0)
                        touched.PushBack(l.other);
                    weight[l.other] += l.weight;
                }
            u32 best = ~0u, bestWeight = 0;
            for (u32 t : touched)
                if (!assigned[t] && weight[t] > bestWeight) {
                    best = t;
                    bestWeight = weight[t];
                }
            if (best == ~0u)
                break;
            group.PushBack(best);
            assigned[best] = true;
        }
        for (u32 t : touched)
            weight[t] = 0;
        groups.PushBack(move(group));
    }
}

} // namespace

bool Build(const CookedMesh& mesh, NanoMesh& out, const BuildOptions& options)
{
    out = NanoMesh();
    Array<Vec3> positions;
    positions.ResizeUninitialized(mesh.vertices.Size());
    for (usize i = 0; i < mesh.vertices.Size(); ++i)
        positions[i] = mesh.vertices[i].position;

    // Welded vertex ids (UV / normal seams split vertices at one position): adjacency and borders follow positions.
    Array<u32> weld;
    weld.ResizeUninitialized(mesh.vertices.Size());
    {
        HashMap<u64, u32> byPosition;
        for (usize i = 0; i < positions.Size(); ++i)
            weld[i] = *byPosition.Insert(HashXX64(&positions[i], sizeof(Vec3)), u32(i)).first;
    }

    Array<BuildCluster> all;
    u32 levels = 1;
    // Group errors are made unique (bumped by ulps, bottom-up so parents stay >= children): clusters sharing a parent
    // error then always form one group, which lets streaming cut the cluster list exactly between groups.
    HashSet<u32> usedErrors;
    for (u32 s = 0; s < mesh.sections.Size(); ++s) {
        const CookedSectionLod& lod0 = mesh.SectionLod(mesh.sections[s], 0);
        Array<u32> indices;
        indices.ResizeUninitialized(lod0.indexCount);
        for (u32 i = 0; i < lod0.indexCount; ++i)
            indices[i] = mesh.Index(lod0.indexOffset + i);
        out.leafTriangles += lod0.indexCount / 3;

        // Leaves.
        Array<BuildCluster> level;
        MakeClusters(indices.View(), positions, s, 0, level);
        for (BuildCluster& c : level)
            c.selfSphere = c.bounds;

        // Levels: groups of neighbors merged, simplified to half with locked borders, cut again. Clusters of a group
        // that can't get simpler move up unchanged and meet other neighbors on the next level.
        for (u32 depth = 1; level.Size() > 1 && depth < 64; ++depth) {
            Array<Array<u32>> groups;
            GroupClusters(level, weld, options.groupSize, groups);
            Array<BuildCluster> next;
            bool anyReduced = false;
            for (const Array<u32>& group : groups) {
                Array<u32> merged;
                Array<Vec4> spheres;
                f32 childError = 0.0f;
                for (u32 ci : group) {
                    const BuildCluster& c = level[ci];
                    merged.Append(c.indices.View());
                    spheres.PushBack(c.selfSphere);
                    childError = Max(childError, c.selfError);
                }
                Array<u32> simplified;
                f32 relError = 0.0f;
                f32 extent = 0.0f;
                if (group.Size() > 1) {
                    // Simplify on a compact copy (the simplifier's cost then follows the group, not the mesh).
                    HashMap<u32, u32> toLocal;
                    Array<u32> toGlobal, local;
                    Array<Vec3> localPos;
                    local.ResizeUninitialized(merged.Size());
                    for (usize i = 0; i < merged.Size(); ++i) {
                        const auto r = toLocal.Insert(merged[i], u32(toGlobal.Size()));
                        if (r.second) {
                            toGlobal.PushBack(merged[i]);
                            localPos.PushBack(positions[merged[i]]);
                        }
                        local[i] = *r.first;
                    }
                    mesh::SimplifyOptions so;
                    so.targetIndexCount = u32(merged.Size() / 6) * 3; // half the triangles
                    so.maxError = 1.0f;
                    so.lockBorders = true;
                    AABB groupBox;
                    for (const Vec3& p : localPos)
                        groupBox.Expand(p);
                    extent = Max(Max(groupBox.max.x - groupBox.min.x, groupBox.max.y - groupBox.min.y), groupBox.max.z - groupBox.min.z);
                    relError = mesh::Simplify(local.View(), localPos.View(), so, simplified);
                    for (u32& i : simplified)
                        i = toGlobal[i];
                }
                const bool reduced = !simplified.Empty() && f32(simplified.Size()) <= f32(merged.Size()) * options.minReduction;
                if (!reduced) {
                    for (u32 ci : group)
                        next.PushBack(move(level[ci]));
                    continue;
                }
                anyReduced = true;
                f32 groupError = childError + relError * extent;
                u32 errorBits = std::bit_cast<u32>(Max(groupError, 0.0f));
                while (!usedErrors.Insert(errorBits))
                    ++errorBits; // next float up
                groupError = std::bit_cast<f32>(errorBits);
                const Vec4 groupSphere = SphereOfSpheres(spheres.View());
                for (u32 ci : group) {
                    BuildCluster& c = level[ci];
                    c.parentError = groupError;
                    c.parentSphere = groupSphere;
                    all.PushBack(move(c));
                }
                Array<BuildCluster> made;
                MakeClusters(simplified.View(), positions, s, depth, made);
                for (BuildCluster& c : made) {
                    c.selfError = groupError;
                    c.selfSphere = groupSphere;
                    next.PushBack(move(c));
                }
            }
            level = move(next);
            if (!anyReduced)
                break; // nothing got simpler: the rest are roots
            levels = Max(levels, depth + 1);
        }
        for (BuildCluster& c : level) { // the coarsest level: roots
            c.parentError = RootError;
            c.parentSphere = c.selfSphere;
            all.PushBack(move(c));
        }
    }
    if (all.Empty())
        return false;

    // Largest parent error first (coarse to fine): the renderer tests a prefix per instance.
    Array<u32> order;
    for (u32 i = 0; i < all.Size(); ++i)
        order.PushBack(i);
    StableSort(order, [&](u32 a, u32 b) { return all[a].parentError > all[b].parentError; });
    Array<u32> localVerts;
    for (u32 i : order) {
        const BuildCluster& c = all[i];
        ClusterHeader h;
        h.bounds = c.bounds;
        h.selfSphere = c.selfSphere;
        h.parentSphere = c.parentSphere;
        h.selfError = c.selfError;
        h.parentError = c.parentError;
        h.triangleStart = u32(out.triangles.Size());
        h.triangleCount = u32(c.indices.Size() / 3);
        h.vertexStart = u32(out.vertexRemap.Size());
        h.section = c.section;
        h.level = c.level;
        localVerts.Clear();
        for (usize t = 0; t + 2 < c.indices.Size(); t += 3) {
            u32 packed = 0;
            for (u32 k = 0; k < 3; ++k) {
                const u32 v = c.indices[t + k];
                usize at = localVerts.Find(v);
                if (at == NPos) {
                    at = localVerts.Size();
                    localVerts.PushBack(v);
                }
                packed |= u32(at) << (k * 8);
            }
            out.triangles.PushBack(packed);
        }
        h.vertexCount = u32(localVerts.Size());
        out.vertexRemap.Append(localVerts.View());
        out.clusters.PushBack(h);
    }
    out.levels = levels;
    return true;
}

namespace {

f32 Hash3(i32 x, i32 y, i32 z, u32 seed)
{
    u32 h = u32(x) * 0x8DA6B343u ^ u32(y) * 0xD8163841u ^ u32(z) * 0xCB1AB31Fu ^ seed * 0x9E3779B9u;
    h = (h ^ (h >> 13)) * 0x5BD1E995u;
    h ^= h >> 15;
    return f32(h & 0xFFFFFF) / f32(0xFFFFFF) * 2.0f - 1.0f;
}

/// Trilinear value noise in [-1, 1].
f32 ValueNoise(Vec3 p, u32 seed)
{
    const f32 fx = Floor(p.x), fy = Floor(p.y), fz = Floor(p.z);
    const i32 x = i32(fx), y = i32(fy), z = i32(fz);
    auto fade = [](f32 t) { return t * t * (3.0f - 2.0f * t); };
    const f32 u = fade(p.x - fx), v = fade(p.y - fy), w = fade(p.z - fz);
    auto lerp = [](f32 a, f32 b, f32 t) { return a + (b - a) * t; };
    const f32 x00 = lerp(Hash3(x, y, z, seed), Hash3(x + 1, y, z, seed), u);
    const f32 x10 = lerp(Hash3(x, y + 1, z, seed), Hash3(x + 1, y + 1, z, seed), u);
    const f32 x01 = lerp(Hash3(x, y, z + 1, seed), Hash3(x + 1, y, z + 1, seed), u);
    const f32 x11 = lerp(Hash3(x, y + 1, z + 1, seed), Hash3(x + 1, y + 1, z + 1, seed), u);
    return lerp(lerp(x00, x10, v), lerp(x01, x11, v), w);
}

} // namespace

MeshData MakeDetailedRock(u32 subdivisions, u32 seed)
{
    // Icosahedron.
    const f32 t = (1.0f + Sqrt(5.0f)) * 0.5f;
    Array<Vec3> points = {Vec3(-1, t, 0), Vec3(1, t, 0), Vec3(-1, -t, 0), Vec3(1, -t, 0), Vec3(0, -1, t), Vec3(0, 1, t),
                          Vec3(0, -1, -t), Vec3(0, 1, -t), Vec3(t, 0, -1), Vec3(t, 0, 1), Vec3(-t, 0, -1), Vec3(-t, 0, 1)};
    for (Vec3& p : points)
        p = Normalize(p);
    Array<u32> indices = {0, 11, 5, 0, 5, 1, 0, 1, 7, 0, 7, 10, 0, 10, 11, 1, 5, 9, 5, 11, 4, 11, 10, 2, 10, 7, 6, 7, 1, 8,
                          3, 9, 4, 3, 4, 2, 3, 2, 6, 3, 6, 8, 3, 8, 9, 4, 9, 5, 2, 4, 11, 6, 2, 10, 8, 6, 7, 9, 8, 1};
    // Subdivide: each edge's midpoint once (keyed by the vertex pair).
    for (u32 level = 0; level < Min(subdivisions, 9u); ++level) {
        HashMap<u64, u32> midpoints;
        Array<u32> next;
        next.Reserve(indices.Size() * 4);
        auto mid = [&](u32 a, u32 b) {
            const u64 key = (u64(Min(a, b)) << 32) | Max(a, b);
            const auto r = midpoints.Insert(key, u32(points.Size()));
            if (r.second)
                points.PushBack(Normalize(points[a] + points[b]));
            return *r.first;
        };
        for (usize i = 0; i < indices.Size(); i += 3) {
            const u32 a = indices[i], b = indices[i + 1], c = indices[i + 2];
            const u32 ab = mid(a, b), bc = mid(b, c), ca = mid(c, a);
            next.Append({a, ab, ca, b, bc, ab, c, ca, bc, ab, bc, ca});
        }
        indices = move(next);
    }
    // Rock shape: a squashed sphere with large lumps, ridges and fine grain.
    MeshData m;
    m.name = "DetailedRock";
    m.attributes = MeshAttributes::Normals | MeshAttributes::Tangents | MeshAttributes::UV0;
    m.vertices.Resize(points.Size());
    for (usize i = 0; i < points.Size(); ++i) {
        const Vec3 d = points[i];
        f32 r = 1.0f;
        f32 amplitude = 0.35f, frequency = 1.3f;
        for (u32 o = 0; o < 7; ++o) {
            const f32 n = ValueNoise(d * frequency + Vec3(f32(o) * 17.3f), seed + o);
            r += (o == 2 ? (0.5f - Abs(n)) : n) * amplitude; // one ridged octave
            amplitude *= 0.5f;
            frequency *= 2.1f;
        }
        MeshVertex& v = m.vertices[i];
        v.position = Vec3(d.x, d.y * 0.7f, d.z) * Max(r, 0.4f);
        v.uv0 = Vec2(Atan2(d.z, d.x) / (2.0f * Pi) + 0.5f, d.y * 0.5f + 0.5f) * 4.0f;
    }
    // Smooth normals from the faces.
    Array<Vec3> normals;
    normals.Resize(points.Size(), Vec3());
    for (usize i = 0; i < indices.Size(); i += 3) {
        const Vec3 a = m.vertices[indices[i]].position, b = m.vertices[indices[i + 1]].position, c = m.vertices[indices[i + 2]].position;
        const Vec3 n = Cross(b - a, c - a);
        for (u32 k = 0; k < 3; ++k)
            normals[indices[i + k]] += n;
    }
    for (usize i = 0; i < points.Size(); ++i) {
        MeshVertex& v = m.vertices[i];
        v.normal = SafeNormalize(normals[i], Vec3(0, 1, 0));
        const Vec3 tangent = SafeNormalize(Cross(Vec3(0, 1, 0), v.normal), Vec3(1, 0, 0));
        v.tangent = Vec4(tangent, 1.0f);
    }
    MeshLod lod;
    lod.indices = move(indices);
    lod.sections.PushBack(MeshSection{0, u32(lod.indices.Size()), 0});
    m.lods.PushBack(move(lod));
    m.ComputeBounds();
    return m;
}

void BuildGpuPages(const CookedMesh& mesh, GpuPages& out)
{
    const NanoMesh& nm = mesh.nano;
    out = GpuPages();
    out.headers.Reserve(nm.clusters.Size());
    // Pages fill with whole clusters in their (coarse to fine) order: vertices from the front, triangles after them.
    // Past the roots, a page may only end where the parent error changes (between groups).
    struct Pending
    {
        u32 cluster;
        u32 vertexBytes, triangleBytes;
    };
    Array<Pending> page;
    u32 pageVertexBytes = 0, pageTriangleBytes = 0;
    auto bytesOf = [](const ClusterHeader& c) { return Pair<u32, u32>{c.vertexCount * u32(sizeof(PackedVertex)), c.triangleCount * 4u}; };
    auto emit = [&](usize count) { // writes the first `count` pending clusters as one page
        const u32 pageIndex = u32(out.pageFirstCluster.Size());
        out.pageFirstCluster.PushBack(page[0].cluster);
        const usize base = out.data.Size();
        out.data.Resize(base + PageBytes, u8(0));
        u8* dst = out.data.Data() + base;
        u32 vertexBytes = 0;
        for (usize i = 0; i < count; ++i)
            vertexBytes += page[i].vertexBytes;
        u32 vertexAt = 0, triangleAt = vertexBytes;
        for (usize i = 0; i < count; ++i) {
            const ClusterHeader& c = nm.clusters[page[i].cluster];
            ClusterHeader h = c;
            h.vertexStart = vertexAt / u32(sizeof(PackedVertex)); // vertices into the page
            h.triangleStart = triangleAt;                         // bytes into the page
            h.level = pageIndex;
            for (u32 v = 0; v < c.vertexCount; ++v)
                MemCopy(dst + vertexAt + v * sizeof(PackedVertex), &mesh.vertices[nm.vertexRemap[c.vertexStart + v]], sizeof(PackedVertex));
            MemCopy(dst + triangleAt, nm.triangles.Data() + c.triangleStart, c.triangleCount * 4);
            vertexAt += page[i].vertexBytes;
            triangleAt += page[i].triangleBytes;
            out.headers.PushBack(h);
            if (c.parentError >= RootError)
                out.pinnedPages = pageIndex + 1;
        }
        page.RemoveAt(0, count);
        pageVertexBytes = pageTriangleBytes = 0;
        for (const Pending& p : page) {
            pageVertexBytes += p.vertexBytes;
            pageTriangleBytes += p.triangleBytes;
        }
    };
    for (u32 i = 0; i < nm.clusters.Size(); ++i) {
        const auto [vb, tb] = bytesOf(nm.clusters[i]);
        if (pageVertexBytes + pageTriangleBytes + vb + tb > PageBytes) {
            // Close the page at the last group boundary (anywhere among the roots: they are always resident).
            usize cut = page.Size();
            const bool rootsOnly = nm.clusters[page.Back().cluster].parentError >= RootError;
            if (!rootsOnly && nm.clusters[i].parentError == nm.clusters[page.Back().cluster].parentError) {
                cut = 0;
                for (usize k = page.Size() - 1; k > 0; --k)
                    if (nm.clusters[page[k].cluster].parentError != nm.clusters[page[k - 1].cluster].parentError) {
                        cut = k;
                        break;
                    }
                if (cut == 0) // one group larger than a page (cannot happen with 4-cluster groups): keep it whole
                    cut = page.Size();
            }
            emit(cut);
        }
        page.PushBack(Pending{i, vb, tb});
        pageVertexBytes += vb;
        pageTriangleBytes += tb;
    }
    while (!page.Empty())
        emit(page.Size());
    out.pinnedPages = Max(out.pinnedPages, 1u);
}

void SelectCutResident(const NanoMesh& m, Vec3 eye, f32 pixelsPerUnit, f32 thresholdPixels, u32 residentClusters, Array<u32>& out)
{
    out.Clear();
    residentClusters = Min(residentClusters, u32(m.clusters.Size()));
    const f32 residentError = residentClusters < m.clusters.Size() && residentClusters > 0 ? m.clusters[residentClusters - 1].parentError : -1.0f;
    for (u32 i = 0; i < residentClusters; ++i) {
        const NanoClusterHeader& c = m.clusters[i];
        // Children not resident (their parent error = this selfError is below the resident prefix): stop here.
        const bool selfOk = c.selfError < residentError || ProjectedError(c.selfError, c.selfSphere, eye, pixelsPerUnit) <= thresholdPixels;
        if (selfOk && ProjectedError(c.parentError, c.parentSphere, eye, pixelsPerUnit) > thresholdPixels)
            out.PushBack(i);
    }
}

void AppendFallbackLods(CookedMesh& mesh, u32 maxLods, f32 referenceHeight)
{
    const NanoMesh& nm = mesh.nano;
    if (nm.Empty() || maxLods < 2)
        return;
    Array<u32> indices;
    indices.ResizeUninitialized(mesh.IndexCount());
    for (u32 i = 0; i < indices.Size(); ++i)
        indices[i] = mesh.Index(i);
    Array<CookedSectionLod> lods;
    u32 lodCount = 1;
    for (u32 s = 0; s < mesh.sections.Size(); ++s) {
        CookedMeshSection& section = mesh.sections[s];
        const CookedSectionLod lod0 = mesh.sectionLods[section.firstLod];
        section.firstLod = u32(lods.Size());
        section.lodCount = 1;
        lods.PushBack(lod0);
        const f32 radius = Max(Length(section.bounds.Extents()), 1e-4f);
        u32 previous = lod0.indexCount;
        f32 previousScreen = 1.0f;
        for (f32 fraction : {0.004f, 0.016f, 0.064f, 0.25f}) {
            if (section.lodCount >= maxLods)
                break;
            // The cut at this error: clusters fine enough whose replacement isn't.
            const f32 error = radius * fraction;
            const u32 offset = u32(indices.Size());
            for (const ClusterHeader& c : nm.clusters) {
                if (c.section != s || c.selfError > error || c.parentError <= error)
                    continue;
                for (u32 t = 0; t < c.triangleCount; ++t) {
                    const u32 packed = nm.triangles[c.triangleStart + t];
                    for (u32 k = 0; k < 3; ++k)
                        indices.PushBack(nm.vertexRemap[c.vertexStart + ((packed >> (k * 8)) & 0xFF)]);
                }
            }
            const u32 count = u32(indices.Size()) - offset;
            if (count == 0 || f32(count) > f32(previous) * 0.6f) { // not much simpler: try a coarser cut
                indices.Resize(offset);
                continue;
            }
            // Projected error <= 1 pixel at the reference height (as mesh::GenerateLods), decreasing along the chain.
            const f32 screen = Min(Clamp(2.0f * radius / (error * referenceHeight), 0.0005f, 1.0f), previousScreen * 0.9f);
            lods.PushBack(CookedSectionLod{offset, count, screen, error});
            ++section.lodCount;
            previous = count;
            previousScreen = screen;
        }
        lodCount = Max(lodCount, section.lodCount);
    }
    mesh.sectionLods = move(lods);
    mesh.lodCount = lodCount;
    const u32 elementBytes = mesh.index16 ? 2 : 4;
    mesh.indexData.ResizeUninitialized(indices.Size() * elementBytes);
    for (usize i = 0; i < indices.Size(); ++i) {
        if (mesh.index16) {
            const u16 v = u16(indices[i]);
            MemCopy(mesh.indexData.Data() + i * 2, &v, 2);
        } else {
            MemCopy(mesh.indexData.Data() + i * 4, &indices[i], 4);
        }
    }
}

} // namespace zx::asset::nano

namespace zx::asset {

void NanoMesh::WriteGpu(Array<u8>& out) const
{
    out.ResizeUninitialized(usize(GpuBytes()));
    MemCopy(out.Data(), clusters.Data(), clusters.Size() * sizeof(NanoClusterHeader));
    MemCopy(out.Data() + TrianglesOffset(), triangles.Data(), triangles.Size() * 4);
    MemCopy(out.Data() + RemapOffset(), vertexRemap.Data(), vertexRemap.Size() * 4);
}

} // namespace zx::asset

