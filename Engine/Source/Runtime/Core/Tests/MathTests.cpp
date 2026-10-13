#include "Core/Math/Math.h"
#include "Core/Testing/Test.h"

using namespace zx;

#define EXPECT_VEC3(a, b) ZX_EXPECT(IsNearlyEqual((a), (b), 1e-4f))

ZX_TEST(Math, VectorBasics)
{
    const Vec3 a(1, 2, 3), b(4, 5, 6);
    ZX_EXPECT_EQ(Dot(a, b), 32.0f);
    EXPECT_VEC3(Cross(Vec3::UnitX(), Vec3::UnitY()), Vec3::UnitZ());
    EXPECT_VEC3(Cross(Vec3::Up(), Vec3::Forward()), Vec3::Right()); // left-handed basis consistency
    ZX_EXPECT_NEAR(Length(Normalize(b)), 1.0f, 1e-6f);
    EXPECT_VEC3(Reflect(Vec3(1, -1, 0), Vec3::Up()), Vec3(1, 1, 0));
    Vec3 t, bt;
    OrthonormalBasis(Normalize(Vec3(0.3f, 0.5f, -0.8f)), t, bt);
    ZX_EXPECT_NEAR(Dot(t, bt), 0.0f, 1e-5f);
    ZX_EXPECT_NEAR(Length(t), 1.0f, 1e-5f);
    ZX_EXPECT(IVec2(1, 2) + IVec2(3, 4) == IVec2(4, 6));
    char buf[64];
    FormatToBuffer(buf, sizeof(buf), "{:.1f}", Vec3(1, 2, 3));
    ZX_EXPECT(StringView(buf) == "(1.0, 2.0, 3.0)");
}

ZX_TEST(Math, MatrixInverseAndTRS)
{
    const Quat r = Quat::FromEuler(0.3f, 1.1f, -0.4f);
    const Mat4 m = Mat4::TRS(Vec3(1, 2, 3), r, Vec3(2, 3, 0.5f));
    const Mat4 inv = Inverse(m);
    ZX_EXPECT(IsNearlyEqual(m * inv, Mat4::Identity(), 1e-5f));
    ZX_EXPECT(IsNearlyEqual(InverseAffine(m), inv, 1e-5f));
    ZX_EXPECT_NEAR(Determinant(m), 2.0f * 3.0f * 0.5f, 1e-4f);
    ZX_EXPECT(IsNearlyEqual(Transpose(Transpose(m)), m, 0.0f));

    Vec3 t, s;
    Quat q;
    ZX_EXPECT(Decompose(m, t, q, s));
    EXPECT_VEC3(t, Vec3(1, 2, 3));
    EXPECT_VEC3(s, Vec3(2, 3, 0.5f));
    ZX_EXPECT(IsNearlyEqual(q, r, 1e-4f));

    // Rotation conventions: yaw +90 turns forward (+Z) into right (+X).
    EXPECT_VEC3(Mat4::RotationY(HalfPi).TransformVector(Vec3::Forward()), Vec3::Right());
    EXPECT_VEC3(Quat::FromAxisAngle(Vec3::Up(), HalfPi) * Vec3::Forward(), Vec3::Right());
    // Matrix and quaternion agree for all axes.
    for (Vec3 axis : {Vec3::UnitX(), Vec3::UnitY(), Vec3::UnitZ()}) {
        const Quat qa = Quat::FromAxisAngle(axis, 0.7f);
        const Mat4 ma = axis.x != 0.0f ? Mat4::RotationX(0.7f)
                                       : (axis.y != 0.0f ? Mat4::RotationY(0.7f) : Mat4::RotationZ(0.7f));
        ZX_EXPECT(IsNearlyEqual(Mat4::FromQuat(qa), ma, 1e-5f));
    }
    // Composition order: T * S applies S first.
    const Mat4 composed = Mat4::Translation(Vec3(5, 0, 0)) * Mat4::Scale(Vec3(2));
    EXPECT_VEC3(composed.TransformPoint(Vec3(1, 0, 0)), Vec3(7, 0, 0));
}

