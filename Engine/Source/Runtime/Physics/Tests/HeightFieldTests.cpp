// Height fields: bodies resting on slopes and bumps, rolling down hills, raycasts through the cell walk, sculpting
// (UpdateHeightField) waking what rests on the field.

#include "Core/Log/Log.h"
#include "Core/Testing/Test.h"

#include "Physics/PhysicsWorld.h"

using namespace zx;
using namespace zx::physics;

namespace {

constexpr f32 Dt = 1.0f / 60.0f;

void Run(PhysicsWorld& w, f32 seconds)
{
    for (u32 i = 0; i < u32(seconds / Dt); ++i)
        w.Step(Dt);
}

/// 65 x 65 samples, 1 m apart (64 m square), height = f(x, z) in field space.
template <typename Fn>
BodyId Field(PhysicsWorld& w, Fn&& height, HeightFieldId* idOut = nullptr)
{
    constexpr u32 N = 65;
    Array<f32> h;
    for (u32 z = 0; z < N; ++z)
        for (u32 x = 0; x < N; ++x)
            h.PushBack(height(f32(x) - 32.0f, f32(z) - 32.0f));
    const HeightFieldId id = w.CreateHeightField(N, N, h.View(), 1.0f, 1.0f);
    if (idOut)
        *idOut = id;
    BodyDesc d;
    d.shapes.PushBack(ShapeDesc::HeightField(id));
    return w.CreateBody(d);
}

BodyId Dynamic(PhysicsWorld& w, const ShapeDesc& shape, Vec3 pos)
{
    BodyDesc d;
    d.motion = MotionType::Dynamic;
    d.position = pos;
    d.shapes.PushBack(shape);
    return w.CreateBody(d);
}

} // namespace

ZX_TEST(HeightField, FlatFieldHoldsShapes)
{
    PhysicsWorld w;
    Field(w, [](f32, f32) { return 2.0f; });
    const BodyId box = Dynamic(w, ShapeDesc::Box(Vec3(0.5f)), Vec3(0.3f, 4.0f, 0.2f));
    const BodyId ball = Dynamic(w, ShapeDesc::Sphere(0.4f), Vec3(5.5f, 4.0f, -3.2f));
    const BodyId pill = Dynamic(w, ShapeDesc::Capsule(0.3f, 0.5f), Vec3(-6.0f, 4.0f, 7.0f));
    Run(w, 3.0f);
    ZX_EXPECT_NEAR(w.GetPosition(box).y, 2.5f, 0.03f);
    ZX_EXPECT_NEAR(w.GetPosition(ball).y, 2.4f, 0.03f);
    ZX_EXPECT(w.GetPosition(pill).y > 2.25f && w.GetPosition(pill).y < 3.1f); // fell over or standing, but on top
    ZX_EXPECT(Length(w.GetLinearVelocity(box)) < 0.05f);
}

ZX_TEST(HeightField, BallRollsDownhillAndBoxRestsOnGentleSlope)
{
    PhysicsWorld w;
    // A bowl: h = 0.02 * r^2 (center lowest).
    Field(w, [](f32 x, f32 z) { return 0.02f * (x * x + z * z); });
    const BodyId ball = Dynamic(w, ShapeDesc::Sphere(0.5f), Vec3(12.0f, 6.0f, 0.0f));
    // It rolls through the bottom and back (little rolling resistance), always on the surface.
    f32 nearest = Infinity;
    for (u32 i = 0; i < 600; ++i) {
        w.Step(Dt);
        const Vec3 p = w.GetPosition(ball);
        nearest = Min(nearest, Abs(p.x));
        ZX_EXPECT(p.y > 0.02f * (p.x * p.x + p.z * p.z) + 0.3f);
    }
    ZX_EXPECT(nearest < 1.0f);
    // A box on the gentle part of the slope (friction holds it).
    PhysicsWorld w2;
    Field(w2, [](f32 x, f32) { return x * 0.15f; });
    const BodyId box = Dynamic(w2, ShapeDesc::Box(Vec3(0.5f)), Vec3(0.0f, 1.0f, 0.0f));
    Run(w2, 3.0f);
    ZX_EXPECT(Abs(w2.GetPosition(box).x) < 0.3f);
    ZX_EXPECT_NEAR(w2.GetPosition(box).y, 0.5f / Cos(Atan(0.15f)), 0.06f);
}

