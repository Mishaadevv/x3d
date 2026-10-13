// Randomized tests against simple reference implementations.

#include "Core/Containers/Algorithm.h"
#include "Core/Containers/HashMap.h"
#include "Core/Containers/RingBuffer.h"
#include "Core/Containers/SlotMap.h"
#include "Core/Jobs/JobSystem.h"
#include "Core/Math/Random.h"
#include "Core/Memory/Allocator.h"
#include "Core/String/Name.h"
#include "Core/String/String.h"
#include "Core/Testing/Test.h"
#include "Core/Threading/Thread.h"

#include <atomic>

using namespace zx;

ZX_TEST(Stress, HashMapVsReference)
{
    Random rng(1234);
    HashMap<u32, u32> map;
    // Reference: dense array indexed by key (keys < 2048) with presence flags.
    constexpr u32 KeyRange = 2048;
    Array<u32> refValue;
    refValue.ResizeZeroed(KeyRange);
    BitArray refPresent(KeyRange);
    usize refCount = 0;
    bool ok = true;
    for (int op = 0; op < 200000; ++op) {
        const u32 key = rng.NextBounded(KeyRange);
        switch (rng.NextBounded(4)) {
        case 0:
        case 1: { // insert or assign
            const u32 value = rng.NextU32();
            map.InsertOrAssign(key, value);
            if (!refPresent.Get(key))
                ++refCount;
            refPresent.Set(key);
            refValue[key] = value;
            break;
        }
        case 2: { // remove
            const bool removed = map.Remove(key);
            ok &= removed == refPresent.Get(key);
            if (refPresent.Get(key))
                --refCount;
            refPresent.Reset(key);
            break;
        }
        default: { // lookup
            const u32* v = map.Find(key);
            ok &= (v != nullptr) == refPresent.Get(key);
            if (v)
                ok &= *v == refValue[key];
            break;
        }
        }
        if (!ok)
            break;
    }
    ZX_EXPECT(ok);
    ZX_EXPECT_EQ(map.Size(), refCount);
    usize iterated = 0;
    for (auto& [k, v] : map) {
        ++iterated;
        ZX_EXPECT(refPresent.Get(k) && refValue[k] == v);
    }
    ZX_EXPECT_EQ(iterated, refCount);
}

ZX_TEST(Stress, SlotMapVsReference)
{
    struct Tag;
    Random rng(99);
    SlotMap<u64, Tag> map;
    Array<Pair<Handle<Tag>, u64>> live;
    Array<Handle<Tag>> dead;
    bool ok = true;
    for (int op = 0; op < 50000 && ok; ++op) {
        const u32 action = rng.NextBounded(3);
        if (action < 2 || live.Empty()) {
            const u64 v = rng.NextU64();
            live.PushBack({map.Insert(v), v});
        } else {
            const usize i = rng.NextBounded(u32(live.Size()));
            ok &= map.Remove(live[i].first);
            dead.PushBack(live[i].first);
            live.RemoveAtSwap(i);
        }
    }
    ZX_EXPECT(ok);
    ZX_EXPECT_EQ(map.Size(), live.Size());
    for (const auto& [h, v] : live) {
        const u64* p = map.Get(h);
        ZX_EXPECT(p && *p == v);
    }
    usize staleHits = 0;
    for (const auto& h : dead)
        staleHits += map.Get(h) != nullptr ? 1 : 0;
    ZX_EXPECT_EQ(staleHits, 0u);
}

ZX_TEST(Stress, RingBufferVsArray)
{
    Random rng(7);
    RingBuffer<i32> ring;
    Array<i32> ref; // front at index 0
    bool ok = true;
    for (int op = 0; op < 20000 && ok; ++op) {
        const i32 v = i32(rng.NextU32());
        switch (rng.NextBounded(4)) {
        case 0:
            ring.PushBack(v);
            ref.PushBack(v);
            break;
        case 1:
            ring.PushFront(v);
            ref.Insert(0, v);
            break;
        case 2:
            if (!ref.Empty()) {
                ok &= ring.PopFront() == ref[0];
                ref.RemoveAt(0);
            }
            break;
        default:
            if (!ref.Empty()) {
                ok &= ring.PopBack() == ref.Back();
                ref.PopBack();
            }
            break;
        }
        ok &= ring.Size() == ref.Size();
    }
    ZX_EXPECT(ok);
    for (usize i = 0; i < ref.Size(); ++i)
        ZX_EXPECT_EQ(ring[i], ref[i]);
}

ZX_TEST(Stress, SortMatchesInsertionSort)
{
    Random rng(55);
    for (int trial = 0; trial < 200; ++trial) {
        const u32 n = rng.NextBounded(300);
        Array<i32> a;
        for (u32 i = 0; i < n; ++i)
            a.PushBack(i32(rng.NextBounded(trial % 2 ? 10 : 100000))); // many duplicates on odd trials
        Array<i32> ref = a;
        for (usize i = 1; i < ref.Size(); ++i)
            for (usize j = i; j > 0 && ref[j] < ref[j - 1]; --j)
                swap(ref[j], ref[j - 1]);
        Sort(a);
        ZX_EXPECT(a == ref);
    }
    // Already sorted / reversed inputs (worst cases for naive quicksort).
    Array<i32> big;
    for (i32 i = 0; i < 100000; ++i)
        big.PushBack(100000 - i);
    Sort(big);
    bool sorted = true;
    for (usize i = 1; i < big.Size(); ++i)
        sorted &= big[i - 1] <= big[i];
    ZX_EXPECT(sorted);
}

