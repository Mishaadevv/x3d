#pragma once

#include "Core/Containers/Array.h"

namespace zx {

template <typename Signature, usize InlineSize = 32>
class Function;

/// Type-erased callable with small-buffer optimization (callables up to InlineSize bytes are stored inline).
/// Move-only callables are supported; copying a Function requires a copyable callable (asserts otherwise).
template <typename R, typename... Args, usize InlineSize>
class Function<R(Args...), InlineSize>
{
public:
    Function() = default;
    Function(std::nullptr_t) {}

    template <typename F>
        requires(!std::is_same_v<std::decay_t<F>, Function> && std::is_invocable_r_v<R, F&, Args...>)
    Function(F&& f)
    {
        Assign(forward<F>(f));
    }

    Function(const Function& o)
    {
        if (o.m_ops)
            o.m_ops->copy(this, &o);
    }
    Function(Function&& o) noexcept
    {
        if (o.m_ops) {
            o.m_ops->move(this, &o);
            o.m_ops = nullptr;
        }
    }
    ~Function() { Reset(); }

    Function& operator=(const Function& o)
    {
        if (this != &o) {
            Reset();
            if (o.m_ops)
                o.m_ops->copy(this, &o);
        }
        return *this;
    }
    Function& operator=(Function&& o) noexcept
    {
        if (this != &o) {
            Reset();
            if (o.m_ops) {
                o.m_ops->move(this, &o);
                o.m_ops = nullptr;
            }
        }
        return *this;
    }
    Function& operator=(std::nullptr_t)
    {
        Reset();
        return *this;
    }
    template <typename F>
        requires(!std::is_same_v<std::decay_t<F>, Function> && std::is_invocable_r_v<R, F&, Args...>)
    Function& operator=(F&& f)
    {
        Reset();
        Assign(forward<F>(f));
        return *this;
    }

    R operator()(Args... args) const
    {
        ZX_ASSERT(m_ops, "Calling an empty Function");
        return m_ops->invoke(this, forward<Args>(args)...);
    }

    explicit operator bool() const { return m_ops != nullptr; }
    bool IsBound() const { return m_ops != nullptr; }

    void Reset()
    {
        if (m_ops) {
            m_ops->destroy(this);
            m_ops = nullptr;
        }
    }

private:
    struct Ops
    {
        R (*invoke)(const Function*, Args&&...);
        void (*copy)(Function* dst, const Function* src);
        void (*move)(Function* dst, Function* src); // leaves src destroyed
        void (*destroy)(Function*);
    };

    template <typename F>
    static constexpr bool FitsInline = sizeof(F) <= InlineSize && alignof(F) <= alignof(std::max_align_t) &&
                                       std::is_nothrow_move_constructible_v<F>;

    template <typename F>
    F* Target() const
    {
        if constexpr (FitsInline<F>)
            return std::launder(reinterpret_cast<F*>(const_cast<u8*>(m_storage)));
        else
            return *reinterpret_cast<F* const*>(m_storage);
    }

    template <typename F>
    static const Ops* GetOps()
    {
        static constexpr Ops ops = {
            [](const Function* self, Args&&... args) -> R {
                return (*self->template Target<F>())(forward<Args>(args)...);
            },
            [](Function* dst, const Function* src) {
                if constexpr (std::is_copy_constructible_v<F>) {
                    dst->template Construct<F>(*src->template Target<F>());
                } else {
                    ZX_UNUSED(dst);
                    ZX_UNUSED(src);
                    ZX_FATAL("Function: copying a move-only callable");
                }
            },
            [](Function* dst, Function* src) {
                if constexpr (FitsInline<F>) {
                    F* s = src->template Target<F>();
                    ::new (dst->m_storage) F(move(*s));
                    s->~F();
                } else {
                    *reinterpret_cast<F**>(dst->m_storage) = *reinterpret_cast<F**>(src->m_storage);
                }
                dst->m_ops = src->m_ops;
            },
            [](Function* self) {
                if constexpr (FitsInline<F>)
                    self->template Target<F>()->~F();
                else
                    Delete(self->template Target<F>());
            },
        };
        return &ops;
    }

    template <typename F, typename Arg>
    void Construct(Arg&& f)
    {
        if constexpr (FitsInline<F>)
            ::new (m_storage) F(forward<Arg>(f));
        else
            *reinterpret_cast<F**>(m_storage) = New<F>(forward<Arg>(f));
        m_ops = GetOps<F>();
    }

    template <typename Fn>
    void Assign(Fn&& f)
    {
        using F = std::decay_t<Fn>;
        if constexpr (std::is_pointer_v<F> || std::is_member_pointer_v<F>) {
            if (f == nullptr)
                return;
        }
        Construct<F>(forward<Fn>(f));
    }

    alignas(std::max_align_t) u8 m_storage[InlineSize > sizeof(void*) ? InlineSize : sizeof(void*)];
    const Ops* m_ops = nullptr;
};

/// Handle returned by MulticastDelegate::Add, used to unsubscribe.
struct DelegateHandle
{
    u64 id = 0;
    bool IsValid() const { return id != 0; }
    friend bool operator==(DelegateHandle, DelegateHandle) = default;
};

/// List of callbacks invoked together (events). Safe to Add/Remove from inside Broadcast.
/// Thread-safety: none.
template <typename... Args>
class MulticastDelegate
{
public:
    using Callback = Function<void(Args...)>;

    DelegateHandle Add(Callback cb)
    {
        const DelegateHandle h{++m_nextId};
        m_entries.PushBack(Entry{h.id, move(cb)});
        return h;
    }
    bool Remove(DelegateHandle h)
    {
        for (usize i = 0; i < m_entries.Size(); ++i) {
            if (m_entries[i].id == h.id) {
                if (m_broadcastDepth > 0) {
                    m_entries[i].id = 0; // compacted after broadcast
                    m_pendingCompact = true;
                } else {
                    m_entries.RemoveAt(i);
                }
                return true;
            }
        }
        return false;
    }
    void Clear()
    {
        if (m_broadcastDepth > 0) {
            for (Entry& e : m_entries)
                e.id = 0;
            m_pendingCompact = true;
        } else {
            m_entries.Clear();
        }
    }
    bool IsBound() const { return !m_entries.Empty(); }
    usize Count() const { return m_entries.Size(); }

    void Broadcast(Args... args)
    {
        ++m_broadcastDepth;
        // Callbacks added during broadcast are not invoked in this pass.
        const usize count = m_entries.Size();
        for (usize i = 0; i < count; ++i) {
            if (m_entries[i].id != 0) {
                Callback cb = m_entries[i].callback; // copy: entry array may grow during the call
                cb(args...);
            }
        }
        if (--m_broadcastDepth == 0 && m_pendingCompact) {
            m_entries.RemoveIf([](const Entry& e) { return e.id == 0; });
            m_pendingCompact = false;
        }
    }

private:
    struct Entry
    {
        u64 id;
        Callback callback;
    };
    Array<Entry> m_entries;
    u64 m_nextId = 0;
    u32 m_broadcastDepth = 0;
    bool m_pendingCompact = false;
};

} // namespace zx
