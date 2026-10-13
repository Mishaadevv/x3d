#pragma once

#include "Core/Containers/Array.h"
#include "Core/Hash/Hash.h"

namespace zx {

/// Generational handle: 32-bit index + 32-bit generation. Generation 0 is never valid.
/// The Tag parameter makes handles of different resources incompatible at compile time.
template <typename Tag>
struct Handle
{
    u32 index = 0;
    u32 generation = 0;

    constexpr bool IsValid() const { return generation != 0; }
    constexpr explicit operator bool() const { return IsValid(); }
    constexpr u64 Pack() const { return (u64(generation) << 32) | index; }
    static constexpr Handle Unpack(u64 v) { return Handle{u32(v), u32(v >> 32)}; }
    friend constexpr bool operator==(Handle, Handle) = default;
};

template <typename Tag>
struct Hasher<Handle<Tag>>
{
    static u64 Hash(Handle<Tag> h) { return HashMix64(h.Pack()); }
};

/// Dense storage addressed by stable generational handles. Insert/Remove/Get are O(1);
/// values are kept contiguous (Remove swaps the last element into the hole), iteration is over the dense array.
template <typename T, typename Tag = T>
class SlotMap
{
public:
    using HandleType = Handle<Tag>;

    template <typename... Args>
    HandleType Emplace(Args&&... args)
    {
        u32 slotIndex;
        if (m_freeHead != InvalidIndex) {
            slotIndex = m_freeHead;
            m_freeHead = m_slots[slotIndex].denseOrNextFree;
        } else {
            slotIndex = u32(m_slots.Size());
            m_slots.PushBack(Slot{InvalidIndex, 1});
        }
        Slot& slot = m_slots[slotIndex];
        slot.denseOrNextFree = u32(m_values.Size());
        m_values.EmplaceBack(forward<Args>(args)...);
        m_denseToSlot.PushBack(slotIndex);
        return HandleType{slotIndex, slot.generation};
    }
    HandleType Insert(const T& v) { return Emplace(v); }
    HandleType Insert(T&& v) { return Emplace(move(v)); }

    bool Contains(HandleType h) const
    {
        return h.index < m_slots.Size() && m_slots[h.index].generation == h.generation && h.generation != 0;
    }

    T* Get(HandleType h)
    {
        if (!Contains(h))
            return nullptr;
        return &m_values[m_slots[h.index].denseOrNextFree];
    }
    const T* Get(HandleType h) const
    {
        if (!Contains(h))
            return nullptr;
        return &m_values[m_slots[h.index].denseOrNextFree];
    }
    T& operator[](HandleType h)
    {
        T* p = Get(h);
        ZX_ASSERT(p, "SlotMap: stale or invalid handle ({}, gen {})", h.index, h.generation);
        return *p;
    }
    const T& operator[](HandleType h) const
    {
        const T* p = Get(h);
        ZX_ASSERT(p, "SlotMap: stale or invalid handle ({}, gen {})", h.index, h.generation);
        return *p;
    }

    bool Remove(HandleType h)
    {
        if (!Contains(h))
            return false;
        Slot& slot = m_slots[h.index];
        const u32 dense = slot.denseOrNextFree;
        const u32 last = u32(m_values.Size() - 1);
        if (dense != last) {
            m_values[dense] = move(m_values[last]);
            m_denseToSlot[dense] = m_denseToSlot[last];
            m_slots[m_denseToSlot[dense]].denseOrNextFree = dense;
        }
        m_values.PopBack();
        m_denseToSlot.PopBack();
        // Bump generation (skip 0 on wrap) and push the slot to the free list.
        slot.generation = slot.generation + 1 == 0 ? 1 : slot.generation + 1;
        slot.denseOrNextFree = m_freeHead;
        m_freeHead = h.index;
        return true;
    }

    void Clear()
    {
        for (u32 dense = 0; dense < m_denseToSlot.Size(); ++dense) {
            const u32 s = m_denseToSlot[dense];
            m_slots[s].generation = m_slots[s].generation + 1 == 0 ? 1 : m_slots[s].generation + 1;
            m_slots[s].denseOrNextFree = m_freeHead;
            m_freeHead = s;
        }
        m_values.Clear();
        m_denseToSlot.Clear();
    }

    usize Size() const { return m_values.Size(); }
    bool Empty() const { return m_values.Empty(); }
    void Reserve(usize n)
    {
        m_values.Reserve(n);
        m_denseToSlot.Reserve(n);
    }

    /// Dense iteration.
    T* begin() { return m_values.begin(); }
    T* end() { return m_values.end(); }
    const T* begin() const { return m_values.begin(); }
    const T* end() const { return m_values.end(); }
    Span<T> Values() { return m_values.View(); }
    Span<const T> Values() const { return m_values.View(); }

    /// Handle of the element at a dense index (for iteration with handles).
    HandleType HandleAt(usize denseIndex) const
    {
        const u32 s = m_denseToSlot[denseIndex];
        return HandleType{s, m_slots[s].generation};
    }

private:
    struct Slot
    {
        u32 denseOrNextFree;
        u32 generation;
    };
    Array<T> m_values;
    Array<u32> m_denseToSlot;
    Array<Slot> m_slots;
    u32 m_freeHead = InvalidIndex;
};

} // namespace zx
