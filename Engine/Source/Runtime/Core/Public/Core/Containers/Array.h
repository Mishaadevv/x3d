#pragma once

#include "Core/Containers/Span.h"
#include "Core/Memory/Allocator.h"

namespace zx {

namespace detail {

template <typename T, usize N>
struct InlineStorage
{
    alignas(T) u8 bytes[sizeof(T) * N];
    T* Ptr() { return reinterpret_cast<T*>(bytes); }
    const T* Ptr() const { return reinterpret_cast<const T*>(bytes); }
};

template <typename T>
struct InlineStorage<T, 0>
{
    T* Ptr() { return nullptr; }
    const T* Ptr() const { return nullptr; }
};

/// Moves `count` elements from src to (uninitialized) dst and destroys the sources.
template <typename T>
void RelocateElements(T* dst, T* src, usize count)
{
    if constexpr (std::is_trivially_copyable_v<T>) {
        if (count)
            MemCopy(dst, src, count * sizeof(T));
    } else {
        for (usize i = 0; i < count; ++i) {
            ::new (dst + i) T(move(src[i]));
            src[i].~T();
        }
    }
}

template <typename T>
void DestroyElements(T* p, usize count)
{
    if constexpr (!std::is_trivially_destructible_v<T>) {
        for (usize i = 0; i < count; ++i)
            p[i].~T();
    }
}

} // namespace detail

/// Dynamic array with optional inline (small-buffer) capacity.
/// `Array<T>` = no inline storage, `SmallArray<T, N>` = first N elements stored inline.
/// Element addresses are invalidated by any growth.
template <typename T, usize InlineCapacity = 0>
class ArrayImpl
{
public:
    using ValueType = T;

    ArrayImpl() = default;
    explicit ArrayImpl(Allocator* allocator) : m_allocator(allocator) {}
    ArrayImpl(std::initializer_list<T> il, Allocator* allocator = nullptr) : m_allocator(allocator)
    {
        Reserve(il.size());
        for (const T& v : il)
            ::new (m_data + m_size++) T(v);
    }
    explicit ArrayImpl(Span<const T> values, Allocator* allocator = nullptr) : m_allocator(allocator)
    {
        Append(values);
    }
    ArrayImpl(const ArrayImpl& o) : m_allocator(o.m_allocator) { Append(Span<const T>(o.Data(), o.Size())); }
    ArrayImpl(ArrayImpl&& o) noexcept : m_allocator(o.m_allocator) { MoveFrom(move(o)); }
    template <usize M>
    ArrayImpl(const ArrayImpl<T, M>& o) : m_allocator(o.GetAllocator())
    {
        Append(Span<const T>(o.Data(), o.Size()));
    }

    ~ArrayImpl()
    {
        Clear();
        FreeBuffer();
    }

    ArrayImpl& operator=(const ArrayImpl& o)
    {
        if (this != &o) {
            Clear();
            Append(Span<const T>(o.Data(), o.Size()));
        }
        return *this;
    }
    ArrayImpl& operator=(ArrayImpl&& o) noexcept
    {
        if (this != &o) {
            Clear();
            if (m_allocator == o.m_allocator) {
                FreeBuffer();
                MoveFrom(move(o));
            } else {
                // Different allocators: move element-wise.
                Reserve(o.m_size);
                for (usize i = 0; i < o.m_size; ++i)
                    ::new (m_data + i) T(move(o.m_data[i]));
                m_size = o.m_size;
                o.Clear();
            }
        }
        return *this;
    }
    ArrayImpl& operator=(std::initializer_list<T> il)
    {
        Clear();
        Reserve(il.size());
        for (const T& v : il)
            ::new (m_data + m_size++) T(v);
        return *this;
    }

    // --- Access -------------------------------------------------------------------------------------------------------
    usize Size() const { return m_size; }
    usize Capacity() const { return m_capacity; }
    bool Empty() const { return m_size == 0; }
    usize SizeBytes() const { return m_size * sizeof(T); }
    T* Data() { return m_data; }
    const T* Data() const { return m_data; }
    T* begin() { return m_data; }
    T* end() { return m_data + m_size; }
    const T* begin() const { return m_data; }
    const T* end() const { return m_data + m_size; }
    Allocator* GetAllocator() const { return m_allocator; }

