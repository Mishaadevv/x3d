#include "Joints.h"

#include <cmath>

namespace zx::physics {

namespace {

struct Frame
{
    Vec3 pA, pB;   // world anchors
    Vec3 rA, rB;   // from the centers of mass
    Vec3 axisA, axisB, refA, refB;
};

Frame MakeFrame(const JointData& j, const SolverBody& A, const SolverBody& B)
{
    Frame f;
    f.rA = A.q * j.localAnchorA;
    f.rB = B.q * j.localAnchorB;
    f.pA = A.x + f.rA;
    f.pB = B.x + f.rB;
    f.axisA = A.q * j.localAxisA;
    f.axisB = B.q * j.localAxisB;
    f.refA = A.q * j.localRefA;
    f.refB = B.q * j.localRefB;
    return f;
}

f32 EffectiveMass(const JointRow& r, const SolverBody& A, const SolverBody& B)
{
    const f32 k = A.invMass * LengthSq(r.linA) + B.invMass * LengthSq(r.linB) + Dot(r.angA, A.invI * r.angA) + Dot(r.angB, B.invI * r.angB);
    return k > 1e-12f ? 1.0f / k : 0.0f;
}

void Push(Array<JointRow>& rows, JointRow r, const SolverBody& A, const SolverBody& B)
{
    r.effMass = EffectiveMass(r, A, B);
    if (r.effMass > 0.0f)
        rows.PushBack(r);
}

/// Linear row along `n` between the anchors.
JointRow Linear(const Frame& f, Vec3 n, f32 C, u32 slot)
{
    JointRow r;
    r.linA = -n;
    r.angA = -Cross(f.rA, n);
    r.linB = n;
    r.angB = Cross(f.rB, n);
    r.C = C;
    r.slot = slot;
    return r;
}

/// Angular row about `n` (relative angular velocity wB - wA along n).
JointRow Angular(Vec3 n, f32 C, u32 slot)
{
    JointRow r;
    r.angA = -n;
    r.angB = n;
    r.C = C;
    r.slot = slot;
    return r;
}

JointRow Negated(JointRow r)
{
    r.linA = -r.linA, r.angA = -r.angA, r.linB = -r.linB, r.angB = -r.angB;
    return r;
}

void Perpendiculars(Vec3 n, Vec3& p1, Vec3& p2)
{
    p1 = Abs(n.x) > 0.57f ? Normalize(Vec3(n.y, -n.x, 0.0f)) : Normalize(Vec3(0.0f, n.z, -n.y));
    p2 = Cross(n, p1);
}

/// Small-angle rotation vector taking qB back to its locked orientation relative to qA.
Vec3 RotationError(const JointData& j, const SolverBody& A, const SolverBody& B)
{
    Quat err = B.q * Conjugate(A.q * j.relRotation);
    if (err.w < 0.0f)
        err = -err;
    return Vec3(err.x, err.y, err.z) * 2.0f;
}

/// Signed angle of B around the axis relative to the creation pose.
f32 TwistAngle(const Frame& f)
{
    Vec3 refB = f.refB - f.axisA * Dot(f.refB, f.axisA);
    if (LengthSq(refB) < 1e-10f)
        return 0.0f;
    refB = Normalize(refB);
    return std::atan2(Dot(Cross(f.refA, refB), f.axisA), Dot(f.refA, refB));
}

/// Two one-sided rows keeping `value` (with derivative along the row's Jacobian) in [lower, upper].
void Limits(Array<JointRow>& rows, JointRow base, f32 value, f32 lower, f32 upper, u32 slotLower, u32 slotUpper, const SolverBody& A,
            const SolverBody& B)
{
    JointRow lo = base;
    lo.C = value - lower;
    lo.lo = 0.0f;
    lo.limit = true;
    lo.slot = slotLower;
    Push(rows, lo, A, B);
    JointRow hi = base;
    hi = Negated(hi);
    hi.C = upper - value;
    hi.lo = 0.0f;
    hi.limit = true;
    hi.slot = slotUpper;
    Push(rows, hi, A, B);
}

void Motor(Array<JointRow>& rows, JointRow base, f32 speed, f32 maxForce, u32 slot, const SolverBody& A, const SolverBody& B)
{
    base.motor = true;
    base.targetVelocity = speed;
    base.lo = -maxForce; // scaled by the substep in the solver
    base.hi = maxForce;
    base.C = 0.0f;
    base.slot = slot;
    Push(rows, base, A, B);
}

/// Marks the last three rows (pushed together) as one block solved with the inverse of their coupled mass matrix:
/// a point or a locked rotation converges in one go even when the rows are strongly coupled (light body far from
/// the anchor).
void MakeBlock(Array<JointRow>& rows, usize first, const SolverBody& A, const SolverBody& B)
{
    if (rows.Size() != first + 3)
        return;
    f32 k[3][3];
    for (u32 i = 0; i < 3; ++i)
        for (u32 j = 0; j < 3; ++j) {
            const JointRow& a = rows[first + i];
            const JointRow& b = rows[first + j];
            k[i][j] = A.invMass * Dot(a.linA, b.linA) + B.invMass * Dot(a.linB, b.linB) + Dot(a.angA, A.invI * b.angA) + Dot(a.angB, B.invI * b.angB);
        }
    const Mat3 K(Vec3(k[0][0], k[1][0], k[2][0]), Vec3(k[0][1], k[1][1], k[2][1]), Vec3(k[0][2], k[1][2], k[2][2]));
    const f32 det = Dot(K.columns[0], Cross(K.columns[1], K.columns[2]));
    const f32 scale = Max(Max(k[0][0], k[1][1]), k[2][2]);
    if (!(Abs(det) > 1e-6f * scale * scale * scale))
        return;
    rows[first].block = 3;
    rows[first].blockMass = Inverse(K);
}

void PointRows(Array<JointRow>& rows, const Frame& f, const SolverBody& A, const SolverBody& B)
{
    const Vec3 d = f.pB - f.pA;
    static const Vec3 Axes[3] = {Vec3(1, 0, 0), Vec3(0, 1, 0), Vec3(0, 0, 1)};
    const usize first = rows.Size();
    for (u32 i = 0; i < 3; ++i)
        Push(rows, Linear(f, Axes[i], Dot(d, Axes[i]), i), A, B);
    MakeBlock(rows, first, A, B);
}

void LockRotationRows(Array<JointRow>& rows, const JointData& j, const SolverBody& A, const SolverBody& B, u32 firstSlot, const Vec3* axes,
                      const bool* locked)
{
    const Vec3 err = RotationError(j, A, B);
    const usize first = rows.Size();
    for (u32 i = 0; i < 3; ++i)
        if (locked[i])
            Push(rows, Angular(axes[i], Dot(err, axes[i]), firstSlot + i), A, B);
    MakeBlock(rows, first, A, B);
}

} // namespace

void BuildJointRows(const JointData& j, const SolverBody& A, const SolverBody& B, Array<JointRow>& rows)
{
    const Frame f = MakeFrame(j, A, B);
    const JointDesc& d = j.desc;
    static const Vec3 WorldAxes[3] = {Vec3(1, 0, 0), Vec3(0, 1, 0), Vec3(0, 0, 1)};
    static const bool AllLocked[3] = {true, true, true};
    switch (d.type) {
    case JointType::Fixed:
        PointRows(rows, f, A, B);
        LockRotationRows(rows, j, A, B, 3, WorldAxes, AllLocked);
        break;
    case JointType::Hinge: {
        PointRows(rows, f, A, B);
        // Keep the axes aligned: rotation only around the hinge axis.
        Vec3 p1, p2;
        Perpendiculars(f.axisA, p1, p2);
        const Vec3 e = Cross(f.axisA, f.axisB);
        Push(rows, Angular(p1, Dot(e, p1), 3), A, B);
        Push(rows, Angular(p2, Dot(e, p2), 4), A, B);
        if (d.useLimits)
            Limits(rows, Negated(Angular(f.axisA, 0.0f, 0)), -TwistAngle(f), d.lower, d.upper, 5, 6, A, B);
        if (d.useMotor)
            Motor(rows, Negated(Angular(f.axisA, 0.0f, 0)), d.motorSpeed, d.maxMotorForce, 7, A, B);
        break;
    }
    case JointType::Slider: {
        // Free along the axis only: no sideways motion, no rotation.
        Vec3 p1, p2;
        Perpendiculars(f.axisA, p1, p2);
        const Vec3 delta = f.pB - f.pA;
        Push(rows, Linear(f, p1, Dot(delta, p1), 0), A, B);
        Push(rows, Linear(f, p2, Dot(delta, p2), 1), A, B);
        LockRotationRows(rows, j, A, B, 2, WorldAxes, AllLocked);
        const f32 along = -Dot(delta, f.axisA);
        if (d.useLimits)
            Limits(rows, Negated(Linear(f, f.axisA, 0.0f, 0)), along, d.lower, d.upper, 5, 6, A, B);
        if (d.useMotor)
            Motor(rows, Negated(Linear(f, f.axisA, 0.0f, 0)), d.motorSpeed, d.maxMotorForce, 7, A, B);
        break;
    }
    case JointType::Ball: {
        PointRows(rows, f, A, B);
        if (d.swingLimit > 0.0f) {
            // Cone: the angle between the two axes stays below the swing limit.
            const f32 c = Clamp(Dot(f.axisA, f.axisB), -1.0f, 1.0f);
            const f32 angle = std::acos(c);
            Vec3 n = Cross(f.axisA, f.axisB);
            if (LengthSq(n) > 1e-10f && angle > d.swingLimit - 0.1f) {
                n = Normalize(n);
                JointRow r;
                r.angA = n;
                r.angB = -n;
                r.C = d.swingLimit - angle;
                r.lo = 0.0f;
                r.limit = true;
                r.slot = 3;
                Push(rows, r, A, B);
            }
        }
        if (d.twistLimit > 0.0f)
            Limits(rows, Angular(f.axisA, 0.0f, 0), TwistAngle(f), -d.twistLimit, d.twistLimit, 4, 5, A, B);
        break;
    }
    case JointType::Distance: {
        Vec3 delta = f.pB - f.pA;
        const f32 len = Length(delta);
        const Vec3 n = len > 1e-6f ? delta / len : Vec3(0, 1, 0);
        const JointRow base = Linear(f, n, 0.0f, 0);
        if (d.springHertz > 0.0f) {
            JointRow spring = base;
            spring.C = len - j.restLength;
            spring.springHertz = d.springHertz;
            spring.springDamping = d.springDamping;
            spring.slot = 0;
            Push(rows, spring, A, B);
        }
        const f32 minLen = d.minDistance, maxLen = d.maxDistance;
        if (d.springHertz <= 0.0f && minLen <= 0.0f && maxLen <= 0.0f) {
            JointRow rod = base; // rigid rod at the creation length
            rod.C = len - j.restLength;
            rod.slot = 1;
            Push(rows, rod, A, B);
        } else {
            if (maxLen > 0.0f) {
                JointRow rope = base; // never longer than maxLen
                rope = Negated(rope);
                rope.C = maxLen - len;
                rope.lo = 0.0f;
                rope.limit = true;
                rope.slot = 2;
                Push(rows, rope, A, B);
            }
            if (minLen > 0.0f) {
                JointRow strut = base;
                strut.C = len - minLen;
                strut.lo = 0.0f;
                strut.limit = true;
                strut.slot = 3;
                Push(rows, strut, A, B);
            }
        }
        break;
    }
    case JointType::Configurable: {
        // Joint frame: X = axis, Y/Z = the reference direction and its perpendicular (from body A).
        const Vec3 ex = f.axisA, ey = f.refA, ez = Cross(ex, ey);
        const Vec3 axes[3] = {ex, ey, ez};
        const Vec3 delta = f.pB - f.pA;
        for (u32 i = 0; i < 3; ++i) {
            const f32 x = Dot(delta, axes[i]);
            if (d.linear[i] == AxisMotion::Locked)
                Push(rows, Linear(f, axes[i], x, i), A, B);
            else if (d.linear[i] == AxisMotion::Limited)
                Limits(rows, Linear(f, axes[i], 0.0f, 0), x, -d.linearLimit, d.linearLimit, 6 + i * 2, 7 + i * 2, A, B);
        }
        const bool locked[3] = {d.angular[0] == AxisMotion::Locked, d.angular[1] == AxisMotion::Locked, d.angular[2] == AxisMotion::Locked};
        LockRotationRows(rows, j, A, B, 3, axes, locked);
        const Vec3 err = RotationError(j, A, B);
        for (u32 i = 0; i < 3; ++i)
            if (d.angular[i] == AxisMotion::Limited)
                Limits(rows, Angular(axes[i], 0.0f, 0), Dot(err, axes[i]), -d.angularLimit, d.angularLimit, 12 + i * 2, 13 + i * 2, A, B);
        break;
    }
    }
}

f32 JointCoordinate(const JointData& j, const SolverBody& A, const SolverBody& B)
{
    const Frame f = MakeFrame(j, A, B);
    switch (j.desc.type) {
    case JointType::Hinge: return -TwistAngle(f);
    case JointType::Slider: return Dot(f.pA - f.pB, f.axisA);
    case JointType::Distance: return Length(f.pB - f.pA);
    default: return 0.0f;
    }
}

} // namespace zx::physics
