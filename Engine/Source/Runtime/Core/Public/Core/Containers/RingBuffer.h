#pragma once

#include "Core/Containers/Array.h"

#include <bit>

namespace zx {

/// Growable double-ended queue on a power-of-two ring. Thread-safety: none.
template <typename T>
class RingBuffer
{
public:
    RingBuffer() = default;
    explicit RingBuffer(Allocator* allocator) : m_allocator(allocator) {}
    RingBuffer(const RingBuffer& o) : m_allocator(o.m_allocator)
    {
        Reserve(o.m_size);
        for (usize i = 0; i < o.m_size; ++i)
            PushBack(o[i]);
    }
    RingBuffer(RingBuffer&& o) noexcept
        : m_data(o.m_data), m_capacity(o.m_capacity), m_head(o.m_head), m_size(o.m_size), m_allocator(o.m_allocator)
    {
        o.m_data = nullptr;
        o.m_capacity = o.m_head = o.m_size = 0;
    }
    RingBuffer& operator=(RingBuffer o) noexcept
    {
        swap(m_data, o.m_data);
        swap(m_capacity, o.m_capacity);
        swap(m_head, o.m_head);
        swap(m_size, o.m_size);
        swap(m_allocator, o.m_allocator);
        return *this;
    }
    ~RingBuffer()
    {
        Clear();
        if (m_data)
            detail::FreeTo(m_allocator, m_data, m_capacity * sizeof(T));
    }

    usize Size() const { return m_size; }
    bool Empty() const { return m_size == 0; }
    usize Capacity() const { return m_capacity; }

    T& operator[](usize i)
    {
        ZX_ASSERT(i < m_size);
        return m_data[(m_head + i) & (m_capacity - 1)];
    }
    const T& operator[](usize i) const
    {
        ZX_ASSERT(i < m_size);
        return m_data[(m_head + i) & (m_capacity - 1)];
    }
    T& Front() { return (*this)[0]; }
    T& Back() { return (*this)[m_size - 1]; }
    const T& Front() const { return (*this)[0]; }
    const T& Back() const { return (*this)[m_size - 1]; }

    template <typename... Args>
    T& EmplaceBack(Args&&... args)
    {
        if (m_size == m_capacity)
            Grow();
        T* p = ::new (&m_data[(m_head + m_size) & (m_capacity - 1)]) T(forward<Args>(args)...);
        ++m_size;
        return *p;
    }
    template <typename... Args>
    T& EmplaceFront(Args&&... args)
    {
        if (m_size == m_capacity)
            Grow();
        m_head = (m_head - 1) & (m_capacity - 1);
        ++m_size;
        return *::new (&m_data[m_head]) T(forward<Args>(args)...);
    }
    void PushBack(const T& v) { EmplaceBack(v); }
    void PushBack(T&& v) { EmplaceBack(move(v)); }
    void PushFront(const T& v) { EmplaceFront(v); }
    void PushFront(T&& v) { EmplaceFront(move(v)); }

    T PopFront()
    {
        ZX_ASSERT(m_size > 0, "PopFront on empty RingBuffer");
        T& slot = m_data[m_head];
        T v(move(slot));
        slot.~T();
        m_head = (m_head + 1) & (m_capacity - 1);
        --m_size;
        return v;
    }
    T PopBack()
    {
        ZX_ASSERT(m_size > 0, "PopBack on empty RingBuffer");
        T& slot = m_data[(m_head + m_size - 1) & (m_capacity - 1)];
        T v(move(slot));
        slot.~T();
        --m_size;
        return v;
    }

    void Clear()
    {
        if constexpr (!std::is_trivially_destructible_v<T>) {
            for (usize i = 0; i < m_size; ++i)
                (*this)[i].~T();
        }
        m_head = 0;
        m_size = 0;
    }

    void Reserve(usize n)
    {
        if (n <= m_capacity)
            return;
        usize cap = m_capacity ? m_capacity : 8;
        while (cap < n)
            cap *= 2;
        Reallocate(cap);
    }

private:
    void Grow() { Reallocate(m_capacity ? m_capacity * 2 : 8); }

