#pragma once

// Convex hulls (incremental quickhull, coplanar triangles merged into polygon faces, at most MaxVertices vertices) and
// GJK distance between convex point sets.

#include "Core/Containers/Array.h"
#include "Core/Math/Geometry.h"
#include "Core/Math/Matrix.h"

namespace zx::physics {

struct ConvexHull
{
    static constexpr u32 MaxVertices = 32;
    static constexpr u32 MaxFaces = 64;
    static constexpr u32 MaxFaceVertices = 16;
    static constexpr u32 MaxEdges = 96;
    static constexpr u32 MaxDirections = 64;

    struct Face
    {
        Vec3 normal; // outward, unit
        f32 d = 0.0f; // plane: dot(normal, x) = d
        u8 first = 0; // into faceVertices
        u8 count = 0; // polygon corners, counter-clockwise seen from outside
    };

    Array<Vec3> vertices;
    Array<Face> faces;
    Array<u8> faceVertices;
    Array<u8> edges;       // 2 vertex indices per edge
    Array<u8> edgeDir;     // per edge: index into `directions`
    Array<Vec3> directions; // unique edge directions (unit)
    AABB bounds;
    Vec3 centroid;         // center of mass
    f32 volume = 0.0f;
    Mat3 inertia;          // unit density, about the centroid

    /// Hull of `points` (duplicates and interior points ignored). False when they don't span a volume.
    bool Build(Span<const Vec3> points, u32 maxVertices = MaxVertices);
    /// Vertex farthest along `dir`.
    u32 Support(Vec3 dir) const;
};

/// GJK: closest points between convex A (support function) and convex B. Returns the distance (0 when they touch or
/// overlap; then the points are not meaningful).
struct GjkResult
{
    f32 distance = 0.0f;
    Vec3 pointA, pointB;
};
GjkResult Gjk(Span<const Vec3> a, Span<const Vec3> b);

} // namespace zx::physics
