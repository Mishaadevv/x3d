#include "Engine/SoftBody.h"
#include <cmath>
#include "Asset/Animation.h"
#include "Asset/CookedAssets.h"
#include "Asset/MeshData.h"
#include "Core/Log/Log.h"
#include "Core/Profiler/Profiler.h"
#include "Engine/AudioSystem.h"
#include "Engine/PhysicsSystem.h"
#include "Engine/SceneRendering.h"
#include "Engine/Wind.h"
#include "Render/RenderAssets.h"
#include "Scene/Hierarchy.h"

namespace zx {

ZX_DEFINE_LOG_CATEGORY_STATIC(LogSoftBody, Info);

ZX_REFLECT_ENUM_BEGIN(VehicleDrive)
    ZX_ENUM_VALUE(Rear)
    ZX_ENUM_VALUE(Front)
    ZX_ENUM_VALUE(All)
ZX_REFLECT_ENUM_END()

ZX_REFLECT_STRUCT_BEGIN(SoftBody)
    ZX_FIELD(mass, Meta::Range(300, 5000), Meta::Units("kg"), Meta::Category("Body"))
    ZX_FIELD(stiffness, Meta::Range(0.05, 4), Meta::Tooltip("Beam stiffness of the body (1 = a steel car; lower = softer, wobblier)"))
    ZX_FIELD(deformation, Meta::Range(0, 5), Meta::Tooltip("How easily the body dents for good (0 = it springs back from anything)"))
    ZX_FIELD(breakStrain, Meta::Range(0.05, 2), Meta::Tooltip("Beams tear when stretched or squeezed by this fraction of their length"))
    ZX_FIELD(substeps, Meta::Range(2, 40), Meta::Tooltip("Simulation steps per frame (more = stiffer and steadier, slower)"))
    ZX_FIELD(enginePower, Meta::Range(10, 1000), Meta::Units("kW"), Meta::Category("Drivetrain"))
    ZX_FIELD(maxDriveForce, Meta::Range(500, 40000), Meta::Units("N"), Meta::Tooltip("Traction at the tires at low speed"))
    ZX_FIELD(brakeForce, Meta::Range(500, 60000), Meta::Units("N"))
    ZX_FIELD(drive, Meta::Tooltip("Driven wheels"))
    ZX_FIELD(grip, Meta::Range(0.1, 3), Meta::Category("Wheels"), Meta::Tooltip("Tire friction coefficient"))
    ZX_FIELD(maxSteer, Meta::Angle(), Meta::Range(0, 1))
    ZX_FIELD(suspensionStiffness, Meta::Range(5000, 200000), Meta::Units("N/m"))
    ZX_FIELD(suspensionDamping, Meta::Range(0, 20000), Meta::Units("N*s/m"))
    ZX_FIELD(suspensionLength, Meta::Range(0.05, 1), Meta::Units("m"))
ZX_REFLECT_STRUCT_END()

namespace {

constexpr f32 NodeRadius = 0.11f;
constexpr f32 WheelTrack = 0.78f;  // half track (m)
constexpr f32 WheelBase = 1.32f;   // half wheelbase (m)
constexpr f32 MountHeight = 0.62f; // suspension mount above the ground (rest)
const Guid SoftBodyNamespace(0x5A58536F66744275ull, 0x6479436172763031ull);

/// The node-beam layout of the car (entity space: ground at y = 0, +Z forward, +X right).
struct Rig
{
    Array<Vec3> nodes;
    Array<f32> massWeights; // relative node masses
    Array<UVec2> beams;
    Vec3 mounts[4];
};

/// Built on demand (cheap): a static would outlive the engine allocator's leak check.
Rig CarRig()
{
    Rig r;
    {
        // Lower body: 3 x 7 x 2 nodes (floor and beltline), the engine bay heavier.
        const f32 xs[] = {-0.88f, 0.0f, 0.88f};
        const f32 zs[] = {-2.1f, -1.35f, -0.65f, 0.05f, 0.75f, 1.45f, 2.1f};
        const f32 ys[] = {0.32f, 0.82f};
        for (f32 z : zs)
            for (f32 y : ys)
                for (f32 x : xs) {
                    r.nodes.PushBack(Vec3(x, y, z));
                    r.massWeights.PushBack((y < 0.5f ? 1.3f : 1.0f) * (z > 1.0f ? 1.5f : 1.0f));
                }
        const u32 lower = u32(r.nodes.Size());
        // Cabin roof.
        for (f32 z : {-1.05f, -0.35f, 0.3f, 0.8f})
            for (f32 x : {-0.7f, 0.7f}) {
                r.nodes.PushBack(Vec3(x, 1.36f, z));
                r.massWeights.PushBack(0.5f);
            }
        // Beams: neighbors and diagonals (< 1.32 m), the roof fully braced.
        for (u32 a = 0; a < r.nodes.Size(); ++a)
            for (u32 b = a + 1; b < r.nodes.Size(); ++b)
                if (Length(r.nodes[a] - r.nodes[b]) < 1.32f || (a >= lower && b >= lower))
                    r.beams.PushBack(UVec2(a, b));
        for (u32 w = 0; w < 4; ++w)
            r.mounts[w] = Vec3(w % 2 ? WheelTrack : -WheelTrack, MountHeight, w < 2 ? WheelBase : -WheelBase);
    }
    return r;
}

Vec3 Horizontal(Vec3 v) { return Vec3(v.x, 0.0f, v.z); }
f32 Sqr(f32 x) { return x * x; }

// --- Body mesh ----------------------------------------------------------------------------------------------------

struct CarProfile
{
    f32 halfWidth, bottom, belt, roof, roofHalfWidth;
    bool windshield; // the greenhouse here is glass all over (windshield / rear window)
};

CarProfile ProfileAt(f32 z)
{
    CarProfile p;
    const f32 az = Abs(z);
    p.halfWidth = 0.89f - (az > 1.85f ? 0.16f * Sqr((az - 1.85f) / 0.37f) : 0.0f);
    p.bottom = 0.3f + (az > 1.95f ? 0.06f * (az - 1.95f) / 0.27f : 0.0f);
    // Beltline: the hood dips towards the nose, the trunk lid stays high.
    p.belt = z > 1.2f ? 0.86f - 0.16f * Sqr((z - 1.2f) / 1.02f) : (z < -1.6f ? 0.9f - 0.08f * (-1.6f - z) / 0.62f : 0.88f);
    // Greenhouse: windshield from z 1.25 up to the roof at 0.65, roof to -0.95, rear window down to -1.65.
    f32 roof = p.belt;
    p.windshield = false;
    if (z <= 1.25f && z >= -1.65f) {
        if (z > 0.65f) {
            roof = p.belt + (1.38f - p.belt) * (1.25f - z) / 0.6f;
            p.windshield = true;
        } else if (z < -0.95f) {
            roof = p.belt + (1.36f - p.belt) * (z + 1.65f) / 0.7f;
            p.windshield = true;
        } else {
            roof = 1.38f - 0.04f * Sqr((z + 0.15f) / 0.8f);
        }
    }
    p.roof = Max(roof, p.belt + 0.015f);
    p.roofHalfWidth = 0.7f;
    return p;
}

/// Bottom of the side panel: wheel arches above the wheels.
f32 SideBottom(f32 z, f32 bottom)
{
    f32 y = bottom + 0.06f;
    for (f32 zw : {WheelBase, -WheelBase}) {
        const f32 r = SoftBodyWheelRadius + 0.07f, d = z - zw;
        if (Abs(d) < r)
            y = Max(y, SoftBodyWheelRadius + std::sqrt(r * r - d * d));
    }
    return y;
}

asset::CookedMesh BuildCarMesh()
{
    using asset::MeshVertex;
    const Rig rig = CarRig();
    asset::MeshData mesh;
    mesh.name = "SoftBodyCar";
    Array<u32> indices[u32(SoftBodyCarSlot::Count)];
    auto vertex = [&](Vec3 p, Vec3 n) {
        MeshVertex v;
        v.position = p;
        v.normal = n;
        v.tangent = Vec4(1, 0, 0, 1);
        // Skinned to the four nearest nodes (every node is a bone).
        u32 best[4] = {0, 0, 0, 0};
        f32 bestD[4] = {1e30f, 1e30f, 1e30f, 1e30f};
        for (u32 i = 0; i < rig.nodes.Size(); ++i) {
            const f32 d = LengthSq(rig.nodes[i] - p);
            for (u32 k = 0; k < 4; ++k)
                if (d < bestD[k]) {
                    for (u32 m = 3; m > k; --m) {
                        bestD[m] = bestD[m - 1];
                        best[m] = best[m - 1];
                    }
                    bestD[k] = d;
                    best[k] = i;
                    break;
                }
        }
        f32 w[4];
        for (u32 k = 0; k < 4; ++k)
            w[k] = 1.0f / (bestD[k] + 0.02f);
        asset::anim::PackSkin(best, w, v.joints, v.weights);
        mesh.vertices.PushBack(v);
        return u32(mesh.vertices.Size() - 1);
    };
    // Triangles wound so their normal (b - a) x (c - a) points along `outward`.
    auto triangle = [&](SoftBodyCarSlot slot, u32 a, u32 b, u32 c, Vec3 outward) {
        const Vec3 pa = mesh.vertices[a].position, pb = mesh.vertices[b].position, pc = mesh.vertices[c].position;
        Array<u32>& out = indices[u32(slot)];
        if (Dot(Cross(pb - pa, pc - pa), outward) >= 0.0f)
            out.Append({a, b, c});
        else
            out.Append({a, c, b});
    };

    // Loft: a ring of points per slice along the body.
    constexpr u32 Slices = 46;
    constexpr f32 Front = 2.22f, Back = -2.22f;
    auto ring = [](f32 z, Array<Vec3>& pts) {
        const CarProfile p = ProfileAt(z);
        const f32 side = SideBottom(z, p.bottom);
        // Right half from the bottom center up to the roof center, then mirrored (closed ring).
        const Vec3 half[] = {
            Vec3(0.0f, p.bottom, z),
            Vec3(p.halfWidth - 0.3f, p.bottom, z),
            Vec3(p.halfWidth - 0.28f, Min(side, p.belt - 0.12f), z), // under the arch
            Vec3(p.halfWidth - 0.02f, Min(side, p.belt - 0.1f), z),
            Vec3(p.halfWidth, Min(side + 0.04f, p.belt - 0.08f), z),
            Vec3(p.halfWidth, p.belt - 0.06f, z),
            Vec3(p.halfWidth - 0.03f, p.belt, z),
            Vec3(Min(p.halfWidth - 0.08f, 0.8f), p.belt + 0.02f, z), // greenhouse base
            Vec3(p.roofHalfWidth, p.roof - 0.05f, z),
            Vec3(p.roofHalfWidth - 0.1f, p.roof, z),
            Vec3(0.0f, p.roof + 0.015f, z),
        };
        const u32 n = ZX_ARRAY_COUNT(half);
        pts.Clear();
        for (u32 i = 0; i < n; ++i)
            pts.PushBack(half[i]);
        for (u32 i = n - 2; i >= 1; --i)
            pts.PushBack(Vec3(-half[i].x, half[i].y, z));
    };
    Array<Array<Vec3>> rings;
    rings.Resize(Slices);
    for (u32 s = 0; s < Slices; ++s)
        ring(Back + (Front - Back) * f32(s) / f32(Slices - 1), rings[s]);
    const u32 count = u32(rings[0].Size());
    // Smooth normals across the loft: per ring point from its neighbors.
    Array<u32> ids;
    ids.Resize(Slices * count);
    for (u32 s = 0; s < Slices; ++s)
        for (u32 i = 0; i < count; ++i) {
            const Vec3 p = rings[s][i];
            const Vec3 around = rings[s][(i + 1) % count] - rings[s][(i + count - 1) % count];
            const Vec3 along = rings[Min(s + 1, Slices - 1)][i] - rings[s > 0 ? s - 1 : 0][i];
            Vec3 n = SafeNormalize(Cross(around, along), Vec3(0, 1, 0));
            const Vec3 center(0.0f, 0.7f, p.z);
            if (Dot(n, p - center) < 0.0f)
                n = -n;
            ids[s * count + i] = vertex(p, n);
        }
    for (u32 s = 0; s + 1 < Slices; ++s) {
        const f32 z = (rings[s][0].z + rings[s + 1][0].z) * 0.5f;
        const CarProfile p = ProfileAt(z);
        for (u32 i = 0; i < count; ++i) {
            const u32 j = (i + 1) % count;
            const u32 a = ids[s * count + i], b = ids[s * count + j], c = ids[(s + 1) * count + j], d = ids[(s + 1) * count + i];
            const Vec3 mid = (rings[s][i] + rings[s][j] + rings[s + 1][i] + rings[s + 1][j]) * 0.25f;
            // Which panel: the floor dark, the greenhouse glass (the roof top painted unless it is a window), the rest paint.
            const u32 k = i < count / 2 + 1 ? i : count - i; // index in the right half
            const u32 kj = j < count / 2 + 1 ? j : count - j;
            const u32 lo = Min(k, kj), hi = Max(k, kj);
            SoftBodyCarSlot slot = SoftBodyCarSlot::Paint;
            if (hi <= 2)
                slot = SoftBodyCarSlot::Trim;
            else if (lo == 7 && hi == 8 && p.roof > p.belt + 0.1f)
                slot = SoftBodyCarSlot::Glass;
            else if (lo >= 8 && p.windshield)
                slot = SoftBodyCarSlot::Glass;
            const Vec3 outward = mid - Vec3(0.0f, 0.7f, mid.z);
            triangle(slot, a, b, c, outward);
            triangle(slot, a, c, d, outward);
        }
    }
    // End caps (flat, own vertices): front and back.
    for (u32 end = 0; end < 2; ++end) {
        const Array<Vec3>& r = rings[end == 0 ? 0 : Slices - 1];
        const Vec3 n(0, 0, end == 0 ? -1.0f : 1.0f);
        Vec3 c;
        for (const Vec3& p : r)
            c += p;
        c /= f32(r.Size());
        const u32 center = vertex(c, n);
        Array<u32> rim;
        for (const Vec3& p : r)
            rim.PushBack(vertex(p, n));
        for (u32 i = 0; i < rim.Size(); ++i)
            triangle(SoftBodyCarSlot::Paint, center, rim[i], rim[(i + 1) % rim.Size()], n);
    }
    // Boxes: lights, grille, bumpers.
    auto box = [&](SoftBodyCarSlot slot, Vec3 center, Vec3 size) {
        const Vec3 h = size * 0.5f;
        const Vec3 axes[3] = {Vec3(1, 0, 0), Vec3(0, 1, 0), Vec3(0, 0, 1)};
        for (u32 a = 0; a < 3; ++a)
            for (f32 sign : {-1.0f, 1.0f}) {
                const Vec3 n = axes[a] * sign;
                const Vec3 u = axes[(a + 1) % 3], v = axes[(a + 2) % 3];
                const f32 hu = Dot(h, u), hv = Dot(h, v);
                const Vec3 c = center + n * Dot(h, axes[a]);
                const u32 q0 = vertex(c - u * hu - v * hv, n), q1 = vertex(c + u * hu - v * hv, n);
                const u32 q2 = vertex(c + u * hu + v * hv, n), q3 = vertex(c - u * hu + v * hv, n);
                triangle(slot, q0, q1, q2, n);
                triangle(slot, q0, q2, q3, n);
            }
    };
    for (f32 x : {-0.56f, 0.56f}) {
        box(SoftBodyCarSlot::Headlight, Vec3(x, 0.66f, 2.2f), Vec3(0.34f, 0.11f, 0.06f));
        box(SoftBodyCarSlot::Taillight, Vec3(x * 1.12f, 0.78f, -2.21f), Vec3(0.3f, 0.1f, 0.05f));
    }
    box(SoftBodyCarSlot::Trim, Vec3(0.0f, 0.52f, 2.21f), Vec3(0.86f, 0.16f, 0.05f)); // grille
    box(SoftBodyCarSlot::Trim, Vec3(0.0f, 0.38f, 2.22f), Vec3(1.62f, 0.13f, 0.12f)); // bumpers
    box(SoftBodyCarSlot::Trim, Vec3(0.0f, 0.4f, -2.22f), Vec3(1.62f, 0.13f, 0.12f));
    for (f32 x : {-1.0f, 1.0f}) // side mirrors
        box(SoftBodyCarSlot::Trim, Vec3(x * 0.93f, 1.0f, 1.12f), Vec3(0.12f, 0.09f, 0.16f));

    asset::MeshLod& lod = mesh.lods.AddDefaulted();
    for (u32 slot = 0; slot < u32(SoftBodyCarSlot::Count); ++slot) {
        asset::MeshSection section;
        section.indexOffset = u32(lod.indices.Size());
        section.indexCount = u32(indices[slot].Size());
        section.materialIndex = slot;
        lod.indices.Append(indices[slot].View());
        if (section.indexCount)
            lod.sections.PushBack(section);
    }
    mesh.attributes = asset::MeshAttributes::Normals | asset::MeshAttributes::Skin;
    mesh.ComputeBounds();
    asset::CookedMesh cooked = asset::CookMesh(mesh, false);
    // Every slot keeps its index even if unused (materials map 1:1 to SoftBodyCarSlot).
    cooked.materialSlotCount = u32(SoftBodyCarSlot::Count);
    cooked.slotSourceMaterial.Clear();
    for (u32 slot = 0; slot < u32(SoftBodyCarSlot::Count); ++slot)
        cooked.slotSourceMaterial.PushBack(slot);
    for (asset::CookedMeshSection& s : cooked.sections) // CookMesh numbers slots by first use: map them back
        s.materialSlot = mesh.lods[0].sections[usize(&s - cooked.sections.Data())].materialIndex;
    cooked.boneCount = u32(rig.nodes.Size());
    return cooked;
}

/// Rotation part of a deformation (Mueller et al. 2016): iterates `q` towards the rotation of A (columns).
void ExtractRotation(const Vec3 a[3], Quat& q, u32 iterations)
{
    for (u32 it = 0; it < iterations; ++it) {
        const Vec3 r[3] = {q * Vec3(1, 0, 0), q * Vec3(0, 1, 0), q * Vec3(0, 0, 1)};
        Vec3 omega = Cross(r[0], a[0]) + Cross(r[1], a[1]) + Cross(r[2], a[2]);
        omega = omega * (1.0f / (Abs(Dot(r[0], a[0]) + Dot(r[1], a[1]) + Dot(r[2], a[2])) + 1e-9f));
        const f32 w = Length(omega);
        if (w < 1e-9f)
            break;
        q = Normalize(Quat::FromAxisAngle(omega / w, w) * q);
    }
}

/// +1 when a positive rotation about +X moves the top of a wheel forward (+Z): the sign for rolling forward.
f32 RollSign()
{
    static const f32 sign = (Quat::FromAxisAngle(Vec3(1, 0, 0), 0.1f) * Vec3(0, 1, 0)).z > 0.0f ? 1.0f : -1.0f;
    return sign;
}

} // namespace

asset::AssetId SoftBodyCarMeshId() { return Guid::FromName(SoftBodyNamespace, "SoftBodyCar/1"); }

Vec3 SoftBodyWheelPosition(u32 wheel)
{
    return Vec3(wheel % 2 ? WheelTrack : -WheelTrack, SoftBodyWheelRadius, wheel < 2 ? WheelBase : -WheelBase);
}

// --- System ---------------------------------------------------------------------------------------------------------

void SoftBodySystem::Update(ecs::World& world, asset::AssetManager& assets, render::RenderAssets* renderAssets)
{
    Array<ecs::Entity> bodies;
    world.Query<const SoftBody>().Each([&](ecs::Entity e, const SoftBody&) { bodies.PushBack(e); });
    if (bodies.Empty())
        return;
    const asset::AssetId mesh = SoftBodyCarMeshId();
    if (!m_meshRegistered) {
        assets.AddMemoryMesh(mesh, BuildCarMesh());
        if (renderAssets)
            renderAssets->ReloadMesh(mesh);
        m_meshRegistered = true;
    }
    for (ecs::Entity e : bodies) {
        MeshRenderer* mr = world.Get<MeshRenderer>(e);
        if (!mr) {
            world.Add(e, MeshRenderer{});
            mr = world.Get<MeshRenderer>(e);
        }
        if (mr->mesh != mesh)
            mr->mesh = mesh;
        if (mr->collision)
            mr->collision = false; // the nodes collide, not the mesh
        // Wheel visuals never collide (the suspension rays would hit them).
        for (ecs::Entity child : scene::GetChildren(world, e)) {
            const scene::EntityName* name = world.Get<const scene::EntityName>(child);
            if (!name || !name->value.View().StartsWith("Wheel "))
                continue;
            Array<ecs::Entity> parts;
            parts.PushBack(child);
            for (usize i = 0; i < parts.Size(); ++i) {
                if (MeshRenderer* w = world.Get<MeshRenderer>(parts[i]); w && w->collision)
                    w->collision = false;
                parts.Append(scene::GetChildren(world, parts[i]));
            }
            // At rest while not simulating.
            if (!m_bodies.Find(e.Pack()))
                for (u32 w = 0; w < 4; ++w)
                    if (name->value.View() == SoftBodyWheelNames[w])
                        if (Transform* t = world.Get<Transform>(child)) {
                            const Vec3 p = SoftBodyWheelPosition(w);
                            if (LengthSq(t->position - p) > 1e-8f || t->rotation != Quat())
                                *t = Transform(p);
                        }
        }
        if (!m_bodies.Find(e.Pack()))
            if (SoftBody* sb = world.Get<SoftBody>(e); sb && !sb->palette.Empty())
                sb->palette.Clear();
    }
}

void SoftBodySystem::Clear(ecs::World* world)
{
    m_bodies.Clear();
    if (world)
        world->Query<SoftBody>().Each([](ecs::Entity, SoftBody& sb) { sb.palette.Clear(); });
}

void SoftBodySystem::Init(ecs::World& world, ecs::Entity e, const SoftBody& sb, Body& b, bool keepHeading)
{
    const Rig rig = CarRig();
    Mat4 m = scene::ComputeWorldMatrix(world, e);
    if (keepHeading) { // upright, same heading, a meter up
        const Vec3 f = SafeNormalize(Horizontal(b.rotation * Vec3(0, 0, 1)), Vec3(0, 0, 1));
        m = Transform(b.position + Vec3(0, 1.0f, 0), Quat::LookRotation(f, Vec3(0, 1, 0))).ToMatrix();
    }
    f32 weightSum = 0.0f;
    for (f32 w : rig.massWeights)
        weightSum += w;
    b.nodes.Clear();
    b.nodes.Resize(rig.nodes.Size());
    Vec3 center;
    f32 total = 0.0f;
    for (usize i = 0; i < rig.nodes.Size(); ++i) {
        Node& n = b.nodes[i];
        n.rest = rig.nodes[i];
        n.p = n.prev = m.TransformPoint(n.rest);
        n.v = Vec3();
        const f32 mass = Max(sb.mass, 50.0f) * rig.massWeights[i] / weightSum;
        n.invMass = 1.0f / mass;
        center += n.rest * mass;
        total += mass;
    }
    b.restCenter = center / total;
    b.beams.Clear();
    for (const UVec2& beam : rig.beams) {
        const f32 len = Length(rig.nodes[beam.x] - rig.nodes[beam.y]);
        b.beams.PushBack(Beam{u16(beam.x), u16(beam.y), len, len, false});
    }
    // Wheels: mounted on the four nearest nodes (inverse squared distance weights).
    for (u32 w = 0; w < 4; ++w) {
        Wheel& wh = b.wheels[w];
        const Vec3 mount = rig.mounts[w];
        f32 bestD[4] = {1e30f, 1e30f, 1e30f, 1e30f};
        u16 best[4] = {0, 0, 0, 0};
        for (u32 i = 0; i < rig.nodes.Size(); ++i) {
            const f32 d = LengthSq(rig.nodes[i] - mount);
            for (u32 k = 0; k < 4; ++k)
                if (d < bestD[k]) {
                    for (u32 j = 3; j > k; --j) {
                        bestD[j] = bestD[j - 1];
                        best[j] = best[j - 1];
                    }
                    bestD[k] = d;
                    best[k] = u16(i);
                    break;
                }
        }
        f32 sum = 0.0f;
        Vec3 weighted;
        for (u32 k = 0; k < 4; ++k) {
            wh.nodes[k] = best[k];
            wh.weights[k] = 1.0f / (bestD[k] + 1e-3f);
            sum += wh.weights[k];
        }
        for (u32 k = 0; k < 4; ++k) {
            wh.weights[k] /= sum;
            weighted += rig.nodes[best[k]] * wh.weights[k];
        }
        wh.offset = mount - weighted;
        wh.front = w < 2;
        wh.compression = 0.0f;
        wh.spinRate = 0.0f;
        wh.distance = -1.0f;
        wh.visual = {};
        for (ecs::Entity child : scene::GetChildren(world, e))
            if (const scene::EntityName* name = world.Get<const scene::EntityName>(child); name && name->value.View() == SoftBodyWheelNames[w])
                wh.visual = child;
    }
    b.rotation = Quat::LookRotation(SafeNormalize(m.TransformVector(Vec3(0, 0, 1)), Vec3(0, 0, 1)), SafeNormalize(m.TransformVector(Vec3(0, 1, 0)), Vec3(0, 1, 0)));
    b.position = m.GetTranslation();
    b.damage = b.impact = 0.0f;
    b.steer = 0.0f;
    UpdateFrame(b, 8);
}

void SoftBodySystem::UpdateFrame(Body& b, u32 iterations)
{
    Vec3 c;
    f32 total = 0.0f;
    for (const Node& n : b.nodes) {
        const f32 m = 1.0f / n.invMass;
        c += n.p * m;
        total += m;
    }
    c /= total;
    Vec3 a[3];
    for (const Node& n : b.nodes) {
        const f32 m = 1.0f / n.invMass;
        const Vec3 d = (n.p - c) * m, r = n.rest - b.restCenter;
        a[0] += d * r.x;
        a[1] += d * r.y;
        a[2] += d * r.z;
    }
    ExtractRotation(a, b.rotation, iterations);
    b.position = c - b.rotation * b.restCenter;
}

void SoftBodySystem::Tick(ecs::World& world, f32 dt, PhysicsSystem* physics)
{
    ZX_PROFILE_SCOPE("SoftBodySystem::Tick");
    dt = Clamp(dt, 0.0f, 1.0f / 30.0f); // a long frame is slowed down rather than taken in one unstable step
    for (auto& [key, b] : m_bodies)
        b.alive = false;
    Array<ecs::Entity> entities;
    world.Query<const SoftBody>().Each([&](ecs::Entity e, const SoftBody&) { entities.PushBack(e); });
    for (ecs::Entity e : entities) {
        SoftBody& sb = *world.Get<SoftBody>(e);
        Body& b = *m_bodies.Insert(e.Pack(), Body{}).first;
        if (b.nodes.Empty() || b.resetRequested) {
            const bool reset = b.resetRequested;
            Init(world, e, sb, b, reset);
            b.resetRequested = false;
            // A reset next to a wall would put the car inside it: back off until every node is clear.
            if (reset && physics && physics->IsRunning()) {
                const Vec3 back = -SafeNormalize(Horizontal(b.rotation * Vec3(0, 0, 1)), Vec3(0, 0, 1)) * 0.5f;
                for (u32 tries = 0; tries < 12; ++tries) {
                    bool blocked = false;
                    PhysicsHit hit;
                    for (const Node& n : b.nodes)
                        if (physics->ClosestPoint(n.p, NodeRadius, hit)) {
                            blocked = true;
                            break;
                        }
                    if (!blocked)
                        break;
                    for (Node& n : b.nodes) {
                        n.p += back;
                        n.prev = n.p;
                    }
                    b.position += back;
                }
            }
        }
        b.alive = true;
        if (overrideInput)
            b.input = forcedInput;
        if (dt > 0.0f)
            Simulate(world, e, sb, b, dt, physics);
    }
    m_bodies.RemoveIf([](u64, const Body& b) { return !b.alive; });
}

void SoftBodySystem::Simulate(ecs::World& world, ecs::Entity e, SoftBody& sb, Body& b, f32 dt, PhysicsSystem* physics)
{
    const u32 steps = u32(Clamp(sb.substeps, 2, 40));
    const f32 h = dt / f32(steps);
    const f32 compliance = 5e-7f / Max(sb.stiffness, 0.01f);
    const f32 alpha = compliance / (h * h);
    const f32 yieldStrain = sb.deformation > 0.0f ? 0.012f / sb.deformation : 1e9f;
    const Vec3 gravity(0.0f, -9.81f, 0.0f);
    const f32 totalMass = Max(sb.mass, 50.0f);

    // Contact planes for this frame: the nearest surface around every node.
    if (physics && physics->IsRunning()) {
        for (Node& n : b.nodes) {
            n.hasPlane = false;
            n.pushed = Vec3();
            PhysicsHit hit;
            const f32 margin = NodeRadius + Length(n.v) * dt * 1.5f + 0.15f;
            if (!physics->ClosestPoint(n.p, margin, hit))
                continue;
            n.hasPlane = true;
            n.planePoint = hit.point;
            n.planeNormal = hit.distance > 1e-4f ? SafeNormalize(n.p - hit.point, hit.normal) : SafeNormalize(hit.normal, Vec3(0, 1, 0));
            n.planeEntity = hit.entity;
            const RigidBody* rb = world.IsAlive(hit.entity) ? world.Get<const RigidBody>(hit.entity) : nullptr;
            n.dynamicPlane = rb && rb->type == RigidBodyType::Dynamic;
        }
    } else {
        for (Node& n : b.nodes)
            n.hasPlane = false;
    }

    // Controls: steering eases in and narrows with speed.
    const Vec3 forwardNow = b.rotation * Vec3(0, 0, 1);
    Vec3 velocity;
    for (const Node& n : b.nodes)
        velocity += n.v;
    velocity /= f32(b.nodes.Size());
    const f32 speed = Dot(velocity, forwardNow);
    const f32 steerTarget = Clamp(b.input.steer, -1.0f, 1.0f) * sb.maxSteer / (1.0f + Abs(speed) / 22.0f);
    b.steer += (steerTarget - b.steer) * Min(1.0f, dt * 7.0f);
    const f32 throttle = Clamp(b.input.throttle, -1.0f, 1.0f);
    const f32 brake = Clamp(b.input.brake, 0.0f, 1.0f);
    u32 driven = 0;
    for (u32 w = 0; w < 4; ++w)
        driven += (sb.drive == VehicleDrive::All || (sb.drive == VehicleDrive::Front) == b.wheels[w].front) ? 1u : 0u;
    const f32 maxSpeed = 70.0f;

    const Vec3 wind = WindFor(world, e, b.position);
    f32 impact = 0.0f;
    for (u32 step = 0; step < steps; ++step) {
        UpdateFrame(b, 1);
        const Vec3 up = b.rotation * Vec3(0, 1, 0), fwd = b.rotation * Vec3(0, 0, 1), right = b.rotation * Vec3(1, 0, 0);

        // Wheels: raycast suspension, tire forces into the mount nodes.
        for (u32 w = 0; w < 4; ++w) {
            Wheel& wh = b.wheels[w];
            Vec3 mount = b.rotation * wh.offset, wheelVelocity;
            for (u32 k = 0; k < 4; ++k) {
                mount += b.nodes[wh.nodes[k]].p * wh.weights[k];
                wheelVelocity += b.nodes[wh.nodes[k]].v * wh.weights[k];
            }
            const f32 length = sb.suspensionLength + SoftBodyWheelRadius;
            PhysicsHit hit;
            const bool contact = physics && physics->IsRunning() && physics->Raycast(mount, -up, length, hit) && Dot(hit.normal, up) > 0.2f;
            const f32 previous = wh.compression;
            wh.distance = contact ? hit.distance : -1.0f;
            wh.compression = contact ? length - hit.distance : 0.0f;
            wh.center = mount - up * (contact ? hit.distance - SoftBodyWheelRadius : sb.suspensionLength);
            Vec3 force;
            const bool isDriven = sb.drive == VehicleDrive::All || (sb.drive == VehicleDrive::Front) == wh.front;
            Vec3 dir = fwd;
            if (wh.front)
                dir = fwd * std::cos(b.steer) + right * std::sin(b.steer);
            if (contact) {
                // Spring + damper, a stiff bump stop at the end of the travel.
                const f32 x = wh.compression, xdot = (x - previous) / h;
                f32 load = sb.suspensionStiffness * x + sb.suspensionDamping * xdot;
                if (x > sb.suspensionLength * 0.92f)
                    load += (x - sb.suspensionLength * 0.92f) * sb.suspensionStiffness * 10.0f;
                load = Max(load, 0.0f);
                force += up * load;
                // Tire: grip along the ground plane, friction circle from the load.
                const Vec3 n = hit.normal;
                const Vec3 ft = SafeNormalize(dir - n * Dot(dir, n), fwd);
                const Vec3 side = SafeNormalize(Cross(n, ft), right);
                const f32 vf = Dot(wheelVelocity, ft), vs = Dot(wheelVelocity, side);
                const f32 quarter = totalMass * 0.25f;
                const bool locked = b.input.handbrake && !wh.front;
                f32 lateral = -vs * quarter / h * 0.35f;
                f32 longitudinal = -vf * 30.0f; // rolling resistance
                if (isDriven && driven > 0 && Abs(vf) < maxSpeed)
                    longitudinal += throttle * Min(sb.maxDriveForce, sb.enginePower * 1000.0f / Max(Abs(vf), 1.0f)) / f32(driven);
                const f32 stop = Abs(vf) * quarter / h;
                if (brake > 0.0f)
                    longitudinal -= Sign(vf) * Min(sb.brakeForce * 0.25f * brake, stop);
                if (locked)
                    longitudinal -= Sign(vf) * Min(sb.brakeForce * 0.4f, stop);
                if (Abs(throttle) < 0.01f && brake <= 0.0f && Abs(vf) < 1.5f) // idling in gear: holds still
                    longitudinal -= Sign(vf) * Min(sb.brakeForce * 0.1f, stop * 0.5f);
                const f32 limit = sb.grip * load * (locked ? 0.55f : 1.0f);
                const f32 total = std::sqrt(lateral * lateral + longitudinal * longitudinal);
                if (total > limit && total > 0.0f) {
                    lateral *= limit / total;
                    longitudinal *= limit / total;
                }
                force += side * lateral + ft * longitudinal;
                wh.spinRate = locked ? 0.0f : vf / SoftBodyWheelRadius;
                if (isDriven && Abs(throttle) > 0.5f && total > limit) // wheelspin
                    wh.spinRate += throttle * 25.0f;
            } else {
                wh.spinRate *= 0.995f;
                if (isDriven)
                    wh.spinRate += throttle * 60.0f * h;
            }
            for (u32 k = 0; k < 4; ++k) {
                Node& n = b.nodes[wh.nodes[k]];
                n.v += force * (wh.weights[k] * h * n.invMass);
            }
            wh.spin += wh.spinRate * h;
        }

        // Predict.
        for (Node& n : b.nodes) {
            n.v += gravity * h;
            const Vec3 rel = n.v - wind;
            n.v -= rel * (Length(rel) * 0.0006f * h); // air drag (and the wind)
            n.prev = n.p;
            n.p += n.v * h;
        }
        // Beams (XPBD), denting and tearing.
        for (Beam& beam : b.beams) {
            if (beam.broken)
                continue;
            Node& na = b.nodes[beam.a];
            Node& nb = b.nodes[beam.b];
            const Vec3 d = nb.p - na.p;
            const f32 len = Length(d);
            if (len < 1e-6f)
                continue;
            const Vec3 dir = d / len;
            const f32 w = na.invMass + nb.invMass;
            const f32 lambda = -(len - beam.rest) / (w + alpha);
            na.p -= dir * (lambda * na.invMass);
            nb.p += dir * (lambda * nb.invMass);
            const f32 strain = (len - beam.original) / beam.original;
            if (Abs(strain) > sb.breakStrain) {
                beam.broken = true;
                b.damage += 0.02f;
                continue;
            }
            const f32 elastic = (len - beam.rest) / beam.original;
            if (Abs(elastic) > yieldStrain) {
                const f32 change = (elastic - Sign(elastic) * yieldStrain) * beam.original;
                const f32 rest = Clamp(beam.rest + change, beam.original * 0.4f, beam.original * 1.6f);
                b.damage += Abs(rest - beam.rest) / beam.original * 0.02f;
                beam.rest = rest;
            }
        }
        // Contacts: push out of the planes; rigid bodies take part of it.
        for (Node& n : b.nodes) {
            const Vec3 v = (n.p - n.prev) / h;
            n.v = v;
            if (!n.hasPlane)
                continue;
            const f32 dist = Dot(n.p - n.planePoint, n.planeNormal) - NodeRadius;
            if (dist >= 0.0f)
                continue;
            const f32 vn = Dot(v, n.planeNormal);
            f32 share = 1.0f;
            if (n.dynamicPlane) {
                const RigidBody* rb = world.Get<const RigidBody>(n.planeEntity);
                const f32 bodyMass = rb && rb->mass > 0.0f ? rb->mass : 40.0f;
                share = Clamp(bodyMass / (bodyMass + 1.0f / n.invMass * 4.0f), 0.05f, 1.0f);
                if (vn < 0.0f)
                    n.pushed -= n.planeNormal * (-vn / n.invMass * (1.0f - share * 0.5f));
            }
            n.p -= n.planeNormal * (dist * share);
            impact = Max(impact, -vn);
            // Velocity: no bounce, Coulomb friction from the correction.
            Vec3 nv = (n.p - n.prev) / h;
            const f32 vnAfter = Dot(nv, n.planeNormal);
            Vec3 vt = nv - n.planeNormal * vnAfter;
            const f32 vtLen = Length(vt);
            const f32 correction = -dist * share / h;
            if (vtLen > 1e-6f)
                vt *= Max(0.0f, 1.0f - 0.6f * correction / vtLen);
            n.v = vt + n.planeNormal * Max(vnAfter, 0.0f) * 0.1f;
        }
        // Beam damping: kills the lattice's ringing.
        const f32 damping = Min(1.0f, 40.0f * h);
        for (const Beam& beam : b.beams) {
            if (beam.broken)
                continue;
            Node& na = b.nodes[beam.a];
            Node& nb = b.nodes[beam.b];
            const Vec3 dir = SafeNormalize(nb.p - na.p, Vec3(0, 1, 0));
            const f32 vr = Dot(nb.v - na.v, dir) * damping;
            const f32 w = na.invMass + nb.invMass;
            na.v += dir * (vr * na.invMass / w);
            nb.v -= dir * (vr * nb.invMass / w);
        }
    }
    UpdateFrame(b, 2);
    b.impact = Max(b.impact, impact);
    b.damage = Min(b.damage, 1.0f);

    // Pushed rigid bodies.
    if (physics)
        for (const Node& n : b.nodes)
            if (n.dynamicPlane && LengthSq(n.pushed) > 1e-6f)
                physics->AddImpulseAtPoint(n.planeEntity, n.pushed * (1.0f / f32(steps)), n.p);

    // The entity follows the body; the mesh is skinned to the nodes' offsets from the rest shape.
    if (Transform* t = world.Get<Transform>(e)) {
        t->position = b.position;
        t->rotation = b.rotation;
    }
    const Quat inv = Inverse(b.rotation);
    sb.palette.Resize(b.nodes.Size());
    for (usize i = 0; i < b.nodes.Size(); ++i) {
        const Vec3 local = inv * (b.nodes[i].p - b.position);
        sb.palette[i] = Transform(local - b.nodes[i].rest).ToMatrix();
    }
    // Wheels: where the suspension holds them, steered and spinning.
    for (u32 w = 0; w < 4; ++w) {
        const Wheel& wh = b.wheels[w];
        Transform* t = world.IsAlive(wh.visual) ? world.Get<Transform>(wh.visual) : nullptr;
        if (!t)
            continue;
        const Vec3 local = inv * (wh.center - b.position);
        const f32 steer = wh.front ? b.steer : 0.0f;
        const Quat yaw = Quat::LookRotation(Vec3(std::sin(steer), 0.0f, std::cos(steer)), Vec3(0, 1, 0));
        *t = Transform(local, Normalize(yaw * Quat::FromAxisAngle(Vec3(1, 0, 0), wh.spin * RollSign())));
    }
    // Engine sound: pitch from the revs of a fake gearbox, louder on the throttle.
    const f32 gear = 11.0f; // m/s per gear
    const f32 v = Abs(speed);
    const f32 inGear = v < gear * 4.0f ? (v - Floor(v / gear) * gear) / gear : Min(1.0f, (v - gear * 4.0f) / (gear * 2.0f) + 0.6f);
    const f32 target = Clamp(0.15f + inGear * 0.75f + Abs(throttle) * 0.12f, 0.0f, 1.1f);
    b.revs += (target - b.revs) * Min(1.0f, dt * 6.0f);
    if (AudioSource* engine = world.Get<AudioSource>(e)) {
        engine->pitch = 0.65f + b.revs * 1.0f;
        engine->volume = 0.35f + Abs(throttle) * 0.35f;
    }
}

void SoftBodySystem::SetInput(ecs::Entity e, const VehicleInput& input)
{
    Body& b = *m_bodies.Insert(e.Pack(), Body{}).first;
    b.input = input;
}

f32 SoftBodySystem::Speed(ecs::Entity e) const
{
    const Body* b = m_bodies.Find(e.Pack());
    if (!b || b->nodes.Empty())
        return 0.0f;
    Vec3 v;
    for (const Node& n : b->nodes)
        v += n.v;
    return Dot(v / f32(b->nodes.Size()), b->rotation * Vec3(0, 0, 1));
}

f32 SoftBodySystem::Damage(ecs::Entity e) const
{
    const Body* b = m_bodies.Find(e.Pack());
    return b ? b->damage : 0.0f;
}

f32 SoftBodySystem::TakeImpact(ecs::Entity e)
{
    Body* b = m_bodies.Find(e.Pack());
    if (!b)
        return 0.0f;
    const f32 impact = b->impact;
    b->impact = 0.0f;
    return impact;
}

void SoftBodySystem::Reset(ecs::Entity e)
{
    if (Body* b = m_bodies.Find(e.Pack()); b && !b->nodes.Empty())
        b->resetRequested = true;
}

u32 SoftBodySystem::BrokenBeams(ecs::Entity e) const
{
    const Body* b = m_bodies.Find(e.Pack());
    u32 n = 0;
    if (b)
        for (const Beam& beam : b->beams)
            n += beam.broken ? 1u : 0u;
    return n;
}

} // namespace zx
