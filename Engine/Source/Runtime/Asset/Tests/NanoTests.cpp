// ZNano: cluster limits, the LOD DAG's invariants (monotonic errors, spheres containing their children), and cuts that
// get coarser with distance while covering the same surface (no holes, no double layers).

#include "Asset/MeshOptimizer.h"
#include "Asset/Nano.h"
#include "Core/Testing/Test.h"

#include <cstdio>

using namespace zx;
using namespace zx::asset;

namespace {

/// A bumpy terrain patch: (n x n) quads, 2 n^2 triangles, 20 x 20 m.
MeshData BumpyGrid(u32 n)
{
    MeshData m;
    m.name = "Bumpy";
    m.attributes = MeshAttributes::Normals | MeshAttributes::UV0;
    for (u32 z = 0; z <= n; ++z)
        for (u32 x = 0; x <= n; ++x) {
            MeshVertex v;
            const f32 fx = f32(x) / f32(n), fz = f32(z) / f32(n);
            v.position = Vec3(fx * 20.0f - 10.0f, Sin(fx * 9.0f) * Cos(fz * 7.0f) * 1.5f + Sin(fx * 31.0f + fz * 17.0f) * 0.2f, fz * 20.0f - 10.0f);
            v.normal = Vec3(0, 1, 0);
            v.uv0 = Vec2(fx, fz);
            m.vertices.PushBack(v);
        }
    MeshLod lod;
    for (u32 z = 0; z < n; ++z)
        for (u32 x = 0; x < n; ++x) {
            const u32 a = z * (n + 1) + x, b = a + 1, c = a + n + 1, d = c + 1;
            lod.indices.Append(Span<const u32>({a, c, b, b, c, d}));
        }
    lod.sections.PushBack(MeshSection{0, u32(lod.indices.Size()), 0});
    m.lods.PushBack(move(lod));
    m.ComputeBounds();
    return m;
}

f32 Area(const CookedMesh& mesh, const NanoMesh& nm, Span<const u32> clusters)
{
    f32 area = 0.0f;
    for (u32 ci : clusters) {
        const NanoClusterHeader& c = nm.clusters[ci];
        for (u32 t = 0; t < c.triangleCount; ++t) {
            const u32 packed = nm.triangles[c.triangleStart + t];
            Vec3 p[3];
            for (u32 k = 0; k < 3; ++k)
                p[k] = mesh.vertices[nm.vertexRemap[c.vertexStart + ((packed >> (k * 8)) & 0xFF)]].position;
            area += Length(Cross(p[1] - p[0], p[2] - p[0])) * 0.5f;
        }
    }
    return area;
}

} // namespace

ZX_TEST(Nano, ClusterDagInvariants)
{
    MeshData data = BumpyGrid(256); // 131072 triangles
    const CookedMesh mesh = CookMesh(data, true, NanoMode::On);
    const NanoMesh& nm = mesh.nano;
    ZX_REQUIRE(!nm.Empty());
    ZX_EXPECT_EQ(nm.leafTriangles, 131072u);
    ZX_EXPECT(nm.levels >= 5u);
    u32 leaves = 0, roots = 0;
    u64 leafTris = 0;
    for (usize i = 0; i < nm.clusters.Size(); ++i) {
        const NanoClusterHeader& c = nm.clusters[i];
        ZX_EXPECT(c.triangleCount >= 1 && c.triangleCount <= 128);
        ZX_EXPECT(c.vertexCount <= 255);
        ZX_EXPECT(c.parentError >= c.selfError);
        if (i > 0) // sorted coarse to fine
            ZX_EXPECT(c.parentError <= nm.clusters[i - 1].parentError);
        if (c.parentError < NanoRootError) // the parent's sphere holds this level's sphere
            ZX_EXPECT(c.parentSphere.w + 1e-3f >= Length(c.parentSphere.XYZ() - c.selfSphere.XYZ()) + c.selfSphere.w);
        else
            ++roots;
        if (c.level == 0) {
            ++leaves;
            leafTris += c.triangleCount;
        }
    }
    ZX_EXPECT_EQ(leafTris, 131072ull);
    ZX_EXPECT(roots < leaves / 20); // the hierarchy reaches far up (the patch's open outer edge stays locked)
    // Fallback LOD chain cut from the DAG: fewer triangles and smaller switch sizes down the chain.
    ZX_REQUIRE(mesh.sections.Size() == 1);
    const CookedMeshSection& section = mesh.sections[0];
    ZX_EXPECT(section.lodCount >= 3u);
    ZX_EXPECT_EQ(mesh.SectionLod(section, 0).indexCount, 131072u * 3);
    for (u32 l = 1; l < section.lodCount; ++l) {
        ZX_EXPECT(mesh.SectionLod(section, l).indexCount < mesh.SectionLod(section, l - 1).indexCount);
        ZX_EXPECT(mesh.SectionLod(section, l).screenSize < mesh.SectionLod(section, l - 1).screenSize);
    }
    // Serialization round trip.
    Array<u8> bytes;
    Serialize(mesh, bytes);
    CookedMesh back;
    ZX_REQUIRE(Deserialize(bytes.View(), back));
    ZX_EXPECT_EQ(back.nano.clusters.Size(), nm.clusters.Size());
    ZX_EXPECT_EQ(back.nano.triangles.Size(), nm.triangles.Size());
}

