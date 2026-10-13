#pragma once

#include "Core/Assert.h"
#include "Core/Memory/Memory.h"

namespace zx {

/// Polymorphic allocator interface used by containers. A null Allocator* means the global heap.
class Allocator
{
public:
    virtual ~Allocator() = default;
    virtual void* Allocate(usize size, usize alignment) = 0;
    virtual void Deallocate(void* ptr, usize size) = 0;
    /// Default implementation: allocate + copy + deallocate.
    virtual void* Reallocate(void* ptr, usize oldSize, usize newSize, usize alignment);
    virtual const char* GetName() const { return "Allocator"; }
};

/// Global heap with a fixed memory tag.
class HeapAllocator final : public Allocator
{
public:
    constexpr explicit HeapAllocator(MemTag tag = MemTag::Count) : m_tag(tag) {}
    void* Allocate(usize size, usize alignment) override { return Malloc(size, alignment, m_tag); }
    void Deallocate(void* ptr, usize) override { Free(ptr); }
    void* Reallocate(void* ptr, usize, usize newSize, usize alignment) override
    {
        return Realloc(ptr, newSize, alignment);
    }
    const char* GetName() const override { return "Heap"; }

private:
    MemTag m_tag;
};

/// The default heap allocator (uses the thread's current memory tag).
Allocator& GetDefaultAllocator();

namespace detail {
ZX_FORCEINLINE void* AllocFrom(Allocator* a, usize size, usize alignment)
{
    return a ? a->Allocate(size, alignment) : Malloc(size, alignment);
}
ZX_FORCEINLINE void FreeTo(Allocator* a, void* p, usize size)
{
    if (a)
        a->Deallocate(p, size);
    else
        Free(p);
}
} // namespace detail

/// Bump allocator over a chain of blocks. Deallocate is a no-op; memory is reclaimed by Reset/RewindTo.
/// Thread-safety: none (use one per thread).
class LinearAllocator final : public Allocator
{
public:
    explicit LinearAllocator(usize blockSize = 64_KB, Allocator* parent = nullptr);
    ~LinearAllocator() override;
    ZX_NON_COPYABLE(LinearAllocator)

    void* Allocate(usize size, usize alignment) override;
    void Deallocate(void*, usize) override {}
    void* Reallocate(void* ptr, usize oldSize, usize newSize, usize alignment) override;
    const char* GetName() const override { return "Linear"; }

    struct Marker
    {
        void* block;
        usize offset;
    };
    Marker GetMarker() const;
    void RewindTo(const Marker& marker);

    /// Frees all allocations; keeps the first block (or all blocks when keepBlocks is true).
    void Reset(bool keepBlocks = true);
    /// Releases all memory to the parent allocator.
    void Release();

    usize BytesUsed() const;
    usize BytesReserved() const { return m_reserved; }

    template <typename T, typename... Args>
    T* New(Args&&... args)
    {
        return ::new (Allocate(sizeof(T), alignof(T))) T(forward<Args>(args)...);
    }
    template <typename T>
    T* NewArray(usize count)
    {
        T* p = static_cast<T*>(Allocate(sizeof(T) * count, alignof(T)));
        for (usize i = 0; i < count; ++i)
            ::new (p + i) T();
        return p;
    }

private:
    struct Block
    {
        Block* next;
        usize capacity; // usable bytes after header
    };
    Block* AllocateBlock(usize minSize);
    static u8* BlockData(Block* b) { return reinterpret_cast<u8*>(b) + AlignUp(sizeof(Block), usize(16)); }

    Allocator* m_parent;
    usize m_blockSize;
    Block* m_first = nullptr;   // first block in chain
    Block* m_current = nullptr; // block currently bumped
    usize m_offset = 0;         // offset in current block
    usize m_reserved = 0;
    void* m_lastAlloc = nullptr; // for in-place Reallocate of the most recent allocation
};

/// RAII rewind of a LinearAllocator to a marker.
class LinearAllocatorScope
{
public:
    explicit LinearAllocatorScope(LinearAllocator& a) : m_alloc(a), m_marker(a.GetMarker()) {}
    ~LinearAllocatorScope() { m_alloc.RewindTo(m_marker); }
    ZX_NON_COPYABLE(LinearAllocatorScope)

private:
    LinearAllocator& m_alloc;
    LinearAllocator::Marker m_marker;
};

/// Fixed-size block pool with an intrusive free list. Thread-safety: none.
class PoolAllocator final : public Allocator
{
public:
    PoolAllocator(usize elementSize, usize elementAlignment, usize elementsPerChunk = 256, Allocator* parent = nullptr);
    ~PoolAllocator() override;
    ZX_NON_COPYABLE(PoolAllocator)

    void* Allocate(usize size, usize alignment) override;
    void Deallocate(void* ptr, usize size) override;
    const char* GetName() const override { return "Pool"; }

    void* AllocateElement();
    void FreeElement(void* ptr);
    usize LiveCount() const { return m_live; }
    usize ElementSize() const { return m_elementSize; }

private:
    void AddChunk();

    Allocator* m_parent;
    usize m_elementSize;
    usize m_alignment;
    usize m_perChunk;
    void* m_freeList = nullptr;
    void* m_chunks = nullptr; // singly-linked list of chunks (first pointer-sized word)
    usize m_live = 0;
};

/// Typed object pool on top of PoolAllocator.
template <typename T>
class ObjectPool
{
public:
    explicit ObjectPool(usize perChunk = 256, Allocator* parent = nullptr)
        : m_pool(sizeof(T), alignof(T), perChunk, parent)
    {
    }
    template <typename... Args>
    T* Create(Args&&... args)
    {
        return ::new (m_pool.AllocateElement()) T(forward<Args>(args)...);
    }
    void Destroy(T* p)
    {
        if (p) {
            p->~T();
            m_pool.FreeElement(p);
        }
    }
    usize LiveCount() const { return m_pool.LiveCount(); }

private:
    PoolAllocator m_pool;
};

/// Reserves a large virtual range up-front and commits pages on demand. Allocations never move.
/// Thread-safety: none.
class VirtualArena final : public Allocator
{
public:
    explicit VirtualArena(usize reserveSize = 1_GB);
    ~VirtualArena() override;
    ZX_NON_COPYABLE(VirtualArena)

    void* Allocate(usize size, usize alignment) override;
    void Deallocate(void*, usize) override {}
    const char* GetName() const override { return "VirtualArena"; }

    usize GetPosition() const { return m_position; }
    void RewindTo(usize position);
    /// Resets to empty. If decommit is true, committed pages are returned to the OS.
    void Reset(bool decommit = false);

    u8* Base() const { return m_base; }
    usize Committed() const { return m_committed; }
    usize Reserved() const { return m_reserved; }

private:
    u8* m_base = nullptr;
    usize m_reserved = 0;
    usize m_committed = 0;
    usize m_position = 0;
};

} // namespace zx
