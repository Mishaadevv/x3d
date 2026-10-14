#pragma once

// Joints as rows of the soft-step solver: each joint turns into scalar velocity constraints ("rows") rebuilt from the
// current poses every substep - the anchor point (3 rows), axis alignment, locked rotation, limits (one-sided rows,
// speculative like contacts), motors (velocity targets with a force cap) and springs (soft rows with their own
// stiffness).

#include "Physics/PhysicsWorld.h"

namespace zx::physics {

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

struct JointData
{
    JointDesc desc;
    u32 generation = 1;
    bool alive = false;
    u32 a = 0, b = ~0u;  // body indices (b = ~0u: the world)
    Vec3 localAnchorA, localAnchorB; // relative to the bodies' centers of mass, body frames (world point for the world)
    Vec3 localAxisA, localAxisB;     // joint axis in each body frame
    Vec3 localRefA, localRefB;       // a direction perpendicular to the axis (angles around it)
    Quat relRotation;                // qB relative to qA at creation (locked rotation)
    f32 restLength = 0.0f;
    static constexpr u32 MaxRows = 18;
    f32 impulses[MaxRows] = {};      // accumulated per row slot (warm start across substeps and steps)
};

struct JointRow
{
    Vec3 linA, angA, linB, angB; // Jacobian
    f32 effMass = 0.0f;
    f32 C = 0.0f;              // position error (0 = satisfied)
    f32 lo = -Infinity, hi = Infinity; // accumulated impulse bounds
    f32 targetVelocity = 0.0f; // motors
    f32 springHertz = 0.0f;    // > 0: soft spring towards C = 0 with its own stiffness
    f32 springDamping = 0.0f;
    bool limit = false;        // one-sided: speculative while C > 0
    bool motor = false;        // velocity target, no position error
    u32 slot = 0;              // index into JointData::impulses
    u32 block = 0;             // 3: this row and the next two are equality rows solved together (blockMass = K^-1)
    Mat3 blockMass;
};

/// Rows of a joint for the current poses (B = the world: invMass 0, x = 0, q = identity).
void BuildJointRows(const JointData& j, const SolverBody& A, const SolverBody& B, Array<JointRow>& rows);

/// Angle around the hinge axis (Hinge), position along the axis (Slider), distance (Distance); 0 for others.
f32 JointCoordinate(const JointData& j, const SolverBody& A, const SolverBody& B);

} // namespace zx::physics
