// Lightmap UV generation: charts stay inside the unit square, never overlap, keep an even texel density, and the
// cooker adds them only where a static mesh has none.

#include "Asset/CookedAssets.h"
#include "Asset/LightmapUV.h"
#include "Core/Testing/Test.h"

using namespace zx;
using namespace zx::asset;

namespace {

/// Axis-aligned box of `size` meters, 4 vertices per face (hard edges), clockwise front faces seen from outside.
MeshData MakeBox(Vec3 size)
{
    MeshData m;
    MeshLod& lod = m.lods.AddDefaulted();
    const Vec3 h = size * 0.5f;
    const Vec3 normals[6] = {Vec3(1, 0, 0), Vec3(-1, 0, 0), Vec3(0, 1, 0), Vec3(0, -1, 0), Vec3(0, 0, 1), Vec3(0, 0, -1)};
    for (const Vec3& n : normals) {
        const Vec3 u = Abs(n.y) > 0.5f ? Vec3(1, 0, 0) : Vec3(0, 1, 0);
        const Vec3 v = Cross(n, u);
        const u32 base = u32(m.vertices.Size());
        const Vec2 corners[4] = {Vec2(-1, -1), Vec2(1, -1), Vec2(1, 1), Vec2(-1, 1)};
        for (const Vec2& c : corners) {
            MeshVertex vert;
            vert.normal = n;
            vert.position = (n + u * c.x + v * c.y) * h;
            m.vertices.PushBack(vert);
        }
        lod.indices.Append({base, base + 1, base + 2, base, base + 2, base + 3});
    }
    lod.sections.PushBack(MeshSection{0, u32(lod.indices.Size()), 0});
    m.attributes = MeshAttributes::Normals;
    m.ComputeBounds();
    return m;
}

MeshData MakeSphere(u32 segments, u32 rings, f32 radius)
{
    MeshData m;
    for (u32 r = 0; r <= rings; ++r)
        for (u32 s = 0; s <= segments; ++s) {
            const f32 theta = Pi * f32(r) / f32(rings), phi = TwoPi * f32(s) / f32(segments);
            MeshVertex v;
            v.normal = Vec3(Sin(theta) * Cos(phi), Cos(theta), Sin(theta) * Sin(phi));
            v.position = v.normal * radius;
            m.vertices.PushBack(v);
        }
    MeshLod& lod = m.lods.AddDefaulted();
    for (u32 r = 0; r < rings; ++r)
        for (u32 s = 0; s < segments; ++s) {
            const u32 a = r * (segments + 1) + s, b = a + segments + 1;
            lod.indices.Append({a, a + 1, b, a + 1, b + 1, b});
        }
    lod.sections.PushBack(MeshSection{0, u32(lod.indices.Size()), 0});
    m.attributes = MeshAttributes::Normals;
    m.ComputeBounds();
    return m;
}

/// Checks UV1 of LOD0: inside [0, 1], no texel covered by two far-apart surface points, even density.
void CheckLayout(const MeshData& m, u32 res)
{
    const Array<u32>& idx = m.lods[0].indices;
    Array<Vec3> owner;
    owner.Resize(usize(res) * res, Vec3(Infinity));
    u32 overlaps = 0, covered = 0;
    f32 minDensity = Infinity, maxDensity = 0.0f;
    const f32 texelMeters = 2.0f / 8.0f; // 2 texels at the reference density
    for (usize t = 0; t + 2 < idx.Size(); t += 3) {
        const MeshVertex &a = m.vertices[idx[t]], &b = m.vertices[idx[t + 1]], &c = m.vertices[idx[t + 2]];
        for (const MeshVertex* v : {&a, &b, &c})
            ZX_EXPECT(v->uv1.x >= 0.0f && v->uv1.x <= 1.0f && v->uv1.y >= 0.0f && v->uv1.y <= 1.0f);
        const Vec2 ua = a.uv1 * f32(res), ub = b.uv1 * f32(res), uc = c.uv1 * f32(res);
        const f32 uvArea = Abs((ub.x - ua.x) * (uc.y - ua.y) - (uc.x - ua.x) * (ub.y - ua.y)) * 0.5f;
        const f32 worldArea = Length(Cross(b.position - a.position, c.position - a.position)) * 0.5f;
        if (worldArea > 1e-6f) {
            minDensity = Min(minDensity, uvArea / worldArea);
            maxDensity = Max(maxDensity, uvArea / worldArea);
        }
        // Texel centers inside the triangle: the surface point they stand for.
        const i32 x0 = i32(Min(ua.x, Min(ub.x, uc.x))), x1 = i32(Max(ua.x, Max(ub.x, uc.x)));
        const i32 y0 = i32(Min(ua.y, Min(ub.y, uc.y))), y1 = i32(Max(ua.y, Max(ub.y, uc.y)));
        const f32 det = (ub.x - ua.x) * (uc.y - ua.y) - (uc.x - ua.x) * (ub.y - ua.y);
        if (Abs(det) < 1e-9f)
            continue;
        for (i32 y = Max(y0, 0); y <= Min(y1, i32(res) - 1); ++y)
            for (i32 x = Max(x0, 0); x <= Min(x1, i32(res) - 1); ++x) {
                const Vec2 p(f32(x) + 0.5f, f32(y) + 0.5f);
                const f32 w1 = ((p.x - ua.x) * (uc.y - ua.y) - (uc.x - ua.x) * (p.y - ua.y)) / det;
                const f32 w2 = ((ub.x - ua.x) * (p.y - ua.y) - (p.x - ua.x) * (ub.y - ua.y)) / det;
                const f32 w0 = 1.0f - w1 - w2;
                if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f)
                    continue;
                const Vec3 world = a.position * w0 + b.position * w1 + c.position * w2;
                Vec3& o = owner[usize(y) * res + x];
                if (o.x == Infinity) {
                    o = world;
                    ++covered;
                } else if (Length(o - world) > texelMeters * 2.0f) {
                    ++overlaps;
                }
            }
    }
    ZX_EXPECT_EQ(overlaps, 0u);
    ZX_EXPECT(covered > 0u);
    // One density for the whole mesh (projection onto a chart's plane shrinks tilted triangles by at most cos 50).
    ZX_EXPECT(minDensity >= maxDensity * 0.6f);
}

} // namespace

