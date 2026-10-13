#pragma once

#include "Core/Math/Vector.h"

namespace zx {

struct Quat;

/// 3x3 matrix, column-major: columns[c] is column c. v' = M * v.
struct Mat3
{
    Vec3 columns[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};

    constexpr Mat3() = default;
    constexpr Mat3(Vec3 c0, Vec3 c1, Vec3 c2) : columns{c0, c1, c2} {}

    static constexpr Mat3 Identity() { return Mat3(); }
    static constexpr Mat3 Scale(Vec3 s) { return Mat3({s.x, 0, 0}, {0, s.y, 0}, {0, 0, s.z}); }
    static Mat3 FromQuat(const Quat& q);

    constexpr Vec3& operator[](usize c) { return columns[c]; }
    constexpr const Vec3& operator[](usize c) const { return columns[c]; }
    /// Element at (row, column).
    constexpr f32 At(usize row, usize col) const { return columns[col][row]; }
    constexpr Vec3 Row(usize r) const { return {columns[0][r], columns[1][r], columns[2][r]}; }

    constexpr Vec3 operator*(Vec3 v) const { return columns[0] * v.x + columns[1] * v.y + columns[2] * v.z; }
    constexpr Mat3 operator*(const Mat3& o) const { return Mat3(*this * o.columns[0], *this * o.columns[1], *this * o.columns[2]); }
    constexpr Mat3 operator*(f32 s) const { return Mat3(columns[0] * s, columns[1] * s, columns[2] * s); }
    friend constexpr bool operator==(const Mat3&, const Mat3&) = default;
};

constexpr Mat3 Transpose(const Mat3& m) { return Mat3(m.Row(0), m.Row(1), m.Row(2)); }
constexpr f32 Determinant(const Mat3& m) { return Dot(m.columns[0], Cross(m.columns[1], m.columns[2])); }
Mat3 Inverse(const Mat3& m);

/// 4x4 matrix, column-major: columns[c] is column c; translation lives in columns[3]. v' = M * v.
/// Uploaded to HLSL as-is (HLSL default column_major packing, mul(M, v)).
struct alignas(16) Mat4
{
    Vec4 columns[4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};

    constexpr Mat4() = default;
    constexpr Mat4(Vec4 c0, Vec4 c1, Vec4 c2, Vec4 c3) : columns{c0, c1, c2, c3} {}
    constexpr explicit Mat4(const Mat3& m, Vec3 translation = Vec3())
        : columns{Vec4(m.columns[0], 0), Vec4(m.columns[1], 0), Vec4(m.columns[2], 0), Vec4(translation, 1)}
    {
    }

