#pragma once

// Day and night (M16). A Time of Day component (the first one counts) sets the sun from a clock: hour, latitude and
// day of the year give the sun's height and direction like the real sky (it rises in the east, culminates in the
// south in the northern hemisphere); the moon follows roughly half a day behind with its phase, and stars come out at
// night. It overrides the Directional Light's rotation and the editor's Sun settings. While playing the clock runs
// (dayLength real minutes per 24 hours; 0 stops it); Run In Editor lets it run while editing too.
//
//   TickTimeOfDay(world, dt, playing);         // once per frame
//   const TimeOfDay* t = FindTimeOfDay(world);
//   CelestialState sky = ComputeCelestial(*t); // directions to the sun and the moon, moon phase

#include "Core/Math/Math.h"
#include "Core/Reflection/Reflection.h"
#include "Scene/ECS/World.h"

namespace zx {

struct TimeOfDay
{
    f32 hour = 10.0f;           // 0..24
    f32 dayLength = 24.0f;      // real minutes per game day while playing (0 = the clock stands still)
    bool runInEditor = false;   // the clock also runs while editing
    f32 latitude = 45.0f;       // degrees (+ north)
    i32 dayOfYear = 172;        // 1..365 (172 = midsummer, 355 = midwinter)
    f32 northAngle = 0.0f;      // degrees around +Y: where north is (0 = +Z)
    f32 moonBrightness = 1.0f;  // multiplier of the moonlight
    f32 starBrightness = 1.0f;  // multiplier of the stars
};

ZX_DECLARE_TYPE(TimeOfDay);

struct CelestialState
{
    Vec3 toSun;        // unit vectors from the ground
    Vec3 toMoon;
    f32 moonPhase = 1; // lit fraction of the disk, 0 (new moon) .. 1 (full)
};

/// Sunrise and sunset of the component's day and place (the sun's center 0.83 degrees below the horizon, as almanacs
/// count it). Near the poles the sun may not set (polarDay) or rise (polarNight) at all.
struct DayTimes
{
    f32 sunrise = 6.0f;  // hours
    f32 sunset = 18.0f;
    bool polarDay = false;
    bool polarNight = false;
};

/// Named moments of the day for presets: hours relative to this day's sunrise and sunset.
enum class DayMoment : u8
{
    Dawn,       // twilight before sunrise
    Sunrise,
    Morning,
    Noon,
    GoldenHour, // low warm sun before sunset
    Sunset,
    Dusk,       // blue hour after sunset
    Midnight,
};

/// Sun and moon for the component's clock and place.
CelestialState ComputeCelestial(const TimeOfDay& t);
DayTimes ComputeDayTimes(const TimeOfDay& t);
/// The hour of a named moment on the component's day (polar days / nights fall back to fixed hours).
f32 HourOf(const TimeOfDay& t, DayMoment moment);
const char* DayMomentName(DayMoment moment);
const TimeOfDay* FindTimeOfDay(ecs::World& world);
/// Advances the clock of every Time of Day (while playing, or always with runInEditor).
void TickTimeOfDay(ecs::World& world, f32 dt, bool playing);
/// "HH:MM" of an hour value.
String FormatHour(f32 hour);

} // namespace zx