    T& operator[](usize i)
    {
        ZX_ASSERT(i < m_size, "Array index {} out of bounds (size {})", i, m_size);
        return m_data[i];
    }
    const T& operator[](usize i) const
    {
        ZX_ASSERT(i < m_size, "Array index {} out of bounds (size {})", i, m_size);
        return m_data[i];
    }
    T& Front() { return (*this)[0]; }
    const T& Front() const { return (*this)[0]; }
    T& Back() { return (*this)[m_size - 1]; }
    const T& Back() const { return (*this)[m_size - 1]; }
    bool IsValidIndex(usize i) const { return i < m_size; }

    Span<T> View() { return Span<T>(m_data, m_size); }
    Span<const T> View() const { return Span<const T>(m_data, m_size); }

    // --- Modification -------------------------------------------------------------------------------------------------
    template <typename... Args>
    T& EmplaceBack(Args&&... args)
    {
        if (m_size == m_capacity)
            Grow(m_size + 1);
        T* p = ::new (m_data + m_size) T(forward<Args>(args)...);
        ++m_size;
        return *p;
    }
    T& PushBack(const T& v)
    {
        if (m_size == m_capacity) {
            // `v` may alias an element of this array; copy before reallocating.
            T tmp(v);
            Grow(m_size + 1);
            return *::new (m_data + m_size++) T(move(tmp));
        }
        return *::new (m_data + m_size++) T(v);
    }
    T& PushBack(T&& v)
    {
        if (m_size == m_capacity) {
            T tmp(move(v));
            Grow(m_size + 1);
            return *::new (m_data + m_size++) T(move(tmp));
        }
        return *::new (m_data + m_size++) T(move(v));
    }
    /// Appends a default-constructed element and returns it.
    T& AddDefaulted() { return EmplaceBack(); }
    /// Adds `v` if not already present. Returns index.
    usize AddUnique(const T& v)
    {
        const usize i = Find(v);
        if (i != NPos)
            return i;
        PushBack(v);
        return m_size - 1;
    }

    void PopBack()
    {
        ZX_ASSERT(m_size > 0, "PopBack on empty array");
        m_data[--m_size].~T();
    }
    T PopBackValue()
    {
        ZX_ASSERT(m_size > 0, "PopBackValue on empty array");
        T v(move(m_data[m_size - 1]));
        m_data[--m_size].~T();
        return v;
    }

    void Append(Span<const T> values)
    {
        if (values.Empty())
            return;
        // Values may alias our storage.
        if (values.Data() >= m_data && values.Data() < m_data + m_size) {
            ArrayImpl<T, 0> copy(values);
            Append(copy.View());
            return;
        }
        Reserve(m_size + values.Size());
        if constexpr (std::is_trivially_copyable_v<T>) {
            MemCopy(m_data + m_size, values.Data(), values.SizeBytes());
        } else {
            for (usize i = 0; i < values.Size(); ++i)
                ::new (m_data + m_size + i) T(values[i]);
        }
        m_size += values.Size();
    }
    void Append(std::initializer_list<T> il) { Append(Span<const T>(il.begin(), il.size())); }

    template <typename... Args>
    T& EmplaceAt(usize index, Args&&... args)
    {
        ZX_ASSERT(index <= m_size, "Insert index {} out of bounds (size {})", index, m_size);
        T tmp(forward<Args>(args)...);
        if (m_size == m_capacity)
            Grow(m_size + 1);
        OpenGap(index, 1);
        ::new (m_data + index) T(move(tmp));
        ++m_size;
        return m_data[index];
    }
    T& Insert(usize index, const T& v) { return EmplaceAt(index, v); }
    T& Insert(usize index, T&& v) { return EmplaceAt(index, move(v)); }
    void Insert(usize index, Span<const T> values)
    {
        ZX_ASSERT(index <= m_size);
        if (values.Empty())
            return;
        ArrayImpl<T, 0> copy(values); // guard against aliasing
        Reserve(m_size + copy.Size());
        OpenGap(index, copy.Size());
        for (usize i = 0; i < copy.Size(); ++i)
            ::new (m_data + index + i) T(move(copy[i]));
        m_size += copy.Size();
    }

