#include "Core/HAL/PlatformMisc.h"
#include "Core/Jobs/JobSystem.h"
#include "Core/String/String.h"
#include "Core/Testing/Test.h"
#include "Core/Time/Time.h"
#include "Scene/ECS/Systems.h"

#include <atomic>

using namespace zx;
using namespace zx::ecs;

namespace {

struct Position
{
    f32 x = 0, y = 0, z = 0;
};
struct Velocity
{
    f32 x = 0, y = 0, z = 0;
};
struct Health
{
    i32 value = 100;
};
struct EnemyTag
{
};
struct DeadTag
{
};
struct Named // non-trivial component
{
    static inline i32 alive = 0;
    String name;
    Named() { ++alive; }
    explicit Named(StringView n) : name(n) { ++alive; }
    Named(const Named& o) : name(o.name) { ++alive; }
    Named(Named&& o) noexcept : name(move(o.name)) { ++alive; }
    Named& operator=(const Named&) = default;
    Named& operator=(Named&&) = default;
    ~Named() { --alive; }
};
struct alignas(64) BigAligned
{
    f32 data[16] = {};
};

struct JobSystemScope
{
    JobSystemScope() { jobs::Initialize({.workerCount = 4}); }
    ~JobSystemScope() { jobs::Shutdown(); }
};

} // namespace

ZX_TEST(ECS, ComponentRegistry)
{
    const ComponentId a = ComponentIdOf<Position>();
    ZX_EXPECT_EQ(a, ComponentIdOf<const Position>());
    ZX_EXPECT(a != ComponentIdOf<Velocity>());
    ZX_EXPECT(GetComponentInfo(a).name == "`anonymous-namespace'::Position");
    ZX_EXPECT_EQ(GetComponentInfo(a).size, u32(sizeof(Position)));
    ZX_EXPECT_EQ(GetComponentInfo(ComponentIdOf<EnemyTag>()).size, 0u);
    ZX_EXPECT(!GetComponentInfo(ComponentIdOf<Named>()).trivial);
    ZX_EXPECT_EQ(FindComponentByName(GetComponentInfo(a).name), a);
}

ZX_TEST(ECS, CreateDestroyGenerations)
{
    World w;
    const Entity a = w.Create();
    const Entity b = w.Create(Position{1, 2, 3});
    ZX_EXPECT(w.IsAlive(a) && w.IsAlive(b));
    ZX_EXPECT_EQ(w.EntityCount(), 2u);
    w.Destroy(a);
    ZX_EXPECT(!w.IsAlive(a));
    const Entity c = w.Create();
    ZX_EXPECT_EQ(c.index, a.index); // index reused...
    ZX_EXPECT(c.generation != a.generation); // ...with a new generation
    ZX_EXPECT(!w.IsAlive(a));
    w.Destroy(a); // stale handle: no-op
    ZX_EXPECT(w.IsAlive(c));
    ZX_EXPECT_EQ(w.Get<Position>(b)->y, 2.0f);
    ZX_EXPECT(w.Get<Position>(c) == nullptr);
    ZX_EXPECT(!w.IsAlive(NullEntity));
}

