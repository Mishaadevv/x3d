// ZPhys: resting, stacking (M11 acceptance: 20 cubes stable for 60 s), bouncing, friction, meshes, raycasts, sleep,
// determinism. ManualPhysics.* = benchmarks (500 bodies).

#include "Core/Jobs/JobSystem.h"
#include "Core/Math/Random.h"
#include "Core/Testing/Test.h"
#include "Core/Time/Time.h"
#include "Physics/PhysicsWorld.h"

#include <cstdio>

using namespace zx;
using namespace zx::physics;

namespace {

constexpr f32 Dt = 1.0f / 60.0f;

BodyId Ground(PhysicsWorld& w, f32 friction = 0.6f)
{
    BodyDesc d;
    d.shapes.PushBack(ShapeDesc::Box(Vec3(50.0f, 0.5f, 50.0f)));
    d.position = Vec3(0.0f, -0.5f, 0.0f);
    d.friction = friction;
    return w.CreateBody(d);
}

BodyId DynamicBox(PhysicsWorld& w, Vec3 pos, Vec3 half = Vec3(0.5f), Quat rot = {})
{
    BodyDesc d;
    d.motion = MotionType::Dynamic;
    d.position = pos;
    d.rotation = rot;
    d.shapes.PushBack(ShapeDesc::Box(half));
    return w.CreateBody(d);
}

void Run(PhysicsWorld& w, f32 seconds)
{
    for (u32 i = 0; i < u32(seconds / Dt); ++i)
        w.Step(Dt);
}

} // namespace

ZX_TEST(Physics, SphereRestsOnGroundAndSleeps)
{
    PhysicsWorld w;
    Ground(w);
    BodyDesc d;
    d.motion = MotionType::Dynamic;
    d.position = Vec3(0.0f, 3.0f, 0.0f);
    d.shapes.PushBack(ShapeDesc::Sphere(0.5f));
    const BodyId s = w.CreateBody(d);
    Run(w, 4.0f);
    ZX_EXPECT(Abs(w.GetPosition(s).y - 0.5f) < 0.02f);
    ZX_EXPECT(Length(w.GetLinearVelocity(s)) < 0.05f);
    ZX_EXPECT(w.IsSleeping(s));
    ZX_EXPECT(Abs(w.GetMass(s) - 1000.0f * 4.0f / 3.0f * Pi * 0.125f) < 1.0f);
}

ZX_TEST(Physics, BoxesRestFlatWithoutJitter)
{
    PhysicsWorld w;
    Ground(w);
    const BodyId a = DynamicBox(w, Vec3(0.0f, 0.6f, 0.0f), Vec3(0.5f), Quat::FromEulerDegrees(Vec3(0.0f, 30.0f, 0.0f)));
    const BodyId b = DynamicBox(w, Vec3(0.2f, 1.8f, 0.1f), Vec3(0.4f), Quat::FromEulerDegrees(Vec3(0.0f, 10.0f, 0.0f)));
    // Tilted box falls and settles on a face.
    const BodyId c = DynamicBox(w, Vec3(3.0f, 2.0f, 0.0f), Vec3(0.5f), Quat::FromEulerDegrees(Vec3(35.0f, 0.0f, 20.0f)));
    Run(w, 5.0f);
    ZX_EXPECT(Abs(w.GetPosition(a).y - 0.5f) < 0.02f);
    ZX_EXPECT(Abs(w.GetPosition(b).y - 1.4f) < 0.03f);
    ZX_EXPECT(Abs(w.GetPosition(c).y - 0.5f) < 0.03f);
    // Upright: the box's up axis is aligned with one of the world axes.
    const Vec3 up = w.GetRotation(c) * Vec3(0, 1, 0);
    ZX_EXPECT(Max(Abs(up.x), Max(Abs(up.y), Abs(up.z))) > 0.995f);
    ZX_EXPECT(w.IsSleeping(a) && w.IsSleeping(b) && w.IsSleeping(c));
}

ZX_TEST(Physics, StackOf20CubesStableFor60Seconds)
{
    // M11 acceptance: 20 cubes stay stacked for a minute.
    PhysicsWorld w;
    Ground(w);
    Array<BodyId> boxes;
    for (u32 i = 0; i < 20; ++i)
        boxes.PushBack(DynamicBox(w, Vec3(0.0f, 0.5f + f32(i) * 1.0f, 0.0f)));
    f32 maxDrift = 0.0f;
    for (u32 step = 0; step < 60 * 60; ++step) {
        w.Step(Dt);
        if (step % 60 == 0)
            for (u32 i = 0; i < 20; ++i) {
                const Vec3 p = w.GetPosition(boxes[i]);
                maxDrift = Max(maxDrift, Max(Abs(p.x), Abs(p.z)));
            }
    }
    const Vec3 top = w.GetPosition(boxes.Back());
    if (maxDrift > 0.05f || Abs(top.y - 19.5f) > 0.1f)
        test::ReportFailure(__FILE__, __LINE__, String::Format("stack moved: drift {:.3f} m, top at {:.3f} m", maxDrift, top.y));
    ZX_EXPECT(w.IsSleeping(boxes.Back()));
}

