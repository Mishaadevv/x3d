#include "Core/Math/Quat.h"

namespace zx {

Quat Quat::FromAxisAngle(Vec3 axis, f32 radians)
{
    f32 s, c;
    SinCos(radians * 0.5f, s, c);
    const Vec3 a = SafeNormalize(axis, Vec3::Up());
    return {a.x * s, a.y * s, a.z * s, c};
}

Quat Quat::FromEuler(f32 pitch, f32 yaw, f32 roll)
{
    // q = qYaw * qPitch * qRoll
    f32 sp, cp, sy, cy, sr, cr;
    SinCos(pitch * 0.5f, sp, cp);
    SinCos(yaw * 0.5f, sy, cy);
    SinCos(roll * 0.5f, sr, cr);
    return {cy * sp * cr + sy * cp * sr, sy * cp * cr - cy * sp * sr, cy * cp * sr - sy * sp * cr,
            cy * cp * cr + sy * sp * sr};
}

Vec3 Quat::ToEuler() const
{
    // Inverse of R = Ry * Rx * Rz. Matrix element (row, col):
    //   m12 = -sin(pitch)            (row 1, col 2)
    //   m02 = sin(yaw) cos(pitch),  m22 = cos(yaw) cos(pitch)
    //   m10 = sin(roll) cos(pitch), m11 = cos(roll) cos(pitch)
    const f32 m12 = 2.0f * (y * z - w * x);
    const f32 sinPitch = -m12;
    f32 pitch, yaw, roll;
    if (Abs(sinPitch) > 0.99999f) {
        // Gimbal lock (cos(pitch) = 0): roll is folded into yaw. With roll = 0:
        //   m00 = cos(yaw), m20 = -sin(yaw)
        pitch = sinPitch > 0.0f ? HalfPi : -HalfPi;
        const f32 m00 = 1.0f - 2.0f * (y * y + z * z);
        const f32 m20 = 2.0f * (x * z - w * y);
        yaw = Atan2(-m20, m00);
        roll = 0.0f;
    } else {
        pitch = Asin(sinPitch);
        const f32 m02 = 2.0f * (x * z + w * y);
        const f32 m22 = 1.0f - 2.0f * (x * x + y * y);
        const f32 m10 = 2.0f * (x * y + w * z);
        const f32 m11 = 1.0f - 2.0f * (x * x + z * z);
        yaw = Atan2(m02, m22);
        roll = Atan2(m10, m11);
    }
    return {pitch, yaw, roll};
}

void Quat::ToAxisAngle(Vec3& axis, f32& radians) const
{
    const Quat q = w < 0.0f ? -*this : *this;
    radians = 2.0f * Acos(q.w);
    const f32 s = Sqrt(1.0f - q.w * q.w);
    axis = s > 1e-6f ? Vec3(q.x, q.y, q.z) / s : Vec3::Up();
}

Quat Quat::FromMat3(const Mat3& m)
{
    // Shepperd's method; m.At(row, col).
    const f32 m00 = m.At(0, 0), m11 = m.At(1, 1), m22 = m.At(2, 2);
    const f32 trace = m00 + m11 + m22;
    Quat q;
    if (trace > 0.0f) {
        const f32 s = Sqrt(trace + 1.0f) * 2.0f;
        q.w = 0.25f * s;
        q.x = (m.At(2, 1) - m.At(1, 2)) / s;
        q.y = (m.At(0, 2) - m.At(2, 0)) / s;
        q.z = (m.At(1, 0) - m.At(0, 1)) / s;
    } else if (m00 > m11 && m00 > m22) {
        const f32 s = Sqrt(1.0f + m00 - m11 - m22) * 2.0f;
        q.w = (m.At(2, 1) - m.At(1, 2)) / s;
        q.x = 0.25f * s;
        q.y = (m.At(0, 1) + m.At(1, 0)) / s;
        q.z = (m.At(0, 2) + m.At(2, 0)) / s;
    } else if (m11 > m22) {
        const f32 s = Sqrt(1.0f + m11 - m00 - m22) * 2.0f;
        q.w = (m.At(0, 2) - m.At(2, 0)) / s;
        q.x = (m.At(0, 1) + m.At(1, 0)) / s;
        q.y = 0.25f * s;
        q.z = (m.At(1, 2) + m.At(2, 1)) / s;
    } else {
        const f32 s = Sqrt(1.0f + m22 - m00 - m11) * 2.0f;
        q.w = (m.At(1, 0) - m.At(0, 1)) / s;
        q.x = (m.At(0, 2) + m.At(2, 0)) / s;
        q.y = (m.At(1, 2) + m.At(2, 1)) / s;
        q.z = 0.25f * s;
    }
    return Normalize(q);
}

Quat Quat::LookRotation(Vec3 forward, Vec3 up)
{
    const Vec3 f = SafeNormalize(forward, Vec3::Forward());
    Vec3 r = Cross(up, f);
    if (LengthSq(r) < 1e-8f)
        r = Cross(Abs(f.y) < 0.99f ? Vec3::Up() : Vec3::Forward(), f);
    r = Normalize(r);
    const Vec3 u = Cross(f, r);
    return FromMat3(Mat3(r, u, f));
}

Quat Quat::FromTo(Vec3 from, Vec3 to)
{
    const f32 d = Dot(from, to);
    if (d >= 1.0f - 1e-6f)
        return Quat::Identity();
    if (d <= -1.0f + 1e-6f) {
        // 180 degrees: any perpendicular axis.
        Vec3 axis = Cross(Vec3::UnitX(), from);
        if (LengthSq(axis) < 1e-6f)
            axis = Cross(Vec3::UnitY(), from);
        return FromAxisAngle(Normalize(axis), Pi);
    }
    const Vec3 c = Cross(from, to);
    return Normalize(Quat(c.x, c.y, c.z, 1.0f + d));
}

Quat Normalize(const Quat& q)
{
    const f32 l2 = Dot(q, q);
    if (l2 < SmallNumber)
        return Quat::Identity();
    const f32 inv = InvSqrt(l2);
    return {q.x * inv, q.y * inv, q.z * inv, q.w * inv};
}

Quat Nlerp(const Quat& a, const Quat& b, f32 t)
{
    const f32 sign = Dot(a, b) < 0.0f ? -1.0f : 1.0f;
    return Normalize(Quat(Lerp(a.x, b.x * sign, t), Lerp(a.y, b.y * sign, t), Lerp(a.z, b.z * sign, t),
                          Lerp(a.w, b.w * sign, t)));
}

Quat Slerp(const Quat& a, const Quat& b, f32 t)
{
    f32 cosTheta = Dot(a, b);
    Quat end = b;
    if (cosTheta < 0.0f) {
        cosTheta = -cosTheta;
        end = -b;
    }
    if (cosTheta > 0.9995f)
        return Nlerp(a, end, t);
    const f32 theta = Acos(cosTheta);
    const f32 sinTheta = Sin(theta);
    const f32 wa = Sin((1.0f - t) * theta) / sinTheta;
    const f32 wb = Sin(t * theta) / sinTheta;
    return {a.x * wa + end.x * wb, a.y * wa + end.y * wb, a.z * wa + end.z * wb, a.w * wa + end.w * wb};
}

f32 AngleBetween(const Quat& a, const Quat& b)
{
    const f32 d = Abs(Dot(a, b));
    return 2.0f * Acos(d > 1.0f ? 1.0f : d);
}

bool IsNearlyEqual(const Quat& a, const Quat& b, f32 tolerance)
{
    return AngleBetween(Normalize(a), Normalize(b)) <= tolerance * 2.0f;
}

// --- Transform ------------------------------------------------------------------------------------------------------

Transform Transform::FromMatrix(const Mat4& m)
{
    Transform t;
    Decompose(m, t.position, t.rotation, t.scale);
    return t;
}

Transform Transform::operator*(const Transform& child) const
{
    Transform r;
    r.rotation = Normalize(rotation * child.rotation);
    r.scale = scale * child.scale;
    r.position = position + rotation * (scale * child.position);
    return r;
}

Transform Transform::Inverse() const
{
    Transform r;
    r.rotation = zx::Inverse(rotation);
    r.scale = Vec3(scale.x != 0.0f ? 1.0f / scale.x : 0.0f, scale.y != 0.0f ? 1.0f / scale.y : 0.0f,
                   scale.z != 0.0f ? 1.0f / scale.z : 0.0f);
    r.position = r.scale * (r.rotation * -position);
    return r;
}

Vec3 Transform::InverseTransformPoint(Vec3 p) const
{
    const Vec3 local = zx::Inverse(rotation) * (p - position);
    return Vec3(scale.x != 0.0f ? local.x / scale.x : 0.0f, scale.y != 0.0f ? local.y / scale.y : 0.0f,
                scale.z != 0.0f ? local.z / scale.z : 0.0f);
}

Transform Lerp(const Transform& a, const Transform& b, f32 t)
{
    return Transform(Lerp(a.position, b.position, t), Slerp(a.rotation, b.rotation, t), Lerp(a.scale, b.scale, t));
}

bool IsNearlyEqual(const Transform& a, const Transform& b, f32 tolerance)
{
    return IsNearlyEqual(a.position, b.position, tolerance) && IsNearlyEqual(a.rotation, b.rotation, tolerance) &&
           IsNearlyEqual(a.scale, b.scale, tolerance);
}

} // namespace zx
