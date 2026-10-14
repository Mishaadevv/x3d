// Starter Content: example materials and scripts copied into new projects (all plain files the user can edit).

#include "Core/CoreInit.h"
#include "Core/FileSystem/FileSystem.h"
#include "Core/HAL/PlatformFile.h"
#include "Core/String/Path.h"
#include "Audio/AudioClip.h"
#include "Core/Containers/Function.h"
#include "Core/Math/Random.h"
#include "Engine/Project.h"

#include <cmath>
#include "Render/MaterialGraph.h"

namespace zx {

namespace {

struct StarterMaterial
{
    const char* name;
    Vec4 baseColor;
    f32 metallic;
    f32 roughness;
    Vec4 emissive; // w = 0: none
};

const StarterMaterial Materials[] = {
    {"M_Plastic_Red", Vec4(0.75f, 0.08f, 0.06f, 1), 0.0f, 0.35f, Vec4(0)},
    {"M_Plastic_White", Vec4(0.85f, 0.85f, 0.83f, 1), 0.0f, 0.45f, Vec4(0)},
    {"M_Rubber_Black", Vec4(0.03f, 0.03f, 0.03f, 1), 0.0f, 0.9f, Vec4(0)},
    {"M_Metal_Steel", Vec4(0.62f, 0.64f, 0.66f, 1), 1.0f, 0.3f, Vec4(0)},
    {"M_Metal_Gold", Vec4(1.0f, 0.77f, 0.34f, 1), 1.0f, 0.2f, Vec4(0)},
    {"M_Metal_Copper", Vec4(0.95f, 0.64f, 0.54f, 1), 1.0f, 0.35f, Vec4(0)},
    {"M_Gold_Glow", Vec4(1.0f, 0.77f, 0.34f, 1), 1.0f, 0.25f, Vec4(0.9f, 0.55f, 0.08f, 1)},
    {"M_Paint_Olive", Vec4(0.2f, 0.24f, 0.12f, 1), 0.0f, 0.55f, Vec4(0)},
    {"M_Emissive_Cyan", Vec4(0.02f, 0.05f, 0.06f, 1), 0.0f, 0.5f, Vec4(0.2f, 3.0f, 4.0f, 1)},
};

/// Output fed by named parameters (Base Color, Metallic, Roughness[, Emissive]) so material instances can override them.
render::MaterialGraph MakeMaterial(const StarterMaterial& m)
{
    render::MaterialGraph g;
    g.guid = Guid::FromName(Guid(), m.name); // stable across projects
    const u32 output = g.AddNode("Output", Vec2(360, 0))->id;
    render::MaterialGraphNode* color = g.AddNode("Color", Vec2(0, -80));
    color->name = "Base Color";
    color->value = m.baseColor;
    const u32 colorId = color->id;
    render::MaterialGraphNode* metal = g.AddNode("Scalar", Vec2(0, 40));
    metal->name = "Metallic";
    metal->value = Vec4(m.metallic, 0, 0, 0);
    const u32 metalId = metal->id;
    render::MaterialGraphNode* rough = g.AddNode("Scalar", Vec2(0, 120));
    rough->name = "Roughness";
    rough->value = Vec4(m.roughness, 0, 0, 0);
    const u32 roughId = rough->id;
    g.Connect(colorId, 0, output, 0);
    g.Connect(metalId, 0, output, 1);
    g.Connect(roughId, 0, output, 2);
    if (m.emissive.w > 0.0f) {
        render::MaterialGraphNode* emissive = g.AddNode("Color", Vec2(0, 200));
        emissive->name = "Emissive";
        emissive->value = m.emissive;
        g.Connect(emissive->id, 0, output, 4);
    }
    return g;
}

/// Procedural surfaces: a world-aligned grid (prototype blocks, like the UE template levels) or two colors mixed by
/// fractal noise (natural surfaces). Every value is a named parameter, so material instances can retint them.
enum class Pattern : u8
{
    Grid,
    Noise,
};

struct PatternMaterial
{
    const char* name;
    Pattern pattern;
    Vec4 colorA;      // base color
    Vec4 colorB;      // grid lines / second noise color
    f32 scale;        // grid cell size (m) / noise frequency (1/m)
    f32 detail;       // grid line width (fraction of a cell) / weight of a 6x finer noise
    f32 roughnessA, roughnessB;
};

const PatternMaterial Patterns[] = {
    {"M_Proto_Grey", Pattern::Grid, Vec4(0.42f, 0.42f, 0.42f, 1), Vec4(0.2f, 0.2f, 0.2f, 1), 1.0f, 0.025f, 0.7f, 0.7f},
    {"M_Proto_Light", Pattern::Grid, Vec4(0.72f, 0.72f, 0.7f, 1), Vec4(0.45f, 0.45f, 0.44f, 1), 1.0f, 0.025f, 0.6f, 0.6f},
    {"M_Proto_Orange", Pattern::Grid, Vec4(0.85f, 0.36f, 0.08f, 1), Vec4(0.5f, 0.18f, 0.03f, 1), 0.5f, 0.04f, 0.5f, 0.5f},
    {"M_Proto_Blue", Pattern::Grid, Vec4(0.12f, 0.32f, 0.75f, 1), Vec4(0.05f, 0.14f, 0.4f, 1), 0.5f, 0.04f, 0.45f, 0.45f},
    {"M_Proto_Teal", Pattern::Grid, Vec4(0.08f, 0.55f, 0.5f, 1), Vec4(0.03f, 0.28f, 0.25f, 1), 0.5f, 0.04f, 0.45f, 0.45f},
    {"M_Concrete", Pattern::Noise, Vec4(0.52f, 0.51f, 0.48f, 1), Vec4(0.36f, 0.35f, 0.33f, 1), 0.7f, 0.45f, 0.8f, 0.95f},
    {"M_Grass", Pattern::Noise, Vec4(0.13f, 0.3f, 0.05f, 1), Vec4(0.32f, 0.42f, 0.1f, 1), 0.25f, 0.4f, 0.85f, 0.95f},
    {"M_Wood", Pattern::Noise, Vec4(0.42f, 0.25f, 0.12f, 1), Vec4(0.25f, 0.14f, 0.06f, 1), 1.6f, 0.5f, 0.55f, 0.75f},
    {"M_Sand", Pattern::Noise, Vec4(0.78f, 0.67f, 0.48f, 1), Vec4(0.62f, 0.52f, 0.36f, 1), 0.6f, 0.6f, 0.85f, 0.95f},
    {"M_Rock", Pattern::Noise, Vec4(0.36f, 0.35f, 0.33f, 1), Vec4(0.16f, 0.16f, 0.15f, 1), 0.9f, 0.55f, 0.65f, 0.9f},
    {"M_Hay", Pattern::Noise, Vec4(0.74f, 0.6f, 0.27f, 1), Vec4(0.5f, 0.38f, 0.14f, 1), 3.0f, 0.7f, 0.9f, 0.95f},
    {"M_Dirt", Pattern::Noise, Vec4(0.3f, 0.21f, 0.13f, 1), Vec4(0.18f, 0.12f, 0.07f, 1), 0.5f, 0.5f, 0.9f, 1.0f},
};

/// Textured relief material: Parallax Occlusion offsets the UV the color and normal maps use (bricks, cobblestones).
render::MaterialGraph MakeReliefMaterial(StringView name, StringView textures, f32 tiling, f32 height, f32 roughness)
{
    render::MaterialGraph g;
    g.guid = Guid::FromName(Guid(), name);
    const u32 output = g.AddNode("Output", Vec2(760, 0))->id;
    render::MaterialGraphNode* uv = g.AddNode("TexCoord", Vec2(0, 0));
    uv->value = Vec4(tiling, tiling, 0, 0);
    const u32 uvId = uv->id;
    render::MaterialGraphNode* pom = g.AddNode("ParallaxOcclusion", Vec2(200, 0));
    pom->name = "Height Map";
    pom->texture = String::Format("StarterContent/Textures/{}_Height.png", textures);
    pom->inputValues[1] = Vec4(height, 0, 0, 0);
    pom->inputValues[2] = Vec4(16, 0, 0, 0);
    const u32 pomId = pom->id;
    g.Connect(uvId, 0, pomId, 0);
    render::MaterialGraphNode* color = g.AddNode("TextureSample", Vec2(460, -120));
    color->name = "Color Map";
    color->texture = String::Format("StarterContent/Textures/{}_Color.png", textures);
    const u32 colorId = color->id;
    render::MaterialGraphNode* normal = g.AddNode("TextureSample", Vec2(460, 120));
    normal->name = "Normal Map";
    normal->texture = String::Format("StarterContent/Textures/{}_Normal.png", textures);
    normal->normalMap = true;
    const u32 normalId = normal->id;
    g.Connect(pomId, 0, colorId, 0);
    g.Connect(pomId, 0, normalId, 0);
    g.Connect(colorId, 0, output, 0);
    g.Connect(normalId, 0, output, 3);
    render::MaterialGraphNode* rough = g.AddNode("Scalar", Vec2(460, 300));
    rough->name = "Roughness";
    rough->value = Vec4(roughness, 0, 0, 0);
    g.Connect(rough->id, 0, output, 2);
    return g;
}

/// Foliage: green with fine noise, two-sided, light shining through it (Subsurface).
render::MaterialGraph MakeLeavesMaterial()
{
    render::MaterialGraph g;
    g.guid = Guid::FromName(Guid(), "M_Leaves");
    g.twoSided = true;
    const u32 output = g.AddNode("Output", Vec2(720, 0))->id;
    auto value = [&](StringView type, StringView name, Vec4 v, Vec2 pos) {
        render::MaterialGraphNode* n = g.AddNode(type, pos);
        n->name = String(name);
        n->value = v;
        return n->id;
    };
    const u32 noise = g.AddNode("Noise", Vec2(200, 60))->id;
    g.Connect(value("Scalar", "Noise Scale", Vec4(3.0f, 0, 0, 0), Vec2(0, 60)), 0, noise, 1);
    const u32 color = g.AddNode("Lerp", Vec2(460, -60))->id;
    g.Connect(value("Color", "Dark Green", Vec4(0.07f, 0.2f, 0.03f, 1), Vec2(0, -160)), 0, color, 0);
    g.Connect(value("Color", "Light Green", Vec4(0.22f, 0.42f, 0.07f, 1), Vec2(0, -60)), 0, color, 1);
    g.Connect(noise, 0, color, 2);
    g.Connect(color, 0, output, 0);
    g.Connect(value("Scalar", "Roughness", Vec4(0.6f, 0, 0, 0), Vec2(460, 160)), 0, output, 2);
    g.Connect(value("Color", "Subsurface", Vec4(0.35f, 0.55f, 0.08f, 1), Vec2(460, 260)), 0, output, 8);
    return g;
}

/// Terrain: grass on flat ground, rock on slopes, snow above a height - all from the world normal and position (no
/// textures, no painting needed).
render::MaterialGraph MakeTerrainMaterial()
{
    render::MaterialGraph g;
    g.guid = Guid::FromName(Guid(), "M_Terrain");
    const u32 output = g.AddNode("Output", Vec2(1300, 0))->id;
    auto value = [&](StringView type, StringView name, Vec4 v, Vec2 pos) {
        render::MaterialGraphNode* n = g.AddNode(type, pos);
        n->name = String(name);
        n->value = v;
        return n->id;
    };
    auto node = [&](StringView type, Vec2 pos) { return g.AddNode(type, pos)->id; };
    auto lerp = [&](u32 a, u32 b, u32 alpha, Vec2 pos) {
        const u32 l = node("Lerp", pos);
        g.Connect(a, 0, l, 0);
        g.Connect(b, 0, l, 1);
        g.Connect(alpha, 0, l, 2);
        return l;
    };
    auto math = [&](StringView type, u32 a, u32 b, Vec2 pos) {
        const u32 m = node(type, pos);
        g.Connect(a, 0, m, 0);
        g.Connect(b, 0, m, 1);
        return m;
    };
    // Noise layers: large patches and fine detail.
    const u32 patches = node("Noise", Vec2(0, -400));
    g.Connect(value("Scalar", "Patch Scale", Vec4(0.06f, 0, 0, 0), Vec2(-200, -400)), 0, patches, 1);
    const u32 detail = node("Noise", Vec2(0, -300));
    g.Connect(value("Scalar", "Detail Scale", Vec4(1.2f, 0, 0, 0), Vec2(-200, -300)), 0, detail, 1);
    // Grass and rock colors.
    const u32 grass = lerp(value("Color", "Grass Dark", Vec4(0.08f, 0.17f, 0.03f, 1), Vec2(200, -500)),
                           value("Color", "Grass Light", Vec4(0.24f, 0.33f, 0.08f, 1), Vec2(200, -420)), patches, Vec2(420, -460));
    const u32 rock = lerp(value("Color", "Rock Dark", Vec4(0.17f, 0.16f, 0.15f, 1), Vec2(200, -320)),
                          value("Color", "Rock Light", Vec4(0.42f, 0.4f, 0.37f, 1), Vec2(200, -240)), detail, Vec2(420, -280));
    // Flatness: (normal.y - Slope Start) * Sharpness, broken up a little by the detail noise.
    render::MaterialGraphNode* mask = g.AddNode("ComponentMask", Vec2(200, 0));
    mask->value = Vec4(0, 1, 0, 0);
    const u32 normalY = mask->id;
    g.Connect(node("VertexNormal", Vec2(0, 0)), 0, normalY, 0);
    const u32 jitter = math("Multiply", detail, value("Scalar", "", Vec4(0.08f, 0, 0, 0), Vec2(200, 100)), Vec2(400, 100));
    const u32 shifted = math("Subtract", math("Add", normalY, jitter, Vec2(560, 0)), value("Scalar", "Slope Start", Vec4(0.78f, 0, 0, 0), Vec2(400, 180)),
                             Vec2(720, 0));
    const u32 flat = node("Saturate", Vec2(1040, 0));
    g.Connect(math("Multiply", shifted, value("Scalar", "Slope Sharpness", Vec4(9.0f, 0, 0, 0), Vec2(720, 120)), Vec2(880, 0)), 0, flat, 0);
    const u32 ground = lerp(rock, grass, flat, Vec2(1040, -300));
    // Snow above Snow Height (on the flatter parts), fading in over 6 m.
    render::MaterialGraphNode* ymask = g.AddNode("ComponentMask", Vec2(200, 300));
    ymask->value = Vec4(0, 1, 0, 0);
    const u32 worldY = ymask->id;
    g.Connect(node("WorldPosition", Vec2(0, 300)), 0, worldY, 0);
    const u32 above = math("Subtract", worldY, value("Scalar", "Snow Height", Vec4(22.0f, 0, 0, 0), Vec2(200, 400)), Vec2(400, 300));
    const u32 snowRamp = node("Saturate", Vec2(720, 300));
    g.Connect(math("Multiply", above, value("Scalar", "", Vec4(1.0f / 6.0f, 0, 0, 0), Vec2(400, 400)), Vec2(560, 300)), 0, snowRamp, 0);
    const u32 snowAmount = math("Multiply", snowRamp, math("Add", flat, value("Scalar", "", Vec4(0.35f, 0, 0, 0), Vec2(720, 460)), Vec2(880, 400)),
                                Vec2(1040, 300));
    const u32 snowClamped = node("Saturate", Vec2(1160, 300));
    g.Connect(snowAmount, 0, snowClamped, 0);
    const u32 color = lerp(ground, value("Color", "Snow", Vec4(0.86f, 0.88f, 0.92f, 1), Vec2(1040, -160)), snowClamped, Vec2(1160, -200));
    g.Connect(color, 0, output, 0);
    // Rough everywhere, snow a bit smoother.
    g.Connect(lerp(value("Scalar", "Roughness", Vec4(0.92f, 0, 0, 0), Vec2(1040, 500)), value("Scalar", "Snow Roughness", Vec4(0.6f, 0, 0, 0), Vec2(1040, 580)),
                   snowClamped, Vec2(1160, 520)),
              0, output, 2);
    return g;
}

render::MaterialGraph MakePatternMaterial(const PatternMaterial& m)
{
    render::MaterialGraph g;
    g.guid = Guid::FromName(Guid(), m.name);
    const u32 output = g.AddNode("Output", Vec2(720, 0))->id;
    auto value = [&](StringView type, StringView name, Vec4 v, Vec2 pos) {
        render::MaterialGraphNode* n = g.AddNode(type, pos);
        n->name = String(name);
        n->value = v;
        return n->id;
    };
    const u32 a = value("Color", "Base Color", m.colorA, Vec2(0, -160));
    const u32 b = value("Color", m.pattern == Pattern::Grid ? "Line Color" : "Color B", m.colorB, Vec2(0, -60));
    const u32 roughA = value("Scalar", "Roughness", Vec4(m.roughnessA, 0, 0, 0), Vec2(0, 260));
    const u32 roughB = value("Scalar", "Roughness B", Vec4(m.roughnessB, 0, 0, 0), Vec2(0, 340));
    u32 mask = 0, maskPin = 0;
    if (m.pattern == Pattern::Grid) {
        const u32 grid = g.AddNode("WorldGrid", Vec2(220, 60))->id;
        g.Connect(value("Scalar", "Cell Size", Vec4(m.scale, 0, 0, 0), Vec2(0, 60)), 0, grid, 0);
        g.Connect(value("Scalar", "Line Width", Vec4(m.detail, 0, 0, 0), Vec2(0, 140)), 0, grid, 1);
        // Every other cell slightly darker, like the classic prototype grid.
        const u32 tint = g.AddNode("Lerp", Vec2(420, -140))->id;
        g.Connect(a, 0, tint, 0);
        const u32 dark = g.AddNode("Multiply", Vec2(220, -200))->id;
        g.Connect(a, 0, dark, 0);
        g.Connect(value("Scalar", "", Vec4(0.88f, 0, 0, 0), Vec2(0, -240)), 0, dark, 1);
        g.Connect(dark, 0, tint, 1);
        g.Connect(grid, 1, tint, 2);
        const u32 color = g.AddNode("Lerp", Vec2(560, -100))->id;
        g.Connect(tint, 0, color, 0);
        g.Connect(b, 0, color, 1);
        g.Connect(grid, 0, color, 2);
        g.Connect(color, 0, output, 0);
        mask = grid;
        maskPin = 0;
    } else {
        // Large-scale noise blended with a finer one.
        const u32 coarse = g.AddNode("Noise", Vec2(220, 60))->id;
        const u32 scale = value("Scalar", "Scale", Vec4(m.scale, 0, 0, 0), Vec2(0, 60));
        g.Connect(scale, 0, coarse, 1);
        const u32 fineScale = g.AddNode("Multiply", Vec2(220, 160))->id;
        g.Connect(scale, 0, fineScale, 0);
        g.Connect(value("Scalar", "", Vec4(6.0f, 0, 0, 0), Vec2(0, 160)), 0, fineScale, 1);
        const u32 fine = g.AddNode("Noise", Vec2(400, 160))->id;
        g.Connect(fineScale, 0, fine, 1);
        const u32 mix = g.AddNode("Lerp", Vec2(560, 100))->id;
        g.Connect(coarse, 0, mix, 0);
        g.Connect(fine, 0, mix, 1);
        g.Connect(value("Scalar", "Detail", Vec4(m.detail, 0, 0, 0), Vec2(220, 260)), 0, mix, 2);
        const u32 color = g.AddNode("Lerp", Vec2(560, -100))->id;
        g.Connect(a, 0, color, 0);
        g.Connect(b, 0, color, 1);
        g.Connect(mix, 0, color, 2);
        g.Connect(color, 0, output, 0);
        mask = mix;
        maskPin = 0;
    }
    const u32 rough = g.AddNode("Lerp", Vec2(560, 300))->id;
    g.Connect(roughA, 0, rough, 0);
    g.Connect(roughB, 0, rough, 1);
    g.Connect(mask, maskPin, rough, 2);
    g.Connect(rough, 0, output, 2);
    return g;
}

struct StarterScript
{
    const char* file;
    const char* source;
};

const StarterScript Scripts[] = {
    {"Rotator.zs", R"zs(// Spins the entity. Change the axis and speed per entity in the Details panel.
script Rotator {
    @tooltip("Rotation axis (local space)")
    prop axis = vec3(0, 1, 0)
    @range(-720, 720) @tooltip("Degrees per second")
    prop speed = 45.0

    fn update(dt) {
        entity.rotate(axis, speed * dt)
    }
}
)zs"},
    {"Bobbing.zs", R"zs(// Floats the entity up and down around its start position.
script Bobbing {
    @range(0, 5) @tooltip("Height of the movement in meters")
    prop height = 0.25
    @range(0, 5) @tooltip("Cycles per second")
    prop frequency = 0.5

    var origin = vec3(0)

    fn start() {
        origin = entity.position
    }

    fn update(dt) {
        entity.position = origin + vec3(0, sin(time.elapsed * frequency * 2 * PI) * height, 0)
    }
}
)zs"},
    {"SimpleMover.zs", R"zs(// Moves the entity with WASD / arrow keys (or a gamepad stick) on the ground plane.
script SimpleMover {
    @range(0, 20) @tooltip("Meters per second")
    prop speed = 4.0
    @range(0, 2) @tooltip("Speed multiplier while Shift is held")
    prop sprint = 1.8
    @tooltip("Turn to face the movement direction")
    prop faceDirection = true

    fn update(dt) {
        var move = input.move
        move.y = 0
        if move.length() < 0.01 { return }
        var s = speed
        if input.key("Shift") { s *= sprint }
        entity.position += move * s * dt
        if faceDirection {
            entity.rotation = quat.lookAt(move)
        }
    }
}
)zs"},
    {"LookAt.zs", R"zs(// Keeps the entity turned towards another entity (by name).
script LookAt {
    @tooltip("Name of the entity to look at")
    prop target = "Player"

    fn update(dt) {
        let other = find(target)
        if other != nil {
            entity.lookAt(other.position)
        }
    }
}
)zs"},
    {"Blinker.zs", R"zs(// Shows and hides the entity on a timer by scaling it (a simple example of state and timers).
script Blinker {
    @range(0.05, 10) prop interval = 0.5

    var timer = 0.0
    var visible = true
    var startScale = vec3(1)

    fn start() {
        startScale = entity.scale
    }

    fn update(dt) {
        timer += dt
        if timer >= interval {
            timer -= interval
            visible = not visible
            if visible { entity.scale = startScale } else { entity.scale = vec3(0.001) }
        }
    }
}
)zs"},
};


const StarterScript GameplayScripts[] = {
    {"FirstPersonController.zs", R"zs(// First-person player: mouse look, WASD walking, Shift sprint, Space jump.
// Needs a Character Controller on this entity and a child entity named "Camera" (with a Camera component).
script FirstPersonController {
    @range(0, 20) @tooltip("Meters per second")
    prop walkSpeed = 4.5
    @range(0, 30) @tooltip("Meters per second while Shift is held")
    prop sprintSpeed = 7.5
    @range(0, 20) @tooltip("Upward speed of a jump (m/s)")
    prop jumpSpeed = 6.5
    @range(0.01, 1) @tooltip("Degrees per mouse count")
    prop mouseSensitivity = 0.12
    prop invertY = false
    @range(0, 3) @tooltip("Height of the eyes above the feet")
    prop eyeHeight = 1.65
    @tooltip("Footstep sound set: variants _0 .. _9 played at random (empty: silent)")
    prop footsteps = "StarterContent/Sounds/Footsteps/Footstep_Concrete"
    @range(0.3, 4) @tooltip("Meters walked between footsteps")
    prop stride = 1.7

    var yaw = 0.0
    var stepDistance = 0.0
    var wasGrounded = true
    var pitch = 0.0
    var camera = nil

    fn start() {
        camera = entity.child("Camera")
        if camera == nil {
            log("FirstPersonController: add a child entity named Camera with a Camera component")
        } else {
            camera.position = vec3(0, eyeHeight, 0)
        }
        yaw = entity.rotation.euler().y
    }

    fn update(dt) {
        // Look: yaw turns the body, pitch tilts only the camera.
        let look = input.look
        yaw += look.x * mouseSensitivity
        var dy = look.y * mouseSensitivity
        if invertY { dy = -dy }
        pitch = clamp(pitch + dy, -85, 85)
        entity.rotation = quat.euler(0, yaw, 0)
        if camera != nil { camera.rotation = quat.euler(pitch, 0, 0) }

        // Move relative to where the body faces.
        let move = input.move
        var speed = walkSpeed
        if input.key("Shift") { speed = sprintSpeed }
        let dir = entity.forward * move.z + entity.right * move.x
        character.move(dir * speed)
        if input.pressed("Space") and character.grounded {
            character.jump(jumpSpeed)
        }
        // Footsteps: one every stride walked on the ground, and one on landing.
        if footsteps != "" {
            if character.grounded {
                stepDistance += min(dir.length(), 1) * speed * dt
                if stepDistance > stride or not wasGrounded {
                    stepDistance = 0
                    audio.playAt(footsteps, entity.worldPosition, 0.45)
                }
            }
            wasGrounded = character.grounded
        }
    }
}
)zs"},
    {"ThirdPersonController.zs", R"zs(// Third-person player: moves relative to the camera, turns towards the direction of travel.
// Needs a Character Controller on this entity; the camera is found by name (see ThirdPersonCamera.zs).
script ThirdPersonController {
    @tooltip("Name of the camera entity used for directions")
    prop camera = "Camera"
    @range(0, 20) prop walkSpeed = 4.0
    @range(0, 30) prop sprintSpeed = 7.0
    @range(0, 20) prop jumpSpeed = 6.5
    @range(1, 30) @tooltip("How fast the body turns to the move direction")
    prop turnRate = 12.0
    @tooltip("Footstep sound set: variants _0 .. _9 played at random (empty: silent)")
    prop footsteps = "StarterContent/Sounds/Footsteps/Footstep_Concrete"
    @range(0.3, 4) @tooltip("Meters walked between footsteps")
    prop stride = 1.7

    var stepDistance = 0.0
    var wasGrounded = true

    fn update(dt) {
        let move = input.move
        var forward = vec3(0, 0, 1)
        var right = vec3(1, 0, 0)
        let cam = find(camera)
        if cam != nil {
            forward = cam.forward
            forward.y = 0
            forward = forward.normalized()
            right = cam.right
            right.y = 0
            right = right.normalized()
        }
        let dir = forward * move.z + right * move.x
        var speed = walkSpeed
        if input.key("Shift") { speed = sprintSpeed }
        character.move(dir * speed)
        if dir.length() > 0.1 {
            entity.rotation = entity.rotation.slerp(quat.lookAt(dir), min(1, turnRate * dt))
        }
        if input.pressed("Space") and character.grounded {
            character.jump(jumpSpeed)
        }
        // Footsteps: one every stride walked on the ground, and one on landing.
        if footsteps != "" {
            if character.grounded {
                stepDistance += min(dir.length(), 1) * speed * dt
                if stepDistance > stride or not wasGrounded {
                    stepDistance = 0
                    audio.playAt(footsteps, entity.worldPosition, 0.45)
                }
            }
            wasGrounded = character.grounded
        }
    }
}
)zs"},
    {"ThirdPersonCamera.zs", R"zs(// Orbit camera that follows a target: mouse to look around, wheel to zoom.
script ThirdPersonCamera {
    @tooltip("Name of the entity to follow")
    prop target = "Player"
    @range(1, 20) prop distance = 5.0
    @range(0, 5) @tooltip("Point looked at, above the target's feet")
    prop height = 1.6
    @range(0.01, 1) prop mouseSensitivity = 0.15
    @range(-89, 89) prop minPitch = -20.0
    @range(-89, 89) prop maxPitch = 70.0
    @range(1, 40) @tooltip("Follow smoothing (higher = tighter)")
    prop smoothing = 15.0

    var yaw = 0.0
    var pitch = 15.0
    var pivot = vec3(0)
    var started = false

    fn update(dt) {
        let t = find(target)
        if t == nil { return }
        let look = input.look
        yaw += look.x * mouseSensitivity
        pitch = clamp(pitch + look.y * mouseSensitivity, minPitch, maxPitch)
        distance = clamp(distance - input.wheel * 0.5, 1.5, 20)
        let goal = t.position + vec3(0, height, 0)
        if not started {
            pivot = goal
            yaw = t.rotation.euler().y
            started = true
        }
        pivot = lerp(pivot, goal, min(1, smoothing * dt))
        let rot = quat.euler(pitch, yaw, 0)
        entity.rotation = rot
        entity.position = pivot - rot.forward() * distance
    }
}
)zs"},
};

/// Scripts of the Starter Content prefabs (vehicles and gameplay props).
const StarterScript PropScripts[] = {
    {"TankController.zs", R"zs(// Tank: W/S drive, A/D turn the hull, the mouse aims the turret and the barrel, left click fires.
// Needs a Character Controller on this entity and descendants named "Turret", "Barrel", "Muzzle" (with a Particle
// Emitter for the muzzle flash) and "Impact" (a Particle Emitter moved to where the shell hits). Shells set Flammable
// objects on fire.
script TankController {
    @tooltip("Reads the keyboard and mouse (off: the tank stands still, e.g. while another player is driven)")
    prop controlled = true
    @range(0, 30) @tooltip("Forward speed (m/s)")
    prop speed = 6.0
    @range(0, 30) prop reverseSpeed = 3.0
    @range(0, 180) @tooltip("Hull turn rate (degrees per second)")
    prop turnRate = 60.0
    @range(0.01, 1) prop mouseSensitivity = 0.12
    @range(-30, 0) prop minElevation = -6.0
    @range(0, 60) prop maxElevation = 20.0
    @range(0.05, 5) @tooltip("Seconds between shots")
    prop reload = 0.8
    @range(0, 100000) @tooltip("Impulse given to what the shell hits (N*s)")
    prop impactImpulse = 6000.0
    @range(10, 2000) prop range = 300.0
    @tooltip("Shells set Flammable objects on fire")
    prop incendiary = true

    var yaw = 0.0
    var turretYaw = 0.0
    var elevation = 0.0
    var cooldown = 0.0
    var turret = nil
    var barrel = nil
    var muzzle = nil
    var impact = nil

    fn start() {
        yaw = entity.rotation.euler().y
        turret = entity.child("Turret")
        barrel = entity.child("Barrel")
        muzzle = entity.child("Muzzle")
        impact = entity.child("Impact")
    }

    fn update(dt) {
        if not controlled { return }
        // Hull: tracks drive along its forward axis and turn it in place.
        let move = input.move
        var s = speed
        if move.z < 0 { s = reverseSpeed }
        character.move(entity.forward * (move.z * s))
        yaw += move.x * turnRate * dt
        entity.rotation = quat.euler(0, yaw, 0)

        // Turret and barrel follow the mouse (relative to the hull).
        let look = input.look
        turretYaw += look.x * mouseSensitivity
        elevation = clamp(elevation - look.y * mouseSensitivity, minElevation, maxElevation)
        if turret != nil { turret.rotation = quat.euler(0, turretYaw, 0) }
        if barrel != nil { barrel.rotation = quat.euler(-elevation, 0, 0) }

        cooldown -= dt
        if input.mousePressed("left") and cooldown <= 0 {
            cooldown = reload
            shoot()
        }
    }

    fn shoot() {
        if muzzle == nil { return }
        let origin = muzzle.worldPosition
        let dir = muzzle.worldForward
        muzzle.emit()
        audio.playAt("StarterContent/Sounds/Props/Explosion_Low", origin, 1.0)
        let hit = physics.raycast(origin, dir, range)
        if hit == nil { return }
        if impact != nil {
            impact.worldPosition = hit.point
            impact.emit()
            audio.playAt("StarterContent/Sounds/Props/Explosion", hit.point, 0.9)
        }
        hit.entity.addImpulse(dir * impactImpulse, hit.point)
        if incendiary { fire.ignite(hit.point, 0.4) }
    }
}
)zs"},
    {"Door.zs", R"zs(// A door on a Hinge Joint: press E near it to open or close (the joint's motor turns it).
script Door {
    @tooltip("Name of the player entity")
    prop player = "Player"
    @range(0.5, 10) @tooltip("How close the player must be (m)")
    prop reach = 2.5
    @range(0.1, 10) @tooltip("Turning speed (radians per second)")
    prop speed = 1.8
    @range(1, 100000) prop torque = 600.0
    prop key = "E"
    prop openSound = "StarterContent/Sounds/Props/Door_Open"
    prop closeSound = "StarterContent/Sounds/Props/Door_Close"

    var open = false

    fn start() {
        entity.setMotor(-speed, torque) // held shut against the hinge's lower limit
    }

    fn update(dt) {
        let p = find(player)
        if p == nil { return }
        if input.pressed(key) and p.worldPosition.distance(entity.worldPosition) < reach {
            open = not open
            if open {
                entity.setMotor(speed, torque)
                audio.playAt(openSound, entity.worldPosition)
            } else {
                entity.setMotor(-speed, torque)
                audio.playAt(closeSound, entity.worldPosition)
            }
        }
    }
}
)zs"},
    {"Elevator.zs", R"zs(// A platform on a Slider Joint going up and down between its limits, waiting at each end.
script Elevator {
    @range(0.5, 50) @tooltip("Travel (m): the slider's upper limit")
    prop height = 4.0
    @range(0.1, 10) @tooltip("Meters per second")
    prop speed = 1.5
    @range(0, 30) @tooltip("Seconds at each end")
    prop wait = 2.0
    @range(100, 10000000) prop force = 200000.0

    var dir = 1
    var timer = 0.0

    fn start() {
        timer = wait
        entity.setMotor(0, force) // hold
    }

    fn update(dt) {
        if timer > 0 {
            timer -= dt
            if timer <= 0 { entity.setMotor(speed * dir, force) }
            return
        }
        let at = entity.jointValue
        if (dir > 0 and at >= height - 0.02) or (dir < 0 and at <= 0.02) {
            dir = -dir
            timer = wait
            entity.setMotor(0, force)
        }
    }
}
)zs"},
    {"Push.zs", R"zs(// Gives the entity's Rigid Body a push when the game starts (swinging lamps, rolling balls).
script Push {
    @tooltip("Impulse in world space (N*s)")
    prop impulse = vec3(8, 0, 4)

    fn start() {
        entity.addImpulse(impulse)
    }
}
)zs"},
    {"JumpPad.zs", R"zs(// Throws whatever enters this trigger upwards (characters and Rigid Bodies), with a burst of particles.
script JumpPad {
    @range(1, 50) @tooltip("Upward speed (m/s)")
    prop strength = 14.0

    fn triggerEnter(other) {
        other.launch(vec3(0, strength, 0))
        entity.emit(30)
        audio.playAt("StarterContent/Sounds/Props/Force_Field", entity.worldPosition, 0.8)
    }
}
)zs"},
    {"Coin.zs", R"zs(// A spinning pickup: the player collects it by walking through (it bursts into sparkles).
script Coin {
    @range(0, 720) @tooltip("Degrees per second")
    prop spinSpeed = 120.0
    @tooltip("Name of the player entity")
    prop player = "Player"

    var base = vec3(0)
    var collected = false
    var timer = 0.0

    fn start() {
        base = entity.position
    }

    fn update(dt) {
        if collected {
            timer -= dt
            if timer <= 0 { entity.destroy() }
            return
        }
        entity.rotate(vec3(0, 1, 0), spinSpeed * dt)
        entity.position = base + vec3(0, sin(time.elapsed * 2.5) * 0.12, 0)
    }

    fn triggerEnter(other) {
        if collected or other.name != player { return }
        collected = true
        timer = 1.5
        entity.emit(40)
        audio.playAt("StarterContent/Sounds/Props/Coins", entity.worldPosition, 0.8)
        entity.scale = vec3(0.001) // hidden; the sparkles finish before it is removed
        log("Coin collected")
    }
}
)zs"},
    {"PhysicsGun.zs", R"zs(// Physics gun: left click pushes what you look at, hold the right button to carry a Rigid Body.
// Put it on a camera (it aims along the entity's forward axis).
script PhysicsGun {
    @range(0, 5000) @tooltip("Push impulse (N*s)")
    prop push = 300.0
    @range(1, 100) @tooltip("How far you can reach (m)")
    prop reach = 30.0
    @range(1, 30) @tooltip("How firmly a carried body follows (1/s)")
    prop grip = 12.0

    var held = nil
    var holdDistance = 3.0

    fn update(dt) {
        let dir = entity.worldForward
        let eye = entity.worldPosition + dir * 0.4
        if input.mousePressed("left") {
            let hit = physics.raycast(eye, dir, reach)
            if hit != nil {
                hit.entity.addImpulse(dir * push, hit.point)
            }
        }
        if input.mousePressed("right") {
            let hit = physics.raycast(eye, dir, 8.0)
            if hit != nil {
                held = hit.entity
                holdDistance = max(hit.distance, 2.0)
            }
        }
        if not input.mouse("right") {
            held = nil
        }
        if held != nil {
            held.velocity = (eye + dir * holdDistance - held.worldPosition) * grip
            held.angularVelocity = held.angularVelocity * 0.8
        }
    }
}
)zs"},
    {"Scanner.zs", R"zs(// Raycast demo: sweeps the child "Head" back and forth; its "Beam" reaches as far as the ray goes and "Dot"
// sits where it hits.
script Scanner {
    @range(0, 180) @tooltip("Sweep angle each way (degrees)")
    prop sweep = 50.0
    @range(0, 5) prop speed = 0.7
    @range(1, 200) prop range = 25.0

    var head = nil
    var beam = nil
    var dot = nil

    fn start() {
        head = entity.child("Head")
        beam = entity.child("Beam")
        dot = entity.child("Dot")
    }

    fn update(dt) {
        if head == nil { return }
        head.rotation = quat.euler(0, sin(time.elapsed * speed) * sweep, 0)
        let dir = head.worldForward
        let hit = physics.raycast(head.worldPosition + dir * 0.4, dir, range)
        var length = range
        if hit != nil {
            length = hit.distance + 0.4
        }
        if beam != nil {
            beam.position = vec3(0, 0, length * 0.5)
            beam.scale = vec3(0.04, 0.04, length)
        }
        if dot != nil {
            dot.position = vec3(0, 0, length)
        }
    }
}
)zs"},
    {"FireStarter.zs", R"zs(// Lighter and water: F sets what you look at on fire (Flammable objects), G pours water on it.
// Put it on a camera (it aims along the entity's forward axis).
script FireStarter {
    prop igniteKey = "F"
    prop extinguishKey = "G"
    @range(1, 100) @tooltip("How far you can reach (m)")
    prop reach = 12.0
    @range(0.1, 10) @tooltip("Radius the water puts out (m)")
    prop splash = 1.5

    fn update(dt) {
        let ignite = input.pressed(igniteKey)
        let douse = input.pressed(extinguishKey)
        if not ignite and not douse { return }
        let dir = entity.worldForward
        let hit = physics.raycast(entity.worldPosition + dir * 0.5, dir, reach)
        if hit == nil { return }
        if ignite {
            if fire.ignite(hit.point, 0.3) == 0 { log("Nothing flammable there") }
        } else {
            fire.extinguish(hit.point, splash)
        }
    }
}
)zs"},
};

u32 WriteScripts(const String& dir, Span<const StarterScript> scripts)
{
    platform::CreateDirectoryTree(dir);
    u32 written = 0;
    for (const StarterScript& s : scripts)
        written += fs::WriteTextFile(String::Format("{}/{}", dir, s.file), s.source) ? 1u : 0u;
    return written;
}

} // namespace

u32 WriteGameplayScripts(StringView contentDir)
{
    return WriteScripts(Path::Join(contentDir, "Scripts"), Span<const StarterScript>(GameplayScripts, ZX_ARRAY_COUNT(GameplayScripts)));
}

// --- Sounds (synthesized, so the starter content needs no recorded files) -------------------------------------------------

namespace {

/// Makes a looping sound seamless: the last `fade` frames are cross-faded into the start and then dropped.
void MakeLoop(audio::PcmData& p, u32 fadeFrames)
{
    const u32 frames = u32(p.samples.Size() / p.channels);
    const u32 keep = frames - fadeFrames;
    for (u32 i = 0; i < fadeFrames; ++i) {
        const f32 t = (f32(i) + 0.5f) / f32(fadeFrames);
        for (u32 c = 0; c < p.channels; ++c) {
            f32& head = p.samples[usize(i) * p.channels + c];
            const f32 tail = p.samples[usize(keep + i) * p.channels + c];
            head = head * Sqrt(t) + tail * Sqrt(1.0f - t);
        }
    }
    p.samples.Resize(usize(keep) * p.channels);
}

audio::PcmData Synth(u32 rate, f32 seconds, const Function<f32(f32 t, u32 i), 32>& f)
{
    audio::PcmData p;
    p.sampleRate = rate;
    p.channels = 1;
    const u32 frames = u32(seconds * f32(rate));
    p.samples.Resize(frames);
    for (u32 i = 0; i < frames; ++i)
        p.samples[i] = f(f32(i) / f32(rate), i);
    return p;
}

/// Copies Engine/Content/StarterContent/<sub> (recursively) into `dir`. Returns the files copied.
u32 CopyLibrary(StringView sub, const String& dir)
{
    const String library = FindEngineRootDirectory() + "/Engine/Content/StarterContent/" + sub;
    u32 copied = 0;
    Array<String> dirs;
    dirs.PushBack(String());
    platform::CreateDirectoryTree(dir);
    for (usize d = 0; d < dirs.Size(); ++d) {
        Array<platform::DirectoryEntry> entries;
        platform::ListDirectory(library + dirs[d], entries);
        for (const platform::DirectoryEntry& e : entries) {
            const String rel = dirs[d] + "/" + e.name;
            if (e.isDirectory) {
                platform::CreateDirectoryTree(dir + rel);
                dirs.PushBack(rel);
            } else if (platform::CopyFileAt(library + rel, dir + rel)) {
                ++copied;
            }
        }
    }
    return copied;
}

u32 WriteSounds(const String& dir)
{
    platform::CreateDirectoryTree(dir);
    u32 written = 0;
    auto save = [&](StringView name, const audio::PcmData& p) {
        written += fs::WriteFile(String::Format("{}/{}", dir, name), audio::EncodeWav(p).View()) ? 1u : 0u;
    };
    Random rng(77);
    // UI click: a short bright blip.
    save("UI_Click.wav", Synth(44100, 0.06f, [](f32 t, u32) {
        return 0.6f * std::sin(TwoPi * 1800.0f * t) * std::exp(-t * 90.0f) + 0.2f * std::sin(TwoPi * 3600.0f * t) * std::exp(-t * 160.0f);
    }));
    // Chime: inharmonic bell partials with their own decays.
    save("Chime.wav", Synth(44100, 2.5f, [](f32 t, u32) {
        const f32 partials[4][3] = {{1.0f, 0.5f, 1.6f}, {2.76f, 0.25f, 2.6f}, {5.40f, 0.12f, 4.0f}, {8.93f, 0.06f, 6.0f}};
        f32 s = 0.0f;
        for (const auto& p : partials)
            s += p[1] * std::sin(TwoPi * 660.0f * p[0] * t) * std::exp(-t * p[2]);
        return s * Min(1.0f, t * 400.0f);
    }));
    // Hum: 110 Hz with harmonics and a slow wobble; 2 s holds whole cycles, so it loops without a seam.
    save("Hum_Loop.wav", Synth(44100, 2.0f, [](f32 t, u32) {
        const f32 wobble = 0.85f + 0.15f * std::sin(TwoPi * 2.0f * t);
        return wobble * (0.35f * std::sin(TwoPi * 110.0f * t) + 0.15f * std::sin(TwoPi * 220.0f * t) + 0.08f * std::sin(TwoPi * 330.0f * t));
    }));
    // Recorded sounds (CC0, Engine/Content/StarterContent/Sounds: ambiences, footsteps, impacts, props).
    written += CopyLibrary("Sounds", dir);
    if (written > 3)
        return written; // the synthesized ambiences below are only a fallback
    // Wind: low-passed noise with a slowly moving cutoff and level.
    {
        f32 lp = 0.0f, lp2 = 0.0f;
        audio::PcmData wind = Synth(22050, 6.5f, [&](f32 t, u32) {
            const f32 gust = 0.5f + 0.3f * std::sin(TwoPi * 0.11f * t) + 0.2f * std::sin(TwoPi * 0.27f * t + 1.0f);
            const f32 coef = 0.02f + 0.06f * gust;
            lp += (rng.Range(-1.0f, 1.0f) - lp) * coef;
            lp2 += (lp - lp2) * coef;
            return lp2 * 3.0f * gust;
        });
        MakeLoop(wind, 22050 / 2);
        save("Wind_Loop.wav", wind);
    }
    // Ocean: deep rumble plus swells of hiss as waves break.
    {
        f32 low = 0.0f, mid = 0.0f;
        audio::PcmData ocean = Synth(22050, 8.5f, [&](f32 t, u32) {
            const f32 n = rng.Range(-1.0f, 1.0f);
            low += (n - low) * 0.01f;
            mid += (n - mid) * 0.12f;
            const f32 swell = std::pow(Max(0.0f, std::sin(TwoPi * 0.125f * t)), 2.0f) + 0.6f * std::pow(Max(0.0f, std::sin(TwoPi * 0.2f * t + 2.0f)), 3.0f);
            return low * 2.2f + mid * 0.35f * (0.25f + swell);
        });
        MakeLoop(ocean, 22050 / 2);
        save("Waves_Loop.wav", ocean);
    }
    // Fire: a soft roar (low-passed noise) with random crackles (short decaying clicks).
    {
        f32 roar = 0.0f, roar2 = 0.0f, crackle = 0.0f, crackleFreq = 0.0f, phase = 0.0f;
        audio::PcmData fire = Synth(22050, 6.0f, [&](f32 t, u32) {
            const f32 n = rng.Range(-1.0f, 1.0f);
            roar += (n - roar) * 0.03f;
            roar2 += (roar - roar2) * 0.05f;
            if (rng.NextFloat() < 0.0009f) {
                crackle = rng.Range(0.3f, 1.0f);
                crackleFreq = rng.Range(900.0f, 3200.0f);
            }
            crackle *= 0.9965f;
            phase += TwoPi * crackleFreq / 22050.0f;
            const f32 breath = 0.8f + 0.2f * std::sin(TwoPi * 0.3f * t);
            return roar2 * 2.5f * breath + crackle * (0.5f * std::sin(phase) + 0.5f * n) * 0.5f;
        });
        MakeLoop(fire, 22050 / 4);
        save("Fire_Loop.wav", fire);
    }
    return written;
}

} // namespace

u32 WriteStarterContent(StringView contentDir)
{
    const String root = Path::Join(contentDir, "StarterContent");
    platform::CreateDirectoryTree(root + "/Materials");
    platform::CreateDirectoryTree(root + "/Scripts");
    u32 written = 0;
    for (const StarterMaterial& m : Materials) {
        const render::MaterialGraph g = MakeMaterial(m);
        written += fs::WriteTextFile(String::Format("{}/Materials/{}.zmat", root, m.name), ToJsonString(g.ToJson())) ? 1u : 0u;
    }
    for (const PatternMaterial& m : Patterns) {
        const render::MaterialGraph g = MakePatternMaterial(m);
        written += fs::WriteTextFile(String::Format("{}/Materials/{}.zmat", root, m.name), ToJsonString(g.ToJson())) ? 1u : 0u;
    }
    // Textures (relief demos) and color grading LUTs shipped with the engine, and the materials using them.
    written += CopyLibrary("Textures", root + "/Textures");
    written += CopyLibrary("LUTs", root + "/LUTs");
    written += CopyLibrary("Flipbooks", root + "/Flipbooks");
    const render::MaterialGraph textured[] = {MakeReliefMaterial("M_Bricks", "T_Bricks", 2.0f, 0.04f, 0.85f),
                                              MakeReliefMaterial("M_Cobblestone", "T_Cobble", 1.0f, 0.05f, 0.75f),
                                              MakeLeavesMaterial(), MakeTerrainMaterial()};
    const char* texturedNames[] = {"M_Bricks", "M_Cobblestone", "M_Leaves", "M_Terrain"};
    for (u32 i = 0; i < ZX_ARRAY_COUNT(texturedNames); ++i)
        written += fs::WriteTextFile(String::Format("{}/Materials/{}.zmat", root, texturedNames[i]), ToJsonString(textured[i].ToJson())) ? 1u : 0u;
    written += WriteScripts(root + "/Scripts", Span<const StarterScript>(Scripts, ZX_ARRAY_COUNT(Scripts)));
    written += WriteScripts(root + "/Scripts", Span<const StarterScript>(GameplayScripts, ZX_ARRAY_COUNT(GameplayScripts)));
    written += WriteScripts(root + "/Scripts", Span<const StarterScript>(PropScripts, ZX_ARRAY_COUNT(PropScripts)));
    written += WriteSounds(root + "/Sounds");
    return written;
}

} // namespace zx
