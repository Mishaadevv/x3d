#pragma once

// Typed ECS queries. Included by World.h.
//
//   world.Query<Transform, const Velocity>()
//       .With<Enemy>().Without<Dead>().Changed<Velocity>(lastRunVersion)
//       .Each([](Entity e, Transform& t, const Velocity& v) { ... });
//
// Each/ParallelEach callbacks take (Ts&...) or (Entity, Ts&...). Non-const Ts mark the chunk columns as changed.

#include "Core/Jobs/JobSystem.h"

#include <tuple>

namespace zx::ecs {

/// One chunk of matched entities, for manual SoA loops (EachChunk).
template <typename... Ts>
struct ChunkView
{
    u32 count = 0;
    const Entity* entities = nullptr;
    std::tuple<std::remove_reference_t<Ts>*...> columns;

    template <usize I>
    auto* Column() const
    {
        return std::get<I>(columns);
    }
};

template <typename... Ts>
class Query
{
    static_assert(((!std::is_empty_v<std::remove_cvref_t<Ts>>) && ...),
                  "Tags have no data - filter them with With<Tag>() instead of the query parameter list");

public:
    explicit Query(World& world) : m_world(world)
    {
        (m_all.Set(ComponentIdOf<Ts>()), ...);
    }

    template <typename... Us>
    Query& With()
    {
        (m_all.Set(ComponentIdOf<Us>()), ...);
        return *this;
    }
    template <typename... Us>
    Query& Without()
    {
        (m_none.Set(ComponentIdOf<Us>()), ...);
        return *this;
    }
    /// Only chunks where any of the given components was written after `sinceVersion` (chunk granularity).
    template <typename... Us>
    Query& Changed(u32 sinceVersion)
    {
        (m_changed.PushBack(ComponentIdOf<Us>()), ...);
        m_changedSince = sinceVersion;
        return *this;
    }

    template <typename F>
    void Each(F&& fn)
    {
        World::IterationScope scope(m_world);
        const u32 version = m_world.Version();
        for (Archetype* a : m_world.MatchArchetypes(m_all, m_none)) {
            i32 cols[sizeof...(Ts) + 1];
            ResolveColumns(a, cols);
            for (u32 c = 0; c < a->ChunkCount(); ++c)
                if (PassesChangeFilter(a, c))
                    RunChunk(a, c, cols, version, fn, std::index_sequence_for<Ts...>{});
        }
    }

    /// Splits matched chunks across the job system. fn must be thread-safe; no structural changes.
    template <typename F>
    void ParallelEach(F&& fn, u32 minChunksPerJob = 1)
    {
        World::IterationScope scope(m_world);
        const u32 version = m_world.Version();
        struct Item
        {
            Archetype* archetype;
            u32 chunk;
            i32 cols[sizeof...(Ts) + 1];
        };
        Array<Item> items;
        for (Archetype* a : m_world.MatchArchetypes(m_all, m_none)) {
            Item item{a, 0, {}};
            ResolveColumns(a, item.cols);
            for (u32 c = 0; c < a->ChunkCount(); ++c)
                if (PassesChangeFilter(a, c)) {
                    item.chunk = c;
                    items.PushBack(item);
                }
        }
        jobs::ParallelFor(u32(items.Size()), minChunksPerJob, [&](u32 begin, u32 end) {
            for (u32 i = begin; i < end; ++i)
                RunChunk(items[i].archetype, items[i].chunk, items[i].cols, version, fn, std::index_sequence_for<Ts...>{});
        });
    }

    /// fn(ChunkView<Ts...>&) per matched chunk.
    template <typename F>
    void EachChunk(F&& fn)
    {
        World::IterationScope scope(m_world);
        const u32 version = m_world.Version();
        for (Archetype* a : m_world.MatchArchetypes(m_all, m_none)) {
            i32 cols[sizeof...(Ts) + 1];
            ResolveColumns(a, cols);
            for (u32 c = 0; c < a->ChunkCount(); ++c) {
                if (!PassesChangeFilter(a, c))
                    continue;
                MarkWritten(a, c, cols, version, std::index_sequence_for<Ts...>{});
                ChunkView<Ts...> view;
                view.count = a->ChunkEntityCount(c);
                view.entities = a->Entities(c);
                view.columns = MakeColumns(a, c, cols, std::index_sequence_for<Ts...>{});
                fn(view);
            }
        }
    }

    u32 Count()
    {
        u32 n = 0;
        for (Archetype* a : m_world.MatchArchetypes(m_all, m_none)) {
            if (m_changed.Empty()) {
                n += a->EntityCount();
                continue;
            }
            for (u32 c = 0; c < a->ChunkCount(); ++c)
                if (PassesChangeFilter(a, c))
                    n += a->ChunkEntityCount(c);
        }
        return n;
    }

    Entity First()
    {
        for (Archetype* a : m_world.MatchArchetypes(m_all, m_none))
            for (u32 c = 0; c < a->ChunkCount(); ++c)
                if (a->ChunkEntityCount(c) && PassesChangeFilter(a, c))
                    return a->Entities(c)[0];
        return NullEntity;
    }

private:
    void ResolveColumns(const Archetype* a, i32* cols) const
    {
        u32 i = 0;
        ((cols[i++] = a->ColumnIndex(ComponentIdOf<Ts>())), ...);
        cols[i] = -1;
    }

    bool PassesChangeFilter(const Archetype* a, u32 chunk) const
    {
        if (m_changed.Empty())
            return true;
        for (ComponentId id : m_changed) {
            const i32 col = a->ColumnIndex(id);
            if (col >= 0 && a->ColumnVersion(chunk, u32(col)) > m_changedSince)
                return true;
        }
        return false;
    }

    template <usize... I>
    static void MarkWritten(Archetype* a, u32 chunk, const i32* cols, u32 version, std::index_sequence<I...>)
    {
        ((std::is_const_v<std::remove_reference_t<Ts>> ? void() : a->MarkColumnChanged(chunk, u32(cols[I]), version)), ...);
    }

    template <usize... I>
    static std::tuple<std::remove_reference_t<Ts>*...> MakeColumns(Archetype* a, u32 chunk, const i32* cols,
                                                                   std::index_sequence<I...>)
    {
        return {static_cast<std::remove_reference_t<Ts>*>(a->ColumnData(chunk, u32(cols[I])))...};
    }

    template <typename F, usize... I>
    static void RunChunk(Archetype* a, u32 chunk, const i32* cols, u32 version, F& fn, std::index_sequence<I...> seq)
    {
        MarkWritten(a, chunk, cols, version, seq);
        const u32 n = a->ChunkEntityCount(chunk);
        const Entity* entities = a->Entities(chunk);
        const std::tuple<std::remove_reference_t<Ts>*...> data = MakeColumns(a, chunk, cols, seq);
        if constexpr (std::is_invocable_v<F&, Entity, std::remove_reference_t<Ts>&...>) {
            for (u32 i = 0; i < n; ++i)
                fn(entities[i], std::get<I>(data)[i]...);
        } else {
            static_assert(std::is_invocable_v<F&, std::remove_reference_t<Ts>&...>,
                          "Query callback must take (Ts&...) or (Entity, Ts&...)");
            for (u32 i = 0; i < n; ++i)
                fn(std::get<I>(data)[i]...);
        }
    }

    World& m_world;
    ComponentMask m_all;
    ComponentMask m_none;
    SmallArray<ComponentId, 4> m_changed;
    u32 m_changedSince = 0;
};

template <typename... Ts>
ecs::Query<Ts...> World::Query()
{
    return ecs::Query<Ts...>(*this);
}

} // namespace zx::ecs
