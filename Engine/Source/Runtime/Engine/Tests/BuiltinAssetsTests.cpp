#include "Core/Testing/Test.h"
#include "Engine/BuiltinAssets.h"

using namespace zx;

ZX_TEST(BuiltinAssets, ShapesAreClosedOutwardAndUnitSized)
{
    for (u32 s = 0; s < u32(BuiltinShape::Count); ++s) {
        const BuiltinShape shape = BuiltinShape(s);
        asset::MeshData m = GenerateShapeMesh(shape);
        ZX_EXPECT(m.Validate().Empty());
        const Array<u32>& idx = m.lods[0].indices;
        ZX_EXPECT(idx.Size() % 3 == 0 && idx.Size() > 0);
        // Front faces are clockwise in the left-handed engine: cross(b - a, c - a) points out of the shape.
        u32 inward = 0;
        for (usize t = 0; t + 2 < idx.Size(); t += 3) {
            const Vec3 a = m.vertices[idx[t]].position, b = m.vertices[idx[t + 1]].position, c = m.vertices[idx[t + 2]].position;
            const Vec3 n = Cross(b - a, c - a);
            const Vec3 centroid = (a + b + c) / 3.0f;
            const Vec3 outward = shape == BuiltinShape::Plane ? Vec3::Up() : centroid;
            if (LengthSq(n) > 1e-12f && Dot(n, outward) <= 0.0f)
                ++inward;
            // Vertex normals agree with the face.
            if (LengthSq(n) > 1e-12f && Dot(m.vertices[idx[t]].normal, n) <= 0.0f)
                ++inward;
        }
        ZX_EXPECT_EQ(inward, 0u);
        const Vec3 size = m.bounds.max - m.bounds.min;
        if (shape == BuiltinShape::Plane)
            ZX_EXPECT(Abs(size.x - 10.0f) < 1e-3f && Abs(size.z - 10.0f) < 1e-3f);
        else
            ZX_EXPECT(Abs(size.x - 1.0f) < 1e-3f && Abs(size.y - 1.0f) < 1e-3f && Abs(size.z - 1.0f) < 1e-3f);
        ZX_EXPECT(builtin::IsBuiltin(builtin::Mesh(shape)));
    }
    ZX_EXPECT(!builtin::IsBuiltin(Guid::New()));
}