ZX_TEST(ECS, AddRemoveKeepsData)
{
    World w;
    Array<Entity> entities;
    for (i32 i = 0; i < 1000; ++i)
        entities.PushBack(w.Create(Position{f32(i), 0, 0}));
    for (i32 i = 0; i < 1000; i += 2)
        w.Add(entities[i], Velocity{0, f32(i), 0});
    for (i32 i = 0; i < 1000; i += 3)
        w.Add<EnemyTag>(entities[i]);
    for (i32 i = 0; i < 1000; i += 6)
        w.Remove<Velocity>(entities[i]);
    bool ok = true;
    for (i32 i = 0; i < 1000; ++i) {
        const Entity e = entities[i];
        ok &= w.Get<const Position>(e)->x == f32(i);
        ok &= w.Has<Velocity>(e) == (i % 2 == 0 && i % 6 != 0);
        ok &= w.Has<EnemyTag>(e) == (i % 3 == 0);
        if (w.Has<Velocity>(e))
            ok &= w.Get<const Velocity>(e)->y == f32(i);
    }
    ZX_EXPECT(ok);
    // Destroy every other entity: swap-remove must keep the rest intact.
    for (i32 i = 0; i < 1000; i += 2)
        w.Destroy(entities[i]);
    ok = true;
    for (i32 i = 1; i < 1000; i += 2)
        ok &= w.IsAlive(entities[i]) && w.Get<const Position>(entities[i])->x == f32(i);
    ZX_EXPECT(ok);
    ZX_EXPECT_EQ(w.EntityCount(), 500u);
    // Add overwrites.
    w.Add(entities[1], Position{42, 0, 0});
    ZX_EXPECT_EQ(w.Get<const Position>(entities[1])->x, 42.0f);
}

ZX_TEST(ECS, NonTrivialComponentsLifetime)
{
    {
        World w;
        Array<Entity> es;
        for (i32 i = 0; i < 300; ++i)
            es.PushBack(w.Create(Named(String::Format("entity {}", i)), Health{i}));
        ZX_EXPECT_EQ(Named::alive, 300);
        for (i32 i = 0; i < 300; i += 3)
            w.Add(es[i], Velocity{}); // archetype move relocates Named
        for (i32 i = 0; i < 300; i += 5)
            w.Remove<Named>(es[i]);
        for (i32 i = 0; i < 300; i += 7)
            w.Destroy(es[i]);
        bool ok = true;
        i32 expected = 0;
        for (i32 i = 0; i < 300; ++i) {
            if (i % 7 == 0)
                continue;
            if (i % 5 == 0) {
                ok &= !w.Has<Named>(es[i]);
                continue;
            }
            ++expected;
            ok &= w.Get<const Named>(es[i])->name == String::Format("entity {}", i);
        }
        ZX_EXPECT(ok);
        ZX_EXPECT_EQ(Named::alive, expected);
        const Entity clone = w.Clone(es[1]);
        ZX_EXPECT(w.Get<const Named>(clone)->name == "entity 1");
        ZX_EXPECT_EQ(w.Get<const Health>(clone)->value, 1);
    }
    ZX_EXPECT_EQ(Named::alive, 0); // world destructor destroys remaining components
}

ZX_TEST(ECS, LargeAndOverAlignedComponents)
{
    World w;
    const Entity e = w.Create(BigAligned{}, Position{1, 1, 1});
    for (i32 i = 0; i < 1000; ++i)
        w.Create(BigAligned{}, Position{});
    ZX_EXPECT(reinterpret_cast<uptr>(w.Get<BigAligned>(e)) % 64 == 0);
    const Archetype* a = w.GetArchetypeOf(e);
    ZX_EXPECT(a->ChunkCapacity() > 100 && a->ChunkCapacity() < 250);
    bool aligned = true;
    w.Query<BigAligned>().Each([&](BigAligned& b) { aligned &= reinterpret_cast<uptr>(&b) % 64 == 0; });
    ZX_EXPECT(aligned);
}

