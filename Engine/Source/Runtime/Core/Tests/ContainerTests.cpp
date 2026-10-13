#include "Core/Containers/Algorithm.h"
#include "Core/Containers/Array.h"
#include "Core/Containers/Function.h"
#include "Core/Containers/HashMap.h"
#include "Core/Containers/Optional.h"
#include "Core/Containers/RingBuffer.h"
#include "Core/Containers/SlotMap.h"
#include "Core/Memory/Pointers.h"
#include "Core/String/String.h"
#include "Core/Testing/Test.h"

using namespace zx;

namespace {
struct Tracked
{
    static inline int alive = 0;
    int value;
    Tracked(int v = 0) : value(v) { ++alive; }
    Tracked(const Tracked& o) : value(o.value) { ++alive; }
    Tracked(Tracked&& o) noexcept : value(o.value) { ++alive; }
    Tracked& operator=(const Tracked&) = default;
    Tracked& operator=(Tracked&&) = default;
    ~Tracked() { --alive; }
    bool operator==(const Tracked& o) const { return value == o.value; }
};
} // namespace

ZX_TEST(Array, Basics)
{
    Array<int> a;
    ZX_EXPECT(a.Empty());
    for (int i = 0; i < 100; ++i)
        a.PushBack(i);
    ZX_EXPECT_EQ(a.Size(), 100u);
    ZX_EXPECT_EQ(a[42], 42);
    ZX_EXPECT_EQ(a.Back(), 99);
    a.RemoveAt(0);
    ZX_EXPECT_EQ(a[0], 1);
    a.RemoveAtSwap(0);
    ZX_EXPECT_EQ(a[0], 99);
    a.Insert(1, 1000);
    ZX_EXPECT_EQ(a[1], 1000);
    ZX_EXPECT_EQ(a.Find(1000), 1u);
    ZX_EXPECT(a.Remove(1000));
    ZX_EXPECT(!a.Contains(1000));
    const usize removed = a.RemoveIf([](int v) { return v % 2 == 0; });
    ZX_EXPECT(removed > 0);
    for (int v : a)
        ZX_EXPECT(v % 2 == 1);
    Array<int> b = a;
    ZX_EXPECT(a == b);
    Array<int> c = move(b);
    ZX_EXPECT(b.Empty());
    ZX_EXPECT(a == c);
    a.Clear();
    ZX_EXPECT(a.Empty());
}

ZX_TEST(Array, SelfAliasPushBack)
{
    Array<String> a;
    a.PushBack("a string long enough to live on the heap ...");
    for (int i = 0; i < 20; ++i)
        a.PushBack(a[0]); // forces reallocation while referencing an element
    ZX_EXPECT(a.Back() == a[0]);
    Array<int> b = {1, 2, 3};
    b.Append(b.View());
    ZX_EXPECT_EQ(b.Size(), 6u);
    ZX_EXPECT_EQ(b[5], 3);
}

ZX_TEST(Array, ObjectLifetimes)
{
    {
        Array<Tracked> a;
        for (int i = 0; i < 50; ++i)
            a.EmplaceBack(i);
        ZX_EXPECT_EQ(Tracked::alive, 50);
        a.RemoveAt(10, 5);
        ZX_EXPECT_EQ(Tracked::alive, 45);
        a.Insert(3, Tracked(7));
        ZX_EXPECT_EQ(Tracked::alive, 46);
        a.Resize(10);
        ZX_EXPECT_EQ(Tracked::alive, 10);
        a.Resize(20, Tracked(1));
        ZX_EXPECT_EQ(Tracked::alive, 20);
    }
    ZX_EXPECT_EQ(Tracked::alive, 0);
}

ZX_TEST(Array, SmallArrayInlineStorage)
{
    SmallArray<int, 4> s;
    s.PushBack(1);
    s.PushBack(2);
    const int* inlinePtr = s.Data();
    s.PushBack(3);
    s.PushBack(4);
    ZX_EXPECT(s.Data() == inlinePtr);
    s.PushBack(5); // spills to heap
    ZX_EXPECT(s.Data() != inlinePtr);
    ZX_EXPECT_EQ(s[4], 5);
    SmallArray<int, 4> moved = move(s);
    ZX_EXPECT_EQ(moved.Size(), 5u);
    SmallArray<Tracked, 2> t;
    t.EmplaceBack(1);
    SmallArray<Tracked, 2> t2 = move(t); // inline move relocates
    ZX_EXPECT_EQ(t2[0].value, 1);
}

