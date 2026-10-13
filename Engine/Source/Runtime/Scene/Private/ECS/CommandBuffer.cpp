#include "Scene/ECS/CommandBuffer.h"
#include "Core/Math/MathCommon.h"
#include "Core/Memory/Memory.h"
#include "Scene/ECS/World.h"

namespace zx::ecs {

CommandBuffer::~CommandBuffer()
{
    Clear();
    for (Page& p : m_pages)
        Free(p.memory);
}

Entity CommandBuffer::Create()
{
    const Entity e{m_pendingCount++, PendingGeneration};
    Push(Op::Create, e, InvalidComponentId, nullptr);
    return e;
}

void CommandBuffer::Destroy(Entity e) { Push(Op::Destroy, e, InvalidComponentId, nullptr); }

void* CommandBuffer::AllocateValue(usize size, usize alignment)
{
    // Values live in pages that never move, so non-trivial components stay valid until playback.
    for (Page& p : m_pages) {
        const usize offset = usize(AlignUp(u64(p.used), u64(alignment)));
        if (offset + size <= p.size) {
            p.used = offset + size;
            return p.memory + offset;
        }
    }
    const usize pageSize = Max(usize(4096), size + alignment);
    Page p{static_cast<u8*>(Malloc(pageSize, Max(alignment, usize(16)), MemTag::Scene)), pageSize, 0};
    const usize offset = usize(AlignUp(u64(0), u64(alignment)));
    p.used = offset + size;
    m_pages.PushBack(p);
    return p.memory + offset;
}

void CommandBuffer::DestroyValues()
{
    for (Command& c : m_commands)
        if (c.op == Op::Set && c.value) {
            const ComponentInfo& info = GetComponentInfo(c.component);
            if (!info.trivial)
                info.destruct(c.value, 1);
            c.value = nullptr;
        }
}

void CommandBuffer::Clear()
{
    DestroyValues();
    m_commands.Clear();
    for (Page& p : m_pages)
        p.used = 0;
    m_pendingCount = 0;
}

void CommandBuffer::Playback(World& world)
{
    Array<Entity> created;
    created.ResizeZeroed(m_pendingCount);
    auto resolve = [&](Entity e) { return IsPending(e) ? created[e.index] : e; };
    for (Command& c : m_commands) {
        const Entity e = resolve(c.entity);
        switch (c.op) {
        case Op::Create: created[c.entity.index] = world.Create(); break;
        case Op::Destroy: world.Destroy(e); break;
        case Op::Set:
            if (world.IsAlive(e)) {
                world.SetComponentRelocate(e, c.component, c.value, false);
                c.value = nullptr; // consumed
            }
            break;
        case Op::Remove: world.RemoveComponent(e, c.component); break;
        }
    }
    Clear();
}

} // namespace zx::ecs