    void Reallocate(usize newCap)
    {
        T* nd = static_cast<T*>(detail::AllocFrom(m_allocator, newCap * sizeof(T),
                                                   alignof(T) > DefaultAlignment ? alignof(T) : DefaultAlignment));
        for (usize i = 0; i < m_size; ++i) {
            T& src = m_data[(m_head + i) & (m_capacity - 1)];
            ::new (nd + i) T(move(src));
            src.~T();
        }
        if (m_data)
            detail::FreeTo(m_allocator, m_data, m_capacity * sizeof(T));
        m_data = nd;
        m_capacity = newCap;
        m_head = 0;
    }

    T* m_data = nullptr;
    usize m_capacity = 0;
    usize m_head = 0;
    usize m_size = 0;
    Allocator* m_allocator = nullptr;
};

/// Dynamic bit array.
class BitArray
{
public:
    BitArray() = default;
    explicit BitArray(usize count, bool value = false) { Resize(count, value); }

    usize Size() const { return m_count; }
    bool Empty() const { return m_count == 0; }

    void Resize(usize count, bool value = false)
    {
        const usize oldCount = m_count;
        m_words.Resize((count + 63) / 64, value ? ~u64(0) : 0);
        // Fix up the bits of the previously-last partial word.
        if (count > oldCount && (oldCount & 63) != 0) {
            u64& w = m_words[oldCount / 64];
            const u64 maskHigh = ~u64(0) << (oldCount & 63);
            w = value ? (w | maskHigh) : (w & ~maskHigh);
        }
        m_count = count;
        ClearUnusedBits();
    }
    void PushBack(bool v)
    {
        Resize(m_count + 1);
        Set(m_count - 1, v);
    }
    void Clear()
    {
        m_words.Clear();
        m_count = 0;
    }

    bool Get(usize i) const
    {
        ZX_CHECK_SLOW(i < m_count);
        return (m_words[i >> 6] >> (i & 63)) & 1;
    }
    bool operator[](usize i) const { return Get(i); }
    void Set(usize i, bool v = true)
    {
        ZX_CHECK_SLOW(i < m_count);
        const u64 bit = u64(1) << (i & 63);
        if (v)
            m_words[i >> 6] |= bit;
        else
            m_words[i >> 6] &= ~bit;
    }
    void Reset(usize i) { Set(i, false); }
    void SetAll(bool v)
    {
        for (u64& w : m_words)
            w = v ? ~u64(0) : 0;
        ClearUnusedBits();
    }
    usize CountSet() const
    {
        usize n = 0;
        for (u64 w : m_words)
            n += usize(std::popcount(w));
        return n;
    }
    /// First set bit at or after `from`, or NPos.
    usize FindFirstSet(usize from = 0) const
    {
        if (from >= m_count)
            return NPos;
        usize wi = from >> 6;
        u64 w = m_words[wi] & (~u64(0) << (from & 63));
        for (;;) {
            if (w)
                return wi * 64 + usize(std::countr_zero(w));
            if (++wi >= m_words.Size())
                return NPos;
            w = m_words[wi];
        }
    }
    /// First clear bit at or after `from`, or NPos.
    usize FindFirstClear(usize from = 0) const
    {
        if (from >= m_count)
            return NPos;
        usize wi = from >> 6;
        u64 w = ~m_words[wi] & (~u64(0) << (from & 63));
        for (;;) {
            if (w) {
                const usize i = wi * 64 + usize(std::countr_zero(w));
                return i < m_count ? i : NPos;
            }
            if (++wi >= m_words.Size())
                return NPos;
            w = ~m_words[wi];
        }
    }
    const u64* Words() const { return m_words.Data(); }

private:
    void ClearUnusedBits()
    {
        if (m_count & 63)
            m_words.Back() &= (u64(1) << (m_count & 63)) - 1;
    }

    Array<u64> m_words;
    usize m_count = 0;
};

} // namespace zx