ZX_TEST(Math, QuaternionEuler)
{
    const Vec3 angles[] = {{0.1f, 0.2f, 0.3f}, {-1.2f, 2.5f, 0.7f}, {1.0f, -3.0f, -1.5f}, {0, 0, 0}};
    for (const Vec3& e : angles) {
        const Quat q = Quat::FromEuler(e);
        // FromEuler == yaw * pitch * roll
        const Quat composed = Quat::FromAxisAngle(Vec3::UnitY(), e.y) * Quat::FromAxisAngle(Vec3::UnitX(), e.x) *
                              Quat::FromAxisAngle(Vec3::UnitZ(), e.z);
        ZX_EXPECT(IsNearlyEqual(q, composed, 1e-5f));
        const Quat back = Quat::FromEuler(q.ToEuler());
        ZX_EXPECT(IsNearlyEqual(q, back, 1e-4f));
    }
    // Gimbal lock round-trip still reproduces the rotation.
    const Quat g = Quat::FromEuler(HalfPi, 0.5f, 0.2f);
    ZX_EXPECT(IsNearlyEqual(Quat::FromEuler(g.ToEuler()), g, 1e-3f));

    const Quat a = Quat::FromAxisAngle(Vec3::UnitY(), 0.0f);
    const Quat b = Quat::FromAxisAngle(Vec3::UnitY(), 1.0f);
    ZX_EXPECT(IsNearlyEqual(Slerp(a, b, 0.5f), Quat::FromAxisAngle(Vec3::UnitY(), 0.5f), 1e-5f));
    ZX_EXPECT_NEAR(AngleBetween(a, b), 1.0f, 1e-4f);

    const Vec3 from = Normalize(Vec3(1, 2, 3)), to = Normalize(Vec3(-2, 0.5f, 1));
    EXPECT_VEC3(Quat::FromTo(from, to) * from, to);
    EXPECT_VEC3(Quat::FromTo(Vec3::UnitX(), -Vec3::UnitX()) * Vec3::UnitX(), -Vec3::UnitX());
    const Quat look = Quat::LookRotation(Normalize(Vec3(1, 0, 1)));
    EXPECT_VEC3(look.GetForward(), Normalize(Vec3(1, 0, 1)));
    EXPECT_VEC3(look.GetUp(), Vec3::Up());
    Vec3 axis;
    f32 angle;
    Quat::FromAxisAngle(Vec3::UnitZ(), 0.8f).ToAxisAngle(axis, angle);
    EXPECT_VEC3(axis, Vec3::UnitZ());
    ZX_EXPECT_NEAR(angle, 0.8f, 1e-5f);
}

ZX_TEST(Math, TransformComposition)
{
    const Transform parent(Vec3(1, 0, 0), Quat::FromAxisAngle(Vec3::Up(), HalfPi), Vec3(2));
    const Transform child(Vec3(0, 0, 1), Quat::FromAxisAngle(Vec3::Right(), 0.3f), Vec3(1));
    const Transform world = parent * child;
    ZX_EXPECT(IsNearlyEqual(world.ToMatrix(), parent.ToMatrix() * child.ToMatrix(), 1e-5f));
    EXPECT_VEC3(world.position, Vec3(3, 0, 0));
    const Vec3 p(0.5f, -1.0f, 2.0f);
    EXPECT_VEC3(parent.InverseTransformPoint(parent.TransformPoint(p)), p);
    EXPECT_VEC3(parent.Inverse().TransformPoint(parent.TransformPoint(p)), p);
    const Transform fromM = Transform::FromMatrix(world.ToMatrix());
    ZX_EXPECT(IsNearlyEqual(fromM, world, 1e-4f));
}

