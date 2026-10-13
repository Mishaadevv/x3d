#include "Physics/PhysicsWorld.h"
#include "AabbTree.h"
#include "Collide.h"
#include "Core/Jobs/JobSystem.h"
#include "Core/Time/Time.h"

#include <cmath>

namespace zx::physics {

ShapeDesc ShapeDesc::Box(Vec3 halfExtents, Vec3 position, Quat rotation)
{
    ShapeDesc s;
    s.type = ShapeType::Box;
    s.halfExtents = halfExtents;
    s.position = position;
    s.rotation = rotation;
    return s;
}

ShapeDesc ShapeDesc::Sphere(f32 radius, Vec3 position)
{
    ShapeDesc s;
    s.type = ShapeType::Sphere;
    s.radius = radius;
    s.position = position;
    return s;
}

ShapeDesc ShapeDesc::Capsule(f32 radius, f32 halfHeight, Vec3 position, Quat rotation)
{
    ShapeDesc s;
    s.type = ShapeType::Capsule;
    s.radius = radius;
    s.halfHeight = halfHeight;
    s.position = position;
    s.rotation = rotation;
    return s;
}

ShapeDesc ShapeDesc::Mesh(MeshId mesh, Vec3 position, Quat rotation)
{
    ShapeDesc s;
    s.type = ShapeType::TriangleMesh;
    s.mesh = mesh;
    s.position = position;
    s.rotation = rotation;
    return s;
}

namespace {

struct Body
{
    u32 generation = 1;
    bool alive = false;
    MotionType motion = MotionType::Static;
    Vec3 x; // center of mass (world)
    Quat q;
    Vec3 comLocal;
    Vec3 v, w;
    Vec3 force, torque;
    f32 mass = 0.0f;
    f32 invMass = 0.0f;
    Mat3 invInertiaLocal = Mat3::Scale(Vec3(0.0f));
    Mat3 invInertiaWorld = Mat3::Scale(Vec3(0.0f));
    f32 friction = 0.6f, restitution = 0.0f;
    f32 linearDamping = 0.0f, angularDamping = 0.0f, gravityScale = 1.0f;
    u32 layer = 0;
    bool allowSleep = true;
    bool sleeping = false;
    f32 sleepTime = 0.0f;
    u64 userData = 0;
    Array<ShapeDesc> shapes;
    Array<AABB> shapeBounds; // world, this step
    AABB bounds;
    f32 extent = 0.0f; // radius around the center of mass (speculative margin for spinning bodies)
    i32 proxy = AabbTree::Null;
    u32 island = 0;
    bool hasSensor = false;

    Vec3 Origin() const { return x - q * comLocal; }
    bool IsDynamic() const { return motion == MotionType::Dynamic; }
    bool IsAwakeMover() const
    {
        return (motion == MotionType::Dynamic && !sleeping) || (motion == MotionType::Kinematic && (LengthSq(v) > 0.0f || LengthSq(w) > 0.0f));
    }
};

u64 Mix(u64 h)
{
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ull;
    h ^= h >> 33;
    return h;
}

u64 ContactKey(u32 a, u32 b, u32 sa, u32 sb, u32 sub)
{
    u64 h = Mix((u64(a) << 32) | b);
    h = Mix(h ^ ((u64(sa) << 32) | sb));
    return Mix(h ^ sub);
}

struct Contact
{
    u64 key = 0;
    u32 a = 0, b = 0; // body indices (a < b)
    u32 shapeA = 0, shapeB = 0;
    bool sensor = false;
    f32 approachSpeed = 0.0f; // closing speed when the pair first came within reach (impact strength)
    Manifold manifold;
    f32 normalImpulse[4] = {};
    f32 tangentImpulse[4][2] = {};
    Vec3 localA[4]; // contact points relative to the centers of mass, body frames (warm-start matching, position solve)
    Vec3 localB[4];
    Vec3 localNormal; // in A's frame
};

struct PairOut
{
    Array<Contact> contacts;
    bool wakeA = false, wakeB = false;
};

struct SolverPoint
{
    Vec3 rA, rB;         // anchors from the centers of mass (fixed for the step)
    Vec3 localA, localB; // body-frame anchors (current separation)
    f32 separation = 0.0f;
    f32 normalMass = 0.0f;
    f32 tangentMass[2] = {};
    f32 normalImpulse = 0.0f;
    f32 tangentImpulse[2] = {};
    f32 approach = 0.0f;   // normal velocity before the step (restitution)
    f32 maxImpulse = 0.0f; // the point pushed during the step
};

struct SolverContact
{
    u32 contact = 0;
    u32 ia = 0, ib = 0; // solver bodies
    u32 count = 0;
    Vec3 normal, t1, t2;
    f32 friction = 0.0f;
    f32 restitution = 0.0f;
    SolverPoint pts[4];
};

struct BodyPair
{
    u32 a, b;
};

/// Compact body state the solver iterates on.
struct SolverBody
{
    Vec3 x, v, w;
    Quat q;
    f32 invMass = 0.0f;
    Mat3 invI;
    Mat3 invILocal;
    Vec3 gravity;      // linear acceleration (gravity + forces)
    Vec3 angularAccel; // from torques
    f32 linearDamping = 0.0f, angularDamping = 0.0f;
    u32 body = 0;
    bool active = false; // dynamic and awake
    bool moves = false;  // integrates its pose (active or kinematic)
};

Quat IntegrateRotation(const Quat& q, Vec3 angle)
{
    const Quat dq(angle.x, angle.y, angle.z, 0.0f);
    Quat r = dq * q;
    return Normalize(Quat(q.x + 0.5f * r.x, q.y + 0.5f * r.y, q.z + 0.5f * r.z, q.w + 0.5f * r.w));
}

Mat3 OuterProduct(Vec3 a, Vec3 b) { return Mat3(a * b.x, a * b.y, a * b.z); }

/// Inertia tensor of a shape about its own center (local frame), unit-density mass returned in `mass`.
Mat3 ShapeInertia(const ShapeDesc& s, f32& mass)
{
    switch (s.type) {
    case ShapeType::Sphere: {
        mass = s.density * (4.0f / 3.0f) * Pi * s.radius * s.radius * s.radius;
        return Mat3::Scale(Vec3(0.4f * mass * s.radius * s.radius));
    }
    case ShapeType::Box: {
        const Vec3 e = s.halfExtents;
        mass = s.density * 8.0f * e.x * e.y * e.z;
        return Mat3::Scale(Vec3(e.y * e.y + e.z * e.z, e.x * e.x + e.z * e.z, e.x * e.x + e.y * e.y) * (mass / 3.0f));
    }
    case ShapeType::Capsule: {
        const f32 r = s.radius, H = 2.0f * s.halfHeight;
        const f32 mc = s.density * Pi * r * r * H, ms = s.density * (4.0f / 3.0f) * Pi * r * r * r;
        mass = mc + ms;
        const f32 iy = mc * r * r * 0.5f + ms * 0.4f * r * r;
        const f32 ix = mc * (H * H / 12.0f + r * r * 0.25f) + ms * (0.4f * r * r + H * H * 0.25f + 0.375f * H * r);
        return Mat3::Scale(Vec3(ix, iy, ix));
    }
    case ShapeType::TriangleMesh: mass = 0.0f; return Mat3::Scale(Vec3(0.0f));
    }
    mass = 0.0f;
    return Mat3::Scale(Vec3(0.0f));
}

} // namespace

struct PhysicsWorld::Impl
{
    Array<Body> bodies;
    Array<u32> freeBodies;
    Array<UniquePtr<TriangleMesh>> meshes;
    AabbTree staticTree, movingTree;
    Array<Contact> contacts;
    HashMap<u64, u32> contactIndex; // key -> index in `contacts`
    Array<SolverContact> solver;
    Array<SolverBody> solverBodies;
    Array<Array<BodyPair>> bodyPairs; // per body, this step
    HashMap<u64, bool> touching;      // body pair (a << 32 | b) -> sensor overlap, as of the last step
    Array<ContactEvent> events;
    Array<PairOut> pairOuts;           // per pair, this step
    Array<u32> toSolver;