ZX_TEST(ECS, QueryFilters)
{
    World w;
    for (i32 i = 0; i < 100; ++i) {
        const Entity e = w.Create(Position{f32(i), 0, 0});
        if (i % 2 == 0)
            w.Add(e, Velocity{1, 0, 0});
        if (i % 4 == 0)
            w.Add<EnemyTag>(e);
        if (i % 8 == 0)
            w.Add<DeadTag>(e);
    }
    ZX_EXPECT_EQ(w.Query<Position>().Count(), 100u);
    ZX_EXPECT_EQ((w.Query<Position, Velocity>().Count()), 50u);
    ZX_EXPECT_EQ(w.Query<Position>().With<EnemyTag>().Count(), 25u);
    ZX_EXPECT_EQ(w.Query<Position>().With<EnemyTag>().Without<DeadTag>().Count(), 12u);
    ZX_EXPECT_EQ(w.Query<Velocity>().Without<EnemyTag>().Count(), 25u);

    w.Query<Position, const Velocity>().Each([](Position& p, const Velocity& v) { p.x += v.x * 1000.0f; });
    f32 sum = 0;
    u32 withEntity = 0;
    w.Query<const Position>().Each([&](Entity e, const Position& p) {
        sum += p.x;
        withEntity += w.IsAlive(e) ? 1 : 0;
    });
    ZX_EXPECT_EQ(sum, 4950.0f + 50 * 1000.0f);
    ZX_EXPECT_EQ(withEntity, 100u);
    ZX_EXPECT(w.Query<Position>().With<DeadTag>().First());
    ZX_EXPECT(!w.Query<Health>().First());

    // Chunk iteration.
    u32 chunkTotal = 0;
    w.Query<const Position, Velocity>().EachChunk([&](ChunkView<const Position, Velocity>& c) {
        for (u32 i = 0; i < c.count; ++i)
            c.Column<1>()[i].y = c.Column<0>()[i].x;
        chunkTotal += c.count;
    });
    ZX_EXPECT_EQ(chunkTotal, 50u);
}

ZX_TEST(ECS, ChangeDetection)
{
    World w;
    Array<Entity> es;
    for (i32 i = 0; i < 5000; ++i)
        es.PushBack(w.Create(Position{}, Velocity{}));
    const u32 v0 = w.IncrementVersion();
    ZX_EXPECT_EQ(w.Query<Position>().Changed<Position>(v0).Count(), 0u);
    w.IncrementVersion();
    w.Get<Position>(es[10])->x = 1; // marks one chunk
    const u32 changed = w.Query<Position>().Changed<Position>(v0).Count();
    ZX_EXPECT(changed > 0 && changed < 5000u);
    ZX_EXPECT_EQ(w.Query<Position>().Changed<Velocity>(v0).Count(), 0u);
    (void)w.Get<const Velocity>(es[10]); // const access doesn't mark
    ZX_EXPECT_EQ(w.Query<Position>().Changed<Velocity>(v0).Count(), 0u);
    w.IncrementVersion();
    w.Query<Velocity>().Each([](Velocity& v) { v.x = 1; });
    ZX_EXPECT_EQ(w.Query<Position>().Changed<Velocity>(v0).Count(), 5000u);
}

ZX_TEST(ECS, CommandBuffer)
{
    World w;
    for (i32 i = 0; i < 100; ++i)
        w.Create(Health{i});
    CommandBuffer cmd;
    w.Query<const Health>().Each([&](Entity e, const Health& h) {
        if (h.value % 2)
            cmd.Destroy(e);
        else
            cmd.Add(e, Named("even"));
    });
    const Entity spawned = cmd.Create();
    cmd.Add(spawned, Position{7, 7, 7});
    cmd.Add<EnemyTag>(spawned);
    cmd.Remove<EnemyTag>(spawned);
    cmd.Playback(w);
    ZX_EXPECT(cmd.Empty());
    ZX_EXPECT_EQ(w.EntityCount(), 51u);
    ZX_EXPECT_EQ(w.Query<Named>().Count(), 50u);
    ZX_EXPECT_EQ(w.Query<Position>().Count(), 1u);
    ZX_EXPECT_EQ(w.Query<Position>().With<EnemyTag>().Count(), 0u);

    // Unplayed values are destroyed with the buffer.
    {
        CommandBuffer dropped;
        dropped.Add(w.Query<Position>().First(), Named("never applied"));
    }
    ZX_EXPECT_EQ(Named::alive, 50);
}

