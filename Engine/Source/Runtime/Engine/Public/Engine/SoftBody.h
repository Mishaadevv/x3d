#pragma once

// Soft-body vehicles: node-beam physics in the spirit of BeamNG.drive. The car body is a lattice of mass nodes joined
// by beams - stiff springs that dent for good when overloaded and tear when ripped apart - so a crash crumples the car
// where it hits and the damage stays. Four wheels hang on raycast suspension from the nodes around them: a bent
// chassis bends the wheels with it.
//
//   SoftBody on a top-level entity -> SoftBodySystem::Update builds the body mesh (skinned to the nodes) and keeps the
//   entity's Mesh Renderer on it; children named "Wheel FL", "Wheel FR", "Wheel RL", "Wheel RR" are the wheel visuals
//   (placed by the system). An Audio Source on the entity becomes the engine sound (pitch follows the revs).
//   SoftBodySystem::Tick (while playing): XPBD substeps - gravity and wheel forces, beams (plastic deformation,
//   tearing), node contacts against the physics world (rigid bodies get pushed).
//   Scripts: entity.carInput(throttle, steer, brake[, handbrake]), carSpeed(), carDamage(), carImpact(), carReset().

#include "Asset/AssetManager.h"
#include "Core/Containers/HashMap.h"
#include "Core/Reflection/Reflection.h"
#include "Scene/ECS/World.h"

namespace zx {

namespace render {
class RenderAssets;
}
class PhysicsSystem;

enum class VehicleDrive : u8
{
    Rear,
    Front,
    All,
};
ZX_DECLARE_ENUM(VehicleDrive);

struct SoftBody
{
    f32 mass = 1300.0f;           // kg
    f32 stiffness = 1.0f;         // body beams (1 = a steel car body; lower = softer, wobblier)
    f32 deformation = 1.0f;       // how easily the body dents for good (0 = never)
    f32 breakStrain = 0.45f;      // beams tear when stretched or squeezed by this fraction
    f32 enginePower = 150.0f;     // kW
    f32 maxDriveForce = 9000.0f;  // N at the tires at low speed
    f32 brakeForce = 15000.0f;    // N
    f32 grip = 1.15f;             // tire friction coefficient
    f32 suspensionStiffness = 42000.0f; // N/m per wheel
    f32 suspensionDamping = 3800.0f;    // N*s/m per wheel
    f32 suspensionLength = 0.3f;        // m
    f32 maxSteer = 0.56f;               // radians
    VehicleDrive drive = VehicleDrive::Rear;
    i32 substeps = 12; // per frame (more = stiffer and more stable, slower)

    // Runtime (not saved): skinning matrices of the body mesh this frame (empty = rest shape).
    Array<Mat4> palette;
};
ZX_DECLARE_TYPE(SoftBody);

/// Car controls (scripts / C++); applied on the next Tick.
struct VehicleInput
{
    f32 throttle = 0.0f; // -1 (reverse) .. 1
    f32 steer = 0.0f;    // -1 (left) .. 1 (right)
    f32 brake = 0.0f;    // 0..1
    bool handbrake = false;
};

/// The body mesh all soft-body cars share (registered by SoftBodySystem::Update).
asset::AssetId SoftBodyCarMeshId();
/// Material slots of that mesh.
enum class SoftBodyCarSlot : u32
{
    Paint,
    Glass,
    Trim,
    Headlight,
    Taillight,
    Count,
};
/// Rest position of a wheel center (entity space): 0 FL, 1 FR, 2 RL, 3 RR; its radius and width.
Vec3 SoftBodyWheelPosition(u32 wheel);
inline constexpr f32 SoftBodyWheelRadius = 0.34f;
inline constexpr f32 SoftBodyWheelWidth = 0.24f;
inline constexpr const char* SoftBodyWheelNames[4] = {"Wheel FL", "Wheel FR", "Wheel RL", "Wheel RR"};

class SoftBodySystem
{
public:
    /// Every frame (editing and playing): registers the body mesh, keeps Mesh Renderers on it, places the wheels at
    /// rest while not simulating.
    void Update(ecs::World& world, asset::AssetManager& assets, render::RenderAssets* renderAssets);
    /// Simulates every soft body (while playing). `physics` supplies the world to collide with (may be null: no
    /// contacts, no wheels).
    void Tick(ecs::World& world, f32 dt, PhysicsSystem* physics);
    /// Drops all simulation state (Play start / stop): bodies restart from their entities.
    void Clear(ecs::World* world = nullptr);

    void SetInput(ecs::Entity e, const VehicleInput& input);
    /// Forward speed (m/s, negative when reversing), damage 0..1, strongest impact speed since the last call (m/s).
    f32 Speed(ecs::Entity e) const;
    f32 Damage(ecs::Entity e) const;
    f32 TakeImpact(ecs::Entity e);
    /// Repairs the car and puts it back on its wheels where it is (1 m up, same heading).
    void Reset(ecs::Entity e);
    u32 BodyCount() const { return u32(m_bodies.Size()); }
    /// Debug / tests: drives every car with these controls instead of their scripts (console car.drive).
    bool overrideInput = false;
    VehicleInput forcedInput;
    u32 BrokenBeams(ecs::Entity e) const;

private:
    struct Node
    {
        Vec3 p, prev, v;
        Vec3 rest; // entity space
        f32 invMass = 0.0f;
        // Contact plane found this frame (point, normal, entity it belongs to).
        Vec3 planePoint, planeNormal;
        ecs::Entity planeEntity;
        bool hasPlane = false;
        bool dynamicPlane = false;
        Vec3 pushed; // impulse given to a rigid body this frame
    };
    struct Beam
    {
        u16 a, b;
        f32 rest, original;
        bool broken = false;
    };
    struct Wheel
    {
        u16 nodes[4];
        f32 weights[4];
        Vec3 offset; // entity space: mount minus the weighted rest nodes
        bool front = false;
        f32 compression = 0.0f, spin = 0.0f, spinRate = 0.0f;
        f32 distance = 0.0f; // ray hit distance (or -1)
        Vec3 center;         // world
        ecs::Entity visual;
    };
    struct Body
    {
        Array<Node> nodes;
        Array<Beam> beams;
        Wheel wheels[4];
        Vec3 restCenter;
        Quat rotation;
        Vec3 position; // entity origin
        VehicleInput input;
        f32 steer = 0.0f;
        f32 damage = 0.0f, impact = 0.0f, revs = 0.0f;
        bool resetRequested = false;
        bool alive = false;
    };
    void Init(ecs::World& world, ecs::Entity e, const SoftBody& sb, Body& b, bool keepHeading);
    void Simulate(ecs::World& world, ecs::Entity e, SoftBody& sb, Body& b, f32 dt, PhysicsSystem* physics);
    void UpdateFrame(Body& b, u32 iterations);

    HashMap<u64, Body> m_bodies;
    bool m_meshRegistered = false;
};

} // namespace zx
