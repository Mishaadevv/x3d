#include "Core/Memory/Allocator.h"
#include "Core/Memory/OffsetAllocator.h"
#include "Core/Memory/Pointers.h"
#include "Core/Testing/Test.h"

using namespace zx;

ZX_TEST(Memory, MallocAlignmentAndSize)
{
    for (usize align : {16u, 32u, 64u, 256u, 4096u}) {
        void* p = Malloc(100, align);
        ZX_EXPECT(reinterpret_cast<uptr>(p) % align == 0);
        ZX_EXPECT_EQ(AllocationSize(p), 100u);
        MemSet(p, 0xAB, 100);
        Free(p);
    }
    void* r = Malloc(8);
    MemSet(r, 7, 8);
    r = Realloc(r, 1000);
    ZX_EXPECT_EQ(static_cast<u8*>(r)[7], 7);
    Free(r);
}

ZX_TEST(Memory, TrackingAndTags)
{
#if ZX_ENABLE_MEMORY_TRACKING
    const MemoryStats before = GetMemoryStats();
    const u64 seq = GetAllocationSequence();
    void* p;
    {
        MemTagScope scope(MemTag::Physics);
        p = Malloc(1000);
        int* viaNew = new int(5); // global new must be routed through zx::Malloc
        const MemoryStats mid = GetMemoryStats();
        ZX_EXPECT_EQ(mid.tags[usize(MemTag::Physics)].currentBytes - before.tags[usize(MemTag::Physics)].currentBytes,
                     i64(1000 + sizeof(int)));
        delete viaNew;
    }
    ZX_EXPECT_EQ(GetThreadMemTag(), MemTag::General);
    Free(p);
    const MemoryStats after = GetMemoryStats();
    ZX_EXPECT_EQ(after.tags[usize(MemTag::Physics)].currentBytes, before.tags[usize(MemTag::Physics)].currentBytes);
    ZX_EXPECT_EQ(ReportMemoryLeaks(seq), 0u);
#endif
}

ZX_TEST(Memory, LinearAllocator)
{
    LinearAllocator a(1024);
    void* p1 = a.Allocate(100, 16);
    void* p2 = a.Allocate(100, 64);
    ZX_EXPECT(reinterpret_cast<uptr>(p2) % 64 == 0);
    ZX_EXPECT(p1 != p2);
    const auto marker = a.GetMarker();
    void* big = a.Allocate(5000, 16); // larger than block size -> dedicated block
    ZX_EXPECT(big != nullptr);
    a.RewindTo(marker);
    void* p3 = a.Allocate(10, 16);
    ZX_EXPECT(p3 != nullptr);
    a.Reset();
    ZX_EXPECT_EQ(a.BytesUsed(), 0u);
    int* arr = a.NewArray<int>(10);
    arr[9] = 3;
    ZX_EXPECT_EQ(arr[9], 3);
}

ZX_TEST(Memory, PoolAllocator)
{
    struct Obj
    {
        u64 a, b, c;
    };
    ObjectPool<Obj> pool(4);
    Obj* objs[10];
    for (int i = 0; i < 10; ++i)
        objs[i] = pool.Create(Obj{u64(i), 0, 0});
    ZX_EXPECT_EQ(pool.LiveCount(), 10u);
    for (int i = 0; i < 10; ++i)
        ZX_EXPECT_EQ(objs[i]->a, u64(i));
    for (int i = 0; i < 10; i += 2)
        pool.Destroy(objs[i]);
    ZX_EXPECT_EQ(pool.LiveCount(), 5u);
    Obj* reused = pool.Create();
    ZX_EXPECT(reused == objs[8]); // LIFO free list
    pool.Destroy(reused);
    for (int i = 1; i < 10; i += 2)
        pool.Destroy(objs[i]);
}

