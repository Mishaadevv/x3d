// Mesh importers (glTF/OBJ, Tests/Data/Models) and mesh processing.

#include "Asset/MeshOptimizer.h"
#include "Asset/SceneImport.h"
#include "Core/CoreInit.h"
#include "Core/Math/Random.h"
#include "Core/String/Base64.h"
#include "Core/HAL/PlatformMisc.h"
#include "Core/Jobs/JobSystem.h"
#include "Core/Testing/Test.h"
#include "Core/Time/Time.h"

using namespace zx;
using namespace zx::asset;

namespace {

String ModelPath(StringView name) { return FindEngineRootDirectory() + "/Tests/Data/Models/" + name; }

/// Fraction of triangles whose clockwise geometric normal agrees with the averaged vertex normals.
f32 WindingAgreement(const MeshData& mesh)
{
    const Array<u32>& idx = mesh.lods[0].indices;
    u32 good = 0, total = 0;
    for (usize t = 0; t + 2 < idx.Size(); t += 3) {
        const MeshVertex& a = mesh.vertices[idx[t]];
        const MeshVertex& b = mesh.vertices[idx[t + 1]];
        const MeshVertex& c = mesh.vertices[idx[t + 2]];
        const Vec3 geometric = Cross(b.position - a.position, c.position - a.position);
        good += Dot(geometric, a.normal + b.normal + c.normal) > 0.0f ? 1 : 0;
        ++total;
    }
    return total ? f32(good) / f32(total) : 0.0f;
}

MeshData MakeGrid(u32 n, bool shuffle)
{
    MeshData m;
    for (u32 y = 0; y <= n; ++y)
        for (u32 x = 0; x <= n; ++x) {
            MeshVertex v;
            v.position = Vec3(f32(x), 0.0f, f32(y));
            v.normal = Vec3::Up();
            v.uv0 = Vec2(f32(x) / f32(n), f32(y) / f32(n));
            m.vertices.PushBack(v);
        }
    MeshLod& lod = m.lods.AddDefaulted();
    for (u32 y = 0; y < n; ++y)
        for (u32 x = 0; x < n; ++x) {
            const u32 i = y * (n + 1) + x;
            // Clockwise seen from +Y (top) in a left-handed system.
            lod.indices.Append({i, i + n + 1, i + 1, i + 1, i + n + 1, i + n + 2});
        }
    if (shuffle) {
        Random rng(7);
        const u32 tris = u32(lod.indices.Size() / 3);
        for (u32 t = tris - 1; t > 0; --t) {
            const u32 j = rng.NextU32() % (t + 1);
            for (u32 k = 0; k < 3; ++k)
                std::swap(lod.indices[t * 3 + k], lod.indices[j * 3 + k]);
        }
    }
    lod.sections.PushBack(MeshSection{0, u32(lod.indices.Size()), 0});
    m.attributes = MeshAttributes::Normals | MeshAttributes::UV0;
    m.ComputeBounds();
    return m;
}

/// UV sphere with a duplicated seam column (exercises seam handling in the simplifier).
MeshData MakeSphere(u32 segments, u32 rings)
{
    MeshData m;
    for (u32 r = 0; r <= rings; ++r)
        for (u32 s = 0; s <= segments; ++s) {
            const f32 theta = Pi * f32(r) / f32(rings), phi = TwoPi * f32(s) / f32(segments);
            MeshVertex v;
            v.normal = Vec3(Sin(theta) * Cos(phi), Cos(theta), Sin(theta) * Sin(phi));
            v.position = v.normal;
            v.uv0 = Vec2(f32(s) / f32(segments), f32(r) / f32(rings));
            m.vertices.PushBack(v);
        }
    MeshLod& lod = m.lods.AddDefaulted();
    for (u32 r = 0; r < rings; ++r)
        for (u32 s = 0; s < segments; ++s) {
            const u32 a = r * (segments + 1) + s, b = a + segments + 1;
            lod.indices.Append({a, a + 1, b, a + 1, b + 1, b});
        }
    lod.sections.PushBack(MeshSection{0, u32(lod.indices.Size()), 0});
    // Fix winding to the engine convention if needed.
    if (WindingAgreement(m) < 0.5f)
        for (usize i = 0; i + 2 < lod.indices.Size(); i += 3)
            std::swap(lod.indices[i + 1], lod.indices[i + 2]);
    m.attributes = MeshAttributes::Normals | MeshAttributes::UV0;
    m.ComputeBounds();
    return m;
}

} // namespace

