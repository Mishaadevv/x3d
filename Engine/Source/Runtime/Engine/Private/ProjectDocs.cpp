// Documentation written into new projects: a guide for people (Docs/) and instructions for AI coding assistants
// (AGENTS.md, CLAUDE.md). Everything is plain Markdown the team can edit.

#include "Core/FileSystem/FileSystem.h"
#include "Core/HAL/PlatformFile.h"
#include "Core/String/Path.h"
#include "Core/Version.h"
#include "Engine/Project.h"

namespace zx {

namespace {

const char* const ReadmeTemplate = R"md(# {name}

A game made with the ZeqouX3D engine {version}.

## Open the project

Start **ZXEditor** and pick the project in the Project Browser, or run:

```
ZXEditor.exe -project="{descriptor}"
```

## Layout

| Path | What it is |
|---|---|
| `{name}.zproj` | Project descriptor (name, engine version, startup scene) |
| `Content/` | Everything the game uses: scenes, models, textures, materials, scripts |
| `Content/Scenes/` | Levels (`.zscene`, JSON) |
| `Content/Scripts/` | Gameplay scripts in ZeqouScript (`.zs`) |
| `Content/StarterContent/` | Example materials and scripts (if added) |
| `Source/<Name>/` | C++ game code (if the project has a game module), built to `Binaries/` |
| `Config/Input.json` | Input Actions (Jump, MoveForward, ...) - edit in Window > Input Actions |
| `Docs/` | This documentation |
| `Saved/` | Editor layout, logs, caches - not meant for version control |

More: [Getting started](Docs/GettingStarted.md), [ZeqouScript reference](Docs/ZeqouScript.md),
[C++ game code](Docs/CppGameCode.md).
)md";

const char* const GettingStarted = R"md(# Getting started

## The editor

- **Viewport** - hold the right mouse button to fly (WASD, Q/E down/up, wheel = speed). W/E/R switch move/rotate/scale.
- **Outliner** - the entities of the scene; drag one onto another to parent it.
- **Details** - the components of the selected entity. **Add Component** adds Mesh Renderer, lights, Camera,
  Character Controller, Water, Script and more.
- **Content Browser** - the `Content/` folder. Drag models into the viewport, double-click materials and scripts to edit.
  Right-click for New Material, New Script, New Folder.
- **Output Log** - messages and the console (type `help`).
- **Ctrl+P** - find any asset, **Ctrl+Shift+P** - run any command. Shortcuts live in `Saved/Config/Keys.ini`.

## Playing

- **F5** Play / Stop, **F6** Pause, **F10** one frame, **Esc** Stop.
- With a Camera in the scene the game view is shown: click the viewport to give the game the mouse,
  **Shift+F1** releases it, **F8** switches between the game camera and the editor camera.
- Everything changed while playing is reset when you stop.

## Making things move

1. Content Browser > right-click > **New Script** (opens the Script Editor).
2. Select an entity > **Add Component > Script** > drop the `.zs` file into the Script slot.
3. Values declared with `prop` appear in the Details panel and can differ per entity.
4. Press Play. Saving a script while playing reloads it immediately.

See [ZeqouScript.md](ZeqouScript.md) for the language.

## Characters

Add a **Character Controller** and a script that calls `character.move(velocity)` / `character.jump(speed)`.
The First/Third Person templates (and Starter Content) include ready controllers. The character is a capsule moving
through the physics world: it slides along walls, climbs steps up to **Step Height** and slopes up to **Max Slope**,
rides moving platforms and pushes Rigid Bodies (light ones move, heavy ones don't - see **Push Mass**).

## Look of the game

**Create > Post Process Volume** changes exposure, color grading, bloom, depth of field and motion blur inside its box
(or everywhere with **Unbound**). Tick the box in front of a setting to override it; volumes blend over Blend Radius
and higher Priority wins. Viewport **Lit > Depth of Field Focus** shows what is in focus.

**Create > Reflection Probe** gives shiny surfaces inside its box real reflections of the room around them (size the
box to the room: floors and walls then line up). Probes update by themselves when the scene changes.

**Create > Decal** projects an image onto the ground below it (rotate the entity to project onto walls): drop an image
from the Content Browser into its Texture field; its alpha channel decides where it covers.

**Physics**: built-in shapes (Create > Cube, Sphere...) come with a collider. Add a **Rigid Body** component to make
an object fall, collide and get pushed (Kinematic = moved by your code, pushes others). Colliders without a Rigid Body
are static; colliders on children of a Rigid Body belong to it. Meshes with **Collision** ticked in their Mesh
Renderer are solid level geometry automatically. The console command `physics.spawn 50` drops a pile to try it.

**HDRI Sky**: double-click an HDR panorama (`.hdr` / `.exr`, lat-long) in the Content Browser, or drag it into the
viewport, and it becomes the sky - background, sky light, fog color, water and reflections. The sun found in the
panorama lights the scene with shadows (**Sun**: From HDRI / Scene Sun / None); turn it with **Rotation**, set the
brightness with **Intensity**. Free panoramas: polyhaven.com/hdris.

Textures with transparency are compressed as BC7 (sharper than BC3 at the same size), HDR images as BC6H. A texture's
`.zmeta` file can force a format: `"settings": { "compression": "BC7" }` (Auto, BC1, BC3, BC4, BC5, BC7, None).

## Input Actions

**Window > Input Actions** names the controls of the game: buttons (`Jump`, `Fire`, ...) and axes (`MoveForward`,
`LookX`, ...) bound to keys, mouse and gamepad. Scripts read them with `input.action("Jump")`,
`input.actionPressed("Jump")` and `input.axis("MoveForward")`, C++ with `host->ActionDown("Jump")` / `host->Axis(...)`.
They are saved in `Config/Input.json`.

## C++

Tools > **Create C++ Game Module** adds `Source/<Name>/` with example components; **Compile** (F7, the hammer next to
Play) builds it in the background and reloads it - also while the game is playing. See [CppGameCode.md](CppGameCode.md).
)md";

const char* const CppReference = R"md(# C++ game code

A project can have a **game module**: C++ components in `Source/<Name>/`, built into `Binaries/<Name>.dll` and loaded
by the editor. Every `.cpp` / `.h` in that folder is part of it.

Requirements: Visual Studio 2022 or its Build Tools with the C++ workload (CMake and Ninja come with it).

## A component

```cpp
#include "ZeqouGame.h"
using namespace zx::game;

class Spinner : public ScriptComponent
{
public:
    float speed = 90.0f;                 // a prop: edited per entity in Details
    Vec3 axis = Vec3(0, 1, 0);

    void OnStart() override {}           // once, when the game starts
    void OnUpdate(float dt) override     // every frame
    {
        SetRotation(Rotation() * Quat::AxisAngle(axis, speed * dt));
    }
};
ZX_GAME_COMPONENT(Spinner, "Spins the entity")
    ZX_GAME_PROP(speed, -720, 720, "Degrees per second")
    ZX_GAME_PROP(axis, 0, 0, "Rotation axis")
ZX_GAME_COMPONENT_END()

ZX_IMPLEMENT_GAME_MODULE("{name}")   // once per module
```

Attach it with **Add Component > C++ Script** and pick the class. Props can be `float`, `int`, `bool` or `Vec3`
(min = max means no range).

## Compile and hot reload

Press **F7** (or the hammer next to Play). The build runs in the background; when it succeeds the new code is loaded.
While playing, running components are recreated on the new code: props keep their values, `OnReloaded()` is called
instead of `OnStart()`, other members start from their defaults. Errors appear in the Output Log
(full output: `Saved/Logs/GameBuild.log`).

## What a component can use

Members: `entity`, `host`; helpers `Position() SetPosition() Rotation() SetRotation() Forward() Right() Rotate(euler)
Translate(local) Name() Log(text)`.

`host->` (the engine):
| Area | Functions |
|---|---|
| Entities | `Find("Name")`, `IsAlive(e)`, `Destroy(e)`, `Spawn("Cube" / "Sphere" / "Cylinder" / "Plane" / "", position)`, `GetName(e)`, `Get/SetPosition`, `Get/SetRotation`, `Get/SetScale` |
| Input | `KeyDown("W")`, `KeyPressed("Space")`, `ActionDown("Jump")`, `ActionPressed("Jump")`, `Axis("MoveForward")`, `MouseDelta()` |
| Time | `DeltaTime()`, `Time()` |
| Character | `CharacterMove(e, velocity)`, `CharacterJump(e, speed)`, `CharacterGrounded(e)` (needs a Character Controller) |
| Sound | `PlaySound("Sounds/x.wav", volume, spatial, position)` -> voice, `StopSound(voice)` |
| Physics | `AddForce(e, force)`, `AddImpulse(e, impulse)`, `Get/SetVelocity(e)` (need a Rigid Body), `Raycast(origin, direction, maxDistance, &result, layerMask)` |

Physics callbacks to override: `OnCollisionEnter(other, speed)`, `OnCollisionExit(other)`, `OnTriggerEnter(other)`,
`OnTriggerExit(other)`.
| Log | `Log(text, isError)` |

Math: `Vec3` (`+ - * /`, `Length() Normalized()`, `Dot Cross Distance Lerp`), `Quat` (`Euler(pitch, yaw, roll)` in
degrees, `AxisAngle(axis, degrees)`, `q * q`, `q * v`).

Code runs inside the editor: a crash in a component takes the editor down, so save your scene before experimenting.
)md";

