#include "Core/Math/Matrix.h"
#include "Core/Assert.h"
#include "Core/Math/Quat.h"

#include <immintrin.h>

namespace zx {

// --- Mat3 -----------------------------------------------------------------------------------------------------------

Mat3 Mat3::FromQuat(const Quat& q)
{
    const f32 xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    const f32 xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    const f32 wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    return Mat3({1.0f - 2.0f * (yy + zz), 2.0f * (xy + wz), 2.0f * (xz - wy)},
                {2.0f * (xy - wz), 1.0f - 2.0f * (xx + zz), 2.0f * (yz + wx)},
                {2.0f * (xz + wy), 2.0f * (yz - wx), 1.0f - 2.0f * (xx + yy)});
}

Mat3 Inverse(const Mat3& m)
{
    const Vec3 a = m.columns[0], b = m.columns[1], c = m.columns[2];
    const Vec3 r0 = Cross(b, c);
    const Vec3 r1 = Cross(c, a);
    const Vec3 r2 = Cross(a, b);
    const f32 det = Dot(r2, c);
    if (Abs(det) < 1e-20f)
        return Mat3::Identity();
    const f32 inv = 1.0f / det;
    // Rows of the inverse are r0, r1, r2 (scaled) -> transpose into columns.
    return Transpose(Mat3(r0 * inv, r1 * inv, r2 * inv));
}

// --- Mat4 construction ----------------------------------------------------------------------------------------------

Mat4 Mat4::RotationX(f32 r)
{
    f32 s, c;
    SinCos(r, s, c);
    return Mat4({1, 0, 0, 0}, {0, c, s, 0}, {0, -s, c, 0}, {0, 0, 0, 1});
}

Mat4 Mat4::RotationY(f32 r)
{
    f32 s, c;
    SinCos(r, s, c);
    return Mat4({c, 0, -s, 0}, {0, 1, 0, 0}, {s, 0, c, 0}, {0, 0, 0, 1});
}

Mat4 Mat4::RotationZ(f32 r)
{
    f32 s, c;
    SinCos(r, s, c);
    return Mat4({c, s, 0, 0}, {-s, c, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1});
}

Mat4 Mat4::FromQuat(const Quat& q) { return Mat4(Mat3::FromQuat(q)); }

Mat4 Mat4::TRS(Vec3 t, const Quat& r, Vec3 s)
{
    const Mat3 m = Mat3::FromQuat(r);
    return Mat4(Vec4(m.columns[0] * s.x, 0), Vec4(m.columns[1] * s.y, 0), Vec4(m.columns[2] * s.z, 0), Vec4(t, 1));
}

Mat4 Mat4::LookTo(Vec3 eye, Vec3 forward, Vec3 up)
{
    const Vec3 f = SafeNormalize(forward, Vec3::Forward());
    Vec3 r = Cross(up, f);
    if (LengthSq(r) < 1e-8f)
        r = Cross(Abs(f.y) < 0.99f ? Vec3::Up() : Vec3::Forward(), f);
    r = Normalize(r);
    const Vec3 u = Cross(f, r);
    // Rows of the view matrix are the camera basis vectors.
    return Mat4({r.x, u.x, f.x, 0}, {r.y, u.y, f.y, 0}, {r.z, u.z, f.z, 0}, {-Dot(r, eye), -Dot(u, eye), -Dot(f, eye), 1});
}

Mat4 Mat4::LookAt(Vec3 eye, Vec3 target, Vec3 up) { return LookTo(eye, target - eye, up); }

Mat4 Mat4::PerspectiveReversedZ(f32 fovY, f32 aspect, f32 n, f32 f)
{
    const f32 h = 1.0f / Tan(fovY * 0.5f);
    const f32 w = h / aspect;
    const f32 a = n / (n - f);
    const f32 b = (f * n) / (f - n);
    return Mat4({w, 0, 0, 0}, {0, h, 0, 0}, {0, 0, a, 1}, {0, 0, b, 0});
}

Mat4 Mat4::PerspectiveInfiniteReversedZ(f32 fovY, f32 aspect, f32 n)
{
    const f32 h = 1.0f / Tan(fovY * 0.5f);
    const f32 w = h / aspect;
    return Mat4({w, 0, 0, 0}, {0, h, 0, 0}, {0, 0, 0, 1}, {0, 0, n, 0});
}

Mat4 Mat4::OrthographicReversedZ(f32 l, f32 r, f32 b, f32 t, f32 n, f32 f)
{
    const f32 rl = 1.0f / (r - l);
    const f32 tb = 1.0f / (t - b);
    const f32 fn = 1.0f / (f - n);
    return Mat4({2.0f * rl, 0, 0, 0}, {0, 2.0f * tb, 0, 0}, {0, 0, -fn, 0}, {-(r + l) * rl, -(t + b) * tb, f * fn, 1});
}

// --- Mat4 math (SSE) ------------------------------------------------------------------------------------------------

Vec4 Mat4::operator*(Vec4 v) const
{
    const __m128 c0 = _mm_load_ps(&columns[0].x);
    const __m128 c1 = _mm_load_ps(&columns[1].x);
    const __m128 c2 = _mm_load_ps(&columns[2].x);
    const __m128 c3 = _mm_load_ps(&columns[3].x);
    __m128 r = _mm_mul_ps(c0, _mm_set1_ps(v.x));
    r = _mm_add_ps(r, _mm_mul_ps(c1, _mm_set1_ps(v.y)));
    r = _mm_add_ps(r, _mm_mul_ps(c2, _mm_set1_ps(v.z)));
    r = _mm_add_ps(r, _mm_mul_ps(c3, _mm_set1_ps(v.w)));
    Vec4 out;
    _mm_store_ps(&out.x, r);
    return out;
}

Mat4 Mat4::operator*(const Mat4& o) const
{
    const __m128 a0 = _mm_load_ps(&columns[0].x);
    const __m128 a1 = _mm_load_ps(&columns[1].x);
    const __m128 a2 = _mm_load_ps(&columns[2].x);
    const __m128 a3 = _mm_load_ps(&columns[3].x);
    Mat4 out;
    for (int c = 0; c < 4; ++c) {
        const Vec4& b = o.columns[c];
        __m128 r = _mm_mul_ps(a0, _mm_set1_ps(b.x));
        r = _mm_add_ps(r, _mm_mul_ps(a1, _mm_set1_ps(b.y)));
        r = _mm_add_ps(r, _mm_mul_ps(a2, _mm_set1_ps(b.z)));
        r = _mm_add_ps(r, _mm_mul_ps(a3, _mm_set1_ps(b.w)));
        _mm_store_ps(&out.columns[c].x, r);
    }
    return out;
}

Vec3 Mat4::TransformPoint(Vec3 p) const
{
    const Vec4 r = *this * Vec4(p, 1.0f);
    if (r.w != 1.0f && r.w != 0.0f)
        return r.XYZ() * (1.0f / r.w);
    return r.XYZ();
}

Mat4 Transpose(const Mat4& m)
{
    __m128 c0 = _mm_load_ps(&m.columns[0].x);
    __m128 c1 = _mm_load_ps(&m.columns[1].x);
    __m128 c2 = _mm_load_ps(&m.columns[2].x);
    __m128 c3 = _mm_load_ps(&m.columns[3].x);
    _MM_TRANSPOSE4_PS(c0, c1, c2, c3);
    Mat4 out;
    _mm_store_ps(&out.columns[0].x, c0);
    _mm_store_ps(&out.columns[1].x, c1);
    _mm_store_ps(&out.columns[2].x, c2);
    _mm_store_ps(&out.columns[3].x, c3);
    return out;
}

namespace {
// Cofactor-based inverse helpers working on element (row, col) = m.columns[col][row].
struct Elements
{
    f32 m[4][4]; // m[row][col]
    explicit Elements(const Mat4& a)
    {
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                m[r][c] = a.columns[c][usize(r)];
    }
};
} // namespace

f32 Determinant(const Mat4& a)
{
    const Elements e(a);
    const auto& m = e.m;
    const f32 s0 = m[0][0] * m[1][1] - m[1][0] * m[0][1];
    const f32 s1 = m[0][0] * m[1][2] - m[1][0] * m[0][2];
    const f32 s2 = m[0][0] * m[1][3] - m[1][0] * m[0][3];
    const f32 s3 = m[0][1] * m[1][2] - m[1][1] * m[0][2];
    const f32 s4 = m[0][1] * m[1][3] - m[1][1] * m[0][3];
    const f32 s5 = m[0][2] * m[1][3] - m[1][2] * m[0][3];
    const f32 c5 = m[2][2] * m[3][3] - m[3][2] * m[2][3];
    const f32 c4 = m[2][1] * m[3][3] - m[3][1] * m[2][3];
    const f32 c3 = m[2][1] * m[3][2] - m[3][1] * m[2][2];
    const f32 c2 = m[2][0] * m[3][3] - m[3][0] * m[2][3];
    const f32 c1 = m[2][0] * m[3][2] - m[3][0] * m[2][2];
    const f32 c0 = m[2][0] * m[3][1] - m[3][0] * m[2][1];
    return s0 * c5 - s1 * c4 + s2 * c3 + s3 * c2 - s4 * c1 + s5 * c0;
}

Mat4 Inverse(const Mat4& a)
{
    const Elements e(a);
    const auto& m = e.m;
    const f32 s0 = m[0][0] * m[1][1] - m[1][0] * m[0][1];
    const f32 s1 = m[0][0] * m[1][2] - m[1][0] * m[0][2];
    const f32 s2 = m[0][0] * m[1][3] - m[1][0] * m[0][3];
    const f32 s3 = m[0][1] * m[1][2] - m[1][1] * m[0][2];
    const f32 s4 = m[0][1] * m[1][3] - m[1][1] * m[0][3];
    const f32 s5 = m[0][2] * m[1][3] - m[1][2] * m[0][3];
    const f32 c5 = m[2][2] * m[3][3] - m[3][2] * m[2][3];
    const f32 c4 = m[2][1] * m[3][3] - m[3][1] * m[2][3];
    const f32 c3 = m[2][1] * m[3][2] - m[3][1] * m[2][2];
    const f32 c2 = m[2][0] * m[3][3] - m[3][0] * m[2][3];
    const f32 c1 = m[2][0] * m[3][2] - m[3][0] * m[2][2];
    const f32 c0 = m[2][0] * m[3][1] - m[3][0] * m[2][1];
    const f32 det = s0 * c5 - s1 * c4 + s2 * c3 + s3 * c2 - s4 * c1 + s5 * c0;
    if (Abs(det) < 1e-30f) {
        ZX_CHECK_SLOW(false, "Inverse of a singular matrix");
        return Mat4::Identity();
    }
    const f32 inv = 1.0f / det;
    f32 r[4][4];
    r[0][0] = (m[1][1] * c5 - m[1][2] * c4 + m[1][3] * c3) * inv;
    r[0][1] = (-m[0][1] * c5 + m[0][2] * c4 - m[0][3] * c3) * inv;
    r[0][2] = (m[3][1] * s5 - m[3][2] * s4 + m[3][3] * s3) * inv;
    r[0][3] = (-m[2][1] * s5 + m[2][2] * s4 - m[2][3] * s3) * inv;
    r[1][0] = (-m[1][0] * c5 + m[1][2] * c2 - m[1][3] * c1) * inv;
    r[1][1] = (m[0][0] * c5 - m[0][2] * c2 + m[0][3] * c1) * inv;
    r[1][2] = (-m[3][0] * s5 + m[3][2] * s2 - m[3][3] * s1) * inv;
    r[1][3] = (m[2][0] * s5 - m[2][2] * s2 + m[2][3] * s1) * inv;
    r[2][0] = (m[1][0] * c4 - m[1][1] * c2 + m[1][3] * c0) * inv;
    r[2][1] = (-m[0][0] * c4 + m[0][1] * c2 - m[0][3] * c0) * inv;
    r[2][2] = (m[3][0] * s4 - m[3][1] * s2 + m[3][3] * s0) * inv;
    r[2][3] = (-m[2][0] * s4 + m[2][1] * s2 - m[2][3] * s0) * inv;
    r[3][0] = (-m[1][0] * c3 + m[1][1] * c1 - m[1][2] * c0) * inv;
    r[3][1] = (m[0][0] * c3 - m[0][1] * c1 + m[0][2] * c0) * inv;
    r[3][2] = (-m[3][0] * s3 + m[3][1] * s1 - m[3][2] * s0) * inv;
    r[3][3] = (m[2][0] * s3 - m[2][1] * s1 + m[2][2] * s0) * inv;
    Mat4 out;
    for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 4; ++col)
            out.columns[col][usize(row)] = r[row][col];
    return out;
}

