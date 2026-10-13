#include "Core/Jobs/JobSystem.h"
#include "Core/Testing/Test.h"
#include "Core/Threading/Thread.h"

#include <atomic>

using namespace zx;

namespace {
struct JobSystemScope
{
    JobSystemScope() { jobs::Initialize({.workerCount = 4}); }
    ~JobSystemScope() { jobs::Shutdown(); }
};
} // namespace

ZX_TEST(Jobs, ScheduleAndWait)
{
    JobSystemScope js;
    std::atomic<int> counter{0};
    Array<JobHandle> handles;
    for (int i = 0; i < 1000; ++i)
        handles.PushBack(jobs::Schedule([&] { counter.fetch_add(1); }));
    jobs::WaitAll(Span<const JobHandle>(handles.Data(), handles.Size()));
    ZX_EXPECT_EQ(counter.load(), 1000);
    for (const JobHandle& h : handles)
        ZX_EXPECT(jobs::IsCompleted(h));
}

ZX_TEST(Jobs, Dependencies)
{
    JobSystemScope js;
    for (int iteration = 0; iteration < 50; ++iteration) {
        std::atomic<int> stage{0};
        std::atomic<bool> orderOk{true};
        JobHandle a = jobs::Schedule([&] {
            Thread::Yield();
            stage.fetch_add(1);
        });
        JobHandle b = jobs::Schedule([&] { stage.fetch_add(1); });
        const JobHandle ab[] = {a, b};
        JobHandle c = jobs::Schedule([&] {
            if (stage.load() != 2)
                orderOk = false;
            stage.fetch_add(10);
        }, ab);
        const JobHandle cs[] = {c};
        JobHandle d = jobs::Schedule([&] {
            if (stage.load() != 12)
                orderOk = false;
        }, cs);
        jobs::Wait(d);
        ZX_EXPECT(orderOk.load());
        ZX_EXPECT_EQ(stage.load(), 12);
    }
}

ZX_TEST(Jobs, ParallelForCoversRangeOnce)
{
    JobSystemScope js;
    constexpr u32 N = 100000;
    Array<u8> hits;
    hits.ResizeZeroed(N);
    jobs::ParallelFor(N, 64, [&](u32 begin, u32 end) {
        for (u32 i = begin; i < end; ++i)
            hits[i]++;
    });
    bool allOnce = true;
    for (u8 h : hits)
        allOnce &= (h == 1);
    ZX_EXPECT(allOnce);

    // Nested parallel-for inside jobs (waiting threads must execute work to avoid deadlock).
    std::atomic<u64> sum{0};
    jobs::ParallelFor(8, 1, [&](u32 b, u32 e) {
        for (u32 i = b; i < e; ++i)
            jobs::ParallelFor(1000, 10, [&](u32 b2, u32 e2) { sum.fetch_add(e2 - b2); });
    });
    ZX_EXPECT_EQ(sum.load(), 8000u);
}

ZX_TEST(Jobs, ScheduleFromJobs)
{
    JobSystemScope js;
    std::atomic<int> leaves{0};
    JobHandle root = jobs::Schedule([&] {
        SmallArray<JobHandle, 16> children;
        for (int i = 0; i < 16; ++i)
            children.PushBack(jobs::Schedule([&] { leaves.fetch_add(1); }));
        jobs::WaitAll(Span<const JobHandle>(children.Data(), children.Size()));
    });
    jobs::Wait(root);
    ZX_EXPECT_EQ(leaves.load(), 16);
    ZX_EXPECT(jobs::GetStats().jobsExecuted >= 17u);
}