const char* const ScriptReference = R"md(# ZeqouScript reference

ZeqouScript (`.zs`) is the scripting language of ZeqouX3D: small, readable, dynamically typed, hot-reloaded.

```zs
script Spinner {
    @range(-360, 360) @tooltip("Degrees per second")
    prop speed = 90.0        // editable per entity in Details
    var turns = 0            // state of this instance

    fn start() { log("Spinner on " + entity.name) }

    fn update(dt) {
        entity.rotate(vec3(0, speed * dt, 0))
        if input.pressed("Space") { turns += 1 }
    }
}
```

## File structure
- One `script Name { ... }` per file. Inside: `prop`, `var`/`let` fields and `fn` functions in any order.
- `prop name = literal` - a number, bool, string or `vec3(...)`; annotations: `@range(min, max)`, `@tooltip("text")`,
  `@color` (vec3 shown as a color picker).
- The engine calls `start()` once and `update(dt)` every frame while playing (both optional).
- Physics callbacks (optional): `collisionEnter(other, speed)` / `collisionExit(other)` when the entity's colliders
  touch another solid body, `triggerEnter(other)` / `triggerExit(other)` for colliders with Is Trigger.

## Statements and operators
- `if a { } elif b { } else { }`, `while cond { }`, `for i in 0..10 { }`, `for item in list { }`, `break`, `continue`, `return`.
- `let`/`var` declare locals (block scope). Assignment: `= += -= *= /=`, also nested: `transform.position.y += 1`.
- `+ - * / %`, `== != < <= > >=`, `and or not` (also `&& || !`). Only `nil`, `false` and `0` are false.
- Strings join with `+`. Comments: `//` and `/* */`.

