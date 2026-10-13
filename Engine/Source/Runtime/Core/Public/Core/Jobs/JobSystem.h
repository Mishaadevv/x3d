#pragma once

// Job system: a pool of worker threads executing small tasks with dependencies.
//
//   JobHandle a = jobs::Schedule([] { LoadA(); });
//   JobHandle b = jobs::Schedule([] { LoadB(); });
//   JobHandle c = jobs::Schedule([] { Combine(); }, {a, b});   // runs after a and b
//   jobs::ParallelFor(count, 64, [&](u32 begin, u32 end) { ... });  // blocking, caller participates
//   jobs::Wait(c);                                               // waiting thread executes other jobs
//
// Handles are generational: a handle to a finished job stays "completed" forever.

#include "Core/Containers/Function.h"
#include "Core/Containers/Span.h"

namespace zx {

enum class JobPriority : u8
{
    High,
    Normal,
    Low,
    Count
};

struct JobHandle
{
    u32 index = InvalidIndex;
    u32 generation = 0;
    bool IsValid() const { return index != InvalidIndex; }
};

namespace jobs {

struct Settings
{
    u32 workerCount = 0; // 0 = logical cores - 1 (min 1)
};

void Initialize(const Settings& settings = {});
void Shutdown();
bool IsInitialized();

u32 GetWorkerCount();
/// 0 = main thread, 1..N = workers, -1 = other threads.
i32 GetCurrentWorkerIndex();

using JobFunction = Function<void(), 48>;

JobHandle Schedule(JobFunction fn, Span<const JobHandle> dependencies = {},
                   JobPriority priority = JobPriority::Normal);

/// Schedules batches of [begin, end) ranges. Returned handle completes when all batches are done.
JobHandle ScheduleParallelFor(u32 count, u32 minBatchSize, Function<void(u32 begin, u32 end), 48> fn,
                              Span<const JobHandle> dependencies = {}, JobPriority priority = JobPriority::Normal);

/// Blocking parallel loop; the calling thread executes batches too.
void ParallelFor(u32 count, u32 minBatchSize, Function<void(u32 begin, u32 end), 48> fn);

/// Returns a handle that completes when all given handles complete.
JobHandle CombineDependencies(Span<const JobHandle> handles);

bool IsCompleted(JobHandle handle);

/// Blocks until complete; the waiting thread executes queued jobs meanwhile.
void Wait(JobHandle handle);
void WaitAll(Span<const JobHandle> handles);

struct Stats
{
    u64 jobsExecuted = 0;
    u32 jobsInFlight = 0;
    u32 peakJobsInFlight = 0;
};
Stats GetStats();

} // namespace jobs
} // namespace zx
