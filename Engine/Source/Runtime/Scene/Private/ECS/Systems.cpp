#include "Scene/ECS/Systems.h"
#include "Core/Jobs/JobSystem.h"
#include "Core/Log/Log.h"

ZX_DEFINE_LOG_CATEGORY_STATIC(LogECS, Info);

namespace zx::ecs {

const char* SystemPhaseName(SystemPhase phase)
{
    switch (phase) {
    case SystemPhase::PreUpdate: return "PreUpdate";
    case SystemPhase::FixedUpdate: return "FixedUpdate";
    case SystemPhase::Update: return "Update";
    case SystemPhase::PostUpdate: return "PostUpdate";
    case SystemPhase::PreRender: return "PreRender";
    default: return "?";
    }
}

SystemDesc& SystemDesc::After(StringView systemName)
{
    m_after.PushBack(String(systemName));
    m_owner->Invalidate();
    return *this;
}

SystemDesc& SystemDesc::Exclusive()
{
    m_exclusive = true;
    m_owner->Invalidate();
    return *this;
}

SystemDesc& SystemDesc::Enabled(bool enabled)
{
    m_enabled = enabled;
    return *this;
}

SystemScheduler::~SystemScheduler()
{
    for (SystemDesc* s : m_systems)
        zx::Delete(s);
}

SystemDesc& SystemScheduler::Add(StringView name, SystemPhase phase, SystemFunction fn)
{
    ZX_ASSERT(!Find(name), "System '{}' registered twice", name);
    SystemDesc* s = zx::New<SystemDesc>();
    s->m_name = String(name);
    s->m_phase = phase;
    s->m_function = move(fn);
    s->m_owner = this;
    m_systems.PushBack(s);
    m_dirty = true;
    return *s;
}

SystemDesc* SystemScheduler::Find(StringView name)
{
    for (SystemDesc* s : m_systems)
        if (s->m_name == name)
            return s;
    return nullptr;
}

bool SystemScheduler::Conflicts(const SystemDesc& a, const SystemDesc& b)
{
    if (!a.m_declared || !b.m_declared || a.m_exclusive || b.m_exclusive)
        return true;
    return a.m_writes.Intersects(b.m_reads) || a.m_writes.Intersects(b.m_writes) || b.m_writes.Intersects(a.m_reads);
}

void SystemScheduler::Rebuild()
{
    for (u32 p = 0; p < u32(SystemPhase::Count); ++p) {
        Array<u32> phaseSystems;
        for (u32 i = 0; i < m_systems.Size(); ++i)
            if (u32(m_systems[i]->m_phase) == p)
                phaseSystems.PushBack(i);

        // Topological order (Kahn, lowest registration index first) over After() constraints.
        const u32 n = u32(phaseSystems.Size());
        Array<Array<u32>> deps; // deps[i] = local indices that must run before i
        deps.Resize(n);
        for (u32 i = 0; i < n; ++i)
            for (const String& name : m_systems[phaseSystems[i]]->m_after) {
                bool found = false;
                for (u32 j = 0; j < n; ++j)
                    if (m_systems[phaseSystems[j]]->m_name == name) {
                        deps[i].PushBack(j);
                        found = true;
                    }
                if (!found)
                    ZX_LOG_WARN(LogECS, "System '{}' runs after unknown system '{}' (or one in another phase)",
                                   m_systems[phaseSystems[i]]->m_name, name);
            }
        Array<u32> order;
        Array<bool> placed;
        placed.Resize(n, false);
        while (order.Size() < n) {
            bool progress = false;
            for (u32 i = 0; i < n; ++i) {
                if (placed[i])
                    continue;
                bool ready = true;
                for (u32 d : deps[i])
                    ready &= placed[d];
                if (ready) {
                    placed[i] = true;
                    order.PushBack(i);
                    progress = true;
                    break;
                }
            }
            if (!progress) {
                ZX_LOG_ERROR(LogECS, "Cycle in system dependencies of phase {}; falling back to registration order",
                             SystemPhaseName(SystemPhase(p)));
                order.Clear();
                for (u32 i = 0; i < n; ++i)
                    order.PushBack(i);
                for (u32 i = 0; i < n; ++i)
                    deps[i].Clear();
                break;
            }
        }

        // Batch level = 1 + max level of earlier conflicting systems or dependencies.
        Array<u32> level;
        level.Resize(n, 0);
        u32 levelCount = 0;
        for (u32 oi = 0; oi < n; ++oi) {
            const u32 i = order[oi];
            const SystemDesc& si = *m_systems[phaseSystems[i]];
            u32 l = 0;
            for (u32 oj = 0; oj < oi; ++oj) {
                const u32 j = order[oj];
                const SystemDesc& sj = *m_systems[phaseSystems[j]];
                bool dependent = Conflicts(si, sj);
                for (u32 d : deps[i])
                    dependent |= d == j;
                if (dependent)
                    l = Max(l, level[j] + 1);
            }
            level[i] = l;
            levelCount = Max(levelCount, l + 1);
        }
        Array<Array<u32>>& batches = m_batches[p];
        batches.Clear();
        batches.Resize(levelCount);
        for (u32 oi = 0; oi < n; ++oi)
            batches[level[order[oi]]].PushBack(phaseSystems[order[oi]]);
    }
    m_dirty = false;
}

void SystemScheduler::Run(World& world, SystemPhase phase, f32 deltaTime)
{
    if (m_dirty)
        Rebuild();
    for (const Array<u32>& batch : m_batches[u32(phase)]) {
        SmallArray<SystemDesc*, 16> active;
        for (u32 i : batch)
            if (m_systems[i]->m_enabled)
                active.PushBack(m_systems[i]);
        if (active.Empty())
            continue;
        const u32 version = world.IncrementVersion();
        SmallArray<CommandBuffer*, 16> buffers;
        for (usize i = 0; i < active.Size(); ++i)
            buffers.PushBack(zx::New<CommandBuffer>());

        auto execute = [&](usize i) {
            SystemDesc* s = active[i];
            SystemContext ctx{world, *buffers[i], deltaTime, s->m_lastRunVersion};
            s->m_function(ctx);
            s->m_lastRunVersion = version;
        };
        if (m_parallel && active.Size() > 1 && jobs::IsInitialized()) {
            SmallArray<JobHandle, 16> handles;
            for (usize i = 1; i < active.Size(); ++i)
                handles.PushBack(jobs::Schedule([&execute, i] { execute(i); }));
            execute(0);
            jobs::WaitAll(handles.View());
        } else {
            for (usize i = 0; i < active.Size(); ++i)
                execute(i);
        }
        for (CommandBuffer* cmd : buffers) {
            cmd->Playback(world);
            zx::Delete(cmd);
        }
    }
}

Array<Array<String>> SystemScheduler::GetBatches(SystemPhase phase)
{
    if (m_dirty)
        Rebuild();
    Array<Array<String>> out;
    for (const Array<u32>& batch : m_batches[u32(phase)]) {
        Array<String>& names = out.AddDefaulted();
        for (u32 i : batch)
            names.PushBack(m_systems[i]->m_name);
    }
    return out;
}

String SystemScheduler::DumpSchedule()
{
    String out;
    for (u32 p = 0; p < u32(SystemPhase::Count); ++p) {
        const Array<Array<String>> batches = GetBatches(SystemPhase(p));
        if (batches.Empty())
            continue;
        out += String::Format("{}:\n", SystemPhaseName(SystemPhase(p)));
        for (usize b = 0; b < batches.Size(); ++b) {
            out += String::Format("  [{}]", b);
            for (const String& name : batches[b])
                out += String::Format(" {}", name);
            out += "\n";
        }
    }
    return out;
}

} // namespace zx::ecs
