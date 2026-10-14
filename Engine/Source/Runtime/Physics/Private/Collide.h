#pragma once

// Narrowphase: contact manifolds between two posed shapes. Normals point from shape A to shape B; every point carries
// its position on both surfaces and the separation along the normal (negative = penetration).

#include "ConvexHull.h"
#include "Physics/PhysicsWorld.h"

namespace zx::physics {

/// Static triangle mesh with a bounding volume hierarchy (median split, up to 4 triangles per leaf).
struct TriangleMesh
{
    bool SolidBelow() const { return false; }
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

/// Regular grid of heights (terrain). Cell (x, z) has corners (x, z) .. (x + 1, z + 1); two triangles per cell.
struct HeightField
{
    bool SolidBelow() const { return true; } // terrain: everything under the surface is ground
    u32 columns = 0, rows = 0; // samples
    f32 spacingX = 1.0f, spacingZ = 1.0f;
    Vec3 origin; // local position of sample (0, 0)
    Array<f32> heights;
    AABB bounds;

    void Build(); // origin and bounds from the heights
    Vec3 Sample(u32 x, u32 z) const { return origin + Vec3(f32(x) * spacingX, heights[z * columns + x], f32(z) * spacingZ); }
    u32 CellsX() const { return columns - 1; }
    void Triangle(u32 t, Vec3& a, Vec3& b, Vec3& c) const
    {
        const u32 cell = t >> 1, x = cell % CellsX(), z = cell / CellsX();
        const Vec3 p00 = Sample(x, z), p11 = Sample(x + 1, z + 1);
        a = p00;
        if (t & 1) {
            b = Sample(x, z + 1);
            c = p11;
        } else {
            b = p11;
            c = Sample(x + 1, z);
        }
    }
    /// Triangles whose cells overlap `box` (field space).
    template <typename Fn>
    void Query(const AABB& box, Fn&& fn) const
    {
        if (columns < 2 || rows < 2 || !box.Intersects(bounds))
            return;
        const i32 x0 = Max(i32(Floor((box.min.x - origin.x) / spacingX)), 0), x1 = Min(i32(Floor((box.max.x - origin.x) / spacingX)), i32(columns) - 2);
        const i32 z0 = Max(i32(Floor((box.min.z - origin.z) / spacingZ)), 0), z1 = Min(i32(Floor((box.max.z - origin.z) / spacingZ)), i32(rows) - 2);
        for (i32 z = z0; z <= z1; ++z)
            for (i32 x = x0; x <= x1; ++x) {
                const f32 h00 = heights[u32(z) * columns + u32(x)], h10 = heights[u32(z) * columns + u32(x) + 1];
                const f32 h01 = heights[u32(z + 1) * columns + u32(x)], h11 = heights[u32(z + 1) * columns + u32(x) + 1];
                const f32 lo = Min(Min(h00, h10), Min(h01, h11)) + origin.y, hi = Max(Max(h00, h10), Max(h01, h11)) + origin.y;
                if (hi < box.min.y || lo > box.max.y)
                    continue;
                const u32 cell = u32(z) * CellsX() + u32(x);
                fn(cell * 2);
                fn(cell * 2 + 1);
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
    const HeightField* heightField = nullptr;
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