ZX_TEST(Math, ProjectionReversedZ)
{
    const f32 n = 0.1f, f = 100.0f;
    const Mat4 p = Mat4::PerspectiveReversedZ(DegToRad(60.0f), 16.0f / 9.0f, n, f);
    const Vec4 nearClip = p * Vec4(0, 0, n, 1);
    const Vec4 farClip = p * Vec4(0, 0, f, 1);
    ZX_EXPECT_NEAR(nearClip.z / nearClip.w, 1.0f, 1e-5f);
    ZX_EXPECT_NEAR(farClip.z / farClip.w, 0.0f, 1e-5f);
    const Mat4 inf = Mat4::PerspectiveInfiniteReversedZ(DegToRad(60.0f), 1.0f, n);
    const Vec4 c = inf * Vec4(0, 0, 10.0f, 1);
    ZX_EXPECT_NEAR(c.z / c.w, n / 10.0f, 1e-6f);
    // Top edge of the frustum maps to y = 1.
    const f32 tanHalf = Tan(DegToRad(30.0f));
    const Vec4 top = p * Vec4(0, tanHalf * 5.0f, 5.0f, 1);
    ZX_EXPECT_NEAR(top.y / top.w, 1.0f, 1e-5f);
    const Mat4 o = Mat4::OrthographicReversedZ(10.0f, 10.0f, 1.0f, 11.0f);
    ZX_EXPECT_NEAR((o * Vec4(0, 0, 1, 1)).z, 1.0f, 1e-6f);
    ZX_EXPECT_NEAR((o * Vec4(0, 0, 11, 1)).z, 0.0f, 1e-6f);
    ZX_EXPECT_NEAR((o * Vec4(5, 0, 6, 1)).x, 1.0f, 1e-6f);

    // View matrix: eye at (0,0,-10) looking at origin -> origin at view z = +10.
    const Mat4 view = Mat4::LookAt(Vec3(0, 0, -10), Vec3::Zero());
    EXPECT_VEC3(view.TransformPoint(Vec3::Zero()), Vec3(0, 0, 10));
    EXPECT_VEC3(view.TransformPoint(Vec3(1, 0, 0)), Vec3(1, 0, 10)); // +X stays right
}

ZX_TEST(Math, FrustumCulling)
{
    const Mat4 view = Mat4::LookAt(Vec3(0, 0, -10), Vec3::Zero());
    for (int variant = 0; variant < 2; ++variant) {
        const Mat4 proj = variant == 0 ? Mat4::PerspectiveReversedZ(DegToRad(60.0f), 1.0f, 0.1f, 50.0f)
                                       : Mat4::PerspectiveInfiniteReversedZ(DegToRad(60.0f), 1.0f, 0.1f);
        const Frustum fr = Frustum::FromMatrix(proj * view);
        ZX_EXPECT_EQ(fr.planeCount, variant == 0 ? 6u : 5u);
        ZX_EXPECT(fr.Contains(Vec3::Zero()));
        ZX_EXPECT(!fr.Contains(Vec3(0, 0, -20))); // behind camera
        ZX_EXPECT(!fr.Contains(Vec3(100, 0, 0))); // far to the right
        ZX_EXPECT(fr.Intersects(AABB::FromCenterExtents(Vec3(5, 0, 0), Vec3(1))));
        ZX_EXPECT(!fr.Intersects(AABB::FromCenterExtents(Vec3(30, 0, 0), Vec3(1))));
        ZX_EXPECT(fr.Intersects(Sphere(Vec3(0, 0, 0), 1.0f)));
        ZX_EXPECT_EQ(fr.Contains(Vec3(0, 0, 60)), variant == 1); // beyond far plane only for finite
    }
}

