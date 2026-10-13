#include "Core/Jobs/JobSystem.h"
#include "Core/Containers/RingBuffer.h"
#include "Core/Threading/Mutex.h"
#include "Core/Threading/Thread.h"

#include <atomic>

namespace zx::jobs {

namespace {

constexpr u32 MaxJobs = 16 * 1024;

struct Job
{
    JobFunction fn;
    std::atomic<u32> generation{1};
    std::atomic<i32> pendingDeps{0};
    std::atomic<bool> done{false};
    SpinLock lock; // guards continuations + the done transition
    SmallArray<u32, 4> continuations;
    JobPriority priority = JobPriority::Normal;
};

struct State
{
    Job* jobs = nullptr;

    SpinLock freeLock;
    Array<u32> freeList;

    Mutex queueMutex;
    ConditionVariable queueCv;
    RingBuffer<u32> queues[u32(JobPriority::Count)];
    u32 queuedCount = 0;
    bool quit = false;

    Array<Thread> workers;
    std::atomic<u32> completionEpoch{0};
    std::atomic<u32> waitersCount{0};

    std::atomic<u64> executed{0};
    std::atomic<u32> inFlight{0};
    std::atomic<u32> peakInFlight{0};
    bool initialized = false;
};

State* g_state = nullptr;
thread_local i32 t_workerIndex = -1;

u32 AllocateJob()
{
    State& s = *g_state;
    for (;;) {
        {
            ScopedLock<SpinLock> guard(s.freeLock);
            if (!s.freeList.Empty())
                return s.freeList.PopBackValue();
        }
        // Pool exhausted: help drain work, then retry.
        ZX_ASSERT(t_workerIndex >= 0, "Job pool exhausted ({} jobs) on a non-job thread", MaxJobs);
        Thread::Yield();
    }
}

void Enqueue(u32 index)
{
    State& s = *g_state;
    {
        ScopedLock<Mutex> guard(s.queueMutex);
        s.queues[u32(s.jobs[index].priority)].PushBack(index);
        ++s.queuedCount;
    }
    s.queueCv.NotifyOne();
}

bool TryDequeue(u32& out)
{
    State& s = *g_state;
    ScopedLock<Mutex> guard(s.queueMutex);
    for (auto& q : s.queues) {
        if (!q.Empty()) {
            out = q.PopFront();
            --s.queuedCount;
            return true;
        }
    }
    return false;
}

void Execute(u32 index)
{
    State& s = *g_state;
    Job& job = s.jobs[index];
    if (job.fn)
        job.fn();
    job.fn = nullptr;

    SmallArray<u32, 4> continuations;
    {
        ScopedLock<SpinLock> guard(job.lock);
        job.done.store(true, std::memory_order_release);
        continuations = move(job.continuations);
        job.continuations.Clear();
    }
    for (u32 c : continuations) {
        if (s.jobs[c].pendingDeps.fetch_sub(1, std::memory_order_acq_rel) == 1)
            Enqueue(c);
    }

    // Retire the slot: bump generation so outstanding handles read as completed.
    job.generation.fetch_add(1, std::memory_order_acq_rel);
    {
        ScopedLock<SpinLock> guard(s.freeLock);
        s.freeList.PushBack(index);
    }
    s.executed.fetch_add(1, std::memory_order_relaxed);
    s.inFlight.fetch_sub(1, std::memory_order_relaxed);

    s.completionEpoch.fetch_add(1, std::memory_order_release);
    if (s.waitersCount.load(std::memory_order_acquire) > 0)
        AtomicWakeAll(s.completionEpoch);
}

void WorkerMain(i32 workerIndex)
{
    t_workerIndex = workerIndex;
    State& s = *g_state;
    for (;;) {
        u32 index = InvalidIndex;
        {
            ScopedLock<Mutex> guard(s.queueMutex);
            while (s.queuedCount == 0 && !s.quit)
                s.queueCv.Wait(s.queueMutex);
            if (s.quit && s.queuedCount == 0)
                return;
            for (auto& q : s.queues) {
                if (!q.Empty()) {
                    index = q.PopFront();
                    --s.queuedCount;
                    break;
                }
            }
        }
        if (index != InvalidIndex)
            Execute(index);
    }
}

} // namespace

void Initialize(const Settings& settings)
{
    ZX_ASSERT(!g_state, "JobSystem already initialized");
    g_state = New<State>();
    State& s = *g_state;
    s.jobs = static_cast<Job*>(Malloc(sizeof(Job) * MaxJobs, 64, MemTag::Jobs));
    for (u32 i = 0; i < MaxJobs; ++i)
        ::new (&s.jobs[i]) Job();
    s.freeList.Reserve(MaxJobs);
    for (u32 i = MaxJobs; i > 0; --i)
        s.freeList.PushBack(i - 1);

    u32 count = settings.workerCount;
    if (count == 0) {
        const u32 cores = Thread::HardwareConcurrency();
        count = cores > 1 ? cores - 1 : 1;
    }
    t_workerIndex = 0; // the initializing thread is the "main" participant
    s.workers.Resize(count);
    for (u32 i = 0; i < count; ++i) {
        const i32 workerIndex = i32(i + 1);
        s.workers[i].Start(String::Format("ZX Worker {}", workerIndex), [workerIndex] { WorkerMain(workerIndex); },
                           ThreadPriority::Normal);
    }
    s.initialized = true;
}

void Shutdown()
{
    if (!g_state)
        return;
    State& s = *g_state;
    {
        ScopedLock<Mutex> guard(s.queueMutex);
        s.quit = true;
    }
    s.queueCv.NotifyAll();
    for (Thread& t : s.workers)
        t.Join();
    s.workers.Reset();
    for (u32 i = 0; i < MaxJobs; ++i)
        s.jobs[i].~Job();
    Free(s.jobs);
    Delete(g_state);
    g_state = nullptr;
    t_workerIndex = -1;
}

bool IsInitialized() { return g_state != nullptr; }
u32 GetWorkerCount() { return g_state ? u32(g_state->workers.Size()) : 0; }
i32 GetCurrentWorkerIndex() { return t_workerIndex; }

JobHandle Schedule(JobFunction fn, Span<const JobHandle> dependencies, JobPriority priority)
{
    ZX_ASSERT(g_state, "JobSystem not initialized");
    State& s = *g_state;
    const u32 index = AllocateJob();
    Job& job = s.jobs[index];
    job.fn = move(fn);
    job.priority = priority;
    job.done.store(false, std::memory_order_relaxed);
    job.continuations.Clear();
    // +1 guard so the job can't start while dependencies are still being registered.
    job.pendingDeps.store(1, std::memory_order_relaxed);
    const JobHandle handle{index, job.generation.load(std::memory_order_acquire)};

    const u32 now = s.inFlight.fetch_add(1, std::memory_order_relaxed) + 1;
    u32 peak = s.peakInFlight.load(std::memory_order_relaxed);
    while (now > peak && !s.peakInFlight.compare_exchange_weak(peak, now)) {
    }

    for (const JobHandle& dep : dependencies) {
        if (!dep.IsValid())
            continue;
        Job& d = s.jobs[dep.index];
        ScopedLock<SpinLock> guard(d.lock);
        if (d.generation.load(std::memory_order_acquire) == dep.generation && !d.done.load(std::memory_order_acquire)) {
            d.continuations.PushBack(index);
            job.pendingDeps.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (job.pendingDeps.fetch_sub(1, std::memory_order_acq_rel) == 1)
        Enqueue(index);
    return handle;
}

JobHandle CombineDependencies(Span<const JobHandle> handles) { return Schedule(nullptr, handles, JobPriority::High); }

JobHandle ScheduleParallelFor(u32 count, u32 minBatchSize, Function<void(u32, u32), 48> fn,
                              Span<const JobHandle> dependencies, JobPriority priority)
{
    if (count == 0)
        return Schedule(nullptr, dependencies, priority);
    const u32 workers = GetWorkerCount() + 1;
    u32 batch = minBatchSize > 0 ? minBatchSize : 1;
    const u32 targetBatches = workers * 4;
    if ((count + batch - 1) / batch > targetBatches)
        batch = (count + targetBatches - 1) / targetBatches;

    // The callable is shared by all batches; the last batch to finish frees it.
    auto* shared = New<Function<void(u32, u32), 48>>(move(fn));
    std::atomic<u32>* remaining = New<std::atomic<u32>>((count + batch - 1) / batch);

    SmallArray<JobHandle, 64> batchHandles;
    for (u32 begin = 0; begin < count; begin += batch) {
        const u32 end = begin + batch < count ? begin + batch : count;
        batchHandles.PushBack(Schedule(
            [shared, remaining, begin, end] {
                (*shared)(begin, end);
                if (remaining->fetch_sub(1, std::memory_order_acq_rel) == 1) {
                    Delete(shared);
                    Delete(remaining);
                }
            },
            dependencies, priority));
    }
    return CombineDependencies(Span<const JobHandle>(batchHandles.Data(), batchHandles.Size()));
}

void ParallelFor(u32 count, u32 minBatchSize, Function<void(u32, u32), 48> fn)
{
    if (count == 0)
        return;
    if (!g_state || count <= minBatchSize) {
        fn(0, count);
        return;
    }
    Wait(ScheduleParallelFor(count, minBatchSize, move(fn)));
}

bool IsCompleted(JobHandle handle)
{
    if (!handle.IsValid() || !g_state)
        return true;
    const Job& job = g_state->jobs[handle.index];
    return job.generation.load(std::memory_order_acquire) != handle.generation ||
           job.done.load(std::memory_order_acquire);
}

void Wait(JobHandle handle)
{
    if (IsCompleted(handle))
        return;
    State& s = *g_state;
    u32 idleSpins = 0;
    while (!IsCompleted(handle)) {
        u32 index;
        if (TryDequeue(index)) {
            Execute(index);
            idleSpins = 0;
            continue;
        }
        if (++idleSpins < 32) {
            CpuPause();
            continue;
        }
        const u32 epoch = s.completionEpoch.load(std::memory_order_acquire);
        if (IsCompleted(handle))
            break;
        s.waitersCount.fetch_add(1, std::memory_order_acq_rel);
        AtomicWait(s.completionEpoch, epoch, 2);
        s.waitersCount.fetch_sub(1, std::memory_order_acq_rel);
    }
}

void WaitAll(Span<const JobHandle> handles)
{
    for (const JobHandle& h : handles)
        Wait(h);
}

Stats GetStats()
{
    Stats st;
    if (g_state) {
        st.jobsExecuted = g_state->executed.load();
        st.jobsInFlight = g_state->inFlight.load();
        st.peakJobsInFlight = g_state->peakInFlight.load();
    }
    return st;
}

} // namespace zx::jobs
