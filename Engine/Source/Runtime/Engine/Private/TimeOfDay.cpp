#include "Engine/TimeOfDay.h"

#include <cmath>

namespace zx {

ZX_REFLECT_STRUCT_BEGIN(TimeOfDay)
    ZX_FIELD(hour, Meta::Range(0, 24), Meta::Units("h"), Meta::Tooltip("Time of day (12 = noon)"))
    ZX_FIELD(dayLength, Meta::Range(0, 240), Meta::Units("min"), Meta::Tooltip("Real minutes per game day while playing (0 = the clock stands still)"))
    ZX_FIELD(runInEditor, Meta::Tooltip("The clock also runs while editing"))
    ZX_FIELD(latitude, Meta::Range(-89, 89), Meta::Units("deg"), Meta::Tooltip("Where on Earth: + north, - south; higher = lower sun and longer twilight"))
    ZX_FIELD(dayOfYear, Meta::Range(1, 365), Meta::Tooltip("172 = midsummer (long days), 355 = midwinter"))
    ZX_FIELD(northAngle, Meta::Range(-180, 180), Meta::Units("deg"), Meta::Tooltip("Which world direction is north (0 = +Z)"))
    ZX_FIELD(moonBrightness, Meta::Range(0, 10))
    ZX_FIELD(starBrightness, Meta::Range(0, 10))
ZX_REFLECT_STRUCT_END()

namespace {

/// Direction to a body at hour angle `hourAngle` (radians, 0 = due south at culmination) and declination `dec` from
/// latitude `lat`, in world space (north = northAngle around +Y from +Z, east = to its right).
Vec3 SkyDirection(f32 hourAngle, f32 dec, f32 lat, f32 northAngle)
{
    // Horizon coordinates: altitude and azimuth measured from north towards east.
    const f32 sinAlt = Sin(lat) * Sin(dec) + Cos(lat) * Cos(dec) * Cos(hourAngle);
    const f32 alt = std::asin(Clamp(sinAlt, -1.0f, 1.0f));
    const f32 az = std::atan2(-Sin(hourAngle) * Cos(dec), Sin(dec) * Cos(lat) - Cos(dec) * Sin(lat) * Cos(hourAngle));
    // North = +Z, east = +X (left-handed, Y up), turned by northAngle.
    const f32 a = az + northAngle;
    return Normalize(Vec3(Cos(alt) * Sin(a), Sin(alt), Cos(alt) * Cos(a)));
}

} // namespace

namespace {

f32 SunDeclination(i32 dayOfYear) { return DegToRad(-23.44f) * Cos(2.0f * Pi / 365.0f * (f32(Clamp(dayOfYear, 1, 365)) + 10.0f)); }

} // namespace

DayTimes ComputeDayTimes(const TimeOfDay& t)
{
    const f32 lat = DegToRad(Clamp(t.latitude, -89.5f, 89.5f));
    const f32 dec = SunDeclination(t.dayOfYear);
    // Hour angle at which the sun's center is 0.833 degrees below the horizon (refraction + the disk's radius).
    const f32 cosH = (Sin(DegToRad(-0.833f)) - Sin(lat) * Sin(dec)) / (Cos(lat) * Cos(dec));
    DayTimes d;
    if (cosH <= -1.0f) {
        d.polarDay = true;
        d.sunrise = 0.0f;
        d.sunset = 24.0f;
    } else if (cosH >= 1.0f) {
        d.polarNight = true;
        d.sunrise = d.sunset = 12.0f;
    } else {
        const f32 halfDay = RadToDeg(std::acos(cosH)) / 15.0f;
        d.sunrise = 12.0f - halfDay;
        d.sunset = 12.0f + halfDay;
    }
    return d;
}

f32 HourOf(const TimeOfDay& t, DayMoment moment)
{
    const DayTimes d = ComputeDayTimes(t);
    const bool normal = !d.polarDay && !d.polarNight;
    switch (moment) {
    case DayMoment::Dawn: return normal ? d.sunrise - 0.4f : 3.0f;
    case DayMoment::Sunrise: return normal ? d.sunrise + 0.15f : 6.0f;
    case DayMoment::Morning: return normal ? Min(d.sunrise + 2.5f, 10.0f) : 9.0f;
    case DayMoment::Noon: return 12.0f;
    case DayMoment::GoldenHour: return normal ? d.sunset - 0.75f : 17.0f;
    case DayMoment::Sunset: return normal ? d.sunset - 0.15f : 18.0f; // the disk touching the horizon
    case DayMoment::Dusk: return normal ? d.sunset + 0.45f : 21.0f;
    case DayMoment::Midnight: return 0.0f;
    }
    return t.hour;
}

const char* DayMomentName(DayMoment moment)
{
    static const char* const Names[] = {"Dawn", "Sunrise", "Morning", "Noon", "Golden Hour", "Sunset", "Dusk", "Midnight"};
    return Names[u32(moment)];
}

CelestialState ComputeCelestial(const TimeOfDay& t)
{
    const f32 lat = DegToRad(Clamp(t.latitude, -89.5f, 89.5f));
    const f32 day = f32(Clamp(t.dayOfYear, 1, 365));
    const f32 hour = std::fmod(std::fmod(t.hour, 24.0f) + 24.0f, 24.0f);
    const f32 north = DegToRad(t.northAngle);
    CelestialState s;
    // Sun: declination over the year, hour angle 15 degrees per hour from solar noon.
    const f32 sunDec = SunDeclination(t.dayOfYear);
    s.toSun = SkyDirection(DegToRad(15.0f * (hour - 12.0f)), sunDec, lat, north);
    // Moon: a 29.5-day cycle; it lags the sun by the phase angle (full moon = opposite the sun), on a path tilted
    // about 5 degrees from the sun's.
    const f32 cycle = std::fmod(day / 29.53f, 1.0f);
    const f32 lag = cycle * 2.0f * Pi;
    s.toMoon = SkyDirection(DegToRad(15.0f * (hour - 12.0f)) - lag, sunDec * Cos(lag) + DegToRad(5.0f) * Sin(lag), lat, north);
    s.moonPhase = 0.5f - 0.5f * Cos(lag);
    return s;
}

const TimeOfDay* FindTimeOfDay(ecs::World& world)
{
    const TimeOfDay* found = nullptr;
    world.Query<const TimeOfDay>().Each([&](const TimeOfDay& t) {
        if (!found)
            found = &t;
    });
    return found;
}

void TickTimeOfDay(ecs::World& world, f32 dt, bool playing)
{
    world.Query<TimeOfDay>().Each([&](TimeOfDay& t) {
        if ((!playing && !t.runInEditor) || t.dayLength <= 0.0f || dt <= 0.0f)
            return;
        t.hour = std::fmod(t.hour + dt * 24.0f / (t.dayLength * 60.0f), 24.0f);
    });
}

String FormatHour(f32 hour)
{
    const f32 h = std::fmod(std::fmod(hour, 24.0f) + 24.0f, 24.0f);
    const u32 minutes = u32(h * 60.0f) % (24 * 60);
    return String::Format("{:02}:{:02}", minutes / 60, minutes % 60);
}

} // namespace zx