    PosedShape Pose(const Body& b, u32 s) const
    {
        PosedShape p;
        p.desc = &b.shapes[s];
        p.mesh = p.desc->type == ShapeType::TriangleMesh && p.desc->mesh < meshes.Size() ? meshes[p.desc->mesh].Get() : nullptr;
        const Vec3 origin = b.Origin();
        p.position = origin + b.q * p.desc->position;
        p.rotation = b.q * p.desc->rotation;
        return p;
    }

    void UpdateBounds(Body& b)
    {
        b.shapeBounds.Resize(b.shapes.Size());
        b.bounds = AABB();
        for (u32 s = 0; s < b.shapes.Size(); ++s) {
            b.shapeBounds[s] = ShapeBounds(Pose(b, s));
            b.bounds.Expand(b.shapeBounds[s]);
        }
        if (!b.bounds.IsValid())
            b.bounds = AABB(b.x, b.x);
    }

    void WakeBody(Body& b)
    {
        if (b.motion == MotionType::Dynamic && b.sleeping) {
            b.sleeping = false;
            b.sleepTime = 0.0f;
        }
    }
};

PhysicsWorld::PhysicsWorld() : m_impl(MakeUnique<Impl>())
{
    for (u32& m : m_layerMask)
        m = ~0u;
}

PhysicsWorld::~PhysicsWorld() = default;

MeshId PhysicsWorld::CreateTriangleMesh(Span<const Vec3> positions, Span<const u32> indices)
{
    UniquePtr<TriangleMesh> mesh = MakeUnique<TriangleMesh>();
    mesh->positions.Append(positions);
    mesh->indices.Append(indices);
    mesh->indices.Resize(mesh->indices.Size() / 3 * 3);
    mesh->Build();
    m_impl->meshes.PushBack(move(mesh));
    return MeshId(m_impl->meshes.Size() - 1);
}

BodyId PhysicsWorld::CreateBody(const BodyDesc& desc)
{
    Impl& I = *m_impl;
    u32 index;
    if (!I.freeBodies.Empty()) {
        index = I.freeBodies.PopBackValue();
    } else {
        index = u32(I.bodies.Size());
        I.bodies.AddDefaulted();
    }
    Body& b = I.bodies[index];
    const u32 generation = b.generation;
    b = Body();
    b.generation = generation;
    b.alive = true;
    b.motion = desc.motion;
    b.q = Normalize(desc.rotation);
    b.shapes = desc.shapes;
    for (ShapeDesc& s : b.shapes)
        s.rotation = Normalize(s.rotation);
    b.friction = desc.friction;
    b.restitution = desc.restitution;
    b.linearDamping = desc.linearDamping;
    b.angularDamping = desc.angularDamping;
    b.gravityScale = desc.gravityScale;
    b.layer = desc.layer & 31;
    b.allowSleep = desc.allowSleep;
    b.userData = desc.userData;
    for (const ShapeDesc& s : b.shapes)
        b.hasSensor |= s.isSensor;

    // Mass properties (dynamic bodies): total mass, center of mass, inertia about it (parallel axis theorem).
    if (b.motion == MotionType::Dynamic) {
        f32 total = 0.0f;
        Vec3 com;
        Array<f32> masses;
        Array<Mat3> inertias;
        for (const ShapeDesc& s : b.shapes) {
            f32 m;
            inertias.PushBack(ShapeInertia(s, m));
            if (s.isSensor) {
                m = 0.0f;
                inertias.Back() = Mat3::Scale(Vec3(0.0f));
            }
            masses.PushBack(m);
            total += m;
            com += s.position * m;
        }
        if (total <= 0.0f) {
            total = 1.0f;
            masses.Clear();
        } else {
            com = com / total;
        }
        Mat3 inertia = Mat3::Scale(Vec3(0.0f));
        for (usize i = 0; i < masses.Size(); ++i) {
            const Mat3 r = Mat3::FromQuat(b.shapes[i].rotation);
            const Mat3 rotated = r * inertias[i] * Transpose(r);
            const Vec3 d = b.shapes[i].position - com;
            const Mat3 shift = Mat3::Scale(Vec3(Dot(d, d))) * masses[i];
            const Mat3 outer = OuterProduct(d, d) * masses[i];
            for (u32 c = 0; c < 3; ++c)
                inertia.columns[c] = inertia.columns[c] + rotated.columns[c] + shift.columns[c] - outer.columns[c];
        }
        if (masses.Empty())
            inertia = Mat3::Scale(Vec3(0.4f)); // no volume: a unit sphere-like body
        f32 mass = total;
        if (desc.mass > 0.0f) {
            inertia = inertia * (desc.mass / total);
            mass = desc.mass;
        }
        b.mass = mass;
        b.invMass = 1.0f / mass;
        b.comLocal = com;
        b.invInertiaLocal = Inverse(inertia);
    }
    b.x = desc.position + b.q * b.comLocal;
    if (b.motion != MotionType::Static) {
        b.v = desc.linearVelocity;
        b.w = desc.angularVelocity;
    }
    const Mat3 r = Mat3::FromQuat(b.q);
    b.invInertiaWorld = r * b.invInertiaLocal * Transpose(r);
    I.UpdateBounds(b);
    b.extent = Length(b.bounds.Extents()) + Length(b.bounds.Center() - b.x);
    AabbTree& tree = b.motion == MotionType::Static ? I.staticTree : I.movingTree;
    b.proxy = tree.Insert(b.bounds.Inflated(0.1f), index);
    ++m_bodyCount;
    return BodyId{index, b.generation};
}

void PhysicsWorld::DestroyBody(BodyId id)
{
    if (!IsValid(id))
        return;
    Impl& I = *m_impl;
    Body& b = I.bodies[id.index];
    // Wake what rested on it.
    for (const Contact& c : I.contacts)
        if (c.a == id.index || c.b == id.index) {
            I.WakeBody(I.bodies[c.a]);
            I.WakeBody(I.bodies[c.b]);
        }
    (b.motion == MotionType::Static ? I.staticTree : I.movingTree).Remove(b.proxy);
    b.alive = false;
    ++b.generation;
    b.shapes.Clear();
    I.freeBodies.PushBack(id.index);
    --m_bodyCount;
    // Drop its contacts.
    usize w = 0;
    for (usize i = 0; i < I.contacts.Size(); ++i)
        if (I.contacts[i].a != id.index && I.contacts[i].b != id.index)
            I.contacts[w++] = I.contacts[i];
    I.contacts.Resize(w);
    I.contactIndex.Clear();
    for (usize i = 0; i < I.contacts.Size(); ++i)
        I.contactIndex.InsertOrAssign(I.contacts[i].key, u32(i));
    I.touching.RemoveIf([&](u64 key, bool) { return u32(key >> 32) == id.index || u32(key) == id.index; });
}

bool PhysicsWorld::IsValid(BodyId id) const
{
    return id.index < m_impl->bodies.Size() && m_impl->bodies[id.index].alive && m_impl->bodies[id.index].generation == id.generation;
}

#define ZX_BODY_OR(fallback)                                                                                                \
    if (!IsValid(id))                                                                                                      \
        return fallback;                                                                                                   \
    const Body& b = m_impl->bodies[id.index];

Vec3 PhysicsWorld::GetPosition(BodyId id) const
{
    ZX_BODY_OR(Vec3());
    return b.Origin();
}
Quat PhysicsWorld::GetRotation(BodyId id) const
{
    ZX_BODY_OR(Quat());
    return b.q;
}
Vec3 PhysicsWorld::GetCenterOfMass(BodyId id) const
{
    ZX_BODY_OR(Vec3());
    return b.x;
}
Vec3 PhysicsWorld::GetLinearVelocity(BodyId id) const
{
    ZX_BODY_OR(Vec3());
    return b.v;
}
Vec3 PhysicsWorld::GetAngularVelocity(BodyId id) const
{
    ZX_BODY_OR(Vec3());
    return b.w;
}
MotionType PhysicsWorld::GetMotionType(BodyId id) const
{
    ZX_BODY_OR(MotionType::Static);
    return b.motion;
}
f32 PhysicsWorld::GetMass(BodyId id) const
{
    ZX_BODY_OR(0.0f);
    return b.mass;
}
u64 PhysicsWorld::GetUserData(BodyId id) const
{
    ZX_BODY_OR(0);
    return b.userData;
}
bool PhysicsWorld::IsSleeping(BodyId id) const
{
    ZX_BODY_OR(false);
    return b.sleeping;
}
AABB PhysicsWorld::GetBounds(BodyId id) const
{
    ZX_BODY_OR(AABB());
    return b.bounds;
}
const Array<ShapeDesc>& PhysicsWorld::GetShapes(BodyId id) const
{
    static const Array<ShapeDesc> none;
    ZX_BODY_OR(none);
    return b.shapes;
}
#undef ZX_BODY_OR

void PhysicsWorld::SetTransform(BodyId id, Vec3 position, Quat rotation)
{
    if (!IsValid(id))
        return;
    Body& b = m_impl->bodies[id.index];
    b.q = Normalize(rotation);
    b.x = position + b.q * b.comLocal;
    const Mat3 r = Mat3::FromQuat(b.q);
    b.invInertiaWorld = r * b.invInertiaLocal * Transpose(r);
    m_impl->UpdateBounds(b);
    (b.motion == MotionType::Static ? m_impl->staticTree : m_impl->movingTree).Move(b.proxy, b.bounds, b.bounds.Inflated(0.1f));
    m_impl->WakeBody(b);
    // Whatever touched it must re-evaluate.
    for (const Contact& c : m_impl->contacts)
        if (c.a == id.index || c.b == id.index) {
            m_impl->WakeBody(m_impl->bodies[c.a]);
            m_impl->WakeBody(m_impl->bodies[c.b]);
        }
}

void PhysicsWorld::SetLinearVelocity(BodyId id, Vec3 v)
{
    if (!IsValid(id) || m_impl->bodies[id.index].motion == MotionType::Static)
        return;
    m_impl->bodies[id.index].v = v;
    m_impl->WakeBody(m_impl->bodies[id.index]);
}

void PhysicsWorld::SetAngularVelocity(BodyId id, Vec3 w)
{
    if (!IsValid(id) || m_impl->bodies[id.index].motion == MotionType::Static)
        return;
    m_impl->bodies[id.index].w = w;
    m_impl->WakeBody(m_impl->bodies[id.index]);
}

void PhysicsWorld::MoveKinematic(BodyId id, Vec3 position, Quat rotation, f32 dt)
{
    if (!IsValid(id) || dt <= 0.0f)
        return;
    Body& b = m_impl->bodies[id.index];
    if (b.motion != MotionType::Kinematic)
        return;
    const Quat target = Normalize(rotation);
    b.v = (position + target * b.comLocal - b.x) / dt;
    Quat dq = target * Conjugate(b.q);
    if (dq.w < 0.0f)
        dq = -dq;
    Vec3 axis;
    f32 angle;
    dq.ToAxisAngle(axis, angle);
    b.w = angle > 1e-7f ? axis * (angle / dt) : Vec3();
}

void PhysicsWorld::AddForce(BodyId id, Vec3 force)
{
    if (!IsValid(id) || !m_impl->bodies[id.index].IsDynamic())
        return;
    m_impl->bodies[id.index].force += force;
    m_impl->WakeBody(m_impl->bodies[id.index]);
}

void PhysicsWorld::AddForceAtPoint(BodyId id, Vec3 force, Vec3 worldPoint)
{
    if (!IsValid(id) || !m_impl->bodies[id.index].IsDynamic())
        return;
    Body& b = m_impl->bodies[id.index];
    b.force += force;
    b.torque += Cross(worldPoint - b.x, force);
    m_impl->WakeBody(b);
}

void PhysicsWorld::AddImpulse(BodyId id, Vec3 impulse)
{
    if (!IsValid(id) || !m_impl->bodies[id.index].IsDynamic())
        return;
    Body& b = m_impl->bodies[id.index];
    b.v += impulse * b.invMass;
    m_impl->WakeBody(b);
}

void PhysicsWorld::AddImpulseAtPoint(BodyId id, Vec3 impulse, Vec3 worldPoint)
{
    if (!IsValid(id) || !m_impl->bodies[id.index].IsDynamic())
        return;
    Body& b = m_impl->bodies[id.index];
    b.v += impulse * b.invMass;
    b.w += b.invInertiaWorld * Cross(worldPoint - b.x, impulse);
    m_impl->WakeBody(b);
}

void PhysicsWorld::AddTorque(BodyId id, Vec3 torque)
{
    if (!IsValid(id) || !m_impl->bodies[id.index].IsDynamic())
        return;
    m_impl->bodies[id.index].torque += torque;
    m_impl->WakeBody(m_impl->bodies[id.index]);
}

void PhysicsWorld::Wake(BodyId id)
{
    if (IsValid(id))
        m_impl->WakeBody(m_impl->bodies[id.index]);
}

void PhysicsWorld::SetLayersCollide(u32 a, u32 b, bool collide)
{
    a &= 31;
    b &= 31;
    if (collide) {
        m_layerMask[a] |= 1u << b;
        m_layerMask[b] |= 1u << a;
    } else {
        m_layerMask[a] &= ~(1u << b);
        m_layerMask[b] &= ~(1u << a);
    }
}

// --- Step --------------------------------------------------------------------------------------------------------------

void PhysicsWorld::Step(f32 dt)
{
    if (dt <= 0.0f)
        return;
    Stopwatch timer;
    Impl& I = *m_impl;
    const PhysicsSettings& S = m_settings;
    Array<Body>& bodies = I.bodies;

    // 1. Broadphase: refresh the moving proxies, then pairs of awake movers with everything near them. Bounds and
    //    tree queries run in parallel (per body); tree updates and the merge stay serial and in body order.
    const bool parallel = jobs::IsInitialized() && bodies.Size() > 128;
    auto forBodies = [&](auto&& fn) {
        if (parallel)
            jobs::ParallelFor(u32(bodies.Size()), 32, [&](u32 begin, u32 end) {
                for (u32 i = begin; i < end; ++i)
                    fn(i);
            });
        else
            for (u32 i = 0; i < bodies.Size(); ++i)
                fn(i);
    };
    forBodies([&](u32 i) {
        Body& b = bodies[i];
        if (b.alive && b.motion != MotionType::Static && !b.sleeping)
            I.UpdateBounds(b);
    });
    for (u32 i = 0; i < bodies.Size(); ++i) {
        Body& b = bodies[i];
        if (!b.alive || b.motion == MotionType::Static || b.sleeping)
            continue;
        const Vec3 move = b.v * dt;
        AABB fat = b.bounds.Inflated(0.1f);
        fat.Expand(fat.min + Min(move * 2.0f, Vec3()));
        fat.Expand(fat.max + Max(move * 2.0f, Vec3()));
        I.movingTree.Move(b.proxy, b.bounds.Inflated(S.speculativeDistance), fat);
    }
    m_stats.boundsMs = timer.ElapsedMilliseconds();
    using Pair = BodyPair;
    Array<Array<Pair>>& bodyPairs = I.bodyPairs;
    bodyPairs.Resize(bodies.Size());
    forBodies([&](u32 i) {
        Array<Pair>& out = bodyPairs[i];
        out.Clear();
        const Body& bi = bodies[i];
        if (!bi.alive || !bi.IsAwakeMover())
            return;
        const AABB query = bi.bounds.Inflated(S.speculativeDistance + Length(bi.v) * dt);
        auto consider = [&](u32 j) {
            if (j == i)
                return true;
            const Body& bj = bodies[j];
            if (!LayersCollide(bi.layer, bj.layer) || !bj.bounds.Intersects(query))
                return true;
            const bool iDyn = bi.IsDynamic();
            switch (bj.motion) {
            case MotionType::Static:
            case MotionType::Kinematic:
                if (!iDyn && !bi.hasSensor && !bj.hasSensor)
                    return true; // kinematic vs static/kinematic: only sensors matter
                if (!iDyn && bj.motion == MotionType::Kinematic && bj.IsAwakeMover() && j < i)
                    return true; // two moving kinematic bodies: the other one reports it
                break;
            case MotionType::Dynamic:
                if (!bj.sleeping && (!iDyn || j < i))
                    return true; // the other awake body (or the dynamic side) reports this pair
                break;
            }
            out.PushBack(Pair{Min(i, j), Max(i, j)});
            return true;
        };
        if (bi.IsDynamic() || bi.motion == MotionType::Kinematic)
            I.staticTree.Query(query, consider);
        I.movingTree.Query(query, consider);
    });
    Array<Pair> pairs;
    for (const Array<Pair>& bp : bodyPairs)
        pairs.Append(bp.View());

    m_stats.broadphaseMs = timer.ElapsedMilliseconds();
    // 2. Narrowphase: manifolds for every shape pair, impulses carried over from matching points of the last step.
    Array<Contact> contacts;
    contacts.Reserve(I.contacts.Size() + 16);
    HashMap<u64, u32> index;
    u32 pointCount = 0;
    // Resting contacts of sleeping islands are kept as they are (warm start when they wake).
    for (const Contact& c : I.contacts) {
        const Body& a = bodies[c.a];
        const Body& b = bodies[c.b];
        if (a.alive && b.alive && !a.IsAwakeMover() && !b.IsAwakeMover() && (a.IsDynamic() || b.IsDynamic())) {
            index.InsertOrAssign(c.key, u32(contacts.Size()));
            contacts.PushBack(c);
        }
    }
    // Bounds of sleeping partners may be stale after a teleport (cheap to refresh, and done before going parallel).
    for (const Pair& p : pairs) {
        if (bodies[p.a].sleeping)
            I.UpdateBounds(bodies[p.a]);
        if (bodies[p.b].sleeping)
            I.UpdateBounds(bodies[p.b]);
    }
    // Pairs are independent: collide them in parallel, then merge in pair order (deterministic).
    Array<PairOut>& outs = I.pairOuts; // persistent: no heap traffic from the workers
    if (outs.Size() < pairs.Size())
        outs.Resize(pairs.Size());
    for (u32 pi = 0; pi < pairs.Size(); ++pi) {
        outs[pi].contacts.Clear();
        outs[pi].wakeA = outs[pi].wakeB = false;
    }
    auto narrow = [&](u32 begin, u32 end) {
        Array<Manifold> scratch; // per batch (a thread_local would outlive the leak check)
        for (u32 pi = begin; pi < end; ++pi) {
            const Pair& p = pairs[pi];
            const Body& A = bodies[p.a];
            const Body& B = bodies[p.b];
            PairOut& out = outs[pi];
            const f32 margin = Min(S.speculativeDistance + (Length(A.v - B.v) + Length(A.w) * A.extent + Length(B.w) * B.extent) * dt, 2.0f);
            for (u32 sa = 0; sa < A.shapes.Size(); ++sa)
                for (u32 sb = 0; sb < B.shapes.Size(); ++sb) {
                    if (!A.shapeBounds[sa].Inflated(margin).Intersects(B.shapeBounds[sb]))
                        continue;
                    scratch.Clear();
                    Collide(I.Pose(A, sa), I.Pose(B, sb), margin, scratch);
                    for (const Manifold& m : scratch) {
                        if (m.count == 0)
                            continue;
                        Contact c;
                        c.key = ContactKey(p.a, p.b, sa, sb, m.subKey);
                        c.a = p.a;
                        c.b = p.b;
                        c.shapeA = sa;
                        c.shapeB = sb;
                        c.sensor = A.shapes[sa].isSensor || B.shapes[sb].isSensor;
                        c.manifold = m;
                        c.localNormal = Conjugate(A.q) * m.normal;
                        const Contact* old = nullptr;
                        if (const u32* oi = I.contactIndex.Find(c.key))
                            old = &I.contacts[*oi];
                        c.approachSpeed = old ? old->approachSpeed : Max(0.0f, -Dot(B.v - A.v, m.normal));
                        f32 minSep = Infinity;
                        for (u32 k = 0; k < m.count; ++k) {
                            c.localA[k] = Conjugate(A.q) * (m.points[k].pointA - A.x);
                            c.localB[k] = Conjugate(B.q) * (m.points[k].pointB - B.x);
                            minSep = Min(minSep, m.points[k].separation);
                            if (!old)
                                continue;
                            for (u32 o = 0; o < old->manifold.count; ++o)
                                if (DistanceSq(old->localA[o], c.localA[k]) < 0.02f * 0.02f) {
                                    c.normalImpulse[k] = old->normalImpulse[o];
                                    c.tangentImpulse[k][0] = old->tangentImpulse[o][0];
                                    c.tangentImpulse[k][1] = old->tangentImpulse[o][1];
                                    break;
                                }
                        }
                        // Touching an awake body wakes a sleeping one.
                        if (minSep < S.speculativeDistance && !c.sensor) {
                            out.wakeA |= A.sleeping && B.IsAwakeMover();
                            out.wakeB |= B.sleeping && A.IsAwakeMover();
                        }
                        out.contacts.PushBack(c);
                    }
                }
        }
    };
    if (jobs::IsInitialized() && pairs.Size() > 64)
        jobs::ParallelFor(u32(pairs.Size()), 32, narrow);
    else
        narrow(0, u32(pairs.Size()));
    for (u32 pi = 0; pi < pairs.Size(); ++pi) {
        PairOut& out = outs[pi];
        if (out.wakeA)
            I.WakeBody(bodies[pairs[pi].a]);
        if (out.wakeB)
            I.WakeBody(bodies[pairs[pi].b]);
        for (Contact& c : out.contacts) {
            pointCount += c.manifold.count;
            index.InsertOrAssign(c.key, u32(contacts.Size()));
            contacts.PushBack(move(c));
        }
    }
    I.contacts = move(contacts);
    I.contactIndex = move(index);

    // Contact events: body pairs that started / stopped touching (or overlapping a sensor) since the last step.
    {
        HashMap<u64, bool> now;
        for (const Contact& c : I.contacts) {
            f32 minSep = Infinity;
            for (u32 k = 0; k < c.manifold.count; ++k)
                minSep = Min(minSep, c.manifold.points[k].separation);
            if (minSep > (c.sensor ? 0.0f : 0.01f))
                continue;
            const u64 key = (u64(c.a) << 32) | c.b;
            bool* sensor = now.Find(key);
            if (sensor) {
                *sensor = *sensor && c.sensor; // any solid contact makes the pair a touch
                continue;
            }
            now.InsertOrAssign(key, c.sensor);
            if (I.touching.Contains(key))
                continue;
            ContactEvent e;
            e.type = c.sensor ? ContactEventType::SensorEnter : ContactEventType::TouchBegin;
            e.a = BodyId{c.a, bodies[c.a].generation};
            e.b = BodyId{c.b, bodies[c.b].generation};
            e.point = c.manifold.points[0].pointB;
            e.normal = c.manifold.normal;
            e.speed = c.approachSpeed;
            I.events.PushBack(e);
        }
        for (const auto& [key, sensor] : I.touching)
            if (!now.Contains(key)) {
                const u32 a = u32(key >> 32), b = u32(key);
                if (!bodies[a].alive || !bodies[b].alive)
                    continue;
                ContactEvent e;
                e.type = sensor ? ContactEventType::SensorExit : ContactEventType::TouchEnd;
                e.a = BodyId{a, bodies[a].generation};
                e.b = BodyId{b, bodies[b].generation};
                I.events.PushBack(e);
            }
        I.touching = move(now);
    }

    m_stats.narrowphaseMs = timer.ElapsedMilliseconds() - m_stats.broadphaseMs;
    // 3. Islands of awake dynamic bodies connected by contacts (union-find).
    Array<u32> parent;
    parent.Resize(bodies.Size());
    for (u32 i = 0; i < bodies.Size(); ++i)
        parent[i] = i;
    auto find = [&](u32 x) {
        while (parent[x] != x) {
            parent[x] = parent[parent[x]];
            x = parent[x];
        }
        return x;
    };
    for (const Contact& c : I.contacts) {
        const Body& a = bodies[c.a];
        const Body& b = bodies[c.b];
        if (a.IsDynamic() && b.IsDynamic() && !a.sleeping && !b.sleeping && !c.sensor)
            parent[find(c.a)] = find(c.b);
    }

    // 4. Solver bodies: a compact copy of every awake dynamic body (all integrate) and of every other body a contact
    //    touches (static / kinematic / sleeping: infinite mass). Written back after the substeps.
    Array<SolverBody>& sbodies = I.solverBodies;
    sbodies.Clear();
    Array<u32>& toSolver = I.toSolver;
    toSolver.Resize(bodies.Size());
    for (u32& t : toSolver)
        t = ~0u;
    auto solverBody = [&](u32 index) {
        if (toSolver[index] != ~0u)
            return toSolver[index];
        const Body& b = bodies[index];
        const bool active = b.IsDynamic() && !b.sleeping;
        SolverBody s;
        s.x = b.x;
        s.q = b.q;
        s.v = active || b.motion == MotionType::Kinematic ? b.v : Vec3();
        s.w = active || b.motion == MotionType::Kinematic ? b.w : Vec3();
        s.invMass = active ? b.invMass : 0.0f;
        s.invI = active ? b.invInertiaWorld : Mat3::Scale(Vec3(0.0f));
        s.invILocal = b.invInertiaLocal;
        s.body = index;
        s.active = active;
        s.moves = active || b.motion == MotionType::Kinematic;
        s.gravity = active ? S.gravity * b.gravityScale + b.force * b.invMass : Vec3();
        s.angularAccel = active ? b.invInertiaWorld * b.torque : Vec3();
        s.linearDamping = b.linearDamping;
        s.angularDamping = b.angularDamping;
        toSolver[index] = u32(sbodies.Size());
        sbodies.PushBack(s);
        return toSolver[index];
    };
    for (u32 i = 0; i < bodies.Size(); ++i)
        if (bodies[i].alive && ((bodies[i].IsDynamic() && !bodies[i].sleeping) || bodies[i].motion == MotionType::Kinematic))
            solverBody(i);

    // Constraints: effective masses for the anchors of this step, approach speed for restitution.
    I.solver.Clear();
    for (u32 ci = 0; ci < I.contacts.Size(); ++ci) {
        Contact& c = I.contacts[ci];
        const Body& A = bodies[c.a];
        const Body& B = bodies[c.b];
        if (c.sensor || (!(A.IsDynamic() && !A.sleeping) && !(B.IsDynamic() && !B.sleeping)))
            continue;
        SolverContact sc;
        sc.contact = ci;
        sc.ia = solverBody(c.a);
        sc.ib = solverBody(c.b);
        sc.count = c.manifold.count;
        sc.normal = c.manifold.normal;
        sc.friction = Sqrt(A.friction * B.friction);
        sc.restitution = Max(A.restitution, B.restitution);
        const Vec3 n = sc.normal;
        sc.t1 = Abs(n.x) > 0.57f ? Normalize(Vec3(n.y, -n.x, 0.0f)) : Normalize(Vec3(0.0f, n.z, -n.y));
        sc.t2 = Cross(n, sc.t1);
        const SolverBody& a = sbodies[sc.ia];
        const SolverBody& b = sbodies[sc.ib];
        auto effMass = [&](Vec3 rA, Vec3 rB, Vec3 dir) {
            const Vec3 ra = Cross(rA, dir), rb = Cross(rB, dir);
            const f32 k = a.invMass + b.invMass + Dot(ra, a.invI * ra) + Dot(rb, b.invI * rb);
            return k > 0.0f ? 1.0f / k : 0.0f;
        };
        for (u32 k = 0; k < sc.count; ++k) {
            SolverPoint& sp = sc.pts[k];
            sp.rA = c.manifold.points[k].pointA - A.x;
            sp.rB = c.manifold.points[k].pointB - B.x;
            sp.localA = c.localA[k];
            sp.localB = c.localB[k];
            sp.normalMass = effMass(sp.rA, sp.rB, n);
            sp.tangentMass[0] = effMass(sp.rA, sp.rB, sc.t1);
            sp.tangentMass[1] = effMass(sp.rA, sp.rB, sc.t2);
            sp.approach = Dot(b.v + Cross(b.w, sp.rB) - a.v - Cross(a.w, sp.rA), n);
            sp.maxImpulse = 0.0f;
            sp.normalImpulse = c.normalImpulse[k];
            sp.tangentImpulse[0] = c.tangentImpulse[k][0];
            sp.tangentImpulse[1] = c.tangentImpulse[k][1];
        }
        I.solver.PushBack(sc);
    }

    // 5. Soft-step solver (as in Box2D v3): substeps of integrate velocities, warm start, solve with a soft
    //    (spring-damper) bias that pushes overlaps apart, integrate positions, relax without bias.
    const u32 substeps = Max(S.substeps, 1u);
    const f32 h = dt / f32(substeps), invH = 1.0f / h;
    const f32 hertz = Min(S.contactHertz, 0.25f * invH), zeta = S.contactDampingRatio;
    const f32 omega = 2.0f * Pi * hertz;
    const f32 a1 = 2.0f * zeta + h * omega, a2 = h * omega * a1, a3 = 1.0f / (1.0f + a2);
    const f32 biasRate = omega / a1, massScale = a2 * a3, impulseScale = a3;
    auto applyImpulse = [](SolverBody& a, SolverBody& b, const SolverPoint& sp, Vec3 P) {
        a.v -= P * a.invMass;
        a.w -= a.invI * Cross(sp.rA, P);
        b.v += P * b.invMass;
        b.w += b.invI * Cross(sp.rB, P);
    };
    // Current separation of every point from its anchors on the moved bodies (once per substep: poses only change
    // when they are integrated).
    auto updateSeparations = [&] {
        for (SolverContact& sc : I.solver) {
            const SolverBody& A = sbodies[sc.ia];
            const SolverBody& B = sbodies[sc.ib];
            for (u32 k = 0; k < sc.count; ++k)
                sc.pts[k].separation = Dot((B.x + B.q * sc.pts[k].localB) - (A.x + A.q * sc.pts[k].localA), sc.normal);
        }
    };
    updateSeparations();
    auto solve = [&](bool useBias) {
        for (SolverContact& sc : I.solver) {
            SolverBody& A = sbodies[sc.ia];
            SolverBody& B = sbodies[sc.ib];
            for (u32 k = 0; k < sc.count; ++k) {
                SolverPoint& sp = sc.pts[k];
                const f32 s = sp.separation;
                f32 bias = 0.0f, mScale = 1.0f, iScale = 0.0f;
                if (s > 0.0f) {
                    bias = s * invH; // speculative: close at most the gap this substep
                } else if (useBias) {
                    bias = Max(biasRate * s, -S.maxPushVelocity);
                    mScale = massScale;
                    iScale = impulseScale;
                }
                const f32 vn = Dot(B.v + Cross(B.w, sp.rB) - A.v - Cross(A.w, sp.rA), sc.normal);
                const f32 lambda = -sp.normalMass * mScale * (vn + bias) - iScale * sp.normalImpulse;
                const f32 old = sp.normalImpulse;
                sp.normalImpulse = Max(old + lambda, 0.0f);
                sp.maxImpulse = Max(sp.maxImpulse, sp.normalImpulse);
                applyImpulse(A, B, sp, sc.normal * (sp.normalImpulse - old));
            }
            for (u32 k = 0; k < sc.count; ++k) {
                SolverPoint& sp = sc.pts[k];
                const f32 limit = sc.friction * sp.normalImpulse;
                const Vec3 dv = B.v + Cross(B.w, sp.rB) - A.v - Cross(A.w, sp.rA);
                const f32 l1 = -sp.tangentMass[0] * Dot(dv, sc.t1), l2 = -sp.tangentMass[1] * Dot(dv, sc.t2);
                const f32 o1 = sp.tangentImpulse[0], o2 = sp.tangentImpulse[1];
                sp.tangentImpulse[0] = Clamp(o1 + l1, -limit, limit);
                sp.tangentImpulse[1] = Clamp(o2 + l2, -limit, limit);
                applyImpulse(A, B, sp, sc.t1 * (sp.tangentImpulse[0] - o1) + sc.t2 * (sp.tangentImpulse[1] - o2));
            }
        }
    };
    for (u32 sub = 0; sub < substeps; ++sub) {
        for (SolverBody& b : sbodies) {
            if (!b.active)
                continue;
            b.v += b.gravity * h;
            b.w += b.angularAccel * h;
            b.v = b.v * (1.0f / (1.0f + h * b.linearDamping));
            b.w = b.w * (1.0f / (1.0f + h * b.angularDamping));
        }
        for (SolverContact& sc : I.solver) {
            SolverBody& A = sbodies[sc.ia];
            SolverBody& B = sbodies[sc.ib];
            for (u32 k = 0; k < sc.count; ++k) {
                const SolverPoint& sp = sc.pts[k];
                applyImpulse(A, B, sp, sc.normal * sp.normalImpulse + sc.t1 * sp.tangentImpulse[0] + sc.t2 * sp.tangentImpulse[1]);
            }
        }
        solve(true);
        for (SolverBody& b : sbodies) {
            if (!b.moves)
                continue;
            b.x += b.v * h;
            b.q = IntegrateRotation(b.q, b.w * h);
            if (b.active) {
                const Mat3 r = Mat3::FromQuat(b.q);
                b.invI = r * b.invILocal * Transpose(r);
            }
        }
        updateSeparations();
        for (u32 it = 0; it < Max(S.relaxIterations, 1u); ++it)
            solve(false);
    }

    // 6. Restitution: points that approached fast and pushed get the bounce velocity.
    for (SolverContact& sc : I.solver) {
        if (sc.restitution <= 0.0f)
            continue;
        SolverBody& A = sbodies[sc.ia];
        SolverBody& B = sbodies[sc.ib];
        for (u32 k = 0; k < sc.count; ++k) {
            SolverPoint& sp = sc.pts[k];
            if (sp.approach > -S.restitutionThreshold || sp.maxImpulse <= 0.0f)
                continue;
            const f32 vn = Dot(B.v + Cross(B.w, sp.rB) - A.v - Cross(A.w, sp.rA), sc.normal);
            const f32 lambda = -sp.normalMass * (vn + sc.restitution * sp.approach);
            const f32 old = sp.normalImpulse;
            sp.normalImpulse = Max(old + lambda, 0.0f);
            applyImpulse(A, B, sp, sc.normal * (sp.normalImpulse - old));
        }
    }
    // Write back: impulses for the next step's warm start, poses and velocities.
    for (const SolverContact& sc : I.solver) {
        Contact& c = I.contacts[sc.contact];
        for (u32 k = 0; k < sc.count; ++k) {
            c.normalImpulse[k] = sc.pts[k].normalImpulse;
            c.tangentImpulse[k][0] = sc.pts[k].tangentImpulse[0];
            c.tangentImpulse[k][1] = sc.pts[k].tangentImpulse[1];
        }
    }
    for (const SolverBody& s : sbodies) {
        if (!s.moves)
            continue;
        Body& b = bodies[s.body];
        b.x = s.x;
        b.q = s.q;
        b.v = s.v;
        b.w = s.w;
        if (s.active)
            b.invInertiaWorld = s.invI;
    }

    m_stats.solverMs = timer.ElapsedMilliseconds() - m_stats.broadphaseMs - m_stats.narrowphaseMs;
    // 7. Sleep: islands whose bodies all rested long enough.
    const f32 sleepV2 = S.sleepVelocity * S.sleepVelocity;
    HashMap<u32, f32> islandRest; // root -> min rest time (or -1 = cannot sleep)
    for (u32 i = 0; i < bodies.Size(); ++i) {
        Body& b = bodies[i];
        if (!b.alive || !b.IsDynamic() || b.sleeping)
            continue;
        if (LengthSq(b.v) > sleepV2 || LengthSq(b.w) > sleepV2 || !b.allowSleep)
            b.sleepTime = 0.0f;
        else
            b.sleepTime += dt;
        const u32 root = find(i);
        f32* rest = islandRest.Find(root);
        const f32 t = b.allowSleep ? b.sleepTime : -1.0f;
        if (!rest)
            islandRest.InsertOrAssign(root, t);
        else
            *rest = Min(*rest, t);
    }
    // Islands touching a moving kinematic body stay awake.
    for (const Contact& c : I.contacts) {
        const Body& a = bodies[c.a];
        const Body& b = bodies[c.b];
        if (a.motion == MotionType::Kinematic && a.IsAwakeMover() && b.IsDynamic())
            if (f32* r = islandRest.Find(find(c.b)))
                *r = -1.0f;
        if (b.motion == MotionType::Kinematic && b.IsAwakeMover() && a.IsDynamic())
            if (f32* r = islandRest.Find(find(c.a)))
                *r = -1.0f;
    }
    u32 awake = 0, dynamicCount = 0;
    for (u32 i = 0; i < bodies.Size(); ++i) {
        Body& b = bodies[i];
        if (!b.alive)
            continue;
        if (b.motion == MotionType::Static)
            continue;
        if (b.IsDynamic())
            ++dynamicCount;
        if (b.IsDynamic() && !b.sleeping) {
            const f32* rest = islandRest.Find(find(i));
            if (rest && *rest >= S.timeToSleep) {
                b.sleeping = true;
                b.v = b.w = Vec3();
                I.UpdateBounds(b);
                I.movingTree.Move(b.proxy, b.bounds, b.bounds.Inflated(0.1f));
            } else {
                ++awake;
            }
        }
        b.force = b.torque = Vec3();
    }

    m_stats.bodies = m_bodyCount;
    m_stats.dynamic = dynamicCount;
    m_stats.awake = awake;
    m_stats.pairs = u32(pairs.Size());
    m_stats.contacts = u32(I.contacts.Size());
    m_stats.points = pointCount;
    m_stats.islands = u32(islandRest.Size());
    m_stats.stepMs = timer.ElapsedMilliseconds();
}

// --- Queries -----------------------------------------------------------------------------------------------------------

bool PhysicsWorld::Raycast(const Ray& ray, f32 maxDistance, RaycastHit& hit, u32 layerMask) const
{
    const Impl& I = *m_impl;
    const Vec3 dir = Normalize(ray.direction);
    bool found = false;
    f32 best = maxDistance;
    auto test = [&](u32 index, f32 maxT) {
        const Body& b = I.bodies[index];
        if (!b.alive || !((layerMask >> b.layer) & 1u))
            return maxT;
        for (u32 s = 0; s < b.shapes.Size(); ++s) {
            f32 t;
            Vec3 n;
            if (RaycastShape(I.Pose(b, s), ray.origin, dir, best, t, n) && t < best) {
                best = t;
                found = true;
                hit.body = BodyId{index, b.generation};
                hit.distance = t;
                hit.point = ray.origin + dir * t;
                hit.normal = n;
                hit.shape = s;
            }
        }
        return best;
    };
    I.staticTree.Raycast(ray.origin, dir, best, test);
    I.movingTree.Raycast(ray.origin, dir, best, test);
    return found;
}

void PhysicsWorld::QueryAabb(const AABB& box, Array<BodyId>& out) const
{
    const Impl& I = *m_impl;
    auto add = [&](u32 index) {
        const Body& b = I.bodies[index];
        if (b.alive && b.bounds.Intersects(box))
            out.PushBack(BodyId{index, b.generation});
        return true;
    };
    I.staticTree.Query(box, add);
    I.movingTree.Query(box, add);
}

void PhysicsWorld::Overlap(const ShapeDesc& shape, Vec3 position, Quat rotation, Array<BodyId>& out, u32 layerMask, BodyId ignore) const
{
    const Impl& I = *m_impl;
    PosedShape query;
    query.desc = &shape;
    query.position = position;
    query.rotation = Normalize(rotation);
    const AABB box = ShapeBounds(query);
    Array<Manifold> scratch;
    auto test = [&](u32 index) {
        const Body& b = I.bodies[index];
        if (!b.alive || !((layerMask >> b.layer) & 1u) || (ignore.IsValid() && ignore.index == index) || !b.bounds.Intersects(box))
            return true;
        for (u32 s = 0; s < b.shapes.Size(); ++s) {
            scratch.Clear();
            Collide(query, I.Pose(b, s), 0.0f, scratch);
            for (const Manifold& m : scratch)
                for (u32 k = 0; k < m.count; ++k)
                    if (m.points[k].separation <= 0.0f) {
                        out.PushBack(BodyId{index, b.generation});
                        return true;
                    }
        }
        return true;
    };
    I.staticTree.Query(box, test);
    I.movingTree.Query(box, test);
}

bool PhysicsWorld::ShapeCast(const ShapeDesc& shape, Vec3 position, Quat rotation, Vec3 direction, f32 maxDistance, RaycastHit& hit,
                             u32 layerMask, BodyId ignore) const
{
    const Impl& I = *m_impl;
    const Vec3 dir = Normalize(direction);
    rotation = Normalize(rotation);
    PosedShape query;
    query.desc = &shape;
    query.rotation = rotation;
    query.position = position;
    AABB swept = ShapeBounds(query);
    query.position = position + dir * maxDistance;
    swept.Expand(ShapeBounds(query));
    f32 best = maxDistance;
    bool found = false;
    Array<Manifold> scratch;
    // Conservative advancement: the separation the narrowphase reports never exceeds the true distance, so moving
    // the shape forward by it can't pass through anything.
    auto test = [&](u32 index) {
        const Body& b = I.bodies[index];
        if (!b.alive || !((layerMask >> b.layer) & 1u) || (ignore.IsValid() && ignore.index == index) || !b.bounds.Intersects(swept))
            return true;
        for (u32 s = 0; s < b.shapes.Size(); ++s) {
            if (b.shapes[s].isSensor || !b.shapeBounds[s].Intersects(swept))
                continue;
            const PosedShape other = I.Pose(b, s);
            f32 t = 0.0f;
            for (u32 it = 0; it < 48 && t <= best; ++it) {
                query.position = position + dir * t;
                scratch.Clear();
                Collide(query, other, best - t + 0.01f, scratch);
                f32 sep = Infinity;
                Vec3 normal, point;
                for (const Manifold& m : scratch)
                    for (u32 k = 0; k < m.count; ++k)
                        if (m.points[k].separation < sep) {
                            sep = m.points[k].separation;
                            normal = -m.normal; // from the other shape towards the cast shape
                            point = m.points[k].pointB;
                        }
                if (sep == Infinity)
                    break; // nothing within reach
                if (sep < 1e-3f) {
                    // Only a hit when moving into it (sliding along a surface we already touch is not).
                    if (t > 0.0f || Dot(normal, dir) < 0.0f || sep < 0.0f) {
                        if (t < best || !found) {
                            best = t;
                            found = true;
                            hit.body = BodyId{index, b.generation};
                            hit.distance = t;
                            hit.normal = normal;
                            hit.point = point;
                            hit.shape = s;
                        }
                    }
                    break;
                }
                t += sep;
            }
        }
        return true;
    };
    I.staticTree.Query(swept, test);
    I.movingTree.Query(swept, test);
    return found;
}

void PhysicsWorld::TakeEvents(Array<ContactEvent>& out)
{
    out.Append(m_impl->events.View());
    m_impl->events.Clear();
}

void PhysicsWorld::GetDebugContacts(Array<DebugContact>& out) const
{
    for (const Contact& c : m_impl->contacts)
        for (u32 k = 0; k < c.manifold.count; ++k)
            out.PushBack(DebugContact{(c.manifold.points[k].pointA + c.manifold.points[k].pointB) * 0.5f, c.manifold.normal,
                                      c.manifold.points[k].separation});
}

void PhysicsWorld::GetBodies(Array<BodyId>& out) const
{
    for (u32 i = 0; i < m_impl->bodies.Size(); ++i)
        if (m_impl->bodies[i].alive)
            out.PushBack(BodyId{i, m_impl->bodies[i].generation});
}

} // namespace zx::physics
