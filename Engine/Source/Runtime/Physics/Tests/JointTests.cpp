// ZPhys joints: pendulum period, hinge motor/limits, fixed cantilever, slider limits/motor, ball chain and cone,
// spring frequency, rope, no collision between connected bodies.

#include "Core/String/String.h"
#include "Core/Testing/Test.h"
#include "Physics/PhysicsWorld.h"

#include <cmath>

using namespace zx;
using namespace zx::physics;

namespace {

constexpr f32 Dt = 1.0f / 60.0f;

BodyId Dynamic(PhysicsWorld& w, Vec3 pos, ShapeDesc shape, f32 mass = 0.0f)
{
    BodyDesc d;
    d.motion = MotionType::Dynamic;
    d.position = pos;
    d.shapes.PushBack(shape);
    d.mass = mass;
    d.linearDamping = 0.0f;
    d.angularDamping = 0.0f;
    return w.CreateBody(d);
}

/// Times between successive upward zero crossings of `value` (averaged), over `seconds`.
template <typename Fn>
f32 MeasurePeriod(PhysicsWorld& w, f32 seconds, Fn&& value)
{
    f32 prev = value(), t = 0.0f, first = -1.0f, last = -1.0f;
    u32 crossings = 0;
    for (u32 i = 0; i < u32(seconds / Dt); ++i) {
        w.Step(Dt);
        t += Dt;
        const f32 v = value();
        if (prev < 0.0f && v >= 0.0f) {
            const f32 at = t - Dt * v / (v - prev);
            if (first < 0.0f)
                first = at;
            last = at;
            ++crossings;
        }
        prev = v;
    }
    return crossings > 1 ? (last - first) / f32(crossings - 1) : 0.0f;
}

} // namespace

ZX_TEST(PhysicsJoints, PendulumPeriod)
{
    PhysicsWorld w;
    // A small heavy ball on a 1 m hinge, released at 10 degrees.
    const f32 L = 1.0f, r = 0.05f, angle = DegToRad(10.0f);
    const BodyId ball = Dynamic(w, Vec3(Sin(angle) * L, 5.0f - Cos(angle) * L, 0.0f), ShapeDesc::Sphere(r));
    JointDesc j;
    j.type = JointType::Hinge;
    j.a = ball;
    j.anchor = Vec3(0, 5, 0);
    j.axis = Vec3(0, 0, 1);
    ZX_EXPECT(w.CreateJoint(j).IsValid());
    const f32 g = 9.81f;
    const f32 expected = 2.0f * Pi * Sqrt((L * L + 0.4f * r * r) / (g * L)) * (1.0f + angle * angle / 16.0f);
    const f32 period = MeasurePeriod(w, 12.0f, [&] { return w.GetPosition(ball).x; });
    if (Abs(period - expected) > expected * 0.01f)
        test::ReportFailure(__FILE__, __LINE__, String::Format("pendulum period {:.4f} s, expected {:.4f} s", period, expected));
    // The rod length holds.
    ZX_EXPECT(Abs(Length(w.GetPosition(ball) - Vec3(0, 5, 0)) - L) < 0.005f);
}

ZX_TEST(PhysicsJoints, HingeMotorAndLimits)
{
    PhysicsWorld w;
    // A door on a vertical hinge at its edge, driven by a motor.
    const BodyId door = Dynamic(w, Vec3(0.5f, 1, 0), ShapeDesc::Box(Vec3(0.5f, 1.0f, 0.05f)));
    JointDesc j;
    j.type = JointType::Hinge;
    j.a = door;
    j.anchor = Vec3(0, 1, 0);
    j.axis = Vec3(0, 1, 0);
    j.useMotor = true;
    j.motorSpeed = 2.0f;
    j.maxMotorForce = 500.0f;
    const JointId hinge = w.CreateJoint(j);
    for (u32 i = 0; i < 60; ++i)
        w.Step(Dt);
    ZX_EXPECT(Abs(w.GetAngularVelocity(door).y - 2.0f) < 0.05f);
    ZX_EXPECT(Abs(w.GetPosition(door).y - 1.0f) < 0.01f); // gravity doesn't sag it
    // Limits: the motor drives it against the upper stop.
    w.SetJointLimits(hinge, true, -0.5f, 0.5f);
    w.SetJointMotor(hinge, true, -3.0f, 500.0f);
    f32 minAngle = 10.0f, maxAngle = -10.0f;
    for (u32 i = 0; i < 180; ++i) {
        w.Step(Dt);
        const f32 a = w.GetJointCoordinate(hinge);
        if (i > 60) {
            minAngle = Min(minAngle, a);
            maxAngle = Max(maxAngle, a);
        }
    }
    if (minAngle < -0.52f || maxAngle > -0.48f)
        test::ReportFailure(__FILE__, __LINE__, String::Format("hinge limit: angle {:.3f}..{:.3f}", minAngle, maxAngle));
    // Without a motor, a spin bounces between the limits and stays inside them.
    w.SetJointMotor(hinge, false, 0.0f, 0.0f);
    w.SetAngularVelocity(door, Vec3(0, 8, 0));
    for (u32 i = 0; i < 120; ++i) {
        w.Step(Dt);
        const f32 a = w.GetJointCoordinate(hinge);
        if (a < -0.53f || a > 0.53f) {
            test::ReportFailure(__FILE__, __LINE__, String::Format("hinge limit exceeded: {:.3f}", a));
            break;
        }
    }
}

