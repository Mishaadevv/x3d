#pragma once

// Navigation (M18): where AI characters can walk and how they get there.
//
//   Nav Mesh Bounds (component): a box around the entity's position (`size`, rotation ignored) where a navigation mesh
//     is built from the scene's static geometry - Mesh Renderers with collision on and terrains (their height
//     fields) - for an agent of the given radius / height / step / slope. Dynamic rigid bodies, characters and agents
//     are left out. Several volumes make several meshes (an agent uses the one it stands on).
//     The mesh is built in tiles (`tileSize`): a change rebuilds only the tiles around it.
//   Nav Mesh Obstacle (component): a box (`size`, centered on the entity, its rotation and scale applied) the agents
//     walk around - a door, a car, a pushed crate. In Play, an obstacle that appears, moves or goes away rebuilds the
//     tiles under it (a few times per second at most) and the agents find new paths.
//   Nav Mesh Agent (component): walks the entity to a destination along the shortest path on the mesh (A* + funnel),
//     with acceleration, arrival slow-down and turning. With a Character Controller it steers the character (the
//     physics sweeps it); without one it moves the transform and keeps the feet on the mesh.
//
//   NavigationSystem nav;
//   nav.Build(world, &renderAssets);          // editor: Build > Build Navigation, console nav.build
//   nav.Save(<scene>.znav) / Load(...)          // the game loads the mesh baked next to the scene
//   nav.SetDestination(world, agent, target);  // scripts: entity.navTo(pos), navStop(), navStatus(), navRemaining()
//   nav.Tick(world, dt);                        // TickGame, before the characters move

#include "Core/Containers/Array.h"
#include "Core/Containers/HashMap.h"
#include "Core/Math/Geometry.h"
#include "Core/Reflection/Reflection.h"
#include "Nav/NavMesh.h"
#include "Scene/ECS/World.h"

namespace zx {

namespace render {
class RenderAssets;
}

struct NavMeshBounds
{
    Vec3 size = Vec3(60.0f, 20.0f, 60.0f); // meters, centered on the entity
    f32 agentRadius = 0.4f;
    f32 agentHeight = 1.8f;
    f32 agentClimb = 0.45f; // highest step
    f32 maxSlope = 45.0f;   // degrees
    f32 cellSize = 0.3f;    // meters (smaller = more detail, slower builds)
    f32 tileSize = 32.0f;   // meters per tile (0 = one piece): changes in Play rebuild only their tiles
};
ZX_DECLARE_TYPE(NavMeshBounds);

struct NavMeshObstacle
{
    Vec3 size = Vec3(1.0f, 2.0f, 1.0f); // meters, centered on the entity
    bool enabled = true;
};
ZX_DECLARE_TYPE(NavMeshObstacle);

enum class NavAgentStatus : u8
{
    Idle,        // no destination
    Moving,      // following a path
    Arrived,     // reached the destination
    Partial,     // the destination is unreachable: going as close as the mesh allows
    NoMesh,      // the agent is not on a navigation mesh
};

struct NavMeshAgent
{
    f32 speed = 3.5f;            // m/s
    f32 acceleration = 12.0f;    // m/s^2
    f32 turnSpeed = 540.0f;      // degrees per second (0 = keep the rotation)
    f32 stoppingDistance = 0.25f; // meters from the destination
    // Runtime (not saved).
    Vec3 destination;
    bool hasDestination = false;
    bool repath = false;
    NavAgentStatus status = NavAgentStatus::Idle;
    Array<Vec3> path;
    u32 corner = 0;
    Vec3 velocity;
};
ZX_DECLARE_TYPE(NavMeshAgent);

struct NavBuildReport
{
    u32 volumes = 0;
    u32 triangles = 0;
    u32 polys = 0;
    u32 tiles = 0;
    f64 milliseconds = 0.0;
};

class NavigationSystem
{
public:
    /// Rebuilds a mesh for every Nav Mesh Bounds from the geometry loaded now (meshes still loading are skipped).
    /// `assets` is kept for later rebuilds (RebuildArea, obstacles).
    bool Build(ecs::World& world, render::RenderAssets* assets, NavBuildReport* report = nullptr);
    /// Rebuilds the tiles `area` (world box) touches from the geometry and obstacles there now; agents find new paths.
    /// False when no mesh covers it.
    bool RebuildArea(ecs::World& world, const AABB& area, NavBuildReport* report = nullptr);
    void Clear()
    {
        m_volumes.Clear();
        m_obstacles.Clear();
        m_dirty = AABB();
    }
    bool HasMesh() const { return !m_volumes.Empty(); }

    bool Save(StringView path) const;
    bool Load(StringView path);

    /// The mesh whose volume holds `p` (or the nearest one with a polygon within a few meters).
    const nav::NavMesh* MeshAt(Vec3 p) const;
    bool FindPath(Vec3 start, Vec3 end, Array<Vec3>& path, bool* partial = nullptr) const;
    /// Snaps `p` onto the mesh (within `searchDistance`).
    bool NearestPoint(Vec3 p, f32 searchDistance, Vec3& onMesh) const;

    /// Sends an agent (repaths on the next Tick). False if the entity has no Nav Mesh Agent.
    bool SetDestination(ecs::World& world, ecs::Entity e, Vec3 target);
    void Stop(ecs::World& world, ecs::Entity e);
    /// Path length still to walk (meters), 0 when idle.
    static f32 RemainingDistance(const ecs::World& world, ecs::Entity e);

    void Tick(ecs::World& world, f32 dt);
    /// Tile rebuilds done by moving obstacles / RebuildArea since the last Build (stats).
    u32 RebuildCount() const { return m_rebuilds; }
    render::RenderAssets* renderAssets = nullptr; // meshes for rebuilds (set by Build)

    struct Volume
    {
        AABB bounds;
        nav::NavMesh mesh;
    };
    Span<const Volume> Volumes() const { return m_volumes.View(); }

private:
    /// World boxes of the enabled obstacles now.
    void GatherObstacles(ecs::World& world, Array<AABB>& out, Array<u64>* ids = nullptr) const;
    /// Obstacles that appeared, moved or went away mark their tiles dirty; rebuilt a few times per second.
    void UpdateObstacles(ecs::World& world, f32 dt);
    Array<Volume> m_volumes;
    HashMap<u64, AABB> m_obstacles; // as the mesh has them (entity -> box)
    AABB m_dirty;
    f32 m_sinceRebuild = 0.0f;
    u32 m_rebuilds = 0;
};

/// World box of an obstacle (its size, rotated and scaled with the entity).
AABB NavObstacleBounds(const NavMeshObstacle& o, const Mat4& world);

/// World-space triangles of the static geometry inside `bounds` (what Build voxelizes).
void GatherNavGeometry(ecs::World& world, render::RenderAssets* assets, const AABB& bounds, Array<Vec3>& vertices, Array<u32>& indices);

const char* NavAgentStatusName(NavAgentStatus s);

} // namespace zx
