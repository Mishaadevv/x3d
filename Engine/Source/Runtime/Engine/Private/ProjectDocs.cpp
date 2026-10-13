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
| `Docs/` | This documentation |
| `Saved/` | Editor layout, logs, caches - not meant for version control |

More: [Getting started](Docs/GettingStarted.md), [ZeqouScript reference](Docs/ZeqouScript.md).
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
The First/Third Person templates (and Starter Content) include ready controllers. Collision uses the boxes of meshes
with **Collision** enabled in their Mesh Renderer.
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
| `entity` / `transform` | `name position rotation scale forward right up alive parent`; `rotate(eulerDeg)`, `rotate(axis, deg)`, `translate(v[, world])`, `lookAt(point)`, `child("Name")`, `destroy()` |
| `find("Name")` | another entity (or `nil`) |
| `character` | the Character Controller: `move(velocity)`, `jump(speed)`, `grounded`, `velocity` |
| `input` | `key("W")`, `pressed("Space")`, `released(...)`, `mouse("left")`, `mousePressed(...)`, `move` (WASD/stick vec3), `look`, `mouseDelta`, `mousePosition`, `wheel` |
| `time` | `dt elapsed frame` |
| `audio` | `play(file[, volume])`, `playAt(file, position[, volume])`, `stop(id)`, `setVolume(bus, volume)`; `entity.playSound()` / `stopSound()` with an Audio Source |

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
  `AudioSource`, `AudioListener`, `ReverbZone`,
  `Rotator`, `Oscillator`, ...). Prefer editing scenes in the editor; when editing JSON keep guids unique and stable.
- `Content/Scripts/*.zs` - gameplay code in **ZeqouScript** (see `Docs/ZeqouScript.md`). This is where most work happens.
- `Content/**/*.zmat` / `.zmati` - material graphs / instances (JSON node graphs). `.zprefab` - prefabs.
- `Saved/` - generated; never commit or edit.

## Writing gameplay code
- Gameplay is ZeqouScript only (C++ modules arrive in a later engine version). One `script Name { }` per `.zs` file.
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
    write("AGENTS.md", AgentsTemplate);
    write("CLAUDE.md", "# Claude Code\n\nSee [AGENTS.md](AGENTS.md) - the instructions for AI assistants working on this project.\n");
    return written;
}

} // namespace zx
