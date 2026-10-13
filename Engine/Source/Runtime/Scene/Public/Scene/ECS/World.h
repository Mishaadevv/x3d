#pragma once

// ZECS world: archetype storage (16 KB chunks, SoA columns), entities, observers, singletons.
//
//   World world;
//   Entity e = world.Create(Position{1, 2, 3}, Velocity{});
//   world.Add<Health>(e, {100});
//   world.Query<Position, const Velocity>().Each([](Position& p, const Velocity& v) { p.value += v.value; });
//
// Structural changes (create/destroy/add/remove) are not allowed while a query iterates - record them in a
// CommandBuffer instead. Get<T>/Query with a non-const T mark the chunk column as changed (see Changed<T> filters).

#include "Core/Containers/Function.h"
#include "Core/Containers/HashMap.h"
#include "Core/Threading/Mutex.h"
#include "Scene/ECS/Component.h"

#include <atomic>

namespace zx::ecs {

class World;
template <typename... Ts>
class Query;

inline constexpr u32 ChunkSize = 16 * 1024;

class Archetype
{
public:
    const ComponentMask& Mask() const { return m_mask; }
    /// All component ids (including tags), ascending.
    Span<const ComponentId> Components() const { return m_components.View(); }
    /// Components with storage (no tags), ascending.
    Span<const ComponentId> Columns() const { return m_columnIds.View(); }
    u32 ChunkCapacity() const { return m_capacity; }
    u32 ChunkCount() const { return u32(m_chunks.Size()); }
    u32 EntityCount() const { return m_entityCount; }
    u32 ChunkEntityCount(u32 chunk) const { return m_chunks[chunk].count; }

    /// Column index of a component with storage, or -1.
    i32 ColumnIndex(ComponentId id) const
    {
        for (u32 i = 0; i < m_columnIds.Size(); ++i)
            if (m_columnIds[i] == id)
                return i32(i);
        return -1;
    }
    Entity* Entities(u32 chunk) const { return reinterpret_cast<Entity*>(m_chunks[chunk].memory); }
    void* ColumnData(u32 chunk, u32 column) const { return m_chunks[chunk].memory + m_columnOffsets[column]; }
    void* ComponentPtr(u32 chunk, u32 column, u32 row) const
    {
        return static_cast<u8*>(ColumnData(chunk, column)) + usize(row) * m_columnSizes[column];
    }
    u32 ColumnVersion(u32 chunk, u32 column) const { return m_chunks[chunk].versions[column]; }
    void MarkColumnChanged(u32 chunk, u32 column, u32 version) { m_chunks[chunk].versions[column] = version; }

private:
    friend class World;
    struct Chunk
    {
        u8* memory = nullptr;
        u32* versions = nullptr; // one per column
        u32 count = 0;
    };

    ComponentMask m_mask;
    Array<ComponentId> m_components;
    Array<ComponentId> m_columnIds;
    Array<u32> m_columnOffsets;
    Array<u32> m_columnSizes;
    u32 m_capacity = 0;
    u32 m_chunkBytes = ChunkSize;
    u32 m_entityCount = 0;
    Array<Chunk> m_chunks;
    HashMap<ComponentId, Archetype*> m_addEdges;
    HashMap<ComponentId, Archetype*> m_removeEdges;
};

using ObserverFunction = Function<void(World&, Entity, void* component), 32>;

class World
{
public:
    World();
    ~World();
    ZX_NON_COPYABLE(World)

    // --- Entities ---------------------------------------------------------------------------------------------
    Entity Create();
    template <typename... Ts>
    Entity Create(Ts&&... components)
    {
        const Entity e = CreateInArchetype(GetArchetype(MakeComponentMask<std::remove_cvref_t<Ts>...>()), false);
        (InitCreated<std::remove_cvref_t<Ts>>(e, std::forward<Ts>(components)), ...);
        (FireHook(m_onAdd, ComponentIdOf<Ts>(), e), ...);
        return e;
    }
    void Destroy(Entity e);
    bool IsAlive(Entity e) const
    {
        return e.index < m_records.Size() && m_records[e.index].generation == e.generation && e.generation != 0;
    }
    u32 EntityCount() const { return m_aliveCount; }
    /// Copies all copyable components into a new entity.
    Entity Clone(Entity e);

