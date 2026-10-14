// Queries and events added for M11 completion: closest point on every shape type, batched raycasts (same answers as
// one by one, in parallel), Stay events, and parallel islands giving the same result as serial solving.

#include "Core/Jobs/JobSystem.h"
#include "Core/Testing/Test.h"
#include "Physics/PhysicsWorld.h"


using namespace zx;
using namespace zx::physics;

namespace {

constexpr f32 Dt = 1.0f / 60.0f;

BodyId Static(PhysicsWorld& w, const ShapeDesc& shape, Vec3 pos, Quat rot = {}, u32 layer = 0)
{
    BodyDesc d;
    d.position = pos;
    d.rotation = rot;
    d.layer = layer;
    d.shapes.PushBack(shape);
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

ZX_TEST(Queries, ClosestPointOnEveryShape)
{
    PhysicsWorld w;
    const BodyId box = Static(w, ShapeDesc::Box(Vec3(1.0f)), Vec3(0, 0, 0));
    const BodyId ball = Static(w, ShapeDesc::Sphere(0.5f), Vec3(10, 0, 0));
    const BodyId pill = Static(w, ShapeDesc::Capsule(0.5f, 1.0f), Vec3(20, 0, 0));
    const Vec3 cube[8] = {Vec3(-1, -1, -1), Vec3(1, -1, -1), Vec3(-1, 1, -1), Vec3(1, 1, -1),
                          Vec3(-1, -1, 1),  Vec3(1, -1, 1),  Vec3(-1, 1, 1),  Vec3(1, 1, 1)};
    const BodyId hull = Static(w, ShapeDesc::Hull(w.CreateConvexHull(Span<const Vec3>(cube, 8))), Vec3(30, 0, 0));
    const f32 flat[16] = {};
    const BodyId field = Static(w, ShapeDesc::HeightField(w.CreateHeightField(4, 4, Span<const f32>(flat, 16), 2.0f, 2.0f)), Vec3(40, 0, 0));
    RaycastHit hit;
    // Box: from the side and from a corner region.
    ZX_REQUIRE(w.ClosestPoint(Vec3(3, 0.5f, 0), 5.0f, hit));
    ZX_EXPECT(hit.body == box);
    ZX_EXPECT_NEAR(hit.distance, 2.0f, 1e-3f);
    ZX_EXPECT(Distance(hit.point, Vec3(1, 0.5f, 0)) < 1e-2f);
    ZX_EXPECT(hit.normal.x > 0.99f);
    // Inside the box: distance 0.
    ZX_REQUIRE(w.ClosestPoint(Vec3(0.2f, 0.1f, 0), 5.0f, hit));
    ZX_EXPECT(hit.body == box && hit.distance == 0.0f);
    // Sphere and capsule.
    ZX_REQUIRE(w.ClosestPoint(Vec3(10, 2, 0), 5.0f, hit));
    ZX_EXPECT(hit.body == ball);
    ZX_EXPECT_NEAR(hit.distance, 1.5f, 1e-3f);
    ZX_REQUIRE(w.ClosestPoint(Vec3(22, 0.8f, 0), 5.0f, hit));
    ZX_EXPECT(hit.body == pill);
    ZX_EXPECT_NEAR(hit.distance, 1.5f, 1e-3f);
    // Convex hull (same cube) and the height field below.
    ZX_REQUIRE(w.ClosestPoint(Vec3(30, 3, 0.2f), 5.0f, hit));
    ZX_EXPECT(hit.body == hull);
    ZX_EXPECT_NEAR(hit.distance, 2.0f, 1e-3f);
    ZX_REQUIRE(w.ClosestPoint(Vec3(40.3f, 1.25f, 0.4f), 5.0f, hit));
    ZX_EXPECT(hit.body == field);
    ZX_EXPECT_NEAR(hit.distance, 1.25f, 1e-3f);
    // Out of range, and filtered by ignore.
    ZX_EXPECT(!w.ClosestPoint(Vec3(5, 0, 0), 1.0f, hit));
    ZX_REQUIRE(w.ClosestPoint(Vec3(5, 0, 0), 6.0f, hit, ~0u, box));
    ZX_EXPECT(hit.body == ball);
}

ZX_TEST(Queries, RaycastBatchMatchesSingleRaysInParallel)
{
    jobs::Initialize();
    struct JobsShutdown
    {
        ~JobsShutdown() { jobs::Shutdown(); }
    } shutdown;
    PhysicsWorld w;
    for (u32 i = 0; i < 40; ++i)
        Static(w, ShapeDesc::Box(Vec3(0.5f)), Vec3(f32(i % 8) * 2.0f - 7.0f, f32(i / 8) * 2.0f, 5.0f), {}, i % 2);
    Array<RayQuery> rays;
    for (u32 i = 0; i < 500; ++i) {
        RayQuery q;
        q.ray = Ray(Vec3(f32(i % 25) * 0.6f - 7.5f, f32(i / 25) * 0.5f - 0.5f, 0.0f), Vec3(0, 0, 1));
        q.maxDistance = 20.0f;
        q.layerMask = i % 3 == 0 ? 1u : ~0u;
        rays.PushBack(q);
    }
    Array<RaycastHit> hits;
    hits.Resize(rays.Size());
    Array<bool> found;
    found.Resize(rays.Size());
    w.RaycastBatch(rays.View(), hits.View(), found.View());
    u32 hitCount = 0;
    for (usize i = 0; i < rays.Size(); ++i) {
        RaycastHit single;
        const bool ok = w.Raycast(rays[i].ray, rays[i].maxDistance, single, rays[i].layerMask);
        ZX_EXPECT_EQ(ok, found[i]);
        if (ok && found[i]) {
            ZX_EXPECT(single.body == hits[i].body);
            ZX_EXPECT(single.distance == hits[i].distance);
            ++hitCount;
        }
    }
    ZX_EXPECT(hitCount > 50);
}

ZX_TEST(Queries, StayEventsWhileTouching)
{
    PhysicsWorld w;
    w.Settings().stayEvents = true;
    const BodyId ground = Static(w, ShapeDesc::Box(Vec3(10, 0.5f, 10)), Vec3(0, -0.5f, 0));
    ShapeDesc zone = ShapeDesc::Box(Vec3(2.0f));
    zone.isSensor = true;
    const BodyId sensor = Static(w, zone, Vec3(0, 0, 0));
    const BodyId ball = Dynamic(w, ShapeDesc::Sphere(0.3f), Vec3(0.5f, 1.0f, 0.5f));
    Array<ContactEvent> events;
    u32 touchStay = 0, sensorStay = 0, begins = 0;
    for (u32 i = 0; i < 120; ++i) {
        w.Step(Dt);
        events.Clear();
        w.TakeEvents(events);
        for (const ContactEvent& e : events) {
            const bool withGround = e.a == ground || e.b == ground, withSensor = e.a == sensor || e.b == sensor;
            const bool withBall = e.a == ball || e.b == ball;
            if (!withBall)
                continue;
            begins += e.type == ContactEventType::TouchBegin && withGround;
            touchStay += e.type == ContactEventType::TouchStay && withGround;
            sensorStay += e.type == ContactEventType::SensorStay && withSensor;
        }
    }
    ZX_EXPECT_EQ(begins, 1u);
    ZX_EXPECT(touchStay > 60); // resting on the ground: one per step after landing (sleeping keeps the contact)
    ZX_EXPECT(sensorStay > 100); // inside the sensor the whole time
    // Off by default.
    PhysicsWorld quiet;
    Static(quiet, ShapeDesc::Box(Vec3(10, 0.5f, 10)), Vec3(0, -0.5f, 0));
    Dynamic(quiet, ShapeDesc::Sphere(0.3f), Vec3(0, 0.5f, 0));
    for (u32 i = 0; i < 30; ++i)
        quiet.Step(Dt);
    events.Clear();
    quiet.TakeEvents(events);
    for (const ContactEvent& e : events)
        ZX_EXPECT(e.type != ContactEventType::TouchStay);
}

ZX_TEST(Queries, ParallelIslandsMatchSerial)
{
    // Separate stacks are separate islands: solved in parallel they end exactly where serial solving puts them.
    bool usedParallel = false;
    auto simulate = [&](bool withJobs, Array<Vec3>& out) {
        if (withJobs)
            jobs::Initialize();
        PhysicsWorld w;
        Static(w, ShapeDesc::Box(Vec3(50, 0.5f, 50)), Vec3(0, -0.5f, 0));
        Array<BodyId> boxes;
        for (u32 s = 0; s < 24; ++s)
            for (u32 i = 0; i < 14; ++i)
                boxes.PushBack(Dynamic(w, ShapeDesc::Box(Vec3(0.5f)), Vec3(f32(s % 6) * 5.0f, 0.5f + f32(i) * 1.05f, f32(s / 6) * 5.0f + f32(i) * 0.03f)));
        for (u32 i = 0; i < 90; ++i) {
            w.Step(Dt);
            usedParallel |= withJobs && w.Stats().parallelIslands;
        }
        out.Clear();
        for (BodyId b : boxes)
            out.PushBack(w.GetPosition(b));
        if (withJobs)
            jobs::Shutdown();
    };
    Array<Vec3> serial, parallel;
    simulate(false, serial);
    simulate(true, parallel);
    ZX_REQUIRE(serial.Size() == parallel.Size());
    bool same = true;
    for (usize i = 0; i < serial.Size(); ++i)
        same &= serial[i] == parallel[i];
    ZX_EXPECT(same);
    ZX_EXPECT(usedParallel);
}

ZX_TEST(Queries, RollingFrictionStopsBalls)
{
    // Two balls rolling at 4 m/s on flat ground: without rolling friction one keeps going (only damping slows it),
    // with 0.1 it stops within a few meters (s = v^2 / (2 * g * mu_r * 5/7) for a solid sphere: ~11 m).
    f32 travelled[2] = {};
    for (u32 pass = 0; pass < 2; ++pass) {
        PhysicsWorld w;
        Static(w, ShapeDesc::Box(Vec3(200, 0.5f, 5)), Vec3(0, -0.5f, 0));
        BodyDesc d;
        d.motion = MotionType::Dynamic;
        d.position = Vec3(0, 0.3f, 0);
        d.linearVelocity = Vec3(4, 0, 0);
        d.angularVelocity = Vec3(0, 0, -4.0f / 0.3f); // already rolling
        d.linearDamping = d.angularDamping = 0.0f;
        d.rollingFriction = pass == 1 ? 0.1f : 0.0f;
        d.shapes.PushBack(ShapeDesc::Sphere(0.3f));
        const BodyId ball = w.CreateBody(d);
        for (u32 i = 0; i < 600; ++i)
            w.Step(Dt);
        travelled[pass] = w.GetPosition(ball).x;
        if (pass == 1)
            ZX_EXPECT(Length(w.GetLinearVelocity(ball)) < 0.05f);
    }
    ZX_EXPECT(travelled[0] > 35.0f); // 10 s at ~4 m/s
    ZX_EXPECT(travelled[1] > 5.0f && travelled[1] < 20.0f);
}