ZX_TEST(Base64, RoundTrip)
{
    const char* text = "ZeqouX3D engine!";
    for (usize len = 0; len <= 16; ++len) {
        const ByteSpan input(reinterpret_cast<const u8*>(text), len);
        Array<u8> decoded;
        ZX_EXPECT(Base64Decode(Base64Encode(input), decoded));
        ZX_EXPECT(decoded.Size() == len && (len == 0 || MemCompare(decoded.Data(), text, len) == 0));
    }
    ZX_EXPECT(Base64Encode(ByteSpan(reinterpret_cast<const u8*>("Man"), 3)) == "TWFu");
    ZX_EXPECT(UriDecode("red%20diffuse.png") == "red diffuse.png");
}

ZX_TEST(GltfImport, Box)
{
    for (StringView file : {"Box.gltf", "BoxInterleaved.gltf"}) {
        Result<ImportedScene> scene = ImportScene(ModelPath(file));
        if (!scene) {
            test::ReportFailure(__FILE__, __LINE__, String::Format("{}: {}", file, scene.GetError().message));
            continue;
        }
        ZX_REQUIRE(scene->meshes.Size() == 1);
        const MeshData& mesh = scene->meshes[0];
        ZX_EXPECT_EQ(mesh.TriangleCount(), 12u);
        ZX_EXPECT(mesh.Validate().Empty());
        ZX_EXPECT_EQ(WindingAgreement(mesh), 1.0f);
        ZX_EXPECT(Length(mesh.bounds.Extents() - Vec3(0.5f)) < 1e-4f);
        ZX_EXPECT_EQ(scene->nodes.Size(), usize(2));
        ZX_EXPECT_EQ(scene->rootNodes.Size(), usize(1));
        ZX_EXPECT_EQ(scene->nodes[1].parent, 0);
        ZX_EXPECT_EQ(scene->nodes[1].mesh, 0);
        ZX_EXPECT(!scene->materials.Empty());
        // Root node matrix rotates -90 degrees around X (Z-up -> Y-up); mirrored to left-handed it stays a pure rotation.
        ZX_EXPECT(Length(scene->nodes[0].local.scale - Vec3::One()) < 1e-4f);
    }
}

ZX_TEST(GltfImport, TexturedGlb)
{
    Result<ImportedScene> scene = ImportScene(ModelPath("BoxTextured.glb"));
    ZX_REQUIRE(scene.IsOk());
    ZX_REQUIRE(scene->textures.Size() == 1);
    const ImportedTexture& tex = scene->textures[0];
    ZX_EXPECT(tex.path.Empty() && tex.data.Size() > 100);
    ZX_EXPECT(tex.mimeType == "image/png" && tex.srgb);
    ZX_EXPECT(scene->materials[0].baseColorTexture.texture == 0);
    const MeshData& mesh = scene->meshes[0];
    ZX_EXPECT((mesh.attributes & MeshAttributes::UV0) != MeshAttributes::None);
    ZX_EXPECT((mesh.attributes & MeshAttributes::Tangents) != MeshAttributes::None); // generated
    ZX_EXPECT_EQ(WindingAgreement(mesh), 1.0f);
    bool tangentsOk = true;
    for (const MeshVertex& v : mesh.vertices)
        tangentsOk &= Abs(Dot(v.normal, v.tangent.XYZ())) < 1e-3f && Abs(Abs(v.tangent.w) - 1.0f) < 1e-6f;
    ZX_EXPECT(tangentsOk);
}

