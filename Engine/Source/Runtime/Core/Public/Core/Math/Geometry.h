#pragma once

#include "Core/Math/Quat.h"

namespace zx {

/// Axis-aligned bounding box. Default-constructed box is "empty" (min > max) and grows with Expand().
struct AABB
{
    Vec3 min = Vec3(Infinity);
    Vec3 max = Vec3(-Infinity);

    constexpr AABB() = default;
    constexpr AABB(Vec3 mn, Vec3 mx) : min(mn), max(mx) {}
    static constexpr AABB FromCenterExtents(Vec3 center, Vec3 extents) { return {center - extents, center + extents}; }
    static AABB FromPoints(const Vec3* points, usize count);

    constexpr bool IsValid() const { return min.x <= max.x && min.y <= max.y && min.z <= max.z; }
    constexpr Vec3 Center() const { return (min + max) * 0.5f; }
    constexpr Vec3 Extents() const { return (max - min) * 0.5f; }
    constexpr Vec3 Size() const { return max - min; }
    constexpr f32 SurfaceArea() const
    {
        const Vec3 d = max - min;
        return 2.0f * (d.x * d.y + d.y * d.z + d.z * d.x);
    }
    constexpr f32 Volume() const
    {
        const Vec3 d = max - min;
        return d.x * d.y * d.z;
    }
    constexpr void Expand(Vec3 p)
    {
        min = zx::Min(min, p);
        max = zx::Max(max, p);
    }
    constexpr void Expand(const AABB& b)
    {
        min = zx::Min(min, b.min);
        max = zx::Max(max, b.max);
    }
    constexpr AABB Inflated(f32 amount) const { return {min - Vec3(amount), max + Vec3(amount)}; }
    constexpr bool Contains(Vec3 p) const
    {
        return p.x >= min.x && p.x <= max.x && p.y >= min.y && p.y <= max.y && p.z >= min.z && p.z <= max.z;
    }
    constexpr bool Contains(const AABB& b) const
    {
        return b.min.x >= min.x && b.max.x <= max.x && b.min.y >= min.y && b.max.y <= max.y && b.min.z >= min.z &&
               b.max.z <= max.z;
    }
    constexpr bool Intersects(const AABB& b) const
    {
        return min.x <= b.max.x && max.x >= b.min.x && min.y <= b.max.y && max.y >= b.min.y && min.z <= b.max.z &&
               max.z >= b.min.z;
    }
    constexpr Vec3 ClosestPoint(Vec3 p) const { return zx::Clamp(p, min, max); }
    f32 DistanceSq(Vec3 p) const { return zx::DistanceSq(ClosestPoint(p), p); }
    /// Corner i (bit 0 = x, bit 1 = y, bit 2 = z selects max).
    constexpr Vec3 Corner(u32 i) const { return {(i & 1) ? max.x : min.x, (i & 2) ? max.y : min.y, (i & 4) ? max.z : min.z}; }
    /// Bounds of the transformed box (Arvo's method).
    AABB Transformed(const Mat4& m) const;

    friend constexpr bool operator==(const AABB&, const AABB&) = default;
};

constexpr AABB Union(const AABB& a, const AABB& b) { return {Min(a.min, b.min), Max(a.max, b.max)}; }
constexpr AABB Intersection(const AABB& a, const AABB& b) { return {Max(a.min, b.min), Min(a.max, b.max)}; }

struct Sphere
{
    Vec3 center;
    f32 radius = 0.0f;

    constexpr Sphere() = default;
    constexpr Sphere(Vec3 c, f32 r) : center(c), radius(r) {}
    /// Bounding sphere of an AABB.
    static Sphere FromAABB(const AABB& b) { return {b.Center(), Length(b.Extents())}; }
    /// Ritter's approximate bounding sphere.
    static Sphere FromPoints(const Vec3* points, usize count);

    constexpr bool Contains(Vec3 p) const { return DistanceSq(p, center) <= radius * radius; }
    constexpr bool Intersects(const Sphere& o) const
    {
        const f32 r = radius + o.radius;
        return DistanceSq(center, o.center) <= r * r;
    }
    bool Intersects(const AABB& b) const { return b.DistanceSq(center) <= radius * radius; }
    /// Bounding sphere after an affine transform (radius scaled by the max axis scale).
    Sphere Transformed(const Mat4& m) const;
};

/// Plane: Dot(normal, p) + d = 0. Positive side is where Dot(normal, p) + d > 0.
struct Plane
{
    Vec3 normal = Vec3::Up();
    f32 d = 0.0f;

    constexpr Plane() = default;
    constexpr Plane(Vec3 n, f32 d_) : normal(n), d(d_) {}
    constexpr explicit Plane(Vec4 v) : normal(v.XYZ()), d(v.w) {}
    static constexpr Plane FromPointNormal(Vec3 point, Vec3 n) { return {n, -Dot(n, point)}; }
    /// Plane through three points; normal = normalize(cross(b - a, c - a)).
    static Plane FromPoints(Vec3 a, Vec3 b, Vec3 c);

    constexpr f32 SignedDistance(Vec3 p) const { return Dot(normal, p) + d; }
    constexpr Vec3 Project(Vec3 p) const { return p - normal * SignedDistance(p); }
    Plane Normalized() const
    {
        const f32 inv = 1.0f / Length(normal);
        return {normal * inv, d * inv};
    }
    constexpr Vec4 AsVec4() const { return Vec4(normal, d); }
};

struct Ray
{
    Vec3 origin;
    Vec3 direction = Vec3::Forward(); // normalized

