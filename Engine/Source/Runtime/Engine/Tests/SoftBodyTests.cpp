// Soft-body vehicle: settles on its wheels without falling apart, drives, steers, dents in a crash, resets.

#include <cmath>
#include "Core/Testing/Test.h"
#include "Engine/PhysicsSystem.h"
#include "Engine/SoftBody.h"
#include "Scene/Hierarchy.h"

using namespace zx;

namespace {

struct CarScene
{
    ecs::World world;
    PhysicsSystem physics;
    SoftBodySystem cars;
    ecs::Entity car;

    explicit CarScene(bool wall = false, bool ramp = false)
    {
        scene::InstallHierarchyHooks(world);
        const ecs::Entity floor = scene::CreateNode(world, "Floor", Transform(Vec3(0, -0.5f, 0), Quat(), Vec3(400, 1, 400)));
        world.Add(floor, BoxCollider{});
        if (wall) {
            const ecs::Entity w = scene::CreateNode(world, "Wall", Transform(Vec3(0, 1.5f, 30.0f), Quat(), Vec3(10, 3, 1)));
            world.Add(w, BoxCollider{});
        }
        if (ramp) { // 14 degrees, 12 m long, its low edge at z = 40
            const f32 a = DegToRad(14.0f);
            const Vec3 along(0, std::sin(a), std::cos(a)), up(0, std::cos(a), -std::sin(a));
            const ecs::Entity r = scene::CreateNode(world, "Ramp",
                                                    Transform(Vec3(0, 0, 40) + along * 6.0f - up * 0.25f, Quat::LookRotation(along, up), Vec3(7, 0.5f, 12)));
            world.Add(r, BoxCollider{});
        }
        car = scene::CreateNode(world, "Car", Transform(Vec3(0, 0.3f, 0)));
        world.Add(car, SoftBody{});
        scene::UpdateWorldTransforms(world);
        physics.Begin(world, nullptr);
    }
    ~CarScene() { physics.End(); }
    void Run(f32 seconds, const VehicleInput& input = {})
    {
        for (u32 i = 0; i < u32(seconds * 60.0f); ++i) {
            cars.SetInput(car, input);
            scene::UpdateWorldTransforms(world);
            cars.Tick(world, 1.0f / 60.0f, &physics);
            scene::UpdateWorldTransforms(world);
            physics.Tick(world, 1.0f / 60.0f);
        }
    }
    Transform Pose() const { return *world.Get<const Transform>(car); }
};

} // namespace

ZX_TEST(SoftBody, SettlesOnItsWheels)
{
    CarScene s;
    s.Run(3.0f);
    const Transform t = s.Pose();
    // Resting on the suspension: the body near its design height, level, not spread out or torn.
    ZX_EXPECT_NEAR(t.position.y, 0.0f, 0.15f);
    ZX_EXPECT_GT((t.rotation * Vec3(0, 1, 0)).y, 0.99f);
    ZX_EXPECT_LT(Length(Vec3(t.position.x, 0, t.position.z)), 0.2f);
    ZX_EXPECT_EQ(s.cars.BrokenBeams(s.car), 0u);
    ZX_EXPECT_LT(s.cars.Damage(s.car), 0.01f);
    ZX_EXPECT_LT(Abs(s.cars.Speed(s.car)), 0.1f);
    // The skinning palette: one matrix per node, offsets small at rest.
    const SoftBody* sb = s.world.Get<const SoftBody>(s.car);
    ZX_REQUIRE(!sb->palette.Empty());
    for (const Mat4& m : sb->palette)
        ZX_EXPECT_LT(Length(m.GetTranslation()), 0.05f);
}

ZX_TEST(SoftBody, DrivesSteersAndBrakes)
{
    CarScene s;
    s.Run(1.5f);
    VehicleInput gas;
    gas.throttle = 1.0f;
    s.Run(4.0f, gas);
    const f32 speed = s.cars.Speed(s.car);
    ZX_EXPECT_GT(speed, 12.0f);                // accelerates forward (+Z)
    ZX_EXPECT_GT(s.Pose().position.z, 15.0f);
    ZX_EXPECT_LT(Abs(s.Pose().position.x), 1.0f); // straight
    // Steering right turns the car towards +X.
    VehicleInput turn = gas;
    turn.throttle = 0.3f;
    turn.steer = 1.0f;
    s.Run(1.5f, turn);
    ZX_EXPECT_GT((s.Pose().rotation * Vec3(0, 0, 1)).x, 0.3f);
    // Braking stops it, upright.
    VehicleInput stop;
    stop.brake = 1.0f;
    s.Run(5.0f, stop);
    ZX_EXPECT_LT(Abs(s.cars.Speed(s.car)), 0.5f);
    ZX_EXPECT_GT((s.Pose().rotation * Vec3(0, 1, 0)).y, 0.95f);
    ZX_EXPECT_EQ(s.cars.BrokenBeams(s.car), 0u);
}

ZX_TEST(SoftBody, CrashDentsTheFrontAndResetRepairs)
{
    CarScene s(true);
    s.Run(1.0f);
    VehicleInput gas;
    gas.throttle = 1.0f;
    s.Run(6.0f, gas); // into the wall at speed
    const f32 impact = s.cars.TakeImpact(s.car);
    ZX_EXPECT_GT(impact, 8.0f);
    ZX_EXPECT_GT(s.cars.Damage(s.car), 0.02f); // dented for good
    ZX_EXPECT_LT(s.Pose().position.z, 30.0f);  // stopped by the wall, not through it
    // The front crumpled: front nodes moved back relative to the rest shape.
    const SoftBody* sb = s.world.Get<const SoftBody>(s.car);
    f32 crush = 0.0f;
    for (const Mat4& m : sb->palette)
        crush = Min(crush, m.GetTranslation().z);
    ZX_EXPECT_LT(crush, -0.1f);
    // Everything stays finite.
    for (const Mat4& m : sb->palette)
        ZX_EXPECT(std::isfinite(m.GetTranslation().x) && std::isfinite(m.GetTranslation().y));
    // Reset: repaired, upright, a meter up.
    s.cars.Reset(s.car);
    s.Run(1.0f / 60.0f);
    ZX_EXPECT_LT(s.cars.Damage(s.car), 0.01f);
    ZX_EXPECT_EQ(s.cars.BrokenBeams(s.car), 0u);
    ZX_EXPECT_GT((s.Pose().rotation * Vec3(0, 1, 0)).y, 0.98f);
}

ZX_TEST(SoftBody, JumpsOffARampAndLandsOnItsWheels)
{
    CarScene s(false, true);
    s.Run(1.0f);
    VehicleInput gas;
    gas.throttle = 1.0f;
    f32 highest = 0.0f;
    for (u32 i = 0; i < 9 * 60; ++i) {
        s.Run(1.0f / 60.0f, gas);
        highest = Max(highest, s.Pose().position.y);
    }
    ZX_EXPECT_GT(highest, 2.0f);              // flew off the ramp
    ZX_EXPECT_GT(s.Pose().position.z, 70.0f); // and landed beyond it
    s.Run(3.0f);
    ZX_EXPECT_GT((s.Pose().rotation * Vec3(0, 1, 0)).y, 0.95f); // on its wheels
    ZX_EXPECT_NEAR(s.Pose().position.y, 0.0f, 0.3f);
    ZX_EXPECT_LT(s.cars.BrokenBeams(s.car), 10u); // a hard landing may bend things, not tear the car apart
}