    /// Removes `count` elements starting at index, preserving order.
    void RemoveAt(usize index, usize count = 1)
    {
        ZX_ASSERT(index + count <= m_size, "RemoveAt({}, {}) out of bounds (size {})", index, count, m_size);
        if (count == 0)
            return;
        detail::DestroyElements(m_data + index, count);
        const usize tail = m_size - index - count;
        if constexpr (std::is_trivially_copyable_v<T>) {
            if (tail)
                MemMove(m_data + index, m_data + index + count, tail * sizeof(T));
        } else {
            for (usize i = 0; i < tail; ++i) {
                ::new (m_data + index + i) T(move(m_data[index + count + i]));
                m_data[index + count + i].~T();
            }
        }
        m_size -= count;
    }
    /// Removes element at index by swapping with the last one (O(1), order not preserved).
    void RemoveAtSwap(usize index)
    {
        ZX_ASSERT(index < m_size, "RemoveAtSwap({}) out of bounds (size {})", index, m_size);
        if (index != m_size - 1)
            m_data[index] = move(m_data[m_size - 1]);
        m_data[--m_size].~T();
    }
    /// Removes first occurrence. Returns true if removed.
    bool Remove(const T& v)
    {
        const usize i = Find(v);
        if (i == NPos)
            return false;
        RemoveAt(i);
        return true;
    }
    bool RemoveSwap(const T& v)
    {
        const usize i = Find(v);
        if (i == NPos)
            return false;
        RemoveAtSwap(i);
        return true;
    }
    /// Removes all elements matching predicate (order preserved). Returns count removed.
    template <typename Pred>
    usize RemoveIf(Pred pred)
    {
        usize w = 0;
        for (usize r = 0; r < m_size; ++r) {
            if (!pred(m_data[r])) {
                if (w != r)
                    m_data[w] = move(m_data[r]);
                ++w;
            }
        }
        const usize removed = m_size - w;
        detail::DestroyElements(m_data + w, removed);
        m_size = w;
        return removed;
    }

    void Clear()
    {
        detail::DestroyElements(m_data, m_size);
        m_size = 0;
    }
    /// Clears and releases heap memory.
    void Reset()
    {
        Clear();
        FreeBuffer();
    }

    void Reserve(usize capacity)
    {
        if (capacity > m_capacity)
            Reallocate(capacity);
    }
    void Resize(usize size)
    {
        if (size > m_size) {
            Reserve(size);
            for (usize i = m_size; i < size; ++i)
                ::new (m_data + i) T();
        } else {
            detail::DestroyElements(m_data + size, m_size - size);
        }
        m_size = size;
    }
    void Resize(usize size, const T& value)
    {
        if (size > m_size) {
            T tmp(value);
            Reserve(size);
            for (usize i = m_size; i < size; ++i)
                ::new (m_data + i) T(tmp);
        } else {
            detail::DestroyElements(m_data + size, m_size - size);
        }
        m_size = size;
    }
    /// Resize without initializing new elements (trivial types only).
    void ResizeUninitialized(usize size)
        requires std::is_trivially_copyable_v<T>
    {
        Reserve(size);
        m_size = size;
    }
    /// Resize with zero-filled new elements (trivial types only).
    void ResizeZeroed(usize size)
        requires std::is_trivially_copyable_v<T>
    {
        Reserve(size);
        if (size > m_size)
            MemZero(m_data + m_size, (size - m_size) * sizeof(T));
        m_size = size;
    }
    void ShrinkToFit()
    {
        if (m_capacity > m_size && m_capacity > InlineCapacity)
            Reallocate(m_size);
    }