ZX_TEST(Algorithm, Sort)
{
    Array<int> a;
    u32 seed = 7;
    for (int i = 0; i < 5000; ++i) {
        seed = seed * 1103515245u + 12345u;
        a.PushBack(int(seed % 1000));
    }
    Sort(a);
    for (usize i = 1; i < a.Size(); ++i)
        ZX_EXPECT(a[i - 1] <= a[i]);
    Sort(a, GreaterOp());
    for (usize i = 1; i < a.Size(); ++i)
        ZX_EXPECT(a[i - 1] >= a[i]);

    struct KV
    {
        int key;
        int order;
    };
    Array<KV> kv;
    for (int i = 0; i < 1000; ++i)
        kv.PushBack(KV{i % 10, i});
    StableSort(kv, [](const KV& x, const KV& y) { return x.key < y.key; });
    for (usize i = 1; i < kv.Size(); ++i) {
        ZX_EXPECT(kv[i - 1].key <= kv[i].key);
        if (kv[i - 1].key == kv[i].key)
            ZX_EXPECT(kv[i - 1].order < kv[i].order);
    }

    Array<int> sorted = {1, 3, 3, 5, 9};
    ZX_EXPECT_EQ(LowerBound(Span<const int>(sorted.View()), 3), 1u);
    ZX_EXPECT_EQ(UpperBound(Span<const int>(sorted.View()), 3), 3u);
    ZX_EXPECT_EQ(BinarySearch(Span<const int>(sorted.View()), 9), 4u);
    ZX_EXPECT_EQ(BinarySearch(Span<const int>(sorted.View()), 4), NPos);
}

ZX_TEST(HashMap, Basics)
{
    HashMap<int, int> m;
    for (int i = 0; i < 1000; ++i)
        m.Insert(i, i * 10);
    ZX_EXPECT_EQ(m.Size(), 1000u);
    for (int i = 0; i < 1000; ++i) {
        const int* v = m.Find(i);
        ZX_REQUIRE(v);
        ZX_EXPECT_EQ(*v, i * 10);
    }
    ZX_EXPECT(m.Find(5000) == nullptr);
    ZX_EXPECT(!m.Insert(5, 0).second); // no overwrite
    m.InsertOrAssign(5, 55);
    ZX_EXPECT_EQ(m.At(5), 55);
    for (int i = 0; i < 1000; i += 2)
        ZX_EXPECT(m.Remove(i));
    ZX_EXPECT_EQ(m.Size(), 500u);
    for (int i = 0; i < 1000; ++i)
        ZX_EXPECT_EQ(m.Contains(i), i % 2 == 1);
    int sum = 0;
    for (auto& [k, v] : m)
        sum += (v == k * 10 || (k == 5 && v == 55)) ? 1 : 0;
    ZX_EXPECT_EQ(sum, 500);
    m[7] += 1;
    ZX_EXPECT_EQ(m.At(7), 71);
    m[2000] = 1;
    ZX_EXPECT_EQ(m.Size(), 501u);
    m.RemoveIf([](int k, int) { return k > 100; });
    for (auto& e : m)
        ZX_EXPECT(e.first <= 100);
}

ZX_TEST(HashMap, StringKeysHeterogeneousLookup)
{
    HashMap<String, int> m;
    m.Insert(String("alpha"), 1);
    m.Insert(String("beta"), 2);
    m["gamma"] = 3;
    ZX_EXPECT_EQ(*m.Find(StringView("alpha")), 1);
    ZX_EXPECT_EQ(*m.Find("beta"), 2);
    ZX_EXPECT_EQ(m.At(String("gamma")), 3);
    HashMap<String, int> copy = m;
    ZX_EXPECT_EQ(copy.Size(), 3u);
    ZX_EXPECT(copy.Remove("beta"));
    ZX_EXPECT(m.Contains("beta"));

    HashSet<String> set = {"a", "b", "c"};
    ZX_EXPECT(set.Contains("b"));
    ZX_EXPECT(!set.Insert("a"));
    ZX_EXPECT(set.Remove("a"));
    ZX_EXPECT_EQ(set.Size(), 2u);
}

