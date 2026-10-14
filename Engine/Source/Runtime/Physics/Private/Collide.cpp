#include "Collide.h"

#include <cmath>

namespace zx::physics {

namespace {

constexpr f32 Eps = 1e-6f;

Vec3 InvRotate(const Quat& q, Vec3 v) { return Conjugate(q) * v; }

/// Closest points between segments p1q1 and p2q2 (Ericson, Real-Time Collision Detection 5.1.9).
void ClosestSegmentSegment(Vec3 p1, Vec3 q1, Vec3 p2, Vec3 q2, Vec3& c1, Vec3& c2)
{
    const Vec3 d1 = q1 - p1, d2 = q2 - p2, r = p1 - p2;
    const f32 a = Dot(d1, d1), e = Dot(d2, d2), f = Dot(d2, r);
    f32 s = 0.0f, t = 0.0f;
    if (a <= Eps && e <= Eps) {
        c1 = p1;
        c2 = p2;
        return;
    }
    if (a <= Eps) {
        t = Clamp(f / e, 0.0f, 1.0f);
    } else {
        const f32 c = Dot(d1, r);
        if (e <= Eps) {
            s = Clamp(-c / a, 0.0f, 1.0f);
        } else {
            const f32 b = Dot(d1, d2), denom = a * e - b * b;
            s = denom > Eps ? Clamp((b * f - c * e) / denom, 0.0f, 1.0f) : 0.0f;
            t = (b * s + f) / e;
            if (t < 0.0f) {
                t = 0.0f;
                s = Clamp(-c / a, 0.0f, 1.0f);
            } else if (t > 1.0f) {
                t = 1.0f;
                s = Clamp((b - c) / a, 0.0f, 1.0f);
            }
        }
    }
    c1 = p1 + d1 * s;
    c2 = p2 + d2 * t;
}

Vec3 ClosestPointSegment(Vec3 p, Vec3 a, Vec3 b)
{
    const Vec3 ab = b - a;
    const f32 len = Dot(ab, ab);
    if (len <= Eps)
        return a;
    return a + ab * Clamp(Dot(p - a, ab) / len, 0.0f, 1.0f);
}

void FlipManifold(Manifold& m)
{
    m.normal = -m.normal;
    for (u32 i = 0; i < m.count; ++i)
        swap(m.points[i].pointA, m.points[i].pointB);
}

void AddPoint(Manifold& m, Vec3 pointA, Vec3 pointB, f32 separation)
{
    if (m.count < 4)
        m.points[m.count++] = ContactPoint{pointA, pointB, separation};
}

/// Keeps the 4 points that span the largest area (deepest first).
void ReduceTo4(Vec3 normal, Array<ContactPoint>& pts, Manifold& m)
{
    m.count = 0;
    if (pts.Size() <= 4) {
        for (const ContactPoint& p : pts)
            m.points[m.count++] = p;
        return;
    }
    usize i0 = 0;
    for (usize i = 1; i < pts.Size(); ++i)
        if (pts[i].separation < pts[i0].separation)
            i0 = i;
    usize i1 = i0;
    f32 best = -1.0f;
    for (usize i = 0; i < pts.Size(); ++i) {
        const f32 d = DistanceSq(pts[i].pointB, pts[i0].pointB);
        if (d > best) {
            best = d;
            i1 = i;
        }
    }
    usize i2 = i0;
    f32 bestArea = 0.0f;
    for (usize i = 0; i < pts.Size(); ++i) {
        const f32 area = Dot(Cross(pts[i1].pointB - pts[i0].pointB, pts[i].pointB - pts[i0].pointB), normal);
        if (Abs(area) > Abs(bestArea)) {
            bestArea = area;
            i2 = i;
        }
    }
    usize i3 = i0;
    f32 bestOther = 0.0f;
    for (usize i = 0; i < pts.Size(); ++i) {
        if (i == i0 || i == i1 || i == i2)
            continue;
        // Area on the opposite side of the i0-i1 diagonal from i2 (completes the quad).
        const f32 area = Dot(Cross(pts[i1].pointB - pts[i0].pointB, pts[i].pointB - pts[i0].pointB), normal);
        if (area * bestArea < 0.0f && Abs(area) > bestOther) {
            bestOther = Abs(area);
            i3 = i;
        }
    }
    m.points[m.count++] = pts[i0];
    if (i1 != i0)
        m.points[m.count++] = pts[i1];
    if (i2 != i0 && i2 != i1)
        m.points[m.count++] = pts[i2];
    if (i3 != i0 && i3 != i1 && i3 != i2)
        m.points[m.count++] = pts[i3];
}

// --- Polyhedra (boxes, triangles): SAT + face clipping -------------------------------------------------------------------

struct Poly
{
    Vec3 v[ConvexHull::MaxVertices];
    u32 nv = 0;
    Vec3 fn[ConvexHull::MaxFaces];
    u8 face[ConvexHull::MaxFaces][ConvexHull::MaxFaceVertices];
    u8 fcount[ConvexHull::MaxFaces];
    u32 nf = 0;
    u8 edge[ConvexHull::MaxEdges][2];
    u8 edgeDir[ConvexHull::MaxEdges];
    u32 ne = 0;
    Vec3 dirs[ConvexHull::MaxDirections];
    u32 ndirs = 0;
    Vec3 center;

