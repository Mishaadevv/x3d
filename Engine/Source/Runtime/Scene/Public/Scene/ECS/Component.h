#pragma once

// ZECS basics: Entity handles, component type registry and component masks.
//
// Any default-constructible, move-constructible type can be a component; empty types are tags (no storage).
// Component ids are assigned on first use: ComponentIdOf<Position>().

#include "Core/Containers/Array.h"
#include "Core/Hash/Hash.h"
#include "Core/String/StringView.h"

#include <new>
#include <type_traits>
#include <utility>

namespace zx::ecs {

/// 32-bit index + 32-bit generation. Generation 0 = null entity.
struct Entity
{
    u32 index = 0;
    u32 generation = 0;

    constexpr bool IsNull() const { return generation == 0; }
    constexpr explicit operator bool() const { return generation != 0; }
    constexpr u64 Pack() const { return (u64(generation) << 32) | index; }
    static constexpr Entity Unpack(u64 v) { return Entity{u32(v), u32(v >> 32)}; }
    friend constexpr bool operator==(Entity, Entity) = default;
};

inline constexpr Entity NullEntity{};

using ComponentId = u16;
inline constexpr u32 MaxComponentTypes = 512;
inline constexpr ComponentId InvalidComponentId = 0xFFFF;

/// Set of component types (archetype signature, query filters).
struct ComponentMask
{
    static constexpr u32 WordCount = MaxComponentTypes / 64;
    u64 words[WordCount] = {};

    void Set(ComponentId id) { words[id >> 6] |= 1ull << (id & 63); }
    void Reset(ComponentId id) { words[id >> 6] &= ~(1ull << (id & 63)); }
    bool Test(ComponentId id) const { return (words[id >> 6] >> (id & 63)) & 1; }
    bool ContainsAll(const ComponentMask& o) const
    {
        for (u32 i = 0; i < WordCount; ++i)
            if ((words[i] & o.words[i]) != o.words[i])
                return false;
        return true;
    }
    bool Intersects(const ComponentMask& o) const
    {
        for (u32 i = 0; i < WordCount; ++i)
            if (words[i] & o.words[i])
                return true;
        return false;
    }
    bool Empty() const
    {
        for (u64 w : words)
            if (w)
                return false;
        return true;
    }
    u32 Count() const
    {
        u32 n = 0;
        for (u64 w : words)
            n += u32(std::popcount(w));
        return n;
    }
    /// Calls f(ComponentId) for every set bit in ascending order.
    template <typename F>
    void ForEach(F&& f) const
    {
        for (u32 i = 0; i < WordCount; ++i)
            for (u64 w = words[i]; w; w &= w - 1)
                f(ComponentId(i * 64 + u32(std::countr_zero(w))));
    }
    u64 Hash() const { return HashXX64(words, sizeof(words)); }
    friend bool operator==(const ComponentMask&, const ComponentMask&) = default;
};

struct ComponentInfo
{
    StringView name; // static storage
    u32 size = 0;    // 0 = tag
    u32 alignment = 1;
    bool trivial = true; // trivially copyable + destructible: relocation is memcpy, no destructor
    void (*construct)(void* dst, u32 count) = nullptr;
    void (*destruct)(void* dst, u32 count) = nullptr;
    /// Move-constructs dst[i] from src[i] and destroys src[i].
    void (*relocate)(void* dst, void* src, u32 count) = nullptr;
    void (*copyConstruct)(void* dst, const void* src) = nullptr; // null if not copyable
};

namespace detail {

ComponentId RegisterComponent(const ComponentInfo& info);

template <typename T>
StringView RawTypeName()
{
    // "...RawTypeName<struct ns::Foo>(void)" -> "ns::Foo"
    StringView s(__FUNCSIG__);
    const usize begin = s.Find("RawTypeName<") + 12;
    const usize end = s.RFind(">(void)");
    s = s.SubStr(begin, end - begin);
    for (StringView prefix : {StringView("struct "), StringView("class "), StringView("enum ")})
        if (s.StartsWith(prefix))
            return s.SubStr(prefix.Size());
    return s;
}

template <typename T>
ComponentInfo MakeComponentInfo()
{
    static_assert(std::is_default_constructible_v<T> && std::is_move_constructible_v<T>,
                  "Components must be default- and move-constructible");
    ComponentInfo info;
    info.name = RawTypeName<T>();
    if constexpr (std::is_empty_v<T>) {
        info.size = 0;
        info.alignment = 1;
        return info;
    } else {
        info.size = u32(sizeof(T));
        info.alignment = u32(alignof(T));
        info.trivial = std::is_trivially_copyable_v<T> && std::is_trivially_destructible_v<T>;
        info.construct = [](void* dst, u32 count) {
            for (u32 i = 0; i < count; ++i)
                ::new (static_cast<T*>(dst) + i) T();
        };
        info.destruct = [](void* dst, u32 count) {
            if constexpr (!std::is_trivially_destructible_v<T>) {
                for (u32 i = 0; i < count; ++i)
                    (static_cast<T*>(dst) + i)->~T();
            } else {
                (void)dst;
                (void)count;
            }
        };
        info.relocate = [](void* dst, void* src, u32 count) {
            if constexpr (std::is_trivially_copyable_v<T>) {
                MemCopy(dst, src, usize(count) * sizeof(T));
            } else {
                for (u32 i = 0; i < count; ++i) {
                    T* s = static_cast<T*>(src) + i;
                    ::new (static_cast<T*>(dst) + i) T(std::move(*s));
                    s->~T();
                }
            }
        };
        if constexpr (std::is_copy_constructible_v<T>)
            info.copyConstruct = [](void* dst, const void* src) { ::new (dst) T(*static_cast<const T*>(src)); };
        return info;
    }
}

} // namespace detail

template <typename T>
ComponentId ComponentIdOf()
{
    using U = std::remove_cvref_t<T>;
    static const ComponentId id = detail::RegisterComponent(detail::MakeComponentInfo<U>());
    return id;
}

const ComponentInfo& GetComponentInfo(ComponentId id);
u32 GetComponentTypeCount();
/// Finds a registered component by its type name ("ns::Type"); InvalidComponentId if none.
ComponentId FindComponentByName(StringView name);

template <typename... Ts>
ComponentMask MakeComponentMask()
{
    ComponentMask m;
    (m.Set(ComponentIdOf<Ts>()), ...);
    return m;
}

} // namespace zx::ecs

namespace zx {
template <>
struct Hasher<ecs::Entity>
{
    static u64 Hash(ecs::Entity e) { return HashMix64(e.Pack()); }
};
} // namespace zx
