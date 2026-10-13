#include "Core/Math/Geometry.h"

namespace zx {

AABB AABB::FromPoints(const Vec3* points, usize count)
{
    AABB b;
    for (usize i = 0; i < count; ++i)
        b.Expand(points[i]);
    return b;
}

AABB AABB::Transformed(const Mat4& m) const
{
    if (!IsValid())
        return *this;
    // Arvo: new center = M * center; new extents = |M3| * extents.
    const Vec3 c = m.TransformPointAffine(Center());
    const Vec3 e = Extents();
    const Vec3 ne = Abs(m.columns[0].XYZ()) * e.x + Abs(m.columns[1].XYZ()) * e.y + Abs(m.columns[2].XYZ()) * e.z;
    return {c - ne, c + ne};
}

Sphere Sphere::FromPoints(const Vec3* points, usize count)
{
    if (count == 0)
        return {};
    // Ritter: find a far pair, start with their sphere, grow to include outliers.
    Vec3 x = points[0];
    Vec3 y = x;
    f32 best = -1.0f;
    for (usize i = 0; i < count; ++i) {
        const f32 d = DistanceSq(points[i], x);
        if (d > best) {
            best = d;
            y = points[i];
        }
    }
    Vec3 z = y;
    best = -1.0f;
    for (usize i = 0; i < count; ++i) {
        const f32 d = DistanceSq(points[i], y);
        if (d > best) {
            best = d;
            z = points[i];
        }
    }
    Sphere s((y + z) * 0.5f, Sqrt(best) * 0.5f);
    for (usize i = 0; i < count; ++i) {
        const f32 d2 = DistanceSq(points[i], s.center);
        if (d2 > s.radius * s.radius) {
            const f32 d = Sqrt(d2);
            const f32 newRadius = (s.radius + d) * 0.5f;
            s.center = s.center + (points[i] - s.center) * ((newRadius - s.radius) / d);
            s.radius = newRadius;
        }
    }
    return s;
}

Sphere Sphere::Transformed(const Mat4& m) const
{
    const Vec3 s = m.GetScale();
    return {m.TransformPointAffine(center), radius * MaxComponent(s)};
}

Plane Plane::FromPoints(Vec3 a, Vec3 b, Vec3 c)
{
    const Vec3 n = SafeNormalize(Cross(b - a, c - a), Vec3::Up());
    return FromPointNormal(a, n);
}

Frustum Frustum::FromMatrix(const Mat4& m)
{
    // Clip-space conditions (D3D): -w<=x<=w, -w<=y<=w, 0<=z<=w. With reversed-Z, z>=0 is the far side and z<=w
    // the near side. Each condition r_a . v >= 0 is a plane whose coefficients are rows of M.
    const Vec4 r0 = m.Row(0), r1 = m.Row(1), r2 = m.Row(2), r3 = m.Row(3);
    const Vec4 raw[PlaneCount] = {r3 + r0, r3 - r0, r3 + r1, r3 - r1, r3 - r2, r2};
    Frustum f;
    f.planeCount = 0;
    for (u32 i = 0; i < PlaneCount; ++i) {
        const f32 len = Length(raw[i].XYZ());
        if (len < 1e-6f)
            continue; // degenerate plane (infinite far)
        f.planes[f.planeCount++] = Plane(raw[i].XYZ() / len, raw[i].w / len);
    }
    return f;
}

bool Frustum::Contains(Vec3 p) const
{
    for (u32 i = 0; i < planeCount; ++i)
        if (planes[i].SignedDistance(p) < 0.0f)
            return false;
    return true;
}

bool Frustum::Intersects(const Sphere& s) const
{
    for (u32 i = 0; i < planeCount; ++i)
        if (planes[i].SignedDistance(s.center) < -s.radius)
            return false;
    return true;
}

bool Frustum::ContainsSphere(const Sphere& s) const
{
    for (u32 i = 0; i < planeCount; ++i)
        if (planes[i].SignedDistance(s.center) < s.radius)
            return false;
    return true;
}

bool Frustum::Intersects(const AABB& b) const
{
    const Vec3 c = b.Center();
    const Vec3 e = b.Extents();
    for (u32 i = 0; i < planeCount; ++i) {
        const Plane& p = planes[i];
        const f32 r = e.x * Abs(p.normal.x) + e.y * Abs(p.normal.y) + e.z * Abs(p.normal.z);
        if (p.SignedDistance(c) < -r)
            return false;
    }
    return true;
}

OBB OBB::FromAABB(const AABB& b, const Mat4& m)
{
    OBB o;
    Vec3 t, s;
    Decompose(m, t, o.rotation, s);
    o.center = m.TransformPointAffine(b.Center());
    o.extents = b.Extents() * Abs(s);
    return o;
}

AABB OBB::GetAABB() const
{
    const Mat3 r = Mat3::FromQuat(rotation);
    const Vec3 ne = Abs(r.columns[0]) * extents.x + Abs(r.columns[1]) * extents.y + Abs(r.columns[2]) * extents.z;
    return {center - ne, center + ne};
}

void OBB::GetCorners(Vec3 out[8]) const
{
    const Vec3 ax = rotation * Vec3(extents.x, 0, 0);
    const Vec3 ay = rotation * Vec3(0, extents.y, 0);
    const Vec3 az = rotation * Vec3(0, 0, extents.z);
    for (u32 i = 0; i < 8; ++i)
        out[i] = center + ((i & 1) ? ax : -ax) + ((i & 2) ? ay : -ay) + ((i & 4) ? az : -az);
}

bool RayIntersectsAABB(const Ray& ray, const AABB& box, f32& tNear, f32& tFar, f32 maxDistance)
{
    f32 t0 = 0.0f, t1 = maxDistance;
    for (usize a = 0; a < 3; ++a) {
        const f32 o = ray.origin[a];
        const f32 d = ray.direction[a];
        if (Abs(d) < 1e-12f) {
            if (o < box.min[a] || o > box.max[a])
                return false;
            continue;
        }
        const f32 inv = 1.0f / d;
        f32 tn = (box.min[a] - o) * inv;
        f32 tf = (box.max[a] - o) * inv;
        if (tn > tf)
            swap(tn, tf);
        t0 = tn > t0 ? tn : t0;
        t1 = tf < t1 ? tf : t1;
        if (t0 > t1)
            return false;
    }
    tNear = t0;
    tFar = t1;
    return true;
}

bool RayIntersectsSphere(const Ray& ray, const Sphere& s, f32& t)
{
    const Vec3 m = ray.origin - s.center;
    const f32 b = Dot(m, ray.direction);
    const f32 c = Dot(m, m) - s.radius * s.radius;
    if (c > 0.0f && b > 0.0f)
        return false;
    const f32 disc = b * b - c;
    if (disc < 0.0f)
        return false;
    t = -b - Sqrt(disc);
    if (t < 0.0f)
        t = 0.0f; // origin inside
    return true;
}

bool RayIntersectsPlane(const Ray& ray, const Plane& plane, f32& t)
{
    const f32 denom = Dot(plane.normal, ray.direction);
    if (Abs(denom) < 1e-8f)
        return false;
    t = -plane.SignedDistance(ray.origin) / denom;
    return t >= 0.0f;
}

bool RayIntersectsTriangle(const Ray& ray, Vec3 a, Vec3 b, Vec3 c, f32& t, f32& u, f32& v, bool cullBackFace)
{
    const Vec3 e1 = b - a;
    const Vec3 e2 = c - a;
    const Vec3 p = Cross(ray.direction, e2);
    const f32 det = Dot(e1, p);
    if (cullBackFace ? det < 1e-10f : Abs(det) < 1e-10f)
        return false;
    const f32 inv = 1.0f / det;
    const Vec3 s = ray.origin - a;
    u = Dot(s, p) * inv;
    if (u < 0.0f || u > 1.0f)
        return false;
    const Vec3 q = Cross(s, e1);
    v = Dot(ray.direction, q) * inv;
    if (v < 0.0f || u + v > 1.0f)
        return false;
    t = Dot(e2, q) * inv;
    return t >= 0.0f;
}

Vec3 ClosestPointOnSegment(Vec3 p, Vec3 a, Vec3 b)
{
    const Vec3 ab = b - a;
    const f32 denom = Dot(ab, ab);
    if (denom < 1e-12f)
        return a;
    return a + ab * Saturate(Dot(p - a, ab) / denom);
}

Vec3 ClosestPointOnTriangle(Vec3 p, Vec3 a, Vec3 b, Vec3 c)
{
    // Ericson, Real-Time Collision Detection 5.1.5.
    const Vec3 ab = b - a, ac = c - a, ap = p - a;
    const f32 d1 = Dot(ab, ap), d2 = Dot(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f)
        return a;
    const Vec3 bp = p - b;
    const f32 d3 = Dot(ab, bp), d4 = Dot(ac, bp);
    if (d3 >= 0.0f && d4 <= d3)
        return b;
    const f32 vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f)
        return a + ab * (d1 / (d1 - d3));
    const Vec3 cp = p - c;
    const f32 d5 = Dot(ab, cp), d6 = Dot(ac, cp);
    if (d6 >= 0.0f && d5 <= d6)
        return c;
    const f32 vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f)
        return a + ac * (d2 / (d2 - d6));
    const f32 va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f)
        return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
    const f32 denom = 1.0f / (va + vb + vc);
    return a + ab * (vb * denom) + ac * (vc * denom);
}

