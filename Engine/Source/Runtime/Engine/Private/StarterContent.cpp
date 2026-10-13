// Starter Content: example materials and scripts copied into new projects (all plain files the user can edit).

#include "Core/FileSystem/FileSystem.h"
#include "Core/HAL/PlatformFile.h"
#include "Core/String/Path.h"
#include "Engine/Project.h"
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
    {"M_Concrete", Vec4(0.5f, 0.49f, 0.46f, 1), 0.0f, 0.85f, Vec4(0)},
    {"M_Grass", Vec4(0.16f, 0.36f, 0.08f, 1), 0.0f, 0.8f, Vec4(0)},
    {"M_Wood", Vec4(0.45f, 0.28f, 0.14f, 1), 0.0f, 0.6f, Vec4(0)},
    {"M_Sand", Vec4(0.76f, 0.66f, 0.47f, 1), 0.0f, 0.9f, Vec4(0)},
    {"M_Rock", Vec4(0.32f, 0.31f, 0.3f, 1), 0.0f, 0.75f, Vec4(0)},
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

    var yaw = 0.0
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
    written += WriteScripts(root + "/Scripts", Span<const StarterScript>(Scripts, ZX_ARRAY_COUNT(Scripts)));
    written += WriteScripts(root + "/Scripts", Span<const StarterScript>(GameplayScripts, ZX_ARRAY_COUNT(GameplayScripts)));
    return written;
}

} // namespace zx
