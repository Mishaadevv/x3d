// Time of Day: the sun's path matches the real sky (height at noon from latitude and season, rising in the east,
// south at noon in the north), the moon's phase cycle, and the clock.

#include "Core/Testing/Test.h"
#include "Engine/TimeOfDay.h"
#include "Scene/Hierarchy.h"

using namespace zx;

namespace {

f32 ElevationDeg(Vec3 d) { return RadToDeg(Asin(Clamp(d.y, -1.0f, 1.0f))); }

} // namespace

ZX_TEST(TimeOfDay, SunPathFollowsLatitudeAndSeason)
{
    TimeOfDay t;
    t.latitude = 45.0f;
    t.hour = 12.0f;
    t.dayOfYear = 172; // midsummer: 90 - 45 + 23.4
    CelestialState s = ComputeCelestial(t);
    ZX_EXPECT(Abs(ElevationDeg(s.toSun) - 68.4f) < 1.0f);
    ZX_EXPECT(s.toSun.z < -0.3f); // south (north = +Z)
    t.dayOfYear = 355; // midwinter: 90 - 45 - 23.4
    s = ComputeCelestial(t);
    ZX_EXPECT(Abs(ElevationDeg(s.toSun) - 21.6f) < 1.0f);
    // Equinox: rises due east at 6, sets due west at 18, below the horizon at midnight.
    t.dayOfYear = 80;
    t.hour = 6.0f;
    s = ComputeCelestial(t);
    ZX_EXPECT(Abs(ElevationDeg(s.toSun)) < 2.0f);
    ZX_EXPECT(s.toSun.x > 0.95f); // east = +X
    t.hour = 18.0f;
    s = ComputeCelestial(t);
    ZX_EXPECT(s.toSun.x < -0.95f);
    t.hour = 0.0f;
    s = ComputeCelestial(t);
    ZX_EXPECT(ElevationDeg(s.toSun) < -30.0f);
    // Southern hemisphere: the noon sun is in the north.
    t.latitude = -35.0f;
    t.hour = 12.0f;
    s = ComputeCelestial(t);
    ZX_EXPECT(s.toSun.z > 0.3f);
    // North turned to +X: the noon sun moves to -X.
    t.latitude = 45.0f;
    t.northAngle = 90.0f;
    s = ComputeCelestial(t);
    ZX_EXPECT(s.toSun.x < -0.3f);
}

ZX_TEST(TimeOfDay, MoonPhaseAndClock)
{
    TimeOfDay t;
    t.dayOfYear = 1;
    const f32 newish = ComputeCelestial(t).moonPhase;
    t.dayOfYear = 16; // half a lunar cycle later
    const f32 fullish = ComputeCelestial(t).moonPhase;
    ZX_EXPECT(fullish > newish + 0.5f);
    // A full moon stands opposite the sun.
    const CelestialState s = ComputeCelestial(t);
    ZX_EXPECT(Dot(s.toSun, s.toMoon) < -0.8f);

    ecs::World world;
    const ecs::Entity e = scene::CreateNode(world, "Clock", Transform());
    TimeOfDay clock;
    clock.hour = 23.5f;
    clock.dayLength = 1.0f; // one real minute per day: an hour takes 2.5 s
    world.Add(e, clock);
    TickTimeOfDay(world, 2.5f, false); // editing: stands still
    ZX_EXPECT_NEAR(FindTimeOfDay(world)->hour, 23.5f, 1e-4f);
    TickTimeOfDay(world, 2.5f, true); // playing: wraps past midnight
    ZX_EXPECT_NEAR(FindTimeOfDay(world)->hour, 0.5f, 1e-3f);
    ZX_EXPECT(FormatHour(13.75f) == "13:45");
}