    // --- Components (typed) -----------------------------------------------------------------------------------
    /// Adds (or overwrites) a component; returns a pointer to it (nullptr for tags).
    template <typename T>
    T* Add(Entity e, T value = {})
    {
        return static_cast<T*>(SetComponentRelocate(e, ComponentIdOf<T>(), std::is_empty_v<T> ? nullptr : &value, false));
    }
    /// Like Add, but fires OnSet observers (use for "value changed" semantics).
    template <typename T>
    T* Set(Entity e, T value)
    {
        return static_cast<T*>(SetComponentRelocate(e, ComponentIdOf<T>(), std::is_empty_v<T> ? nullptr : &value, true));
    }
    template <typename T>
    bool Remove(Entity e)
    {
        return RemoveComponent(e, ComponentIdOf<T>());
    }
    template <typename T>
    bool Has(Entity e) const
    {
        return HasComponent(e, ComponentIdOf<T>());
    }
    /// Get<T> marks the component as changed, Get<const T> does not. nullptr if absent (or a tag).
    template <typename T>
    T* Get(Entity e)
    {
        return static_cast<T*>(GetComponent(e, ComponentIdOf<T>(), !std::is_const_v<T>));
    }
    template <typename T>
    const T* Get(Entity e) const
    {
        return static_cast<const T*>(const_cast<World*>(this)->GetComponent(e, ComponentIdOf<T>(), false));
    }

    // --- Components (untyped, for serialization/editor) -------------------------------------------------------
    /// Adds a default-constructed component if missing. Returns its storage (nullptr for tags / dead entity).
    void* AddComponent(Entity e, ComponentId id);
    /// Adds or overwrites by relocating `value` (moved-from and destroyed by the call). value may be null for tags.
    void* SetComponentRelocate(Entity e, ComponentId id, void* value, bool fireOnSet);
    bool RemoveComponent(Entity e, ComponentId id);
    bool HasComponent(Entity e, ComponentId id) const;
    void* GetComponent(Entity e, ComponentId id, bool markChanged);
    const Archetype* GetArchetypeOf(Entity e) const;

    // --- Queries ----------------------------------------------------------------------------------------------
    template <typename... Ts>
    ecs::Query<Ts...> Query();
    /// Archetypes containing all of `all` and none of `none` (cached; valid until the next structural change).
    Span<Archetype* const> MatchArchetypes(const ComponentMask& all, const ComponentMask& none);
    Span<Archetype* const> Archetypes() const { return m_archetypes.View(); }

    // --- Change detection -------------------------------------------------------------------------------------
    /// Monotonic version stamped on written chunk columns. Systems compare against their last run version.
    u32 Version() const { return m_version; }
    u32 IncrementVersion() { return ++m_version; }

    // --- Observers (called after add, before remove, after Set) ------------------------------------------------
    template <typename T, typename F>
    void OnAdd(F&& fn)
    {
        AddHook(m_onAdd, ComponentIdOf<T>(), WrapHook<T>(std::forward<F>(fn)));
    }
    template <typename T, typename F>
    void OnRemove(F&& fn)
    {
        AddHook(m_onRemove, ComponentIdOf<T>(), WrapHook<T>(std::forward<F>(fn)));
    }
    template <typename T, typename F>
    void OnSet(F&& fn)
    {
        AddHook(m_onSet, ComponentIdOf<T>(), WrapHook<T>(std::forward<F>(fn)));
    }

    // --- Singletons (world-global components) ------------------------------------------------------------------
    template <typename T>
    T& SetSingleton(T value = {})
    {
        if (!m_singletonEntity)
            m_singletonEntity = Create();
        return *Add<T>(m_singletonEntity, std::move(value));
    }
    template <typename T>
    T* GetSingleton()
    {
        return m_singletonEntity ? Get<T>(m_singletonEntity) : nullptr;
    }