    constexpr Ray() = default;
    constexpr Ray(Vec3 o, Vec3 dir) : origin(o), direction(dir) {}
    constexpr Vec3 At(f32 t) const { return origin + direction * t; }
};

/// View frustum as 6 inward-facing planes (point inside if all signed distances >= 0).
struct Frustum
{
    enum PlaneIndex : u32
    {
        Left,
        Right,
        Bottom,
        Top,
        Near,
        Far,
        PlaneCount
    };
    Plane planes[PlaneCount];
    /// Number of valid planes (an infinite-far projection has no far plane -> 5).
    u32 planeCount = PlaneCount;

    /// Extracts planes from a view-projection matrix (D3D clip conventions, reversed-Z aware).
    static Frustum FromMatrix(const Mat4& viewProjection);

    bool Contains(Vec3 p) const;
    bool Intersects(const Sphere& s) const;
    /// True if the sphere is completely inside (no plane intersects it).
    bool ContainsSphere(const Sphere& s) const;
    /// Conservative AABB test (may report intersection for boxes outside near corners).
    bool Intersects(const AABB& b) const;
};

/// Oriented bounding box.
struct OBB
{
    Vec3 center;
    Vec3 extents;
    Quat rotation;

    static OBB FromAABB(const AABB& b, const Mat4& m);
    AABB GetAABB() const;
    void GetCorners(Vec3 out[8]) const;
};

/// 2D axis-aligned rectangle.
struct Rect
{
    Vec2 min;
    Vec2 max;

    constexpr Rect() = default;
    constexpr Rect(Vec2 mn, Vec2 mx) : min(mn), max(mx) {}
    static constexpr Rect FromPosSize(Vec2 pos, Vec2 size) { return {pos, pos + size}; }
    static constexpr Rect FromXYWH(f32 x, f32 y, f32 w, f32 h) { return {{x, y}, {x + w, y + h}}; }

    constexpr f32 Width() const { return max.x - min.x; }
    constexpr f32 Height() const { return max.y - min.y; }
    constexpr Vec2 Size() const { return max - min; }
    constexpr Vec2 Center() const { return (min + max) * 0.5f; }
    constexpr bool IsEmpty() const { return max.x <= min.x || max.y <= min.y; }
    constexpr bool Contains(Vec2 p) const { return p.x >= min.x && p.x < max.x && p.y >= min.y && p.y < max.y; }
    constexpr bool Intersects(const Rect& o) const
    {
        return min.x < o.max.x && max.x > o.min.x && min.y < o.max.y && max.y > o.min.y;
    }
    constexpr Rect Intersect(const Rect& o) const { return {zx::Max(min, o.min), zx::Min(max, o.max)}; }
    constexpr Rect Union(const Rect& o) const { return {zx::Min(min, o.min), zx::Max(max, o.max)}; }
    constexpr Rect Expanded(f32 amount) const { return {min - Vec2(amount), max + Vec2(amount)}; }
    constexpr Rect Translated(Vec2 offset) const { return {min + offset, max + offset}; }
    friend constexpr bool operator==(const Rect&, const Rect&) = default;
};

struct IRect
{
    IVec2 min;
    IVec2 max;
    constexpr i32 Width() const { return max.x - min.x; }
    constexpr i32 Height() const { return max.y - min.y; }
    friend constexpr bool operator==(const IRect&, const IRect&) = default;
};

// --- Intersection queries -------------------------------------------------------------------------------------------

/// Slab test. On hit returns entry distance (0 if origin inside) in tNear and exit in tFar.
bool RayIntersectsAABB(const Ray& ray, const AABB& box, f32& tNear, f32& tFar, f32 maxDistance = Infinity);
bool RayIntersectsSphere(const Ray& ray, const Sphere& sphere, f32& t);
bool RayIntersectsPlane(const Ray& ray, const Plane& plane, f32& t);
/// Möller-Trumbore. Returns distance t and barycentrics (u, v) of the hit. Two-sided unless cullBackFace.
bool RayIntersectsTriangle(const Ray& ray, Vec3 a, Vec3 b, Vec3 c, f32& t, f32& u, f32& v, bool cullBackFace = false);

Vec3 ClosestPointOnSegment(Vec3 p, Vec3 a, Vec3 b);
Vec3 ClosestPointOnTriangle(Vec3 p, Vec3 a, Vec3 b, Vec3 c);
/// Closest points between segments p1-q1 and p2-q2; returns squared distance.
f32 ClosestPointsSegmentSegment(Vec3 p1, Vec3 q1, Vec3 p2, Vec3 q2, Vec3& c1, Vec3& c2);
/// Barycentric coordinates of p with respect to triangle abc.
Vec3 Barycentric(Vec3 p, Vec3 a, Vec3 b, Vec3 c);


// Reflection (defined in Core/Private/Reflection/CoreTypes.cpp).
class TypeInfo;
const TypeInfo& ZxTypeOf(AABB*);
const TypeInfo& ZxTypeOf(Sphere*);
const TypeInfo& ZxTypeOf(Rect*);

} // namespace zx