Mat4 InverseAffine(const Mat4& m)
{
    const Mat3 inv = Inverse(m.GetMat3());
    const Vec3 t = -(inv * m.GetTranslation());
    return Mat4(inv, t);
}

Mat3 NormalMatrix(const Mat4& m) { return Transpose(Inverse(m.GetMat3())); }

bool IsNearlyEqual(const Mat4& a, const Mat4& b, f32 tolerance)
{
    for (usize c = 0; c < 4; ++c)
        if (!IsNearlyEqual(a.columns[c], b.columns[c], tolerance))
            return false;
    return true;
}

bool Decompose(const Mat4& m, Vec3& translation, Quat& rotation, Vec3& scale)
{
    translation = m.GetTranslation();
    Vec3 c0 = m.columns[0].XYZ(), c1 = m.columns[1].XYZ(), c2 = m.columns[2].XYZ();
    scale = Vec3(Length(c0), Length(c1), Length(c2));
    if (scale.x < SmallNumber || scale.y < SmallNumber || scale.z < SmallNumber) {
        rotation = Quat::Identity();
        return false;
    }
    // Negative determinant: mirror -> flip one axis' scale.
    if (Dot(c0, Cross(c1, c2)) < 0.0f)
        scale.x = -scale.x;
    c0 = c0 / scale.x;
    c1 = c1 / scale.y;
    c2 = c2 / scale.z;
    rotation = Quat::FromMat3(Mat3(c0, c1, c2));
    return true;
}

} // namespace zx