ZX_TEST(Math, GeometryQueries)
{
    const AABB box(Vec3(-1), Vec3(1));
    f32 tn, tf;
    ZX_EXPECT(RayIntersectsAABB(Ray(Vec3(-5, 0, 0), Vec3::UnitX()), box, tn, tf));
    ZX_EXPECT_NEAR(tn, 4.0f, 1e-5f);
    ZX_EXPECT_NEAR(tf, 6.0f, 1e-5f);
    ZX_EXPECT(!RayIntersectsAABB(Ray(Vec3(-5, 2, 0), Vec3::UnitX()), box, tn, tf));
    f32 t, u, v;
    ZX_EXPECT(RayIntersectsTriangle(Ray(Vec3(0.2f, 0.2f, -1), Vec3::UnitZ()), Vec3(0, 0, 0), Vec3(1, 0, 0),
                                    Vec3(0, 1, 0), t, u, v));
    ZX_EXPECT_NEAR(t, 1.0f, 1e-5f);
    ZX_EXPECT(RayIntersectsSphere(Ray(Vec3(0, 0, -5), Vec3::UnitZ()), Sphere(Vec3::Zero(), 1.0f), t));
    ZX_EXPECT_NEAR(t, 4.0f, 1e-5f);
    ZX_EXPECT(
        RayIntersectsPlane(Ray(Vec3(0, 5, 0), -Vec3::UnitY()), Plane::FromPointNormal(Vec3::Zero(), Vec3::Up()), t));
    ZX_EXPECT_NEAR(t, 5.0f, 1e-5f);

    const AABB rotated = box.Transformed(Mat4::RotationY(Pi / 4.0f));
    ZX_EXPECT_NEAR(rotated.max.x, Sqrt(2.0f), 1e-4f);
    EXPECT_VEC3(ClosestPointOnTriangle(Vec3(0.25f, 0.25f, 3), Vec3(0, 0, 0), Vec3(1, 0, 0), Vec3(0, 1, 0)),
                Vec3(0.25f, 0.25f, 0));
    EXPECT_VEC3(ClosestPointOnSegment(Vec3(5, 1, 0), Vec3(0, 0, 0), Vec3(2, 0, 0)), Vec3(2, 0, 0));
    Vec3 c1, c2;
    const f32 d2 = ClosestPointsSegmentSegment(Vec3(-1, 0, 0), Vec3(1, 0, 0), Vec3(0, 1, -1), Vec3(0, 1, 1), c1, c2);
    ZX_EXPECT_NEAR(d2, 1.0f, 1e-5f);
    const Vec3 bary = Barycentric(Vec3(0.25f, 0.5f, 0), Vec3(0, 0, 0), Vec3(1, 0, 0), Vec3(0, 1, 0));
    EXPECT_VEC3(bary, Vec3(0.25f, 0.25f, 0.5f));

    Vec3 pts[] = {{1, 0, 0}, {-1, 0, 0}, {0, 2, 0}, {0, 0, -1}};
    const Sphere s = Sphere::FromPoints(pts, 4);
    for (const Vec3& p : pts)
        ZX_EXPECT(DistanceSq(p, s.center) <= s.radius * s.radius * 1.0001f);
}

ZX_TEST(Math, Packing)
{
    const f32 values[] = {0.0f, -0.0f, 1.0f, -2.5f, 65504.0f, 0.000061035156f, 5.96e-8f, 0.1f, 1000.0f};
    for (f32 v : values) {
        const f32 back = HalfToFloat(FloatToHalf(v));
        ZX_EXPECT(Abs(back - v) <= Abs(v) * 1e-3f + 6e-8f);
    }
    ZX_EXPECT_EQ(FloatToHalf(1.0f), u16(0x3C00));
    ZX_EXPECT_EQ(FloatToHalf(-2.0f), u16(0xC000));
    ZX_EXPECT_EQ(FloatToHalf(65504.0f), u16(0x7BFF));
    ZX_EXPECT_EQ(FloatToHalf(1e6f), u16(0x7C00)); // inf
    ZX_EXPECT(IsNaN(HalfToFloat(FloatToHalf(std::nanf("")))));
    // Exhaustive: every finite half round-trips exactly.
    bool allExact = true;
    for (u32 h = 0; h < 0x10000; ++h) {
        if ((h & 0x7C00) == 0x7C00)
            continue;
        allExact &= FloatToHalf(HalfToFloat(u16(h))) == u16(h);
    }
    ZX_EXPECT(allExact);

    Random rng(42);
    for (int i = 0; i < 200; ++i) {
        const Vec3 n = rng.OnUnitSphere();
        const Vec3 d = UnpackNormalOct16(PackNormalOct16(n));
        ZX_EXPECT(Dot(n, d) > 0.99999f);
    }
    const Vec3 hdr(1.5f, 0.25f, 100.0f);
    const Vec3 r11 = UnpackR11G11B10F(PackR11G11B10F(hdr));
    ZX_EXPECT(Abs(r11.x - hdr.x) < 0.05f && Abs(r11.z - hdr.z) < 4.0f);
    const Vec3 e5 = UnpackRGB9E5(PackRGB9E5(hdr));
    ZX_EXPECT(Abs(e5.z - hdr.z) < 0.5f && Abs(e5.x - hdr.x) < 0.25f);
    ZX_EXPECT(UnpackUnorm4x8(PackUnorm4x8(Vec4(0, 0.5f, 1, 1))).y > 0.49f);
}