ZX_TEST(Physics, CapsuleAndSphereStacks)
{
    PhysicsWorld w;
    Ground(w);
    BodyDesc d;
    d.motion = MotionType::Dynamic;
    d.position = Vec3(0.0f, 2.0f, 0.0f);
    d.rotation = Quat::FromEulerDegrees(Vec3(0.0f, 0.0f, 90.0f)); // lying along X
    d.shapes.PushBack(ShapeDesc::Capsule(0.3f, 0.6f));
    const BodyId cap = w.CreateBody(d);
    // Standing capsule tips over or stays: either way it must end up resting on the ground.
    BodyDesc s;
    s.motion = MotionType::Dynamic;
    s.position = Vec3(0.0f, 4.0f, 0.0f);
    s.shapes.PushBack(ShapeDesc::Sphere(0.25f));
    const BodyId sphere = w.CreateBody(s);
    Run(w, 6.0f);
    ZX_EXPECT(Abs(w.GetPosition(cap).y - 0.3f) < 0.03f);
    ZX_EXPECT(w.GetPosition(sphere).y > 0.2f);
    ZX_EXPECT(Length(w.GetLinearVelocity(sphere)) < 0.1f);
}

ZX_TEST(Physics, RestitutionBounces)
{
    PhysicsWorld w;
    const BodyId g = Ground(w);
    (void)g;
    BodyDesc d;
    d.motion = MotionType::Dynamic;
    d.position = Vec3(0.0f, 5.5f, 0.0f);
    d.restitution = 0.8f;
    d.linearDamping = 0.0f;
    d.shapes.PushBack(ShapeDesc::Sphere(0.5f));
    const BodyId s = w.CreateBody(d);
    f32 peakAfterBounce = 0.0f;
    bool bounced = false;
    for (u32 i = 0; i < 240; ++i) {
        w.Step(Dt);
        const f32 vy = w.GetLinearVelocity(s).y;
        if (vy > 1.0f)
            bounced = true;
        if (bounced)
            peakAfterBounce = Max(peakAfterBounce, w.GetPosition(s).y - 0.5f);
    }
    // Dropped from 5 m: e^2 * 5 = 3.2 m.
    if (!bounced || Abs(peakAfterBounce - 3.2f) > 0.5f)
        test::ReportFailure(__FILE__, __LINE__, String::Format("bounce peak {:.2f} m (expected ~3.2)", peakAfterBounce));
}

ZX_TEST(Physics, FrictionHoldsOnSlope)
{
    // A 20-degree ramp: friction 0.8 holds a box, friction 0.05 lets it slide.
    for (f32 friction : {0.8f, 0.05f}) {
        PhysicsWorld w;
        BodyDesc ramp;
        ramp.rotation = Quat::FromEulerDegrees(Vec3(0.0f, 0.0f, 20.0f));
        ramp.shapes.PushBack(ShapeDesc::Box(Vec3(10.0f, 0.5f, 3.0f)));
        ramp.friction = friction;
        w.CreateBody(ramp);
        BodyDesc box;
        box.motion = MotionType::Dynamic;
        box.rotation = ramp.rotation;
        box.position = ramp.rotation * Vec3(0.0f, 1.0f, 0.0f);
        box.friction = friction;
        box.shapes.PushBack(ShapeDesc::Box(Vec3(0.5f)));
        const BodyId b = w.CreateBody(box);
        const Vec3 start = w.GetPosition(b);
        Run(w, 2.0f);
        const f32 moved = Distance(w.GetPosition(b), start);
        if (friction > 0.5f ? moved > 0.05f : moved < 1.0f)
            test::ReportFailure(__FILE__, __LINE__, String::Format("friction {}: moved {:.3f} m", friction, moved));
    }
}

