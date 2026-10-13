#pragma once

// Dynamic AABB tree (broadphase): leaves hold "fat" boxes (inflated by a margin and the predicted motion) so most moves
// don't touch the tree; insertion picks the sibling by the surface-area heuristic and rebalances with rotations.

#include "Core/Containers/Array.h"
#include "Core/Math/Geometry.h"

namespace zx::physics {

class AabbTree
{
public:
    static constexpr i32 Null = -1;

    i32 Insert(const AABB& fatBox, u32 userData);
    void Remove(i32 proxy);
    /// Moves a proxy; returns false (no change) while `tightBox` still fits in the stored fat box.
    bool Move(i32 proxy, const AABB& tightBox, const AABB& fatBox);
    const AABB& FatBox(i32 proxy) const { return m_nodes[usize(proxy)].box; }
    u32 UserData(i32 proxy) const { return m_nodes[usize(proxy)].userData; }

    /// Calls fn(userData) for leaves overlapping `box`; fn returns false to stop.
    template <typename Fn>
    void Query(const AABB& box, Fn&& fn) const
    {
        if (m_root == Null)
            return;
        i32 stack[256];
        i32 top = 0;
        stack[top++] = m_root;
        while (top > 0) {
            const Node& n = m_nodes[usize(stack[--top])];
            if (!n.box.Intersects(box))
                continue;
            if (n.IsLeaf()) {
                if (!fn(n.userData))
                    return;
            } else if (top + 2 <= 256) {
                stack[top++] = n.child1;
                stack[top++] = n.child2;
            }
        }
    }

    /// Calls fn(userData, maxT) for leaves the ray (origin + t * dir, t in [0, maxT]) may hit; fn returns the new maxT
    /// (shrinks the search; return a negative value to stop).
    template <typename Fn>
    void Raycast(Vec3 origin, Vec3 dir, f32 maxT, Fn&& fn) const
    {
        if (m_root == Null)
            return;
        const Vec3 inv(dir.x != 0.0f ? 1.0f / dir.x : 1e30f, dir.y != 0.0f ? 1.0f / dir.y : 1e30f, dir.z != 0.0f ? 1.0f / dir.z : 1e30f);
        i32 stack[256];
        i32 top = 0;
        stack[top++] = m_root;
        while (top > 0) {
            const Node& n = m_nodes[usize(stack[--top])];
            if (!RayHitsBox(origin, inv, n.box, maxT))
                continue;
            if (n.IsLeaf()) {
                maxT = fn(n.userData, maxT);
                if (maxT < 0.0f)
                    return;
            } else if (top + 2 <= 256) {
                stack[top++] = n.child1;
                stack[top++] = n.child2;
            }
        }
    }

    static bool RayHitsBox(Vec3 o, Vec3 inv, const AABB& b, f32 maxT)
    {
        f32 t0 = 0.0f, t1 = maxT;
        for (u32 a = 0; a < 3; ++a) {
            f32 n = (b.min[a] - o[a]) * inv[a], f = (b.max[a] - o[a]) * inv[a];
            if (n > f)
                swap(n, f);
            t0 = n > t0 ? n : t0;
            t1 = f < t1 ? f : t1;
            if (t0 > t1)
                return false;
        }
        return true;
    }

    i32 Height() const { return m_root == Null ? 0 : m_nodes[usize(m_root)].height; }

private:
    struct Node
    {
        AABB box;
        i32 parent = Null;
        i32 child1 = Null;
        i32 child2 = Null;
        i32 height = -1; // leaf = 0, free = -1
        u32 userData = 0;
        bool IsLeaf() const { return child1 == Null; }
    };

    i32 Allocate();
    void Free(i32 node);
    void InsertLeaf(i32 leaf);
    void RemoveLeaf(i32 leaf);
    i32 Balance(i32 a);

    Array<Node> m_nodes;
    i32 m_root = Null;
    i32 m_free = Null;
};

} // namespace zx::physics
