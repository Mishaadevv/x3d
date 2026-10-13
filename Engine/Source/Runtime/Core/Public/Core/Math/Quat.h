#pragma once

#include "Core/Math/Matrix.h"

namespace zx {

/// Unit quaternion rotation (x, y, z = vector part, w = scalar). q * v rotates v.
/// Composition: (a * b) applies b first, then a (same as matrices).
struct Quat
{
    f32 x = 0.0f, y = 0.0f, z = 0.0f, w = 1.0f;

    constexpr Quat() = default;
    constexpr Quat(f32 x_, f32 y_, f32 z_, f32 w_) : x(x_), y(y_), z(z_), w(w_) {}

    static constexpr Quat Identity() { return Quat(); }
    /// Rotation of `radians` about unit `axis` (positive = clockwise looking down the axis toward the origin,
    /// i.e. yaw +90 deg turns +Z forward into +X right).
    static Quat FromAxisAngle(Vec3 axis, f32 radians);
    /// Euler angles in radians: pitch about X, yaw about Y, roll about Z. Applied roll, then pitch, then yaw
    /// (q = yaw * pitch * roll), same as Unity.
    static Quat FromEuler(f32 pitch, f32 yaw, f32 roll);
    static Quat FromEuler(Vec3 pitchYawRoll) { return FromEuler(pitchYawRoll.x, pitchYawRoll.y, pitchYawRoll.z); }
    static Quat FromEulerDegrees(Vec3 pitchYawRollDeg)
    {
        return FromEuler(DegToRad(pitchYawRollDeg.x), DegToRad(pitchYawRollDeg.y), DegToRad(pitchYawRollDeg.z));
    }
    /// From an orthonormal rotation matrix.
    static Quat FromMat3(const Mat3& m);
    /// Rotation whose forward (+Z) points along `forward` and up is as close to `up` as possible.
    static Quat LookRotation(Vec3 forward, Vec3 up = Vec3::Up());
    /// Shortest rotation taking unit vector `from` to unit vector `to`.
    static Quat FromTo(Vec3 from, Vec3 to);

    /// Returns (pitch, yaw, roll) radians; inverse of FromEuler.
    Vec3 ToEuler() const;
    Vec3 ToEulerDegrees() const
    {
        const Vec3 e = ToEuler();
        return {RadToDeg(e.x), RadToDeg(e.y), RadToDeg(e.z)};
    }
    void ToAxisAngle(Vec3& axis, f32& radians) const;

    constexpr Quat operator*(const Quat& b) const
    {
        return {w * b.x + x * b.w + y * b.z - z * b.y, w * b.y - x * b.z + y * b.w + z * b.x,
                w * b.z + x * b.y - y * b.x + z * b.w, w * b.w - x * b.x - y * b.y - z * b.z};
    }
    constexpr Quat& operator*=(const Quat& b) { return *this = *this * b; }
    /// Rotates a vector.
    constexpr Vec3 operator*(Vec3 v) const
    {
        // v' = v + 2w(q x v) + 2 q x (q x v)
        const Vec3 q(x, y, z);
        const Vec3 t = Cross(q, v) * 2.0f;
        return v + t * w + Cross(q, t);
    }
    constexpr Quat operator-() const { return {-x, -y, -z, -w}; }
    friend constexpr bool operator==(const Quat&, const Quat&) = default;

    constexpr Vec3 GetForward() const { return *this * Vec3::Forward(); }
    constexpr Vec3 GetRight() const { return *this * Vec3::Right(); }
    constexpr Vec3 GetUp() const { return *this * Vec3::Up(); }
};

constexpr f32 Dot(const Quat& a, const Quat& b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }
constexpr Quat Conjugate(const Quat& q) { return {-q.x, -q.y, -q.z, q.w}; }
/// Inverse of a unit quaternion.
constexpr Quat Inverse(const Quat& q) { return Conjugate(q); }
Quat Normalize(const Quat& q);
/// Normalized linear interpolation along the shortest path.
Quat Nlerp(const Quat& a, const Quat& b, f32 t);
/// Spherical linear interpolation along the shortest path.
Quat Slerp(const Quat& a, const Quat& b, f32 t);
/// Angle in radians between two rotations.
f32 AngleBetween(const Quat& a, const Quat& b);
bool IsNearlyEqual(const Quat& a, const Quat& b, f32 tolerance = KindaSmallNumber); // accounts for q == -q

template <>
struct Formatter<Quat>
{
    static void Format(FormatSink& s, const Quat& q, const FormatSpec& spec)
    {
        Formatter<Vec4>::Format(s, Vec4(q.x, q.y, q.z, q.w), spec);
    }
};

/// Translation + rotation + non-uniform scale. Composition (parent * child) treats scale as non-shearing
/// (exact for uniform scale; standard game-engine approximation otherwise).
struct Transform
{
    Vec3 position;
    Quat rotation;
    Vec3 scale = Vec3::One();

    constexpr Transform() = default;
    constexpr Transform(Vec3 p, Quat r = Quat(), Vec3 s = Vec3::One()) : position(p), rotation(r), scale(s) {}

    static constexpr Transform Identity() { return Transform(); }
    static Transform FromMatrix(const Mat4& m);

    Mat4 ToMatrix() const { return Mat4::TRS(position, rotation, scale); }
    /// Applies child-local transform: result = this * child.
    Transform operator*(const Transform& child) const;
    Transform Inverse() const;
    Vec3 TransformPoint(Vec3 p) const { return position + rotation * (scale * p); }
    Vec3 TransformVector(Vec3 v) const { return rotation * (scale * v); }
    Vec3 TransformDirection(Vec3 d) const { return rotation * d; }
    Vec3 InverseTransformPoint(Vec3 p) const;

    constexpr Vec3 GetForward() const { return rotation.GetForward(); }
    constexpr Vec3 GetRight() const { return rotation.GetRight(); }
    constexpr Vec3 GetUp() const { return rotation.GetUp(); }

    friend constexpr bool operator==(const Transform&, const Transform&) = default;
};

/// Interpolates position/scale linearly and rotation with slerp.
Transform Lerp(const Transform& a, const Transform& b, f32 t);
bool IsNearlyEqual(const Transform& a, const Transform& b, f32 tolerance = KindaSmallNumber);


// Reflection (defined in Core/Private/Reflection/CoreTypes.cpp).
class TypeInfo;
const TypeInfo& ZxTypeOf(Quat*);
const TypeInfo& ZxTypeOf(Transform*);

} // namespace zx
