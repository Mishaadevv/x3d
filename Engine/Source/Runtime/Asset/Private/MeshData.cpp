#include "Asset/MeshData.h"
#include "Core/Hash/Hash.h"

namespace zx::asset {

void MeshData::ComputeBounds()
{
    bounds = AABB();
    for (const MeshVertex& v : vertices)
        bounds.Expand(v.position);
}

u32 MeshData::TriangleCount(u32 lod) const
{
    if (lods.Empty())
        return 0;
    if (lod == 0)
        return u32(lods[0].indices.Size() / 3);
    lod = Min(lod, u32(lods.Size() - 1));
    u32 n = 0;
    for (usize s = 0; s < lods[0].sections.Size(); ++s) {
        u32 l = lod;
        while (l > 0 && (s >= lods[l].sections.Size() || lods[l].sections[s].indexCount == 0))
            --l;
        n += lods[l].sections[s].indexCount / 3;
    }
    return n;
}

String MeshData::Validate() const
{
    if (lods.Empty())
        return "mesh has no LODs";
    for (usize l = 0; l < lods.Size(); ++l) {
        const MeshLod& lod = lods[l];
        if (lod.indices.Size() % 3)
            return String::Format("LOD{}: index count {} is not a multiple of 3", l, lod.indices.Size());
        for (u32 i : lod.indices)
            if (i >= vertices.Size())
                return String::Format("LOD{}: index {} out of range ({} vertices)", l, i, vertices.Size());
        u64 covered = 0;
        for (const MeshSection& s : lod.sections) {
            if (u64(s.indexOffset) + s.indexCount > lod.indices.Size() || s.indexCount % 3)
                return String::Format("LOD{}: bad section [{}, +{})", l, s.indexOffset, s.indexCount);
            covered += s.indexCount;
        }
        if (covered != lod.indices.Size())
            return String::Format("LOD{}: sections cover {} of {} indices", l, covered, lod.indices.Size());
        if (l > 0 && lod.sections.Size() != lods[0].sections.Size())
            return String::Format("LOD{}: {} sections, LOD0 has {}", l, lod.sections.Size(), lods[0].sections.Size());
    }
    return {};
}

namespace mesh {

namespace {

bool SameVertex(const MeshVertex& a, const MeshVertex& b)
{
    return a.position == b.position && a.normal == b.normal && a.tangent == b.tangent && a.uv0 == b.uv0 &&
           a.uv1 == b.uv1 && a.color == b.color;
}

u64 HashVertex(const MeshVertex& v)
{
    XXHash64Stream h;
    h.UpdateValue(v.position);
    h.UpdateValue(v.normal);
    h.Update(&v.tangent, sizeof(f32) * 4);
    h.UpdateValue(v.uv0);
    h.UpdateValue(v.uv1);
    h.UpdateValue(v.color);
    return h.Digest();
}

template <typename F>
void ForEachTriangle(const MeshData& mesh, F&& f)
{
    for (const MeshLod& lod : mesh.lods)
        for (usize i = 0; i + 2 < lod.indices.Size(); i += 3)
            f(lod.indices[i], lod.indices[i + 1], lod.indices[i + 2]);
}

void RemapIndices(MeshData& mesh, const Array<u32>& remap)
{
    for (MeshLod& lod : mesh.lods)
        for (u32& i : lod.indices)
            i = remap[i];
}

} // namespace

void GenerateNormals(MeshData& mesh)
{
    Array<Vec3> accum;
    accum.Resize(mesh.vertices.Size(), Vec3());
    // Only LOD0 defines the surface.
    if (mesh.lods.Empty())
        return;
    const Array<u32>& idx = mesh.lods[0].indices;
    for (usize t = 0; t + 2 < idx.Size(); t += 3) {
        const u32 i[3] = {idx[t], idx[t + 1], idx[t + 2]};
        const Vec3 p[3] = {mesh.vertices[i[0]].position, mesh.vertices[i[1]].position, mesh.vertices[i[2]].position};
        // Clockwise front faces in a left-handed system: cross(b - a, c - a) points outwards.
        const Vec3 n = Cross(p[1] - p[0], p[2] - p[0]);
        const f32 len = Length(n);
        if (len <= 1e-20f)
            continue;
        const Vec3 fn = n / len;
        for (u32 k = 0; k < 3; ++k) {
            const Vec3 e0 = SafeNormalize(p[(k + 1) % 3] - p[k]);
            const Vec3 e1 = SafeNormalize(p[(k + 2) % 3] - p[k]);
            accum[i[k]] += fn * Acos(Dot(e0, e1)); // angle-weighted
        }
    }
    for (usize v = 0; v < mesh.vertices.Size(); ++v)
        mesh.vertices[v].normal = SafeNormalize(accum[v], Vec3::Up());
    mesh.attributes |= MeshAttributes::Normals;
}

void GenerateTangents(MeshData& mesh)
{
    if (mesh.lods.Empty())
        return;
    Array<Vec3> tan, bitan;
    tan.Resize(mesh.vertices.Size(), Vec3());
    bitan.Resize(mesh.vertices.Size(), Vec3());
    const Array<u32>& idx = mesh.lods[0].indices;
    for (usize t = 0; t + 2 < idx.Size(); t += 3) {
        const MeshVertex& a = mesh.vertices[idx[t]];
        const MeshVertex& b = mesh.vertices[idx[t + 1]];
        const MeshVertex& c = mesh.vertices[idx[t + 2]];
        const Vec3 dp1 = b.position - a.position, dp2 = c.position - a.position;
        const Vec2 duv1 = b.uv0 - a.uv0, duv2 = c.uv0 - a.uv0;
        const f32 det = duv1.x * duv2.y - duv2.x * duv1.y;
        if (Abs(det) < 1e-20f)
            continue;
        const f32 r = 1.0f / det;
        const Vec3 tdir = (dp1 * duv2.y - dp2 * duv1.y) * r; // direction of +u
        const Vec3 bdir = (dp2 * duv1.x - dp1 * duv2.x) * r; // direction of +v
        for (usize k = 0; k < 3; ++k) {
            tan[idx[t + k]] += tdir;
            bitan[idx[t + k]] += bdir;
        }
    }
    for (usize v = 0; v < mesh.vertices.Size(); ++v) {
        MeshVertex& vx = mesh.vertices[v];
        const Vec3 n = vx.normal;
        Vec3 t = tan[v] - n * Dot(n, tan[v]);
        if (LengthSq(t) < 1e-20f) {
            Vec3 b;
            OrthonormalBasis(n, t, b);
        }
        t = Normalize(t);
        // Normal maps use the glTF/OpenGL convention: green = towards the top of the image = -v (UV origin top-left).
        const f32 w = Dot(Cross(n, t), -bitan[v]) < 0.0f ? -1.0f : 1.0f;
        vx.tangent = Vec4(t, w);
    }
    mesh.attributes |= MeshAttributes::Tangents;
}

void WeldVertices(MeshData& mesh)
{
    const usize n = mesh.vertices.Size();
    if (n == 0)
        return;
    usize cap = 1;
    while (cap < n * 2)
        cap <<= 1;
    Array<u32> table;
    table.Resize(cap, InvalidIndex);
    Array<u32> remap;
    remap.ResizeUninitialized(n);
    Array<MeshVertex> unique;
    unique.Reserve(n);
    for (usize v = 0; v < n; ++v) {
        const MeshVertex& vx = mesh.vertices[v];
        usize slot = usize(HashVertex(vx)) & (cap - 1);
        for (;;) {
            const u32 existing = table[slot];
            if (existing == InvalidIndex) {
                table[slot] = u32(unique.Size());
                remap[v] = u32(unique.Size());
                unique.PushBack(vx);
                break;
            }
            if (SameVertex(unique[existing], vx)) {
                remap[v] = existing;
                break;
            }
            slot = (slot + 1) & (cap - 1);
        }
    }
    RemapIndices(mesh, remap);
    mesh.vertices = move(unique);
}

void RemoveUnusedVertices(MeshData& mesh)
{
    Array<u32> remap;
    remap.Resize(mesh.vertices.Size(), InvalidIndex);
    Array<MeshVertex> used;
    ForEachTriangle(mesh, [&](u32 a, u32 b, u32 c) {
        for (u32 i : {a, b, c})
            if (remap[i] == InvalidIndex) {
                remap[i] = u32(used.Size());
                used.PushBack(mesh.vertices[i]);
            }
    });
    RemapIndices(mesh, remap);
    mesh.vertices = move(used);
}

u32 RemoveDegenerateTriangles(MeshData& mesh)
{
    u32 removed = 0;
    for (MeshLod& lod : mesh.lods) {
        Array<u32> out;
        out.Reserve(lod.indices.Size());
        for (MeshSection& s : lod.sections) {
            const u32 start = u32(out.Size());
            for (u32 t = s.indexOffset; t + 2 < s.indexOffset + s.indexCount; t += 3) {
                const u32 a = lod.indices[t], b = lod.indices[t + 1], c = lod.indices[t + 2];
                const Vec3 pa = mesh.vertices[a].position, pb = mesh.vertices[b].position, pc = mesh.vertices[c].position;
                if (a == b || b == c || a == c || pa == pb || pb == pc || pa == pc || LengthSq(Cross(pb - pa, pc - pa)) == 0.0f) {
                    ++removed;
                    continue;
                }
                out.Append({a, b, c});
            }
            s.indexOffset = start;
            s.indexCount = u32(out.Size()) - start;
        }
        lod.indices = move(out);
        if (mesh.lods.Size() == 1) // generated LODs keep their 1:1 section layout
            lod.sections.RemoveIf([](const MeshSection& s) { return s.indexCount == 0; });
    }
    return removed;
}

void MirrorX(MeshData& mesh)
{
    // Right-handed -> left-handed: the rendered image stays the same, so counter-clockwise front faces stay
    // counter-clockwise on screen - reverse them to the engine's clockwise convention. cross(N', T') = -M cross(N, T),
    // so the tangent handedness flips too.
    for (MeshVertex& v : mesh.vertices) {
        v.position.x = -v.position.x;
        v.normal.x = -v.normal.x;
        v.tangent.x = -v.tangent.x;
        v.tangent.w = -v.tangent.w;
    }
    for (MeshLod& lod : mesh.lods)
        for (usize i = 0; i + 2 < lod.indices.Size(); i += 3)
            std::swap(lod.indices[i + 1], lod.indices[i + 2]);
    mesh.ComputeBounds();
}

void Transform(MeshData& mesh, const Mat4& m)
{
    const Mat4 normalMatrix = Transpose(Inverse(m));
    const bool flip = Determinant(m) < 0.0f;
    for (MeshVertex& v : mesh.vertices) {
        v.position = m.TransformPoint(v.position);
        v.normal = SafeNormalize(normalMatrix.TransformVector(v.normal), Vec3::Up());
        const Vec3 t = SafeNormalize(m.TransformVector(v.tangent.XYZ()), Vec3::Right());
        v.tangent = Vec4(t, flip ? -v.tangent.w : v.tangent.w);
    }
    if (flip)
        for (MeshLod& lod : mesh.lods)
            for (usize i = 0; i + 2 < lod.indices.Size(); i += 3)
                std::swap(lod.indices[i + 1], lod.indices[i + 2]);
    mesh.ComputeBounds();
}

void Append(MeshData& dst, const MeshData& src, u32 materialOffset)
{
    const u32 base = u32(dst.vertices.Size());
    dst.vertices.Append(src.vertices.View());
    if (dst.lods.Empty())
        dst.lods.AddDefaulted();
    MeshLod& lod = dst.lods[0];
    if (src.lods.Empty())
        return;
    for (const MeshSection& s : src.lods[0].sections) {
        MeshSection ns;
        ns.indexOffset = u32(lod.indices.Size());
        ns.indexCount = s.indexCount;
        ns.materialIndex = s.materialIndex + materialOffset;
        for (u32 i = 0; i < s.indexCount; ++i)
            lod.indices.PushBack(src.lods[0].indices[s.indexOffset + i] + base);
        lod.sections.PushBack(ns);
    }
    dst.attributes |= src.attributes;
    dst.bounds.Expand(src.bounds);
}

} // namespace mesh

} // namespace zx::asset