ZX_TEST(ECS, Observers)
{
    World w;
    i32 added = 0, removed = 0, set = 0, enemies = 0;
    f32 lastSet = 0;
    w.OnAdd<Health>([&](World&, Entity, Health& h) { added += h.value; });
    w.OnRemove<Health>([&](World&, Entity) { ++removed; });
    w.OnSet<Position>([&](World&, Entity, Position& p) {
        ++set;
        lastSet = p.x;
    });
    w.OnAdd<EnemyTag>([&](World&, Entity) { ++enemies; });
    const Entity a = w.Create(Health{5});
    const Entity b = w.Create();
    w.Add(b, Health{10});
    w.Add<EnemyTag>(b);
    w.Set(a, Position{3, 0, 0});
    w.Set(a, Position{4, 0, 0});
    w.Remove<Health>(a);
    w.Destroy(b);
    ZX_EXPECT_EQ(added, 15);
    ZX_EXPECT_EQ(removed, 2);
    ZX_EXPECT_EQ(set, 2);
    ZX_EXPECT_EQ(lastSet, 4.0f);
    ZX_EXPECT_EQ(enemies, 1);
}

ZX_TEST(ECS, Singletons)
{
    World w;
    ZX_EXPECT(w.GetSingleton<Health>() == nullptr);
    w.SetSingleton(Health{77});
    w.SetSingleton(Position{1, 2, 3});
    ZX_EXPECT_EQ(w.GetSingleton<Health>()->value, 77);
    ZX_EXPECT_EQ(w.GetSingleton<Position>()->z, 3.0f);
}

ZX_TEST(ECS, ParallelEach)
{
    JobSystemScope jobsScope;
    World w;
    for (i32 i = 0; i < 100000; ++i)
        w.Create(Position{f32(i % 100), 0, 0}, Velocity{1, 2, 3});
    w.Query<Position, const Velocity>().ParallelEach([](Position& p, const Velocity& v) {
        p.y += v.y;
        p.z += v.z;
    });
    std::atomic<u64> sum{0};
    w.Query<const Position>().ParallelEach([&](const Position& p) { sum.fetch_add(u64(p.x + p.y + p.z), std::memory_order_relaxed); });
    ZX_EXPECT_EQ(sum.load(), u64(4950 * 1000 + 5 * 100000));
}

ZX_TEST(ECS, SchedulerBatches)
{
    JobSystemScope jobsScope;
    World w;
    for (i32 i = 0; i < 1000; ++i)
        w.Create(Position{}, Velocity{1, 0, 0}, Health{});
    SystemScheduler s;
    std::atomic<i32> order{0};
    i32 moveOrder = -1, renderOrder = -1;
    s.Add("Move", SystemPhase::Update, [&](SystemContext& ctx) {
         ctx.world.Query<Position, const Velocity>().Each([](Position& p, const Velocity& v) { p.x += v.x; });
         moveOrder = order++;
     }).Writes<Position>().Reads<Velocity>();
    s.Add("Regen", SystemPhase::Update, [&](SystemContext& ctx) {
         ctx.world.Query<Health>().Each([](Health& h) { h.value += 1; });
     }).Writes<Health>();
    s.Add("Accelerate", SystemPhase::Update, [&](SystemContext& ctx) {
         ctx.world.Query<Velocity>().Each([](Velocity& v) { v.x *= 2; });
     }).Writes<Velocity>();
    s.Add("Render", SystemPhase::Update, [&](SystemContext& ctx) {
         ZX_UNUSED(ctx);
         renderOrder = order++;
     }).Reads<Position>().After("Move");
    s.Add("Spawn", SystemPhase::Update, [&](SystemContext& ctx) {
         ctx.commands.Add(ctx.commands.Create(), Health{1});
     }).Writes<Health>();
    s.Add("Debug", SystemPhase::Update, [&](SystemContext&) {}); // undeclared = exclusive

    const Array<Array<String>> batches = s.GetBatches(SystemPhase::Update);
    // Move | Regen | (Accelerate after Move: writes Velocity which Move reads)
    ZX_REQUIRE(batches.Size() >= 3);
    ZX_EXPECT(batches[0].Size() == 2 && batches[0][0] == "Move" && batches[0][1] == "Regen");
    ZX_EXPECT(batches.Back().Size() == 1 && batches.Back()[0] == "Debug");
    s.Run(w, SystemPhase::Update, 1.0f / 60.0f);
    s.Run(w, SystemPhase::Update, 1.0f / 60.0f);
    ZX_EXPECT(moveOrder >= 0 && renderOrder > moveOrder);
    // Move ran before Accelerate both frames: x = 1 + 2.
    ZX_EXPECT_EQ(w.Get<const Position>(w.Query<Position>().First())->x, 3.0f);
    ZX_EXPECT_EQ(w.Query<Health>().Count(), 1002u); // two spawned through command buffers
    ZX_EXPECT(!s.DumpSchedule().Empty());
}

