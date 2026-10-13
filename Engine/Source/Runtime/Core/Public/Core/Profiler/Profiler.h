#pragma once

// CPU profiler: hierarchical zones per thread, frame markers, counters, Chrome Tracing export.
//
//   void Update() { ZX_PROFILE_FUNCTION(); ... { ZX_PROFILE_SCOPE("Physics.Step"); ... } }
//   profiler::BeginFrame();                         // once per frame, main thread
//   profiler::GetFrameZones(frame, zones);          // editor profiler panel
//   profiler::ExportChromeTrace("Saved/trace.json", firstFrame, lastFrame);   // open in chrome://tracing
//
// Zone names must be string literals (or otherwise live forever). Overhead: ~2 QPC reads per zone.
// Each thread keeps the last N zones in a ring buffer; old data is overwritten.

#include "Core/Containers/Array.h"
#include "Core/String/String.h"

namespace zx::profiler {

struct Zone
{
    const char* name;
    u64 startTicks;
    u64 endTicks;
    u64 frame;
    u32 threadId;
    u16 depth;
};

struct ThreadInfo
{
    u32 threadId;
    String name;
};

void SetEnabled(bool enabled);
bool IsEnabled();

/// Marks the start of a new frame (main thread). Returns the new frame index.
u64 BeginFrame();
u64 CurrentFrame();
/// Tick timestamps of frame starts (for frame time graphs). Returns false if frame is no longer recorded.
bool GetFrameTime(u64 frame, u64& startTicks, u64& endTicks);

void BeginZone(const char* name);
void EndZone();

/// Records a counter value for the current frame (e.g. draw calls). Name must be a literal.
void SetCounter(const char* name, f64 value);

/// Copies all zones of `frame` from every thread (unsorted). Returns false if the frame is too old.
bool GetFrameZones(u64 frame, Array<Zone>& out);
Array<ThreadInfo> GetThreads();

/// Writes zones of frames [firstFrame, lastFrame] in Chrome Trace Event format.
bool ExportChromeTrace(StringView path, u64 firstFrame, u64 lastFrame);

class ScopedZone
{
public:
    explicit ScopedZone(const char* name) { BeginZone(name); }
    ~ScopedZone() { EndZone(); }
    ScopedZone(const ScopedZone&) = delete;
    ScopedZone& operator=(const ScopedZone&) = delete;
};

} // namespace zx::profiler

#if ZX_ENABLE_PROFILER
    #define ZX_PROFILE_SCOPE(name) ::zx::profiler::ScopedZone ZX_UNIQUE_NAME(zxProfileZone_)(name)
    #define ZX_PROFILE_FUNCTION() ZX_PROFILE_SCOPE(__FUNCTION__)
    #define ZX_PROFILE_COUNTER(name, value) ::zx::profiler::SetCounter(name, double(value))
#else
    #define ZX_PROFILE_SCOPE(name)                                                                                     \
        do {                                                                                                           \
        } while (0)
    #define ZX_PROFILE_FUNCTION()                                                                                      \
        do {                                                                                                           \
        } while (0)
    #define ZX_PROFILE_COUNTER(name, value)                                                                            \
        do {                                                                                                           \
        } while (0)
#endif
