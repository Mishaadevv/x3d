#include "Core/Containers/Queue.h"
#include "Core/Result.h"
#include "Core/String/Parse.h"
#include "Core/Testing/Test.h"
#include "Core/Threading/Thread.h"

#include <atomic>

using namespace zx;

namespace {

Result<i32> ParsePositive(StringView text)
{
    const auto v = ParseInt(text);
    if (!v)
        return Error::Make(ErrorCode::InvalidData, "'{}' is not a number", text);
    if (*v <= 0)
        return Error::Make(ErrorCode::InvalidArgument, "{} is not positive", *v);
    return i32(*v);
}

Result<i32> SumOfTwo(StringView a, StringView b)
{
    ZX_TRY_ASSIGN(x, ParsePositive(a));
    ZX_TRY_ASSIGN(y, ParsePositive(b));
    return x + y;
}

Result<void> Validate(StringView a)
{
    ZX_TRY(ParsePositive(a));
    return Result<void>::Ok();
}

} // namespace

ZX_TEST(Result, ValueAndErrorPropagation)
{
    const Result<i32> ok = SumOfTwo("2", "40");
    ZX_REQUIRE(ok.IsOk());
    ZX_EXPECT_EQ(*ok, 42);
    const Result<i32> bad = SumOfTwo("2", "x");
    ZX_EXPECT(!bad);
    ZX_EXPECT(bad.GetError().code == ErrorCode::InvalidData);
    ZX_EXPECT(bad.GetError().message == "'x' is not a number");
    ZX_EXPECT(String::Format("{}", bad.GetError()) == "InvalidData: 'x' is not a number");
    ZX_EXPECT_EQ(SumOfTwo("-1", "1").GetError().code, ErrorCode::InvalidArgument);
    ZX_EXPECT(Validate("5").IsOk());
    ZX_EXPECT(!Validate("0"));
    ZX_EXPECT_EQ(bad.ValueOr(-1), -1);

    Result<String> s = String("moved");
    const String taken = s.Take();
    ZX_EXPECT(taken == "moved");
}

ZX_TEST(Queue, SPSCOrderAcrossThreads)
{
    SPSCQueue<u32> q(1024);
    constexpr u32 Count = 200000;
    std::atomic<bool> ok{true};
    Thread consumer;
    consumer.Start("SPSCConsumer", [&] {
        u32 expected = 0;
        while (expected < Count) {
            u32 v;
            if (q.TryPop(v)) {
                if (v != expected)
                    ok = false;
                ++expected;
            }
        }
    });
    for (u32 i = 0; i < Count;) {
        if (q.TryPush(i))
            ++i;
    }
    consumer.Join();
    ZX_EXPECT(ok.load());
    u32 dummy;
    ZX_EXPECT(!q.TryPop(dummy));
}

ZX_TEST(Queue, MPMCNoLossNoDuplicates)
{
    MPMCQueue<u32> q(256);
    constexpr u32 PerProducer = 50000;
    constexpr int Producers = 3;
    constexpr int Consumers = 3;
    std::atomic<u8>* flags = static_cast<std::atomic<u8>*>(Malloc(sizeof(std::atomic<u8>) * PerProducer * Producers));
    for (u32 i = 0; i < PerProducer * Producers; ++i)
        ::new (&flags[i]) std::atomic<u8>(0);
    std::atomic<u32> consumed{0};
    Thread threads[Producers + Consumers];
    for (int p = 0; p < Producers; ++p) {
        threads[p].Start("Producer", [&q, p] {
            for (u32 i = 0; i < PerProducer;) {
                if (q.TryPush(u32(p) * PerProducer + i))
                    ++i;
            }
        });
    }
    for (int c = 0; c < Consumers; ++c) {
        threads[Producers + c].Start("Consumer", [&] {
            while (consumed.load() < PerProducer * Producers) {
                u32 v;
                if (q.TryPop(v)) {
                    flags[v].fetch_add(1);
                    consumed.fetch_add(1);
                }
            }
        });
    }
    for (Thread& t : threads)
        t.Join();
    bool exactlyOnce = true;
    for (u32 i = 0; i < PerProducer * Producers; ++i)
        exactlyOnce &= flags[i].load() == 1;
    ZX_EXPECT(exactlyOnce);
    Free(flags);

    MPMCQueue<String> strings(4);
    ZX_EXPECT(strings.TryPush(String("a long string that lives on the heap!")));
    ZX_EXPECT(strings.TryPush(String("b")));
    String out;
    ZX_EXPECT(strings.TryPop(out));
    ZX_EXPECT(out.StartsWith("a long"));
    // Remaining element destroyed by the destructor (checked by leak tracking).
}
