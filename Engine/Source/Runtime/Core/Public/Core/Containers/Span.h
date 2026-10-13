#pragma once

#include "Core/Assert.h"

#include <initializer_list>

namespace zx {

inline constexpr usize NPos = ~usize(0);

/// Non-owning view of a contiguous range.
template <typename T>
class Span
{
public:
    using ValueType = std::remove_cv_t<T>;

    constexpr Span() = default;
    constexpr Span(T* data, usize size) : m_data(data), m_size(size) {}
    constexpr Span(T* begin, T* end) : m_data(begin), m_size(usize(end - begin)) {}
    template <usize N>
    constexpr Span(T (&arr)[N]) : m_data(arr), m_size(N)
    {
    }
    /// Only valid for the duration of the full expression (e.g. as a function argument).
    constexpr Span(std::initializer_list<ValueType> il)
        requires std::is_const_v<T>
        : m_data(il.begin()), m_size(il.size())
    {
    }
    /// Any container with Data()/Size() (Array, String, FixedArray, ...).
    template <typename C>
        requires(!std::is_same_v<std::remove_cvref_t<C>, Span> &&
                 requires(C& c) {
                     { c.Data() } -> std::convertible_to<T*>;
                     { c.Size() } -> std::convertible_to<usize>;
                 })
    constexpr Span(C& c) : m_data(c.Data()), m_size(c.Size())
    {
    }
    template <typename U>
        requires(std::is_convertible_v<U (*)[], T (*)[]>)
    constexpr Span(const Span<U>& o) : m_data(o.Data()), m_size(o.Size())
    {
    }

    constexpr T* Data() const { return m_data; }
    constexpr usize Size() const { return m_size; }
    constexpr usize SizeBytes() const { return m_size * sizeof(T); }
    constexpr bool Empty() const { return m_size == 0; }
    constexpr T* begin() const { return m_data; }
    constexpr T* end() const { return m_data + m_size; }
    constexpr T& operator[](usize i) const
    {
        ZX_CHECK_SLOW(i < m_size, "Span index {} out of range {}", i, m_size);
        return m_data[i];
    }
    constexpr T& Front() const { return m_data[0]; }
    constexpr T& Back() const { return m_data[m_size - 1]; }

    constexpr Span SubSpan(usize offset, usize count = NPos) const
    {
        ZX_CHECK_SLOW(offset <= m_size);
        const usize avail = m_size - offset;
        return Span(m_data + offset, count < avail ? count : avail);
    }
    constexpr Span First(usize n) const { return SubSpan(0, n); }
    constexpr Span Last(usize n) const { return SubSpan(m_size - (n < m_size ? n : m_size)); }

    Span<const u8> AsBytes() const { return Span<const u8>(reinterpret_cast<const u8*>(m_data), SizeBytes()); }

    constexpr usize Find(const ValueType& v) const
    {
        for (usize i = 0; i < m_size; ++i)
            if (m_data[i] == v)
                return i;
        return NPos;
    }
    constexpr bool Contains(const ValueType& v) const { return Find(v) != NPos; }

private:
    T* m_data = nullptr;
    usize m_size = 0;
};

template <typename T>
Span(T*, usize) -> Span<T>;
template <typename T, usize N>
Span(T (&)[N]) -> Span<T>;

using ByteSpan = Span<const u8>;
using MutableByteSpan = Span<u8>;

template <typename T>
ByteSpan AsBytes(const T& v)
    requires std::is_trivially_copyable_v<T>
{
    return ByteSpan(reinterpret_cast<const u8*>(&v), sizeof(T));
}

/// Fixed-size array value type (std::array-like).
template <typename T, usize N>
struct FixedArray
{
    T elements[N > 0 ? N : 1];

    static constexpr usize Size() { return N; }
    constexpr T* Data() { return elements; }
    constexpr const T* Data() const { return elements; }
    constexpr T& operator[](usize i)
    {
        ZX_CHECK_SLOW(i < N);
        return elements[i];
    }
    constexpr const T& operator[](usize i) const
    {
        ZX_CHECK_SLOW(i < N);
        return elements[i];
    }
    constexpr T* begin() { return elements; }
    constexpr T* end() { return elements + N; }
    constexpr const T* begin() const { return elements; }
    constexpr const T* end() const { return elements + N; }
    constexpr void Fill(const T& v)
    {
        for (usize i = 0; i < N; ++i)
            elements[i] = v;
    }
    friend constexpr bool operator==(const FixedArray& a, const FixedArray& b)
    {
        for (usize i = 0; i < N; ++i)
            if (!(a.elements[i] == b.elements[i]))
                return false;
        return true;
    }
};

} // namespace zx