ZX_TEST(Math, ColorAndRandom)
{
    ZX_EXPECT_NEAR(SRGBToLinear(LinearToSRGB(0.3f)), 0.3f, 1e-5f);
    ZX_EXPECT_NEAR(SRGB8ToLinear(255), 1.0f, 1e-6f);
    const LinearColor c = LinearColor::FromHex("#FF8000");
    ZX_EXPECT_NEAR(c.r, 1.0f, 1e-5f);
    ZX_EXPECT_NEAR(c.g, SRGBToLinear(128.0f / 255.0f), 1e-5f);
    ZX_EXPECT(Color::FromLinear(c) == Color(255, 128, 0, 255));
    f32 h, s, v;
    LinearColor::FromHSV(0.6f, 0.5f, 0.8f).ToHSV(h, s, v);
    ZX_EXPECT_NEAR(h, 0.6f, 1e-3f);
    ZX_EXPECT_NEAR(s, 0.5f, 1e-3f);
    ZX_EXPECT_NEAR(v, 0.8f, 1e-3f);
    const LinearColor warm = LinearColor::FromTemperature(2700.0f);
    ZX_EXPECT(warm.r > warm.b);

    Random a(123), b(123);
    for (int i = 0; i < 10; ++i)
        ZX_EXPECT_EQ(a.NextU32(), b.NextU32());
    Random r(7);
    f32 mn = 1, mx = 0, sum = 0;
    for (int i = 0; i < 10000; ++i) {
        const f32 x = r.NextFloat();
        mn = Min(mn, x);
        mx = Max(mx, x);
        sum += x;
    }
    ZX_EXPECT(mn >= 0.0f && mx < 1.0f);
    ZX_EXPECT_NEAR(sum / 10000.0f, 0.5f, 0.02f);
    for (int i = 0; i < 1000; ++i) {
        const i32 k = r.Range(-3, 3);
        ZX_EXPECT(k >= -3 && k <= 3);
    }
    ZX_EXPECT_NEAR(Length(r.NextRotation() * Vec3::UnitX()), 1.0f, 1e-5f);
}

ZX_TEST(Math, NoiseAndCurves)
{
    f32 mn = 10, mx = -10;
    for (int i = 0; i < 2000; ++i) {
        const Vec3 p(f32(i) * 0.37f, f32(i % 17) * 0.11f, f32(i % 5) * 1.3f);
        const f32 n = noise::Simplex(p);
        mn = Min(mn, n);
        mx = Max(mx, n);
        ZX_EXPECT(Abs(noise::Perlin(p)) <= 1.5f);
    }
    ZX_EXPECT(mn < -0.3f && mx > 0.3f && mn >= -1.1f && mx <= 1.1f);
    ZX_EXPECT_EQ(noise::Perlin(Vec2(3.0f, 4.0f)), 0.0f); // zero at lattice points
    ZX_EXPECT_EQ(noise::Simplex(Vec2(1.3f, 2.7f), 5), noise::Simplex(Vec2(1.3f, 2.7f), 5));
    ZX_EXPECT(noise::Worley(Vec2(0.5f, 0.5f)) < 1.5f);
    const f32 ridge = noise::Ridged(Vec2(0.3f, 0.9f));
    ZX_EXPECT(ridge >= 0.0f && ridge <= 1.0f);

    Curve lin = Curve::Linear(0.0f, 0.0f, 2.0f, 10.0f);
    ZX_EXPECT_NEAR(lin.Evaluate(1.0f), 5.0f, 1e-5f);
    ZX_EXPECT_NEAR(lin.Evaluate(-1.0f), 0.0f, 1e-5f);
    lin.postWrap = CurveWrap::Loop;
    ZX_EXPECT_NEAR(lin.Evaluate(3.0f), 5.0f, 1e-4f);
    lin.postWrap = CurveWrap::PingPong;
    ZX_EXPECT_NEAR(lin.Evaluate(3.0f), 5.0f, 1e-4f);
    ZX_EXPECT_NEAR(lin.Evaluate(3.5f), 2.5f, 1e-4f);
    const Curve ease = Curve::EaseInOut(0.0f, 0.0f, 1.0f, 1.0f);
    ZX_EXPECT_NEAR(ease.Evaluate(0.5f), 0.5f, 1e-5f);
    ZX_EXPECT(ease.Evaluate(0.1f) < 0.1f);
    ZX_EXPECT_NEAR(CatmullRom(0.0f, 1.0f, 2.0f, 3.0f, 0.5f), 1.5f, 1e-5f);
    ZX_EXPECT_NEAR(Damp(0.0f, 10.0f, 1.0f, 1.0f), 5.0f, 1e-4f);
}