ZX_TEST(HashMap, ObjectLifetimes)
{
    {
        HashMap<int, Tracked> m;
        for (int i = 0; i < 200; ++i)
            m.Insert(i, Tracked(i));
        for (int i = 0; i < 100; ++i)
            m.Remove(i);
        ZX_EXPECT_EQ(Tracked::alive, 100);
    }
    ZX_EXPECT_EQ(Tracked::alive, 0);
}

ZX_TEST(SlotMap, HandlesAndGenerations)
{
    struct Tag;
    SlotMap<int, Tag> map;
    auto a = map.Insert(1);
    auto b = map.Insert(2);
    auto c = map.Insert(3);
    ZX_EXPECT_EQ(map[b], 2);
    ZX_EXPECT(map.Remove(a));
    ZX_EXPECT(!map.Contains(a));
    ZX_EXPECT(map.Get(a) == nullptr);
    ZX_EXPECT_EQ(map[c], 3); // moved into a's dense slot, handle still valid
    auto d = map.Insert(4);  // reuses a's slot with new generation
    ZX_EXPECT_EQ(d.index, a.index);
    ZX_EXPECT(d.generation != a.generation);
    ZX_EXPECT(map.Get(a) == nullptr);
    ZX_EXPECT_EQ(map.Size(), 3u);
    int sum = 0;
    for (int v : map)
        sum += v;
    ZX_EXPECT_EQ(sum, 9);
}

ZX_TEST(RingBuffer, Deque)
{
    RingBuffer<int> r;
    for (int i = 0; i < 20; ++i)
        r.PushBack(i);
    for (int i = 0; i < 5; ++i)
        r.PushFront(-i - 1);
    ZX_EXPECT_EQ(r.Size(), 25u);
    ZX_EXPECT_EQ(r.Front(), -5);
    ZX_EXPECT_EQ(r.Back(), 19);
    ZX_EXPECT_EQ(r.PopFront(), -5);
    ZX_EXPECT_EQ(r.PopBack(), 19);
    ZX_EXPECT_EQ(r[0], -4);

    BitArray bits(130);
    bits.Set(0);
    bits.Set(64);
    bits.Set(129);
    ZX_EXPECT_EQ(bits.CountSet(), 3u);
    ZX_EXPECT_EQ(bits.FindFirstSet(1), 64u);
    ZX_EXPECT_EQ(bits.FindFirstClear(0), 1u);
    bits.Resize(200, true);
    ZX_EXPECT_EQ(bits.CountSet(), 3u + 70u);
}

ZX_TEST(Function, CallablesAndDelegates)
{
    int captured = 10;
    Function<int(int)> f = [captured](int x) { return x + captured; };
    ZX_EXPECT_EQ(f(5), 15);
    Function<int(int)> g = f;
    ZX_EXPECT_EQ(g(1), 11);

    // Large capture spills to heap.
    struct Big
    {
        u64 data[16] = {};
    } big;
    big.data[15] = 42;
    Function<u64()> h = [big] { return big.data[15]; };
    Function<u64()> h2 = move(h);
    ZX_EXPECT(!h);
    ZX_EXPECT_EQ(h2(), 42u);

    // Move-only callable.
    UniquePtr<int> owned = MakeUnique<int>(7);
    Function<int()> mo = [p = move(owned)] { return *p; };
    ZX_EXPECT_EQ(mo(), 7);

    MulticastDelegate<int> event;
    int total = 0;
    DelegateHandle h1 = event.Add([&](int v) { total += v; });
    event.Add([&](int v) { total += v * 10; });
    event.Broadcast(1);
    ZX_EXPECT_EQ(total, 11);
    event.Remove(h1);
    event.Broadcast(1);
    ZX_EXPECT_EQ(total, 21);
}

ZX_TEST(Optional, Basics)
{
    Optional<String> o;
    ZX_EXPECT(!o);
    o = String("value");
    ZX_EXPECT(o.HasValue());
    ZX_EXPECT(*o == "value");
    Optional<String> copy = o;
    o.Reset();
    ZX_EXPECT(copy->Size() == 5);
    ZX_EXPECT(Optional<int>().ValueOr(3) == 3);
}