## Values
| Type | Members / methods |
|---|---|
| number | - |
| string | `len() upper() lower() contains(s) startsWith(s)`, `s[i]` |
| vec3 | `.x .y .z`, `length() lengthSquared() normalized() dot(b) cross(b) distance(b) lerp(b, t)`, `+ - * /` |
| quat | `forward() right() up() inverse() euler() rotate(v) slerp(b, t)`, `q * v`, `q1 * q2` |
| list | `len() push(v) pop() insert(i, v) remove(i) contains(v) indexOf(v) clear()`, `l[i]`, `l[-1]` |

vec3/quat are values (copied); lists and strings are references.

## Engine objects
| Name | Use |
|---|---|
| `entity` / `transform` | `name position rotation scale forward right up alive parent` (local), `worldPosition` (read / write) `worldRotation worldForward`; `rotate(eulerDeg)`, `rotate(axis, deg)`, `translate(v[, world])`, `lookAt(point)`, `child("Name")`, `destroy()` |
| `find("Name")` | another entity (or `nil`) |
| `character` | the Character Controller: `move(velocity)`, `jump(speed)`, `grounded`, `velocity` |
| `input` | `key("W")`, `pressed("Space")`, `released(...)`, `mouse("left")`, `mousePressed(...)`, `move` (WASD/stick vec3), `look`, `mouseDelta`, `mousePosition`, `wheel`; Input Actions: `action("Jump")`, `actionPressed(...)`, `actionReleased(...)`, `axis("MoveForward")` |
| `time` | `dt elapsed frame` |
| `audio` | `play(file[, volume])` (a path without an extension plays a random variant `_0 .. _9`), `playAt(file, position[, volume])`, `stop(id)`, `setVolume(bus, volume)`; `entity.playSound()` / `stopSound()` with an Audio Source |
| Rigid Body | `entity.velocity`, `entity.angularVelocity` (read / write), `entity.addForce(v)`, `entity.addImpulse(v[, point])`, `entity.addTorque(v)`; joints: `entity.jointValue` (hinge angle in radians / slider offset / distance), `entity.setMotor(speed[, force])`, `entity.setMotor(nil)`; `entity.launch(velocity)` (throws a character / sets a body's velocity) |
| Particle Emitter | `entity.emit([count])` (burst), `entity.emitting` (read / write) |
| Animator | `entity.animParam(name, value)`, `entity.animTrigger(name)`, `entity.playAnim(clip)`, `entity.animState()` (on the entity or a descendant; a Character Controller sets Speed / VerticalSpeed / Grounded) |
| Flammable | `entity.ignite([point])` (false if burnt out), `entity.extinguish()`, `entity.burning`, `entity.burnt`, `entity.fire` (0..1 in flames), `entity.temperature` (C) |
| `fire` | `ignite(point[, radius])` / `extinguish(point[, radius])` -> how many objects caught fire / went out; `burning` (count) |
| `physics` | `raycast(origin, direction[, maxDistance[, layers]])` / `sphereCast(origin, radius, direction[, maxDistance[, layers]])` -> hit (`entity point normal distance`) or `nil`; `overlapSphere(center, radius[, layers])` -> list of entities; `gravity`. `layers`: a layer number or a list (`[0, 2]`); triggers are not hit |

Positions are local to the parent; Y is up, +Z forward, meters, rotations in degrees in the API.

## Standard library
`sin cos tan asin acos atan atan2 sqrt abs floor ceil round exp sign fract pow min max clamp lerp smoothstep deg rad PI`,
`random() random(a, b) randomInt(a, b)`, `vec3(...)`, `quat.euler(p, y, r) quat.axisAngle(axis, deg) quat.lookAt(dir) quat.identity`,
`log(...) print(...) str(v) num(s) len(x) type(v)`.

## Errors
Compile errors show in the Script Editor while typing and in Details. Runtime errors print `file:line: message` to the
Output Log and stop only that script instance.
)md";

