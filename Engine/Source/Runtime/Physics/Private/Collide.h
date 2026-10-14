#pragma once

// Narrowphase: contact manifolds between two posed shapes. Normals point from shape A to shape B; every point carries
// its position on both surfaces and the separation along the normal (negative = penetration).

#include "ConvexHull.h"
#include "Physics/PhysicsWorld.h"

namespace zx::physics {

/// Static triangle mesh with a bounding volume hierarchy (median split, up to 4 triangles per leaf).
struct TriangleMesh
{
    Array<Vec3> positions;
    Array<u32> indices; // 3 per triangle
    struct Node
    {
        AABB box;
        u32 first = 0; // leaf: first triangle in `order`; inner: left child index (right = left + 1)
        u32 count = 0; // leaf: triangle count; 0 = inner node
    };
    Array<Node> nodes;
    Array<u32> order; // triangle indices in leaf order

    void Build();
    u32 TriangleCount() const { return u32(indices.Size() / 3); }
    void Triangle(u32 t, Vec3& a, Vec3& b, Vec3& c) const
    {
        a = positions[indices[t * 3]];
        b = positions[indices[t * 3 + 1]];
        c = positions[indices[t * 3 + 2]];
    }
    /// Triangles whose bounds overlap `box` (mesh space).
    template <typename Fn>
    void Query(const AABB& box, Fn&& fn) const
    {
        if (nodes.Empty())
            return;
        u32 stack[64];
        u32 top = 0;
        stack[top++] = 0;
        while (top > 0) {
            const Node& n = nodes[stack[--top]];
            if (!n.box.Intersects(box))
                continue;
            if (n.count) {
                for (u32 i = 0; i < n.count; ++i)
                    fn(order[n.first + i]);
            } else if (top + 2 <= 64) {
                stack[top++] = n.first;
                stack[top++] = n.first + 1;
            }
        }
    }
    bool Raycast(Vec3 origin, Vec3 dir, f32 maxT, f32& t, Vec3& normal) const;
};

/// A shape placed in the world.
struct PosedShape
{
    const ShapeDesc* desc = nullptr;
    const TriangleMesh* mesh = nullptr;
    const ConvexHull* hull = nullptr;
    Vec3 position;
    Quat rotation;
};

struct ContactPoint
{
    Vec3 pointA; // on A's surface (world)
    Vec3 pointB; // on B's surface (world)
    f32 separation = 0.0f;
};

struct Manifold
{
    Vec3 normal; // A -> B
    ContactPoint points[4];
    u32 count = 0;
    u32 subKey = 0; // distinguishes several manifolds of one shape pair (triangle index for meshes)
};

/// Contacts of a shape pair within `margin` (speculative distance). Mesh shapes produce one manifold per triangle.
void Collide(const PosedShape& a, const PosedShape& b, f32 margin, Array<Manifold>& out);

/// Bounds of a posed shape.
AABB ShapeBounds(const PosedShape& s);
/// Ray against one posed shape (world ray, unit dir).
bool RaycastShape(const PosedShape& s, Vec3 origin, Vec3 dir, f32 maxT, f32& t, Vec3& normal);

} // namespace zx::physics