    static constexpr Mat4 Identity() { return Mat4(); }
    static constexpr Mat4 Zero() { return Mat4(Vec4(), Vec4(), Vec4(), Vec4()); }
    static constexpr Mat4 Translation(Vec3 t) { return Mat4({1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {t.x, t.y, t.z, 1}); }
    static constexpr Mat4 Scale(Vec3 s) { return Mat4({s.x, 0, 0, 0}, {0, s.y, 0, 0}, {0, 0, s.z, 0}, {0, 0, 0, 1}); }
    static Mat4 RotationX(f32 radians);
    static Mat4 RotationY(f32 radians);
    static Mat4 RotationZ(f32 radians);
    static Mat4 FromQuat(const Quat& q);
    /// Translation * Rotation * Scale (scale applied first).
    static Mat4 TRS(Vec3 translation, const Quat& rotation, Vec3 scale);

    /// Left-handed view matrix looking from eye to target.
    static Mat4 LookAt(Vec3 eye, Vec3 target, Vec3 up = Vec3::Up());
    /// Left-handed view matrix from position and forward direction.
    static Mat4 LookTo(Vec3 eye, Vec3 forward, Vec3 up = Vec3::Up());

    /// Reversed-Z perspective (depth 1 at near, 0 at far), D3D clip space z in [0, w].
    static Mat4 PerspectiveReversedZ(f32 fovYRadians, f32 aspect, f32 nearZ, f32 farZ);
    /// Reversed-Z perspective with infinite far plane (depth = near / z).
    static Mat4 PerspectiveInfiniteReversedZ(f32 fovYRadians, f32 aspect, f32 nearZ);
    /// Reversed-Z orthographic projection.
    static Mat4 OrthographicReversedZ(f32 left, f32 right, f32 bottom, f32 top, f32 nearZ, f32 farZ);
    static Mat4 OrthographicReversedZ(f32 width, f32 height, f32 nearZ, f32 farZ)
    {
        return OrthographicReversedZ(-width * 0.5f, width * 0.5f, -height * 0.5f, height * 0.5f, nearZ, farZ);
    }

    constexpr Vec4& operator[](usize c) { return columns[c]; }
    constexpr const Vec4& operator[](usize c) const { return columns[c]; }
    constexpr f32 At(usize row, usize col) const { return columns[col][row]; }
    constexpr Vec4 Row(usize r) const { return {columns[0][r], columns[1][r], columns[2][r], columns[3][r]}; }

    Vec4 operator*(Vec4 v) const;
    Mat4 operator*(const Mat4& o) const;
    Mat4& operator*=(const Mat4& o) { return *this = *this * o; }
    friend constexpr bool operator==(const Mat4&, const Mat4&) = default;

    /// Transforms a point (w = 1, with perspective divide when w != 1).
    Vec3 TransformPoint(Vec3 p) const;
    /// Transforms a point assuming an affine matrix (no divide).
    constexpr Vec3 TransformPointAffine(Vec3 p) const
    {
        return (columns[0] * p.x + columns[1] * p.y + columns[2] * p.z + columns[3]).XYZ();
    }
    /// Transforms a direction (w = 0).
    constexpr Vec3 TransformVector(Vec3 v) const { return (columns[0] * v.x + columns[1] * v.y + columns[2] * v.z).XYZ(); }

    constexpr Vec3 GetTranslation() const { return columns[3].XYZ(); }
    constexpr void SetTranslation(Vec3 t) { columns[3] = Vec4(t, 1.0f); }
    constexpr Mat3 GetMat3() const { return Mat3(columns[0].XYZ(), columns[1].XYZ(), columns[2].XYZ()); }
    Vec3 GetScale() const { return {Length(columns[0].XYZ()), Length(columns[1].XYZ()), Length(columns[2].XYZ())}; }
    constexpr Vec3 GetRight() const { return columns[0].XYZ(); }
    constexpr Vec3 GetUp() const { return columns[1].XYZ(); }
    constexpr Vec3 GetForward() const { return columns[2].XYZ(); }
};

Mat4 Transpose(const Mat4& m);
f32 Determinant(const Mat4& m);
/// General inverse. Returns identity (and asserts in debug) if singular.
Mat4 Inverse(const Mat4& m);
/// Fast inverse for affine matrices (rotation/scale/translation, last row 0001).
Mat4 InverseAffine(const Mat4& m);
/// Inverse-transpose of the upper 3x3 (for transforming normals).
Mat3 NormalMatrix(const Mat4& m);
bool IsNearlyEqual(const Mat4& a, const Mat4& b, f32 tolerance = KindaSmallNumber);

/// Splits an affine TRS matrix into components. Returns false if the matrix has zero scale.
bool Decompose(const Mat4& m, Vec3& translation, Quat& rotation, Vec3& scale);

template <>
struct Formatter<Mat4>
{
    static void Format(FormatSink& s, const Mat4& m, const FormatSpec& spec)
    {
        s.Append("[");
        for (usize r = 0; r < 4; ++r) {
            if (r)
                s.Append("; ");
            Formatter<Vec4>::Format(s, m.Row(r), spec);
        }
        s.Append("]");
    }
};

} // namespace zx