ZX_TEST(Memory, VirtualArena)
{
    VirtualArena arena(64_MB);
    u8* a = static_cast<u8*>(arena.Allocate(1_MB, 16));
    MemSet(a, 1, 1_MB);
    u8* b = static_cast<u8*>(arena.Allocate(3_MB, 4096));
    ZX_EXPECT(reinterpret_cast<uptr>(b) % 4096 == 0);
    MemSet(b, 2, 3_MB);
    ZX_EXPECT(arena.Committed() >= 4_MB);
    arena.Reset(true);
    ZX_EXPECT_EQ(arena.Committed(), 0u);
}

ZX_TEST(Memory, OffsetAllocatorBasic)
{
    OffsetAllocator a(1024 * 1024, 1000);
    auto x = a.Allocate(1000);
    auto y = a.Allocate(5000, 4096);
    auto z = a.Allocate(300, 256);
    ZX_REQUIRE(x.IsValid() && y.IsValid() && z.IsValid());
    ZX_EXPECT_EQ(x.offset, 0u);
    ZX_EXPECT_EQ(y.offset % 4096, 0u);
    ZX_EXPECT_EQ(z.offset % 256, 0u);
    ZX_EXPECT(y.offset >= 1000);
    ZX_EXPECT(z.offset >= y.offset + 5000 || z.offset + 300 <= y.offset);
    a.Free(y);
    a.Free(x);
    a.Free(z);
    const auto stats = a.GetStats();
    ZX_EXPECT_EQ(stats.freeSize, 1024u * 1024u);
    ZX_EXPECT_EQ(stats.freeBlockCount, 1u); // fully coalesced
    ZX_EXPECT_EQ(stats.allocationCount, 0u);
}

ZX_TEST(Memory, OffsetAllocatorStress)
{
    constexpr u64 Total = 16 * 1024 * 1024;
    OffsetAllocator a(Total, 4096);
    OffsetAllocator::Allocation allocs[2000];
    u64 sizes[2000];
    u32 rng = 12345;
    auto next = [&] {
        rng = rng * 1664525u + 1013904223u;
        return rng >> 8;
    };
    for (int round = 0; round < 3; ++round) {
        for (int i = 0; i < 2000; ++i) {
            sizes[i] = 1 + next() % 4000;
            allocs[i] = a.Allocate(sizes[i], u64(1) << (next() % 8));
            ZX_REQUIRE(allocs[i].IsValid());
        }
        // Verify no overlaps by marking ranges in a bitmap of 64-byte granules is too coarse;
        // instead sort-free check: pairwise on a sample.
        for (int i = 0; i < 2000; i += 37)
            for (int j = i + 1; j < 2000; j += 41) {
                const bool disjoint = allocs[i].offset + sizes[i] <= allocs[j].offset ||
                                      allocs[j].offset + sizes[j] <= allocs[i].offset;
                ZX_EXPECT(disjoint);
            }
        for (int i = 0; i < 2000; i += 2)
            a.Free(allocs[i]);
        for (int i = 1; i < 2000; i += 2)
            a.Free(allocs[i]);
        ZX_EXPECT_EQ(a.FreeSize(), Total);
    }
}

namespace {
struct Counted : RefCounted
{
    static inline int alive = 0;
    Counted() { ++alive; }
    ~Counted() override { --alive; }
};

struct Base
{
    virtual ~Base() = default;
    u64 pad = 1;
};
struct Other
{
    virtual ~Other() = default;
    u64 pad2 = 2;
};
struct Derived : Other, Base
{
    static inline int alive = 0;
    Derived() { ++alive; }
    ~Derived() override { --alive; }
};
} // namespace

ZX_TEST(Memory, SmartPointers)
{
    {
        RefPtr<Counted> a = MakeRef<Counted>();
        ZX_EXPECT_EQ(Counted::alive, 1);
        RefPtr<Counted> b = a;
        ZX_EXPECT_EQ(a->GetRefCount(), 2u);
        a = nullptr;
        ZX_EXPECT_EQ(Counted::alive, 1);
    }
    ZX_EXPECT_EQ(Counted::alive, 0);
    {
        // Deleting through a non-first base must free the right address.
        UniquePtr<Base> p = MakeUnique<Derived>();
        ZX_EXPECT_EQ(Derived::alive, 1);
    }
    ZX_EXPECT_EQ(Derived::alive, 0);
}
