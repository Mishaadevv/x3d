#include "Asset/LightmapUV.h"
#include "Core/Containers/HashMap.h"
#include "Core/Containers/Algorithm.h"

#include <bit>
#include <cmath>

namespace zx::asset::mesh {

namespace {

u64 PositionKey(Vec3 p)
{
    return HashCombine(HashCombine(std::bit_cast<u32>(p.x), std::bit_cast<u32>(p.y)), std::bit_cast<u32>(p.z));
}

u64 EdgeKey(u32 a, u32 b) { return a < b ? (u64(a) << 32) | b : (u64(b) << 32) | a; }

struct Chart
{
    Array<u32> triangles;
    Vec3 normal;
    Vec3 tangent, bitangent; // projection basis (rotated to the smallest rectangle)
    Vec2 min, size;          // projected bounds, meters
    u32 x = 0, y = 0;        // packed position, texels
    u32 w = 0, h = 0;        // packed size with the gutter, texels
};

/// Shelf packing of the charts (tallest first) into a square of `res` texels; false if they don't fit.
bool Pack(Array<Chart>& charts, Span<const u32> order, u32 res)
{
    u32 x = 0, y = 0, shelf = 0;
    for (u32 i : order) {
        Chart& c = charts[i];
        if (c.w > res || c.h > res)
            return false;
        if (x + c.w > res) {
            x = 0;
            y += shelf;
            shelf = 0;
        }
        if (y + c.h > res)
            return false;
        c.x = x;
        c.y = y;
        x += c.w;
        shelf = Max(shelf, c.h);
    }
    return true;
}

} // namespace

LightmapUVResult GenerateLightmapUVs(MeshData& mesh, const LightmapUVOptions& o)
{
    LightmapUVResult result;
    if (mesh.lods.Empty() || mesh.vertices.Empty())
        return result;
    Array<u32>& indices = mesh.lods[0].indices;
    const u32 triCount = u32(indices.Size() / 3);
    if (triCount == 0)
        return result;

    // Shared positions (UV / normal seams split vertices; charts must grow across them).
    Array<u32> canonical;
    canonical.Resize(mesh.vertices.Size());
    {
        HashMap<u64, u32> byPosition;
        byPosition.Reserve(mesh.vertices.Size());
        for (u32 v = 0; v < u32(mesh.vertices.Size()); ++v) {
            canonical[v] = *byPosition.Insert(PositionKey(mesh.vertices[v].position), v).first;
        }
    }
    // Face normals and areas.
    Array<Vec3> faceNormal;
    Array<f32> faceArea;
    faceNormal.Resize(triCount);
    faceArea.Resize(triCount);
    for (u32 t = 0; t < triCount; ++t) {
        const Vec3 a = mesh.vertices[indices[t * 3]].position, b = mesh.vertices[indices[t * 3 + 1]].position,
                   c = mesh.vertices[indices[t * 3 + 2]].position;
        const Vec3 n = Cross(b - a, c - a);
        const f32 len = Length(n);
        faceArea[t] = len * 0.5f;
        faceNormal[t] = len > 1e-12f ? n / len : Vec3(0, 1, 0);
    }
    // Edge adjacency over shared positions (non-manifold edges keep their first two triangles).
    Array<u32> neighbor;
    neighbor.Resize(triCount * 3, InvalidIndex);
    {
        HashMap<u64, u32> edges; // edge -> triangle * 3 + corner
        edges.Reserve(triCount * 3);
        for (u32 t = 0; t < triCount; ++t)
            for (u32 e = 0; e < 3; ++e) {
                const u64 key = EdgeKey(canonical[indices[t * 3 + e]], canonical[indices[t * 3 + (e + 1) % 3]]);
                const Pair<u32*, bool> ins = edges.Insert(key, t * 3 + e);
                if (!ins.second) {
                    const u32 other = *ins.first;
                    if (neighbor[other] == InvalidIndex && neighbor[t * 3 + e] == InvalidIndex) {
                        neighbor[other] = t;
                        neighbor[t * 3 + e] = other / 3;
                    }
                }
            }
    }

    // Charts: grow from the largest unassigned triangle while normals stay within the cone.
    const f32 cosAngle = std::cos(DegToRad(Clamp(o.chartAngle, 5.0f, 89.0f)));
    Array<u32> order;
    order.Resize(triCount);
    for (u32 t = 0; t < triCount; ++t)
        order[t] = t;
    Sort(order, [&](u32 a, u32 b) { return faceArea[a] > faceArea[b]; });
    Array<u32> chartOf;
    chartOf.Resize(triCount, InvalidIndex);
    Array<Chart> charts;
    Array<u32> queue;
    for (u32 seed : order) {
        if (chartOf[seed] != InvalidIndex)
            continue;
        const u32 id = u32(charts.Size());
        Chart& chart = charts.EmplaceBack();
        chartOf[seed] = id;
        chart.triangles.PushBack(seed);
        queue.Clear();
        queue.PushBack(seed);
        // The cone is around the seed's normal (an average would drift as the chart grows) and the chart is projected
        // onto it: no triangle tilts more than the cone's angle, so the texel density stays within cos(angle).
        const Vec3 chartN = faceNormal[seed];
        for (usize q = 0; q < queue.Size(); ++q) {
            const u32 t = queue[q];
            for (u32 e = 0; e < 3; ++e) {
                const u32 n = neighbor[t * 3 + e];
                if (n == InvalidIndex || chartOf[n] != InvalidIndex)
                    continue;
                if (Dot(faceNormal[n], chartN) < cosAngle && faceArea[n] > 1e-10f)
                    continue;
                chartOf[n] = id;
                chart.triangles.PushBack(n);
                queue.PushBack(n);
            }
        }
        chart.normal = chartN;
    }

    // Projection onto each chart's plane, turned to the smallest bounding rectangle.
    f32 totalArea = 0.0f;
    Array<Vec2> projected;
    for (Chart& c : charts) {
        const Vec3 n = c.normal;
        const Vec3 ref = Abs(n.y) < 0.9f ? Vec3(0, 1, 0) : Vec3(1, 0, 0);
        const Vec3 t0 = Normalize(Cross(ref, n)), b0 = Cross(n, t0);
        projected.Clear();
        for (u32 t : c.triangles)
            for (u32 k = 0; k < 3; ++k) {
                const Vec3 p = mesh.vertices[indices[t * 3 + k]].position;
                projected.PushBack(Vec2(Dot(p, t0), Dot(p, b0)));
            }
        f32 bestArea = Infinity, bestAngle = 0.0f;
        for (u32 step = 0; step < 18; ++step) {
            const f32 a = DegToRad(f32(step) * 5.0f), ca = std::cos(a), sa = std::sin(a);
            Vec2 lo(Infinity), hi(-Infinity);
            for (Vec2 p : projected) {
                const Vec2 r(ca * p.x + sa * p.y, -sa * p.x + ca * p.y);
                lo = Min(lo, r);
                hi = Max(hi, r);
            }
            const f32 area = (hi.x - lo.x) * (hi.y - lo.y);
            if (area < bestArea) {
                bestArea = area;
                bestAngle = a;
            }
        }
        const f32 ca = std::cos(bestAngle), sa = std::sin(bestAngle);
        c.tangent = t0 * ca + b0 * sa;
        c.bitangent = b0 * ca - t0 * sa;
        Vec2 lo(Infinity), hi(-Infinity);
        for (u32 t : c.triangles)
            for (u32 k = 0; k < 3; ++k) {
                const Vec3 p = mesh.vertices[indices[t * 3 + k]].position;
                const Vec2 r(Dot(p, c.tangent), Dot(p, c.bitangent));
                lo = Min(lo, r);
                hi = Max(hi, r);
            }
        c.min = lo;
        c.size = Max(hi - lo, Vec2(0.0f));
        totalArea += Max(c.size.x, 1e-4f) * Max(c.size.y, 1e-4f);
    }

    // Packing: start at the resolution the reference density asks for; grow, then shrink the density until it fits.
    const u32 minRes = Clamp(std::bit_ceil(Max(o.minResolution, 4u)), 4u, 8192u);
    const u32 maxRes = Max(std::bit_ceil(Max(o.maxResolution, 4u)), minRes);
    const u32 gutter = Max(o.gutterTexels, 1u);
    u32 res = Clamp(std::bit_ceil(u32(std::sqrt(totalArea) * Max(o.texelsPerMeter, 0.01f) * 1.2f) + 1u), minRes, maxRes);
    f32 density = Max(o.texelsPerMeter, 0.01f);
    Array<u32> packOrder;
    packOrder.Resize(charts.Size());
    for (u32 i = 0; i < u32(charts.Size()); ++i)
        packOrder[i] = i;
    for (u32 attempt = 0; attempt < 64; ++attempt) {
        for (Chart& c : charts) {
            c.w = u32(std::ceil(c.size.x * density)) + 1 + gutter;
            c.h = u32(std::ceil(c.size.y * density)) + 1 + gutter;
        }
        Sort(packOrder, [&](u32 a, u32 b) {
            return charts[a].h != charts[b].h ? charts[a].h > charts[b].h : charts[a].w > charts[b].w;
        });
        if (Pack(charts, packOrder.View(), res))
            break;
        if (res < maxRes)
            res *= 2;
        else
            density *= 0.9f;
    }

    // UV1 per corner; vertices on chart borders are duplicated (one copy per chart).
    Array<MeshVertex> vertices;
    vertices.Reserve(mesh.vertices.Size() + mesh.vertices.Size() / 4);
    HashMap<u64, u32> remap; // (old vertex, chart) -> new vertex
    remap.Reserve(mesh.vertices.Size());
    Array<u32> firstCopy;
    firstCopy.Resize(mesh.vertices.Size(), InvalidIndex);
    const f32 invRes = 1.0f / f32(res);
    for (u32 t = 0; t < triCount; ++t) {
        const Chart& c = charts[chartOf[t]];
        for (u32 k = 0; k < 3; ++k) {
            const u32 old = indices[t * 3 + k];
            const u64 key = (u64(old) << 32) | chartOf[t];
            const Pair<u32*, bool> ins = remap.Insert(key, u32(vertices.Size()));
            const u32* it = ins.first;
            if (ins.second) {
                MeshVertex v = mesh.vertices[old];
                const Vec2 r(Dot(v.position, c.tangent), Dot(v.position, c.bitangent));
                const Vec2 texel = Vec2(f32(c.x), f32(c.y)) + Vec2(f32(gutter) * 0.5f + 0.5f) + (r - c.min) * density;
                v.uv1 = texel * invRes;
                vertices.PushBack(v);
                if (firstCopy[old] == InvalidIndex)
                    firstCopy[old] = *it;
            }
            indices[t * 3 + k] = *it;
        }
    }
    // Other LODs (imported ones) keep pointing at a copy of their vertices.
    for (usize l = 1; l < mesh.lods.Size(); ++l)
        for (u32& i : mesh.lods[l].indices)
            if (i < firstCopy.Size()) {
                if (firstCopy[i] == InvalidIndex) {
                    firstCopy[i] = u32(vertices.Size());
                    vertices.PushBack(mesh.vertices[i]);
                }
                i = firstCopy[i];
            }
    result.addedVertices = u32(vertices.Size() > mesh.vertices.Size() ? vertices.Size() - mesh.vertices.Size() : 0);
    mesh.vertices = move(vertices);
    mesh.attributes |= MeshAttributes::UV1;
    result.charts = u32(charts.Size());
    result.resolution = res;
    f32 used = 0.0f;
    for (const Chart& c : charts)
        used += f32(c.w) * f32(c.h);
    result.coverage = used / (f32(res) * f32(res));
    return result;
}

} // namespace zx::asset::mesh
