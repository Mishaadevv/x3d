#pragma once

#include "Core/Assert.h"
#include "Core/Memory/Memory.h"

#include <atomic>

namespace zx {

/// Exclusive ownership (allocated with zx::New / MakeUnique).
template <typename T>
class UniquePtr
{
public:
    constexpr UniquePtr() = default;
    constexpr UniquePtr(std::nullptr_t) {}
    explicit UniquePtr(T* p) : m_ptr(p) {}
    UniquePtr(UniquePtr&& o) noexcept : m_ptr(o.Release()) {}
    template <typename U>
        requires std::is_convertible_v<U*, T*>
    UniquePtr(UniquePtr<U>&& o) noexcept : m_ptr(o.Release())
    {
    }
    ~UniquePtr() { Reset(); }

    UniquePtr& operator=(UniquePtr&& o) noexcept
    {
        if (this != &o)
            Reset(o.Release());
        return *this;
    }
    template <typename U>
        requires std::is_convertible_v<U*, T*>
    UniquePtr& operator=(UniquePtr<U>&& o) noexcept
    {
        Reset(o.Release());
        return *this;
    }
    UniquePtr& operator=(std::nullptr_t)
    {
        Reset();
        return *this;
    }
    UniquePtr(const UniquePtr&) = delete;
    UniquePtr& operator=(const UniquePtr&) = delete;

    T* Get() const { return m_ptr; }
    T* operator->() const
    {
        ZX_CHECK_SLOW(m_ptr);
        return m_ptr;
    }
    T& operator*() const
    {
        ZX_CHECK_SLOW(m_ptr);
        return *m_ptr;
    }
    explicit operator bool() const { return m_ptr != nullptr; }

    T* Release()
    {
        T* p = m_ptr;
        m_ptr = nullptr;
        return p;
    }
    void Reset(T* p = nullptr)
    {
        T* old = m_ptr;
        m_ptr = p;
        if (old)
            Delete(old);
    }

    friend bool operator==(const UniquePtr& a, std::nullptr_t) { return a.m_ptr == nullptr; }

private:
    T* m_ptr = nullptr;
};

template <typename T, typename... Args>
UniquePtr<T> MakeUnique(Args&&... args)
{
    return UniquePtr<T>(New<T>(forward<Args>(args)...));
}

/// Intrusive thread-safe reference counting base. Objects start with refcount 0;
/// the first RefPtr takes ownership. Deleted with zx::Delete when the count reaches zero.
class RefCounted
{
public:
    RefCounted() = default;
    RefCounted(const RefCounted&) : m_refCount(0) {}
    RefCounted& operator=(const RefCounted&) { return *this; }

    u32 AddRef() const { return m_refCount.fetch_add(1, std::memory_order_relaxed) + 1; }
    u32 ReleaseRef() const
    {
        const u32 remaining = m_refCount.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (remaining == 0)
            delete this;
        return remaining;
    }
    u32 GetRefCount() const { return m_refCount.load(std::memory_order_relaxed); }

protected:
    virtual ~RefCounted() = default;

private:
    mutable std::atomic<u32> m_refCount{0};
};

template <typename T>
class RefPtr
{
public:
    constexpr RefPtr() = default;
    constexpr RefPtr(std::nullptr_t) {}
    RefPtr(T* p) : m_ptr(p)
    {
        if (m_ptr)
            m_ptr->AddRef();
    }
    RefPtr(const RefPtr& o) : RefPtr(o.m_ptr) {}
    RefPtr(RefPtr&& o) noexcept : m_ptr(o.m_ptr) { o.m_ptr = nullptr; }
    template <typename U>
        requires std::is_convertible_v<U*, T*>
    RefPtr(const RefPtr<U>& o) : RefPtr(o.Get())
    {
    }
    template <typename U>
        requires std::is_convertible_v<U*, T*>
    RefPtr(RefPtr<U>&& o) noexcept : m_ptr(o.Detach())
    {
    }
    ~RefPtr()
    {
        if (m_ptr)
            m_ptr->ReleaseRef();
    }

    RefPtr& operator=(const RefPtr& o)
    {
        RefPtr(o).Swap(*this);
        return *this;
    }
    RefPtr& operator=(RefPtr&& o) noexcept
    {
        RefPtr(move(o)).Swap(*this);
        return *this;
    }
    RefPtr& operator=(T* p)
    {
        RefPtr(p).Swap(*this);
        return *this;
    }
    RefPtr& operator=(std::nullptr_t)
    {
        RefPtr().Swap(*this);
        return *this;
    }

    void Swap(RefPtr& o) noexcept
    {
        T* t = m_ptr;
        m_ptr = o.m_ptr;
        o.m_ptr = t;
    }

    T* Get() const { return m_ptr; }
    T* operator->() const
    {
        ZX_CHECK_SLOW(m_ptr);
        return m_ptr;
    }
    T& operator*() const { return *m_ptr; }
    explicit operator bool() const { return m_ptr != nullptr; }

    /// Gives up ownership without releasing the reference.
    T* Detach()
    {
        T* p = m_ptr;
        m_ptr = nullptr;
        return p;
    }

    friend bool operator==(const RefPtr& a, const RefPtr& b) { return a.m_ptr == b.m_ptr; }
    friend bool operator==(const RefPtr& a, std::nullptr_t) { return a.m_ptr == nullptr; }

private:
    T* m_ptr = nullptr;
};

template <typename T, typename... Args>
RefPtr<T> MakeRef(Args&&... args)
{
    return RefPtr<T>(New<T>(forward<Args>(args)...));
}

} // namespace zx
