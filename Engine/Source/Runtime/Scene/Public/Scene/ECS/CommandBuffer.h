#pragma once

// Deferred structural changes, recorded during iteration (or on worker threads) and applied with Playback().
//
//   CommandBuffer cmd;
//   world.Query<const Health>().Each([&](Entity e, const Health& h) { if (h.value <= 0) cmd.Destroy(e); });
//   Entity bullet = cmd.Create();            // placeholder, valid only inside this buffer
//   cmd.Add(bullet, Velocity{...});
//   cmd.Playback(world);
//
// A CommandBuffer is not thread-safe: use one per thread / system.

#include "Scene/ECS/Component.h"

namespace zx::ecs {

class World;

class CommandBuffer
{
public:
    CommandBuffer() = default;
    ~CommandBuffer();
    ZX_NON_COPYABLE(CommandBuffer)

    /// Returns a placeholder entity, resolved to a real one on playback.
    Entity Create();
    void Destroy(Entity e);
    /// Adds or overwrites a component.
    template <typename T>
    void Add(Entity e, T value = {})
    {
        using U = std::remove_cvref_t<T>;
        void* storage = nullptr;
        if constexpr (!std::is_empty_v<U>)
            storage = ::new (AllocateValue(sizeof(U), alignof(U))) U(std::move(value));
        Push(Op::Set, e, ComponentIdOf<U>(), storage);
    }
    template <typename T>
    void Remove(Entity e)
    {
        Push(Op::Remove, e, ComponentIdOf<T>(), nullptr);
    }

    /// Applies all commands in order and clears the buffer. Commands on dead entities are skipped.
    void Playback(World& world);
    void Clear();
    bool Empty() const { return m_commands.Empty(); }
    u32 CommandCount() const { return u32(m_commands.Size()); }

    /// Placeholder entities created by Create() carry this generation.
    static constexpr u32 PendingGeneration = 0xFFFFFFFFu;
    static bool IsPending(Entity e) { return e.generation == PendingGeneration; }

private:
    enum class Op : u8
    {
        Create,
        Destroy,
        Set,
        Remove
    };
    struct Command
    {
        Op op;
        ComponentId component;
        Entity entity;
        void* value;
    };
    struct Page
    {
        u8* memory;
        usize size;
        usize used;
    };
    void Push(Op op, Entity e, ComponentId id, void* value) { m_commands.PushBack(Command{op, id, e, value}); }
    void* AllocateValue(usize size, usize alignment);
    void DestroyValues();

    Array<Command> m_commands;
    Array<Page> m_pages;
    u32 m_pendingCount = 0;
};

} // namespace zx::ecs