ZX_TEST(PhysicsJoints, FixedCantileverAndPair)
{
    PhysicsWorld w;
    // A 2 m beam welded to the world at one end.
    const BodyId beam = Dynamic(w, Vec3(1, 5, 0), ShapeDesc::Box(Vec3(1.0f, 0.1f, 0.1f)));
    JointDesc j;
    j.type = JointType::Fixed;
    j.a = beam;
    j.anchor = Vec3(0, 5, 0);
    w.CreateJoint(j);
    for (u32 i = 0; i < 180; ++i)
        w.Step(Dt);
    const Vec3 p = w.GetPosition(beam);
    ZX_EXPECT(Abs(p.y - 5.0f) < 0.01f && Abs(p.x - 1.0f) < 0.01f);
    ZX_EXPECT(Dot(w.GetRotation(beam) * Vec3(1, 0, 0), Vec3(1, 0, 0)) > 0.9999f);

    // Two welded boxes, overlapping (no contacts between them), dropped on the ground: they stay one piece.
    PhysicsWorld v;
    BodyDesc ground;
    ground.shapes.PushBack(ShapeDesc::Box(Vec3(20, 0.5f, 20)));
    ground.position = Vec3(0, -0.5f, 0);
    v.CreateBody(ground);
    const BodyId a = Dynamic(v, Vec3(0, 2, 0), ShapeDesc::Box(Vec3(0.5f)));
    const BodyId b = Dynamic(v, Vec3(0.8f, 2.3f, 0), ShapeDesc::Box(Vec3(0.5f)));
    JointDesc weld;
    weld.type = JointType::Fixed;
    weld.a = a;
    weld.b = b;
    weld.anchor = Vec3(0.4f, 2.15f, 0);
    const JointId id = v.CreateJoint(weld);
    for (u32 i = 0; i < 180; ++i)
        v.Step(Dt);
    const Vec3 offset = Conjugate(v.GetRotation(a)) * (v.GetPosition(b) - v.GetPosition(a));
    if (DistanceSq(offset, Vec3(0.8f, 0.3f, 0)) > 0.01f * 0.01f)
        test::ReportFailure(__FILE__, __LINE__, String::Format("weld offset ({:.3f}, {:.3f}, {:.3f})", offset.x, offset.y, offset.z));
    ZX_EXPECT(v.GetPosition(a).y < 1.0f); // fell and rests
    // Destroying a body removes its joints.
    ZX_EXPECT_EQ(v.JointCount(), 1u);
    v.DestroyBody(b);
    ZX_EXPECT_EQ(v.JointCount(), 0u);
    ZX_EXPECT(!v.IsValid(id));
}

ZX_TEST(PhysicsJoints, SliderLimitsAndMotor)
{
    PhysicsWorld w;
    // A vertical slider: falls until the lower limit 1 m down, keeps its X/Z and rotation.
    const BodyId box = Dynamic(w, Vec3(2, 5, 1), ShapeDesc::Box(Vec3(0.3f)));
    w.SetAngularVelocity(box, Vec3(1, 2, 3));
    JointDesc j;
    j.type = JointType::Slider;
    j.a = box;
    j.anchor = Vec3(2, 5, 1);
    j.axis = Vec3(0, 1, 0);
    j.useLimits = true;
    j.lower = -1.0f;
    j.upper = 0.5f;
    const JointId slider = w.CreateJoint(j);
    for (u32 i = 0; i < 120; ++i)
        w.Step(Dt);
    const Vec3 p = w.GetPosition(box);
    if (Abs(p.y - 4.0f) > 0.01f || Abs(p.x - 2.0f) > 0.005f || Abs(p.z - 1.0f) > 0.005f)
        test::ReportFailure(__FILE__, __LINE__, String::Format("slider at ({:.3f}, {:.3f}, {:.3f})", p.x, p.y, p.z));
    ZX_EXPECT(Abs(w.GetRotation(box).w) > 0.9999f);
    ZX_EXPECT(Abs(w.GetJointCoordinate(slider) + 1.0f) < 0.01f);
    // A motor lifts it at 1 m/s up to the upper limit.
    w.SetJointMotor(slider, true, 1.0f, 5000.0f); // 216 kg
    for (u32 i = 0; i < 60; ++i)
        w.Step(Dt);
    ZX_EXPECT(Abs(w.GetLinearVelocity(box).y - 1.0f) < 0.05f);
    for (u32 i = 0; i < 120; ++i)
        w.Step(Dt);
    ZX_EXPECT(Abs(w.GetJointCoordinate(slider) - 0.5f) < 0.01f);
}

