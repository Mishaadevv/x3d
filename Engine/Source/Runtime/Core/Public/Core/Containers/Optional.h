#pragma once

#include "Core/Assert.h"

namespace zx {

struct NullOptT
{
};
inline constexpr NullOptT NullOpt{};

/// Optional value (no exceptions; accessing an empty Optional asserts).
template <typename T>
class Optional
{
public:
    constexpr Optional() {}
    constexpr Optional(NullOptT) {}
    Optional(const T& v) : m_has(true) { ::new (&m_storage) T(v); }
    Optional(T&& v) : m_has(true) { ::new (&m_storage) T(move(v)); }
    Optional(const Optional& o)
    {
        if (o.m_has)
            Emplace(*o);
    }
    Optional(Optional&& o) noexcept
    {
        if (o.m_has)
            Emplace(move(*o));
    }
    ~Optional() { Reset(); }

    Optional& operator=(const Optional& o)
    {
        if (this != &o) {
            Reset();
            if (o.m_has)
                Emplace(*o);
        }
        return *this;
    }
    Optional& operator=(Optional&& o) noexcept
    {
        if (this != &o) {
            Reset();
            if (o.m_has)
                Emplace(move(*o));
        }
        return *this;
    }
    Optional& operator=(NullOptT)
    {
        Reset();
        return *this;
    }

    template <typename... Args>
    T& Emplace(Args&&... args)
    {
        Reset();
        ::new (&m_storage) T(forward<Args>(args)...);
        m_has = true;
        return Get();
    }
    void Reset()
    {
        if (m_has) {
            Get().~T();
            m_has = false;
        }
    }

    bool HasValue() const { return m_has; }
    explicit operator bool() const { return m_has; }

    T& Get()
    {
        ZX_ASSERT(m_has, "Optional is empty");
        return *std::launder(reinterpret_cast<T*>(&m_storage));
    }
    const T& Get() const
    {
        ZX_ASSERT(m_has, "Optional is empty");
        return *std::launder(reinterpret_cast<const T*>(&m_storage));
    }
    T& operator*() { return Get(); }
    const T& operator*() const { return Get(); }
    T* operator->() { return &Get(); }
    const T* operator->() const { return &Get(); }

    T ValueOr(const T& fallback) const { return m_has ? Get() : fallback; }
    T* GetPtrOrNull() { return m_has ? &Get() : nullptr; }
    const T* GetPtrOrNull() const { return m_has ? &Get() : nullptr; }

    friend bool operator==(const Optional& a, const Optional& b)
    {
        if (a.m_has != b.m_has)
            return false;
        return !a.m_has || a.Get() == b.Get();
    }

private:
    alignas(T) u8 m_storage[sizeof(T)];
    bool m_has = false;
};

} // namespace zx