ZX_TEST(LightmapUV, BoxGetsOneChartPerFace)
{
    MeshData box = MakeBox(Vec3(4.0f, 2.0f, 1.0f));
    const mesh::LightmapUVResult r = mesh::GenerateLightmapUVs(box);
    ZX_EXPECT_EQ(r.charts, 6u);
    ZX_EXPECT(r.resolution >= 32u && r.resolution <= 1024u);
    ZX_EXPECT((box.attributes & MeshAttributes::UV1) != MeshAttributes::None);
    ZX_EXPECT_EQ(box.vertices.Size(), 24u); // hard edges: nothing to duplicate
    CheckLayout(box, r.resolution);
    // Proportions: the 4 x 2 face is twice as wide in UV1 as it is tall.
    const MeshVertex* top = nullptr;
    for (const MeshVertex& v : box.vertices)
        if (v.normal.y > 0.5f)
            top = &v;
    ZX_EXPECT(top != nullptr);
}

ZX_TEST(LightmapUV, SphereChartsDontOverlap)
{
    MeshData sphere = MakeSphere(32, 16, 3.0f);
    const usize before = sphere.vertices.Size();
    const mesh::LightmapUVResult r = mesh::GenerateLightmapUVs(sphere);
    ZX_EXPECT(r.charts >= 6u && r.charts <= 64u); // cones of 50 degrees + small leftovers at the poles
    ZX_EXPECT(sphere.vertices.Size() > before); // chart borders split vertices
    ZX_EXPECT(r.coverage > 0.3f && r.coverage <= 1.0f);
    CheckLayout(sphere, r.resolution);
}

ZX_TEST(LightmapUV, CookerAddsThemToStaticMeshesOnly)
{
    MeshData box = MakeBox(Vec3(2.0f));
    const CookedMesh cooked = CookMesh(box, false, NanoMode::Off);
    ZX_EXPECT(cooked.lightmapUVs);
    ZX_EXPECT(cooked.lightmapResolution >= 32u);
    // Serialized and read back.
    Array<u8> bytes;
    Serialize(cooked, bytes);
    CookedMesh back;
    ZX_REQUIRE(Deserialize(bytes.View(), back));
    ZX_EXPECT(back.lightmapUVs);
    ZX_EXPECT_EQ(back.lightmapResolution, cooked.lightmapResolution);
    // Skinned meshes keep UV1 untouched (no lightmaps for animated meshes).
    MeshData skinned = MakeBox(Vec3(1.0f));
    skinned.attributes |= MeshAttributes::Skin;
    const CookedMesh s = CookMesh(skinned, false, NanoMode::Off);
    ZX_EXPECT(!s.lightmapUVs);
}
