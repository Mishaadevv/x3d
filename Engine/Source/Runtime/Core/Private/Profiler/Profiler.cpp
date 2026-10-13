#include "Core/Profiler/Profiler.h"
#include "Core/FileSystem/FileSystem.h"
#include "Core/Threading/Mutex.h"
#include "Core/Threading/Thread.h"
#include "Core/Time/Time.h"

#include <atomic>

namespace zx::profiler {

namespace {

constexpr u32 ZoneCapacity = 1u << 16; // per thread
constexpr u32 FrameHistory = 1024;
constexpr u32 MaxDepth = 64;
constexpr u32 MaxCounters = 64;

struct ThreadData
{
    u32 threadId = 0;
    String name;
    Zone* zones = nullptr;          // ring buffer
    std::atomic<u64> written{0};    // total zones ever written
    SpinLock readLock;              // taken by the owner while writing one entry and by readers while copying
    u64 openStart[MaxDepth];
    const char* openName[MaxDepth];
    u32 depth = 0;
};

struct CounterSample
{
    const char* name;
    f64 value;
};

struct FrameRecord
{
    u64 index = 0;
    u64 start = 0;
    u64 end = 0;
    CounterSample counters[MaxCounters];
    u32 counterCount = 0;
};

struct ProfilerState
{
    Mutex threadsMutex;
    Array<ThreadData*> threads;
    std::atomic<bool> enabled{ZX_ENABLE_PROFILER != 0};
    std::atomic<u64> frame{0};
    Mutex frameMutex;
    FrameRecord frames[FrameHistory];
};

ProfilerState& State()
{
    static ProfilerState* s = [] {
        MemTagScope tag(MemTag::Core);
        LeakIgnoreScope permanent;
        return new ProfilerState(); // leaked: threads may record during static destruction
    }();
    return *s;
}

thread_local ThreadData* t_data = nullptr;

ThreadData& ThisThread()
{
    if (!t_data) {
        MemTagScope tag(MemTag::Core);
        LeakIgnoreScope permanent;
        ThreadData* d = new ThreadData();
        d->threadId = Thread::CurrentId();
        d->name = Thread::IsMainThread() ? String("Main") : String::Format("Thread {}", d->threadId);
        d->zones = static_cast<Zone*>(Malloc(sizeof(Zone) * ZoneCapacity, 64, MemTag::Core));
        ProfilerState& s = State();
        ScopedLock<Mutex> guard(s.threadsMutex);
        s.threads.PushBack(d);
        t_data = d;
    }
    return *t_data;
}

} // namespace

void SetEnabled(bool enabled) { State().enabled.store(enabled); }
bool IsEnabled() { return State().enabled.load(std::memory_order_relaxed); }

u64 BeginFrame()
{
    ProfilerState& s = State();
    const u64 now = Clock::NowTicks();
    ScopedLock<Mutex> guard(s.frameMutex);
    const u64 previous = s.frame.load();
    s.frames[previous % FrameHistory].end = now;
    const u64 next = previous + 1;
    FrameRecord& rec = s.frames[next % FrameHistory];
    rec.index = next;
    rec.start = now;
    rec.end = 0;
    rec.counterCount = 0;
    s.frame.store(next, std::memory_order_release);
    return next;
}

u64 CurrentFrame() { return State().frame.load(std::memory_order_acquire); }

bool GetFrameTime(u64 frame, u64& startTicks, u64& endTicks)
{
    ProfilerState& s = State();
    ScopedLock<Mutex> guard(s.frameMutex);
    const FrameRecord& rec = s.frames[frame % FrameHistory];
    if (rec.index != frame)
        return false;
    startTicks = rec.start;
    endTicks = rec.end ? rec.end : Clock::NowTicks();
    return true;
}

void BeginZone(const char* name)
{
    if (!IsEnabled())
        return;
    ThreadData& t = ThisThread();
    if (t.depth < MaxDepth) {
        t.openName[t.depth] = name;
        t.openStart[t.depth] = Clock::NowTicks();
    }
    ++t.depth;
}

void EndZone()
{
    ThreadData* t = t_data;
    if (!t || t->depth == 0)
        return; // zone began while profiling was disabled
    --t->depth;
    if (t->depth >= MaxDepth)
        return;
    const u64 end = Clock::NowTicks();
    const u64 index = t->written.load(std::memory_order_relaxed);
    ScopedLock<SpinLock> guard(t->readLock);
    Zone& z = t->zones[index % ZoneCapacity];
    z.name = t->openName[t->depth];
    z.startTicks = t->openStart[t->depth];
    z.endTicks = end;
    z.frame = State().frame.load(std::memory_order_relaxed);
    z.threadId = t->threadId;
    z.depth = u16(t->depth);
    t->written.store(index + 1, std::memory_order_release);
}

void SetCounter(const char* name, f64 value)
{
    ProfilerState& s = State();
    ScopedLock<Mutex> guard(s.frameMutex);
    FrameRecord& rec = s.frames[s.frame.load() % FrameHistory];
    for (u32 i = 0; i < rec.counterCount; ++i) {
        if (rec.counters[i].name == name) {
            rec.counters[i].value = value;
            return;
        }
    }
    if (rec.counterCount < MaxCounters)
        rec.counters[rec.counterCount++] = CounterSample{name, value};
}

bool GetFrameZones(u64 frame, Array<Zone>& out)
{
    ProfilerState& s = State();
    bool any = false;
    ScopedLock<Mutex> guard(s.threadsMutex);
    for (ThreadData* t : s.threads) {
        ScopedLock<SpinLock> read(t->readLock);
        const u64 written = t->written.load(std::memory_order_acquire);
        const u64 count = written < ZoneCapacity ? written : ZoneCapacity;
        // Walk backwards from newest; zones are written in end order so frames are roughly contiguous.
        for (u64 i = 0; i < count; ++i) {
            const Zone& z = t->zones[(written - 1 - i) % ZoneCapacity];
            if (z.frame == frame) {
                out.PushBack(z);
                any = true;
            } else if (z.frame + 2 < frame) {
                break; // older than the requested frame (allow slack for long cross-frame zones)
            }
        }
    }
    return any;
}

Array<ThreadInfo> GetThreads()
{
    ProfilerState& s = State();
    ScopedLock<Mutex> guard(s.threadsMutex);
    Array<ThreadInfo> out;
    for (ThreadData* t : s.threads)
        out.PushBack(ThreadInfo{t->threadId, t->name});
    return out;
}

bool ExportChromeTrace(StringView path, u64 firstFrame, u64 lastFrame)
{
    String json;
    json.Reserve(1 << 20);
    json += "{\"displayTimeUnit\":\"ms\",\"traceEvents\":[\n";
    bool first = true;
    auto comma = [&] {
        if (!first)
            json += ",\n";
        first = false;
    };
    for (const ThreadInfo& t : GetThreads()) {
        comma();
        json.Appendf(R"({{"name":"thread_name","ph":"M","pid":1,"tid":{},"args":{{"name":"{}"}}}})", t.threadId, t.name);
    }
    const f64 toUs = 1e6 / f64(Clock::TicksPerSecond());
    Array<Zone> zones;
    for (u64 f = firstFrame; f <= lastFrame; ++f) {
        zones.Clear();
        GetFrameZones(f, zones);
        for (const Zone& z : zones) {
            comma();
            json.Appendf(R"({{"name":"{}","ph":"X","pid":1,"tid":{},"ts":{:.3f},"dur":{:.3f}}})", z.name, z.threadId,
                         f64(z.startTicks) * toUs, f64(z.endTicks - z.startTicks) * toUs);
        }
        u64 fs0, fs1;
        if (GetFrameTime(f, fs0, fs1)) {
            comma();
            json.Appendf(R"({{"name":"Frame {}","ph":"i","s":"g","pid":1,"tid":0,"ts":{:.3f}}})", f, f64(fs0) * toUs);
        }
    }
    json += "\n]}\n";
    return fs::WriteTextFile(path, json);
}

} // namespace zx::profiler
