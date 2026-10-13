#pragma once

// Bounded lock-free queues.
//   SPSCQueue<T> - single producer / single consumer ring (wait-free).
//   MPMCQueue<T> - multi producer / multi consumer (Dmitry Vyukov's bounded queue).
// Capacity is rounded up to a power of two. TryPush/TryPop never block.

#include "Core/Memory/Memory.h"

#include <atomic>
#include <bit>

namespace zx {

inline constexpr usize CacheLineSize = 64;

template <typename T>
class SPSCQueue
{
public:
    explicit SPSCQueue(usize capacity)
    {
        m_capacity = std::bit_ceil(capacity < 2 ? usize(2) : capacity);
        m_mask = m_capacity - 1;
        m_buffer = static_cast<T*>(Malloc(sizeof(T) * m_capacity, alignof(T) > 16 ? alignof(T) : 16));
    }
    ~SPSCQueue()
    {
        T tmp;
        while (TryPop(tmp)) {
        }
        Free(m_buffer);
    }
    SPSCQueue(const SPSCQueue&) = delete;
    SPSCQueue& operator=(const SPSCQueue&) = delete;

    template <typename U>
    bool TryPush(U&& value)
    {
        const usize head = m_head.load(std::memory_order_relaxed);
        if (head - m_cachedTail >= m_capacity) {
            m_cachedTail = m_tail.load(std::memory_order_acquire);
            if (head - m_cachedTail >= m_capacity)
                return false;
        }
        ::new (&m_buffer[head & m_mask]) T(forward<U>(value));
        m_head.store(head + 1, std::memory_order_release);
        return true;
    }

    bool TryPop(T& out)
    {
        const usize tail = m_tail.load(std::memory_order_relaxed);
        if (tail == m_cachedHead) {
            m_cachedHead = m_head.load(std::memory_order_acquire);
            if (tail == m_cachedHead)
                return false;
        }
        T& slot = m_buffer[tail & m_mask];
        out = move(slot);
        slot.~T();
        m_tail.store(tail + 1, std::memory_order_release);
        return true;
    }

    usize Capacity() const { return m_capacity; }
    usize ApproxSize() const { return m_head.load(std::memory_order_relaxed) - m_tail.load(std::memory_order_relaxed); }

private:
    T* m_buffer;
    usize m_capacity;
    usize m_mask;
    alignas(CacheLineSize) std::atomic<usize> m_head{0}; // written by producer
    usize m_cachedTail = 0;                               // producer's view of tail
    alignas(CacheLineSize) std::atomic<usize> m_tail{0}; // written by consumer
    usize m_cachedHead = 0;                               // consumer's view of head
};

template <typename T>
class MPMCQueue
{
public:
    explicit MPMCQueue(usize capacity)
    {
        m_capacity = std::bit_ceil(capacity < 2 ? usize(2) : capacity);
        m_mask = m_capacity - 1;
        m_cells = static_cast<Cell*>(Malloc(sizeof(Cell) * m_capacity, CacheLineSize));
        for (usize i = 0; i < m_capacity; ++i)
            ::new (&m_cells[i]) Cell(i);
    }
    ~MPMCQueue()
    {
        T tmp;
        while (TryPop(tmp)) {
        }
        for (usize i = 0; i < m_capacity; ++i)
            m_cells[i].~Cell();
        Free(m_cells);
    }
    MPMCQueue(const MPMCQueue&) = delete;
    MPMCQueue& operator=(const MPMCQueue&) = delete;

    template <typename U>
    bool TryPush(U&& value)
    {
        usize pos = m_enqueuePos.load(std::memory_order_relaxed);
        Cell* cell;
        for (;;) {
            cell = &m_cells[pos & m_mask];
            const usize seq = cell->sequence.load(std::memory_order_acquire);
            const isize diff = isize(seq) - isize(pos);
            if (diff == 0) {
                if (m_enqueuePos.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
                    break;
            } else if (diff < 0) {
                return false; // full
            } else {
                pos = m_enqueuePos.load(std::memory_order_relaxed);
            }
        }
        ::new (cell->storage) T(forward<U>(value));
        cell->sequence.store(pos + 1, std::memory_order_release);
        return true;
    }

    bool TryPop(T& out)
    {
        usize pos = m_dequeuePos.load(std::memory_order_relaxed);
        Cell* cell;
        for (;;) {
            cell = &m_cells[pos & m_mask];
            const usize seq = cell->sequence.load(std::memory_order_acquire);
            const isize diff = isize(seq) - isize(pos + 1);
            if (diff == 0) {
                if (m_dequeuePos.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
                    break;
            } else if (diff < 0) {
                return false; // empty
            } else {
                pos = m_dequeuePos.load(std::memory_order_relaxed);
            }
        }
        T* value = std::launder(reinterpret_cast<T*>(cell->storage));
        out = move(*value);
        value->~T();
        cell->sequence.store(pos + m_mask + 1, std::memory_order_release);
        return true;
    }

    usize Capacity() const { return m_capacity; }

private:
    struct Cell
    {
        explicit Cell(usize seq) : sequence(seq) {}
        std::atomic<usize> sequence;
        alignas(T) u8 storage[sizeof(T)];
    };

    Cell* m_cells;
    usize m_capacity;
    usize m_mask;
    alignas(CacheLineSize) std::atomic<usize> m_enqueuePos{0};
    alignas(CacheLineSize) std::atomic<usize> m_dequeuePos{0};
};

} // namespace zx