ZX_TEST(PhysicsJoints, BallChainAndCone)
{
    PhysicsWorld w;
    // 10 capsule links hanging from the world, released horizontally.
    const u32 links = 10;
    const f32 len = 0.5f;
    Array<BodyId> chain;
    Array<JointId> joints;
    for (u32 i = 0; i < links; ++i) {
        const BodyId link = Dynamic(w, Vec3(len * (f32(i) + 0.5f), 10, 0), ShapeDesc::Capsule(0.05f, len * 0.5f - 0.05f, {}, Quat::FromAxisAngle(Vec3(0, 0, 1), Pi * 0.5f)));
        JointDesc j;
        j.type = JointType::Ball;
        j.a = link;
        j.b = i > 0 ? chain.Back() : BodyId{};
        j.anchor = Vec3(len * f32(i), 10, 0);
        joints.PushBack(w.CreateJoint(j));
        chain.PushBack(link);
    }
    f32 worstGap = 0.0f;
    for (u32 i = 0; i < 300; ++i) {
        w.Step(Dt);
        for (JointId id : joints) {
            Vec3 a, b;
            w.GetJointAnchors(id, a, b);
            worstGap = Max(worstGap, Distance(a, b));
        }
    }
    if (worstGap > 0.03f)
        test::ReportFailure(__FILE__, __LINE__, String::Format("chain stretched by {:.4f} m", worstGap));
    ZX_EXPECT(Length(w.GetLinearVelocity(chain.Back())) < 20.0f);

    // Cone: a hanging rod (like a ragdoll limb) hit sideways never swings past 30 degrees.
    PhysicsWorld c;
    const BodyId bob = Dynamic(c, Vec3(0, 4.5f, 0), ShapeDesc::Capsule(0.08f, 0.42f));
    JointDesc cone;
    cone.type = JointType::Ball;
    cone.a = bob;
    cone.anchor = Vec3(0, 5, 0);
    cone.axis = Vec3(0, -1, 0);
    cone.swingLimit = DegToRad(30.0f);
    c.CreateJoint(cone);
    c.SetLinearVelocity(bob, Vec3(3, 0, 1.5f));
    c.SetAngularVelocity(bob, Vec3(3, 0, -6));
    f32 worst = 0.0f;
    for (u32 i = 0; i < 180; ++i) {
        c.Step(Dt);
        const Vec3 d = c.GetRotation(bob) * Vec3(0, -1, 0);
        worst = Max(worst, RadToDeg(std::acos(Clamp(Dot(d, Vec3(0, -1, 0)), -1.0f, 1.0f))));
    }
    if (worst > 31.5f)
        test::ReportFailure(__FILE__, __LINE__, String::Format("cone exceeded: {:.2f} degrees", worst));
}

ZX_TEST(PhysicsJoints, SpringFrequencyAndRope)
{
    PhysicsWorld w;
    // An undamped 1 Hz spring without gravity, stretched by 0.5 m: oscillates with a 1 s period.
    const BodyId mass = Dynamic(w, Vec3(0, 0, 0), ShapeDesc::Sphere(0.1f), 3.0f);
    JointDesc j;
    j.type = JointType::Distance;
    j.a = mass;
    j.anchor = Vec3(0, 0, 0);
    j.anchorB = Vec3(-2, 0, 0);
    j.springHertz = 1.0f;
    j.springDamping = 0.0f;
    w.CreateJoint(j);
    w.Settings().gravity = Vec3();
    w.SetTransform(mass, Vec3(0.5f, 0, 0), Quat());
    const f32 period = MeasurePeriod(w, 6.0f, [&] { return w.GetPosition(mass).x; });
    if (Abs(period - 1.0f) > 0.03f)
        test::ReportFailure(__FILE__, __LINE__, String::Format("spring period {:.4f} s", period));

    // Rope: slack until it reaches 2 m, then it holds.
    PhysicsWorld r;
    const BodyId weight = Dynamic(r, Vec3(0, 4, 0), ShapeDesc::Sphere(0.1f));
    JointDesc rope;
    rope.type = JointType::Distance;
    rope.a = weight;
    rope.anchor = Vec3(0, 4, 0);
    rope.anchorB = Vec3(0, 5, 0);
    rope.maxDistance = 2.0f;
    r.CreateJoint(rope);
    for (u32 i = 0; i < 18; ++i) // 0.3 s of free fall (0.44 m)
        r.Step(Dt);
    ZX_EXPECT(Abs(r.GetPosition(weight).y - (4.0f - 0.5f * 9.81f * 0.09f)) < 0.03f);
    for (u32 i = 0; i < 180; ++i)
        r.Step(Dt);
    const f32 hang = Length(r.GetPosition(weight) - Vec3(0, 5, 0));
    if (Abs(hang - 2.0f) > 0.01f)
        test::ReportFailure(__FILE__, __LINE__, String::Format("rope length {:.4f}", hang));
}