const char* const AgentsTemplate = R"md(# Instructions for AI coding assistants

This is a game project for the **ZeqouX3D** engine ({version}). Read this before changing anything.

## What lives where
- `{name}.zproj` - project descriptor (JSON). Don't rename it.
- `Content/Scenes/*.zscene` - scenes as JSON: `entities[]` with a `guid`, optional `parent`, and one object per
  component (`Transform`, `MeshRenderer`, `PointLight`, `SpotLight`, `Camera`, `CharacterController`, `Water`, `Script`,
  `NativeScript` (C++), `AudioSource`, `AudioListener`, `ReverbZone`, `Terrain` (generated hills: size, height, seed, ...), `PostProcessVolume`, `ReflectionProbe`, `Decal`, `HdriSky`,
  `RigidBody`, `BoxCollider`, `SphereCollider`, `CapsuleCollider`, `CylinderCollider`, `MeshCollider` (`convex: true` = hull, works on a dynamic RigidBody), `HingeJoint`, `FixedJoint`,
  `SliderJoint`, `BallJoint`, `DistanceJoint`, `ConfigurableJoint` (`connectedBody` = entity name, empty = world), `ParticleEmitter`, `Flammable`, `ImpactSound` (`rollSound` = loop while rolling / sliding), `DirectionalLight` (the sun: its rotation), `SkyLight`, `HeightFog`, `FogVolume`, `VolumetricClouds`, `FluidVolume`,
  `Rotator`, `Oscillator`, ...). Prefer editing scenes in the editor; when editing JSON keep guids unique and stable.