ZX_TEST(ECS, SystemChangedFilter)
{
    World w;
    for (i32 i = 0; i < 10; ++i)
        w.Create(Position{}, Velocity{});
    SystemScheduler s;
    u32 seen = 0;
    bool write = true;
    s.Add("Writer", SystemPhase::Update, [&](SystemContext& ctx) {
         if (write)
             ctx.world.Query<Position>().Each([](Position& p) { p.x += 1; });
     }).Writes<Position>();
    s.Add("Watcher", SystemPhase::Update, [&](SystemContext& ctx) {
         seen += ctx.world.Query<const Position>().Changed<Position>(ctx.lastRunVersion).Count();
     }).Reads<Position>().After("Writer");
    s.Run(w, SystemPhase::Update, 0.0f);
    ZX_EXPECT_EQ(seen, 10u);
    write = false;
    s.Run(w, SystemPhase::Update, 0.0f);
    ZX_EXPECT_EQ(seen, 10u); // nothing new
    write = true;
    s.Run(w, SystemPhase::Update, 0.0f);
    ZX_EXPECT_EQ(seen, 20u);
}

ZX_TEST(ManualBench, ECSIterate)
{
    World w;
    constexpr u32 N = 1'000'000;
    Stopwatch sw;
    for (u32 i = 0; i < N; ++i)
        w.Create(Position{f32(i), 0, 0}, Velocity{1, 1, 1});
    const f64 createMs = sw.ElapsedMilliseconds();
    sw.Restart();
    w.Query<Position, const Velocity>().Each([](Position& p, const Velocity& v) {
        p.x += v.x;
        p.y += v.y;
        p.z += v.z;
    });
    f64 iterMs = sw.ElapsedMilliseconds();
    for (u32 pass = 0; pass < 5; ++pass) {
        sw.Restart();
        w.Query<Position, const Velocity>().Each([](Position& p, const Velocity& v) {
            p.x += v.x;
            p.y += v.y;
            p.z += v.z;
        });
        iterMs = Min(iterMs, sw.ElapsedMilliseconds());
    }
    sw.Restart();
    Array<Entity> es;
    w.Query<Position>().Each([&](Entity e, Position&) { es.PushBack(e); });
    for (u32 i = 0; i < N; i += 2)
        w.Add<Health>(es[i]);
    const f64 addMs = sw.ElapsedMilliseconds();
    // Baseline: the same loop over plain arrays.
    Array<Position> ps;
    Array<Velocity> vs;
    ps.Resize(N);
    vs.Resize(N, Velocity{1, 1, 1});
    f64 rawMs = 1e9;
    for (u32 pass = 0; pass < 5; ++pass) {
        sw.Restart();
        for (u32 i = 0; i < N; ++i) {
            ps[i].x += vs[i].x;
            ps[i].y += vs[i].y;
            ps[i].z += vs[i].z;
        }
        rawMs = Min(rawMs, sw.ElapsedMilliseconds());
    }
    char line[256];
    const usize n = FormatToBuffer(line, sizeof(line),
                                   "  ECS 1M: create {:.1f} ms, iterate {:.2f} ms ({:.2f} ns/entity; raw arrays {:.2f} ms), add 500k {:.1f} ms\n",
                                   createMs, iterMs, iterMs * 1e6 / N, rawMs, addMs);
    platform::ConsoleWrite(StringView(line, n));
}
