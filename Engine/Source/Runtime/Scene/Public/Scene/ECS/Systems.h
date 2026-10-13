#pragma once

// Systems and the scheduler. Systems declare component access; the scheduler groups non-conflicting systems of a
// phase into batches that run in parallel on the job system. Each system gets its own CommandBuffer, played back
// (in registration order) after its batch.
//
//   scheduler.Add("Movement", SystemPhase::Update, [](SystemContext& ctx) {
//       ctx.world.Query<Transform, const Velocity>().Each(...);
//   }).Writes<Transform>().Reads<Velocity>();
//   scheduler.Run(world, SystemPhase::Update, dt);
//
// A system without declared access is exclusive (runs alone).

#include "Core/String/String.h"
#include "Scene/ECS/CommandBuffer.h"
#include "Scene/ECS/World.h"

namespace zx::ecs {

enum class SystemPhase : u8
{
    PreUpdate,
    FixedUpdate,
    Update,
    PostUpdate,
    PreRender,
    Count
};

const char* SystemPhaseName(SystemPhase phase);

struct SystemContext
{
    World& world;
    CommandBuffer& commands;
    f32 deltaTime = 0.0f;
    /// World version when this system last ran (0 on the first run) - use with Query::Changed.
    u32 lastRunVersion = 0;
};

using SystemFunction = Function<void(SystemContext&), 48>;

class SystemScheduler;

class SystemDesc
{
public:
    template <typename... Ts>
    SystemDesc& Reads()
    {
        (m_reads.Set(ComponentIdOf<Ts>()), ...);
        m_declared = true;
        return *this;
    }
    template <typename... Ts>
    SystemDesc& Writes()
    {
        (m_writes.Set(ComponentIdOf<Ts>()), ...);
        m_declared = true;
        return *this;
    }
    /// Runs after the named system (same phase).
    SystemDesc& After(StringView systemName);
    /// Forces the system to run alone on the calling thread.
    SystemDesc& Exclusive();
    SystemDesc& Enabled(bool enabled);

    StringView Name() const { return m_name.View(); }
    SystemPhase Phase() const { return m_phase; }
    bool IsEnabled() const { return m_enabled; }

private:
    friend class SystemScheduler;
    String m_name;
    SystemPhase m_phase = SystemPhase::Update;
    SystemFunction m_function;
    ComponentMask m_reads;
    ComponentMask m_writes;
    Array<String> m_after;
    bool m_declared = false;
    bool m_exclusive = false;
    bool m_enabled = true;
    u32 m_lastRunVersion = 0;
    SystemScheduler* m_owner = nullptr;
};

class SystemScheduler
{
public:
    SystemScheduler() = default;
    ~SystemScheduler();
    ZX_NON_COPYABLE(SystemScheduler)

    SystemDesc& Add(StringView name, SystemPhase phase, SystemFunction fn);
    SystemDesc* Find(StringView name);

    /// Runs all enabled systems of the phase.
    void Run(World& world, SystemPhase phase, f32 deltaTime);
    /// Disable to run every system sequentially on the calling thread (debugging).
    void SetParallel(bool parallel) { m_parallel = parallel; }

    /// Batches of the phase as system-name lists (for tests/debug UI).
    Array<Array<String>> GetBatches(SystemPhase phase);
    String DumpSchedule();

private:
    friend class SystemDesc;
    void Invalidate() { m_dirty = true; }
    static bool Conflicts(const SystemDesc& a, const SystemDesc& b);
    void Rebuild();

    Array<SystemDesc*> m_systems;
    /// Per phase: batches of system indices.
    Array<Array<u32>> m_batches[u32(SystemPhase::Count)];
    bool m_dirty = true;
    bool m_parallel = true;
};

} // namespace zx::ecs