f32 ClosestPointsSegmentSegment(Vec3 p1, Vec3 q1, Vec3 p2, Vec3 q2, Vec3& c1, Vec3& c2)
{
    // Ericson 5.1.9.
    const Vec3 d1 = q1 - p1, d2 = q2 - p2, r = p1 - p2;
    const f32 a = Dot(d1, d1), e = Dot(d2, d2), f = Dot(d2, r);
    f32 s, t;
    if (a <= 1e-12f && e <= 1e-12f) {
        c1 = p1;
        c2 = p2;
        return Dot(c1 - c2, c1 - c2);
    }
    if (a <= 1e-12f) {
        s = 0.0f;
        t = Saturate(f / e);
    } else {
        const f32 c = Dot(d1, r);
        if (e <= 1e-12f) {
            t = 0.0f;
            s = Saturate(-c / a);
        } else {
            const f32 b = Dot(d1, d2);
            const f32 denom = a * e - b * b;
            s = denom != 0.0f ? Saturate((b * f - c * e) / denom) : 0.0f;
            t = (b * s + f) / e;
            if (t < 0.0f) {
                t = 0.0f;
                s = Saturate(-c / a);
            } else if (t > 1.0f) {
                t = 1.0f;
                s = Saturate((b - c) / a);
            }
        }
    }
    c1 = p1 + d1 * s;
    c2 = p2 + d2 * t;
    return Dot(c1 - c2, c1 - c2);
}

Vec3 Barycentric(Vec3 p, Vec3 a, Vec3 b, Vec3 c)
{
    const Vec3 v0 = b - a, v1 = c - a, v2 = p - a;
    const f32 d00 = Dot(v0, v0), d01 = Dot(v0, v1), d11 = Dot(v1, v1);
    const f32 d20 = Dot(v2, v0), d21 = Dot(v2, v1);
    const f32 denom = d00 * d11 - d01 * d01;
    if (Abs(denom) < 1e-20f)
        return {1.0f, 0.0f, 0.0f};
    const f32 v = (d11 * d20 - d01 * d21) / denom;
    const f32 w = (d00 * d21 - d01 * d20) / denom;
    return {1.0f - v - w, v, w};
}

} // namespace zx