ZX_TEST(HeightField, BumpsDoNotSnagSmallBodies)
{
    PhysicsWorld w;
    Field(w, [](f32 x, f32 z) { return Sin(x * 0.4f) * Cos(z * 0.3f) * 1.5f; });
    Array<BodyId> balls;
    for (u32 i = 0; i < 20; ++i)
        balls.PushBack(Dynamic(w, ShapeDesc::Sphere(0.3f), Vec3(f32(i % 5) * 4.0f - 8.0f, 5.0f, f32(i / 5) * 4.0f - 6.0f)));
    Run(w, 6.0f);
    for (BodyId b : balls) {
        const Vec3 p = w.GetPosition(b);
        RaycastHit hit;
        ZX_REQUIRE(w.Raycast(Ray(Vec3(p.x, 20.0f, p.z), Vec3(0, -1, 0)), 40.0f, hit, ~0u, b));
        ZX_EXPECT(p.y > hit.point.y); // above the ground under it
    }
}

ZX_TEST(HeightField, RaycastsWalkTheCells)
{
    PhysicsWorld w;
    Field(w, [](f32 x, f32) { return x > 10.0f ? 5.0f : 0.0f; }); // a cliff at x = 10..11
    RaycastHit hit;
    // Straight down.
    ZX_REQUIRE(w.Raycast(Ray(Vec3(-3.3f, 10, 4.7f), Vec3(0, -1, 0)), 100.0f, hit));
    ZX_EXPECT_NEAR(hit.distance, 10.0f, 1e-3f);
    ZX_EXPECT(Dot(hit.normal, Vec3(0, 1, 0)) > 0.999f);
    // Horizontal ray hits the cliff face (x between 10 and 11), not the plateau behind it.
    ZX_REQUIRE(w.Raycast(Ray(Vec3(-20, 2.0f, 0.5f), Vec3(1, 0, 0)), 100.0f, hit));
    ZX_EXPECT(hit.point.x > 10.0f && hit.point.x < 11.0f);
    ZX_EXPECT(hit.normal.x < -0.5f);
    // Diagonal ray from above the plateau into the valley.
    const Vec3 dir = Normalize(Vec3(-1.0f, -0.3f, 0.7f));
    ZX_REQUIRE(w.Raycast(Ray(Vec3(25, 8, -10), dir), 200.0f, hit));
    ZX_EXPECT_NEAR(hit.point.y, 5.0f, 1e-3f); // grazes the plateau exactly through a grid vertex
    ZX_EXPECT_NEAR(hit.point.x, 15.0f, 1e-2f);
    // Misses: outside the field, pointing up.
    ZX_EXPECT(!w.Raycast(Ray(Vec3(100, 10, 0), Vec3(0, -1, 0)), 100.0f, hit));
    ZX_EXPECT(!w.Raycast(Ray(Vec3(0, 1, 0), Vec3(0, 1, 0)), 100.0f, hit));
}

ZX_TEST(HeightField, SculptingWakesAndLifts)
{
    PhysicsWorld w;
    HeightFieldId id = InvalidHeightField;
    Field(w, [](f32, f32) { return 0.0f; }, &id);
    ZX_REQUIRE(id != InvalidHeightField);
    const BodyId box = Dynamic(w, ShapeDesc::Box(Vec3(0.5f)), Vec3(0.0f, 1.0f, 0.0f));
    Run(w, 3.0f);
    ZX_EXPECT_NEAR(w.GetPosition(box).y, 0.5f, 0.03f);
    // Raise a 9 x 9 patch under the box by 1 m (10 cm per frame, like a sculpt brush): it rides up on the new surface.
    for (u32 step = 1; step <= 10; ++step) {
        Array<f32> raised;
        for (u32 i = 0; i < 81; ++i)
            raised.PushBack(f32(step) * 0.1f);
        w.UpdateHeightField(id, 28, 28, 9, 9, raised.View());
        w.Step(Dt);
    }
    Run(w, 2.0f);
    ZX_EXPECT_NEAR(w.GetPosition(box).y, 1.5f, 0.05f);
    // Invalid input is rejected.
    const f32 one = 1.0f;
    ZX_EXPECT(w.CreateHeightField(1, 5, Span<const f32>(&one, 1), 1.0f, 1.0f) == InvalidHeightField);
}