- `Content/Scripts/*.zs` - gameplay code in **ZeqouScript** (see `Docs/ZeqouScript.md`). This is where most work happens.
- `Content/**/*.zmat` / `.zmati` - material graphs / instances (JSON node graphs). `.zprefab` - prefabs.
- `Source/<Name>/` - C++ game module, if the project has one (see `Docs/CppGameCode.md`).
- `Config/Input.json` - Input Actions (named buttons and axes).
- `Saved/`, `Intermediate/`, `Binaries/` - generated; never commit or edit.

## Writing gameplay code
- Gameplay is ZeqouScript (`.zs`, one `script Name { }` per file) and, in projects with a game module, C++ components
  (`Source/<Name>/`, `ScriptComponent` subclasses registered with `ZX_GAME_COMPONENT`; build with
  `powershell -ExecutionPolicy Bypass -File <engine>/Tools/build_game.ps1 -Project <project dir> -Name <Name>`).
- Read player input through Input Actions (`input.action("Jump")`, `input.axis("MoveForward")`; C++ `host->ActionDown`,
  `host->Axis`) rather than hard-coded keys.
- Expose tunables as `prop` with `@range`/`@tooltip`; keep per-instance state in `var` fields.
- Use the engine objects exactly as documented in `Docs/ZeqouScript.md` (`entity`, `character`, `input`, `time`,
  `find`). There are no other globals; unknown names are runtime errors.
- Movement of characters goes through `character.move(velocity)` / `character.jump(speed)`, not by setting positions.
- Attach a script to an entity with a `Script` component: `{ "file": "Scripts/Name.zs", "properties": "{}" }`
  (`properties` holds only values that differ from the `prop` defaults, as a JSON string).

## Checking your work
- Open the project: `ZXEditor.exe -project=<path to {name}.zproj>`; the Script Editor and Output Log show errors with
  file and line.
- Automated run: `ZXEditor.exe -project=<zproj> -autotest=300 -exec=play` plays for 300 frames and exits (exit code 0 = no
  errors); the log is in `Saved/Logs/ZXEditor.log`.
- Standalone build: `ZXEditor.exe -project=<zproj> -autotest=30 -exec="package"` writes the game to `Builds/Windows/<Name>`
  (`<Name>.exe -autotest=300 -windowed` runs it; its log is in `%LOCALAPPDATA%/<Name>/Saved/Logs/Game.log`).

## Conventions
- All user-facing text (names, logs, UI strings in scripts) in English unless the team says otherwise.
- Units: meters, seconds, degrees in script APIs; Y up, +Z forward (left-handed).
)md";

String Fill(StringView text, const ProjectInfo& p)
{
    String s(text);
    s.ReplaceAll("{name}", p.descriptor.name);
    s.ReplaceAll("{version}", EngineVersionString);
    s.ReplaceAll("{descriptor}", p.descriptorPath);
    return s;
}

} // namespace

u32 WriteProjectDocs(const ProjectInfo& project)
{
    platform::CreateDirectoryTree(project.root + "/Docs");
    u32 written = 0;
    auto write = [&](StringView rel, StringView text) { written += fs::WriteTextFile(Path::Join(project.root, rel), Fill(text, project)) ? 1u : 0u; };
    write("README.md", ReadmeTemplate);
    write("Docs/GettingStarted.md", GettingStarted);
    write("Docs/ZeqouScript.md", ScriptReference);
    write("Docs/CppGameCode.md", CppReference);
    write("AGENTS.md", AgentsTemplate);
    write("CLAUDE.md", "# Claude Code\n\nSee [AGENTS.md](AGENTS.md) - the instructions for AI assistants working on this project.\n");
    return written;
}

} // namespace zx
