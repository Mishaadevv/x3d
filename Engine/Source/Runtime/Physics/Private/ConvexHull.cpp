#include "ConvexHull.h"
#include "Core/Containers/HashMap.h"

namespace zx::physics {

namespace {

struct Tri
{
    u32 v[3];
    Vec3 n;
    f32 d = 0.0f;
    bool alive = true;
};

u64 EdgeKey(u32 a, u32 b) { return (u64(a) << 32) | b; }

Tri MakeTri(const Array<Vec3>& p, u32 a, u32 b, u32 c)
{
    Tri t;
    t.v[0] = a;
    t.v[1] = b;
    t.v[2] = c;
    t.n = SafeNormalize(Cross(p[b] - p[a], p[c] - p[a]), Vec3(0, 1, 0));
    t.d = Dot(t.n, p[a]);
    return t;
}

/// Unit vectors spread evenly over the sphere (Fibonacci lattice).
Vec3 FibonacciDirection(u32 i, u32 count)
{
    const f32 y = 1.0f - 2.0f * (f32(i) + 0.5f) / f32(count);
    const f32 r = Sqrt(Max(0.0f, 1.0f - y * y));
    const f32 phi = f32(i) * 2.39996323f;
    return Vec3(Cos(phi) * r, y, Sin(phi) * r);
}

/// Indices of the points extreme along `count` directions (deduplicated).
Array<Vec3> ExtremePoints(Span<const Vec3> points, u32 count)
{
    Array<u32> picked;
    for (u32 i = 0; i < count; ++i) {
        const Vec3 d = FibonacciDirection(i, count);
        u32 best = 0;
        f32 bestDot = -Infinity;
        for (u32 k = 0; k < points.Size(); ++k)
            if (const f32 dd = Dot(points[k], d); dd > bestDot) {
                bestDot = dd;
                best = k;
            }
        bool seen = false;
        for (u32 q : picked)
            seen |= q == best;
        if (!seen)
            picked.PushBack(best);
    }
    Array<Vec3> out;
    for (u32 q : picked)
        out.PushBack(points[q]);
    return out;
}

} // namespace

u32 ConvexHull::Support(Vec3 dir) const
{
    u32 best = 0;
    f32 bestDot = -Infinity;
    for (u32 i = 0; i < vertices.Size(); ++i)
        if (const f32 d = Dot(vertices[i], dir); d > bestDot) {
            bestDot = d;
            best = i;
        }
    return best;
}

bool ConvexHull::Build(Span<const Vec3> input, u32 maxVertices)
{
    vertices.Clear();
    faces.Clear();
    faceVertices.Clear();
    edges.Clear();
    edgeDir.Clear();
    directions.Clear();
    maxVertices = Clamp(maxVertices, 4u, MaxVertices);
    if (input.Size() < 4)
        return false;
    AABB box;
    for (Vec3 p : input)
        box.Expand(p);
    const f32 scale = Max(Length(box.Size()), 1e-6f);
    const f32 eps = scale * 1e-5f;
    // Unique points (big inputs first thinned to their extreme points: the hull is all that matters).
    Array<Vec3> pts;
    {
        Array<Vec3> source(input);
        if (source.Size() > 1024)
            source = ExtremePoints(input, 512);
        for (Vec3 p : source) {
            bool duplicate = false;
            for (Vec3 q : pts)
                if (DistanceSq(p, q) < eps * eps) {
                    duplicate = true;
                    break;
                }
            if (!duplicate)
                pts.PushBack(p);
        }
    }
    if (pts.Size() < 4)
        return false;

    // Initial tetrahedron from extreme points.
    u32 i0 = 0, i1 = 0;
    for (u32 i = 1; i < pts.Size(); ++i) {
        if (pts[i].x < pts[i0].x)
            i0 = i;
        if (pts[i].x > pts[i1].x)
            i1 = i;
    }
    if (i0 == i1)
        for (u32 i = 0; i < pts.Size(); ++i)
            if (DistanceSq(pts[i], pts[i0]) > DistanceSq(pts[i1], pts[i0]))
                i1 = i;
    u32 i2 = i0;
    f32 best = 0.0f;
    const Vec3 axis = SafeNormalize(pts[i1] - pts[i0], Vec3(1, 0, 0));
    for (u32 i = 0; i < pts.Size(); ++i) {
        const Vec3 d = pts[i] - pts[i0];
        const f32 dist = LengthSq(d - axis * Dot(d, axis));
        if (dist > best) {
            best = dist;
            i2 = i;
        }
    }
    if (best < eps * eps)
        return false;
    const Vec3 planeN = SafeNormalize(Cross(pts[i1] - pts[i0], pts[i2] - pts[i0]), Vec3(0, 1, 0));
    u32 i3 = i0;
    best = 0.0f;
    for (u32 i = 0; i < pts.Size(); ++i) {
        const f32 dist = Abs(Dot(pts[i] - pts[i0], planeN));
        if (dist > best) {
            best = dist;
            i3 = i;
        }
    }
    if (best < eps)
        return false; // flat
    Array<Tri> tris;
    const Vec3 inside = (pts[i0] + pts[i1] + pts[i2] + pts[i3]) * 0.25f;
    const u32 initial[4][3] = {{i0, i1, i2}, {i0, i3, i1}, {i0, i2, i3}, {i1, i3, i2}};
    for (const auto& f : initial) {
        Tri t = MakeTri(pts, f[0], f[1], f[2]);
        if (Dot(t.n, inside) - t.d > 0.0f)
            t = MakeTri(pts, f[0], f[2], f[1]);
        tris.PushBack(t);
    }
    // Add the remaining points one at a time: remove the faces they see, close the hole with a fan from the horizon.
    HashMap<u64, u32> edgeOwner;
    for (u32 p = 0; p < pts.Size(); ++p) {
        if (p == i0 || p == i1 || p == i2 || p == i3)
            continue;
        bool visibleAny = false;
        for (const Tri& t : tris)
            if (t.alive && Dot(t.n, pts[p]) - t.d > eps) {
                visibleAny = true;
                break;
            }
        if (!visibleAny)
            continue;
        edgeOwner.Clear();
        for (u32 ti = 0; ti < tris.Size(); ++ti)
            if (tris[ti].alive)
                for (u32 k = 0; k < 3; ++k)
                    edgeOwner.InsertOrAssign(EdgeKey(tris[ti].v[k], tris[ti].v[(k + 1) % 3]), ti);
        Array<u32> visible;
        for (u32 ti = 0; ti < tris.Size(); ++ti)
            if (tris[ti].alive && Dot(tris[ti].n, pts[p]) - tris[ti].d > eps)
                visible.PushBack(ti);
        Array<u32> horizon; // pairs
        for (u32 ti : visible)
            for (u32 k = 0; k < 3; ++k) {
                const u32 a = tris[ti].v[k], b = tris[ti].v[(k + 1) % 3];
                const u32* twin = edgeOwner.Find(EdgeKey(b, a));
                const bool twinVisible = twin && Dot(tris[*twin].n, pts[p]) - tris[*twin].d > eps;
                if (!twinVisible) {
                    horizon.PushBack(a);
                    horizon.PushBack(b);
                }
            }
        for (u32 ti : visible)
            tris[ti].alive = false;
        for (usize h = 0; h < horizon.Size(); h += 2)
            tris.PushBack(MakeTri(pts, horizon[h], horizon[h + 1], p));
    }

    // Used vertices; too many -> rebuild from the extreme ones.
    Array<u32> remap;
    remap.Resize(pts.Size(), ~0u);
    Array<Vec3> used;
    for (const Tri& t : tris)
        if (t.alive)
            for (u32 k = 0; k < 3; ++k)
                if (remap[t.v[k]] == ~0u) {
                    remap[t.v[k]] = u32(used.Size());
                    used.PushBack(pts[t.v[k]]);
                }
    if (used.Size() > maxVertices) {
        for (u32 count = maxVertices; count >= 8; --count) {
            Array<Vec3> reduced = ExtremePoints(used.View(), count);
            if (reduced.Size() <= maxVertices)
                return Build(reduced.View(), maxVertices);
        }
        return false;
    }
    vertices = move(used);
    Array<Tri> alive;
    for (const Tri& t : tris)
        if (t.alive) {
            Tri r = t;
            for (u32 k = 0; k < 3; ++k)
                r.v[k] = remap[t.v[k]];
            alive.PushBack(r);
        }

    // Merge coplanar neighbours into polygon faces.
    HashMap<u64, u32> owner;
    for (u32 ti = 0; ti < alive.Size(); ++ti)
        for (u32 k = 0; k < 3; ++k)
            owner.InsertOrAssign(EdgeKey(alive[ti].v[k], alive[ti].v[(k + 1) % 3]), ti);
    Array<i32> group;
    group.Resize(alive.Size(), -1);
    for (u32 seed = 0; seed < alive.Size(); ++seed) {
        if (group[seed] >= 0)
            continue;
        const i32 g = i32(seed);
        Array<u32> members;
        members.PushBack(seed);
        group[seed] = g;
        for (usize m = 0; m < members.Size(); ++m) {
            const Tri& t = alive[members[m]];
            for (u32 k = 0; k < 3; ++k) {
                const u32* nb = owner.Find(EdgeKey(t.v[(k + 1) % 3], t.v[k]));
                if (!nb || group[*nb] >= 0)
                    continue;
                const Tri& o = alive[*nb];
                if (Dot(o.n, alive[seed].n) > 1.0f - 1e-4f && Abs(o.d - alive[seed].d) < eps * 10.0f) {
                    group[*nb] = g;
                    members.PushBack(*nb);
                }
            }
        }
        // Boundary loop: directed edges of the group whose reverse is not in the group.
        HashMap<u32, u32> next;
        Vec3 normal;
        for (u32 mi : members) {
            const Tri& t = alive[mi];
            normal += Cross(vertices[t.v[1]] - vertices[t.v[0]], vertices[t.v[2]] - vertices[t.v[0]]);
            for (u32 k = 0; k < 3; ++k) {
                const u32 a = t.v[k], b = t.v[(k + 1) % 3];
                const u32* nb = owner.Find(EdgeKey(b, a));
                if (!nb || group[*nb] != g)
                    next.InsertOrAssign(a, b);
            }
        }
        Array<u8> loop;
        bool ok = !next.Empty();
        if (ok) {
            u32 start = 0;
            for (const auto& [from, to] : next) {
                start = from;
                break;
            }
            u32 cur = start;
            do {
                loop.PushBack(u8(cur));
                const u32* n = next.Find(cur);
                if (!n || loop.Size() > MaxFaceVertices) {
                    ok = false;
                    break;
                }
                cur = *n;
            } while (cur != start);
            ok &= loop.Size() == next.Size();
        }
        auto addFace = [&](Vec3 n, const u8* idx, u32 count) {
            if (faces.Size() >= MaxFaces)
                return;
            Face f;
            f.normal = SafeNormalize(n, Vec3(0, 1, 0));
            f.d = Dot(f.normal, vertices[idx[0]]);
            f.first = u8(faceVertices.Size());
            f.count = u8(count);
            for (u32 i = 0; i < count; ++i)
                faceVertices.PushBack(idx[i]);
            faces.PushBack(f);
        };
        if (ok && faceVertices.Size() + loop.Size() < 256) {
            addFace(normal, loop.Data(), u32(loop.Size()));
        } else {
            for (u32 mi : members) { // not a simple polygon: keep its triangles
                const u8 idx[3] = {u8(alive[mi].v[0]), u8(alive[mi].v[1]), u8(alive[mi].v[2])};
                if (faceVertices.Size() + 3 < 256)
                    addFace(alive[mi].n, idx, 3);
            }
        }
    }

    // Edges and their distinct directions.
    for (const Face& f : faces)
        for (u32 i = 0; i < f.count; ++i) {
            u8 a = faceVertices[f.first + i], b = faceVertices[f.first + (i + 1) % f.count];
            if (a > b)
                swap(a, b);
            bool seen = false;
            for (usize e = 0; e < edges.Size(); e += 2)
                seen |= edges[e] == a && edges[e + 1] == b;
            if (seen || edges.Size() / 2 >= MaxEdges)
                continue;
            const Vec3 dir = SafeNormalize(vertices[b] - vertices[a], Vec3(1, 0, 0));
            u32 d = u32(directions.Size());
            for (u32 k = 0; k < directions.Size(); ++k)
                if (Abs(Dot(directions[k], dir)) > 0.9999f) {
                    d = k;
                    break;
                }
            if (d == directions.Size()) {
                if (directions.Size() >= MaxDirections)
                    continue;
                directions.PushBack(dir);
            }
            edges.PushBack(a);
            edges.PushBack(b);
            edgeDir.PushBack(u8(d));
        }

    // Mass properties (unit density): tetrahedra from an interior point to every face triangle.
    bounds = AABB();
    Vec3 ref;
    for (Vec3 v : vertices) {
        bounds.Expand(v);
        ref += v;
    }
    ref = ref / f32(vertices.Size());
    volume = 0.0f;
    Vec3 weighted;
    f32 cov[3][3] = {};
    for (const Face& f : faces)
        for (u32 i = 1; i + 1 < f.count; ++i) {
            const Vec3 a = vertices[faceVertices[f.first]] - ref, b = vertices[faceVertices[f.first + i]] - ref,
                       c = vertices[faceVertices[f.first + i + 1]] - ref;
            const f32 det = Dot(a, Cross(b, c)); // 6 x signed volume
            volume += det / 6.0f;
            weighted += (a + b + c) * (det / 24.0f);
            // Covariance of the tetrahedron (0, a, b, c): det / 120 * (sum of products + sum of squares).
            const f32 A[3] = {a.x, a.y, a.z}, B[3] = {b.x, b.y, b.z}, C[3] = {c.x, c.y, c.z};
            for (u32 r = 0; r < 3; ++r)
                for (u32 s = 0; s < 3; ++s)
                    cov[r][s] += det / 120.0f *
                                 (2.0f * (A[r] * A[s] + B[r] * B[s] + C[r] * C[s]) + A[r] * B[s] + A[s] * B[r] + A[r] * C[s] + A[s] * C[r] +
                                  B[r] * C[s] + B[s] * C[r]);
        }
    if (volume <= 1e-12f)
        return false;
    const Vec3 com = weighted / volume; // relative to ref
    centroid = ref + com;
    const f32 c[3] = {com.x, com.y, com.z};
    for (u32 r = 0; r < 3; ++r)
        for (u32 s = 0; s < 3; ++s)
            cov[r][s] -= volume * c[r] * c[s];
    const f32 trace = cov[0][0] + cov[1][1] + cov[2][2];
    inertia = Mat3(Vec3(trace - cov[0][0], -cov[1][0], -cov[2][0]), Vec3(-cov[0][1], trace - cov[1][1], -cov[2][1]),
                   Vec3(-cov[0][2], -cov[1][2], trace - cov[2][2]));
    return faces.Size() >= 4;
}

// --- GJK -----------------------------------------------------------------------------------------------------------

namespace {

/// Closest point to the origin on the simplex w[0..n): barycentric weights of the best feature (brute force over the
/// sub-simplices; n <= 4). Returns the point.
Vec3 ClosestOnSimplex(const Vec3* w, u32 n, f32* lambda)
{
    Vec3 bestPoint = w[0];
    f32 bestDist = Infinity;
    f32 bestL[4] = {1, 0, 0, 0};
    for (u32 mask = 1; mask < (1u << n); ++mask) {
        u32 idx[4], k = 0;
        for (u32 i = 0; i < n; ++i)
            if (mask & (1u << i))
                idx[k++] = i;
        // Minimise |sum l_i w_i| with sum l_i = 1: Gram system on e_j = w_j - w_0.
        f32 l[4] = {1, 0, 0, 0};
        bool valid = true;
        if (k > 1) {
            const u32 m = k - 1;
            f32 G[3][3], rhs[3];
            Vec3 e[3];
            for (u32 j = 0; j < m; ++j)
                e[j] = w[idx[j + 1]] - w[idx[0]];
            for (u32 r = 0; r < m; ++r) {
                for (u32 s = 0; s < m; ++s)
                    G[r][s] = Dot(e[r], e[s]);
                rhs[r] = -Dot(e[r], w[idx[0]]);
            }
            f32 mu[3] = {};
            if (m == 1) {
                if (G[0][0] < 1e-12f)
                    valid = false;
                else
                    mu[0] = rhs[0] / G[0][0];
            } else if (m == 2) {
                const f32 det = G[0][0] * G[1][1] - G[0][1] * G[1][0];
                if (Abs(det) < 1e-14f)
                    valid = false;
                else {
                    mu[0] = (rhs[0] * G[1][1] - G[0][1] * rhs[1]) / det;
                    mu[1] = (G[0][0] * rhs[1] - rhs[0] * G[1][0]) / det;
                }
            } else {
                const f32 det = G[0][0] * (G[1][1] * G[2][2] - G[1][2] * G[2][1]) - G[0][1] * (G[1][0] * G[2][2] - G[1][2] * G[2][0]) +
                                G[0][2] * (G[1][0] * G[2][1] - G[1][1] * G[2][0]);
                if (Abs(det) < 1e-16f)
                    valid = false;
                else {
                    for (u32 col = 0; col < 3; ++col) {
                        f32 M[3][3];
                        for (u32 r = 0; r < 3; ++r)
                            for (u32 s = 0; s < 3; ++s)
                                M[r][s] = s == col ? rhs[r] : G[r][s];
                        mu[col] = (M[0][0] * (M[1][1] * M[2][2] - M[1][2] * M[2][1]) - M[0][1] * (M[1][0] * M[2][2] - M[1][2] * M[2][0]) +
                                   M[0][2] * (M[1][0] * M[2][1] - M[1][1] * M[2][0])) /
                                  det;
                    }
                }
            }
            if (valid) {
                f32 sum = 0.0f;
                for (u32 j = 0; j < m; ++j) {
                    l[j + 1] = mu[j];
                    sum += mu[j];
                }
                l[0] = 1.0f - sum;
                for (u32 j = 0; j < k; ++j)
                    valid &= l[j] >= -1e-6f;
            }
        }
        if (!valid)
            continue;
        Vec3 p;
        for (u32 j = 0; j < k; ++j)
            p += w[idx[j]] * l[j];
        const f32 d = LengthSq(p);
        if (d < bestDist - 1e-12f) {
            bestDist = d;
            bestPoint = p;
            for (u32 i = 0; i < 4; ++i)
                bestL[i] = 0.0f;
            for (u32 j = 0; j < k; ++j)
                bestL[idx[j]] = Max(l[j], 0.0f);
        }
    }
    for (u32 i = 0; i < n; ++i)
        lambda[i] = bestL[i];
    return bestPoint;
}

u32 SupportIndex(Span<const Vec3> s, Vec3 d)
{
    u32 best = 0;
    f32 bestDot = -Infinity;
    for (u32 i = 0; i < s.Size(); ++i)
        if (const f32 v = Dot(s[i], d); v > bestDot) {
            bestDot = v;
            best = i;
        }
    return best;
}

} // namespace

GjkResult Gjk(Span<const Vec3> a, Span<const Vec3> b)
{
    GjkResult r;
    if (a.Empty() || b.Empty())
        return r;
    Vec3 w[4];
    u32 ia[4], ib[4];
    u32 n = 1;
    ia[0] = 0;
    ib[0] = 0;
    w[0] = a[0] - b[0];
    f32 lambda[4] = {1, 0, 0, 0};
    Vec3 v = w[0];
    for (u32 iter = 0; iter < 64; ++iter) {
        const f32 vv = LengthSq(v);
        if (vv < 1e-12f) {
            r.distance = 0.0f;
            return r; // touching / overlapping
        }
        const u32 sa = SupportIndex(a, -v), sb = SupportIndex(b, v);
        const Vec3 nw = a[sa] - b[sb];
        if (vv - Dot(v, nw) <= 1e-6f * vv)
            break; // no further progress: v is the closest point
        bool repeated = false;
        for (u32 i = 0; i < n; ++i)
            repeated |= ia[i] == sa && ib[i] == sb;
        if (repeated || n == 4)
            break;
        w[n] = nw;
        ia[n] = sa;
        ib[n] = sb;
        ++n;
        v = ClosestOnSimplex(w, n, lambda);
        // Keep only the support set.
        u32 k = 0;
        for (u32 i = 0; i < n; ++i)
            if (lambda[i] > 0.0f) {
                w[k] = w[i];
                ia[k] = ia[i];
                ib[k] = ib[i];
                lambda[k] = lambda[i];
                ++k;
            }
        n = Max(k, 1u);
    }
    Vec3 pa, pb;
    f32 total = 0.0f;
    for (u32 i = 0; i < n; ++i)
        total += lambda[i];
    if (total <= 0.0f) {
        lambda[0] = 1.0f;
        total = 1.0f;
        n = 1;
    }
    for (u32 i = 0; i < n; ++i) {
        pa += a[ia[i]] * (lambda[i] / total);
        pb += b[ib[i]] * (lambda[i] / total);
    }
    r.pointA = pa;
    r.pointB = pb;
    r.distance = Distance(pa, pb);
    return r;
}

} // namespace zx::physics
