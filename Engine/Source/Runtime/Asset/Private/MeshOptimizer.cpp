#include "Asset/MeshOptimizer.h"
#include "Core/Containers/Algorithm.h"
#include "Core/Containers/HashMap.h"
#include "Core/Hash/Hash.h"
#include "Core/Jobs/JobSystem.h"

#include <cmath>

namespace zx::asset::mesh {

// --- Vertex cache (Forsyth) -----------------------------------------------------------------------------------------

namespace {

constexpr u32 CacheSize = 32;
constexpr u32 MaxValence = 32;

struct ForsythTables
{
    f32 cache[CacheSize];
    f32 valence[MaxValence + 1];
    ForsythTables()
    {
        constexpr f32 decayPower = 1.5f, lastTriScore = 0.75f;
        for (u32 i = 0; i < CacheSize; ++i) {
            if (i < 3) {
                cache[i] = lastTriScore;
            } else {
                const f32 scaler = 1.0f / f32(CacheSize - 3);
                cache[i] = std::pow(1.0f - f32(i - 3) * scaler, decayPower);
            }
        }
        valence[0] = 0.0f;
        for (u32 i = 1; i <= MaxValence; ++i)
            valence[i] = 2.0f * std::pow(f32(i), -0.5f);
    }
};

const ForsythTables& Tables()
{
    static const ForsythTables t;
    return t;
}

f32 VertexScore(i32 cachePos, u32 remaining)
{
    if (remaining == 0)
        return -1.0f;
    const ForsythTables& t = Tables();
    const f32 s = cachePos >= 0 ? t.cache[cachePos] : 0.0f;
    return s + t.valence[Min(remaining, MaxValence)];
}

} // namespace

namespace {
void OptimizeVertexCacheCompact(Span<u32> indices, u32 vertexCount);
} // namespace

void OptimizeVertexCache(Span<u32> indices, u32 vertexCount)
{
    if (indices.Size() < 6)
        return;
    // Remap to the vertices actually used so per-call cost scales with the index range.
    Array<u32> toLocal;
    toLocal.Resize(vertexCount, InvalidIndex);
    Array<u32> toGlobal;
    for (u32& i : indices) {
        u32& l = toLocal[i];
        if (l == InvalidIndex) {
            l = u32(toGlobal.Size());
            toGlobal.PushBack(i);
        }
        i = l;
    }
    OptimizeVertexCacheCompact(indices, u32(toGlobal.Size()));
    for (u32& i : indices)
        i = toGlobal[i];
}

namespace {
void OptimizeVertexCacheCompact(Span<u32> indices, u32 vertexCount)
{
    const u32 triCount = u32(indices.Size() / 3);
    if (triCount < 2)
        return;
    // Adjacency: triangles per vertex (CSR).
    Array<u32> remaining, offsets, adjacency;
    remaining.Resize(vertexCount, 0);
    for (u32 i : indices)
        ++remaining[i];
    offsets.ResizeUninitialized(vertexCount + 1);
    offsets[0] = 0;
    for (u32 v = 0; v < vertexCount; ++v)
        offsets[v + 1] = offsets[v] + remaining[v];
    adjacency.ResizeUninitialized(indices.Size());
    {
        Array<u32> fill;
        fill.Resize(vertexCount, 0);
        for (u32 t = 0; t < triCount; ++t)
            for (u32 k = 0; k < 3; ++k) {
                const u32 v = indices[t * 3 + k];
                adjacency[offsets[v] + fill[v]++] = t;
            }
    }
    Array<i32> cachePos;
    cachePos.Resize(vertexCount, -1);
    Array<f32> vScore;
    vScore.ResizeUninitialized(vertexCount);
    for (u32 v = 0; v < vertexCount; ++v)
        vScore[v] = VertexScore(-1, remaining[v]);
    Array<f32> tScore;
    tScore.ResizeUninitialized(triCount);
    Array<bool> emitted;
    emitted.Resize(triCount, false);
    for (u32 t = 0; t < triCount; ++t)
        tScore[t] = vScore[indices[t * 3]] + vScore[indices[t * 3 + 1]] + vScore[indices[t * 3 + 2]];

    // Mutable per-vertex list of live triangles: swap emitted ones to the end of the vertex's range.
    Array<u32> live = remaining;

    Array<u32> out;
    out.ResizeUninitialized(indices.Size());
    u32 cache[CacheSize + 3];
    u32 cacheCount = 0;
    u32 scanCursor = 0;
    u32 best = InvalidIndex;
    for (u32 emittedCount = 0; emittedCount < triCount; ++emittedCount) {
        if (best == InvalidIndex) {
            // No candidate in the cache: take the next un-emitted triangle in input order.
            while (scanCursor < triCount && emitted[scanCursor])
                ++scanCursor;
            best = scanCursor;
        }
        const u32 t = best;
        emitted[t] = true;
        for (u32 k = 0; k < 3; ++k)
            out[emittedCount * 3 + k] = indices[t * 3 + k];

        // Update the LRU cache: put the triangle's vertices in front.
        u32 newCache[CacheSize + 3];
        u32 newCount = 0;
        for (u32 k = 0; k < 3; ++k) {
            const u32 v = indices[t * 3 + k];
            newCache[newCount++] = v;
            // Remove t from v's live triangle list.
            const u32 begin = offsets[v];
            for (u32 j = begin; j < begin + live[v]; ++j)
                if (adjacency[j] == t) {
                    adjacency[j] = adjacency[begin + live[v] - 1];
                    adjacency[begin + live[v] - 1] = t;
                    break;
                }
            --live[v];
        }
        for (u32 i = 0; i < cacheCount; ++i) {
            const u32 v = cache[i];
            if (v != newCache[0] && v != newCache[1] && v != newCache[2])
                newCache[newCount++] = v;
        }
        cacheCount = Min(newCount, CacheSize);
        for (u32 i = 0; i < cacheCount; ++i)
            cache[i] = newCache[i];

        // Rescore cached vertices and their live triangles; pick the best among them.
        best = InvalidIndex;
        f32 bestScore = -1.0f;
        for (u32 i = 0; i < newCount; ++i) {
            const u32 v = newCache[i];
            const i32 pos = i < CacheSize ? i32(i) : -1;
            cachePos[v] = pos;
            const f32 s = VertexScore(pos, live[v]);
            const f32 delta = s - vScore[v];
            vScore[v] = s;
            for (u32 j = offsets[v]; j < offsets[v] + live[v]; ++j)
                tScore[adjacency[j]] += delta;
        }
        for (u32 i = 0; i < cacheCount; ++i) {
            const u32 v = cache[i];
            for (u32 j = offsets[v]; j < offsets[v] + live[v]; ++j) {
                const u32 tri = adjacency[j];
                if (tScore[tri] > bestScore) {
                    bestScore = tScore[tri];
                    best = tri;
                }
            }
        }
    }
    MemCopy(indices.Data(), out.Data(), out.SizeBytes());
}
} // namespace

f32 ComputeACMR(Span<const u32> indices, u32 vertexCount, u32 cacheSize)
{
    if (indices.Size() < 3)
        return 0.0f;
    Array<u32> stamp; // FIFO: vertex is cached if inserted within the last cacheSize misses
    stamp.Resize(vertexCount, 0);
    u32 time = cacheSize + 1, misses = 0;
    for (u32 i : indices) {
        if (time - stamp[i] > cacheSize) {
            stamp[i] = time++;
            ++misses;
        }
    }
    return f32(misses) / f32(indices.Size() / 3);
}

void OptimizeMesh(MeshData& mesh)
{
    const u32 vertexCount = u32(mesh.vertices.Size());
    for (MeshLod& lod : mesh.lods)
        for (const MeshSection& s : lod.sections)
            OptimizeVertexCache(Span<u32>(lod.indices.Data() + s.indexOffset, s.indexCount), vertexCount);
    // Vertex fetch order: first use (LOD0 first, then the remaining LODs).
    Array<u32> remap;
    remap.Resize(vertexCount, InvalidIndex);
    Array<u32> ordered; // new vertex -> old vertex
    ordered.Reserve(vertexCount);
    for (MeshLod& lod : mesh.lods)
        for (u32& i : lod.indices) {
            if (remap[i] == InvalidIndex) {
                remap[i] = u32(ordered.Size());
                ordered.PushBack(i);
            }
            i = remap[i];
        }
    PermuteVertices(mesh, ordered.View());
}

// --- QEM simplification ---------------------------------------------------------------------------------------------

namespace {

struct Quadric
{
    f64 a00 = 0, a01 = 0, a02 = 0, a11 = 0, a12 = 0, a22 = 0;
    f64 b0 = 0, b1 = 0, b2 = 0;
    f64 c = 0;

