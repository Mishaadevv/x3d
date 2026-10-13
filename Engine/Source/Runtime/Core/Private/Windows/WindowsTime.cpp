#include "Core/Time/Time.h"

#include "Windows/WindowsInclude.h"

namespace zx {

namespace {
struct ClockData
{
    u64 frequency;
    u64 start;
    f64 invFrequency;
};

const ClockData& GetClockData()
{
    static const ClockData data = [] {
        LARGE_INTEGER f, s;
        ::QueryPerformanceFrequency(&f);
        ::QueryPerformanceCounter(&s);
        return ClockData{u64(f.QuadPart), u64(s.QuadPart), 1.0 / f64(f.QuadPart)};
    }();
    return data;
}

DateTime FromSystemTime(const SYSTEMTIME& st)
{
    DateTime d;
    d.year = st.wYear;
    d.month = u8(st.wMonth);
    d.day = u8(st.wDay);
    d.hour = u8(st.wHour);
    d.minute = u8(st.wMinute);
    d.second = u8(st.wSecond);
    d.millisecond = st.wMilliseconds;
    return d;
}
} // namespace

namespace Clock {

u64 NowTicks()
{
    LARGE_INTEGER c;
    ::QueryPerformanceCounter(&c);
    return u64(c.QuadPart);
}
u64 TicksPerSecond() { return GetClockData().frequency; }
f64 TicksToSeconds(u64 ticks) { return f64(ticks) * GetClockData().invFrequency; }
f64 TicksToMilliseconds(u64 ticks) { return f64(ticks) * GetClockData().invFrequency * 1000.0; }
u64 SecondsToTicks(f64 seconds) { return u64(seconds * f64(GetClockData().frequency)); }
f64 NowSeconds() { return TicksToSeconds(NowTicks() - GetClockData().start); }

} // namespace Clock

DateTime DateTime::Now()
{
    SYSTEMTIME st;
    ::GetLocalTime(&st);
    return FromSystemTime(st);
}

DateTime DateTime::NowUtc()
{
    SYSTEMTIME st;
    ::GetSystemTime(&st);
    return FromSystemTime(st);
}

String DateTime::ToString() const
{
    return String::Format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}", year, month, day, hour, minute, second);
}

String DateTime::ToFileNameString() const
{
    return String::Format("{:04}{:02}{:02}_{:02}{:02}{:02}", year, month, day, hour, minute, second);
}

i64 DateTime::UnixTimeNow()
{
    FILETIME ft;
    ::GetSystemTimeAsFileTime(&ft);
    const u64 t = (u64(ft.dwHighDateTime) << 32) | ft.dwLowDateTime; // 100ns since 1601
    return i64(t / 10000000ull) - 11644473600ll;
}

f32 FrameTimer::Tick()
{
    const u64 now = Clock::NowTicks();
    if (m_last == 0)
        m_last = now;
    f32 dt = f32(Clock::TicksToSeconds(now - m_last));
    m_last = now;
    if (dt > m_maxDelta)
        dt = m_maxDelta;
    if (dt < 0.0f)
        dt = 0.0f;
    m_unscaledDelta = dt;
    m_delta = dt * m_timeScale;
    m_smoothed = m_smoothed * 0.9f + dt * 0.1f;
    m_total += m_delta;
    ++m_frame;
    return m_delta;
}

} // namespace zx
