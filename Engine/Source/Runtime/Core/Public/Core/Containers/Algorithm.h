#pragma once

#include "Core/Containers/Array.h"

#include <bit>

namespace zx {

struct LessOp
{
    template <typename A, typename B>
    constexpr bool operator()(const A& a, const B& b) const
    {
        return a < b;
    }
};

struct GreaterOp
{
    template <typename A, typename B>
    constexpr bool operator()(const A& a, const B& b) const
    {
        return b < a;
    }
};

namespace detail {

template <typename T, typename Less>
void InsertionSort(T* first, T* last, Less& less)
{
    for (T* i = first + 1; i < last; ++i) {
        T tmp(move(*i));
        T* j = i;
        while (j > first && less(tmp, *(j - 1))) {
            *j = move(*(j - 1));
            --j;
        }
        *j = move(tmp);
    }
}

template <typename T, typename Less>
void SiftDown(T* base, usize start, usize count, Less& less)
{
    usize root = start;
    for (;;) {
        usize child = 2 * root + 1;
        if (child >= count)
            break;
        if (child + 1 < count && less(base[child], base[child + 1]))
            ++child;
        if (!less(base[root], base[child]))
            break;
        swap(base[root], base[child]);
        root = child;
    }
}

template <typename T, typename Less>
void HeapSort(T* first, T* last, Less& less)
{
    const usize n = usize(last - first);
    for (usize i = n / 2; i > 0; --i)
        SiftDown(first, i - 1, n, less);
    for (usize end = n; end > 1; --end) {
        swap(first[0], first[end - 1]);
        SiftDown(first, 0, end - 1, less);
    }
}

template <typename T, typename Less>
void IntroSort(T* first, T* last, u32 depth, Less& less)
{
    while (last - first > 16) {
        if (depth == 0) {
            HeapSort(first, last, less);
            return;
        }
        --depth;
        // Median of three -> pivot at first.
        T* mid = first + (last - first) / 2;
        T* back = last - 1;
        if (less(*mid, *first))
            swap(*mid, *first);
        if (less(*back, *mid)) {
            swap(*back, *mid);
            if (less(*mid, *first))
                swap(*mid, *first);
        }
        swap(*first, *mid);
        // Hoare partition around *first.
        T* lo = first + 1;
        T* hi = last - 1;
        for (;;) {
            while (less(*lo, *first))
                ++lo;
            while (less(*first, *hi))
                --hi;
            if (lo >= hi)
                break;
            swap(*lo, *hi);
            ++lo;
            --hi;
        }
        swap(*first, *hi);
        // Recurse into the smaller part, loop on the larger.
        if (hi - first < last - (hi + 1)) {
            IntroSort(first, hi, depth, less);
            first = hi + 1;
        } else {
            IntroSort(hi + 1, last, depth, less);
            last = hi;
        }
    }
    InsertionSort(first, last, less);
}

template <typename T, typename Less>
void MergeSortImpl(T* data, T* tmp, usize n, Less& less)
{
    if (n <= 16) {
        InsertionSort(data, data + n, less);
        return;
    }
    const usize half = n / 2;
    MergeSortImpl(data, tmp, half, less);
    MergeSortImpl(data + half, tmp, n - half, less);
    if (!less(data[half], data[half - 1]))
        return; // already ordered
    for (usize i = 0; i < half; ++i)
        tmp[i] = move(data[i]);
    usize a = 0, b = half, out = 0;
    while (a < half && b < n) {
        if (less(data[b], tmp[a]))
            data[out++] = move(data[b++]);
        else
            data[out++] = move(tmp[a++]);
    }
    while (a < half)
        data[out++] = move(tmp[a++]);
}

} // namespace detail

/// Unstable in-place sort (introsort), O(n log n) worst case.
template <typename T, typename Less = LessOp>
void Sort(Span<T> range, Less less = Less())
{
    if (range.Size() < 2)
        return;
    const u32 depth = 2 * u32(std::bit_width(range.Size()));
    detail::IntroSort(range.begin(), range.end(), depth, less);
}

template <typename T, usize N, typename Less = LessOp>
void Sort(ArrayImpl<T, N>& array, Less less = Less())
{
    Sort(Span<T>(array.Data(), array.Size()), less);
}

/// Stable sort (merge sort, n/2 temporary elements).
template <typename T, typename Less = LessOp>
void StableSort(Span<T> range, Less less = Less())
{
    if (range.Size() < 2)
        return;
    Array<T> tmp;
    tmp.Resize(range.Size() / 2 + 1);
    detail::MergeSortImpl(range.Data(), tmp.Data(), range.Size(), less);
}

template <typename T, usize N, typename Less = LessOp>
void StableSort(ArrayImpl<T, N>& array, Less less = Less())
{
    StableSort(Span<T>(array.Data(), array.Size()), less);
}

/// First index i such that !(range[i] < value).
template <typename T, typename V, typename Less = LessOp>
usize LowerBound(Span<const T> range, const V& value, Less less = Less())
{
    usize lo = 0, hi = range.Size();
    while (lo < hi) {
        const usize mid = lo + (hi - lo) / 2;
        if (less(range[mid], value))
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

/// First index i such that value < range[i].
template <typename T, typename V, typename Less = LessOp>
usize UpperBound(Span<const T> range, const V& value, Less less = Less())
{
    usize lo = 0, hi = range.Size();
    while (lo < hi) {
        const usize mid = lo + (hi - lo) / 2;
        if (less(value, range[mid]))
            hi = mid;
        else
            lo = mid + 1;
    }
    return lo;
}

/// Index of `value` in a sorted range, or NPos.
template <typename T, typename V, typename Less = LessOp>
usize BinarySearch(Span<const T> range, const V& value, Less less = Less())
{
    const usize i = LowerBound(range, value, less);
    return (i < range.Size() && !less(value, range[i])) ? i : NPos;
}

template <typename T>
void Reverse(Span<T> range)
{
    if (range.Size() < 2)
        return;
    for (usize i = 0, j = range.Size() - 1; i < j; ++i, --j)
        swap(range[i], range[j]);
}

template <typename T, typename Pred>
bool AnyOf(Span<T> range, Pred pred)
{
    for (auto& v : range)
        if (pred(v))
            return true;
    return false;
}

template <typename T, typename Pred>
bool AllOf(Span<T> range, Pred pred)
{
    for (auto& v : range)
        if (!pred(v))
            return false;
    return true;
}

template <typename T, typename Pred>
usize CountIf(Span<T> range, Pred pred)
{
    usize n = 0;
    for (auto& v : range)
        if (pred(v))
            ++n;
    return n;
}

} // namespace zx