    static Quadric FromPlane(f64 nx, f64 ny, f64 nz, f64 d, f64 w)
    {
        Quadric q;
        q.a00 = w * nx * nx;
        q.a01 = w * nx * ny;
        q.a02 = w * nx * nz;
        q.a11 = w * ny * ny;
        q.a12 = w * ny * nz;
        q.a22 = w * nz * nz;
        q.b0 = w * nx * d;
        q.b1 = w * ny * d;
        q.b2 = w * nz * d;
        q.c = w * d * d;
        return q;
    }
    void operator+=(const Quadric& o)
    {
        a00 += o.a00;
        a01 += o.a01;
        a02 += o.a02;
        a11 += o.a11;
        a12 += o.a12;
        a22 += o.a22;
        b0 += o.b0;
        b1 += o.b1;
        b2 += o.b2;
        c += o.c;
    }
    f64 Error(Vec3 p) const
    {
        const f64 x = p.x, y = p.y, z = p.z;
        const f64 e = a00 * x * x + 2 * a01 * x * y + 2 * a02 * x * z + a11 * y * y + 2 * a12 * y * z + a22 * z * z +
                      2 * (b0 * x + b1 * y + b2 * z) + c;
        return e > 0 ? e : 0;
    }
};

struct Collapse
{
    u32 from; // canonical vertices
    u32 to;
    f32 cost;
};

u64 EdgeKey(u32 a, u32 b) { return a < b ? (u64(a) << 32) | b : (u64(b) << 32) | a; }

} // namespace

namespace {
f32 SimplifyCompact(Span<const u32> indices, Span<const Vec3> positionsIn, const SimplifyOptions& options, Array<u32>& outIndices);
} // namespace

f32 Simplify(Span<const u32> indices, Span<const Vec3> positions, const SimplifyOptions& options, Array<u32>& outIndices)
{
    // Work on the vertices this index list actually uses, so cost scales with the section, not the whole mesh.
    Array<u32> toLocal;
    toLocal.Resize(positions.Size(), InvalidIndex);
    Array<u32> toGlobal;
    Array<Vec3> localPositions;
    Array<u32> localIndices;
    localIndices.ResizeUninitialized(indices.Size());
    for (usize i = 0; i < indices.Size(); ++i) {
        u32& l = toLocal[indices[i]];
        if (l == InvalidIndex) {
            l = u32(toGlobal.Size());
            toGlobal.PushBack(indices[i]);
            localPositions.PushBack(positions[indices[i]]);
        }
        localIndices[i] = l;
    }
    const f32 error = SimplifyCompact(localIndices.View(), localPositions.View(), options, outIndices);
    for (u32& i : outIndices)
        i = toGlobal[i];
    return error;
}

namespace {
f32 SimplifyCompact(Span<const u32> indices, Span<const Vec3> positionsIn, const SimplifyOptions& options, Array<u32>& outIndices)
{
    outIndices = Array<u32>(indices);
    const u32 vertexCount = u32(positionsIn.Size());
    if (indices.Size() <= options.targetIndexCount || vertexCount == 0)
        return 0.0f;

    // Normalize positions into a unit box so errors are relative.
    AABB box;
    for (u32 i : indices)
        box.Expand(positionsIn[i]);
    const f32 extent = Max(Max(box.max.x - box.min.x, box.max.y - box.min.y), Max(box.max.z - box.min.z, 1e-12f));
    const f32 invExtent = 1.0f / extent;
    Array<Vec3> pos;
    pos.ResizeUninitialized(vertexCount);
    for (u32 v = 0; v < vertexCount; ++v)
        pos[v] = (positionsIn[v] - box.min) * invExtent;

    // Canonical vertex per position (welds UV/normal seams for topology purposes).
    Array<u32> canon;
    canon.ResizeUninitialized(vertexCount);
    {
        HashMap<Vec3, u32> first;
        for (u32 v = 0; v < vertexCount; ++v) {
            const auto r = first.Insert(positionsIn[v], v);
            canon[v] = *r.first;
        }
    }
    // Wedges: list of vertices sharing a canonical vertex (CSR).
    Array<u32> wedgeOffsets, wedges;
    {
        Array<u32> counts;
        counts.Resize(vertexCount + 1, 0);
        for (u32 v = 0; v < vertexCount; ++v)
            ++counts[canon[v]];
        wedgeOffsets.ResizeUninitialized(vertexCount + 1);
        wedgeOffsets[0] = 0;
        for (u32 v = 0; v < vertexCount; ++v)
            wedgeOffsets[v + 1] = wedgeOffsets[v] + counts[v];
        wedges.ResizeUninitialized(vertexCount);
        Array<u32> fill;
        fill.Resize(vertexCount, 0);
        for (u32 v = 0; v < vertexCount; ++v)
            wedges[wedgeOffsets[canon[v]] + fill[canon[v]]++] = v;
    }

    // Quadrics from the original surface (accumulated as vertices collapse).
    Array<Quadric> quadrics;
    quadrics.Resize(vertexCount);
    for (usize t = 0; t + 2 < outIndices.Size(); t += 3) {
        const u32 a = canon[outIndices[t]], b = canon[outIndices[t + 1]], c = canon[outIndices[t + 2]];
        const Vec3 n = Cross(pos[b] - pos[a], pos[c] - pos[a]);
        const f32 len = Length(n);
        if (len < 1e-30f)
            continue;
        const Vec3 un = n / len;
        const Quadric q = Quadric::FromPlane(un.x, un.y, un.z, -f64(Dot(un, pos[a])), f64(len) * 0.5);
        quadrics[a] += q;
        quadrics[b] += q;
        quadrics[c] += q;
    }

    const f64 maxErrorSq = f64(options.maxError) * options.maxError;
    f64 achievedErrorSq = 0.0;
    Array<u32> vertexRemap;
    vertexRemap.ResizeUninitialized(vertexCount);
    Array<bool> locked, border, touched;
    Array<u32> triOffsets, triAdjacency, triCount;
    HashMap<u64, u32> edgeUse;
    bool firstPass = true;

    for (u32 pass = 0; pass < 64 && outIndices.Size() > options.targetIndexCount; ++pass) {
        const u32 tris = u32(outIndices.Size() / 3);
        // Topology: canonical edge use counts, border/non-manifold classification, triangle adjacency.
        edgeUse.Clear();
        for (u32 t = 0; t < tris; ++t)
            for (u32 k = 0; k < 3; ++k) {
                const u32 a = canon[outIndices[t * 3 + k]], b = canon[outIndices[t * 3 + (k + 1) % 3]];
                const auto r = edgeUse.Insert(EdgeKey(a, b), 0u);
                ++*r.first;
            }
        locked.Clear();
        locked.Resize(vertexCount, false);
        border.Clear();
        border.Resize(vertexCount, false);
        for (const auto& e : edgeUse) {
            const u32 a = u32(e.first >> 32), b = u32(e.first);
            if (e.second == 1) {
                border[a] = border[b] = true;
                if (options.lockBorders)
                    locked[a] = locked[b] = true;
            } else if (e.second > 2) {
                locked[a] = locked[b] = true;
            }
        }
        if (firstPass && !options.lockBorders) {
            // Border-preserving planes (perpendicular to the surface along open edges).
            for (u32 t = 0; t < tris; ++t) {
                const u32 c0 = canon[outIndices[t * 3]], c1 = canon[outIndices[t * 3 + 1]], c2 = canon[outIndices[t * 3 + 2]];
                const Vec3 n = SafeNormalize(Cross(pos[c1] - pos[c0], pos[c2] - pos[c0]));
                const u32 cs[3] = {c0, c1, c2};
                for (u32 k = 0; k < 3; ++k) {
                    const u32 a = cs[k], b = cs[(k + 1) % 3];
                    if (*edgeUse.Find(EdgeKey(a, b)) != 1)
                        continue;
                    const Vec3 edge = pos[b] - pos[a];
                    const Vec3 m = SafeNormalize(Cross(edge, n));
                    const Quadric q = Quadric::FromPlane(m.x, m.y, m.z, -f64(Dot(m, pos[a])), f64(LengthSq(edge)) * 10.0);
                    quadrics[a] += q;
                    quadrics[b] += q;
                }
            }
        }
        firstPass = false;
        triCount.Clear();
        triCount.Resize(vertexCount + 1, 0);
        for (u32 t = 0; t < tris; ++t)
            for (u32 k = 0; k < 3; ++k)
                ++triCount[canon[outIndices[t * 3 + k]]];
        triOffsets.ResizeUninitialized(vertexCount + 1);
        triOffsets[0] = 0;
        for (u32 v = 0; v < vertexCount; ++v)
            triOffsets[v + 1] = triOffsets[v] + triCount[v];
        triAdjacency.ResizeUninitialized(triOffsets[vertexCount]);
        for (u32& c : triCount)
            c = 0;
        for (u32 t = 0; t < tris; ++t)
            for (u32 k = 0; k < 3; ++k) {
                const u32 c = canon[outIndices[t * 3 + k]];
                triAdjacency[triOffsets[c] + triCount[c]++] = t;
            }

        // Candidate collapses (canonical u -> v) sorted by cost.
        Array<Collapse> candidates;
        for (const auto& e : edgeUse) {
            const u32 a = u32(e.first >> 32), b = u32(e.first);
            const bool borderEdge = e.second == 1;
            for (u32 dir = 0; dir < 2; ++dir) {
                const u32 u = dir ? b : a, v = dir ? a : b;
                if (locked[u] || (border[u] && !borderEdge))
                    continue;
                Quadric q = quadrics[u];
                q += quadrics[v];
                candidates.PushBack(Collapse{u, v, f32(q.Error(pos[v]))});
            }
        }
        if (candidates.Empty())
            break;
        Sort(candidates, [](const Collapse& x, const Collapse& y) { return x.cost < y.cost; });

        for (u32 v = 0; v < vertexCount; ++v)
            vertexRemap[v] = v;
        touched.Clear();
        touched.Resize(vertexCount, false);
        const u32 targetTris = options.targetIndexCount / 3;
        u32 estimatedTris = tris;
        u32 collapses = 0;
        for (const Collapse& c : candidates) {
            if (estimatedTris <= targetTris || c.cost > maxErrorSq)
                break;
            if (touched[c.from] || touched[c.to])
                continue;
            // Flip check on triangles around `from` that survive the collapse.
            bool flips = false;
            u32 removedTris = 0;
            for (u32 j = triOffsets[c.from]; j < triOffsets[c.from + 1] && !flips; ++j) {
                const u32 t = triAdjacency[j];
                u32 cs[3];
                bool hasTo = false;
                for (u32 k = 0; k < 3; ++k) {
                    cs[k] = canon[outIndices[t * 3 + k]];
                    hasTo |= cs[k] == c.to;
                }
                if (hasTo) {
                    ++removedTris;
                    continue;
                }
                Vec3 p[3], q[3];
                for (u32 k = 0; k < 3; ++k) {
                    p[k] = pos[cs[k]];
                    q[k] = cs[k] == c.from ? pos[c.to] : p[k];
                }
                const Vec3 n0 = Cross(p[1] - p[0], p[2] - p[0]);
                const Vec3 n1 = Cross(q[1] - q[0], q[2] - q[0]);
                flips = Dot(n0, n1) <= 0.25f * Length(n0) * Length(n1);
            }
            if (flips || removedTris == 0)
                continue;
            // Map every wedge of `from` onto a wedge of `to` it shares a triangle with.
            bool mapped = true;
            u32 mapping[16];
            const u32 wedgeCount = wedgeOffsets[c.from + 1] - wedgeOffsets[c.from];
            if (wedgeCount > 16)
                continue;
            for (u32 w = 0; w < wedgeCount && mapped; ++w) {
                const u32 wu = wedges[wedgeOffsets[c.from] + w];
                u32 target = InvalidIndex;
                bool used = false;
                for (u32 j = triOffsets[c.from]; j < triOffsets[c.from + 1]; ++j) {
                    const u32 t = triAdjacency[j];
                    bool hasWu = false;
                    u32 candidate = InvalidIndex;
                    for (u32 k = 0; k < 3; ++k) {
                        const u32 vi = outIndices[t * 3 + k];
                        hasWu |= vi == wu;
                        if (canon[vi] == c.to)
                            candidate = vi;
                    }
                    if (!hasWu)
                        continue;
                    used = true;
                    if (candidate == InvalidIndex)
                        continue;
                    if (target != InvalidIndex && target != candidate) {
                        mapped = false; // ambiguous: wedge spans an attribute seam at `to`
                        break;
                    }
                    target = candidate;
                }
                if (!used)
                    target = wu; // wedge no longer referenced
                else if (target == InvalidIndex)
                    mapped = false;
                mapping[w] = target;
            }
            if (!mapped)
                continue;
            for (u32 w = 0; w < wedgeCount; ++w)
                vertexRemap[wedges[wedgeOffsets[c.from] + w]] = mapping[w];
            quadrics[c.to] += quadrics[c.from];
            touched[c.from] = touched[c.to] = true;
            achievedErrorSq = Max(achievedErrorSq, f64(c.cost));
            estimatedTris -= Min(estimatedTris, removedTris);
            ++collapses;
        }
        if (collapses == 0)
            break;
        // Apply and drop degenerate triangles.
        u32 write = 0;
        for (u32 t = 0; t < tris; ++t) {
            const u32 a = vertexRemap[outIndices[t * 3]], b = vertexRemap[outIndices[t * 3 + 1]],
                      c = vertexRemap[outIndices[t * 3 + 2]];
            if (canon[a] == canon[b] || canon[b] == canon[c] || canon[a] == canon[c])
                continue;
            outIndices[write++] = a;
            outIndices[write++] = b;
            outIndices[write++] = c;
        }
        outIndices.Resize(write);
    }
    return f32(std::sqrt(achievedErrorSq));
}
} // namespace

u32 SplitSections(MeshData& mesh, const SplitOptions& options)
{
    if (mesh.lods.Empty())
        return 0;
    mesh.lods.Resize(1);
    MeshLod& lod = mesh.lods[0];
    Array<u32> outIndices;
    outIndices.Reserve(lod.indices.Size());
    Array<MeshSection> outSections;
    struct Range
    {
        u32 begin; // triangle range in `tris`
        u32 end;
    };
    for (const MeshSection& s : lod.sections) {
        const u32 triCount = s.indexCount / 3;
        Array<u32> tris;
        tris.ResizeUninitialized(triCount);
        Array<Vec3> centroids;
        centroids.ResizeUninitialized(triCount);
        for (u32 t = 0; t < triCount; ++t) {
            tris[t] = t;
            const u32* idx = lod.indices.Data() + s.indexOffset + t * 3;
            centroids[t] = (mesh.vertices[idx[0]].position + mesh.vertices[idx[1]].position + mesh.vertices[idx[2]].position) / 3.0f;
        }
        Array<Range> stack{Range{0, triCount}};
        Array<Range> leaves;
        while (!stack.Empty()) {
            const Range r = stack.PopBackValue();
            AABB box;
            for (u32 i = r.begin; i < r.end; ++i)
                for (u32 k = 0; k < 3; ++k)
                    box.Expand(mesh.vertices[lod.indices[s.indexOffset + tris[i] * 3 + k]].position);
            const Vec3 size = box.max - box.min;
            const f32 extent = Max(size.x, Max(size.y, size.z));
            if (r.end - r.begin <= options.maxTriangles || extent <= options.minExtent) {
                leaves.PushBack(r);
                continue;
            }
            const u32 axis = size.x >= size.y && size.x >= size.z ? 0 : size.y >= size.z ? 1 : 2;
            auto key = [&](u32 t) { return axis == 0 ? centroids[t].x : axis == 1 ? centroids[t].y : centroids[t].z; };
            Sort(Span<u32>(tris.Data() + r.begin, r.end - r.begin), [&](u32 a, u32 b) { return key(a) < key(b); });
            const u32 mid = r.begin + (r.end - r.begin) / 2;
            stack.PushBack(Range{r.begin, mid});
            stack.PushBack(Range{mid, r.end});
        }
        Sort(leaves, [](const Range& a, const Range& b) { return a.begin < b.begin; });
        for (const Range& r : leaves) {
            MeshSection ns = s;
            ns.indexOffset = u32(outIndices.Size());
            ns.indexCount = (r.end - r.begin) * 3;
            for (u32 i = r.begin; i < r.end; ++i)
                outIndices.Append(Span<const u32>(lod.indices.Data() + s.indexOffset + tris[i] * 3, 3));
            outSections.PushBack(ns);
        }
    }
    const u32 added = u32(outSections.Size() - lod.sections.Size());
    lod.indices = move(outIndices);
    lod.sections = move(outSections);
    return added;
}

void GenerateLods(MeshData& mesh, const LodOptions& options)
{
    if (mesh.lods.Empty())
        return;
    mesh.lods.Resize(1);
    Array<Vec3> positions;
    positions.ResizeUninitialized(mesh.vertices.Size());
    for (usize v = 0; v < mesh.vertices.Size(); ++v)
        positions[v] = mesh.vertices[v].position;
    const MeshLod& base = mesh.lods[0];
    const u32 sectionCount = u32(base.sections.Size());
    const u32 levels = Max(1u, options.maxLods);

    // Per section: an independent chain simplified from LOD0 with locked borders.
    Array<Array<Array<u32>>> results; // [section][lod - 1]
    results.Resize(sectionCount);
    Array<Array<f32>> errors; // meters
    errors.Resize(sectionCount);
    Array<f32> radii;
    radii.Resize(sectionCount, 0.0f);
    jobs::ParallelFor(sectionCount, 1, [&](u32 begin, u32 end) {
        for (u32 si = begin; si < end; ++si) {
            const MeshSection& s = base.sections[si];
            const Span<const u32> source(base.indices.Data() + s.indexOffset, s.indexCount);
            AABB box;
            for (u32 i : source)
                box.Expand(positions[i]);
            const Vec3 size = box.max - box.min;
            const f32 extent = Max(size.x, Max(size.y, size.z));
            radii[si] = Length(box.Extents());
            results[si].Resize(levels - 1);
            errors[si].Resize(levels - 1, 0.0f);
            u32 previous = s.indexCount / 3;
            for (u32 l = 1; l < levels; ++l) {
                const u32 target = u32(f32(s.indexCount / 3) * std::pow(options.reduction, f32(l)));
                if (target < options.minTriangles)
                    break;
                SimplifyOptions so;
                so.targetIndexCount = target * 3;
                so.maxError = options.maxError * f32(l) / f32(Max(1u, levels - 1));
                so.lockBorders = true; // section/chunk borders must match their neighbors at every LOD
                Array<u32> out;
                const f32 relError = Simplify(source, positions.View(), so, out);
                const u32 tris = u32(out.Size() / 3);
                if (tris == 0 || f32(tris) > f32(previous) * 0.85f)
                    break; // stalled (error limit / locked borders)
                results[si][l - 1] = move(out);
                errors[si][l - 1] = relError * extent;
                previous = tris;
            }
        }
    });

    for (u32 l = 1; l < levels; ++l) {
        MeshLod lod;
        bool levelUsed = false;
        for (u32 si = 0; si < sectionCount; ++si) {
            MeshSection ns = base.sections[si];
            ns.indexOffset = u32(lod.indices.Size());
            const Array<u32>& r = results[si][l - 1];
            ns.indexCount = u32(r.Size());
            ns.error = errors[si][l - 1];
            ns.screenSize = 1.0f;
            if (!r.Empty()) {
                levelUsed = true;
                lod.indices.Append(r.View());
                // Projected error <= 1 pixel: screenSize = radius / (distance * tan(fov/2)) <= 2 * radius / (error * H).
                const f32 screen = ns.error > 0.0f ? 2.0f * radii[si] / (ns.error * options.referenceHeight) : 1.0f;
                ns.screenSize = Clamp(screen, 0.0005f, 1.0f);
                for (u32 pl = l - 1; pl > 0; --pl) // keep thresholds decreasing along the chain
                    if (mesh.lods[pl].sections[si].indexCount) {
                        ns.screenSize = Min(ns.screenSize, mesh.lods[pl].sections[si].screenSize * 0.9f);
                        break;
                    }
            }
            lod.sections.PushBack(ns);
        }
        if (!levelUsed)
            break;
        mesh.lods.PushBack(move(lod));
    }
}

} // namespace zx::asset::mesh