    void FixWinding(u32 f)
    {
        const Vec3 a = v[face[f][0]], b = v[face[f][1]], c = v[face[f][2]];
        if (Dot(Cross(b - a, c - b), fn[f]) < 0.0f)
            for (u32 i = 0; i < u32(fcount[f]) / 2; ++i)
                swap(face[f][i], face[f][fcount[f] - 1 - i]);
    }
};

void MakeBoxPoly(Vec3 pos, const Quat& rot, Vec3 e, Poly& p)
{
    const Vec3 ax = rot * Vec3(1, 0, 0), ay = rot * Vec3(0, 1, 0), az = rot * Vec3(0, 0, 1);
    p.nv = 8;
    for (u32 i = 0; i < 8; ++i)
        p.v[i] = pos + ax * ((i & 1) ? e.x : -e.x) + ay * ((i & 2) ? e.y : -e.y) + az * ((i & 4) ? e.z : -e.z);
    static constexpr u8 Faces[6][4] = {{1, 3, 7, 5}, {0, 2, 6, 4}, {2, 3, 7, 6}, {0, 1, 5, 4}, {4, 5, 7, 6}, {0, 1, 3, 2}};
    const Vec3 normals[6] = {ax, -ax, ay, -ay, az, -az};
    p.nf = 6;
    for (u32 f = 0; f < 6; ++f) {
        p.fn[f] = normals[f];
        p.fcount[f] = 4;
        for (u32 k = 0; k < 4; ++k)
            p.face[f][k] = Faces[f][k];
        p.FixWinding(f);
    }
    static constexpr u8 Edges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    p.ne = 12;
    for (u32 i = 0; i < 12; ++i) {
        p.edge[i][0] = Edges[i][0];
        p.edge[i][1] = Edges[i][1];
        p.edgeDir[i] = u8(i / 4);
    }
    p.ndirs = 3;
    p.dirs[0] = ax;
    p.dirs[1] = ay;
    p.dirs[2] = az;
    p.center = pos;
}

void MakeHullPoly(Vec3 pos, const Quat& rot, const ConvexHull& h, Poly& p)
{
    p.nv = u32(h.vertices.Size());
    p.center = Vec3();
    for (u32 i = 0; i < p.nv; ++i) {
        p.v[i] = pos + rot * h.vertices[i];
        p.center += p.v[i];
    }
    p.center = p.center / f32(Max(p.nv, 1u));
    p.nf = u32(h.faces.Size());
    for (u32 f = 0; f < p.nf; ++f) {
        const ConvexHull::Face& face = h.faces[f];
        p.fn[f] = rot * face.normal;
        p.fcount[f] = face.count;
        for (u32 k = 0; k < face.count; ++k)
            p.face[f][k] = h.faceVertices[face.first + k];
    }
    p.ne = u32(h.edges.Size() / 2);
    for (u32 e = 0; e < p.ne; ++e) {
        p.edge[e][0] = h.edges[e * 2];
        p.edge[e][1] = h.edges[e * 2 + 1];
        p.edgeDir[e] = h.edgeDir[e];
    }
    p.ndirs = u32(h.directions.Size());
    for (u32 d = 0; d < p.ndirs; ++d)
        p.dirs[d] = rot * h.directions[d];
}

/// Polyhedral shapes (box, convex hull) as a Poly.
bool MakeShapePoly(const PosedShape& s, Poly& p)
{
    if (s.desc->type == ShapeType::Box) {
        MakeBoxPoly(s.position, s.rotation, s.desc->halfExtents, p);
        return true;
    }
    if (s.desc->type == ShapeType::ConvexHull && s.hull) {
        MakeHullPoly(s.position, s.rotation, *s.hull, p);
        return true;
    }
    return false;
}

void MakeTrianglePoly(Vec3 a, Vec3 b, Vec3 c, Vec3 n, Poly& p)
{
    p.nv = 3;
    p.v[0] = a;
    p.v[1] = b;
    p.v[2] = c;
    p.nf = 2;
    p.fn[0] = n;
    p.fn[1] = -n;
    p.fcount[0] = p.fcount[1] = 3;
    p.face[0][0] = 0, p.face[0][1] = 1, p.face[0][2] = 2;
    p.face[1][0] = 0, p.face[1][1] = 2, p.face[1][2] = 1;
    p.FixWinding(0);
    p.FixWinding(1);
    p.ne = 3;
    p.ndirs = 3;
    for (u32 i = 0; i < 3; ++i) {
        p.edge[i][0] = u8(i);
        p.edge[i][1] = u8((i + 1) % 3);
        p.edgeDir[i] = u8(i);
        p.dirs[i] = SafeNormalize(p.v[(i + 1) % 3] - p.v[i], Vec3(1, 0, 0));
    }
    p.center = (a + b + c) / 3.0f;
}

/// Contacts between polyhedra A and B (normal A -> B). refIsA reports a face contact on A's face.
bool CollidePolys(const Poly& A, const Poly& B, f32 margin, Manifold& m, bool& faceOfA)
{
    m.count = 0;
    faceOfA = false;
    // Face axes of A.
    f32 sepA = -Infinity;
    u32 faceA = 0;
    for (u32 f = 0; f < A.nf; ++f) {
        const Vec3 n = A.fn[f];
        const f32 d = Dot(n, A.v[A.face[f][0]]);
        f32 minB = Infinity;
        for (u32 i = 0; i < B.nv; ++i)
            minB = Min(minB, Dot(n, B.v[i]));
        const f32 sep = minB - d;
        if (sep > margin)
            return false;
        if (sep > sepA) {
            sepA = sep;
            faceA = f;
        }
    }
    f32 sepB = -Infinity;
    u32 faceB = 0;
    for (u32 f = 0; f < B.nf; ++f) {
        const Vec3 n = B.fn[f];
        const f32 d = Dot(n, B.v[B.face[f][0]]);
        f32 minA = Infinity;
        for (u32 i = 0; i < A.nv; ++i)
            minA = Min(minA, Dot(n, A.v[i]));
        const f32 sep = minA - d;
        if (sep > margin)
            return false;
        if (sep > sepB) {
            sepB = sep;
            faceB = f;
        }
    }
    // Edge-edge axes.
    f32 sepE = -Infinity;
    u32 edgeI = 0, edgeJ = 0;
    Vec3 edgeAxis;
    const Vec3 between = B.center - A.center;
    for (u32 i = 0; i < A.ndirs; ++i)
        for (u32 j = 0; j < B.ndirs; ++j) {
            Vec3 L = Cross(A.dirs[i], B.dirs[j]);
            const f32 len = LengthSq(L);
            if (len < 1e-6f)
                continue;
            L = L * (1.0f / Sqrt(len));
            if (Dot(L, between) < 0.0f)
                L = -L;
            f32 maxA = -Infinity, minB = Infinity;
            for (u32 k = 0; k < A.nv; ++k)
                maxA = Max(maxA, Dot(L, A.v[k]));
            for (u32 k = 0; k < B.nv; ++k)
                minB = Min(minB, Dot(L, B.v[k]));
            const f32 sep = minB - maxA;
            if (sep > margin)
                return false;
            if (sep > sepE) {
                sepE = sep;
                edgeI = i;
                edgeJ = j;
                edgeAxis = L;
            }
        }
    const f32 bestFace = Max(sepA, sepB);
    if (sepE > bestFace + 0.005f) {
        // Edge contact: the two supporting edges, closest points between them.
        i32 ea = -1, eb = -1;
        f32 bestA = -Infinity, bestB = Infinity;
        for (u32 k = 0; k < A.ne; ++k)
            if (A.edgeDir[k] == edgeI) {
                const f32 d = Dot(edgeAxis, A.v[A.edge[k][0]] + A.v[A.edge[k][1]]);
                if (d > bestA) {
                    bestA = d;
                    ea = i32(k);
                }
            }
        for (u32 k = 0; k < B.ne; ++k)
            if (B.edgeDir[k] == edgeJ) {
                const f32 d = Dot(edgeAxis, B.v[B.edge[k][0]] + B.v[B.edge[k][1]]);
                if (d < bestB) {
                    bestB = d;
                    eb = i32(k);
                }
            }
        if (ea < 0 || eb < 0)
            return false;
        Vec3 cA, cB;
        ClosestSegmentSegment(A.v[A.edge[ea][0]], A.v[A.edge[ea][1]], B.v[B.edge[eb][0]], B.v[B.edge[eb][1]], cA, cB);
        m.normal = edgeAxis;
        AddPoint(m, cA, cB, Dot(cB - cA, edgeAxis));
        return true;
    }
    // Face contact: reference face, incident face of the other polyhedron clipped to the reference's side planes.
    const bool refA = sepA >= sepB - 0.002f;
    const Poly& R = refA ? A : B;
    const Poly& I = refA ? B : A;
    const u32 fr = refA ? faceA : faceB;
    faceOfA = refA;
    const Vec3 nR = R.fn[fr];
    u32 fi = 0;
    f32 minDot = Infinity;
    for (u32 f = 0; f < I.nf; ++f) {
        const f32 d = Dot(I.fn[f], nR);
        if (d < minDot) {
            minDot = d;
            fi = f;
        }
    }
    Vec3 bufA[64], bufB[64];
    u32 count = I.fcount[fi];
    for (u32 k = 0; k < count; ++k)
        bufA[k] = I.v[I.face[fi][k]];
    Vec3* in = bufA;
    Vec3* out = bufB;
    for (u32 k = 0; k < R.fcount[fr] && count > 0; ++k) {
        const Vec3 v0 = R.v[R.face[fr][k]], v1 = R.v[R.face[fr][(k + 1) % R.fcount[fr]]];
        const Vec3 side = Normalize(Cross(v1 - v0, nR));
        // Tolerance: stacked boxes have coincident edges; without it rounding adds near-duplicate points.
        constexpr f32 Tol = 1e-4f;
        u32 n = 0;
        for (u32 i = 0; i < count; ++i) {
            const Vec3 p = in[i], q = in[(i + 1) % count];
            const f32 dp = Dot(side, p - v0), dq = Dot(side, q - v0);
            if (dp <= Tol && n < 64)
                out[n++] = p;
            if (((dp < -Tol && dq > Tol) || (dp > Tol && dq < -Tol)) && n < 64)
                out[n++] = p + (q - p) * (dp / (dp - dq));
        }
        swap(in, out);
        count = n;
    }
    const f32 refD = Dot(nR, R.v[R.face[fr][0]]);
    Array<ContactPoint> pts;
    for (u32 i = 0; i < count; ++i) {
        const f32 sep = Dot(nR, in[i]) - refD;
        if (sep > margin)
            continue;
        bool duplicate = false;
        for (const ContactPoint& cp : pts)
            duplicate |= DistanceSq(refA ? cp.pointB : cp.pointA, in[i]) < 1e-6f;
        if (duplicate)
            continue;
        const Vec3 onRef = in[i] - nR * sep;
        pts.PushBack(refA ? ContactPoint{onRef, in[i], sep} : ContactPoint{in[i], onRef, sep});
    }
    m.normal = refA ? nR : -nR;
    ReduceTo4(m.normal, pts, m);
    return m.count > 0;
}

// --- Spheres and capsules (a point or segment with a radius) -------------------------------------------------------

struct Rounded
{
    Vec3 p0, p1; // core segment (p0 == p1 for spheres)
    f32 radius;
};

Rounded MakeRounded(const PosedShape& s)
{
    if (s.desc->type == ShapeType::Sphere)
        return {s.position, s.position, s.desc->radius};
    const Vec3 axis = s.rotation * Vec3(0.0f, s.desc->halfHeight, 0.0f);
    return {s.position - axis, s.position + axis, s.desc->radius};
}

void RoundedRounded(const Rounded& a, const Rounded& b, f32 margin, Manifold& m)
{
    m.count = 0;
    const Vec3 da = a.p1 - a.p0, db = b.p1 - b.p0;
    const f32 la = LengthSq(da), lb = LengthSq(db);
    const f32 radii = a.radius + b.radius;
    // Parallel capsules lying on each other: two points over the overlap.
    if (la > 1e-6f && lb > 1e-6f) {
        const Vec3 ua = da * (1.0f / Sqrt(la)), ub = db * (1.0f / Sqrt(lb));
        if (Abs(Dot(ua, ub)) > 0.995f) {
            const f32 t0 = Clamp(Dot(b.p0 - a.p0, ua), 0.0f, Sqrt(la)), t1 = Clamp(Dot(b.p1 - a.p0, ua), 0.0f, Sqrt(la));
            if (Abs(t1 - t0) > 1e-3f) {
                Vec3 n;
                bool any = false;
                for (f32 t : {t0, t1}) {
                    const Vec3 pa = a.p0 + ua * t;
                    const Vec3 pb = ClosestPointSegment(pa, b.p0, b.p1);
                    const Vec3 d = pb - pa;
                    const f32 dist = Length(d);
                    if (dist < 1e-5f)
                        break;
                    n = d / dist;
                    any = true;
                    const f32 sep = dist - radii;
                    if (sep <= margin)
                        AddPoint(m, pa + n * a.radius, pb - n * b.radius, sep);
                }
                if (any && m.count > 0) {
                    m.normal = n;
                    return;
                }
                m.count = 0;
            }
        }
    }
    Vec3 ca, cb;
    ClosestSegmentSegment(a.p0, a.p1, b.p0, b.p1, ca, cb);
    Vec3 d = cb - ca;
    const f32 dist = Length(d);
    const Vec3 n = dist > 1e-5f ? d / dist : Vec3(0, 1, 0);
    const f32 sep = dist - radii;
    if (sep > margin)
        return;
    m.normal = n;
    AddPoint(m, ca + n * a.radius, cb - n * b.radius, sep);
}

/// Convex polyhedron (A, e.g. a hull) vs rounded shape (B); normal from A to B. Points of the core outside the hull:
/// GJK closest points; inside: the least-penetrated face. A capsule contributes its two ends (and the closest point of
/// its segment when that is nearer), so it rests flat on faces.
void PolyRounded(const Poly& H, const Rounded& r, f32 margin, Manifold& m)
{
    m.count = 0;
    struct Candidate
    {
        Vec3 onHull, core, normal;
        f32 separation;
    };
    Candidate cands[3];
    u32 nc = 0;
    auto faceDistance = [&](Vec3 p, u32& face) {
        f32 best = -Infinity;
        face = 0;
        for (u32 f = 0; f < H.nf; ++f) {
            const f32 s = Dot(H.fn[f], p) - Dot(H.fn[f], H.v[H.face[f][0]]);
            if (s > best) {
                best = s;
                face = f;
            }
        }
        return best;
    };
    auto addPoint = [&](Vec3 p) {
        u32 face;
        const f32 s = faceDistance(p, face);
        Candidate c;
        c.core = p;
        if (s <= 0.0f) { // inside the hull: out through the nearest face
            c.normal = H.fn[face];
            c.onHull = p - c.normal * s;
            c.separation = s - r.radius;
        } else {
            const GjkResult g = Gjk(Span<const Vec3>(H.v, H.nv), Span<const Vec3>(&p, 1));
            c.normal = g.distance > 1e-6f ? (p - g.pointA) / g.distance : H.fn[face];
            c.onHull = g.distance > 1e-6f ? g.pointA : p - H.fn[face] * s;
            c.separation = Max(g.distance, 0.0f) - r.radius;
        }
        cands[nc++] = c;
    };
    addPoint(r.p0);
    const bool segment = DistanceSq(r.p0, r.p1) > 1e-10f;
    if (segment) {
        addPoint(r.p1);
        const Vec3 core[2] = {r.p0, r.p1};
        const GjkResult g = Gjk(Span<const Vec3>(H.v, H.nv), Span<const Vec3>(core, 2));
        if (g.distance > 1e-5f) {
            Candidate c;
            c.onHull = g.pointA;
            c.core = g.pointB;
            c.normal = (g.pointB - g.pointA) / g.distance;
            c.separation = g.distance - r.radius;
            cands[nc++] = c;
        } else if (cands[0].separation > -r.radius && cands[1].separation > -r.radius) {
            // The segment passes through the hull with both ends outside: least-penetrated face axis.
            f32 best = -Infinity;
            u32 bestFace = 0;
            for (u32 f = 0; f < H.nf; ++f) {
                const f32 d = Dot(H.fn[f], H.v[H.face[f][0]]);
                const f32 s = Min(Dot(H.fn[f], r.p0), Dot(H.fn[f], r.p1)) - d;
                if (s > best) {
                    best = s;
                    bestFace = f;
                }
            }
            Candidate c;
            c.normal = H.fn[bestFace];
            c.core = Dot(c.normal, r.p0) < Dot(c.normal, r.p1) ? r.p0 : r.p1;
            c.onHull = c.core - c.normal * best;
            c.separation = best - r.radius;
            cands[nc++] = c;
        }
    }
    u32 deepest = 0;
    for (u32 i = 1; i < nc; ++i)
        if (cands[i].separation < cands[deepest].separation)
            deepest = i;
    if (cands[deepest].separation > margin)
        return;
    m.normal = cands[deepest].normal;
    for (u32 i = 0; i < nc; ++i) {
        const Candidate& c = cands[i];
        if (c.separation > margin || (i != deepest && Dot(c.normal, m.normal) < 0.95f))
            continue;
        bool duplicate = false;
        for (u32 k = 0; k < m.count; ++k)
            duplicate |= DistanceSq(m.points[k].pointA, c.onHull) < 1e-8f;
        if (!duplicate)
            AddPoint(m, c.onHull, c.core - m.normal * r.radius, c.separation);
    }
}

/// Box (A) vs rounded shape (B); normal from the box to B.
void BoxRounded(const PosedShape& box, const Rounded& r, f32 margin, Manifold& m)
{
    m.count = 0;
    const Vec3 e = box.desc->halfExtents;
    const Vec3 l0 = InvRotate(box.rotation, r.p0 - box.position), l1 = InvRotate(box.rotation, r.p1 - box.position);
    auto clampBox = [&](Vec3 p) { return Vec3(Clamp(p.x, -e.x, e.x), Clamp(p.y, -e.y, e.y), Clamp(p.z, -e.z, e.z)); };
    auto distSq = [&](f32 t) {
        const Vec3 p = Lerp(l0, l1, t);
        return DistanceSq(p, clampBox(p));
    };
    // The distance along the segment is convex: golden-section search for its minimum.
    f32 lo = 0.0f, hi = 1.0f;
    if (DistanceSq(l0, l1) > 1e-10f) {
        constexpr f32 G = 0.6180339887f;
        f32 x1 = hi - G * (hi - lo), x2 = lo + G * (hi - lo);
        f32 f1 = distSq(x1), f2 = distSq(x2);
        for (u32 it = 0; it < 28; ++it) {
            if (f1 < f2) {
                hi = x2;
                x2 = x1;
                f2 = f1;
                x1 = hi - G * (hi - lo);
                f1 = distSq(x1);
            } else {
                lo = x1;
                x1 = x2;
                f1 = f2;
                x2 = lo + G * (hi - lo);
                f2 = distSq(x2);
            }
        }
    }
    f32 t = (lo + hi) * 0.5f;
    for (f32 end : {0.0f, 1.0f}) // the minimum may sit at an end
        if (distSq(end) < distSq(t))
            t = end;
    const Vec3 p = Lerp(l0, l1, t), q = clampBox(p);
    const f32 dist = Distance(p, q);
    const Vec3 segDir = l1 - l0;
    // Face contacts: the part of the segment above face `k` (sign s), clipped to the face rectangle.
    auto faceContacts = [&](u32 k, f32 s) {
        const u32 u = (k + 1) % 3, v = (k + 2) % 3;
        f32 t0 = 0.0f, t1 = 1.0f;
        for (u32 axis : {u, v}) {
            const f32 d = segDir[axis], o = l0[axis];
            if (Abs(d) < 1e-7f) {
                if (o < -e[axis] || o > e[axis])
                    return false;
                continue;
            }
            f32 ta = (-e[axis] - o) / d, tb = (e[axis] - o) / d;
            if (ta > tb)
                swap(ta, tb);
            t0 = Max(t0, ta);
            t1 = Min(t1, tb);
        }
        if (t0 > t1)
            return false;
        Vec3 nl;
        nl[k] = s;
        const Vec3 nw = box.rotation * nl;
        const u32 samples = DistanceSq(l0, l1) > 1e-10f && t1 - t0 > 1e-4f ? 2 : 1;
        for (u32 i = 0; i < samples; ++i) {
            const f32 ti = samples == 1 ? (t0 + t1) * 0.5f : (i == 0 ? t0 : t1);
            const Vec3 pl = Lerp(l0, l1, ti);
            const f32 sep = (s * pl[k] - e[k]) - r.radius;
            if (sep > margin)
                continue;
            Vec3 onBox = pl;
            onBox[k] = s * e[k];
            onBox = Vec3(Clamp(onBox.x, -e.x, e.x), Clamp(onBox.y, -e.y, e.y), Clamp(onBox.z, -e.z, e.z));
            AddPoint(m, box.position + box.rotation * onBox, box.position + box.rotation * pl - nw * r.radius, sep);
        }
        m.normal = nw;
        return m.count > 0;
    };
    if (dist > 1e-5f) {
        if (dist - r.radius > margin)
            return;
        const Vec3 nl = (p - q) / dist;
        // Lying flat on a face: two points along the segment.
        for (u32 k = 0; k < 3; ++k)
            if (Abs(nl[k]) > 0.999f && LengthSq(segDir) > 1e-8f && Abs(Dot(Normalize(segDir), nl)) < 0.05f) {
                if (faceContacts(k, nl[k] > 0.0f ? 1.0f : -1.0f))
                    return;
                m.count = 0;
            }
        const Vec3 nw = box.rotation * nl;
        m.normal = nw;
        AddPoint(m, box.position + box.rotation * q, box.position + box.rotation * p - nw * r.radius, dist - r.radius);
        return;
    }
    // The core touches the box: SAT over the box axes and segment x axis.
    f32 bestSep = -Infinity;
    Vec3 bestAxis;
    i32 bestFace = -1;
    auto test = [&](Vec3 L, i32 face) {
        const f32 len = Length(L);
        if (len < 1e-5f)
            return;
        L = L / len;
        const f32 boxR = e.x * Abs(L.x) + e.y * Abs(L.y) + e.z * Abs(L.z);
        const f32 a = Dot(L, l0), b = Dot(L, l1);
        const f32 sepPos = Min(a, b) - boxR, sepNeg = -Max(a, b) - boxR;
        const f32 sep = Max(sepPos, sepNeg);
        if (sep > bestSep) {
            bestSep = sep;
            bestAxis = sepPos >= sepNeg ? L : -L;
            bestFace = face;
        }
    };
    for (u32 k = 0; k < 3; ++k) {
        Vec3 L;
        L[k] = 1.0f;
        test(L, i32(k));
    }
    if (LengthSq(segDir) > 1e-8f)
        for (u32 k = 0; k < 3; ++k) {
            Vec3 L;
            L[k] = 1.0f;
            test(Cross(segDir, L), -1);
        }
    if (bestFace >= 0 && faceContacts(u32(bestFace), bestAxis[u32(bestFace)] > 0.0f ? 1.0f : -1.0f))
        return;
    m.count = 0;
    const Vec3 nw = box.rotation * bestAxis;
    const Vec3 deep = Dot(bestAxis, l0) < Dot(bestAxis, l1) ? l0 : l1;
    const f32 sep = bestSep - r.radius;
    m.normal = nw;
    AddPoint(m, box.position + box.rotation * (deep - bestAxis * (bestSep)), box.position + box.rotation * deep - nw * r.radius, sep);
}

/// Triangle (A, normal n = counter-clockwise front) vs rounded shape (B). One-sided: B behind the triangle is ignored.
/// faceContact reports contacts on the triangle's face (not its edges).
void TriangleRounded(Vec3 a, Vec3 b, Vec3 c, Vec3 n, const Rounded& r, f32 margin, Manifold& m, bool& faceContact)
{
    m.count = 0;
    faceContact = false;
    const f32 d0 = Dot(r.p0 - a, n), d1 = Dot(r.p1 - a, n);
    if (Max(d0, d1) < -r.radius || Min(d0, d1) > r.radius + margin)
        return;
    // The part of the core inside the triangle's prism.
    f32 t0 = 0.0f, t1 = 1.0f;
    const Vec3 dir = r.p1 - r.p0;
    const Vec3 verts[3] = {a, b, c};
    bool inside = true;
    for (u32 i = 0; i < 3 && inside; ++i) {
        const Vec3 v0 = verts[i], v1 = verts[(i + 1) % 3];
        const Vec3 side = Cross(v1 - v0, n);
        const f32 sd0 = Dot(side, r.p0 - v0), sdd = Dot(side, dir);
        if (Abs(sdd) < 1e-9f) {
            if (sd0 > 0.0f)
                inside = false;
            continue;
        }
        const f32 tHit = -sd0 / sdd;
        if (sdd > 0.0f)
            t1 = Min(t1, tHit);
        else
            t0 = Max(t0, tHit);
        if (t0 > t1)
            inside = false;
    }
    if (inside) {
        const u32 samples = LengthSq(dir) > 1e-10f && t1 - t0 > 1e-4f ? 2 : 1;
        for (u32 i = 0; i < samples; ++i) {
            const f32 t = samples == 1 ? (t0 + t1) * 0.5f : (i == 0 ? t0 : t1);
            const Vec3 p = r.p0 + dir * t;
            const f32 dist = Dot(p - a, n);
            if (dist < -r.radius)
                continue;
            const f32 sep = dist - r.radius;
            if (sep > margin)
                continue;
            AddPoint(m, p - n * dist, p - n * r.radius, sep);
        }
        if (m.count > 0) {
            m.normal = n;
            faceContact = true;
            return;
        }
    }
    // Edges (and so vertices).
    f32 best = Infinity;
    Vec3 bestCore, bestTri;
    for (u32 i = 0; i < 3; ++i) {
        Vec3 ce, cs;
        ClosestSegmentSegment(verts[i], verts[(i + 1) % 3], r.p0, r.p1, ce, cs);
        const f32 d = DistanceSq(ce, cs);
        if (d < best) {
            best = d;
            bestCore = cs;
            bestTri = ce;
        }
    }
    const f32 dist = Sqrt(best);
    if (dist - r.radius > margin || dist < 1e-6f || Dot(bestCore - a, n) < -0.01f)
        return;
    const Vec3 nn = (bestCore - bestTri) / dist;
    m.normal = nn;
    AddPoint(m, bestTri, bestCore - nn * r.radius, dist - r.radius);
}

/// Height fields are solid below: a shape whose center sank under a triangle is pushed back up along its normal (from
/// the corners / core points that lie over the triangle), never out through the bottom. Triangle = A, shape = B.
void BelowSurface(const Poly* poly, const Rounded& r, Vec3 a, Vec3 b, Vec3 c, Vec3 n, f32 margin, Manifold& m)
{
    m = Manifold{};
    m.normal = n;
    auto over = [&](Vec3 p) {
        constexpr f32 Tol = -1e-6f;
        return Dot(Cross(b - a, p - a), n) >= Tol && Dot(Cross(c - b, p - b), n) >= Tol && Dot(Cross(a - c, p - c), n) >= Tol;
    };
    Array<ContactPoint> pts;
    auto add = [&](Vec3 p, f32 radius) {
        const f32 h = Dot(p - a, n);
        if (h - radius < margin && over(p))
            pts.PushBack(ContactPoint{p - n * h, p - n * radius, h - radius});
    };
    if (poly) {
        for (u32 i = 0; i < poly->nv; ++i)
            add(poly->v[i], 0.0f);
    } else {
        add(r.p0, r.radius);
        if (DistanceSq(r.p0, r.p1) > 1e-12f)
            add(r.p1, r.radius);
    }
    ReduceTo4(n, pts, m);
}

/// Shape (A) against the triangles of a mesh or height field (B): one manifold per touching triangle; normals from A
/// to the mesh.
template <typename Triangles>
void ShapeMesh(const PosedShape& s, const PosedShape& meshShape, const Triangles& mesh, f32 margin, Array<Manifold>& out)
{
    const AABB worldBox = ShapeBounds(s).Inflated(margin);
    // Query in mesh space: the world box's corners transformed back.
    AABB local;
    for (u32 i = 0; i < 8; ++i) {
        const Vec3 corner((i & 1) ? worldBox.max.x : worldBox.min.x, (i & 2) ? worldBox.max.y : worldBox.min.y,
                          (i & 4) ? worldBox.max.z : worldBox.min.z);
        local.Expand(InvRotate(meshShape.rotation, corner - meshShape.position));
    }
    const usize first = out.Size();
    Array<bool> faceFlags;
    Poly shapePoly;
    const bool isBox = MakeShapePoly(s, shapePoly); // box or convex hull
    const Rounded rounded = isBox ? Rounded{} : MakeRounded(s);
    mesh.Query(local, [&](u32 tri) {
        Vec3 a, b, c;
        mesh.Triangle(tri, a, b, c);
        a = meshShape.position + meshShape.rotation * a;
        b = meshShape.position + meshShape.rotation * b;
        c = meshShape.position + meshShape.rotation * c;
        const Vec3 cr = Cross(b - a, c - a);
        const f32 len = Length(cr);
        if (len < 1e-10f)
            return;
        const Vec3 n = cr / len;
        Manifold m;
        bool face = false;
        if (mesh.SolidBelow() && Dot(s.position - a, n) < 0.0f) {
            BelowSurface(isBox ? &shapePoly : nullptr, rounded, a, b, c, n, margin, m);
            if (m.count == 0)
                return;
            face = true;
        } else if (isBox) {
            Poly tp;
            MakeTrianglePoly(a, b, c, n, tp);
            bool faceOfTri = false;
            if (!CollidePolys(tp, shapePoly, margin, m, faceOfTri) || Dot(m.normal, n) < -0.05f)
                return;
            face = faceOfTri && Dot(m.normal, n) > 0.0f;
        } else {
            TriangleRounded(a, b, c, n, rounded, margin, m, face);
            if (m.count == 0)
                return;
        }
        FlipManifold(m); // shape -> mesh
        m.subKey = tri + 1;
        out.PushBack(m);
        faceFlags.PushBack(face);
    });
    // Edge contacts next to face contacts are internal-edge "ghosts" on smooth surfaces: keep the faces.
    bool anyFace = false;
    for (bool f : faceFlags)
        anyFace |= f;
    if (!anyFace)
        return;
    usize w = first;
    for (usize i = first; i < out.Size(); ++i)
        if (faceFlags[i - first])
            out[w++] = out[i];
    out.Resize(w);
}

} // namespace

// --- Triangle mesh ---------------------------------------------------------------------------------------------------

void TriangleMesh::Build()
{
    const u32 n = TriangleCount();
    order.Resize(n);
    Array<Vec3> centers;
    centers.Resize(n);
    Array<AABB> boxes;
    boxes.Resize(n);
    for (u32 t = 0; t < n; ++t) {
        order[t] = t;
        Vec3 a, b, c;
        Triangle(t, a, b, c);
        boxes[t] = AABB(Min(a, Min(b, c)), Max(a, Max(b, c)));
        centers[t] = (a + b + c) / 3.0f;
    }
    nodes.Clear();
    if (n == 0)
        return;
    nodes.Reserve(n * 2);
    struct Item
    {
        u32 node, begin, end;
    };
    Array<Item> stack;
    nodes.PushBack(Node());
    stack.PushBack(Item{0, 0, n});
    while (!stack.Empty()) {
        const Item it = stack.PopBackValue();
        AABB box, centerBox;
        for (u32 i = it.begin; i < it.end; ++i) {
            box.Expand(boxes[order[i]]);
            centerBox.Expand(centers[order[i]]);
        }
        nodes[it.node].box = box;
        if (it.end - it.begin <= 4) {
            nodes[it.node].first = it.begin;
            nodes[it.node].count = it.end - it.begin;
            continue;
        }
        const Vec3 size = centerBox.Size();
        const u32 axis = size.x > size.y ? (size.x > size.z ? 0 : 2) : (size.y > size.z ? 1 : 2);
        const f32 split = centerBox.Center()[axis];
        u32 mid = it.begin;
        for (u32 i = it.begin; i < it.end; ++i)
            if (centers[order[i]][axis] < split)
                swap(order[i], order[mid++]);
        if (mid == it.begin || mid == it.end)
            mid = (it.begin + it.end) / 2;
        const u32 left = u32(nodes.Size());
        nodes.PushBack(Node());
        nodes.PushBack(Node());
        nodes[it.node].first = left;
        nodes[it.node].count = 0;
        stack.PushBack(Item{left, it.begin, mid});
        stack.PushBack(Item{left + 1, mid, it.end});
    }
}

namespace {
bool RayTriangle(Vec3 o, Vec3 d, Vec3 a, Vec3 b, Vec3 c, f32 maxT, f32& t)
{
    const Vec3 e1 = b - a, e2 = c - a, p = Cross(d, e2);
    const f32 det = Dot(e1, p);
    if (Abs(det) < 1e-12f)
        return false;
    const f32 inv = 1.0f / det;
    const Vec3 s = o - a;
    // A little tolerance: rays through a shared edge or vertex must not slip between the triangles.
    constexpr f32 Tol = 1e-5f;
    const f32 u = Dot(s, p) * inv;
    if (u < -Tol || u > 1.0f + Tol)
        return false;
    const Vec3 q = Cross(s, e1);
    const f32 v = Dot(d, q) * inv;
    if (v < -Tol || u + v > 1.0f + Tol)
        return false;
    t = Dot(e2, q) * inv;
    return t >= 0.0f && t <= maxT;
}
} // namespace

bool TriangleMesh::Raycast(Vec3 origin, Vec3 dir, f32 maxT, f32& tOut, Vec3& normal) const
{
    if (nodes.Empty())
        return false;
    const Vec3 inv(dir.x != 0.0f ? 1.0f / dir.x : 1e30f, dir.y != 0.0f ? 1.0f / dir.y : 1e30f, dir.z != 0.0f ? 1.0f / dir.z : 1e30f);
    bool hit = false;
    u32 stack[64];
    u32 top = 0;
    stack[top++] = 0;
    while (top > 0) {
        const Node& n = nodes[stack[--top]];
        f32 t0 = 0.0f, t1 = maxT;
        bool overlap = true;
        for (u32 ax = 0; ax < 3 && overlap; ++ax) {
            f32 a = (n.box.min[ax] - origin[ax]) * inv[ax], b = (n.box.max[ax] - origin[ax]) * inv[ax];
            if (a > b)
                swap(a, b);
            t0 = Max(t0, a);
            t1 = Min(t1, b);
            overlap = t0 <= t1;
        }
        if (!overlap)
            continue;
        if (n.count) {
            for (u32 i = 0; i < n.count; ++i) {
                Vec3 a, b, c;
                Triangle(order[n.first + i], a, b, c);
                f32 t;
                if (RayTriangle(origin, dir, a, b, c, maxT, t)) {
                    maxT = t;
                    tOut = t;
                    normal = Normalize(Cross(b - a, c - a));
                    if (Dot(normal, dir) > 0.0f)
                        normal = -normal;
                    hit = true;
                }
            }
        } else if (top + 2 <= 64) {
            stack[top++] = n.first;
            stack[top++] = n.first + 1;
        }
    }
    return hit;
}

// --- Height field ----------------------------------------------------------------------------------------------------

void HeightField::Build()
{
    origin = Vec3(-f32(columns - 1) * spacingX * 0.5f, 0.0f, -f32(rows - 1) * spacingZ * 0.5f);
    f32 lo = Infinity, hi = -Infinity;
    for (f32 h : heights) {
        lo = Min(lo, h);
        hi = Max(hi, h);
    }
    if (heights.Empty())
        lo = hi = 0.0f;
    bounds = AABB(Vec3(origin.x, lo, origin.z), Vec3(-origin.x, hi, -origin.z));
}

bool HeightField::Raycast(Vec3 o, Vec3 dir, f32 maxT, f32& tOut, Vec3& normal) const
{
    if (columns < 2 || rows < 2)
        return false;
    // Clip to the bounds, then walk the cells under the ray (2D DDA) nearest first.
    f32 t0 = 0.0f, t1 = maxT;
    for (u32 ax = 0; ax < 3; ++ax) {
        if (Abs(dir[ax]) < 1e-12f) {
            if (o[ax] < bounds.min[ax] - 1e-4f || o[ax] > bounds.max[ax] + 1e-4f)
                return false;
            continue;
        }
        f32 a = (bounds.min[ax] - 1e-4f - o[ax]) / dir[ax], b = (bounds.max[ax] + 1e-4f - o[ax]) / dir[ax];
        if (a > b)
            swap(a, b);
        t0 = Max(t0, a);
        t1 = Min(t1, b);
        if (t0 > t1)
            return false;
    }
    const Vec3 p = o + dir * t0;
    i32 x = Clamp(i32(Floor((p.x - origin.x) / spacingX)), 0, i32(columns) - 2);
    i32 z = Clamp(i32(Floor((p.z - origin.z) / spacingZ)), 0, i32(rows) - 2);
    const i32 stepX = dir.x > 0.0f ? 1 : -1, stepZ = dir.z > 0.0f ? 1 : -1;
    const f32 dx = Abs(dir.x) > 1e-12f ? spacingX / Abs(dir.x) : Infinity, dz = Abs(dir.z) > 1e-12f ? spacingZ / Abs(dir.z) : Infinity;
    f32 nextX = Abs(dir.x) > 1e-12f ? (origin.x + f32(x + (stepX > 0 ? 1 : 0)) * spacingX - o.x) / dir.x : Infinity;
    f32 nextZ = Abs(dir.z) > 1e-12f ? (origin.z + f32(z + (stepZ > 0 ? 1 : 0)) * spacingZ - o.z) / dir.z : Infinity;
    for (u32 guard = 0; guard < columns + rows + 4; ++guard) {
        const u32 cell = u32(z) * CellsX() + u32(x);
        bool hit = false;
        for (u32 k = 0; k < 2; ++k) {
            Vec3 a, b, c;
            Triangle(cell * 2 + k, a, b, c);
            f32 t;
            if (RayTriangle(o, dir, a, b, c, maxT, t)) {
                maxT = t;
                tOut = t;
                normal = Normalize(Cross(b - a, c - a));
                if (Dot(normal, dir) > 0.0f)
                    normal = -normal;
                hit = true;
            }
        }
        if (hit)
            return true; // later cells are farther along the ray
        if (Min(nextX, nextZ) > t1)
            return false;
        if (nextX < nextZ) {
            x += stepX;
            nextX += dx;
        } else {
            z += stepZ;
            nextZ += dz;
        }
        if (x < 0 || z < 0 || x > i32(columns) - 2 || z > i32(rows) - 2)
            return false;
    }
    return false;
}

// --- Dispatch ----------------------------------------------------------------------------------------------------------

void Collide(const PosedShape& a, const PosedShape& b, f32 margin, Array<Manifold>& out)
{
    const ShapeType ta = a.desc->type, tb = b.desc->type;
    if (u32(ta) > u32(tb)) {
        const usize first = out.Size();
        Collide(b, a, margin, out);
        for (usize i = first; i < out.Size(); ++i)
            FlipManifold(out[i]);
        return;
    }
    Manifold m;
    switch (tb) {
    case ShapeType::TriangleMesh:
        if (ta != ShapeType::TriangleMesh && b.mesh)
            ShapeMesh(a, b, *b.mesh, margin, out);
        return;
    case ShapeType::HeightField:
        if (ta != ShapeType::TriangleMesh && ta != ShapeType::HeightField && b.heightField)
            ShapeMesh(a, b, *b.heightField, margin, out);
        return;
    case ShapeType::ConvexHull: {
        if (!b.hull)
            return;
        Poly hb;
        MakeHullPoly(b.position, b.rotation, *b.hull, hb);
        if (ta == ShapeType::Box || ta == ShapeType::ConvexHull) {
            Poly pa;
            if (!MakeShapePoly(a, pa))
                return;
            bool faceOfA = false;
            if (!CollidePolys(pa, hb, margin, m, faceOfA))
                return;
        } else { // sphere / capsule vs hull
            PolyRounded(hb, MakeRounded(a), margin, m);
            FlipManifold(m);
        }
        break;
    }
    case ShapeType::Box:
        if (ta == ShapeType::Box) {
            Poly pa, pb;
            MakeBoxPoly(a.position, a.rotation, a.desc->halfExtents, pa);
            MakeBoxPoly(b.position, b.rotation, b.desc->halfExtents, pb);
            bool faceOfA = false;
            if (!CollidePolys(pa, pb, margin, m, faceOfA))
                return;
        } else { // sphere vs box
            BoxRounded(b, MakeRounded(a), margin, m);
            FlipManifold(m);
        }
        break;
    case ShapeType::Capsule:
        if (ta == ShapeType::Box)
            BoxRounded(a, MakeRounded(b), margin, m);
        else
            RoundedRounded(MakeRounded(a), MakeRounded(b), margin, m);
        break;
    case ShapeType::Sphere: RoundedRounded(MakeRounded(a), MakeRounded(b), margin, m); break;
    }
    if (m.count > 0)
        out.PushBack(m);
}

AABB ShapeBounds(const PosedShape& s)
{
    switch (s.desc->type) {
    case ShapeType::Sphere: return AABB::FromCenterExtents(s.position, Vec3(s.desc->radius));
    case ShapeType::Box: {
        const Mat3 r = Mat3::FromQuat(s.rotation);
        const Vec3 e = s.desc->halfExtents;
        const Vec3 ext = Abs(r.columns[0]) * e.x + Abs(r.columns[1]) * e.y + Abs(r.columns[2]) * e.z;
        return AABB::FromCenterExtents(s.position, ext);
    }
    case ShapeType::Capsule: {
        const Vec3 axis = s.rotation * Vec3(0.0f, s.desc->halfHeight, 0.0f);
        return AABB(Min(s.position - axis, s.position + axis) - Vec3(s.desc->radius), Max(s.position - axis, s.position + axis) + Vec3(s.desc->radius));
    }
    case ShapeType::ConvexHull: {
        AABB box;
        if (!s.hull)
            return AABB(s.position, s.position);
        for (Vec3 v : s.hull->vertices)
            box.Expand(s.position + s.rotation * v);
        return box;
    }
    case ShapeType::TriangleMesh:
    case ShapeType::HeightField: {
        AABB box;
        const bool mesh = s.desc->type == ShapeType::TriangleMesh;
        if (mesh ? (!s.mesh || s.mesh->nodes.Empty()) : !s.heightField)
            return AABB(s.position, s.position);
        const AABB& lb = mesh ? s.mesh->nodes[0].box : s.heightField->bounds;
        for (u32 i = 0; i < 8; ++i)
            box.Expand(s.position + s.rotation * Vec3((i & 1) ? lb.max.x : lb.min.x, (i & 2) ? lb.max.y : lb.min.y, (i & 4) ? lb.max.z : lb.min.z));
        return box;
    }
    }
    return AABB(s.position, s.position);
}

namespace {
bool RaySphere(Vec3 o, Vec3 d, Vec3 c, f32 r, f32 maxT, f32& t)
{
    const Vec3 m = o - c;
    const f32 b = Dot(m, d), cc = Dot(m, m) - r * r;
    if (cc > 0.0f && b > 0.0f)
        return false;
    const f32 disc = b * b - cc;
    if (disc < 0.0f)
        return false;
    t = Max(-b - Sqrt(disc), 0.0f);
    return t <= maxT;
}
} // namespace

bool RaycastShape(const PosedShape& s, Vec3 origin, Vec3 dir, f32 maxT, f32& t, Vec3& normal)
{
    switch (s.desc->type) {
    case ShapeType::ConvexHull: {
        // Clip the ray against every face plane (Cyrus-Beck).
        if (!s.hull)
            return false;
        const Vec3 o = InvRotate(s.rotation, origin - s.position), d = InvRotate(s.rotation, dir);
        f32 t0 = 0.0f, t1 = maxT;
        i32 enter = -1;
        for (u32 f = 0; f < s.hull->faces.Size(); ++f) {
            const ConvexHull::Face& face = s.hull->faces[f];
            const f32 denom = Dot(face.normal, d), dist = face.d - Dot(face.normal, o);
            if (Abs(denom) < 1e-12f) {
                if (dist < 0.0f)
                    return false;
                continue;
            }
            const f32 th = dist / denom;
            if (denom < 0.0f) {
                if (th > t0) {
                    t0 = th;
                    enter = i32(f);
                }
            } else {
                t1 = Min(t1, th);
            }
            if (t0 > t1)
                return false;
        }
        t = t0;
        normal = enter >= 0 ? s.rotation * s.hull->faces[u32(enter)].normal : -dir;
        return true;
    }
    case ShapeType::Sphere:
        if (!RaySphere(origin, dir, s.position, s.desc->radius, maxT, t))
            return false;
        normal = SafeNormalize(origin + dir * t - s.position, -dir);
        return true;
    case ShapeType::Box: {
        const Vec3 o = InvRotate(s.rotation, origin - s.position), d = InvRotate(s.rotation, dir);
        const Vec3 e = s.desc->halfExtents;
        f32 t0 = 0.0f, t1 = maxT;
        i32 axis = -1;
        f32 sign = 0.0f;
        for (u32 k = 0; k < 3; ++k) {
            if (Abs(d[k]) < 1e-12f) {
                if (o[k] < -e[k] || o[k] > e[k])
                    return false;
                continue;
            }
            f32 a = (-e[k] - o[k]) / d[k], b = (e[k] - o[k]) / d[k];
            f32 sa = -1.0f;
            if (a > b) {
                swap(a, b);
                sa = 1.0f;
            }
            if (a > t0) {
                t0 = a;
                axis = i32(k);
                sign = sa;
            }
            t1 = Min(t1, b);
            if (t0 > t1)
                return false;
        }
        t = t0;
        Vec3 nl;
        if (axis >= 0)
            nl[u32(axis)] = sign;
        else
            nl = -d; // started inside
        normal = s.rotation * nl;
        return true;
    }
    case ShapeType::Capsule: {
        const Vec3 axis = s.rotation * Vec3(0.0f, s.desc->halfHeight, 0.0f);
        const Vec3 p0 = s.position - axis, p1 = s.position + axis;
        const f32 r = s.desc->radius;
        bool hit = false;
        f32 best = maxT;
        for (Vec3 c : {p0, p1}) {
            f32 tt;
            if (RaySphere(origin, dir, c, r, best, tt)) {
                best = tt;
                hit = true;
            }
        }
        // Cylinder body.
        const Vec3 ax = p1 - p0;
        const f32 axLen2 = Dot(ax, ax);
        if (axLen2 > 1e-10f) {
            const Vec3 m = origin - p0;
            const f32 md = Dot(m, ax), nd = Dot(dir, ax);
            const f32 a = axLen2 - nd * nd, k = Dot(m, m) - r * r, c = axLen2 * k - md * md, b = axLen2 * Dot(m, dir) - nd * md;
            const f32 disc = b * b - a * c;
            if (Abs(a) > 1e-12f && disc >= 0.0f) {
                const f32 tt = (-b - Sqrt(disc)) / a;
                const f32 along = md + tt * nd;
                if (tt >= 0.0f && tt < best && along >= 0.0f && along <= axLen2) {
                    best = tt;
                    hit = true;
                }
            }
        }
        if (!hit)
            return false;
        t = best;
        const Vec3 p = origin + dir * t;
        normal = SafeNormalize(p - ClosestPointSegment(p, p0, p1), -dir);
        return true;
    }
    case ShapeType::TriangleMesh: {
        if (!s.mesh)
            return false;
        const Vec3 o = InvRotate(s.rotation, origin - s.position), d = InvRotate(s.rotation, dir);
        Vec3 nl;
        if (!s.mesh->Raycast(o, d, maxT, t, nl))
            return false;
        normal = s.rotation * nl;
        return true;
    }
    case ShapeType::HeightField: {
        if (!s.heightField)
            return false;
        const Vec3 o = InvRotate(s.rotation, origin - s.position), d = InvRotate(s.rotation, dir);
        Vec3 nl;
        if (!s.heightField->Raycast(o, d, maxT, t, nl))
            return false;
        normal = s.rotation * nl;
        return true;
    }
    }
    return false;
}

} // namespace zx::physics