ZX_TEST(Stress, StringOperationsVsStd)
{
    // Compare String against a char-array model built with the same edits.
    Random rng(3);
    String s;
    Array<char> model;
    bool ok = true;
    for (int op = 0; op < 5000 && ok; ++op) {
        const u32 action = rng.NextBounded(4);
        if (action == 0 || model.Empty()) {
            const char c = char('a' + rng.NextBounded(26));
            const usize n = rng.NextBounded(40);
            String chunk(n, c);
            s.Append(chunk.View());
            for (usize i = 0; i < n; ++i)
                model.PushBack(c);
        } else if (action == 1) {
            const usize pos = rng.NextBounded(u32(model.Size() + 1));
            s.Insert(pos, "XY");
            model.Insert(pos, Span<const char>("XY", 2));
        } else if (action == 2) {
            const usize pos = rng.NextBounded(u32(model.Size()));
            const usize cnt = rng.NextBounded(10);
            s.Erase(pos, cnt);
            model.RemoveAt(pos, Min(cnt, model.Size() - pos));
        } else {
            s.Append(s.View().SubStr(0, Min<usize>(s.Size(), 8))); // self-append
            const usize n = Min<usize>(model.Size(), 8);
            for (usize i = 0; i < n; ++i)
                model.PushBack(model[i]);
        }
        ok &= s.Size() == model.Size() && MemCompare(s.Data(), model.Data(), model.Size()) == 0 &&
              s.CStr()[s.Size()] == '\0';
    }
    ZX_EXPECT(ok);
}

ZX_TEST(Stress, NameInterningFromManyThreads)
{
    Thread threads[4];
    std::atomic<bool> ok{true};
    for (int t = 0; t < 4; ++t) {
        threads[t].Start("NameStress", [&ok, t] {
            for (int i = 0; i < 2000; ++i) {
                const String text = String::Format("StressName_{}", (i * 7 + t) % 1500);
                const Name n(text);
                if (n.View() != text.View() || Name(text) != n)
                    ok = false;
            }
        });
    }
    for (Thread& th : threads)
        th.Join();
    ZX_EXPECT(ok.load());
    ZX_EXPECT(Name("StressName_0") == Name::Find("StressName_0"));
}

ZX_TEST(Stress, JobPrioritiesAndManyDependencies)
{
    jobs::Initialize({.workerCount = 3});
    // Chain of 500 jobs, each depending on the previous one.
    std::atomic<u32> counter{0};
    std::atomic<bool> order{true};
    JobHandle prev;
    for (u32 i = 0; i < 500; ++i) {
        const JobHandle deps[] = {prev};
        prev = jobs::Schedule([&counter, &order, i] {
            if (counter.fetch_add(1) != i)
                order = false;
        }, prev.IsValid() ? Span<const JobHandle>(deps) : Span<const JobHandle>(), JobPriority(i % 3));
    }
    jobs::Wait(prev);
    ZX_EXPECT(order.load());
    ZX_EXPECT_EQ(counter.load(), 500u);

    // Fan-in: one job depending on 200 others.
    std::atomic<u32> done{0};
    Array<JobHandle> many;
    for (int i = 0; i < 200; ++i)
        many.PushBack(jobs::Schedule([&done] { done.fetch_add(1); }, {}, JobPriority::Low));
    std::atomic<u32> seenAtJoin{0};
    const JobHandle join = jobs::Schedule([&] { seenAtJoin = done.load(); },
                                          Span<const JobHandle>(many.Data(), many.Size()), JobPriority::High);
    jobs::Wait(join);
    ZX_EXPECT_EQ(seenAtJoin.load(), 200u);
    jobs::Shutdown();
}

ZX_TEST(Stress, LinearAllocatorManyBlocks)
{
    LinearAllocator a(4096);
    Random rng(17);
    Array<Pair<u8*, usize>> allocs;
    for (int i = 0; i < 5000; ++i) {
        const usize size = 1 + rng.NextBounded(700);
        const usize align = usize(1) << rng.NextBounded(7);
        u8* p = static_cast<u8*>(a.Allocate(size, align));
        ZX_REQUIRE(reinterpret_cast<uptr>(p) % align == 0);
        MemSet(p, u8(i), size);
        allocs.PushBack({p, size});
    }
    bool intact = true;
    for (usize i = 0; i < allocs.Size(); ++i)
        for (usize k = 0; k < allocs[i].second; ++k)
            intact &= allocs[i].first[k] == u8(i);
    ZX_EXPECT(intact);
    a.Reset(false);
    ZX_EXPECT_EQ(a.BytesUsed(), 0u);
}