ZX_TEST(PhysicsJoints, ConfigurableAndSleep)
{
    PhysicsWorld w;
    BodyDesc ground;
    ground.shapes.PushBack(ShapeDesc::Box(Vec3(20, 0.5f, 20)));
    ground.position = Vec3(0, -0.5f, 0);
    w.CreateBody(ground);
    // Free along X only (a configurable slider with locked rotation), limited to +-1 m: pushed sideways, it
    // stops at the limit.
    const BodyId box = Dynamic(w, Vec3(0, 3, 0), ShapeDesc::Box(Vec3(0.25f)));
    JointDesc j;
    j.type = JointType::Configurable;
    j.a = box;
    j.anchor = Vec3(0, 3, 0);
    j.axis = Vec3(1, 0, 0);
    j.linear[0] = AxisMotion::Limited;
    j.linearLimit = 1.0f;
    w.CreateJoint(j);
    w.SetLinearVelocity(box, Vec3(4, 0, 0));
    for (u32 i = 0; i < 120; ++i)
        w.Step(Dt);
    const Vec3 p = w.GetPosition(box);
    if (Abs(p.x - 1.0f) > 0.01f || Abs(p.y - 3.0f) > 0.01f)
        test::ReportFailure(__FILE__, __LINE__, String::Format("configurable at ({:.3f}, {:.3f}, {:.3f})", p.x, p.y, p.z));
    // A damped pendulum comes to rest and sleeps; poking it wakes it.
    BodyDesc d;
    d.motion = MotionType::Dynamic;
    d.position = Vec3(5, 3, 0);
    d.shapes.PushBack(ShapeDesc::Sphere(0.2f));
    d.linearDamping = 2.0f;
    d.angularDamping = 2.0f;
    const BodyId bob = w.CreateBody(d);
    JointDesc hinge;
    hinge.type = JointType::Hinge;
    hinge.a = bob;
    hinge.anchor = Vec3(4, 3, 0);
    hinge.axis = Vec3(0, 0, 1);
    w.CreateJoint(hinge);
    for (u32 i = 0; i < 900; ++i)
        w.Step(Dt);
    ZX_EXPECT(w.IsSleeping(bob));
    ZX_EXPECT(Abs(w.GetPosition(bob).x - 4.0f) < 0.02f && Abs(w.GetPosition(bob).y - 2.0f) < 0.02f);
    w.AddImpulse(bob, Vec3(2, 0, 0));
    w.Step(Dt);
    ZX_EXPECT(!w.IsSleeping(bob));
}

ZX_TEST(PhysicsJoints, BreakForce)
{
    // A 100 kg weight (981 N) on a ball joint: a strong one holds it, a weak one breaks.
    for (f32 limit : {2000.0f, 500.0f}) {
        PhysicsWorld w;
        const BodyId weight = Dynamic(w, Vec3(0, 4, 0), ShapeDesc::Sphere(0.2f), 100.0f);
        JointDesc j;
        j.type = JointType::Ball;
        j.a = weight;
        j.anchor = Vec3(0, 5, 0);
        j.breakForce = limit;
        const JointId id = w.CreateJoint(j);
        for (u32 i = 0; i < 60; ++i)
            w.Step(Dt);
        Array<JointId> broken;
        w.TakeBrokenJoints(broken);
        if (limit > 981.0f) {
            ZX_EXPECT(w.IsValid(id) && broken.Empty());
            ZX_EXPECT(Abs(w.GetPosition(weight).y - 4.0f) < 0.01f);
        } else {
            ZX_EXPECT(!w.IsValid(id) && broken.Size() == 1 && broken[0] == id);
            ZX_EXPECT(w.GetPosition(weight).y < 0.0f);
        }
    }
}
