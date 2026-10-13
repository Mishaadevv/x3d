#pragma once

#include "Core/String/String.h"

namespace zx {

/// High-resolution monotonic clock (QueryPerformanceCounter).
namespace Clock {
u64 NowTicks();
u64 TicksPerSecond();
f64 TicksToSeconds(u64 ticks);
f64 TicksToMilliseconds(u64 ticks);
u64 SecondsToTicks(f64 seconds);
/// Seconds since process start (first clock use).
f64 NowSeconds();
} // namespace Clock

/// Measures elapsed time.
class Stopwatch
{
public:
    Stopwatch() : m_start(Clock::NowTicks()) {}
    void Restart() { m_start = Clock::NowTicks(); }
    u64 ElapsedTicks() const { return Clock::NowTicks() - m_start; }
    f64 ElapsedSeconds() const { return Clock::TicksToSeconds(ElapsedTicks()); }
    f64 ElapsedMilliseconds() const { return Clock::TicksToMilliseconds(ElapsedTicks()); }

private:
    u64 m_start;
};

/// Calendar date/time (local or UTC).
struct DateTime
{
    u16 year = 0;
    u8 month = 0;
    u8 day = 0;
    u8 hour = 0;
    u8 minute = 0;
    u8 second = 0;
    u16 millisecond = 0;

    static DateTime Now();
    static DateTime NowUtc();
    /// Seconds since 1970-01-01 UTC.
    static i64 UnixTimeNow();
    /// "YYYY-MM-DD hh:mm:ss"
    String ToString() const;
    /// "YYYYMMDD_hhmmss"
    String ToFileNameString() const;
};

/// Per-frame delta time with clamping, time scale and smoothing.
class FrameTimer
{
public:
    /// Call once per frame. Returns scaled delta seconds.
    f32 Tick();

    f32 DeltaSeconds() const { return m_delta; }
    f32 UnscaledDeltaSeconds() const { return m_unscaledDelta; }
    f32 SmoothedDeltaSeconds() const { return m_smoothed; }
    f64 TotalSeconds() const { return m_total; }
    u64 FrameIndex() const { return m_frame; }

    void SetTimeScale(f32 scale) { m_timeScale = scale; }
    f32 GetTimeScale() const { return m_timeScale; }
    /// Clamp for hitches (e.g. after a breakpoint). Default 0.25 s.
    void SetMaxDelta(f32 seconds) { m_maxDelta = seconds; }

private:
    u64 m_last = 0;
    f32 m_delta = 0.0f;
    f32 m_unscaledDelta = 0.0f;
    f32 m_smoothed = 1.0f / 60.0f;
    f32 m_timeScale = 1.0f;
    f32 m_maxDelta = 0.25f;
    f64 m_total = 0.0;
    u64 m_frame = 0;
};

} // namespace zx
