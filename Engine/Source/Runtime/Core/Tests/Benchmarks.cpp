// Micro-benchmarks (run explicitly: ZXTests "ManualBench.*", preferably in Development).

#include "Core/Containers/HashMap.h"
#include "Core/HAL/PlatformMisc.h"
#include "Core/Jobs/JobSystem.h"
#include "Core/Math/Random.h"
#include "Core/Testing/Test.h"
#include "Core/Time/Time.h"

#include <atomic>

using namespace zx;

namespace {
void Report(StringView name, f64 ms, u64 ops)
{
    char line[256];
    const usize n = FormatToBuffer(line, sizeof(line), "  {:<40} {:9.2f} ms  {:8.1f} ns/op\n", name, ms,
                                   ms * 1e6 / f64(ops));
    platform::ConsoleWrite(StringView(line, n));
}
} // namespace

ZX_TEST(ManualBench, HashMap)
{
    constexpr u32 N = 1'000'000;
    Array<u32> keys;
    keys.ResizeUninitialized(N);
    Random rng(1);
    for (u32& k : keys)
        k = rng.NextU32();

    HashMap<u32, u32> map;
    Stopwatch sw;
    for (u32 i = 0; i < N; ++i)
        map.InsertOrAssign(keys[i], i);
    Report("HashMap<u32,u32> insert 1M (no reserve)", sw.ElapsedMilliseconds(), N);

    sw.Restart();
    u64 found = 0;
    for (u32 i = 0; i < N; ++i)
        found += map.Find(keys[i]) != nullptr;
    Report("HashMap<u32,u32> find hit 1M", sw.ElapsedMilliseconds(), N);
    ZX_EXPECT_EQ(found, u64(map.Size()) == N ? N : found);

    sw.Restart();
    u64 misses = 0;
    for (u32 i = 0; i < N; ++i)
        misses += map.Find(keys[i] ^ 0x5bd1e995u) == nullptr;
    Report("HashMap<u32,u32> find miss 1M", sw.ElapsedMilliseconds(), N);

    HashMap<String, u32> strings;
    Array<String> names;
    for (u32 i = 0; i < 100000; ++i)
        names.PushBack(String::Format("Entity_{}_Mesh", i));
    sw.Restart();
    for (u32 i = 0; i < names.Size(); ++i)
        strings.Insert(names[i], i);
    Report("HashMap<String,u32> insert 100k", sw.ElapsedMilliseconds(), names.Size());
    sw.Restart();
    for (const String& s : names)
        found += strings.Find(s.View()) != nullptr;
    Report("HashMap<String,u32> find 100k (StringView)", sw.ElapsedMilliseconds(), names.Size());
}

ZX_TEST(ManualBench, JobSystem)
{
    jobs::Initialize();
    constexpr u32 Jobs = 50000;
    std::atomic<u32> counter{0};
    Array<JobHandle> handles;
    handles.Reserve(Jobs);
    Stopwatch sw;
    for (u32 i = 0; i < Jobs; i += 1000) {
        handles.Clear();
        for (u32 k = 0; k < 1000; ++k)
            handles.PushBack(jobs::Schedule([&counter] { counter.fetch_add(1, std::memory_order_relaxed); }));
        jobs::WaitAll(Span<const JobHandle>(handles.Data(), handles.Size()));
    }
    Report("Schedule+run empty jobs (batches of 1000)", sw.ElapsedMilliseconds(), Jobs);
    ZX_EXPECT_EQ(counter.load(), Jobs);

    constexpr u32 Elements = 4'000'000;
    Array<f32> data;
    data.ResizeUninitialized(Elements);
    sw.Restart();
    jobs::ParallelFor(Elements, 16384, [&](u32 b, u32 e) {
        for (u32 i = b; i < e; ++i)
            data[i] = Sqrt(f32(i)) * 0.5f;
    });
    const f64 parallelMs = sw.ElapsedMilliseconds();
    sw.Restart();
    for (u32 i = 0; i < Elements; ++i)
        data[i] = Sqrt(f32(i)) * 0.5f;
    const f64 serialMs = sw.ElapsedMilliseconds();
    Report("ParallelFor 4M sqrt", parallelMs, Elements);
    Report("Serial 4M sqrt", serialMs, Elements);
    char line[128];
    const usize n = FormatToBuffer(line, sizeof(line), "  speedup x{:.2f} on {} workers + main\n", serialMs / parallelMs,
                                   jobs::GetWorkerCount());
    platform::ConsoleWrite(StringView(line, n));
    jobs::Shutdown();
}