ZX_TEST(Physics, TriangleMeshGround)
{
    PhysicsWorld w;
    // A 20 x 20 m grid of 2 x 2 m quads (many triangles: shared edges must not bump things).
    Array<Vec3> pos;
    Array<u32> idx;
    constexpr u32 N = 10;
    for (u32 z = 0; z <= N; ++z)
        for (u32 x = 0; x <= N; ++x)
            pos.PushBack(Vec3(f32(x) * 2.0f - 10.0f, 0.0f, f32(z) * 2.0f - 10.0f));
    for (u32 z = 0; z < N; ++z)
        for (u32 x = 0; x < N; ++x) {
            const u32 i = z * (N + 1) + x;
            idx.Append({i, i + N + 1, i + 1, i + 1, i + N + 1, i + N + 2}); // counter-clockwise seen from above
        }
    const MeshId mesh = w.CreateTriangleMesh(pos.View(), idx.View());
    BodyDesc ground;
    ground.shapes.PushBack(ShapeDesc::Mesh(mesh));
    w.CreateBody(ground);
    const BodyId box = DynamicBox(w, Vec3(1.0f, 2.0f, 1.0f), Vec3(0.5f), Quat::FromEulerDegrees(Vec3(0.0f, 20.0f, 0.0f)));
    BodyDesc s;
    s.motion = MotionType::Dynamic;
    s.position = Vec3(-3.0f, 1.0f, 0.0f);
    s.linearVelocity = Vec3(4.0f, 0.0f, 0.0f); // rolls across many triangle edges
    s.shapes.PushBack(ShapeDesc::Sphere(0.5f));
    const BodyId sphere = w.CreateBody(s);
    f32 maxSphereY = 0.0f;
    for (u32 i = 0; i < 240; ++i) {
        w.Step(Dt);
        if (i > 60)
            maxSphereY = Max(maxSphereY, w.GetPosition(sphere).y);
    }
    ZX_EXPECT(Abs(w.GetPosition(box).y - 0.5f) < 0.03f);
    ZX_EXPECT(Abs(w.GetPosition(sphere).y - 0.5f) < 0.03f);
    ZX_EXPECT(maxSphereY < 0.55f); // no bumps on internal edges
    ZX_EXPECT(w.GetPosition(sphere).x > 0.0f); // it rolled
}

ZX_TEST(Physics, RaycastLayersAndShapes)
{
    PhysicsWorld w;
    Ground(w);
    BodyDesc d;
    d.position = Vec3(0.0f, 1.0f, 5.0f);
    d.shapes.PushBack(ShapeDesc::Sphere(1.0f));
    d.layer = 3;
    const BodyId sphere = w.CreateBody(d);
    BodyDesc c;
    c.position = Vec3(0.0f, 1.0f, 10.0f);
    c.shapes.PushBack(ShapeDesc::Capsule(0.5f, 1.0f));
    const BodyId capsule = w.CreateBody(c);
    RaycastHit hit;
    ZX_REQUIRE(w.Raycast(Ray(Vec3(0.0f, 1.0f, 0.0f), Vec3(0, 0, 1)), 100.0f, hit));
    ZX_EXPECT(hit.body == sphere && Abs(hit.distance - 4.0f) < 1e-3f && hit.normal.z < -0.99f);
    ZX_REQUIRE(w.Raycast(Ray(Vec3(0.0f, 1.0f, 0.0f), Vec3(0, 0, 1)), 100.0f, hit, ~(1u << 3)));
    ZX_EXPECT(hit.body == capsule && Abs(hit.distance - 9.5f) < 1e-3f);
    ZX_REQUIRE(w.Raycast(Ray(Vec3(2.0f, 5.0f, 0.0f), Vec3(0, -1, 0)), 100.0f, hit));
    ZX_EXPECT(Abs(hit.point.y) < 1e-3f && hit.normal.y > 0.99f);
    ZX_EXPECT(!w.Raycast(Ray(Vec3(0.0f, 5.0f, 0.0f), Vec3(0, 1, 0)), 100.0f, hit));
}

ZX_TEST(Physics, CompoundCenterOfMassAndLayers)
{
    PhysicsWorld w;
    Ground(w);
    // Dumbbell: heavy sphere at one end; the center of mass sits near it.
    BodyDesc d;
    d.motion = MotionType::Dynamic;
    d.position = Vec3(0.0f, 2.0f, 0.0f);
    ShapeDesc heavy = ShapeDesc::Sphere(0.5f, Vec3(1.0f, 0.0f, 0.0f));
    heavy.density = 3000.0f;
    d.shapes.PushBack(heavy);
    d.shapes.PushBack(ShapeDesc::Sphere(0.5f, Vec3(-1.0f, 0.0f, 0.0f)));
    const BodyId b = w.CreateBody(d);
    ZX_EXPECT(Abs(w.GetCenterOfMass(b).x - 0.5f) < 1e-3f);
    // Layers that don't collide fall through each other.
    w.SetLayersCollide(1, 0, false);
    BodyDesc ghost;
    ghost.motion = MotionType::Dynamic;
    ghost.layer = 1;
    ghost.position = Vec3(5.0f, 1.0f, 0.0f);
    ghost.shapes.PushBack(ShapeDesc::Box(Vec3(0.3f)));
    const BodyId g = w.CreateBody(ghost);
    Run(w, 2.0f);
    ZX_EXPECT(w.GetPosition(g).y < -5.0f);
    ZX_EXPECT(Abs(w.GetPosition(b).y - 0.5f) < 0.03f);
}