ZX_TEST(GltfImport, EmbeddedAndSparse)
{
    Result<ImportedScene> tri = ImportScene(ModelPath("TriangleWithoutIndices.gltf"));
    ZX_REQUIRE(tri.IsOk());
    ZX_EXPECT_EQ(tri->meshes[0].TriangleCount(), 1u);
    ZX_EXPECT_EQ(WindingAgreement(tri->meshes[0]), 1.0f); // flat normals generated after the winding flip

    Result<ImportedScene> sparse = ImportScene(ModelPath("SimpleSparseAccessor.gltf"));
    ZX_REQUIRE(sparse.IsOk());
    const AABB& b = sparse->meshes[0].bounds;
    ZX_EXPECT_EQ(b.max.y, 4.0f); // displaced by the sparse accessor
    ZX_EXPECT_EQ(b.min.x, -6.0f); // mirrored X
    ZX_EXPECT_EQ(sparse->meshes[0].TriangleCount(), 12u);
}

ZX_TEST(ObjImport, CubeWithMaterials)
{
    Result<ImportedScene> scene = ImportScene(ModelPath("cube.obj"));
    ZX_REQUIRE(scene.IsOk());
    ZX_REQUIRE(scene->meshes.Size() == 1 && scene->materials.Size() == 2);
    const MeshData& mesh = scene->meshes[0];
    ZX_EXPECT_EQ(mesh.TriangleCount(), 12u);
    ZX_EXPECT_EQ(mesh.vertices.Size(), usize(24)); // welded corners: 4 per face
    ZX_EXPECT_EQ(WindingAgreement(mesh), 1.0f);
    ZX_EXPECT(mesh.Validate().Empty());
    ZX_REQUIRE(mesh.lods[0].sections.Size() == 2);
    const ImportedMaterial& red = scene->materials[mesh.lods[0].sections[0].materialIndex];
    const ImportedMaterial& blue = scene->materials[mesh.lods[0].sections[1].materialIndex];
    ZX_EXPECT(red.name == "Red" && blue.name == "Blue");
    ZX_EXPECT(red.baseColor.x > 0.7f && red.roughness < 0.2f);
    ZX_EXPECT(blue.alphaMode == AlphaMode::Blend);
    ZX_REQUIRE(red.baseColorTexture.texture >= 0);
    ZX_EXPECT(scene->textures[usize(red.baseColorTexture.texture)].path.View().EndsWith("textures/red diffuse.png"));
    // UV origin flipped to top-left: front face corner (-1,-1,1) has vt (0,0) -> uv (0,1).
    bool found = false;
    for (const MeshVertex& v : mesh.vertices)
        if (v.position == Vec3(1, -1, 1) && v.normal == Vec3(0, 0, 1)) // x mirrored
            found = v.uv0 == Vec2(0, 1);
    ZX_EXPECT(found);
}

ZX_TEST(MeshProcessing, NormalsWeldDegenerate)
{
    MeshData sphere = MakeSphere(24, 12);
    for (MeshVertex& v : sphere.vertices)
        v.normal = Vec3();
    mesh::GenerateNormals(sphere);
    f32 maxErr = 0.0f;
    for (const MeshVertex& v : sphere.vertices)
        if (Abs(v.position.y) < 0.99f && v.uv0.x > 0.0f && v.uv0.x < 1.0f) // skip poles and the unwelded seam
            maxErr = Max(maxErr, Length(v.normal - Normalize(v.position)));
    ZX_EXPECT(maxErr < 0.05f);

    MeshData grid = MakeGrid(4, false);
    const usize before = grid.vertices.Size();
    grid.vertices.Append(grid.vertices.View()); // duplicates
    for (u32& i : grid.lods[0].indices)
        i += u32(before);
    mesh::WeldVertices(grid);
    ZX_EXPECT_EQ(grid.vertices.Size(), before);
    grid.lods[0].indices.Append({0, 0, 1});
    grid.lods[0].sections[0].indexCount += 3;
    ZX_EXPECT_EQ(mesh::RemoveDegenerateTriangles(grid), 1u);
    ZX_EXPECT(grid.Validate().Empty());
}

