#pragma once

#include "Core/Types.h"

namespace zx {

/// TLSF (two-level segregated fit) allocator over an abstract range [0, size).
/// It never touches the managed memory - bookkeeping lives in external node arrays - so it is used for
/// sub-allocating GPU heaps, descriptor ranges, buffer arenas etc. O(1) allocate and free.
/// Thread-safety: none.
class OffsetAllocator
{
public:
    static constexpr u64 InvalidOffset = U64Max;

    struct Allocation
    {
        u64 offset = InvalidOffset;
        u32 node = InvalidIndex;
        bool IsValid() const { return node != InvalidIndex; }
    };

    struct Stats
    {
        u64 totalSize = 0;
        u64 freeSize = 0;
        u64 largestFree = 0;
        u32 allocationCount = 0;
        u32 freeBlockCount = 0;
    };

    explicit OffsetAllocator(u64 size = 0, u32 maxAllocations = 64 * 1024);
    ~OffsetAllocator();
    OffsetAllocator(OffsetAllocator&& other) noexcept;
    OffsetAllocator& operator=(OffsetAllocator&& other) noexcept;
    OffsetAllocator(const OffsetAllocator&) = delete;
    OffsetAllocator& operator=(const OffsetAllocator&) = delete;

    /// Re-initializes to a single free block of `size` (all previous allocations are invalidated).
    void Reset(u64 size, u32 maxAllocations);
    void Reset() { Reset(m_size, m_maxNodes); }

    /// Returns an invalid Allocation when there is no space (or node capacity is exhausted).
    Allocation Allocate(u64 size, u64 alignment = 1);
    void Free(Allocation allocation);

    u64 AllocationSize(Allocation allocation) const;
    u64 TotalSize() const { return m_size; }
    u64 FreeSize() const { return m_freeSize; }
    Stats GetStats() const;

private:
    static constexpr u32 SLLog2 = 4;
    static constexpr u32 SLCount = 1u << SLLog2;
    static constexpr u32 FLCount = 64;

    struct Node
    {
        u64 offset;
        u64 size;
        u32 prevPhys;
        u32 nextPhys;
        u32 prevFree;
        u32 nextFree;
        bool used;
    };

    static void MappingInsert(u64 size, u32& fl, u32& sl);
    static void MappingSearch(u64 size, u32& fl, u32& sl);

    u32 NewNode();
    void ReleaseNode(u32 index);
    void InsertFree(u32 index);
    void RemoveFree(u32 index);
    u32 FindFree(u64 size) const;
    void Destroy();

    u64 m_size = 0;
    u64 m_freeSize = 0;
    u32 m_maxNodes = 0;
    u32 m_liveAllocations = 0;
    Node* m_nodes = nullptr;
    u32* m_unusedNodes = nullptr; // stack of free node indices
    u32 m_unusedCount = 0;
    u64 m_flBitmap = 0;
    u32 m_slBitmap[FLCount] = {};
    u32 m_binHeads[FLCount][SLCount];
};

} // namespace zx