    // --- Iteration guard ---------------------------------------------------------------------------------------
    bool IsIterating() const { return m_iterating.load(std::memory_order_relaxed) != 0; }
    struct IterationScope
    {
        explicit IterationScope(World& w) : world(w) { world.m_iterating.fetch_add(1, std::memory_order_relaxed); }
        ~IterationScope() { world.m_iterating.fetch_sub(1, std::memory_order_relaxed); }
        World& world;
    };

    struct Stats
    {
        u32 entities = 0;
        u32 archetypes = 0;
        u32 chunks = 0;
        u64 chunkBytes = 0;
    };
    Stats GetStats() const;

private:
    struct Record
    {
        Archetype* archetype = nullptr;
        u32 chunk = 0;
        u32 row = 0;
        u32 generation = 1;
    };
    struct CachedQuery
    {
        ComponentMask all;
        ComponentMask none;
        Array<Archetype*> archetypes;
        u32 checkedCount = 0;
    };

    Archetype* GetArchetype(const ComponentMask& mask);
    Archetype* CreateArchetype(const ComponentMask& mask);
    Archetype* AddEdge(Archetype* from, ComponentId id);
    Archetype* RemoveEdge(Archetype* from, ComponentId id);
    Entity CreateInArchetype(Archetype* archetype, bool construct);
    /// Allocates a row (components unconstructed). Returns {chunk,row}.
    void AllocateRow(Archetype* a, Entity e, u32& chunk, u32& row);
    /// Removes a row by moving the archetype's last entity into it. destroy = run destructors on the row first.
    void FreeRow(Archetype* a, u32 chunk, u32 row, bool destroy);
    /// Moves an entity to another archetype; components in both are relocated, extra source ones destroyed,
    /// missing destination ones default-constructed unless skipConstruct is set.
    void MoveEntity(Entity e, Archetype* to, ComponentId skipConstruct);
    void FreeChunk(Archetype::Chunk& c);
    void CheckNotIterating() const;

    template <typename T, typename V>
    void InitCreated(Entity e, V&& value)
    {
        if constexpr (!std::is_empty_v<T>) {
            const Record& r = m_records[e.index];
            const i32 col = r.archetype->ColumnIndex(ComponentIdOf<T>());
            ::new (r.archetype->ComponentPtr(r.chunk, u32(col), r.row)) T(std::forward<V>(value));
        }
    }

    using HookList = Array<ObserverFunction>;
    template <typename T, typename F>
    static ObserverFunction WrapHook(F&& fn)
    {
        return ObserverFunction(
            [f = std::forward<F>(fn)](World& w, Entity e, void* c) mutable {
                if constexpr (!std::is_empty_v<T> && std::is_invocable_v<F&, World&, Entity, T&>) {
                    f(w, e, *static_cast<T*>(c));
                } else {
                    (void)c;
                    f(w, e);
                }
            });
    }
    void AddHook(HashMap<ComponentId, HookList>& hooks, ComponentId id, ObserverFunction fn);
    void FireHook(HashMap<ComponentId, HookList>& hooks, ComponentId id, Entity e);

    Array<Record> m_records;
    Array<u32> m_freeIndices;
    u32 m_aliveCount = 0;
    Array<Archetype*> m_archetypes;
    HashMap<u64, Array<Archetype*>> m_archetypeByHash;
    Archetype* m_emptyArchetype = nullptr;
    Array<CachedQuery*> m_queryCache;
    HashMap<u64, Array<CachedQuery*>> m_queryCacheByHash;
    Mutex m_queryCacheMutex;
    u32 m_version = 1;
    std::atomic<u32> m_iterating{0};
    HashMap<ComponentId, HookList> m_onAdd;
    HashMap<ComponentId, HookList> m_onRemove;
    HashMap<ComponentId, HookList> m_onSet;
    Entity m_singletonEntity;
};

} // namespace zx::ecs

#include "Scene/ECS/Query.h"