    // --- Search -------------------------------------------------------------------------------------------------------
    template <typename U>
    usize Find(const U& v) const
    {
        for (usize i = 0; i < m_size; ++i)
            if (m_data[i] == v)
                return i;
        return NPos;
    }
    template <typename Pred>
    usize FindIf(Pred pred) const
    {
        for (usize i = 0; i < m_size; ++i)
            if (pred(m_data[i]))
                return i;
        return NPos;
    }
    template <typename U>
    usize FindLast(const U& v) const
    {
        for (usize i = m_size; i > 0; --i)
            if (m_data[i - 1] == v)
                return i - 1;
        return NPos;
    }
    template <typename U>
    bool Contains(const U& v) const
    {
        return Find(v) != NPos;
    }
    template <typename Pred>
    bool ContainsIf(Pred pred) const
    {
        return FindIf(pred) != NPos;
    }

    template <usize M>
    friend bool operator==(const ArrayImpl& a, const ArrayImpl<T, M>& b)
    {
        if (a.Size() != b.Size())
            return false;
        for (usize i = 0; i < a.Size(); ++i)
            if (!(a.Data()[i] == b.Data()[i]))
                return false;
        return true;
    }

private:
    template <typename, usize>
    friend class ArrayImpl;

    bool IsInline() const { return InlineCapacity > 0 && m_data == m_inline.Ptr(); }

    void MoveFrom(ArrayImpl&& o)
    {
        if (o.IsInline()) {
            // Source uses inline storage: relocate elements into our inline buffer.
            m_data = m_inline.Ptr();
            m_capacity = InlineCapacity;
            detail::RelocateElements(m_data, o.m_data, o.m_size);
            m_size = o.m_size;
            o.m_size = 0;
            return;
        }
        m_data = o.m_data;
        m_size = o.m_size;
        m_capacity = o.m_capacity;
        o.m_data = o.m_inline.Ptr();
        o.m_size = 0;
        o.m_capacity = InlineCapacity;
    }

    void Grow(usize minCapacity)
    {
        usize newCap = m_capacity + m_capacity / 2;
        if (newCap < 4)
            newCap = 4;
        if (newCap < minCapacity)
            newCap = minCapacity;
        Reallocate(newCap);
    }

    void Reallocate(usize newCapacity)
    {
        ZX_ASSERT(newCapacity >= m_size);
        T* newData;
        if (newCapacity <= InlineCapacity) {
            if (IsInline())
                return;
            newData = m_inline.Ptr();
            newCapacity = InlineCapacity;
        } else {
            newData = static_cast<T*>(detail::AllocFrom(m_allocator, newCapacity * sizeof(T),
                                                        alignof(T) > DefaultAlignment ? alignof(T) : DefaultAlignment));
        }
        detail::RelocateElements(newData, m_data, m_size);
        FreeBuffer();
        m_data = newData;
        m_capacity = newCapacity;
    }

    void FreeBuffer()
    {
        if (m_data && !IsInline())
            detail::FreeTo(m_allocator, m_data, m_capacity * sizeof(T));
        m_data = m_inline.Ptr();
        m_capacity = InlineCapacity;
    }

    // Shifts [index, size) right by `count` into uninitialized space (capacity must suffice).
    void OpenGap(usize index, usize count)
    {
        const usize tail = m_size - index;
        if constexpr (std::is_trivially_copyable_v<T>) {
            if (tail)
                MemMove(m_data + index + count, m_data + index, tail * sizeof(T));
        } else {
            for (usize i = tail; i > 0; --i) {
                ::new (m_data + index + count + i - 1) T(move(m_data[index + i - 1]));
                m_data[index + i - 1].~T();
            }
        }
    }

    [[no_unique_address]] detail::InlineStorage<T, InlineCapacity> m_inline;
    T* m_data = m_inline.Ptr();
    usize m_size = 0;
    usize m_capacity = InlineCapacity;
    Allocator* m_allocator = nullptr;
};

template <typename T>
using Array = ArrayImpl<T, 0>;

template <typename T, usize N>
using SmallArray = ArrayImpl<T, N>;

} // namespace zx
