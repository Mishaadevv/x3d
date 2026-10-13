#include "AabbTree.h"

namespace zx::physics {

i32 AabbTree::Allocate()
{
    if (m_free != Null) {
        const i32 n = m_free;
        m_free = m_nodes[usize(n)].parent;
        m_nodes[usize(n)] = Node();
        return n;
    }
    m_nodes.PushBack(Node());
    return i32(m_nodes.Size() - 1);
}

void AabbTree::Free(i32 node)
{
    m_nodes[usize(node)].height = -1;
    m_nodes[usize(node)].parent = m_free;
    m_free = node;
}

i32 AabbTree::Insert(const AABB& fatBox, u32 userData)
{
    const i32 leaf = Allocate();
    Node& n = m_nodes[usize(leaf)];
    n.box = fatBox;
    n.userData = userData;
    n.height = 0;
    InsertLeaf(leaf);
    return leaf;
}

void AabbTree::Remove(i32 proxy)
{
    RemoveLeaf(proxy);
    Free(proxy);
}

bool AabbTree::Move(i32 proxy, const AABB& tightBox, const AABB& fatBox)
{
    if (m_nodes[usize(proxy)].box.Contains(tightBox))
        return false;
    RemoveLeaf(proxy);
    m_nodes[usize(proxy)].box = fatBox;
    InsertLeaf(proxy);
    return true;
}

void AabbTree::InsertLeaf(i32 leaf)
{
    if (m_root == Null) {
        m_root = leaf;
        m_nodes[usize(leaf)].parent = Null;
        return;
    }
    // Sibling: descend by the cheapest increase of surface area.
    const AABB leafBox = m_nodes[usize(leaf)].box;
    i32 index = m_root;
    while (!m_nodes[usize(index)].IsLeaf()) {
        const Node& n = m_nodes[usize(index)];
        const f32 area = n.box.SurfaceArea();
        const f32 combined = Union(n.box, leafBox).SurfaceArea();
        const f32 cost = 2.0f * combined;
        const f32 inheritance = 2.0f * (combined - area);
        auto childCost = [&](i32 c) {
            const Node& ch = m_nodes[usize(c)];
            const f32 u = Union(leafBox, ch.box).SurfaceArea();
            return ch.IsLeaf() ? u + inheritance : (u - ch.box.SurfaceArea()) + inheritance;
        };
        const f32 c1 = childCost(n.child1), c2 = childCost(n.child2);
        if (cost < c1 && cost < c2)
            break;
        index = c1 < c2 ? n.child1 : n.child2;
    }
    const i32 sibling = index;
    const i32 oldParent = m_nodes[usize(sibling)].parent;
    const i32 newParent = Allocate();
    {
        Node& p = m_nodes[usize(newParent)];
        p.parent = oldParent;
        p.box = Union(leafBox, m_nodes[usize(sibling)].box);
        p.height = m_nodes[usize(sibling)].height + 1;
        p.child1 = sibling;
        p.child2 = leaf;
    }
    if (oldParent != Null) {
        Node& op = m_nodes[usize(oldParent)];
        (op.child1 == sibling ? op.child1 : op.child2) = newParent;
    } else {
        m_root = newParent;
    }
    m_nodes[usize(sibling)].parent = newParent;
    m_nodes[usize(leaf)].parent = newParent;
    // Refit and rebalance up the tree.
    index = m_nodes[usize(leaf)].parent;
    while (index != Null) {
        index = Balance(index);
        Node& n = m_nodes[usize(index)];
        n.height = 1 + Max(m_nodes[usize(n.child1)].height, m_nodes[usize(n.child2)].height);
        n.box = Union(m_nodes[usize(n.child1)].box, m_nodes[usize(n.child2)].box);
        index = n.parent;
    }
}

void AabbTree::RemoveLeaf(i32 leaf)
{
    if (leaf == m_root) {
        m_root = Null;
        return;
    }
    const i32 parent = m_nodes[usize(leaf)].parent;
    const i32 grand = m_nodes[usize(parent)].parent;
    const i32 sibling = m_nodes[usize(parent)].child1 == leaf ? m_nodes[usize(parent)].child2 : m_nodes[usize(parent)].child1;
    if (grand != Null) {
        Node& g = m_nodes[usize(grand)];
        (g.child1 == parent ? g.child1 : g.child2) = sibling;
        m_nodes[usize(sibling)].parent = grand;
        Free(parent);
        i32 index = grand;
        while (index != Null) {
            index = Balance(index);
            Node& n = m_nodes[usize(index)];
            n.box = Union(m_nodes[usize(n.child1)].box, m_nodes[usize(n.child2)].box);
            n.height = 1 + Max(m_nodes[usize(n.child1)].height, m_nodes[usize(n.child2)].height);
            index = n.parent;
        }
    } else {
        m_root = sibling;
        m_nodes[usize(sibling)].parent = Null;
        Free(parent);
    }
}

// AVL-style rotation (Box2D): if one child of `iA` is two levels taller, promote its taller child.
i32 AabbTree::Balance(i32 iA)
{
    Node& A = m_nodes[usize(iA)];
    if (A.IsLeaf() || A.height < 2)
        return iA;
    const i32 iB = A.child1, iC = A.child2;
    const i32 balance = m_nodes[usize(iC)].height - m_nodes[usize(iB)].height;
    auto rotate = [&](i32 iUp, i32 iOther, bool upIsChild2) -> i32 {
        // iUp (taller child of A) moves up; one of its children replaces it under A.
        Node& U = m_nodes[usize(iUp)];
        const i32 iF = U.child1, iG = U.child2;
        U.child1 = iA;
        U.parent = A.parent;
        A.parent = iUp;
        if (U.parent != Null) {
            Node& p = m_nodes[usize(U.parent)];
            (p.child1 == iA ? p.child1 : p.child2) = iUp;
        } else {
            m_root = iUp;
        }
        Node& F = m_nodes[usize(iF)];
        Node& G = m_nodes[usize(iG)];
        const i32 iKeep = F.height > G.height ? iF : iG, iMove = F.height > G.height ? iG : iF;
        U.child2 = iKeep;
        (upIsChild2 ? A.child2 : A.child1) = iMove;
        m_nodes[usize(iMove)].parent = iA;
        A.box = Union(m_nodes[usize(iOther)].box, m_nodes[usize(iMove)].box);
        U.box = Union(A.box, m_nodes[usize(iKeep)].box);
        A.height = 1 + Max(m_nodes[usize(iOther)].height, m_nodes[usize(iMove)].height);
        U.height = 1 + Max(A.height, m_nodes[usize(iKeep)].height);
        return iUp;
    };
    if (balance > 1)
        return rotate(iC, iB, true);
    if (balance < -1)
        return rotate(iB, iC, false);
    return iA;
}

} // namespace zx::physics