ZX_TEST(MeshProcessing, VertexCacheOptimization)
{
    MeshData grid = MakeGrid(64, true);
    const u32 vc = u32(grid.vertices.Size());
    const f32 before = mesh::ComputeACMR(grid.lods[0].indices.View(), vc);
    mesh::OptimizeMesh(grid);
    const f32 after = mesh::ComputeACMR(grid.lods[0].indices.View(), vc);
    ZX_EXPECT(before > 1.5f);
    ZX_EXPECT(after < 0.8f);
    ZX_EXPECT(grid.Validate().Empty());
    // Vertex fetch order: first index is 0 and indices grow by first use.
    ZX_EXPECT_EQ(grid.lods[0].indices[0], 0u);
}

ZX_TEST(MeshProcessing, SimplifyAndLods)
{
    // A flat grid collapses to (almost) nothing with zero error.
    MeshData grid = MakeGrid(32, false);
    Array<Vec3> gp;
    for (const MeshVertex& v : grid.vertices)
        gp.PushBack(v.position);
    mesh::SimplifyOptions so;
    so.targetIndexCount = 3 * 64;
    Array<u32> out;
    const f32 flatError = mesh::Simplify(grid.lods[0].indices.View(), gp.View(), so, out);
    ZX_EXPECT(out.Size() <= so.targetIndexCount);
    ZX_EXPECT(flatError < 1e-4f);

    // A sphere keeps its shape at 25% triangles; seams stay closed (no holes = no new border edges).
    MeshData sphere = MakeSphere(48, 24);
    Array<Vec3> sp;
    for (const MeshVertex& v : sphere.vertices)
        sp.PushBack(v.position);
    so.targetIndexCount = u32(sphere.lods[0].indices.Size() / 4) / 3 * 3;
    so.maxError = 0.05f;
    const f32 sphereError = mesh::Simplify(sphere.lods[0].indices.View(), sp.View(), so, out);
    ZX_EXPECT(out.Size() <= sphere.lods[0].indices.Size() / 3);
    ZX_EXPECT(sphereError < 0.03f);
    bool onSurface = true;
    for (u32 i : out)
        onSurface &= Abs(Length(sp[i]) - 1.0f) < 1e-4f; // collapses onto existing vertices only
    ZX_EXPECT(onSurface);

    mesh::GenerateLods(sphere);
    ZX_EXPECT(sphere.lods.Size() >= 3);
    for (usize l = 1; l < sphere.lods.Size(); ++l) {
        ZX_EXPECT(sphere.TriangleCount(u32(l)) < sphere.TriangleCount(u32(l - 1)));
        ZX_EXPECT(sphere.lods[l].sections[0].error > 0.0f);
        if (l > 1)
            ZX_EXPECT(sphere.lods[l].sections[0].screenSize < sphere.lods[l - 1].sections[0].screenSize);
    }
    ZX_EXPECT(sphere.Validate().Empty());
    mesh::OptimizeMesh(sphere);
    ZX_EXPECT(sphere.Validate().Empty());
}

