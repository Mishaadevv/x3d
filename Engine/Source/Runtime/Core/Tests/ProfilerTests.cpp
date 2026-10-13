#include "Core/FileSystem/FileSystem.h"
#include "Core/Jobs/JobSystem.h"
#include "Core/Profiler/Profiler.h"
#include "Core/Serialization/Json.h"
#include "Core/Testing/Test.h"
#include "Core/Threading/Thread.h"

using namespace zx;

#if ZX_ENABLE_PROFILER

namespace {
void Leaf()
{
    ZX_PROFILE_SCOPE("Leaf");
    volatile int x = 0;
    for (int i = 0; i < 1000; ++i)
        x = x + i;
}
} // namespace

ZX_TEST(Profiler, ZonesAndExport)
{
    const u64 frame = profiler::BeginFrame();
    {
        ZX_PROFILE_SCOPE("Outer");
        Leaf();
        Leaf();
        ZX_PROFILE_COUNTER("DrawCalls", 42);
    }
    Thread worker;
    worker.Start("ProfiledWorker", [] {
        ZX_PROFILE_SCOPE("WorkerZone");
        Leaf();
    });
    worker.Join();
    profiler::BeginFrame();

    Array<profiler::Zone> zones;
    ZX_EXPECT(profiler::GetFrameZones(frame, zones));
    int outer = 0, leaf = 0, workerZones = 0;
    for (const profiler::Zone& z : zones) {
        const StringView n(z.name);
        outer += n == "Outer";
        leaf += n == "Leaf";
        workerZones += n == "WorkerZone";
        ZX_EXPECT(z.endTicks >= z.startTicks);
        if (n == "Leaf" && z.threadId == Thread::CurrentId())
            ZX_EXPECT_EQ(z.depth, u16(1));
    }
    ZX_EXPECT_EQ(outer, 1);
    ZX_EXPECT_EQ(leaf, 3);
    ZX_EXPECT_EQ(workerZones, 1);
    u64 fs0, fs1;
    ZX_EXPECT(profiler::GetFrameTime(frame, fs0, fs1));
    ZX_EXPECT(fs1 >= fs0);

    fs::Initialize(1);
    const String path = platform::GetTempDirectory() + "/zx_trace_test.json";
    ZX_EXPECT(profiler::ExportChromeTrace(path, frame, frame));
    String text;
    ZX_EXPECT(fs::ReadTextFile(path, text));
    JsonValue trace;
    ZX_EXPECT(ParseJson(text, trace));
    ZX_EXPECT(trace.Get("traceEvents").Size() >= 5u);
    platform::DeleteFileAt(path);
    fs::Shutdown();

    profiler::SetEnabled(false);
    const u64 disabledFrame = profiler::BeginFrame();
    {
        ZX_PROFILE_SCOPE("ShouldNotRecord");
    }
    profiler::SetEnabled(true);
    zones.Clear();
    profiler::GetFrameZones(disabledFrame, zones);
    for (const profiler::Zone& z : zones)
        ZX_EXPECT(StringView(z.name) != "ShouldNotRecord");
}

#endif
