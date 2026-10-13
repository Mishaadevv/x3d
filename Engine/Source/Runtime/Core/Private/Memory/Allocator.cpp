#include "Core/Memory/Allocator.h"
#include "Core/HAL/PlatformMemory.h"

namespace zx {

void* Allocator::Reallocate(void* ptr, usize oldSize, usize newSize, usize alignment)
{
    void* result = newSize ? Allocate(newSize, alignment) : nullptr;
    if (ptr) {
        if (result)
            MemCopy(result, ptr, oldSize < newSize ? oldSize : newSize);
        Deallocate(ptr, oldSize);
    }
    return result;
}

Allocator& GetDefaultAllocator()
{
    static HeapAllocator s_default;
    return s_default;
}

// ---------------------------------------------------------------------------------------------------------------------
// LinearAllocator
// ---------------------------------------------------------------------------------------------------------------------

LinearAllocator::LinearAllocator(usize blockSize, Allocator* parent) : m_parent(parent), m_blockSize(blockSize) {}

LinearAllocator::~LinearAllocator() { Release(); }

LinearAllocator::Block* LinearAllocator::AllocateBlock(usize minSize)
{
    const usize headerSize = AlignUp(sizeof(Block), usize(16));
    const usize capacity = minSize > m_blockSize ? minSize : m_blockSize;
    Block* b = static_cast<Block*>(detail::AllocFrom(m_parent, headerSize + capacity, 16));
    b->next = nullptr;
    b->capacity = capacity;
    m_reserved += capacity;
    return b;
}

void* LinearAllocator::Allocate(usize size, usize alignment)
{
    if (alignment < 1)
        alignment = 1;
    if (!m_current) {
        m_first = m_current = AllocateBlock(size + alignment);
        m_offset = 0;
    }
    for (;;) {
        u8* data = BlockData(m_current);
        const uptr start = AlignUp(uptr(data + m_offset), uptr(alignment));
        const usize newOffset = usize(start - uptr(data)) + size;
        if (newOffset <= m_current->capacity) {
            m_offset = newOffset;
            m_lastAlloc = reinterpret_cast<void*>(start);
            return m_lastAlloc;
        }
        // Move to the next (reused) block, or append a new one.
        if (m_current->next && m_current->next->capacity >= size + alignment) {
            m_current = m_current->next;
        } else {
            Block* nb = AllocateBlock(size + alignment);
            nb->next = m_current->next;
            m_current->next = nb;
            m_current = nb;
        }
        m_offset = 0;
    }
}

void* LinearAllocator::Reallocate(void* ptr, usize oldSize, usize newSize, usize alignment)
{
    // Grow in place when ptr is the most recent allocation and fits in the current block.
    if (ptr && ptr == m_lastAlloc && m_current) {
        const usize start = usize(static_cast<u8*>(ptr) - BlockData(m_current));
        if (start + newSize <= m_current->capacity) {
            m_offset = start + newSize;
            return ptr;
        }
    }
    void* result = Allocate(newSize, alignment);
    if (ptr)
        MemCopy(result, ptr, oldSize < newSize ? oldSize : newSize);
    return result;
}

LinearAllocator::Marker LinearAllocator::GetMarker() const { return Marker{m_current, m_offset}; }

void LinearAllocator::RewindTo(const Marker& marker)
{
    if (!marker.block) {
        Reset(true);
        return;
    }
    m_current = static_cast<Block*>(marker.block);
    m_offset = marker.offset;
    m_lastAlloc = nullptr;
}

void LinearAllocator::Reset(bool keepBlocks)
{
    if (!keepBlocks && m_first) {
        Block* b = m_first->next;
        while (b) {
            Block* next = b->next;
            m_reserved -= b->capacity;
            detail::FreeTo(m_parent, b, 0);
            b = next;
        }
        m_first->next = nullptr;
    }
    m_current = m_first;
    m_offset = 0;
    m_lastAlloc = nullptr;
}

void LinearAllocator::Release()
{
    Block* b = m_first;
    while (b) {
        Block* next = b->next;
        detail::FreeTo(m_parent, b, 0);
        b = next;
    }
    m_first = m_current = nullptr;
    m_offset = 0;
    m_reserved = 0;
    m_lastAlloc = nullptr;
}

usize LinearAllocator::BytesUsed() const
{
    usize used = 0;
    for (Block* b = m_first; b; b = b->next) {
        if (b == m_current) {
            used += m_offset;
            break;
        }
        used += b->capacity;
    }
    return used;
}

// ---------------------------------------------------------------------------------------------------------------------
// PoolAllocator
// ---------------------------------------------------------------------------------------------------------------------

PoolAllocator::PoolAllocator(usize elementSize, usize elementAlignment, usize elementsPerChunk, Allocator* parent)
    : m_parent(parent)
{
    m_alignment = elementAlignment < alignof(void*) ? alignof(void*) : elementAlignment;
    m_elementSize = AlignUp(elementSize < sizeof(void*) ? sizeof(void*) : elementSize, m_alignment);
    m_perChunk = elementsPerChunk ? elementsPerChunk : 1;
}

PoolAllocator::~PoolAllocator()
{
    ZX_ASSERT(m_live == 0, "PoolAllocator destroyed with {} live elements", m_live);
    void* chunk = m_chunks;
    while (chunk) {
        void* next = *static_cast<void**>(chunk);
        detail::FreeTo(m_parent, chunk, 0);
        chunk = next;
    }
}

void PoolAllocator::AddChunk()
{
    const usize headerSize = AlignUp(sizeof(void*), m_alignment);
    u8* chunk = static_cast<u8*>(detail::AllocFrom(m_parent, headerSize + m_elementSize * m_perChunk,
                                                   m_alignment > 16 ? m_alignment : 16));
    *reinterpret_cast<void**>(chunk) = m_chunks;
    m_chunks = chunk;
    u8* elements = chunk + headerSize;
    // Push in reverse so allocation order follows memory order.
    for (usize i = m_perChunk; i > 0; --i) {
        void* e = elements + (i - 1) * m_elementSize;
        *static_cast<void**>(e) = m_freeList;
        m_freeList = e;
    }
}

void* PoolAllocator::AllocateElement()
{
    if (!m_freeList)
        AddChunk();
    void* e = m_freeList;
    m_freeList = *static_cast<void**>(e);
    ++m_live;
    return e;
}

void PoolAllocator::FreeElement(void* ptr)
{
    if (!ptr)
        return;
    ZX_ASSERT(m_live > 0);
    *static_cast<void**>(ptr) = m_freeList;
    m_freeList = ptr;
    --m_live;
}

void* PoolAllocator::Allocate(usize size, usize alignment)
{
    ZX_ASSERT(size <= m_elementSize && alignment <= m_alignment, "PoolAllocator: request {}B/{} exceeds element {}B/{}",
              size, alignment, m_elementSize, m_alignment);
    return AllocateElement();
}

void PoolAllocator::Deallocate(void* ptr, usize) { FreeElement(ptr); }

// ---------------------------------------------------------------------------------------------------------------------
// VirtualArena
// ---------------------------------------------------------------------------------------------------------------------

VirtualArena::VirtualArena(usize reserveSize)
{
    const usize gran = platform::GetAllocationGranularity();
    m_reserved = AlignUp(reserveSize, gran);
    m_base = static_cast<u8*>(platform::VirtualReserve(m_reserved));
    if (!m_base)
        ZX_FATAL("VirtualArena: failed to reserve {} bytes of address space", m_reserved);
}

VirtualArena::~VirtualArena() { platform::VirtualRelease(m_base); }

void* VirtualArena::Allocate(usize size, usize alignment)
{
    const usize start = AlignUp(m_position, alignment ? alignment : 1);
    const usize end = start + size;
    if (end > m_reserved)
        ZX_FATAL("VirtualArena: out of reserved space ({} > {})", end, m_reserved);
    if (end > m_committed) {
        const usize page = platform::GetPageSize();
        // Commit in 64 KB steps to limit syscalls.
        const usize newCommitted = AlignUp(end, page * 16 > m_reserved ? page : page * 16);
        const usize commitEnd = newCommitted < m_reserved ? newCommitted : m_reserved;
        if (!platform::VirtualCommit(m_base + m_committed, commitEnd - m_committed))
            ZX_FATAL("VirtualArena: failed to commit memory");
        m_committed = commitEnd;
    }
    m_position = end;
    return m_base + start;
}

void VirtualArena::RewindTo(usize position)
{
    ZX_ASSERT(position <= m_position);
    m_position = position;
}

void VirtualArena::Reset(bool decommit)
{
    m_position = 0;
    if (decommit && m_committed > 0) {
        platform::VirtualDecommit(m_base, m_committed);
        m_committed = 0;
    }
}

} // namespace zx