ZX_TEST(MeshProcessing, SplitSectionsIntoChunks)
{
    // 128 m x 128 m grid with gentle bumps: 32768 triangles in one section.
    MeshData terrain = MakeGrid(128, false);
    for (MeshVertex& v : terrain.vertices)
        v.position.y = Sin(v.position.x * 0.3f) * Cos(v.position.z * 0.2f);
    const u32 tris = terrain.TriangleCount();
    const u32 added = mesh::SplitSections(terrain);
    const Array<MeshSection>& sections = terrain.lods[0].sections;
    ZX_EXPECT(added >= 7);
    ZX_EXPECT_EQ(terrain.TriangleCount(), tris);
    bool small = true;
    for (const MeshSection& s : sections)
        small &= s.indexCount / 3 <= 4096;
    ZX_EXPECT(small);
    ZX_EXPECT(terrain.Validate().Empty());

    mesh::GenerateLods(terrain);
    ZX_REQUIRE(terrain.lods.Size() >= 2);
    ZX_EXPECT(terrain.Validate().Empty());
    ZX_EXPECT(terrain.TriangleCount(1) < tris);
    // Locked borders: every chunk-border vertex of LOD0 is still referenced by LOD1 (no cracks between chunks).
    Array<u32> chunkOf;
    chunkOf.Resize(terrain.vertices.Size(), InvalidIndex);
    Array<bool> border;
    border.Resize(terrain.vertices.Size(), false);
    for (u32 s = 0; s < sections.Size(); ++s)
        for (u32 i = 0; i < sections[s].indexCount; ++i) {
            const u32 v = terrain.lods[0].indices[sections[s].indexOffset + i];
            if (chunkOf[v] != InvalidIndex && chunkOf[v] != s)
                border[v] = true;
            chunkOf[v] = s;
        }
    Array<bool> used;
    used.Resize(terrain.vertices.Size(), false);
    for (u32 i : terrain.lods[1].indices)
        used[i] = true;
    bool crackFree = true;
    u32 borderCount = 0;
    for (usize v = 0; v < border.Size(); ++v)
        if (border[v]) {
            ++borderCount;
            // Sections that kept LOD0 at level 1 don't appear in lods[1].indices; only check fully reduced grids.
            crackFree &= used[v] || terrain.lods[1].sections.Size() != sections.Size();
        }
    ZX_EXPECT(borderCount > 100);
    ZX_EXPECT(crackFree);
}

// Real-world content (Tools/fetch_samples.ps1). Run explicitly: ZXTests "ManualAsset.*"
ZX_TEST(ManualAsset, ImportSamples)
{
    jobs::Initialize();
    struct JobsShutdown
    {
        ~JobsShutdown() { jobs::Shutdown(); }
    } jobsShutdown;
    for (StringView file : {"Sponza/Sponza.gltf", "DamagedHelmet/DamagedHelmet.gltf", "NormalTangentMirrorTest/NormalTangentMirrorTest.gltf"}) {
        const String path = FindEngineRootDirectory() + "/Samples/Content/" + file;
        Stopwatch sw;
        Result<ImportedScene> scene = ImportScene(path);
        const f64 importMs = sw.ElapsedMilliseconds();
        if (!scene) {
            test::ReportFailure(__FILE__, __LINE__, String::Format("{}: {}", file, scene.GetError().message));
            continue;
        }
        f32 worstWinding = 1.0f;
        u32 tris = 0, verts = 0;
        sw.Restart();
        u32 lodCount = 0;
        f64 lodOnlyMs = 0.0;
        for (MeshData& m : scene->meshes) {
            ZX_EXPECT(m.Validate().Empty());
            worstWinding = Min(worstWinding, WindingAgreement(m));
            tris += m.TriangleCount();
            verts += u32(m.vertices.Size());
            Stopwatch part;
            mesh::GenerateLods(m);
            lodOnlyMs += part.ElapsedMilliseconds();
            mesh::OptimizeMesh(m);
            lodCount += u32(m.lods.Size());
        }
        const f64 lodMs = sw.ElapsedMilliseconds();
        char line[512];
        const usize n = FormatToBuffer(line, sizeof(line),
                                       "  {}: {} meshes, {} tris, {} verts, {} materials, {} textures, import {:.0f} ms, "
                                       "LODs+optimize {:.0f} ms (LODs {:.0f} ms, {} LODs), winding agreement {:.3f}\n",
                                       file, scene->meshes.Size(), tris, verts, scene->materials.Size(),
                                       scene->textures.Size(), importMs, lodMs, lodOnlyMs, lodCount, worstWinding);
        platform::ConsoleWrite(StringView(line, n));
        ZX_EXPECT(worstWinding > 0.95f);
    }
}