ZX_TEST(Nano, CutsGetCoarserWithoutHoles)
{
    MeshData data = BumpyGrid(256);
    const CookedMesh mesh = CookMesh(data, true, NanoMode::On);
    const NanoMesh& nm = mesh.nano;
    Array<u32> all;
    for (u32 i = 0; i < nm.clusters.Size(); ++i)
        if (nm.clusters[i].level == 0)
            all.PushBack(i);
    const f32 fullArea = Area(mesh, nm, all.View());
    const f32 pixelsPerUnit = 1080.0f / (2.0f * Tan(DegToRad(30.0f)));
    u64 prevTris = ~0ull;
    for (f32 distance : {2.0f, 15.0f, 60.0f, 250.0f, 1000.0f}) {
        Array<u32> cut;
        nano::SelectCut(nm, Vec3(0.0f, distance, -distance), pixelsPerUnit, 1.0f, cut);
        u64 tris = 0;
        for (u32 c : cut)
            tris += nm.clusters[c].triangleCount;
        ZX_EXPECT(!cut.Empty());
        ZX_EXPECT(tris <= prevTris); // farther: fewer triangles
        prevTris = tris;
        // The surface is covered once: the simplified levels keep the area within a few percent.
        const f32 area = Area(mesh, nm, cut.View());
        ZX_EXPECT(Abs(area - fullArea) < fullArea * 0.06f);
    }
    ZX_EXPECT(prevTris < 131072ull / 20); // 1 km away: a small fraction
}

ZX_TEST(Nano, ClosedMeshReducesToAFewClusters)
{
    MeshData rock = nano::MakeDetailedRock(6, 3); // 81920 triangles, closed: no locked outer border
    const CookedMesh mesh = CookMesh(rock, true, NanoMode::On);
    const NanoMesh& nm = mesh.nano;
    ZX_EXPECT_EQ(nm.leafTriangles, 81920u);
    u64 rootTris = 0;
    for (const NanoClusterHeader& c : nm.clusters)
        if (c.parentError >= NanoRootError)
            rootTris += c.triangleCount;
    ZX_EXPECT(rootTris < 2000ull); // the coarsest cut is tiny
    ZX_EXPECT(nm.levels < 24u);
}

ZX_TEST(ManualNano, RockDagStats)
{
    for (u32 sub : {6u, 8u}) {
        MeshData rock = nano::MakeDetailedRock(sub, 1);
        const CookedMesh mesh = CookMesh(rock, true, NanoMode::On);
        const NanoMesh& nm = mesh.nano;
        u32 perLevel[64] = {}, rootsPerLevel[64] = {};
        u64 rootTris = 0;
        for (const NanoClusterHeader& c : nm.clusters) {
            perLevel[Min(c.level, 63u)] += c.triangleCount;
            if (c.parentError >= NanoRootError) {
                rootTris += c.triangleCount;
                rootsPerLevel[Min(c.level, 63u)] += c.triangleCount;
            }
        }
        String line = String::Format("sub {}: {} levels, root tris {}; per level:", sub, nm.levels, rootTris);
        for (u32 l = 0; l < nm.levels; ++l)
            line += String::Format(" {}({})", perLevel[l], rootsPerLevel[l]);
        std::printf("%s\n", line.CStr());
    }
}
