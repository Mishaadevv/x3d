#include "Engine/BuiltinAssets.h"

namespace zx {

namespace {

const Guid BuiltinNamespace(0x5A58425549544E31ull, 0x4D45534845533031ull); // "ZXBUITN1MESHES01"

constexpr StringView ShapeNames[] = {"Cube", "Sphere", "Plane", "Cylinder"};

/// Accumulates vertices/triangles; triangles are given counter-clockwise as seen from outside and stored clockwise
/// (the engine's front faces are clockwise in a left-handed system).
struct Builder
{
    asset::MeshData mesh;

    u32 Vertex(Vec3 p, Vec3 n, Vec2 uv)
    {
        asset::MeshVertex v;
        v.position = p;
        v.normal = n;
        v.uv0 = uv;
        v.uv1 = uv;
        mesh.vertices.PushBack(v);
        return u32(mesh.vertices.Size() - 1);
    }
    void Triangle(u32 a, u32 b, u32 c)
    {
        Array<u32>& idx = mesh.lods[0].indices;
        idx.PushBack(a);
        idx.PushBack(c);
        idx.PushBack(b);
    }
    void Quad(u32 a, u32 b, u32 c, u32 d) // a b c d counter-clockwise from outside
    {
        Triangle(a, b, c);
        Triangle(a, c, d);
    }
};

void BuildBox(Builder& b)
{
    // One quad per face: its normal and "up" as seen by a viewer outside; right = up x forward (left-handed),
    // with forward = -normal.
    struct Face
    {
        Vec3 n, up;
    };
    const Face faces[6] = {
        {Vec3(1, 0, 0), Vec3(0, 1, 0)}, {Vec3(-1, 0, 0), Vec3(0, 1, 0)}, {Vec3(0, 1, 0), Vec3(0, 0, 1)},
        {Vec3(0, -1, 0), Vec3(0, 0, -1)}, {Vec3(0, 0, 1), Vec3(0, 1, 0)}, {Vec3(0, 0, -1), Vec3(0, 1, 0)},
    };
    for (const Face& face : faces) {
        struct
        {
            Vec3 n, right, up;
        } f{face.n, Cross(face.up, -face.n), face.up};
        const Vec3 c = f.n * 0.5f;
        const u32 v0 = b.Vertex(c - f.right * 0.5f - f.up * 0.5f, f.n, Vec2(0, 1));
        const u32 v1 = b.Vertex(c + f.right * 0.5f - f.up * 0.5f, f.n, Vec2(1, 1));
        const u32 v2 = b.Vertex(c + f.right * 0.5f + f.up * 0.5f, f.n, Vec2(1, 0));
        const u32 v3 = b.Vertex(c - f.right * 0.5f + f.up * 0.5f, f.n, Vec2(0, 0));
        b.Quad(v0, v1, v2, v3);
    }
}

void BuildSphere(Builder& b, u32 segments = 48, u32 rings = 24)
{
    const f32 r = 0.5f;
    for (u32 y = 0; y <= rings; ++y) {
        const f32 v = f32(y) / f32(rings);
        const f32 theta = v * Pi; // from +Y down
        for (u32 x = 0; x <= segments; ++x) {
            const f32 u = f32(x) / f32(segments);
            const f32 phi = u * TwoPi;
            // Left-handed: seen from outside, u grows to the viewer's right.
            const Vec3 n(-Sin(theta) * Sin(phi), Cos(theta), Sin(theta) * Cos(phi));
            b.Vertex(n * r, n, Vec2(u, v));
        }
    }
    const u32 stride = segments + 1;
    for (u32 y = 0; y < rings; ++y)
        for (u32 x = 0; x < segments; ++x) {
            const u32 a = y * stride + x, c = (y + 1) * stride + x;
            // Seen from outside, u grows to the right (around +Y) and v grows downwards.
            b.Quad(a, c, c + 1, a + 1);
        }
}

void BuildPlane(Builder& b)
{
    const f32 h = 5.0f;
    const Vec3 n = Vec3::Up();
    const u32 v0 = b.Vertex(Vec3(-h, 0, -h), n, Vec2(0, 10));
    const u32 v1 = b.Vertex(Vec3(h, 0, -h), n, Vec2(10, 10));
    const u32 v2 = b.Vertex(Vec3(h, 0, h), n, Vec2(10, 0));
    const u32 v3 = b.Vertex(Vec3(-h, 0, h), n, Vec2(0, 0));
    b.Quad(v0, v1, v2, v3);
}

void BuildCylinder(Builder& b, u32 segments = 40)
{
    const f32 r = 0.5f, h = 0.5f;
    // Side.
    for (u32 x = 0; x <= segments; ++x) {
        const f32 u = f32(x) / f32(segments);
        const f32 phi = u * TwoPi;
        const Vec3 n(-Sin(phi), 0, Cos(phi));
        b.Vertex(n * r + Vec3(0, h, 0), n, Vec2(u, 0));
        b.Vertex(n * r - Vec3(0, h, 0), n, Vec2(u, 1));
    }
    for (u32 x = 0; x < segments; ++x) {
        const u32 top = x * 2, bottom = top + 1;
        b.Quad(top, bottom, bottom + 2, top + 2);
    }
    // Caps (fans around a center vertex).
    for (i32 side = -1; side <= 1; side += 2) {
        const Vec3 n(0, f32(side), 0);
        const u32 center = b.Vertex(Vec3(0, h * f32(side), 0), n, Vec2(0.5f, 0.5f));
        const u32 first = u32(b.mesh.vertices.Size());
        for (u32 x = 0; x <= segments; ++x) {
            const f32 phi = TwoPi * f32(x) / f32(segments);
            const Vec3 d(-Sin(phi), 0, Cos(phi));
            b.Vertex(d * r + n * h, n, Vec2(0.5f + d.x * 0.5f, 0.5f - d.z * 0.5f));
        }
        for (u32 x = 0; x < segments; ++x) {
            if (side > 0)
                b.Triangle(center, first + x, first + x + 1);
            else
                b.Triangle(center, first + x + 1, first + x);
        }
    }
}

} // namespace

namespace builtin {

asset::AssetId Mesh(BuiltinShape shape) { return Guid::FromName(BuiltinNamespace, ShapeNames[u32(shape)]); }

StringView ShapeName(BuiltinShape shape) { return ShapeNames[u32(shape)]; }

bool IsBuiltin(const asset::AssetId& id)
{
    for (u32 i = 0; i < u32(BuiltinShape::Count); ++i)
        if (Mesh(BuiltinShape(i)) == id)
            return true;
    return false;
}

} // namespace builtin

asset::MeshData GenerateShapeMesh(BuiltinShape shape)
{
    Builder b;
    b.mesh.name = String(ShapeNames[u32(shape)]);
    b.mesh.lods.AddDefaulted();
    switch (shape) {
    case BuiltinShape::Cube: BuildBox(b); break;
    case BuiltinShape::Sphere: BuildSphere(b); break;
    case BuiltinShape::Plane: BuildPlane(b); break;
    case BuiltinShape::Cylinder: BuildCylinder(b); break;
    case BuiltinShape::Count: break;
    }
    asset::MeshSection section;
    section.indexCount = u32(b.mesh.lods[0].indices.Size());
    b.mesh.lods[0].sections.PushBack(section);
    b.mesh.attributes = asset::MeshAttributes::Normals | asset::MeshAttributes::UV0 | asset::MeshAttributes::UV1;
    asset::mesh::GenerateTangents(b.mesh);
    b.mesh.attributes |= asset::MeshAttributes::Tangents;
    b.mesh.ComputeBounds();
    return move(b.mesh);
}

void RegisterBuiltinAssets(asset::AssetManager& assets)
{
    for (u32 i = 0; i < u32(BuiltinShape::Count); ++i) {
        asset::MeshData data = GenerateShapeMesh(BuiltinShape(i));
        asset::CookedMesh cooked = asset::CookMesh(data, false);
        cooked.name = String(ShapeNames[i]);
        assets.AddMemoryMesh(builtin::Mesh(BuiltinShape(i)), move(cooked));
    }
}

} // namespace zx
