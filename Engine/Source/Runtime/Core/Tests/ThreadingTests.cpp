#include "Core/Testing/Test.h"
#include "Core/Threading/Mutex.h"
#include "Core/Threading/Thread.h"
#include "Core/Time/Time.h"

using namespace zx;

ZX_TEST(Threading, ThreadsAndMutex)
{
    Mutex mutex;
    i64 counter = 0;
    Thread threads[4];
    for (int t = 0; t < 4; ++t) {
        ZX_REQUIRE(threads[t].Start(String::Format("Worker{}", t), [&] {
            for (int i = 0; i < 10000; ++i) {
                ZX_SCOPED_LOCK(mutex);
                ++counter;
            }
        }));
    }
    for (Thread& t : threads)
        t.Join();
    ZX_EXPECT_EQ(counter, 40000);
}

ZX_TEST(Threading, SpinLockAndAtomics)
{
    SpinLock lock;
    std::atomic<i32> atomicCounter{0};
    i32 guarded = 0;
    Thread threads[4];
    for (Thread& th : threads) {
        th.Start("Spin", [&] {
            for (int i = 0; i < 5000; ++i) {
                atomicCounter.fetch_add(1);
                ScopedLock<SpinLock> guard(lock);
                ++guarded;
            }
        });
    }
    for (Thread& th : threads)
        th.Join();
    ZX_EXPECT_EQ(atomicCounter.load(), 20000);
    ZX_EXPECT_EQ(guarded, 20000);
}

ZX_TEST(Threading, ConditionVariableAndEvent)
{
    Mutex m;
    ConditionVariable cv;
    bool ready = false;
    int observed = 0;
    Thread waiter;
    waiter.Start("Waiter", [&] {
        m.Lock();
        while (!ready)
            cv.Wait(m);
        observed = 1;
        m.Unlock();
    });
    Thread::SleepMs(5);
    m.Lock();
    ready = true;
    m.Unlock();
    cv.NotifyAll();
    waiter.Join();
    ZX_EXPECT_EQ(observed, 1);

    Event ev(true);
    ZX_EXPECT(!ev.Wait(1));
    ev.Signal();
    ZX_EXPECT(ev.Wait(0));

    Semaphore sem(0);
    Thread producer;
    producer.Start("Producer", [&] { sem.Release(3); });
    ZX_EXPECT(sem.Acquire(1000));
    ZX_EXPECT(sem.Acquire(1000));
    ZX_EXPECT(sem.Acquire(1000));
    producer.Join();
}

ZX_TEST(Time, ClockAndStopwatch)
{
    Stopwatch sw;
    Thread::SleepMs(10);
    const f64 ms = sw.ElapsedMilliseconds();
    ZX_EXPECT(ms >= 9.0 && ms < 500.0);
    const DateTime now = DateTime::Now();
    ZX_EXPECT(now.year >= 2024);
    ZX_EXPECT(DateTime::UnixTimeNow() > 1700000000);
    FrameTimer ft;
    ft.Tick();
    ft.SetTimeScale(0.5f);
    Thread::SleepMs(2);
    ft.Tick();
    ZX_EXPECT(ft.DeltaSeconds() > 0.0f);
    ZX_EXPECT_NEAR(ft.DeltaSeconds(), ft.UnscaledDeltaSeconds() * 0.5f, 1e-6);
}