ZX_TEST(Physics, KinematicPushesAndSleepersWake)
{
    PhysicsWorld w;
    Ground(w);
    const BodyId box = DynamicBox(w, Vec3(0.0f, 0.5f, 0.0f));
    Run(w, 2.0f);
    ZX_EXPECT(w.IsSleeping(box));
    BodyDesc k;
    k.motion = MotionType::Kinematic;
    k.position = Vec3(-3.0f, 0.5f, 0.0f);
    k.shapes.PushBack(ShapeDesc::Box(Vec3(0.5f)));
    const BodyId pusher = w.CreateBody(k);
    for (u32 i = 0; i < 120; ++i) {
        const Vec3 target(-3.0f + f32(i + 1) * Dt * 2.0f, 0.5f, 0.0f); // 2 m/s for 2 s
        w.MoveKinematic(pusher, target, Quat(), Dt);
        w.Step(Dt);
    }
    ZX_EXPECT(Abs(w.GetPosition(pusher).x - 1.0f) < 0.01f);
    ZX_EXPECT(w.GetPosition(box).x > 1.4f); // pushed ahead of it
}

ZX_TEST(Physics, Deterministic)
{
    auto simulate = [](Array<Vec3>& out) {
        PhysicsWorld w;
        Ground(w);
        Random rng(3);
        Array<BodyId> ids;
        for (u32 i = 0; i < 40; ++i)
            ids.PushBack(DynamicBox(w, Vec3(rng.Range(-2.0f, 2.0f), 1.0f + f32(i) * 0.6f, rng.Range(-2.0f, 2.0f)), Vec3(0.3f),
                                    Quat::FromEulerDegrees(Vec3(rng.Range(0.0f, 90.0f), rng.Range(0.0f, 90.0f), 0.0f))));
        Run(w, 3.0f);
        for (BodyId id : ids)
            out.PushBack(w.GetPosition(id));
    };
    Array<Vec3> a, b;
    simulate(a);
    simulate(b);
    bool same = a.Size() == b.Size();
    for (usize i = 0; same && i < a.Size(); ++i)
        same = a[i] == b[i];
    ZX_EXPECT(same);
}

ZX_TEST(ManualPhysics, Pile500Bodies)
{
    // M11 acceptance: 500 bodies <= 3 ms per step.
    jobs::Initialize();
    struct JobsShutdown
    {
        ~JobsShutdown() { jobs::Shutdown(); }
    } shutdown;
    PhysicsWorld w;
    Ground(w);
    Random rng(9);
    for (u32 i = 0; i < 500; ++i) {
        const Vec3 p(rng.Range(-6.0f, 6.0f), 0.5f + f32(i / 50) * 1.2f, rng.Range(-6.0f, 6.0f));
        if (i % 3 == 0) {
            BodyDesc d;
            d.motion = MotionType::Dynamic;
            d.position = p;
            d.shapes.PushBack(ShapeDesc::Sphere(0.35f));
            w.CreateBody(d);
        } else {
            DynamicBox(w, p, Vec3(0.35f), Quat::FromEulerDegrees(Vec3(0.0f, rng.Range(0.0f, 90.0f), 0.0f)));
        }
    }
    f64 total = 0.0, worst = 0.0, bp = 0.0, np = 0.0, sv = 0.0, bounds = 0.0;
    u32 steps = 0;
    for (u32 i = 0; i < 600; ++i) {
        w.Step(Dt);
        if (i >= 60) {
            bp += w.Stats().broadphaseMs;
            bounds += w.Stats().boundsMs;
            np += w.Stats().narrowphaseMs;
            sv += w.Stats().solverMs;
            total += w.Stats().stepMs;
            worst = Max(worst, w.Stats().stepMs);
            ++steps;
        }
    }
    const PhysicsStats& s = w.Stats();
    std::printf("500 bodies: %.2f ms average (broadphase %.2f [bounds %.2f], narrowphase %.2f, solver %.2f), %.2f ms worst; awake %u, contacts %u, points %u\n",
                total / steps, bp / steps, bounds / steps, np / steps, sv / steps, worst, s.awake, s.contacts, s.points);
}
