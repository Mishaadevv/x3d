#include "Core/Memory/OffsetAllocator.h"
#include "Core/Assert.h"
#include "Core/Memory/Memory.h"

#include <bit>

namespace zx {

namespace {
ZX_FORCEINLINE u32 Log2Floor(u64 v) { return 63u - u32(std::countl_zero(v)); }
} // namespace

OffsetAllocator::OffsetAllocator(u64 size, u32 maxAllocations)
{
    if (size > 0)
        Reset(size, maxAllocations);
}

OffsetAllocator::~OffsetAllocator() { Destroy(); }

OffsetAllocator::OffsetAllocator(OffsetAllocator&& other) noexcept { *this = move(other); }

OffsetAllocator& OffsetAllocator::operator=(OffsetAllocator&& other) noexcept
{
    if (this != &other) {
        Destroy();
        MemCopy(this, &other, sizeof(OffsetAllocator));
        other.m_nodes = nullptr;
        other.m_unusedNodes = nullptr;
        other.m_size = 0;
        other.m_freeSize = 0;
        other.m_maxNodes = 0;
        other.m_unusedCount = 0;
        other.m_liveAllocations = 0;
    }
    return *this;
}

void OffsetAllocator::Destroy()
{
    zx::Free(m_nodes);
    zx::Free(m_unusedNodes);
    m_nodes = nullptr;
    m_unusedNodes = nullptr;
}

void OffsetAllocator::Reset(u64 size, u32 maxAllocations)
{
    ZX_ASSERT(size > 0 && maxAllocations > 0);
    if (maxAllocations != m_maxNodes || !m_nodes) {
        Destroy();
        // Each allocation can create up to two extra free blocks (alignment front + tail remainder).
        m_maxNodes = maxAllocations;
        const u32 nodeCapacity = maxAllocations * 2 + 1;
        m_nodes = static_cast<Node*>(Malloc(sizeof(Node) * nodeCapacity, 16, MemTag::Core));
        m_unusedNodes = static_cast<u32*>(Malloc(sizeof(u32) * nodeCapacity, 16, MemTag::Core));
    }
    const u32 nodeCapacity = m_maxNodes * 2 + 1;
    m_size = size;
    m_freeSize = 0;
    m_liveAllocations = 0;
    m_flBitmap = 0;
    MemZero(m_slBitmap, sizeof(m_slBitmap));
    for (u32 f = 0; f < FLCount; ++f)
        for (u32 s = 0; s < SLCount; ++s)
            m_binHeads[f][s] = InvalidIndex;
    m_unusedCount = nodeCapacity;
    for (u32 i = 0; i < nodeCapacity; ++i)
        m_unusedNodes[i] = nodeCapacity - 1 - i;

    const u32 root = NewNode();
    m_nodes[root] = Node{0, size, InvalidIndex, InvalidIndex, InvalidIndex, InvalidIndex, false};
    InsertFree(root);
}

void OffsetAllocator::MappingInsert(u64 size, u32& fl, u32& sl)
{
    if (size < SLCount) {
        fl = 0;
        sl = u32(size);
    } else {
        const u32 log2 = Log2Floor(size);
        sl = u32(size >> (log2 - SLLog2)) ^ SLCount;
        fl = log2 - SLLog2 + 1;
    }
}

void OffsetAllocator::MappingSearch(u64 size, u32& fl, u32& sl)
{
    if (size >= SLCount) {
        const u64 round = (u64(1) << (Log2Floor(size) - SLLog2)) - 1;
        size += round;
    }
    MappingInsert(size, fl, sl);
}

u32 OffsetAllocator::NewNode()
{
    if (m_unusedCount == 0)
        return InvalidIndex;
    return m_unusedNodes[--m_unusedCount];
}

void OffsetAllocator::ReleaseNode(u32 index) { m_unusedNodes[m_unusedCount++] = index; }

void OffsetAllocator::InsertFree(u32 index)
{
    Node& n = m_nodes[index];
    u32 fl, sl;
    MappingInsert(n.size, fl, sl);
    n.used = false;
    n.prevFree = InvalidIndex;
    n.nextFree = m_binHeads[fl][sl];
    if (n.nextFree != InvalidIndex)
        m_nodes[n.nextFree].prevFree = index;
    m_binHeads[fl][sl] = index;
    m_flBitmap |= u64(1) << fl;
    m_slBitmap[fl] |= 1u << sl;
    m_freeSize += n.size;
}

void OffsetAllocator::RemoveFree(u32 index)
{
    Node& n = m_nodes[index];
    u32 fl, sl;
    MappingInsert(n.size, fl, sl);
    if (n.prevFree != InvalidIndex)
        m_nodes[n.prevFree].nextFree = n.nextFree;
    else
        m_binHeads[fl][sl] = n.nextFree;
    if (n.nextFree != InvalidIndex)
        m_nodes[n.nextFree].prevFree = n.prevFree;
    if (m_binHeads[fl][sl] == InvalidIndex) {
        m_slBitmap[fl] &= ~(1u << sl);
        if (m_slBitmap[fl] == 0)
            m_flBitmap &= ~(u64(1) << fl);
    }
    m_freeSize -= n.size;
}

u32 OffsetAllocator::FindFree(u64 size) const
{
    u32 fl, sl;
    MappingSearch(size, fl, sl);
    if (fl < FLCount) {
        u32 slMap = m_slBitmap[fl] & (~0u << sl);
        if (slMap == 0) {
            const u64 flMap = (fl + 1 < 64) ? (m_flBitmap & (~u64(0) << (fl + 1))) : 0;
            if (flMap != 0) {
                fl = u32(std::countr_zero(flMap));
                slMap = m_slBitmap[fl];
            }
        }
        if (slMap != 0)
            return m_binHeads[fl][u32(std::countr_zero(slMap))];
    }
    // Good-fit search rounds the size up to the next bin, so a block in the request's own bin that is large
    // enough (e.g. the whole heap) is missed. Fall back to scanning that bin.
    MappingInsert(size, fl, sl);
    if (fl >= FLCount)
        return InvalidIndex;
    for (u32 i = m_binHeads[fl][sl]; i != InvalidIndex; i = m_nodes[i].nextFree)
        if (m_nodes[i].size >= size)
            return i;
    return InvalidIndex;
}

OffsetAllocator::Allocation OffsetAllocator::Allocate(u64 size, u64 alignment)
{
    if (size == 0 || !m_nodes)
        return {};
    if (alignment == 0)
        alignment = 1;
    ZX_ASSERT(IsPowerOfTwo(alignment));
    if (m_liveAllocations >= m_maxNodes || m_unusedCount < 2)
        return {};

    const u64 need = size + (alignment - 1);
    const u32 index = FindFree(need);
    if (index == InvalidIndex)
        return {};
    RemoveFree(index);

    Node* n = &m_nodes[index];
    const u64 aligned = AlignUp(n->offset, alignment);
    const u64 front = aligned - n->offset;
    if (front > 0) {
        // Split front padding into its own free block placed before `index`.
        const u32 f = NewNode();
        n = &m_nodes[index];
        m_nodes[f] = Node{n->offset, front, n->prevPhys, index, InvalidIndex, InvalidIndex, false};
        if (n->prevPhys != InvalidIndex)
            m_nodes[n->prevPhys].nextPhys = f;
        n->prevPhys = f;
        n->offset += front;
        n->size -= front;
        InsertFree(f);
        n = &m_nodes[index];
    }
    const u64 remainder = n->size - size;
    if (remainder > 0) {
        const u32 r = NewNode();
        n = &m_nodes[index];
        m_nodes[r] = Node{n->offset + size, remainder, index, n->nextPhys, InvalidIndex, InvalidIndex, false};
        if (n->nextPhys != InvalidIndex)
            m_nodes[n->nextPhys].prevPhys = r;
        n->nextPhys = r;
        n->size = size;
        InsertFree(r);
        n = &m_nodes[index];
    }
    n->used = true;
    ++m_liveAllocations;
    return Allocation{n->offset, index};
}

void OffsetAllocator::Free(Allocation allocation)
{
    if (!allocation.IsValid())
        return;
    u32 index = allocation.node;
    ZX_ASSERT(m_nodes[index].used, "OffsetAllocator: double free");
    --m_liveAllocations;

    Node& n = m_nodes[index];
    // Merge with previous physical neighbor.
    if (n.prevPhys != InvalidIndex && !m_nodes[n.prevPhys].used) {
        const u32 p = n.prevPhys;
        RemoveFree(p);
        n.offset = m_nodes[p].offset;
        n.size += m_nodes[p].size;
        n.prevPhys = m_nodes[p].prevPhys;
        if (n.prevPhys != InvalidIndex)
            m_nodes[n.prevPhys].nextPhys = index;
        ReleaseNode(p);
    }
    // Merge with next physical neighbor.
    if (n.nextPhys != InvalidIndex && !m_nodes[n.nextPhys].used) {
        const u32 x = n.nextPhys;
        RemoveFree(x);
        n.size += m_nodes[x].size;
        n.nextPhys = m_nodes[x].nextPhys;
        if (n.nextPhys != InvalidIndex)
            m_nodes[n.nextPhys].prevPhys = index;
        ReleaseNode(x);
    }
    InsertFree(index);
}

u64 OffsetAllocator::AllocationSize(Allocation allocation) const
{
    return allocation.IsValid() ? m_nodes[allocation.node].size : 0;
}

OffsetAllocator::Stats OffsetAllocator::GetStats() const
{
    Stats s;
    s.totalSize = m_size;
    s.freeSize = m_freeSize;
    s.allocationCount = m_liveAllocations;
    for (u32 f = 0; f < FLCount; ++f) {
        if (!(m_flBitmap & (u64(1) << f)))
            continue;
        for (u32 sl = 0; sl < SLCount; ++sl) {
            for (u32 i = m_binHeads[f][sl]; i != InvalidIndex; i = m_nodes[i].nextFree) {
                ++s.freeBlockCount;
                if (m_nodes[i].size > s.largestFree)
                    s.largestFree = m_nodes[i].size;
            }
        }
    }
    return s;
}

} // namespace zx
