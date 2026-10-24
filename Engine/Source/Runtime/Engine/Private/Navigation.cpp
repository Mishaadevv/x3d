#include "Engine/Navigation.h"
#include "Core/FileSystem/FileSystem.h"
#include "Core/Serialization/Binary.h"
#include "Core/Time/Time.h"
#include "Engine/Gameplay.h"
#include "Engine/PhysicsSystem.h"
#include "Engine/SceneRendering.h"
#include "Engine/Terrain.h"
#include "Engine/WorldPartition.h"
#include "Render/RenderAssets.h"
#include "Scene/Hierarchy.h"

#include <cmath>

namespace zx {

ZX_REFLECT_STRUCT_BEGIN(NavMeshBounds)
    ZX_FIELD(size, Meta::Units("m"), Meta::Tooltip("The box (centered on the entity) where the navigation mesh is built"))
    ZX_FIELD(agentRadius, Meta::Range(0.05f, 5.0f), Meta::Units("m"), Meta::Tooltip("Agents keep this far from walls and edges"))
    ZX_FIELD(agentHeight, Meta::Range(0.2f, 10.0f), Meta::Units("m"), Meta::Tooltip("Agents need this much room above the floor"))
    ZX_FIELD(agentClimb, Meta::Range(0.0f, 2.0f), Meta::Units("m"), Meta::Tooltip("Highest step an agent walks up"))
    ZX_FIELD(maxSlope, Meta::Range(0.0f, 89.0f), Meta::Units("deg"), Meta::Tooltip("Steeper ground is not walkable"))
    ZX_FIELD(cellSize, Meta::Range(0.05f, 2.0f), Meta::Units("m"), Meta::Tooltip("Voxel size: smaller = more detail, slower builds"))
ZX_REFLECT_STRUCT_END()

ZX_REFLECT_STRUCT_BEGIN(NavMeshAgent)
    ZX_FIELD(speed, Meta::Range(0.0f, 50.0f), Meta::Units("m/s"))
    ZX_FIELD(acceleration, Meta::Range(0.1f, 200.0f), Meta::Units("m/s2"))
    ZX_FIELD(turnSpeed, Meta::Range(0.0f, 3600.0f), Meta::Units("deg/s"), Meta::Tooltip("How fast the agent turns to face where it walks (0 = never)"))
    ZX_FIELD(stoppingDistance, Meta::Range(0.0f, 10.0f), Meta::Units("m"), Meta::Tooltip("Stops this close to the destination"))
ZX_REFLECT_STRUCT_END()

const char* NavAgentStatusName(NavAgentStatus s)
{
    switch (s) {
    case NavAgentStatus::Idle: return "idle";
    case NavAgentStatus::Moving: return "moving";
    case NavAgentStatus::Arrived: return "arrived";
    case NavAgentStatus::Partial: return "partial";
    case NavAgentStatus::NoMesh: return "nomesh";
    }
    return "idle";
}

namespace {

AABB VolumeBounds(const NavMeshBounds& b, const scene::WorldTransform& wt)
{
    const Vec3 c = wt.GetPosition();
    return AABB::FromCenterExtents(c, Max(b.size, Vec3(0.5f)) * 0.5f);
}

bool Overlaps(const AABB& a, const AABB& b)
{
    return a.min.x <= b.max.x && a.max.x >= b.min.x && a.min.y <= b.max.y && a.max.y >= b.min.y && a.min.z <= b.max.z && a.max.z >= b.min.z;
}

AABB TransformBounds(const AABB& local, const Mat4& m)
{
    AABB out;
    for (u32 i = 0; i < 8; ++i)
        out.Expand(m.TransformPoint(Vec3(i & 1 ? local.max.x : local.min.x, i & 2 ? local.max.y : local.min.y, i & 4 ? local.max.z : local.min.z)));
    return out;
}

/// Entities whose geometry moves (or is the agent itself) are not part of the static world.
bool IsDynamic(const ecs::World& world, ecs::Entity e)
{
    for (ecs::Entity x = e; world.IsAlive(x);) {
        if (const RigidBody* rb = world.Get<const RigidBody>(x); rb && rb->type == RigidBodyType::Dynamic)
            return true;
        if (world.Has<NavMeshAgent>(x) || world.Has<CharacterController>(x) || world.Has<HlodProxy>(x))
            return true;
        const scene::Parent* p = world.Get<const scene::Parent>(x);
        if (!p)
            break;
        x = p->entity;
    }
    return false;
}

constexpr u32 ZNavMagic = 0x56414E5A; // "ZNAV" (file of volumes)
constexpr u32 ZNavFileVersion = 1;

} // namespace

void GatherNavGeometry(ecs::World& world, render::RenderAssets* assets, const AABB& bounds, Array<Vec3>& vertices, Array<u32>& indices)
{
    const AABB grown(bounds.min - Vec3(1.0f), bounds.max + Vec3(1.0f));
    // Terrains: their height fields within the bounds (finer than the drawn chunk LODs, no skirts).
    world.Query<const Terrain, const scene::WorldTransform>().Each([&](ecs::Entity, const Terrain& t, const scene::WorldTransform& wt) {
        if (!t.collision)
            return;
        const u32 n = TerrainSamples(t);
        const f32 sp = TerrainSpacing(t);
        const f32 half = t.size * 0.5f;
        const Vec3 o = wt.GetPosition();
        // Sample range inside the bounds (the terrain is axis-aligned at the entity's position).
        const i32 x0 = Clamp(i32(std::floor((grown.min.x - o.x + half) / sp)), 0, i32(n) - 1);
        const i32 x1 = Clamp(i32(std::ceil((grown.max.x - o.x + half) / sp)), 0, i32(n) - 1);
        const i32 z0 = Clamp(i32(std::floor((grown.min.z - o.z + half) / sp)), 0, i32(n) - 1);
        const i32 z1 = Clamp(i32(std::ceil((grown.max.z - o.z + half) / sp)), 0, i32(n) - 1);
        if (x1 <= x0 || z1 <= z0)
            return;
        Array<f32> heights;
        ComputeTerrainHeights(t, heights);
        const u32 base = u32(vertices.Size());
        const u32 w = u32(x1 - x0 + 1);
        for (i32 z = z0; z <= z1; ++z)
            for (i32 x = x0; x <= x1; ++x)
                vertices.PushBack(wt.matrix.TransformPoint(Vec3(f32(x) * sp - half, heights[usize(z) * n + usize(x)], f32(z) * sp - half)));
        for (u32 z = 0; z + 1 < u32(z1 - z0 + 1); ++z)
            for (u32 x = 0; x + 1 < w; ++x) {
                const u32 a = base + z * w + x, b = a + 1, c = a + w, d = c + 1;
                // Up-facing triangles: Cross(b - a, c - a) points up for (a, c, b) with +z rows.
                indices.PushBack(a);
                indices.PushBack(c);
                indices.PushBack(b);
                indices.PushBack(b);
                indices.PushBack(c);
                indices.PushBack(d);
            }
    });
    if (!assets)
        return;
    world.Query<const MeshRenderer, const scene::WorldTransform>().Each([&](ecs::Entity e, const MeshRenderer& mr, const scene::WorldTransform& wt) {
        if (!mr.visible || !mr.collision || world.Has<Terrain>(e) || IsDynamic(world, e))
            return;
        const render::GpuMesh* mesh = assets->GetMesh(mr.mesh);
        if (!mesh || mesh->cpuIndices.Empty() || !Overlaps(TransformBounds(mesh->bounds, wt.matrix), grown))
            return;
        const bool flip = Determinant(wt.matrix) < 0.0f; // mirrored: the winding turns around
        const u32 base = u32(vertices.Size());
        for (Vec3 v : mesh->cpuPositions)
            vertices.PushBack(wt.matrix.TransformPoint(v));
        for (usize i = 0; i + 2 < mesh->cpuIndices.Size(); i += 3) {
            indices.PushBack(base + mesh->cpuIndices[i]);
            indices.PushBack(base + mesh->cpuIndices[flip ? i + 2 : i + 1]);
            indices.PushBack(base + mesh->cpuIndices[flip ? i + 1 : i + 2]);
        }
    });
}

bool NavigationSystem::Build(ecs::World& world, render::RenderAssets* assets, NavBuildReport* report)
{
    Stopwatch timer;
    m_volumes.Clear();
    scene::UpdateWorldTransforms(world);
    NavBuildReport r;
    struct Pending
    {
        AABB bounds;
        NavMeshBounds settings;
    };
    Array<Pending> volumes;
    world.Query<const NavMeshBounds, const scene::WorldTransform>().Each(
        [&](ecs::Entity, const NavMeshBounds& b, const scene::WorldTransform& wt) { volumes.PushBack({VolumeBounds(b, wt), b}); });
    for (const Pending& pending : volumes) {
        const AABB& bounds = pending.bounds;
        const NavMeshBounds& b = pending.settings;
        Array<Vec3> vertices;
        Array<u32> indices;
        GatherNavGeometry(world, assets, bounds, vertices, indices);
        nav::BuildSettings s;
        s.bounds = bounds;
        s.cellSize = Max(b.cellSize, 0.05f);
        s.cellHeight = Clamp(s.cellSize * 0.67f, 0.05f, 0.5f);
        s.agentRadius = b.agentRadius;
        s.agentHeight = b.agentHeight;
        s.agentClimb = b.agentClimb;
        s.maxSlope = b.maxSlope;
        Volume v;
        v.bounds = bounds;
        r.triangles += u32(indices.Size() / 3);
        if (!v.mesh.Build(vertices.View(), indices.View(), s))
            continue;
        r.polys += u32(v.mesh.Polys().Size());
        m_volumes.PushBack(move(v));
    }
    r.volumes = u32(m_volumes.Size());
    r.milliseconds = timer.ElapsedMilliseconds();
    if (report)
        *report = r;
    return !m_volumes.Empty();
}

bool NavigationSystem::Save(StringView path) const
{
    Array<u8> bytes;
    BinaryWriter w(bytes);
    w.Write(ZNavMagic);
    w.Write(ZNavFileVersion);
    w.Write(u32(m_volumes.Size()));
    for (const Volume& v : m_volumes) {
        w.Write(v.bounds);
        Array<u8> mesh;
        v.mesh.Serialize(mesh);
        w.WriteArray(Span<const u8>(mesh.View()));
    }
    return fs::WriteFile(path, bytes.View());
}

bool NavigationSystem::Load(StringView path)
{
    m_volumes.Clear();
    Array<u8> bytes;
    if (!fs::ReadFile(path, bytes))
        return false;
    BinaryReader r(bytes.View());
    if (r.Read<u32>() != ZNavMagic || r.Read<u32>() != ZNavFileVersion)
        return false;
    const u32 count = r.Read<u32>();
    for (u32 i = 0; i < count && !r.HasError(); ++i) {
        Volume v;
        r.Read(v.bounds);
        Array<u8> mesh;
        if (!r.ReadArray(mesh) || !v.mesh.Deserialize(mesh.View())) {
            m_volumes.Clear();
            return false;
        }
        m_volumes.PushBack(move(v));
    }
    if (r.HasError())
        m_volumes.Clear();
    return !m_volumes.Empty();
}

const nav::NavMesh* NavigationSystem::MeshAt(Vec3 p) const
{
    const nav::NavMesh* best = nullptr;
    for (const Volume& v : m_volumes) {
        if (p.x >= v.bounds.min.x && p.x <= v.bounds.max.x && p.z >= v.bounds.min.z && p.z <= v.bounds.max.z &&
            p.y >= v.bounds.min.y - 2.0f && p.y <= v.bounds.max.y + 2.0f)
            return &v.mesh;
        if (!best && v.mesh.FindNearestPoly(p, 3.0f) >= 0)
            best = &v.mesh;
    }
    return best;
}

bool NavigationSystem::FindPath(Vec3 start, Vec3 end, Array<Vec3>& path, bool* partial) const
{
    path.Clear();
    const nav::NavMesh* mesh = MeshAt(start);
    return mesh && mesh->FindPath(start, end, path, partial);
}

bool NavigationSystem::NearestPoint(Vec3 p, f32 searchDistance, Vec3& onMesh) const
{
    const nav::NavMesh* mesh = MeshAt(p);
    return mesh && mesh->FindNearestPoly(p, searchDistance, &onMesh) >= 0;
}

bool NavigationSystem::SetDestination(ecs::World& world, ecs::Entity e, Vec3 target)
{
    NavMeshAgent* a = world.IsAlive(e) ? world.Get<NavMeshAgent>(e) : nullptr;
    if (!a)
        return false;
    a->destination = target;
    a->hasDestination = true;
    a->repath = true;
    a->status = NavAgentStatus::Moving;
    return true;
}

void NavigationSystem::Stop(ecs::World& world, ecs::Entity e)
{
    if (NavMeshAgent* a = world.IsAlive(e) ? world.Get<NavMeshAgent>(e) : nullptr) {
        a->hasDestination = false;
        a->path.Clear();
        a->status = NavAgentStatus::Idle;
    }
}

f32 NavigationSystem::RemainingDistance(const ecs::World& world, ecs::Entity e)
{
    const NavMeshAgent* a = world.IsAlive(e) ? world.Get<const NavMeshAgent>(e) : nullptr;
    const scene::WorldTransform* wt = world.IsAlive(e) ? world.Get<const scene::WorldTransform>(e) : nullptr;
    if (!a || !wt || !a->hasDestination || a->corner >= a->path.Size())
        return 0.0f;
    f32 d = Distance(wt->GetPosition(), a->path[a->corner]);
    for (usize i = a->corner + 1; i < a->path.Size(); ++i)
        d += Distance(a->path[i - 1], a->path[i]);
    return d;
}

void NavigationSystem::Tick(ecs::World& world, f32 dt)
{
    if (dt <= 0.0f)
        return;
    world.Query<NavMeshAgent, Transform, const scene::WorldTransform>().Each(
        [&](ecs::Entity e, NavMeshAgent& a, Transform& t, const scene::WorldTransform& wt) {
            const Vec3 pos = wt.GetPosition();
            CharacterState* character = world.Has<CharacterController>(e) ? world.Get<CharacterState>(e) : nullptr;
            if (!a.hasDestination) {
                a.velocity = Vec3();
                return;
            }
            if (a.repath) {
                a.repath = false;
                bool partial = false;
                a.corner = 1;
                if (!FindPath(pos, a.destination, a.path, &partial) && !partial) {
                    a.path.Clear();
                    a.status = NavAgentStatus::NoMesh;
                    a.hasDestination = false;
                    return;
                }
                a.status = partial ? NavAgentStatus::Partial : NavAgentStatus::Moving;
            }
            // Next corner: skip the ones already reached (horizontally).
            auto flatDistance = [](Vec3 p, Vec3 q) { return Length(Vec2(q.x - p.x, q.z - p.z)); };
            while (a.corner + 1 < a.path.Size() && flatDistance(pos, a.path[a.corner]) < Max(0.15f, a.speed * dt))
                ++a.corner;
            const bool last = a.corner + 1 >= a.path.Size();
            const Vec3 target = a.corner < a.path.Size() ? a.path[a.corner] : pos;
            const f32 dist = flatDistance(pos, target);
            if (last && dist <= Max(a.stoppingDistance, 0.02f)) {
                a.hasDestination = false;
                a.velocity = Vec3();
                if (a.status != NavAgentStatus::Partial)
                    a.status = NavAgentStatus::Arrived;
                return;
            }
            // Desired velocity: full speed, slowing down to stop at the destination.
            Vec3 dir(target.x - pos.x, 0.0f, target.z - pos.z);
            dir = dist > 1e-5f ? dir / dist : Vec3();
            f32 speed = a.speed;
            if (last) {
                const f32 brake = Max(dist - a.stoppingDistance, 0.0f);
                speed = Min(speed, Sqrt(2.0f * a.acceleration * brake) + 0.1f);
            }
            const Vec3 desired = dir * speed;
            const Vec3 dv = desired - a.velocity;
            const f32 dvLen = Length(dv), maxDv = a.acceleration * dt;
            a.velocity = dvLen > maxDv ? a.velocity + dv * (maxDv / dvLen) : desired;
            // Face the way it walks.
            const f32 flatSpeed = Length(Vec2(a.velocity.x, a.velocity.z));
            if (a.turnSpeed > 0.0f && flatSpeed > 0.05f) {
                const Quat want = Quat::FromAxisAngle(Vec3::Up(), Atan2(a.velocity.x, a.velocity.z));
                const f32 angle = 2.0f * Acos(Min(Abs(Dot(want, t.rotation)), 1.0f));
                const f32 step = DegToRad(a.turnSpeed) * dt;
                t.rotation = angle <= step ? want : Normalize(Slerp(t.rotation, want, step / angle));
            }
            if (character) {
                character->desiredMove = Vec3(a.velocity.x, 0.0f, a.velocity.z);
                return;
            }
            // No character: move the transform, feet on the mesh.
            Vec3 next = pos + Vec3(a.velocity.x, 0.0f, a.velocity.z) * dt;
            if (last && flatDistance(pos, target) < flatSpeed * dt) // don't overshoot the destination
                next = Vec3(target.x, pos.y, target.z);
            Vec3 onMesh;
            if (const nav::NavMesh* mesh = MeshAt(next); mesh && mesh->FindNearestPoly(next, 1.5f, &onMesh) >= 0)
                next.y = onMesh.y;
            const scene::Parent* parent = world.Get<const scene::Parent>(e);
            t.position = parent && world.IsAlive(parent->entity)
                             ? Inverse(scene::ComputeWorldMatrix(world, parent->entity)).TransformPoint(next)
                             : next;
        });
}

} // namespace zx
