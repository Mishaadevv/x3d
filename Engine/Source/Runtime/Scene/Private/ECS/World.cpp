#include "Scene/ECS/World.h"
#include "Core/Math/MathCommon.h"
#include "Core/Memory/Memory.h"

namespace zx::ecs {

// --- Component registry ---------------------------------------------------------------------------------------------

namespace {
ComponentInfo g_componentInfos[MaxComponentTypes];
std::atomic<u32> g_componentCount{0};
Mutex g_registryMutex;
} // namespace

ComponentId detail::RegisterComponent(const ComponentInfo& info)
{
    ScopedLock lock(g_registryMutex);
    const u32 count = g_componentCount.load(std::memory_order_relaxed);
    // Same name registered again (e.g. a reloaded game module): reuse the id if the layout matches.
    for (u32 i = 0; i < count; ++i)
        if (g_componentInfos[i].name == info.name) {
            ZX_ASSERT(g_componentInfos[i].size == info.size && g_componentInfos[i].alignment == info.alignment,
                      "Component '{}' re-registered with a different layout", info.name);
            g_componentInfos[i] = info;
            return ComponentId(i);
        }
    ZX_ASSERT(count < MaxComponentTypes, "Too many component types (max {})", MaxComponentTypes);
    g_componentInfos[count] = info;
    g_componentCount.store(count + 1, std::memory_order_release);
    return ComponentId(count);
}

const ComponentInfo& GetComponentInfo(ComponentId id)
{
    ZX_ASSERT(id < g_componentCount.load(std::memory_order_acquire), "Invalid component id {}", id);
    return g_componentInfos[id];
}

u32 GetComponentTypeCount() { return g_componentCount.load(std::memory_order_acquire); }

ComponentId FindComponentByName(StringView name)
{
    const u32 count = GetComponentTypeCount();
    for (u32 i = 0; i < count; ++i)
        if (g_componentInfos[i].name == name)
            return ComponentId(i);
    return InvalidComponentId;
}

// --- World ----------------------------------------------------------------------------------------------------------

World::World() { m_emptyArchetype = CreateArchetype(ComponentMask{}); }

World::~World()
{
    for (Archetype* a : m_archetypes) {
        for (Archetype::Chunk& c : a->m_chunks) {
            for (u32 col = 0; col < a->m_columnIds.Size(); ++col) {
                const ComponentInfo& info = GetComponentInfo(a->m_columnIds[col]);
                if (!info.trivial)
                    info.destruct(c.memory + a->m_columnOffsets[col], c.count);
            }
            FreeChunk(c);
        }
        zx::Delete(a);
    }
    for (CachedQuery* q : m_queryCache)
        zx::Delete(q);
}

void World::CheckNotIterating() const
{
    ZX_ASSERT(!IsIterating(), "Structural ECS change during query iteration - record it in a CommandBuffer");
}

Archetype* World::CreateArchetype(const ComponentMask& mask)
{
    Archetype* a = zx::New<Archetype>();
    a->m_mask = mask;
    u32 bytesPerEntity = sizeof(Entity);
    mask.ForEach([&](ComponentId id) {
        a->m_components.PushBack(id);
        const ComponentInfo& info = GetComponentInfo(id);
        if (info.size > 0) {
            a->m_columnIds.PushBack(id);
            a->m_columnSizes.PushBack(info.size);
            bytesPerEntity += info.size;
        }
    });
    a->m_columnOffsets.ResizeZeroed(a->m_columnIds.Size());

    // Largest capacity whose aligned SoA layout fits in a chunk (at least one entity; big entities get bigger chunks).
    auto layout = [&](u32 capacity) {
        u32 offset = capacity * u32(sizeof(Entity));
        for (u32 col = 0; col < a->m_columnIds.Size(); ++col) {
            const ComponentInfo& info = GetComponentInfo(a->m_columnIds[col]);
            offset = u32(AlignUp(u64(offset), u64(info.alignment)));
            a->m_columnOffsets[col] = offset;
            offset += info.size * capacity;
        }
        return offset;
    };
    u32 capacity = Max(1u, ChunkSize / bytesPerEntity);
    while (capacity > 1 && layout(capacity) > ChunkSize)
        --capacity;
    const u32 bytes = layout(capacity);
    a->m_capacity = capacity;
    a->m_chunkBytes = Max(ChunkSize, u32(AlignUp(u64(bytes), u64(64))));

    m_archetypes.PushBack(a);
    Array<Archetype*>* bucket = m_archetypeByHash.Find(mask.Hash());
    if (!bucket)
        bucket = &m_archetypeByHash.InsertOrAssign(mask.Hash(), Array<Archetype*>());
    bucket->PushBack(a);
    return a;
}

Archetype* World::GetArchetype(const ComponentMask& mask)
{
    if (Array<Archetype*>* bucket = m_archetypeByHash.Find(mask.Hash()))
        for (Archetype* a : *bucket)
            if (a->m_mask == mask)
                return a;
    return CreateArchetype(mask);
}

Archetype* World::AddEdge(Archetype* from, ComponentId id)
{
    if (Archetype** cached = from->m_addEdges.Find(id))
        return *cached;
    ComponentMask mask = from->m_mask;
    mask.Set(id);
    Archetype* to = GetArchetype(mask);
    from->m_addEdges.InsertOrAssign(id, to);
    to->m_removeEdges.InsertOrAssign(id, from);
    return to;
}

Archetype* World::RemoveEdge(Archetype* from, ComponentId id)
{
    if (Archetype** cached = from->m_removeEdges.Find(id))
        return *cached;
    ComponentMask mask = from->m_mask;
    mask.Reset(id);
    Archetype* to = GetArchetype(mask);
    from->m_removeEdges.InsertOrAssign(id, to);
    to->m_addEdges.InsertOrAssign(id, from);
    return to;
}

void World::AllocateRow(Archetype* a, Entity e, u32& chunk, u32& row)
{
    if (a->m_chunks.Empty() || a->m_chunks.Back().count == a->m_capacity) {
        Archetype::Chunk c;
        c.memory = static_cast<u8*>(Malloc(a->m_chunkBytes, 64, MemTag::Scene));
        if (!a->m_columnIds.Empty())
            c.versions = static_cast<u32*>(Malloc(a->m_columnIds.Size() * sizeof(u32), alignof(u32), MemTag::Scene));
        a->m_chunks.PushBack(c);
    }
    chunk = u32(a->m_chunks.Size() - 1);
    Archetype::Chunk& c = a->m_chunks[chunk];
    row = c.count++;
    // A structural change counts as a write of every column of the chunk.
    for (u32 col = 0; col < a->m_columnIds.Size(); ++col)
        c.versions[col] = m_version;
    reinterpret_cast<Entity*>(c.memory)[row] = e;
    ++a->m_entityCount;
}

void World::FreeChunk(Archetype::Chunk& c)
{
    Free(c.memory);
    if (c.versions)
        Free(c.versions);
    c = {};
}

void World::FreeRow(Archetype* a, u32 chunk, u32 row, bool destroy)
{
    const u32 lastChunk = u32(a->m_chunks.Size() - 1);
    const u32 lastRow = a->m_chunks[lastChunk].count - 1;
    const bool moveLast = chunk != lastChunk || row != lastRow;
    for (u32 col = 0; col < a->m_columnIds.Size(); ++col) {
        const ComponentInfo& info = GetComponentInfo(a->m_columnIds[col]);
        void* dst = a->ComponentPtr(chunk, col, row);
        if (destroy && !info.trivial)
            info.destruct(dst, 1);
        if (moveLast) {
            info.relocate(dst, a->ComponentPtr(lastChunk, col, lastRow), 1);
            a->m_chunks[chunk].versions[col] = m_version;
        }
    }
    if (moveLast) {
        const Entity moved = a->Entities(lastChunk)[lastRow];
        a->Entities(chunk)[row] = moved;
        Record& r = m_records[moved.index];
        r.chunk = chunk;
        r.row = row;
    }
    --a->m_entityCount;
    if (--a->m_chunks[lastChunk].count == 0) {
        FreeChunk(a->m_chunks[lastChunk]);
        a->m_chunks.PopBack();
    }
}

Entity World::CreateInArchetype(Archetype* a, bool construct)
{
    CheckNotIterating();
    u32 index;
    if (!m_freeIndices.Empty()) {
        index = m_freeIndices.PopBackValue();
    } else {
        index = u32(m_records.Size());
        m_records.PushBack(Record{});
    }
    Record& r = m_records[index];
    const Entity e{index, r.generation};
    AllocateRow(a, e, r.chunk, r.row);
    r.archetype = a;
    if (construct)
        for (u32 col = 0; col < a->m_columnIds.Size(); ++col)
            GetComponentInfo(a->m_columnIds[col]).construct(a->ComponentPtr(r.chunk, col, r.row), 1);
    ++m_aliveCount;
    return e;
}

Entity World::Create() { return CreateInArchetype(m_emptyArchetype, true); }

void World::Destroy(Entity e)
{
    CheckNotIterating();
    if (!IsAlive(e))
        return;
    // Observers see the entity intact.
    for (ComponentId id : m_records[e.index].archetype->m_components) {
        FireHook(m_onRemove, id, e);
        if (!IsAlive(e))
            return;
    }
    Record& r = m_records[e.index];
    FreeRow(r.archetype, r.chunk, r.row, true);
    r.archetype = nullptr;
    if (++r.generation == 0 || r.generation == 0xFFFFFFFFu)
        r.generation = 1;
    m_freeIndices.PushBack(e.index);
    --m_aliveCount;
    if (e == m_singletonEntity)
        m_singletonEntity = NullEntity;
}

Entity World::Clone(Entity src)
{
    if (!IsAlive(src))
        return NullEntity;
    Archetype* a = m_records[src.index].archetype;
    const Entity e = CreateInArchetype(a, false);
    const Record& s = m_records[src.index];
    const Record& d = m_records[e.index];
    for (u32 col = 0; col < a->m_columnIds.Size(); ++col) {
        const ComponentInfo& info = GetComponentInfo(a->m_columnIds[col]);
        void* dst = a->ComponentPtr(d.chunk, col, d.row);
        if (info.copyConstruct)
            info.copyConstruct(dst, a->ComponentPtr(s.chunk, col, s.row));
        else
            info.construct(dst, 1);
    }
    for (ComponentId id : a->m_components)
        FireHook(m_onAdd, id, e);
    return e;
}

void World::MoveEntity(Entity e, Archetype* to, ComponentId skipConstruct)
{
    Record& r = m_records[e.index];
    Archetype* from = r.archetype;
    u32 chunk, row;
    AllocateRow(to, e, chunk, row);
    for (u32 col = 0; col < to->m_columnIds.Size(); ++col) {
        const ComponentId id = to->m_columnIds[col];
        const ComponentInfo& info = GetComponentInfo(id);
        void* dst = to->ComponentPtr(chunk, col, row);
        const i32 fromCol = from->ColumnIndex(id);
        if (fromCol >= 0)
            info.relocate(dst, from->ComponentPtr(r.chunk, u32(fromCol), r.row), 1);
        else if (id != skipConstruct)
            info.construct(dst, 1);
    }
    for (u32 col = 0; col < from->m_columnIds.Size(); ++col) {
        const ComponentId id = from->m_columnIds[col];
        if (to->ColumnIndex(id) < 0) {
            const ComponentInfo& info = GetComponentInfo(id);
            if (!info.trivial)
                info.destruct(from->ComponentPtr(r.chunk, col, r.row), 1);
        }
    }
    FreeRow(from, r.chunk, r.row, false);
    Record& nr = m_records[e.index]; // FreeRow may have touched other records, never this one
    nr.archetype = to;
    nr.chunk = chunk;
    nr.row = row;
}

void* World::GetComponent(Entity e, ComponentId id, bool markChanged)
{
    if (!IsAlive(e))
        return nullptr;
    const Record& r = m_records[e.index];
    const i32 col = r.archetype->ColumnIndex(id);
    if (col < 0)
        return nullptr;
    if (markChanged)
        r.archetype->MarkColumnChanged(r.chunk, u32(col), m_version);
    return r.archetype->ComponentPtr(r.chunk, u32(col), r.row);
}

bool World::HasComponent(Entity e, ComponentId id) const
{
    return IsAlive(e) && m_records[e.index].archetype->m_mask.Test(id);
}

const Archetype* World::GetArchetypeOf(Entity e) const { return IsAlive(e) ? m_records[e.index].archetype : nullptr; }

void* World::SetComponentRelocate(Entity e, ComponentId id, void* value, bool fireOnSet)
{
    if (!IsAlive(e))
        return nullptr;
    const ComponentInfo& info = GetComponentInfo(id);
    if (HasComponent(e, id)) {
        void* ptr = GetComponent(e, id, true);
        if (value && ptr) {
            if (!info.trivial)
                info.destruct(ptr, 1);
            info.relocate(ptr, value, 1);
        }
        if (fireOnSet)
            FireHook(m_onSet, id, e);
        return GetComponent(e, id, false);
    }
    CheckNotIterating();
    Archetype* to = AddEdge(m_records[e.index].archetype, id);
    MoveEntity(e, to, value ? id : InvalidComponentId);
    if (value && info.size > 0)
        info.relocate(GetComponent(e, id, false), value, 1);
    FireHook(m_onAdd, id, e);
    if (fireOnSet && IsAlive(e))
        FireHook(m_onSet, id, e);
    return GetComponent(e, id, false);
}

void* World::AddComponent(Entity e, ComponentId id)
{
    if (HasComponent(e, id))
        return GetComponent(e, id, false);
    return SetComponentRelocate(e, id, nullptr, false);
}

bool World::RemoveComponent(Entity e, ComponentId id)
{
    if (!HasComponent(e, id))
        return false;
    CheckNotIterating();
    FireHook(m_onRemove, id, e);
    if (!HasComponent(e, id))
        return true;
    MoveEntity(e, RemoveEdge(m_records[e.index].archetype, id), InvalidComponentId);
    return true;
}

Span<Archetype* const> World::MatchArchetypes(const ComponentMask& all, const ComponentMask& none)
{
    ScopedLock lock(m_queryCacheMutex);
    const u64 key = all.Hash() ^ (none.Hash() * 0x9E3779B97F4A7C15ull);
    CachedQuery* query = nullptr;
    Array<CachedQuery*>* bucket = m_queryCacheByHash.Find(key);
    if (bucket)
        for (CachedQuery* q : *bucket)
            if (q->all == all && q->none == none)
                query = q;
    if (!query) {
        query = zx::New<CachedQuery>();
        query->all = all;
        query->none = none;
        m_queryCache.PushBack(query);
        if (!bucket)
            bucket = &m_queryCacheByHash.InsertOrAssign(key, Array<CachedQuery*>());
        bucket->PushBack(query);
    }
    for (; query->checkedCount < m_archetypes.Size(); ++query->checkedCount) {
        Archetype* a = m_archetypes[query->checkedCount];
        if (a->m_mask.ContainsAll(all) && !a->m_mask.Intersects(none))
            query->archetypes.PushBack(a);
    }
    return Span<Archetype* const>(query->archetypes.Data(), query->archetypes.Size());
}

void World::AddHook(HashMap<ComponentId, HookList>& hooks, ComponentId id, ObserverFunction fn)
{
    HookList* list = hooks.Find(id);
    if (!list)
        list = &hooks.InsertOrAssign(id, HookList());
    list->PushBack(move(fn));
}

void World::FireHook(HashMap<ComponentId, HookList>& hooks, ComponentId id, Entity e)
{
    HookList* list = hooks.Find(id);
    if (!list)
        return;
    for (usize i = 0; i < list->Size() && IsAlive(e); ++i)
        (*list)[i](*this, e, GetComponent(e, id, false));
}

World::Stats World::GetStats() const
{
    Stats s;
    s.entities = m_aliveCount;
    s.archetypes = u32(m_archetypes.Size());
    for (const Archetype* a : m_archetypes) {
        s.chunks += a->ChunkCount();
        s.chunkBytes += u64(a->ChunkCount()) * a->m_chunkBytes;
    }
    return s;
}

} // namespace zx::ecs
