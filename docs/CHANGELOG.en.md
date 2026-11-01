# ZeqouX3D — changelog (English)

The full history is in [CHANGELOG.md](CHANGELOG.md) (Russian); this file has the same entries in English, in the same order. Version scheme up to 1.0: **0.<last closed milestone>.<release within it>**.

## 0.17.10 — AI panel: model picker size and motion (2026-11-01)
- **Model picker**: the popup has a fixed size (`ui::SetPopupSize`) that is clamped to the window. Its content (wide
  rows, nested regions) cannot stretch it. The provider and model lists take their height and width from that size, and
  the model rows leave room for the scrollbar.
- **Motion**: the panel opens and closes on a spring instead of a linear step. A new message from you fades in and rises
  into place; assistant replies and tool rows only rise into place, without fading. The message box border eases to the
  accent colour on focus. The context ring's arc eases to its value, while the percentage and the tooltip show the exact
  number. The chat glides to the newest message instead of jumping.
- **Found by reading the code, and fixed**: after some parts of a chat item (for example, the copy button of a code block) the layout cursor could stay inside the block, so the next item might be placed over it. The cursor is now returned to the start of the current item before moving on. Not reproduced on screen.
- UI: `ui::SetPopupSize`, `ui::SetScrollY`; test `UI.FixedSizePopupFitsWindow`. Full ZXTests run: 474 passed, 0 failed. Development build: no errors or warnings.

## 0.17.9 — Zeqou AI: 22 providers, model picker in chat, context window, new panel (2026-11-01)
- **Providers**: 22 instead of three — Anthropic, OpenAI, Google Gemini, xAI Grok, DeepSeek, Mistral, OpenRouter, Groq,
  Cerebras, Together, Fireworks, Perplexity, Moonshot (Kimi), Z.ai (GLM), Alibaba Qwen, NVIDIA NIM, GitHub Models,
  Hugging Face, Ollama, LM Studio, llama.cpp / vLLM and Custom (any OpenAI-compatible server). Each has its own key,
  model and URL; a check mark in the list means the provider is set up. Settings from 0.17.8 (`AI.json`) are read.
- **Model picker in the chat**: the model name under the message field opens a list — providers on the left, models on
  the right, with search, the context size of each model, a refresh button, and any model id typed by hand. The list
  comes from the provider's API (`/models`: OpenAI, Anthropic, OpenRouter, Groq, Mistral, Gemini, Ollama, GitHub Models
  and others). Embedding, speech and image models are filtered out.
- **Context window**: detected automatically — from the provider's answer (`context_length`, `context_window`,
  `max_context_length`, `inputTokenLimit` and similar), otherwise from the model family (a table), otherwise 128k. In
  Settings > AI you can set it by hand for a model (slider and presets from 8k to 1M; the maximum is **1 million
  tokens**). When a conversation does not fit, the oldest turns are dropped and a note appears in the chat; the latest
  turn and each tool call with its result are kept together.
- **Context ring** to the right of the message box: how full the window is (accent, then yellow, then red), a percentage
  from 50%, a tooltip with where the value came from; a click opens Settings > AI.
- **A new panel** in the editor's own style: a flat card in the theme's colours instead of the glass card with a glow.
  Assistant replies are real Markdown (headings, lists, quotes, bold, italic, `code`, links, code blocks with Copy and
  wrapping). Tool calls are compact rows with status, duration and an expandable result; there is a "Thinking" line
  while the model works; errors and notices are shown inline; a "jump to latest" button appears when you scroll up. The
  panel's left edge can be dragged to change its width. The opening animation has no spring.
- **Message box**: several lines (Enter sends, Shift+Enter adds a line), grows up to 8 lines. Esc does not erase the
  draft; Esc stops the assistant while it works, when the message box has focus.
- UI: `InputTextFlags::SubmitOnEnter`, `InputTextMultiline` with a hint and focus, `OpenPopupAnchored`,
  `GetScrollY` / `GetScrollMaxY`, `GetDisplaySize`.
- Fixed: the node lists of the Material, Behavior Tree and ZGraph editors stay on screen (they scroll) and take typing
  as soon as they open.
- Tests: `Models.*` (parsing of model lists in OpenAI, Anthropic, OpenRouter, Gemini, Ollama, llama.cpp and other
  formats, the known-context table, list requests), `Agent.ContextTrimming`, `UI.MultilineSubmitOnEnter`,
  `UI.PopupSearchFieldAcceptsTyping`. Full `ZXTests` run: 473 tests passed. Live check: the editor against an
  OpenAI-compatible test server — the model list is requested when the panel opens, a tool call and a reply go through.
- Settings from 0.17.8 (`AI.json`) are read: keys, model, URL, effort, Max Tokens and MCP settings. `openai-compatible`
  becomes the "Custom" provider.

## 0.17.8 — Zeqou AI: assistant and MCP server (2026-11-01)
- **Zeqou AI**, an AI assistant in the editor. The button in the toolbar (to the left of LITE / FULL) or **Ctrl+Space**
  opens the panel. Chat bubbles, tool-call chips with their state, a typing indicator, prompt suggestions, a request and
  token counter, buttons for a new chat, connecting an agent, settings, and Esc to close.
  - **Your own API key**: Anthropic (Claude Opus 5.5 by default; effort low to max, a fallback model when a request is
    declined, conversation caching), OpenAI, or any OpenAI-compatible server (Ollama, LM Studio, OpenRouter and others).
    The key is encrypted for your Windows account (DPAPI) and stored in `%LOCALAPPDATA%/ZeqouX3D/AI.json`, never in a
    project.
  - The assistant builds things itself: it creates and changes objects, writes scripts, starts Play, looks at the
    viewport through a screenshot and reads the log. Every scene change is a separate Undo step, "AI: …".
- **Editor MCP server** (Model Context Protocol, Streamable HTTP, JSON responses). Any agent (Claude Code, Cursor,
  VS Code / Copilot, Claude Desktop through `mcp-remote`) can connect to the editor and make a game from a text request.
  It is turned on in Settings > AI or on the panel's connect page, which also has ready commands and configs with a Copy
  button. It listens on 127.0.0.1 only, requires a token (`Authorization: Bearer`) and rejects foreign Origins.
- 17 tools shared by the assistant and MCP: `get_editor_state`, `get_docs`, `list_entities`, `get_entity`,
  `create_entities` (cube, sphere, cylinder and plane shapes with a collider; position, rotation in degrees, scale,
  parent, components), `modify_entity`, `delete_entities`, `select_entities`, `run_console_command`, `get_log`,
  `list_files` / `read_file` / `write_file` (text files inside the project only), `screenshot`, `play_control`,
  `save_scene`, `undo`. Calls run on the main thread between frames.
- **Settings > AI**: provider, key, model, server, effort, Max Tokens, MCP port and token (New Token).
- Console: `ai.open [0|1] [chat|connect]`, `ai.send <text>`, `ai.mcp <0|1>`.
- Platform: an HTTP(S) client on WinHTTP (with cancellation), a local HTTP server on Winsock, DPAPI secrets, the system
  random generator.
- Tests: `Mcp.*` (protocol, authorization, Origin, batches, a real request over localhost), `Agent.*` (the tool loop of
  Claude and OpenAI on recorded responses, cut-off and declined responses, a retry without unsupported parameters, key
  errors), `Net.SecretsRoundTrip`. Manual check: an external MCP client went through all 17 tools in the live editor; the
  built-in agent completed a full cycle against an OpenAI-compatible server.

## 0.17.7 — FBX import (M18) (2026-10-31)
- **FBX import** with our own code, no FBX SDK: binary FBX 7.x (7.4 and 7.5+ with 64-bit records, compressed arrays)
  - meshes: polygons of any shape (fan-triangulated), normals, two UV sets, vertex colours, several materials per mesh
  - Phong / Lambert materials → PBR (colour, opacity, emission; roughness and metalness from Blender files); colour, normal and emission textures, either as files or embedded in the FBX; moved files are searched by name next to the model, in `Textures`, `../Textures` and `<name>.fbm`
  - node hierarchy with pivots, pre- and post-rotation and any rotation order; axes (Y-up, Z-up) and file units (centimetres, metres) are converted to the engine's metres
  - skeletons and skins (up to 4 bones per vertex), animations (each stack is a clip; Blender names `Armature|Run` become `Run`)
  - Import, double-click and drag-and-drop of `.fbx` work as they do for glTF; when copied into a project, textures from neighbouring folders are copied next to the model
- Not supported yet: blend shapes (morph targets), ASCII FBX (the importer shows a message asking you to re-save it as binary), cubic animation curves (linear between keys; Blender and Maya with baked animation are exact)
- Tests FbxImport.*: a scene from Blender (`Tools/gen_fbx_testdata.py`) as glTF and three FBX variants (skin poses match to within 2 mm in all clips); a real Quaternius model; damaged and truncated files do not crash the engine. `ManualFbx.CompareWithGltf` (ZX_FBX_FILE / ZX_GLTF_FILE) compares any FBX with the glTF of the same scene

## 0.17.6 — Sequencer and editor fixes (M18) (2026-10-31)
- **Sequencer**: cutscenes and scripted scene changes on a timeline (`.zseq`, JSON in Content)
  - tracks: **Transform** (Linear / Smooth / Step / Spline over positions), **Animation** (a clip with `speed`, `length` and `loop`; the pose is computed from time, so scrubbing works), **Event** (calls `fn onSequenceEvent(name)` on the target and its parents), **Camera** (switches to a camera, sets FOV; an empty key returns to the game camera), **Audio** (sound, volume, 3D at the target's position)
  - the track target is an entity name, a `Parent/Child` path, `@self` or `@self/Child`
  - **Sequence Player** component (Play On Start, Loop, Speed, Restore When Done); scripts `entity.sequencePlay(from)`, `sequenceStop()`, `sequencePause(bool)`, `sequenceTime()`, `sequencePlaying()`; a feature flag in Features: `sequencer`
  - **Sequencer window** (Window > Sequencer, double-click a `.zseq`, Content Browser > New Sequence, Create > Cinematic, the Edit Sequence button on the component, console `seq.edit` / `seq.status` / `seq.play` / `seq.reload`): ruler, tracks, keys, playhead; Space plays, K adds a key, Delete, Ctrl+Z / Ctrl+Y / Ctrl+S; snapping to frames (30/s); dragging keys; zoom with the wheel; a key settings panel; "look through the frame's camera" in the viewport; a problems list (no target in the scene, an empty clip or sound, two keys at the same time)
  - the preview changes the scene only while the window is open: before Play, saving and clearing the scene, everything is restored
  - tests Sequencer.* (format, interpolation, Apply / Restore with cameras, the player, registration)
- **Scripts on objects** are clearer: `.zs` and `.zgraph` files can be dragged from the Content Browser onto an object in the viewport, in the Outliner or in Details, and there is "Add to Selected Entity" in the Content Browser file menu; one check and one undo step for all of these paths (`AssignScript`, test ScriptAssignment.*)
- **Multi-select**: Shift/Ctrl+click in the viewport; in the Outliner, Ctrl adds and Shift selects a range; the gizmo moves the whole group (children of selected objects are not moved twice); the outline shows on all selected objects; Delete and Ctrl+D work on the group; one undo step
- **Ctrl+C / Ctrl+V**: copying objects with their children (the clipboard holds JSON, so it can be pasted between editor windows), new GUIDs, paste next to the original; Edit > Copy / Paste
- **Content Browser: rename** files and folders (Rename menu, F2): `.zmeta` and `.znav` move together with the file; invalid characters and taken names are checked
- **Scene graphics** (File > Settings > Rendering): Fog (height and volumetric), Auto Exposure / Manual Exposure + EV100; they are saved in the scene and apply in the packaged game
- Fixed the build: a repeated `= false` in the definition of `Editor::Select`

## 0.17.5 — Behavior Trees editor (M18) (2026-10-24)
- **Behavior Tree window** (double-click a `.zbt`, Content Browser > New Behavior Tree, the Edit Tree button on the component, console `bt.edit`): the tree is drawn top-down on a canvas; children run left to right (order = horizontal position, shown as a number on each node)
  - adding: right-click, or drag a link from a node's bottom pin into empty space, which opens a menu with search by category (composites, decorators, tasks) and hints; Alt+click on a pin disconnects it; dragging nodes, a selection rectangle, Delete, Make Root, Ctrl+Z / Ctrl+Y, Ctrl+S
  - **Arrange** lays the tree out neatly; Frame shows everything
  - on the right: node settings (blackboard key, comparison, value with its type, seconds, radius, counter, text); on the left: blackboard default values; a problems list (a decorator without a child, a composite without children, a task without a key, unconnected nodes)
  - **debugging in Play**: choose an AI; nodes are highlighted: running (amber), last success (green), failure (red); blackboard values are live; after saving, running AIs pick up the new tree at once
- The starter Guard tree is now laid out neatly (`LayoutBehaviorTree`)
- Test BehaviorTree.LayoutAndProblems

## 0.17.4 — Behavior Trees: AI decisions (M18) (2026-10-24)
- **Behavior Tree component**: a decision tree from a `.zbt` file (JSON in Content, reloaded by `bt.reload`)
  - composites: **Selector** (reactive: a branch with higher priority that can run interrupts the current one, "chase as soon as the player is seen"), **Sequence** (the conditions before a running step are checked every tick: if the target is lost, the chase is interrupted), **Parallel**
  - decorators: Inverter, Succeeder, Repeat, Cooldown
  - tasks: Wait, MoveTo (a point or an entity from the blackboard, through a Nav Mesh Agent, re-planning the path behind a moving target), Stop, Condition, SetValue / ClearValue, RandomPoint (a random reachable point), NextWaypoint (the children of a group entity, in order), FaceTarget, Event (`fn onAIEvent(name)` in scripts), Log
- **Blackboard** on every AI (bool, number, vec3, string, entity), with default values in the file; scripts: `entity.bbGet(key)` / `entity.bbSet(key, value)`
- **Sight**: range, field of view, eye height, line of sight through physics; writes `target`, `targetVisible` and `targetLastSeen`
- Starter Content: **AI/Guard.zbt**. A guard patrols the points named "Waypoints", chases Player while it can see them, searches the place where it lost them, and returns to patrol. Create > AI Agent creates an agent with this tree and four patrol points; Nav Mesh Bounds and AI Agent are placed on the ground under the creation point
- Console: `bt.status` (running nodes and each AI's blackboard), `bt.reload`
- Fixed: the Nav Mesh Bounds and Nav Mesh Agent components from 0.17.3 were not registered (they were missing from Add Component and were not saved to the scene)
- Tests BehaviorTree.* (format and tree check, priorities and interruptions, Repeat / Cooldown, the guard patrols, chases and searches)

## 0.17.3 — AI navigation: NavMesh, pathfinding, agents (M18) (2026-10-24)
- **New Nav module**: a navigation mesh like Recast. The scene geometry is voxelized (cells 0.3 × 0.2 m). Walkable are the surfaces flatter than the set slope that have room above them for the agent, with no drops higher than a step. The edge pulls back from walls by the agent's radius, and small islands are discarded. Cells merge into convex polygons with links (bridges and floors stacked on top of each other are not merged)
- **Pathfinding**: A* over polygons, the "funnel" algorithm (the shortest path over the mesh), and cutting extra corners with a ray over the mesh. An unreachable target gets a path to the nearest point (status partial). `Raycast` over the mesh; the nearest point on the mesh
- **Components**: **Nav Mesh Bounds** (the box where the mesh is built: agent radius and height, step, slope, cell size) and **Nav Mesh Agent** (speed, acceleration, slowing down near the target, turning in the direction of movement). With a Character Controller, the agent drives the character through physics; without one, it moves the object and places it on the mesh
- The mesh is built from Mesh Renderers with collision and from terrains (using their height maps); dynamic bodies, characters and agents do not affect it. It is saved next to the scene (`<scene>.znav`) and the game loads it; without the file it is built when Play starts
- **Editor**: Build > Build Navigation, Show > Navigation (polygons and agent paths), Create > Nav Mesh Bounds / AI Agent; console `nav.build`, `nav.show`, `nav.stats`, `nav.goto x y z`, `create navbounds|navagent`
- **Scripts**: `entity.navTo(pos)`, `navStop()`, `navStatus()`, `navRemaining()`
- Tests NavMesh.* and Navigation.* (erosion, going around a wall by the shortest path, steps / drops / steep slopes, floors, the `.znav` file, an agent reaching its target)

## 0.17.2 — vegetation: forests, bushes, rocks, grass (M18) (2026-10-24)
- **Foliage component**: many copies of one model (a tree, a bush, a rock, grass) on one entity. The model's parts (mesh, materials) come from its prefab; the instances (position, rotation, scale) are stored packed. Each instance is drawn through the GPU-driven path (culling, LOD), and beyond its own distance (`Cull Distance`) it is not drawn
- **Scattering by rules** (`ScatterFoliage`): density, maximum slope, a height band, "patches" (forests and clearings from noise), scale variation; deterministic from the seed and the place
- `world.create` now plants the world: deciduous trees in the lowlands, spruces higher up, bushes, rocks, and grass near the camera (the CC0 Quaternius Stylized Nature pack is copied into the project). That is about 280 thousand plants and rocks on 4×4 km, with one Foliage entity per model in each cell (streamed together with the cell)
- Test of a flight over the 4×4 km forest world: the world update averages 1.8 ms, peak 31 ms, frames over 50 ms: 0

## 0.17.1 — open worlds, part 1: World Partition, HLOD, a 4×4 km world (M18) (2026-10-24)
- **World Partition**: the scene is split into cells (256 m by default). Root objects with the **Spatially Loaded** component are stored not in `.zscene` but in cell files `<scene>_Cells/x_z.zcell`, and are loaded around the camera (in the editor) or the game camera (in the game) and unloaded when far away
  - the files are read and parsed by background tasks; objects are created one tree at a time within the frame budget; new terrain tiles are built two per frame, so there are no long frames
  - saving distributes objects into cells; a cell that a new object falls into is loaded first, so nothing is lost; empty cells are deleted
  - in the editor, cells are unloaded only for a saved scene (unsaved edits stay in memory)
- **HLOD**: for every 4×4 cells, one simplified mesh of their terrain (about 16 m between points, with skirts, slightly below the real ground). Groups not loaded within 8 km show it, so the horizon is never empty, and the whole world is 16 meshes instead of hundreds
- **Terrain for large worlds**: `noiseOffset`. Tiles with the same settings continue each other's hills without seams (normals at the edges are computed from the generator too). Terrain chunks got an LOD chain (every 2nd, 4th … point) with skirts against gaps
- **`world.create [km] [tile m] [seed]`**: an open-world generator that writes a grid of terrain tiles straight into the cell files, plus HLOD, sky, sun, clouds, time of day, and a snow material instance only on the top vertices; `world.hlod`, `world.stats`
- **`world.flytest [s] [m/s]`**: a streaming test. The camera flies over the world and the world update time on the main thread is measured. A 4×4 km world (256 tiles), 30 s at 60 m/s: on average 1.2 ms, peak 38 ms, frames over 50 ms: 0
- The packaged game (ZXGame) streams cells around the game camera

## 0.17.0 — milestone M17 "Global illumination" closed (2026-10-24)
- **Lumen-lite (GI-2 prototype)**: indirect light is computed for each pixel (at half resolution). The ray first goes through the screen (exact detail at contacts, a lit frame); if it leaves the screen or finds nothing, it goes through the scene's distance field, where a hit is lit by the sun, lamps and the probe cache. Close hits in the field (coarser than two voxels) are handed to the probes. More contact detail: fabric folds, protrusions, shadows under ledges
  - for powerful GPUs (UHD 620: about 17 ms, Full profile): GI Quality Ultra or `r.GI.Lumen`; off by default
- **M17 acceptance**: Sponza with a moving sun (morning / day / evening) and indirect light in the Full profile: GI 8–10 ms on Intel UHD 620 (about 1 ms on a GTX 1060). Lighting bake on Low (Lite): Sponza's probes in about 15 s, lightmaps for the Third Person level
- Hardware ray tracing (DXR) is postponed: the development machine does not have it (RT tier 0). All tracing code goes through one function, ShadeRay; the RayQuery path will be added there once there is hardware to test it on
- README: a section on global illumination, with Sponza screenshots with and without GI

## 0.16.8 — lightmaps (M17, GI-0) (2026-10-24)
- **Bake Lighting now bakes lightmaps too**: indirect diffuse light per texel for static meshes with a lightmap UV (levels, props, built-in shapes). Everything else (heavy virtual geometry, moving objects) takes its light from the baked probes
  - objects are packed into an atlas (4 texels per metre, squares scaled to the object, width 1024, height growing up to 8192). Surfaces are rasterized into the atlas conservatively (the position and normal of each texel)
  - from each texel: 8 rays × 32 iterations through the distance field, with the same lighting of hits as the probes. Rays that start inside geometry (wall and floor joins) are not counted. Map seams are filled from their neighbours
  - the atlas is saved in `.zgi` (format 2); each object's place is stored in its Mesh Renderer (saved with the scene; the bake is undone through Undo)
- In Baked mode (the Lite profile), objects with a lightmap take indirect light from their own texels: soft shadows at joins, coloured bounces, no light leaks between probes. Direct sunlight stays real-time
- Built-in shapes (Cube, Sphere, Plane, Cylinder) get real lightmap UVs (before, UV1 was UV0 and the cube's faces overlapped)
- The GPU instance row grows from 192 to 208 bytes (lightmap data)

## 0.16.7 — screen-space GI (SSGI) (2026-10-24)
- **Screen Space GI**: small reflected light that the probes (1–1.6 m steps) cannot see: the floor next to coloured objects, light from moving and glowing objects, bounces in corners
  - half resolution, 4 rays per pixel (cosine distribution) travel through the depth buffer up to 2.5 m. A hit takes the already lit colour of the frame (direct light + probes = further bounces). Misses are left to the probes
  - accumulation over frames with reprojection and neighbour clamping (no trails); applied from the next frame
  - Full profile (it needs the G-buffer normals); File → Settings → Rendering → Screen Space GI, `r.GI.ScreenSpace`; off in the debug view modes
- On UHD 620 in Full: about 4.4 ms (designed for discrete graphics cards)

## 0.16.6 — GI: empty probes are not traced (2026-10-24)
- **Probe classification**: a probe with no surface at all within its grid step gives no light to anyone. Its rays are only fired once every 8 of its queues, so that newly appeared geometry is still noticed
- Dynamic GI on Intel UHD 620 (Lite profile): an open Third Person scene 7.1 → 2.7 ms, Sponza 3.8 → 1.7 ms per frame; the picture is the same

## 0.16.5 — lighting bake (M17, GI-0: Baked) (2026-10-24)
- **File → Bake Lighting** (or `gi.bake [step m] [iterations]`): the light of the static scene is baked into probes
  - the probe grid is fitted to all the static geometry (step 1 m, growing if there are more than 24 000 probes); the distance field is 4 levels around it
  - each probe: 256 rays × 48 iterations with exact averaging, multiple reflections. Sponza (11 616 probes) bakes in about 15 s on Intel UHD 620; progress is shown in the Output Log
  - the result is saved next to the scene (`<scene>.zgi`; for an unsaved scene, `Saved/BakedLighting`), loaded when the scene opens and in the packaged game
- **GI mode** (File → Settings → Rendering → GI Mode, `r.GI.Mode`): Dynamic: probes are traced every frame (a moving sun and lamps). Baked: baked probes, **0 ms on the GPU**; dynamic objects are lit by the same probes (probes for dynamic objects). The Lite profile defaults to Baked when the scene has a bake
- `gi.clearbake` removes the scene's bake; `stat.scene` shows which GI is running

## 0.16.4 — lightmap UVs (M17, preparation for the light bake) (2026-10-24)
- **Lightmap UV generation on import** (like "Generate Lightmap UVs" in UE and Unity) for static meshes without their own second UV channel:
  - triangles grow into charts while their normals stay within a 50° cone of the first (largest) face. Walls, floors and the faces of round shapes become separate charts
  - each chart is projected onto its own plane and rotated to its minimum rectangle
  - charts are packed in shelves into a square with 3-texel gaps (a reference resolution of 32…1024 by the mesh's area); the texel density is the same over the whole mesh; vertices on chart edges are duplicated
  - animated meshes and heavy virtual geometry with organic shapes do not get them (seams would get in the way of cluster simplification). They will take light from probes
- The second UV channel now reaches the shaders: the TexCoord 1 node in materials works for real
- `r.ViewMode 10` shows the lightmap UV density (a checkerboard; red for meshes without lightmap UVs)
- The import cache is rebuilt once (cooker version 9)

## 0.16.3 — GI: probes move themselves out of walls (2026-10-24)
- **Probe relocation** (as in RTXGI): a probe that ends up inside a wall, a column or behind thin fabric finds the nearest back side of a surface with its own rays and moves out through it. A probe too close to a surface steps back. The shift is at most 45 % of the grid step and is reset when the grid scrolls
- Removes dark spots and light leaks at thin geometry (the Sponza banners), and brings back probes that used to simply switch off

## 0.16.2 — dynamic global illumination (M17, part 1: DDGI on a global SDF) (2026-10-24)
- **Light bounces off surfaces in real time**: a floor lit by the sun lights ceilings and arches with warm light, fabric and walls "paint" the light around them, and galleries and corners are dark rather than flat-filled by the sky. It also works with a moving sun or Time of Day: the light rebuilds within a couple of seconds
- How it works:
  - **a global distance field** of the scene: static geometry is voxelized on the GPU into 3 nested levels of 128³ around the camera (0.25 / 0.5 / 1 m), with surface colours (a texture at a suitable mip) and normals. The distances are computed with jump flooding. A level is rebuilt only when the camera moves away from its centre or the static scene changes (no more often than once every 20 frames). Moving and animated objects are not put into the field
  - **a probe grid (DDGI)** around the camera: each probe fires rays through the distance field. A hit is lit by the sun (shadow from the cascades, and beyond them a soft shadow from the field), point and spot lights, and the probes of the previous frame themselves (multiple bounces). A miss is lit by the sky. The probes store irradiance (an 8×8 octahedron) and distances (16×16) and blend between frames
  - **surface lighting**: the flat "sky light" is replaced by the light of the 8 neighbouring probes with a visibility test (a Chebyshev test), so light does not leak through walls. Probes inside walls switch themselves off
- Cost on Intel UHD 620: the Lite profile takes about 3.5 ms (4000 probes, updated 1/8 per frame). The Full profile has 9408 probes, half per frame (designed for discrete graphics cards)
- Settings: **File → Settings → Rendering → Global Illumination** and **GI Quality** (Low…Ultra); the Features window → Global Illumination (for the project); console `r.GI`, `r.GI.Intensity`, `r.GI.Rays`, `r.GI.Stride`, `r.GI.Spacing`; `r.ViewMode 9` shows only indirect light; `stat.scene` shows the probes and the field rebuilds
- The 2011 and 2008 graphics eras have no dynamic GI (2017 has it)

## 0.16.1 — shadow and time-of-day settings in the interface, soft virtual shadows (2026-10-24)
- **Directional Light → Shadows section** (as in UE):
  - **Source Angle**: the apparent size of the sun. Larger = softer shadows far from the contact (the real sun is 0.53°)
  - **Shadow Distance**: how many metres around the camera the sun casts shadows (0 = the engine profile's value)
  - **Shadow Bias**: an offset from the surface. Higher removes stripes ("acne"); lower if the shadow detaches from the object
  - **Contact Shadows**: a checkbox, plus the length of a short screen-space shadow where objects touch the ground
- **File → Settings → Rendering**:
  - **Shadow Method**: Default / Cascades (cascades, cheap) / Virtual (virtual shadow maps: sharp and close, and far)
  - **Shadow Quality** now also controls virtual shadows: texel density, the distance (120–400 m), and how many pages are redrawn per frame
  - **Contact Shadows**: on/off
  - settings are applied only when they change, so console `r.*` values are no longer overwritten every frame
- **Soft virtual shadows** (Shadow Quality Ultra or the Full profile): a shadow is sharp at the contact and blurs with the distance to the occluder. The wide penumbra is taken from a coarser level, so the cost does not grow; samples cross page boundaries without seams
- Page marking is cheaper (on UHD 620: 0.33 → 0.23 ms; with soft shadows 1.45 → 0.35 ms): an atomic write only if the page is not yet marked, and coarse levels are marked less often
- **Time of Day**:
  - the **Dawn, Sunrise, Morning, Noon, Golden, Sunset, Dusk, Midnight** buttons set the time from the real sunrise and sunset of that day and latitude
  - under the buttons: sunrise, sunset, the length of the day (polar day / polar night), the heights of the sun and the moon, the moon phase
  - console: `time golden`, `time sunset`, `time dusk`...
- Console: `r.Shadows.Virtual.Distance`; `select` accepts `_` in place of a space (`select Time_of_Day`)

## 0.16.0 — milestone M16 closed: virtual shadow maps and day–night cycle (2026-10-18)
- **Virtual shadow maps (VSM)** for the sun: sharp shadows both at the feet and far away
  - levels of 16k × 16k virtual texels around the camera. Each next level has texels twice as large. A pixel takes the level that matches its size on screen
  - only the 128 × 128 pages the screen needs are drawn (the GPU marks them itself). Pages are cached while nothing moves in them; moving objects redraw only their own pages
  - rocks and other ZNano meshes are drawn into the shadows as clusters, so the shadows are detailed too
  - when the sun turns, the shadows for the new position are gathered in the background and replace the old ones as a whole. The shadows shift in steps but never disappear
  - on by default in the Full profile; in Lite, `r.Shadows.Virtual 1` (cascades remain there)
  - the Cascades view mode (`r.ViewMode 5`) colours the levels; `stat.scene` shows the pages
- **Time of Day — day and night** (a component from Add Component, or the command `time <hour> [day length in minutes]`):
  - the sun moves as it does in reality: its height and direction follow the hour, the latitude and the day of the year (at dawn it is in the east, at noon in the south in the northern hemisphere). The direction of north can be set
  - during play the clock runs (the day length in real minutes), and optionally in the editor too
  - sunset and dusk, then night: the sky fades, stars and a moon with phases appear, and moonlight becomes the main light (with shadows). Auto-exposure does not turn the night into day

## 0.15.0 — milestone M15 "ZNano virtual geometry" closed: cluster streaming (2026-10-18)
- **Streaming of cluster pages**: ZNano geometry no longer has to lie entirely in video memory
  - a mesh's clusters are laid out into pages of about 128 KB (vertices and triangles together), from coarse levels to detailed ones. The pages are written to a temporary file; a pool of pages lives on the GPU (160 MB)
  - each frame shows which pages the camera and the shadows need. Missing ones are loaded (up to 24 pages per frame), long-unused ones are evicted, and if the pool is full, it is divided equally between the meshes
  - while the detailed pages have not arrived, the nearest coarse level is drawn, so there are no holes or cracks
  - check: `stress.nano 770 8 8` — 8 different rocks of 1.3 million triangles each, 825 MB of pages with a 160 MB pool, about 34 FPS on UHD 620. `stat.scene` shows how full the pool is
- ZNano meshes no longer duplicate their full detail in ordinary buffers: it lives only in the pages. For the fallback paths (without GPU-driven rendering, lamp shadows, translucent parts) a chain of LODs from LOD1 remains
- Error values of groups in the cluster hierarchy are now unique, so the page boundary always runs between groups (the import cache is rebuilt once)

## 0.14.1 — ZNano: virtual geometry (like Nanite), 1 billion triangles on integrated graphics (2026-10-18)
- **ZNano virtual geometry** (milestone M15, part one): heavy static meshes are drawn as clusters of 128 triangles, each at exactly the detail the screen needs
  - on import, a mesh is cut into clusters, and a hierarchy of simplifications (a DAG, as in Nanite) is built from them. Groups of 4 neighbouring clusters are simplified by half with their edges frozen, so neighbouring areas of different detail join without cracks
  - it switches on by itself for static meshes of 50 thousand triangles and more (in .zmeta: `"virtualGeometry": true / false`). Sponza is now drawn this way too
  - on the GPU, for each view (the camera, the cascade shadows) the clusters with an error of at most 1 pixel are selected (2 on Lite), those outside the screen and behind other objects are culled (two-phase HiZ), and the visible ones are collected into one index buffer and drawn with one indirect call per material
  - **triangle budget** (3 million; 1.5 million on Lite): if the scene does not fit into it, the detail drops smoothly instead of lowering the FPS
  - for the other paths (CPU rendering, lamp shadows, translucent parts), an ordinary LOD chain is cut from the hierarchy
- **Check**: the command `stress.nano 770` — 770 rocks of 1.3 million triangles = **1 billion source triangles**. On Intel UHD 620, about 40 FPS (about 1 million triangles are drawn)
- The **LOD** view mode (`r.ViewMode 4`) colours the clusters. Settings `r.Nano`, `r.Nano.PixelError`, `r.Nano.TriangleBudget`; `stat.scene` shows the cluster statistics
- Fixed: repeatedly appending to the engine's arrays (`Array::Append`) copied the whole array every time. Importing large meshes and some tests became many times faster (tests: 39 → 9 s)

## 0.14.0 — milestone M14 "Game UI + ZGraph" closed: a graph-driven mini-game in the Third Person template (2026-10-18)
- **Acceptance of M14**: the Third Person template now has a small game whose entire logic is built from ZGraph nodes (`Content/Scripts/Graphs`; double-click to open and look)
  - **Coin.zgraph**: five coins spin around the arena (on the jumping platform, on the balcony, by the columns…). Walk up to one and it disappears with a sound, and the counter goes +1
  - **Gate.zgraph**: a treasury in the south-east corner. E at the gate: if there are enough coins, the gate rises smoothly with a creak; otherwise the hint says "Coins needed: 5". The coin count is the Exposed variable (Details)
  - **GameHUD.zgraph**: the coin counter at the top, hints at the bottom, a pause menu on Tab (Continue, switching the language English / Русский (Russian); it works with the mouse, the arrow keys and a gamepad; Escape / B continue)
  - the look is the `Game.zstyle` style; the texts are localization tables en / ru
- New for graphs and scripts:
  - **shared game values**: `game.get / set / add` and the nodes Get Game Value, Set Game Value, Add To Game Value: a score, keys, collected items, visible to all scripts and graphs
  - **smooth movement**: `entity.moveTo(position, seconds)` and the Move To node (doors, lifts, platforms)
  - the nodes Focus Widget, On UI Back, Greater Or Equal
  - an action on an object that no longer exists (for example, a text that was deleted) is now skipped instead of stopping the whole graph

## 0.13.23 — ZGraph, part 3: graph debugging (2026-10-18)
- **Execution highlighting**: during Play, the nodes that have just run, and the wires along which execution passed, glow yellow and fade smoothly. You can see which branches run each frame
- **Breakpoints**: F9 (or the node menu → Toggle Breakpoint) puts a red dot on a node. When the node runs, the game pauses after that frame, the node is outlined in red, and "Breakpoint: … — F6 continue, F10 next frame" appears at the top. While paused, the highlighting freezes, so the whole path of the last frame stays visible
- **Variable values**: each graph variable shows its current value during play (`now: 3`) for the selected entity
- **Whose graph we look at**: if a graph sits on several entities, the panel lets you pick the one you need
- How it works: in the editor, the graph is compiled with `__trace(node)` calls before each step (they are absent in the packaged game). ScriptSystem reports the node and the entity to the editor
- Console: `graph.break <node id>`

## 0.13.22 — ZGraph, part 2: the node editor (2026-10-18)
- **Script Graph window**: double-click a `.zgraph` in the Content Browser (or right-click → Edit). New Script Graph creates a new graph with On Start and On Update
  - the canvas: nodes coloured by type (events red, actions blue, values green, flow grey), white triangular execution pins, coloured data pins (number, bool, text, vector, entity), and the values of unconnected inputs shown on the node
  - the mouse: right-click to search and add a node (Enter adds the first match); drag a wire between pins; drop a wire into empty space to get a list of only the suitable nodes, with auto-connection; Alt+click on a pin disconnects it; a selection rectangle; dragging nodes; the wheel zooms; the right button pans; F shows everything
  - Ctrl+C / V / D, Delete, Ctrl+A, Ctrl+Z / Y (the graph's own history), Ctrl+S; Break Links in the node menu
  - on the left, **variables**: the name (renaming updates the nodes), the type, the default value, Exposed
  - on the right, **node properties**: settings (variable, key, event name, widget, seconds) and input values; errors and warnings; **Show Code** shows the ZeqouScript the graph compiles to
  - nodes with errors are outlined in red, and the compile status is shown in the panel. Changes are saved when the window closes
- Unknown variables and wrong types in Get / Set Variable give clear compile errors
- Console: `graph.edit <file>`

## 0.13.21 — ZGraph, part 1: visual scripts (format, nodes, compilation to the VM) (2026-10-18)
- **`.zgraph` files**: a graph of nodes and links (like Blueprints in Unreal): variables, nodes with their values and settings, links. It is attached to an entity with the same **Script** component as `.zs`
- **Compilation**: the graph becomes ZeqouScript code and is built into bytecode for the same VM. So graphs have everything scripts have: variables with the Exposed checkbox are edited separately for each entity in Details, the graph reloads on the fly during play, and the whole engine API is available. Errors name the node: `Node 'Set Position' (#3): 'position' expects vec3 but gets string`
- **Node library** (about 80):
  - events: On Start, On Update, On Key Pressed / Released, Every N Seconds, On UI Click / Changed, On Trigger Enter / Exit, On Collision, Custom Event (+ Call Event)
  - flow: Branch, Sequence, For Loop, Do Once, Flip Flop, Delay
  - actions: Print, Set Variable, Set Position / World Position / Rotation / Scale, Move, Rotate, Look At, Destroy, Set Text / Visible / UI Value, UI Animate, Show Mouse Cursor, Set Time Scale, Set Language, Play Sound (At), Add Impulse, Set Velocity, Emit Particles, Ignite, Animation Trigger / Parameter, Character Move / Jump
  - values: variables, Self, Find Entity, Child / Parent, positions and directions, input (Key Down, Move Input, Input Action), time, maths, comparisons, logic, vectors, text, Translate
- In the Starter Content there is an example, `Scripts/SpinnerGraph.zgraph`. `.zgraph` files are visible in the Content Browser and fit the Script slot
- Console: `graph.code <file>` shows the code a graph produces, `graph.new <file>` creates an example, `script.attach <file>` attaches a script or graph to the selected entity
- The node editor (a canvas with links drawn by mouse) is in the next version

## 0.13.20 — UI Designer: HUD and menus with the mouse, right in the viewport (2026-10-18)
- **UI Designer tool** (U, the button on the toolbar, Tools → UI Designer): game UI is built with the mouse
  - a click selects a widget (as in Figma: first the top level, a double-click goes inside, Ctrl+click picks the deepest). Hovering highlights a widget; the selected one shows its name and size in UI units
  - dragging moves anchored widgets (HUD corners, popups) and changes the order of widgets in the layout (a yellow line shows where it will land). 8 handles resize; the opposite edge stays in place
  - a snap grid (hold Alt to turn it off); the arrow keys move by one grid step (Shift: ×10); each action is one Undo step
  - **Preview**: a frame of another screen shape — 16:9, 21:9, 4:3, 16:10, a 9:16 phone — shows how the UI stretches
  - the panel: add Canvas / Panel / Text / Button / Image / Slider / Toggle / Progress / Input; a Parent button; "Anchored" (a free position that keeps its place on screen); a 3×3 anchor grid; Earlier / Later; Fit Content; the canvas style file and its errors
- Console: `tool ui`

## 0.13.19 — localization: game texts in different languages (2026-10-18)
- **`.zloc` tables** in `Content/Localization`: one file per language (`en.zloc`, `ru.zloc`, `de.zloc`…), lines `key = text`, `\n` for a line break, `{0}` for arguments, `@language: Русский` for the language's name. The project's tables override the Starter Content tables. If there is no translation, English is used, and then the widget's own text
- **UI Text > Key**: the widget shows the translation of the key in the current language (for UI Input Field, the Placeholder Key)
- **Game language**: on the first launch, the system language (if a table exists). The player's choice is remembered (`Saved/Config/Language.txt` in the packaged game). Switching is instant, even during play
- **Scripts**: `tr("hud.score", score)`, `lang.current`, `lang.set("ru")`, `lang.list()`, `lang.name("ru")`, `entity.textKey`. Setting `entity.text = ...` from a script removes the key (the text is no longer translated)
- **Window > Localization**: a table of keys × languages, with editing, search, adding keys and languages, a button to "set the key on the selected UI Text", and a preview of the language in the viewport. Save writes `Content/Localization/*.zloc`; the files are reloaded on the fly
- The Vehicle template is translated into Russian: the HUD and the pause menu use keys, and in the menu the "Language" button switches between English and Русский
- Console: `lang` (the list), `lang <code>` (a preview)

## 0.13.18 — game UI: .zstyle styles, animations, gamepad control (2026-10-18)
- **`.zstyle` styles** (UI Canvas > Style): CSS-like rules set the look of all the widgets on a canvas in one file
  - selectors: by type (`Button`, `Text`, `Panel`, `Slider`…), by class (UI Element > Style Class: `.title`), by name (`#Play`), by nesting (`Panel.menu Button`), by state (`:hover`, `:pressed`, `:focused`, `:disabled`)
  - properties are the names from Details (`font-size`, `corner-radius`, `hover-color`, `padding: 10 20`…); variables `@accent: #3B8CF5;` used as `$accent`; colours `#RRGGBB(AA)` / `rgba()`
  - the file is reloaded on the fly: edit a style during play and the UI changes at once; errors go to the Output Log
  - the Starter Content has a ready `StarterContent/UI/Game.zstyle`; the HUD and menus of the Vehicle template are styled with it
- **Look for each element**: Opacity, Scale, Translate (drawn around the centre together with the children; the layout does not move), and Transition for smooth changes (a button grows smoothly on hover)
- **UI Animation component**: Fade In / Out, Slide In (from the left, right, top or bottom), Pop In, Pulse, Float, Blink, Shake. A delay, a duration, curves (EaseOut, Back, Elastic, Bounce…). Plays when the element appears
- **Tweens from scripts**: `entity.uiAnimate("opacity", 0, 0.3, "easeOut")` for any numeric property or colour; `entity.uiPlay()`, `entity.addClass / removeClass / hasClass`
- **Menu control with a gamepad and the keyboard** (when the cursor is shown, `ui.cursor`): the arrow keys / D-pad / stick move the focus to the nearest widget in that direction. Enter / Space / A press. Left and right move a slider. Escape / B calls `onUIBack`. The focus is shown with a frame and the `:focused` style. `entity.focus()`, `ui.focused`
- Scripts: `input.pressed("GamepadStart")` and other gamepad buttons. The Vehicle menu also opens with the Start button
- Console: `input.press <key>` presses a key during Play (for checks)

## 0.13.17 — new licence: you can make and sell games on the engine (2026-10-18)
- **ZeqouX3D License** = PolyForm Strict 1.0.0 + a **Game Exception**: games made on the engine can be given away and sold for free, without royalties, together with the runtime and content that Package Project puts into the game. The engine itself, the editor and the source code may not be distributed, modified or resold
- The installer shows this briefly on the licence page. The README on GitHub was rewritten (features, downloads, new screenshots, the licence)

## 0.13.16 — the first public release (Beta): installer; Terrain brush v2, flat terrain (2026-10-18)
- **The installer ZeqouX3D-0.13.16-Beta-Setup.exe** (our own, without third-party tools; the program is ZXSetup, on our UI): a welcome page with a warning that this is a beta and not a complete 3D engine, the licence, a choice of folder and components (the asset packs can be left out), desktop and Start menu shortcuts, progress, and the editor starts at the end
  - it installs for the current user into %LOCALAPPDATA%\Programs\ZeqouX3D; no administrator rights are needed. Running it again updates the installation and keeps the settings
  - uninstalling through "Apps & features" (Uninstall.exe) leaves projects untouched
  - also **Portable.zip**: the same files, which can be unpacked anywhere
  - `Tools/make_release.ps1`: a Shipping build, the install folder, the installer and a zip in Build/Release
- Help → Documentation (F1) in the installed engine opens the README on GitHub
- **Create → Flat Terrain** (and the Flat button in Tool Settings; console `create terrain flat`): flat ground without hills, a blank sheet to sculpt your own map from scratch. The ordinary Terrain with hills remains
- **New brush modes**:
  - Set Height: to a given height (Alt+click on the ground takes the height from under the cursor)
  - Ramp: drag the mouse from the start to the end, giving a straight slope or a road as wide as the brush (with a preview of the line)
  - Terrace: steps of a set height and sharpness (terraces, plateaus, cliffs)
  - Erode: thermal erosion. Steep slopes slide, giving natural scree and valleys
  - Noise: now a real smooth noise with a "lump" size
  - Paint Grass: a new grass layer (it also paints on slopes and peaks). Ctrl with any layer erases it
- **Brush settings**: the shape Circle / Square, Target Height, Lump Size, Step Height, Sharpness, the erosion angle. The softness of the edge is an honest cosine falloff from the inner circle
- **Whole Terrain** (each operation is one Undo step): Flatten All, Smooth All, Reset Sculpt, Clear Paint, Paint by Slope (rock on steep slopes, earth on medium ones, grass on flat ground: a quick natural look), Import Heightmap (PNG 8/16-bit or RAW 16-bit .r16 from World Machine / Gaea / Unity, with the height of white), Export Heightmap (.r16)
- Painting now has 4 layers (RGBA: rock, earth, sand, grass). Scenes from 0.13.14–0.13.15 load as before
- The Tool Settings panel comes to the front by itself when the tool is turned on
- Console: `terrain.sculpt` knows the new modes; `terrain.ramp x0 z0 x1 z1 [radius]`; `terrain.op flatten|smooth|reset|clearpaint|slope|import <file> [height]|export <file>`

## 0.13.15 — Terrain tool: fixes (2026-10-18)
- The terrain no longer rebuilds as a whole every frame while you sculpt (it used to take 12–15 ms, make the mesh flicker and recapture the reflections). Only the affected vertex rows change, directly in the GPU buffer (about 0.05 ms)
- The Tool Settings panel opens as a tab next to Details, not as a window over the viewport
- With the Terrain tool on, the move gizmo and the orange selection outline no longer get in the way
- The editor grid is not drawn when the scene has terrain (it stuck out as a dark square through the ground)
- Shadows update along with the brush. The brush is stronger, and its edge is softer (a smooth bell instead of a cliff)

## 0.13.14 — Terrain tool, a settings window, one-sided scaling (2026-10-18)
- **Terrain tool** (T, the mountain button on the panel, Tools → Terrain Tool): you paint the terrain right in the viewport, holding the left mouse button:
  - sculpting: Raise / Lower (Ctrl swaps them), Smooth (or Shift), Flatten (to the height where the stroke started: plateaus, roads), Noise (uneven natural ground)
  - painting by layers: Rock, Dirt, Sand, and Erase (returns the automatic grass / rock / snow of the M_Terrain material)
  - the radius ([ and ]), the strength, the edge softness. The brush circle lies on the terrain. Each stroke is one Undo step
  - Terrain has a new **Hills** checkbox: turn it off and you get flat ground to build a map from scratch
  - what you sculpt and paint is stored in the component itself (it is saved with the scene and works in prefabs and Undo). Physics and characters walk on the new terrain. When the resolution changes, the sculpted data is resampled
  - Create → Terrain in a project without the Starter Content adds the M_Terrain material by itself
  - console `terrain.sculpt <mode> <x> <z> [seconds] [radius] [strength]`
- **File → Settings** (Ctrl+,): all the settings in one window:
  - Editor: interface scale, sounds, scene autosave
  - Viewport: camera speed, mouse sensitivity, field of view, grid, snapping (the step for move / rotate / scale), one-sided scaling
  - Rendering (this computer): resolution, anti-aliasing (TAA / FXAA / none), shadow quality, AO, reflections, volumetric fog, clouds, bloom, DOF, motion blur, vsync
  - Audio: master volume, music, effects
  - Project: graphics era, Features, engine profile
  - they are saved in Saved/Config/EditorSettings.json
- **One-sided scaling**: when you drag a scale handle, the object stretches towards that handle while the opposite side stays in place (the centre handle keeps the bottom on the ground). It is on by default. A button on the toolbar works with the Scale tool; Alt while dragging uses the other mode

## 0.13.13 — game UI: UI in the world, scrolling, input fields, wrapping (2026-10-18)
- **UI in the world** (UI Canvas → World Space): the canvas hangs above its own object, so names and health bars show above characters, NPCs and cars. It shrinks with distance (Full Size Distance / Min Scale), hides behind the camera and beyond Max Distance. Distant ones are drawn under near ones and under the screen UI. Create → UI Name Plate (World) makes a plate with a name and a health bar above the selected object
- **Scrolling** (UI Element → Scroll): children are not squeezed; they scroll with the mouse wheel. A scroll bar, clipping to the frame. Create → UI Scroll View
- **UI Input Field**: click and type (chat, a player's name, a password shown as dots). Backspace; Enter submits (`fn onUISubmit(name, text)` in scripts; Clear On Submit for chat); Escape leaves. `entity.text` reads and writes the field's text. Cyrillic and any UTF-8 characters work. While the field has focus the game gets no input (typing does not move the character)
- **Wrapping in flex** (UI Element → Wrap): a row of children moves onto the next line (inventories, tag lists)

## 0.13.12 — the Empty template and the graphics of past years: 2017, 2011, 2008 (2026-10-18)
- **Empty template**: a completely empty scene with no objects and no lights (the Starter Content can still be added with a checkbox)
- **Graphics 2017 / 2011 / 2008 templates**: an empty level (sun, sky, fog, clouds, ground), and the project renders the way the best games of that year could:
  - 2017 (Battlefield 1, Horizon Zero Dawn): everything except PCSS shadows and fluid simulation. TAA, SSR, volumetric fog and clouds
  - 2011 (Crysis 2, Battlefield 3, Skyrim): deferred, SSAO, FXAA, three shadow cascades, Filmic tonemapping. No TAA, SSR, volumetric fog or 3D clouds
  - 2008 (Crysis, GTA IV, Fallout 3): forward, early SSAO, two cascades of hard shadows, reflections of the sky only, strong bloom, no anti-aliasing
- The graphics era is a project setting (`graphicsEra` in the .zproj). It is changed in Window → Features (the Graphics section) or with the console `graphics.era <modern|2017|2011|2008>`, and it applies in the packaged game too
- A new **Filmic** tonemapper (Hable / Uncharted 2), for Post Process Volumes and the older eras

## 0.13.11 — effects: new particle features, an explosion, a portal, snow (2026-10-18)
- **Particle Emitter**, new settings:
  - Mid Color + Mid Color Time: a gradient of three colours (fire: white-yellow → orange → dark red)
  - Size Curve / Alpha Curve: non-linear growth and fading (a fireball swells quickly and slows down; a flame stays bright almost its whole life and fades at the end)
  - Burst Interval: the flash repeats every N seconds. Start Delay: a delay before the start (stages of an effect: smoke after a flash)
  - Orbit: rotation around the emitter's axis (vortices, portals, tornadoes). The Ring shape: particles are born on a circle
  - Light Flash: a Point Light on the object flashes with each emission and fades (explosions)
  - Stretch: the length of a spark's trail as the path over N seconds (short sparks, long tracers)
- **Particle shader**: smoke is lit as a volume (sunlight from the side, self-shadowing inside the cloud, glowing edges against the sun, sky light from above). Puffs with three octaves of noise. Sparks have a white-hot core. Glowing particles keep their colour instead of burning out to white
- **Starter Content** redone: a flame with a hot core among the logs by the campfire, thick billowing smoke, embers swirling above the fire, and welding sparks that cool from white to red and bounce off the floor
- **New effects**: P_Explosion (a flash of light, a fireball, black smoke, burning debris, a ring of dust along the ground, a shock wave that distorts the air, embers; it repeats every 4.5 s). P_Magic is now a portal (a ring of rotating sparks and flames, glowing in the centre). P_Snow (snowfall over 30×30 m, swirling in the wind)
- The Starter Content showcase has an explosion added

## 0.13.10 — clouds rewritten (2026-10-18)
- **Weather map**: clouds grow in clusters with gaps between them, not as an even scatter. Each place has its own cloud type
- **Three types**: stratiform (a flat sheet), cumulus (domes like cauliflower with flat bases) and cumulonimbus storm clouds (towers up to 9 km with an anvil on top). The Cloud Type parameter blends them
- **Lighting like UE5 / Horizon**: multiple scattering (the clouds are white, not grey), a silver lining around the sun, dark bases on thick clouds, the diffuse light of the sky, and light bounced from the ground. At sunset the edges are golden
- **Curvature of the Earth**: the clouds go over the horizon and sink into the haze of the atmosphere
- The clouds change shape slowly (Evolution). Their edges are eaten away by fine noise (Detail). The size of the clouds and the gaps is Scale; Seed gives a different pattern for the cloud fields
- **Lightning** inside the storm clouds (Lightning)
- **Presets** in the Volumetric Clouds component: Clear, Fair Weather, Scattered, Cloudy, Overcast, Storm (Custom for your own settings). Console `clouds.preset <name>`. The templates start with Fair Weather
- The cloud shadows on the ground are deeper (under solid cloud cover the sun almost never breaks through). The clouds move with the game time
- Distant mountains and objects can stand inside or behind the clouds (the composite takes depth into account)
- Performance: each frame computes one pixel of every 2×2 block (the others come from past frames). Noise with mip levels. The expensive lighting calculations are limited per ray: about 3–3.5 ms on UHD 620 (Lite, a quarter of the resolution). Full uses half the resolution and more steps
- Console: `sun <height> [azimuth]` turns the scene's sun
- **Fixed the "GPU device lost" crash** when the viewport was resized under heavy GPU load (a window larger than the screen, a layout reset, a heavy scene). A texture deleted between frames was freed while the GPU was still drawing the previous frame. Now the deletion waits for the last submitted frame to finish. When the device is lost, the log (DRED) names the pass where it happened

## 0.13.9 — underwater, buoyancy, splashes (2026-10-18)
- **Underwater view**: if the camera is in the water, the scene sinks into the water's colour with distance (red fades first), caustics dance on the bottom and on objects, light rays pass through the water, and the picture drifts slightly. With the camera half in the water, the waterline is visible: above it the world over the water, below it the underwater world
- **The surface seen from below**: a Snell's window (above your head you see the sky and clouds, refracted), and total internal reflection at the edges, as in real water. The whitecaps' foam shows from below as dark patches
- **Buoyancy** (Add Component → Buoyancy, together with a Rigid Body): an object floats on the same waves that are drawn. It rocks, drifts and damps the rocking. Density decides how deep it sits: wood at about 600 kg/m³ (60 % under water), ice 920, anything heavier than 1000 sinks. Water resistance, rocking damping, volume (taken from the colliders by default)
- **Splashes**: any physical body falling into the water raises a splash (drops and water spray, sized by speed and mass). Fast-moving objects leave spray at their bow
- The Water template: a boat at the pier rocks on the waves, and crates, barrels and a log float in the sea
- The waves now run on game time (pause and time slowdown stop them too). Lite and Full show the same large waves
- Fixed: the wind's force on physical bodies acted only on the first physics step of a frame (at low FPS the wind was weaker). Now the forces are applied before each step

## 0.13.8 — asset packs when creating a project; switching the Lite / Full profile (2026-10-17)
- **Asset Packs** in the New Project window (like the Starter Content): tick the packs that come with the engine (`Engine/Content/Packs`), and they are copied into the project at `Content/Packs/<Pack>`. All three are from Quaternius, licence CC0 (usable anywhere, including commercial games):
  - **Medieval Village MegaKit**: 176 modular pieces of medieval houses (walls, floors, roofs, doors, windows, balconies, stairs) with PBR textures
  - **Stylized Nature MegaKit**: 68 models: trees, spruces, dry and crooked trees, bushes, grass, flowers, ferns, mushrooms, rocks, path tiles. The shared textures are moved into one folder (an archive of 86 MB → 19 MB). The foliage uses two-sided alpha testing (in the source it was semi-transparent and was not drawn)
  - **Survival Pack**: 53 items: weapons, tools, tinned food, first-aid kits, a tent, a raft, a campfire, torches. Converted from .blend to .glb and scaled to real sizes (in the source the axe was 2.5 m and the tent 25 m). The pivot point is at the bottom
- `Tools/import_asset_packs.py <folder with the archives>` rebuilds the packs from the original archives. `-packs=all` (or a comma-separated list) picks packs when a project is created from the command line
- **The Lite / Full engine profile can be changed at any time**: click the LITE / FULL badge on the toolbar (or the profile line at the bottom of the Project Browser, or Help → Engine Profile). The window shows the current profile and the one recommended for this computer. Choosing Full on integrated graphics shows a warning that the FPS will be very low. "Restart Now" saves the choice (%LOCALAPPDATA%/ZeqouX3D/EngineProfile.txt), asks about unsaved changes, and restarts the editor on the same project. Switching back works the same way. Console: `profile.switch [lite|full|auto] [restart]`
- Console `import <path> [x y z]`: the path can be relative to Content (Packs/StylizedNature/Pine.gltf)

## 0.13.7 — water rewritten: an FFT ocean (2026-10-17)
- **Waves are an FFT simulation of the ocean** (as in Sea of Thieves / Crest / UE Water): three cascades (swell, waves, ripples). The JONSWAP spectrum with directional spread is computed from the wind speed and the fetch (the distance the wind blows over water). Each frame on the GPU, the spectrum evolves and an inverse FFT produces the displacement and slope textures. Sharp crests (choppiness). The waves run downwind from the Wind Zone
- **Whitecaps**: foam builds up where a crest "breaks" (the Jacobian of the displacement) and melts away in a few seconds. Foam also appears at the shore and around objects
- **Shading**: scene reflections (SSR) over a filtered sky, the sun's glint with shadows from the cascades, light through the crests (SSS), absorption with depth, refraction, caustics on the bottom. The foam and caustics textures are seamless (baked once)
- **Mesh**: the large water is rings around the camera (detailed close up, reaching the horizon far away). The small water is a regular grid
- **Presets** in the Water component: Ocean, Lake, Pool, Storm, Tropical (Custom for your own settings). Console `water.preset`. The Water template is a 3 km tropical sea. The Starter Content pool / fountain uses the Pool preset
- Lite profile: 64×64 cascades and a coarser mesh (about 3–5 ms on UHD 620)
- The editor's autotests no longer react to the real mouse of the user (the flag `-autotestinput` turns that on)

## 0.13.6 — wind for the whole scene, adjustable for each object (2026-10-17)
- **Wind Zone** (Create → Wind Zone): a source of wind. Either directional (across the whole scene, along the entity's blue arrow) or spherical (it blows out from the centre and fades towards the radius). Speed, swirling, gusts (strength and frequency). There can be any number of zones, and the wind adds up. In the viewport there is an arrow showing the wind and a ring for the radius
- **Wind Response**: on any object, the strength of the wind it receives (0 = sheltered from the wind, 2 = twice as strong; it also affects the children), its own wind (direction and speed; zones are ignored), and for physical bodies, how hard the wind pushes them (area × drag coefficient)
- Things that react to the wind: **particles** (smoke, sparks, rain, snow are carried off at a speed set by their air resistance, on the CPU and the GPU), **fire and smoke from burning** (the smoke column leans), **volumetric fog** (Fog Volume), **clouds** (direction and speed; at the top the wind is 2.5 times stronger), **water** (the waves run downwind and grow with its strength), **physical bodies** with Wind Response, and the **car** (air resistance taking the wind into account). If the scene has no wind zones, the clouds, water and fog work with their own settings, as before
- The templates now have a light breeze (a Wind zone, 4 m/s)

## 0.13.5 — prefabs: a Revert / Apply bar, no "beep" on Play (2026-10-17)
- The position and rotation of a prefab instance's root are no longer counted as a change (as in Unity / UE). A newly placed prefab shows "no overrides". After Apply, the changes are reset (a test)
- The prefab bar in Details: the name is cut with "…" and does not overlap the buttons. Revert / Apply are inactive when there is nothing to change
- The editor's feedback sounds (the signal on Play / Stop, a build, packaging) are off by default. This was the "beep" at start-up. They are turned on with the command `editor.UISounds 1`
- Console: `prefab.overrides [all]` shows which fields of an instance differ from the prefab

## 0.13.4 — fixed: the editor panels did not react to the mouse (2026-10-17)
- **The real cause of "nothing clicks"**: in the definition of the window under the cursor, "no modal window = order -1". All the docked panels (Viewport, Outliner, Details, Content Browser, Output Log) have a negative order. So they were always treated as "under a modal window" and got neither hover nor clicks. Only the title bar with the menu and the toolbar worked. The bug had existed since docking appeared (M7). It is fixed, and a test was added that clicks a button in a docked window (without the fix, the test fails)
- The fixes from 0.13.3 (a stuck mouse button, a stuck flight mode) remain as additional protection

## 0.13.3 — fix: the editor stopped responding to clicks (2026-10-17)
- If a window did not receive the message that the mouse button was released (a loss of mouse capture, for example during Play with the cursor captured), the editor considered the button held down for ever, and no click worked again until you switched to another window. Now a lost capture releases the buttons, and every frame the button state is checked against the system
- The camera's flight mode (the right button) disabled input in the whole interface and could "stick" if the viewport was hidden behind another tab. Now it ends as soon as the right button is released

## 0.13.2 — game UI: HUD, menus, buttons, sliders (2026-10-17)
- **Game interface** (the start of M14): widgets are ordinary scene entities under a **UI Canvas**. So they are visible in the Outliner, configured in Details, included in prefabs, undo and saving, and scripts find them by name
- **UI Element**: the box of any widget. Its size (0 = by content), the layout of its children as in flexbox (direction, alignment along and across, gaps, padding, "stretch" into the free space), or **anchoring** to a corner or the centre of the parent (anchor, pivot, offset), for HUDs. Everything scales with the window's height (the canvas's Reference Height)
- Widgets: **Panel** (colour, rounding, border, shadow), **Text** (size, Inter weight, alignment, line wrapping, a shadow for legibility), **Image**, **Button** (hover and press colours, a click sound), **Progress Bar**, **Slider**, **Toggle**. A click fires on release over the same button; a slider is dragged with the mouse
- Scripts: `fn onUIClick(name)`, `fn onUIChanged(name, value)` (on the widget or on its parents, for example on the canvas), `find("Score").text = "…"`, `find("Health").value = 0.5`, `find("Menu").visible = true`, `ui.cursor = true` (shows the mouse for menus), `time.scale = 0` (a pause: physics, animations and particles stop, the UI keeps working), `time.realDt`
- The UI is drawn over the viewport: in the editor as a preview (Show → Game UI), in Play over the game camera, and in the packaged game over the frame (the fonts are now in Content.zpak). Create → UI: Canvas, Panel, Text, Button, Image, Progress Bar, Slider, Toggle. The console `create ui.button` and similar
- **Vehicle Playground**: a speedometer (km/h) and a damage bar, a controls hint, a pause menu on **Tab** (Resume, Repair, volume). The script `CarHUD.zs`

## 0.13.1 — soft bodies: a car like BeamNG.drive, the Vehicle Playground template (2026-10-17)
- **Soft Body** (a new component, "nodes and beams" physics as in BeamNG.drive): the body is a lattice of nodes with mass, joined by beams. The beams spring back. When overloaded they **bend for good** (dents stay), and when stretched too far they **break**. The nodes collide with the world (static objects and rigid bodies: crates and barrels fly off). Four wheels on suspension (a spring, a shock absorber, a bump stop), tyres with a grip limit (sliding, wheelspin), rear, front and all-wheel drive, brakes and a handbrake. A bent suspension angles the wheels together with the body. Settings: mass, stiffness, deformation give, the breaking threshold, power, brakes, grip, suspension, steering, and the number of substeps
- The body mesh (a retro saloon with windows, headlights and tail lights) is skinned to the nodes on the GPU, so dents show without rebuilding the mesh. An Audio Source on the car becomes the engine sound (the pitch follows the revs)
- **Ready-made scripts** in the Starter Content: `SoftBodyCar.zs` (W/S for throttle and brake, reverse after stopping, A/D for steering, Space for the handbrake, R to repair and put the car back on its wheels; the crunch of metal on impacts), and `CarCamera.zs` (a camera behind the car; the right mouse button to look around). Scripts: `entity.carInput(throttle, steer, brake[, handbrake])`, `carSpeed()`, `carDamage()`, `carImpact()`, `carReset()`
- The prefab **Characters/P_SoftCar** and a new project template **Vehicle Playground**: a ramp, a brick wall and concrete posts for crashes, walls of crates and a pyramid of barrels, a road of logs (you can see the suspension at work), a steep hill
- Console: `car.drive <throttle> <steer> [brake]` / `car.drive off` to drive cars without a script (for debugging)
- Fixed: `entity.lookAt(point)` on an entity without an Animator (for example, a camera) turns it again (since 0.12.3 it raised the error "No Animator")

## 0.13.0 — M13 "Animation" closed: root motion, morph targets (2026-10-17)
- **Root motion** (Animator → Root Motion): the movement of the root bone in the clips moves the character itself, either its Character Controller (stopped by walls and steps) or the top-level entity. The model stays over the entity. It works in graphs (a blend space and transitions blend the movement too) and without a graph. Clip loops do not jump back
- **Morph targets** (blend shapes): imported from glTF together with their names and the weight animation. The offsets are computed on the GPU in the same pass as the skinning (and for meshes without a skeleton). A new **Morph Targets** component: the weights by name in Details. Scripts: `entity.setMorph("Blink", 1)`, `entity.morph("Blink")`
- **A slime** in the Starter Content (`Characters/SK_Blob.glb`, made by the script `Tools/make_starter_blob.py` in Blender): morphs Squash / Stretch / Blink, and clips Idle and Bounce. In the third-person template, three slimes bounce along the path with a "squelch" when they land (the script `Slime.zs`)
- Animation Viewer: `anim.viewer <model> [clip [time]]`. Scrubbing while paused no longer resets to zero when the clip changes
- The format of the ready-made assets has changed (CookerVersion 5): the models are re-prepared the first time they are opened
- **M13 closed**: skeletal animation, an AnimGraph with layers and 1D / 2D blend spaces, IK, events, root motion, morph targets, the animation editors

## 0.12.5 — animation layers, bone masks, additive, 2D Blend Space (2026-10-17)
- **AnimGraph layers** (like Animator layers in Unity, or layered blending per bone in UE): on top of the base state machine, there are your own state machines using the same parameters. A layer has a **bone mask** (a bone and everything below it, for example `Spine` for the upper body), a **weight** and a weight parameter, and a mode: **Override** (replaces the bone poses under the mask) or **Additive** (adds movement relative to the first frame of the state: breathing, recoil, leaning on top of any animation). A layer state with no animation mutes the layer. Transitions into and out of it switch the layer on and off smoothly
- **2D Blend Space**: clips on a plane of two parameters (for example speed and direction). Blending without triangles (gradient band interpolation). The phases are synchronized as in 1D
- **AnimGraph Editor**: layer tabs (Base Layer, the layers, "+" for a new layer), layer properties (name, Override / Additive, weight, the weight parameter, the mask bones), the motion mode of a state None / Single Clip / Blend Space 1D / 2D. For 2D, a field with samples and a preview point: drag the point to see which clip weighs how much
- **Punch in the third-person template**: LMB or F, and the character punches with its arm while its legs keep walking or running (the UpperBody layer of the Locomotion graph). At the moment of the punch (the `PunchHit` event), the bodies in front of the character fly away, and a thud sounds
- Scripts: `entity.animState(layer)` gives a layer's state. `entity.animState()` without an Animator returns `""` (so you can check whether there is an animated model)

## 0.12.4 — animation editors: the Animation Viewer and the AnimGraph Editor (2026-10-17)
- **Animation Viewer** (Content Browser → right-click a model → Open in Animation Viewer): the model in its own preview with a floor and a shadow, a list of clips, a timeline with scrubbing (scrubbing pauses), speed, looping, and an orbit camera (drag with the mouse, the wheel zooms)
- **AnimGraph Editor** (double-click a `.zanimgraph`, or right-click in the Content Browser → New Anim Graph): states are nodes, transitions are arrows, and there is an Any State node. Right-click to add a state, make a transition, set the entry state, or delete. A parameters panel (name, type, default value; renaming updates the conditions and the blend space). Properties of the selected state (a clip, or a Blend Space 1D with samples, speed, looping, events) and of the transition (duration, exit time, conditions). Undo / Redo, Ctrl+S. Saving applies at once to the animators that are playing. In Play, the state of the selected entity's animator is highlighted
- The node layout is stored in the `.zanimgraph` (`pos`, `anyPos`)
- Console: `anim.viewer <model>`, `anim.graph <graph>`

## 0.12.3 — IK: feet on uneven ground, looking at a target, footsteps in time with the legs (2026-10-17)
- **Foot IK**: in the game, the character's feet stand on steps and slopes. Under each foot there is a ray to the ground, the hips drop for the lower foot, the knee bends (two-bone IK), and the sole lies along the slope. It switches on and off smoothly (in a jump the legs return to the animation). Settings: Foot IK, Foot Reach, and the foot bones in the Animator
- **Looking at a target**: `entity.lookAt(point [, weight])` turns the head towards a point (no more than 70°, smoothly). `entity.lookAt(nil)` looks forward again
- **Animation events**: footsteps are detected by themselves. When a foot touches the ground during movement, scripts receive `onAnimEvent("Footstep")`. In a graph you can add your own events on the state's timeline (`events`). The third-person template's character now stamps its footsteps exactly in time with its legs
- The IK maths in the engine: a two-bone solver (an exact hit on the target), and the rotation of a bone in skeleton space

## 0.12.2 — AnimGraph: an animation state machine, a living character in the third-person template (2026-10-17)
- **AnimGraph** (a `.zanimgraph` file, like an Animator Controller in Unity or a state machine in UE's AnimBP): parameters (number, bool, trigger); states are a clip or a Blend Space 1D (clips placed on a parameter's values, with synchronized phases so that the walking and running steps line up); transitions with conditions, a duration and an exit time; transitions "from any state". Clips are referenced by name, so the graph fits any model that has the same clips
- **The Character Controller drives the animation by itself**: the parameters `Speed`, `VerticalSpeed` and `Grounded` are filled in by the controller on the entity or its parent. A short loss of ground on a step is not counted as a jump
- The Starter Content has a **Locomotion** graph: Idle → Walk → Run by speed, a jump when leaving the ground, and landing
- **Third-person template**: instead of a mannequin made of primitives, an animated character that stands, walks, runs (Shift) and jumps
- Scripts: `entity.animParam(name, value)`, `entity.animTrigger(name)`, `entity.playAnim(clip)`, `entity.animState()`. They work with an Animator on the entity or on its children
- The Graph field on the Animator. Without a graph, the Animator still plays the chosen clip

## 0.12.1 — skeletal animation: importing skins and clips, GPU skinning, the Animator component (2026-10-17)
- **Animated models from glTF**: the skeleton and the animations are imported together with the mesh (Skeleton and Animation assets next to the model's meshes and materials). Bones do not become entities; they live in the skeleton
- **The Animator component** appears on a skinned mesh by itself. It plays a clip (Idle by default), with speed, looping, a smooth transition when the clip changes (Blend Time). The animation is visible in the editor too (Preview In Editor). The clip is picked from a dropdown of the model's clips
- **GPU skinning**: a compute pass poses each character's vertices into its own buffer. After that, all passes (shadows, deferred, reflections, selection outline) draw it as an ordinary mesh. Motion blur and TAA see the movement of the arms and legs (the previous frame's buffer)
- **The starter character** in the Starter Content: Characters/SK_Human.glb (Quaternius, CC0), with Idle, Walk, Run, Jump, Punch, Working and Death
- **Import scale** of models in .zmeta (`settings.scale`), for models in other units
- Shadows: animated characters refresh the cascades they touch every frame

## 0.12.0 — milestone M12 "GPU-driven rendering + Deferred path" closed (2026-10-17)
- **Deferred path**: opaque surfaces are written to the G-buffer (base colour + AO, normal + roughness, subsurface + metallic; emission goes straight into the frame). Then one full-screen pass lights each pixel exactly once. The shader library is shared with forward+: the same material, BRDF, shadows, reflections and clustered lights, so the picture matches forward (average difference < 0.5 out of 255). Graph materials get a G-buffer variant automatically. Transparent and debug modes use forward
- `r.Renderer.Path 0/1` (forward+ / deferred). The Full profile defaults to deferred, Lite to forward+ (on integrated graphics both cost the same in Sponza, about 5.7 ms; deferred wins with many light sources)
- **Milestone acceptance**: 100 000 objects in Sponza on UHD 620: CPU 26 ms, GPU 32 ms in the editor (of which about 8 ms is the interface and the selection outline). That is ≥ 30 FPS, and the CPU is not the bottleneck
- Summary of the milestone (0.11.3–0.12.0): GPU culling and ExecuteIndirect, soft shadows (PCSS), contact shadows, a shadow cache that handles moving objects, particles on the GPU, the deferred path. Volumetric fog was done earlier (0.10.9)

## 0.11.5 — particles on the GPU: hundreds of thousands of sparks, collisions with the visible scene (2026-10-17)
- **GPU Simulation** on a Particle Emitter: birth, movement, turbulence, colour and size over lifetime are computed in compute shaders. The CPU only says how many particles to spawn. All shapes (sphere, cone, box), flipbooks, lighting and fog are the same as for CPU particles
- **Collisions from the depth buffer**: GPU particles bounce off (or vanish against) everything the camera sees, such as floors, walls and props, with no physics and no raycasts. On a floor, particles come to rest. Along walls, they slide down
- **200 000 sparks**: about 5 ms to simulate and about 6 ms to draw on UHD 620. The particles are stored as a compacted array (the survivors are rewritten contiguously into another buffer). A "pool with a free list" scheme was 5 times slower
- A new prefab **Effects/P_SparkShower** (a cascade of sparks with a crackling sound) is in the Starter Content's demo level
- Console: `stress.particles N` is a fountain of GPU sparks in front of the camera
- Limits of the GPU mode: particles are not sorted (best for additive effects: sparks, magic, rain). There is no light, no heat haze and no On Hit. For these, the CPU path remains
- Fixed: there was no UAV barrier between passes that write to the same UAV resource (the GPU could start the second pass before the first pass's writes had finished)

## 0.11.4 — soft sun shadows, contact shadows, a shadow cache for moving objects (2026-10-17)
- **Soft shadows (PCSS)**: a shadow is sharp where an object touches the ground and softer further from it, as with the real sun. The softness is set by the sun's angular size (`shadows.sourceAngle`, 1.5° by default). On in the Full profile; in Lite, by `r.ShadowFilter 3`
- **Contact shadows**: a short ray towards the sun through the depth buffer finds the small shadows that the cascades miss, under pots and furniture, by stones, in cracks and under ledges. About 1 ms on UHD 620. `r.ContactShadows 0/1`
- **Fixed: shadows of moving objects.** The cascade cache redrew the sun's shadow only when the camera moved. So with a still camera, the shadow of a falling crate stayed in its old place. Now a cascade is refreshed when an object in it moves (the old and the new position). In a still scene, it still is not redrawn
- `stat.scene` shows the data of the GPU-driven path and the number of redrawn cascades

## 0.11.3 — GPU-driven rendering: culling on the GPU and ExecuteIndirect (2026-10-17)
- **GPU culling**: opaque geometry is no longer walked on the CPU object by object. A compute shader tests each section of each mesh against the view frustum and the depth buffer (HiZ, two-phase: first against the previous frame's depth, then against what was just drawn). It picks the LOD and writes the draw arguments itself. The CPU issues one `ExecuteIndirect` per "shader + mesh" group. The sun's cascades are culled the same way (only the frustum, with their own LOD)
- **100 000 objects in Sponza**: a frame was about 130 ms (≈ 20 000 draw calls) and became about 32 ms on UHD 620. The CPU part of the frame is about 25 ms, the depth pre-pass went from 27 to 2 ms, and the main pass from 34 to 8 ms. The picture matches the old path
- Preparing a frame on the CPU is cheaper for ordinary scenes too: collecting instances and updating world matrices run in parallel. The "this / previous frame" pairs for motion blur are made in one pass without a hash table. The previous frame's matrix is copied only for objects that moved. The resolution of meshes and materials is cached between frames
- Console: `r.GPUDriven 0/1` (the old CPU draw lists, for comparison), `r.OcclusionCulling 0/1`, `stress.instances N` (N objects around the camera), `stat.cpu` (the profiler zones of the main thread for the frame)

## 0.11.2 — sound everywhere: ambience, footsteps on surfaces, rolling, props with sounds (2026-10-15)
- **Every template makes sound**: new levels have an ambience "birds and wind" (an Ambience entity next to the sky). Before, nothing played in the first- or third-person templates during play until you walked somewhere
- **Footsteps on surfaces**: the character hears what it walks on: grass (and terrain), wood (and hay), snow, sand, carpet, concrete. The surface is taken from the material (`M_Grass`, `M_Wood`, and so on), with no setup. In scripts, `character.surface` and `hit.surface` for ray hits
- **Jumping and landing**: a rustle of clothes on a jump, a dull thud on a hard landing (louder from a height)
- **Rolling and sliding**: Impact Sound has a new field, Roll Sound. It is a loop that plays while a body rolls or slides over something. It gets louder and higher as the speed rises (the balls and barrels in the templates). It works on Stay events
- **Props with sounds**: steam hisses, a fountain gurgles (a real stream instead of rain), a lift hums with its motor and clicks its latch at each floor, the magic sphere hums, the scanner in the Physics Playground hums, the physics gun fires, picks up and throws
- **Editor sounds** (quiet; turned off with `editor.UISounds 0`): the start and stop of Play, the success or failure of packaging and of the C++ compile
- New CC0 sounds in the Starter Content (169 files): birds and wind (Spring Spring), a stream, boiling, steam, rolling, a rumble (rubberduck, OpenGameArt), interface (Kenney Interface Sounds), lasers and items (Kenney)
- Fixed: a ray from a script that starts inside its own body (the character's capsule) did not find the ground under the feet

## 0.11.1 — packaging the game: a separate .exe without the editor (2026-10-15)
- **File → Package Project**: the project is built into a folder with a ready game: `<Name>.exe`, `Content.zpak`, `Game.json`, and the shader compiler's DLL (plus the game module's C++ DLL, if there is one). It runs in the background. The window shows the progress and an Open Folder button. The console command `package [folder]`. By default it goes to `Builds/Windows/<Name>` in the project. Options: full screen (borderless), and "Lite textures" (textures up to 1024 px, for a smaller package and weak GPUs)
- **ZXGame**: the runtime of the packaged game. It mounts the package, loads the start scene, waits for the meshes to load and the material shaders to compile, and starts the game. The game captures the mouse; Esc releases it, and a click captures it again. Logs and the shader cache go to `%LOCALAPPDATA%/<Game name>/Saved`. Flags: `-windowed`, `-resolution=WxH`
- **The .zpak format**: files are cut into blocks of 64 KB and compressed with our own ZLZ (incompressible blocks are stored as they are). The table of contents is at the end. The package is mounted into the engine's file system as a read-only folder
- Packaging again "cooks" the project for the paths of the packaged game (`/Game/...`). So the game finds all the ready data and recomputes nothing at start-up. The warmed-up shader cache of the editor goes into the package
- **Play in the editor and the packaged game use one game loop** (`Engine/GameRuntime.h`). The order of the systems (scripts, the C++ module, characters, physics, fire, sound) is now shared. What works in Play works in the build too
- Verified: Physics Playground, Water and a C++ project were packaged, moved to a folder outside the engine and run, with no errors, and the D3D12 debug layer clean

## 0.11.0 — milestone M11 "Physics" closed (2026-10-15)
- **Physics Playground template** in New Project: stacks (a tower, a pyramid, a brick wall), dominoes along an arc (they fall in a chain on Play), a ramp with balls and barrels, a heap of rocks (convex hulls), joints — a bridge, a door, a lamp, a lift, a chain, a swing — a trampoline, a scanner beam (raycasts), and a first-person player with a physics gun
- Starter Content scripts: **PhysicsGun** (left button pushes what you look at; right button holds and carries a body) and **Scanner** (a beam sweeps the scene, a dot shows the hit)
- **Parallel solving of islands**: independent groups of bodies are solved on different cores (24 stacks: the solver went from 1.85 to 0.81 ms). The result is bit-for-bit identical to the sequential one. One large island is solved as before
- **Show → Physics Debug** in the view window (and the action view.physicsDebug): during Play, bounding boxes of bodies by island (grey = sleeping, blue = kinematic), contact points and normals, joints
- **Stay events**: `collisionStay(other)` / `triggerStay(other)` in ZeqouScript, and `OnCollisionStay` / `OnTriggerStay` in C++ (Game SDK v3; C++ modules are rebuilt). They fire on every physics step while the bodies touch
- **Closest-point query**: `physics.closestPoint(point[, range[, layers]])` returns the nearest point on the surface of any collider (inside, the distance is 0). Batched rays `PhysicsWorld::RaycastBatch` (in parallel)
- **Cylinder Collider**: a true cylinder (a convex hull of 32 vertices). Create → Cylinder now gets it instead of a capsule, stands on its end, and rolls on its side
- **Rolling friction** on Rigid Body (`rollingFriction`): a ball no longer rolls for ever (0.05 is a ball on grass)

## 0.10.17 — terrain and HeightField physics (2026-10-14)
- **Terrain component** (first version): a square landscape from fractal noise, from gentle hills to sharp ridges (Ridges). Size, resolution up to 1025 × 1025, height, seed, the scale and roughness of detail, and a flat patch in the centre for buildings. Create → Terrain places it under the scene: the flat middle covers whatever is already standing, and it sits a little below the floor. Change the parameters in Details and the landscape rebuilds on the fly (257 × 257 takes about 40 ms)
- The terrain is drawn through an ordinary Mesh Renderer (it is added by itself), so selection, shadows, the outline, Drop to Ground and the Brush tool work on it like on any mesh. The mesh is cut into chunks, and invisible chunks are culled
- **M_Terrain** in the Starter Content: grass on the flat, rock on the slopes, snow above a set height. No textures and no painting: it works from the normal and the height
- **HeightField**: a new physics shape, a grid of heights instead of a triangle mesh (less memory, fast cell queries, rays walk the cells along the ray). The terrain collides as a HeightField. Below the surface, everything counts as ground: a body that falls through is pushed up, not through. UpdateHeightField changes part of the heights on the fly (for future sculpting) and wakes what lies on it
- Rays against triangles (meshes and HeightFields) no longer slip exactly through a shared vertex or edge
- The Sun sliders in the view window can now be undone (Ctrl+Z): one step per drag

## 0.10.16 — convex hulls: dynamic bodies of any shape (2026-10-14)
- **Convex Hull**: a new physics shape, the convex hull of a point cloud (Quickhull). Coplanar triangles merge into polygonal faces of no more than 32 vertices; the extra ones are removed with minimal loss of shape. Mass, the centre of mass and the inertia tensor are computed from the volume of the hull
- **Mesh Collider → Convex**: a tick turns a mesh's collider into its convex hull. Now stones, barrels, crates and any models can be dynamic Rigid Bodies. Instances of one mesh with the same scale share one hull. If the mesh is still loading, the body appears as soon as the mesh arrives. A green wireframe of the hull is shown in the view window
- Collisions of hulls with everything: with boxes and other hulls (SAT over faces and edges, plus face clipping, up to 4 contact points), with spheres and capsules (GJK: the closest points; inside, the least-penetrated face), with the level's triangle meshes, and rays (Cyrus–Beck)
- The editor's physics self-test (`-physicstest`) drops a convex cylinder onto a stack of cubes

## 0.10.15 — scene lighting like UE, a brighter picture (2026-10-14)
- **Directional Light component** (the sun): the entity's rotation is the sun's direction. Brightness (1 = a real sun), colour, shadows. The Sun sliders in the view window now turn it, if it is in the scene
- **Sky Light component**: the strength of the diffuse light from the sky, its tint, and the brightness of the visible sky
- The new template levels are assembled like a new UE level: Sun, Sky Light, Height Fog and Volumetric Clouds are separate entities, all set up in Details. Create → Directional Light / Sky Light
- **A brighter picture**: the auto-exposure compensation by default is +1.6 EV (it was +1.0), and the visible sky is brighter. (The physical sky next to ground lit by the sun looked dusky and dark blue.) Now the daytime sky is blue, and the scene is light, as in UE by default

## 0.10.14 — motion blur of moving objects (2026-10-14)
- **Object velocity (velocity buffer)**: each frame the engine compares the positions of objects with the previous frame. For the ones that moved, it draws their screen velocity into a separate texture (a Velocity pass, only for moving objects, a fraction of a millisecond)
- **Motion Blur of objects**: motion blur now works on the objects themselves (wheels, blades, falling crates, a flying projectile), not only when the camera moves. The still background stays sharp. It is switched on in the Post Process Volume (Motion Blur), as before
- **TAA without trails**: the anti-aliasing takes its history along the object's motion, so moving objects have fewer ghosts
- The editor's command line: `-exec="play; wait 60; stat.gpu"`. `wait N` delays the rest of the chain by N frames (for snapshots and measurements during Play)

## 0.10.13 — fire and smoke flipbooks (2026-10-14)
- **Flipbooks on particles**: Particle Emitter → Flipbook, an animated atlas of frames (columns × rows). It plays over the particle's life, or in a loop at a set rate from a random frame, and neighbouring frames blend smoothly. For the Flame look, the red channel is temperature (dark orange edges → a yellow-white core), and alpha is the shape. For Soft, rgb is the shading and alpha is the density
- **Flipbooks baked from a fluid simulation** (`Tools/bake_flipbooks.py`, a 2D stable-fluids solver with buoyancy and vortices). T_Flipbook_Fire: looping ragged tongues of flame. T_Flipbook_Smoke: a lumpy cloud that grows and billows. The fire, the campfire, the torch and the smoke of the starter set, and burning objects (Flammable), use them, so flame and smoke look as they do in games and not like procedural blobs. Without the starter set, the procedural shapes remain
- A GPU particle now uses flags in place of two floats, and the atlas index and the frame grid fit in the same 64 bytes

## 0.10.12 — relief, subsurface scattering, LUTs (2026-10-14)
- **Parallax Occlusion Mapping**: a **Parallax Occlusion** node in the material graph (a height texture, the depth of the relief, the number of steps) shifts the UVs that the other textures read. Bricks, cobbles and stones look three-dimensional on flat geometry, and the relief changes with the viewing angle
- **Subsurface scattering**: a new Subsurface output of the material (a colour). Light passes through foliage, skin, wax and snow: a soft transition to the shadow side and a glow against the light. The palm leaves in the Water template now glow in the sun
- **Colour grading through a LUT**: in the Post Process Volume, Color LUT (a 256 × 16 image in the Unreal format) and its strength. It blends by the volume's weight
- **Starter set**: brick and cobblestone textures (colour, height, normals), the materials M_Bricks and M_Cobblestone (with relief), M_Leaves (with subsurface), and six LUTs (Neutral, Warm, Cold, Cinematic, BlackWhite, Vintage, uncompressed; the import settings sit next to them). The demo level has a brick wall on cobblestones and a cinematic LUT. Everything is generated by `Tools/make_starter_textures.py`

## 0.10.11 — fire and smoke simulation (2026-10-14)
- **Fluid Volume component**: a real gas simulation on the GPU (the Navier–Stokes equations, the stable fluids method) in the entity's box. Each frame the velocity field is advected by itself. Hot gas rises (buoyancy), smoke pulls downwards, vortices are maintained (vorticity confinement), and the field is made incompressible (a pressure solve). The smoke density and the temperature are carried by the flow, dissipate and cool down. The floor is solid, and the sides and top are open
- Drawing is a ray march through the box. Smoke absorbs and scatters the sun and the sky (with self-shadowing). Hot gas glows, with its colour set by temperature (dark red → orange → yellow-white). The scene's depth is taken into account
- Everything is adjustable: the grid resolution, the source (position, radius, speed, how much smoke and heat, turbulence), buoyancy, the weight of the smoke, vortices, dissipation, cooling, the density and colour of the smoke, the brightness and the glow threshold of the fire. Simulate In Editor runs it live in the editor. Create → Fluid Fire / Fluid Smoke. Features → Fluid Simulation
- Starter set: P_FluidFire (with a light) and P_FluidSmoke
- The cost on Intel UHD: about 1 ms for a 48³ simulation and 2 ms to draw one volume. Up to 4 volumes at once

## 0.10.10 — volumetric clouds (2026-10-14)
- **Volumetric Clouds component**: a layer of real 3D clouds (a ray through tiling Perlin-Worley noise, with the edges eaten away by fine noise), drifting with the wind. Lighting: the sun with self-shadowing inside the cloud (bright tops, grey bottoms, a "silver lining" against the sun), and the sky from above. At sunset the clouds colour themselves, since the sun's light comes through the atmosphere. Settings: the coverage (from clear to overcast), the height and thickness of the layer, the density, the detail of the edges, the wind's speed and direction, the colour, and the shadow strength. Create → Volumetric Clouds; Features → Volumetric Clouds
- **Cloud shadows** move across the ground (on everything the sun lights) and over volumetric fog
- They are computed at a quarter resolution with accumulation over frames: about 3–4 ms on an integrated Intel UHD
- In the First Person, Third Person and Water templates, and in the Starter Content's demo level, there is now Height Fog (volumetric) and clouds. The template screenshots were retaken
- GPU debugging: when the device is lost, the log records the DRED operations on which the graphics card failed, and the location of the page error. The flag `-gpuvalidation` turns on the checking of resource states on the GPU

## 0.10.9 — volumetric fog and smoke, heat haze, particle collisions (2026-10-14)
- **Volumetric Fog**: the first 80 m of fog is a real volume (a grid of cells along the view, as in UE). The sun shines through it with shadows, producing **god rays** from behind walls, trees and windows. Point and spot lights glow in the fog as halos. **Fire lights the smoke and the fog around it.** Beyond that, the fog continues analytically, with no seam. The frames are smoothed over time. Features → Volumetric Fog
- **Height Fog component** (like Exponential Height Fog in UE): density, falloff with height (the height is taken from the entity), colour, forward scattering towards the sun, distance, a Volumetric checkbox, the depth of the volume, the density inside the volume, and the brightness of lamps in the fog. Before this, the fog could not be configured at all. Create → Height Fog
- **Fog Volume component**: a local volume of fog, steam or smoke (a cube or a sphere, set by the entity's scale), with a soft edge, drifting billowing noise, a colour (dark for smoke) and a glow. Create → Fog Volume
- **Volumetric smoke above fire**: burning objects (Flammable) release a billowing column of volumetric smoke. Its density follows the strength of the fire, and it is lit by the fire itself and by the sun
- **Particles are lit by the volume**: smoke from particles receives the same light as the fog (the sun's shadows, lamps, fire), and it sinks into the fog together with the scene
- **Heat haze**: a new particle type, Heat. It is invisible, and it bends whatever is behind it. The air shimmers above burning objects, the campfire and the fire of the starter set
- **Particle collisions**: Collision = Bounce (they bounce off, and sparks hop along the floor) or Die (they vanish on impact). On Hit Emitter is an emitter that throws out splashes at the point of impact. The rain of the starter set splashes on the ground and the roofs, and the sparks bounce. In Play the collisions use the physics; in the editor they use the meshes

## 0.10.8 — real sounds (2026-10-14)
- **A library of recorded sounds** in the starter set (119 files, all CC0, usable in any game): a crackling campfire, rain, wind, waves; footsteps on concrete, grass, wood, carpet, snow; hits on wood, planks, metal, stone, glass, soft objects; doors, coins, creaks, explosions, a force field, engines. The sources are Kenney and OpenGameArt (the authors are in `StarterContent/Sounds/CREDITS.txt`). The synthesized placeholders are gone. The loops are made seamless (`Tools/import_starter_sounds.py`)
- **Impact Sound component**: a physical body makes a sound when it hits something. It is louder on a hard hit, with a random pitch. A set of variants is picked at random. Crates, balls, cubes, the bridge's planks, hay bales, the lamp and the lift all have sounds
- **Sets of variants**: a path without an extension (`"StarterContent/Sounds/Footsteps/Footstep_Grass"`) plays a random one of `_0 .. _9`. This works in Impact Sound and in `audio.play` / `audio.playAt`
- Footsteps for the first- and third-person controllers (the Footsteps and Stride properties, and a landing sound). A door opens and closes with a sound, a coin rings, the trampoline hums, the tank has an engine, and a shot and an explosion play at the point of impact. Rain and the fountain make noise, and in the Water template there are recorded waves

## 0.10.7 — living fire: spread, charring, burning out (2026-10-14)
- **Flammable component**: an object can catch fire. In Play it heats up from fire nearby (from other burning objects, campfires and torches, that is, a Particle Emitter with the Flame look, and from scripts), and it ignites at its hottest point once it reaches its ignition temperature. Fire heats it more from above (heat rises). An object several metres away does not catch fire
- **The flame creeps across the surface** from the point of ignition at the Spread Speed. Each spot burns for Burn Time seconds and goes out, so a board burns through from one end to the other. The number of flame tongues, the smoke, the sparks and the brightness of the flickering light depend on the area burning right now, not on fixed values. When only embers remain, the smoke becomes thin and light. The crackling sound (Sound) gets louder or quieter with the fire
- **Charring in the shader**: the burnt part turns black with a ragged edge and soot in front of it. The edge of the flame glows, embers glow in the cracks of the charcoal and then cool. When the object has burnt out, it stays charred (Charred), or it settles into a heap of ash and disappears (Crumble). The joints of burnt-out objects break (Break Joints): the suspension bridge collapses. Under water (the Water component) the fire goes out
- Everything is adjustable: the ignition temperature, heat capacity, burn time and spread speed, the heat and its radius, the height, density and colour of the flame, the amount and colour of the smoke, the degree of charring, the light, the sound
- **ZeqouScript**: `entity.ignite([point])`, `entity.extinguish()`, `entity.burning`, `entity.burnt`, `entity.fire` (how much of the object is on fire, 0..1), `entity.temperature`. The global `fire.ignite(point[, radius])`, `fire.extinguish(point[, radius])`, `fire.burning`
- **Starter set**: the wooden crates, the door and the suspension bridge now burn. New props: a wooden fence, a hay bale (it catches quickly and crumbles to ash), and a woodpile. Tank shells set fire to whatever they hit. Characters get a FireStarter script: **F** sets fire to whatever you look at, **G** puts it out with water. In the demo level of the starter set, a burning hay bale sets the fence alight. The fire spreads along it to the woodpile and to a second bale
- **The flame looks better** (on Particle Emitter too): vertical tongues instead of rotating blobs, a hot yellow-white core and dark orange edges. The flame partly covers the background and does not fade out against a bright sky
- Features → Fire (the Gameplay tab)

## 0.10.6 — the starter asset set, particles, new templates (2026-10-14)
- **Starter Content as in UE**: when creating a project you can choose ready-made assets: **Effects**, **Characters & Vehicles**, **Gameplay Props** (prefabs in `Content/StarterContent/Prefabs`, all editable): fire, a campfire (with a crackling sound), a torch, smoke, sparks, steam, a creeping fog, a magic sphere, rain, a fountain, a pool with water. A lightweight third-person character with a camera, a first-person character, a **tank** (W/S/A/D; the mouse aims the turret and the barrel; a click fires: a flash, sparks at the point of impact, an impulse to the body). A door on a hinge (it opens on E), a lift on a slider, a trampoline, a coin (a collectable item), a stack of crates, a swinging lamp, a suspension bridge on joints. An empty project with the starter set gets a demo level with all of this
- **Particles**: the **Particle Emitter** component. A shape (point / sphere / box / cone), speed, acceleration (gravity or rising heat), drag, turbulence, size and colour over lifetime, a look (a soft cloud, a flame tongue, a spark, a spark stretched along its motion), blending (glow or smoke), and a flickering Point Light on the same entity. **Simulate In Editor** shows a real-time preview right in the editor (when off, it runs only in Play). Soft edges at geometry, sorting, one draw call. Features → Particles. ZeqouScript: `entity.emit(n)`, `entity.emitting`
- **The templates were redone**: First / Third Person have a prototype arena with grid materials (as in the UE templates), a ramp, a staircase to a balcony, platforms, physics crates and balls, and a mannequin for Third Person. Water has a smooth island with palm trees, a hut, a boat and a pier, with 8 waves instead of 4 (no repeating pattern), and the wave normals per pixel. Sample has a floor and a pedestal. **The template pictures** in the Project Browser are real screenshots from the editor (`Tools/make_template_thumbs.py`, flag `-screenshotviewport`)
- **Materials**: the **World Grid** nodes of the graph (a grid in world coordinates, with no UVs) and **Noise** (fractal noise). The starter materials `M_Proto_*` (grids) and the natural ones (grass, sand, stone, wood, concrete, earth) built on noise
- The distance fog takes the sky's colour at the horizon, so the grey band between the sea and the sky is gone
- Joints: several joints on one entity. Connected Body picks the nearest entity with that name (copies of a prefab connect each to itself)
- ZeqouScript: `entity.worldPosition` (read and write), `entity.worldRotation`, `entity.worldForward`, `entity.launch(speed)` to pop a character up or give a body a velocity

## 0.10.5 — physics, part 3: joints (2026-10-14)
- **Joints** (components on an entity with a Rigid Body; **Connected Body** is the name of another entity, empty = the world): **Hinge Joint** (doors, wheels; angle limits and a motor), **Fixed Joint** (glue), **Slider Joint** (pistons, lifts; limits and a motor), **Ball Joint** (chains, ragdolls; a swing cone and a twist limit), **Distance Joint** (a rigid rod, a rope with **Max Distance**, a spring with frequency and damping), **Configurable Joint** (each linear and angular axis Locked / Limited / Free)
- **Break Force / Break Torque**: a joint breaks if pulled harder than this. **Collide Connected**: whether the connected bodies collide with each other
- Solver: joints are rows of the same soft-step solver (soft correction at 60 Hz, warm start, limits like speculative contacts). The point and the rotation lock are solved by a 3×3 block, so a pendulum keeps its length and period (error < 1 %)
- In the editor, the selected entity shows its anchor, its axis, and a line to the connected body
- ZeqouScript: `entity.jointValue`, `entity.setMotor(speed[, force])`
- Tests: the pendulum period, a hinge motor and limits, the console, a slider, a chain and a cone, the spring frequency, a rope, configurable joints, sleep, breaking. Components in a scene (saving, a door on a motor, a lamp on a rope, welding)

## 0.10.4 — a character on physics (2026-10-13)
- **Character Controller** is now a capsule that moves through the physics world (a shape cast). It slides along walls, climbs steps up to **Step Height** and comes down them without hopping, walks on slopes up to **Max Slope** (a new field, 50°) and slides off steeper ones, rides moving platforms, and pushes Rigid Bodies by mass (a new field **Push Mass**, 80 kg: a light crate gets pushed, a heavy one stops it). On a step's edge, the support is found from the face under the contact point, not from the normal of the rounded bottom, so the character does not slide off steps
- The character moves before the physics step, and its capsule follows it at once (bodies fall onto it and bump into it)
- Without physics (Features → Physics switched off), the earlier simplified mode based on mesh boxes still works
- **Milestone M11 acceptance (part 2)**: stairs and slopes (the tests CharacterPhysics.*), raycasts filtered by layer from gameplay

## 0.10.3 — physics, part 2: triggers, events, queries, API (2026-10-13)
- **Triggers**: Box / Sphere / Capsule Colliders have an **Is Trigger** tick. The collider does not collide. It reports bodies entering and leaving (the character included)
- **Events**: the start and end of contact between two bodies (with the impact speed), and entering and leaving a trigger. Scripts receive `collisionEnter(other, speed)`, `collisionExit(other)`, `triggerEnter(other)`, `triggerExit(other)`. C++ components get `OnCollisionEnter/Exit`, `OnTriggerEnter/Exit`
- **Queries**: raycast (it skips triggers), shape cast (a sphere, box or capsule, with a conservative-advancement method), overlap. **Layers** (Layer 0–31) on colliders, with a filter in any query
- **ZeqouScript API**: `entity.velocity`, `entity.angularVelocity`, `entity.addForce / addImpulse / addTorque`, and the global `physics`: `raycast`, `sphereCast`, `overlapSphere`, `gravity`
- **C++ API** (Game SDK v2): `host->AddForce / AddImpulse / Get/SetVelocity / Raycast`. Modules built against the old SDK ask to be rebuilt (Compile, F7)
- Fast bodies do not pass through walls (speculative contacts with a margin for speed: 100 m/s against 10 cm, a test)
- Tests: events and triggers, overlap and shape cast, tunnelling, the script ↔ physics link

## 0.10.2 — ZPhys physics, part 1 (2026-10-13)
- **Our own physics engine** (the Physics module, no third-party libraries): shapes sphere, box, capsule, a static triangle mesh (BVH), compound bodies. Mass and inertia from the volume. Two dynamic AABB trees, layers with a collision matrix. Contacts by SAT with face clipping (up to 4 points), and exact distances for spheres and capsules. Speculative contacts (fast bodies do not fall through), with no "stumbling" on the edges of neighbouring triangles. A soft-step solver with substeps (as in Box2D v3). Friction, bounce. Islands fall asleep at rest and wake on contact. Determinism. Raycasts by layer. The broad and narrow phases run in parallel on the job system
- **Milestone M11 acceptance (part 1)**: a stack of 20 cubes stands for a minute. 500 bodies take 2.9 ms per step on the development laptop
- **Rigid Body** components (Dynamic / Kinematic, mass, friction, bounce, damping, gravity, sleep) and **Box / Sphere / Capsule / Mesh Collider**, as in Unity: a collider without a Rigid Body is static. Colliders of child entities join the parent's body, and scale is taken into account. Meshes with the **Collision** tick in their Mesh Renderer, and without colliders, automatically become static collision (old scenes and templates work at once)
- Play: a fixed 60 Hz step with interpolation of poses. Kinematic bodies follow their entity. Moving a body from a script teleports it. The Character Controller pushes bodies with its capsule
- Editor: Create > Cube / Sphere / Cylinder / Plane add a collider. Green collider wireframes on the selected entity. Features → Physics. Console: `physics.spawn [N]` (a heap of bodies in front of the camera), `physics.stats`, `action <id>` (any editor action, for example `action play.eject`)
- The self-test `-physicstest`. Tests Physics.* (rest, stacks, bounce, friction on a slope, a mesh floor, raycasts, layers, kinematics, determinism) and PhysicsSystem.*. The benchmark ManualPhysics.Pile500Bodies

## 0.10.1 — the progress window (2026-10-13)
- **A progress window**, as in Unity: a modal "Hold On" window shown while the user waits (importing models, opening a scene). It says what is being cooked ("Cooking Sponza.gltf: optimizing meshes, building LODs"), how many assets are ready, a progress bar, and the time. Background work (cooking textures, uploading to the GPU, compiling material shaders and the C++ module) is shown in a small panel in the bottom-right corner. Tasks shorter than 0.4 s do not open the window
- The AssetManager counts started and finished tasks, and reports what it is working on (`CurrentWork`)
- The flag `-screenshotnow` takes a screenshot in the `-autotest` frame even while loading

## 0.10.0 — M10 closed: EXR, BC6H/BC7, HDRI sky (2026-10-13)
- **OpenEXR**: our own decoder, scanline and tiled, with compression NONE / RLE / ZIPS / ZIP / PIZ / PXR24, channels half / float / uint, RGB(A), Y and layers (`beauty.R`). The result matches the OpenEXR library bit for bit on reference files. Our own EXR writer (half, ZIP). .exr files are imported as textures
- **BC6H** (HDR) and **BC7**: our own encoders and decoders. BC6H: all 14 modes. BC7: modes 6, 1 and 4/5 with channel rotation. The bit layouts were checked against the reference GPU decoder (WARP). HDR textures are cooked with mips in BC6H (4 times smaller than half-float). Textures with transparency use BC7 instead of BC3 (+3.9 dB at the same size). The format can be set in .zmeta: `"compression": "BC7"`
- **HDRI Sky component**: an HDR panorama (.hdr / .exr) replaces the physical sky. It provides the background at full resolution, the sky's lighting, the fog colour, the water, a reflection cube (128 px from the panorama itself), and probes. The sun is found in the panorama during cooking, and it lights the scene as a directional light with shadows (and is cut out of the sky lighting so it is not counted twice). Modes From HDRI / Scene Sun / None, a rotation, the brightness in nits. Create > HDRI Sky. Double-click a .hdr/.exr in the Content Browser, or drag it into the viewport. "Use as HDRI Sky" in the file menu. Console: `hdri.info`, `hdri.set`, `create hdrisky <panorama>`
- **Content Browser**: previews of HDR pictures with exposure and tonemapping. The "HDR Image" type. .tga files also show as textures
- Console: `camera x y z [yaw] [pitch]`
- Fixed: a damaged zlib stream could make the DEFLATE decoder loop (this affected PNG). It now fails cleanly
- `Tools/fetch_samples.ps1` also downloads a sample Poly Haven panorama (CC0). `Tools/gen_exr_testdata.py` makes the EXR references
- The self-test `-hdritest`. Tests Exr.*, TextureTools.Bc7*, Bc6h*, HdrMipChain, RHI.BlockCompressionMatchesHardwareDecoder

## 0.9.4 — decals, ZTSR, dynamic resolution (2026-10-13)
- **Decal component**: it projects a picture (alpha = coverage) and, optionally, a normal map, onto everything inside a box along the entity's −Y axis (by default, onto the ground under it). It changes colour, roughness and the normal, and can glow. Fall-off by angle and depth, and the stacking order. Decals are applied in the forward shader before lighting, so they are lit like the surface itself and are visible in the reflections of probes. Create > Decal places a decal on the ground. The box with an arrow shows the projection in the viewport. A texture can be dragged from the Content Browser into the field in Details
- **ZTSR**, a temporal upscale. When the render resolution is below 100 %, TAA accumulates its history at the output resolution (reconstruction from the jitter, Gaussian weights, history clipping). That is noticeably sharper than a bilinear stretch. Post-processing (DOF, motion blur, bloom) runs at the output resolution. `r.TSR`, Show > Temporal Upscaling
- **Dynamic resolution**: the render scale follows the GPU frame time towards a target (steps of 5 %, four times a second, with no reset of the ZTSR history). `r.DynamicResolution`, `r.DynamicResolution.TargetMs`, `.MinPercentage`, Show > Dynamic Resolution. The scale and "ZTSR" are shown in the viewport statistics
- The Outliner shows an entity's component icon (camera, sound, decal, probe…), not a generic icon
- The self-test `-decaltest`. Test DynamicResolution.*

## 0.9.3 — reflection probes and SSR (2026-10-13)
- **Sky reflections**: the sky-view LUT is moved into a cube map and pre-filtered with GGX into mips (roughness = mip). The specular environment now comes from the cube, not from SH. Metals and glossy surfaces reflect the real sky
- **Reflection Probe component**: a cube map of the scene around a point (64 px on Lite, 128 on Full). Capture one face per frame, with a separate scene renderer. Automatic recapture when the scene, the sun or the sky changes, and when meshes, textures or material shaders finish loading. Box projection (the reflections of floors and walls line up with the room), a blend at the edge of the box, and priority. Create > Reflection Probe. The box is shown in the viewport. Console: `reflections.recapture`, `reflections.stats`
- **SSR**: reflections traced through the depth buffer at half resolution, after the opaque pass. (The normal and roughness go into a second render target through a shader variant.) They are used by the next frame, on top of the probes. On by default in Full: `r.SSR` / Show > Screen Space Reflections
- Features: Reflections, Screen Space Reflections. Probes in the templates (the First / Third Person platform, the Water hut)
- The self-test `-reflectiontest` (a mirror floor, glowing figures, a probe's capture and recapture, SSR under the debug layer)

## 0.9.2 — Post Process Volumes, depth of field, motion blur (2026-10-13)
- **Post Process Volume component**: a box (or Unbound, the whole world), with a radius, a blend weight and a priority. It overrides exposure, bloom, contrast, saturation, white balance, the colour filter, vignette, DOF and motion blur. Volumes blend as in UE: by priority, and the weight fades smoothly outside the box. Switches change when the weight is ≥ 0.5. Create > Post Process Volume. The icon and the box outline (with the blend zone) are shown in the viewport
- **Depth of field**: a physical camera model (a 24 mm sensor, the focal length from the field of view, the aperture f/N, the focus distance). Bokeh at half resolution (a golden-angle spiral, scatter-as-gather). The foreground blurs over the background. The viewport mode **Depth of Field Focus** (near = red, far = blue, in focus = green)
- **Camera motion blur**: the pixel velocity comes from reprojecting the depth. The shutter length and the limit in pixels. No blur at camera cuts. In the editor, only during Play
- A colour filter in the grading. New Features: Post Process Volumes, Depth of Field, Motion Blur. Ticks in the Show menu
- Details: sections by field category, and override ticks before the label (EditCondition, as in UE). Long labels are cut with "…"
- The self-test `-posttest` (volumes blend as the camera moves, DOF and motion blur under the debug layer). The flag `-uiscale=`. MeshViewer: `-dof=<focus>,<f-number>`, `-motionblur=<fraction>`

## 0.9.1 — C++ game module, hot reload, Input Actions (2026-10-13)
- **C++ gameplay**: the project's game module (`Source/<Name>/`) is built into `Binaries/<Name>.dll` and loaded by the editor. Components are subclasses of `ScriptComponent` from the single-file Game SDK (`Engine/Source/GameSDK/ZeqouGame.h`), registered with the macros `ZX_GAME_COMPONENT` / `ZX_GAME_PROP` (float, int, bool, Vec3, with a range and a hint)
- **Hot reload in PIE**: the Compile button (the hammer by Play, F7) builds the module in the background (`Tools/build_game.ps1`: MSVC + CMake + Ninja. The vcvars environment is cached, so a rebuild takes about 1 s). After the build, the running components are recreated on the new code with the same props and get `OnReloaded()`. The game does not stop. Compiler errors go to the Output Log
- The **C++ Script** component (NativeScript): choose the class and the props in Details (only the differences from the defaults are saved in the scene). A message about an unbuilt module, with Create / Compile buttons
- The host API for C++: entities (Find, spawning shapes, Destroy, position / rotation / scale), input (keys, Input Actions, the mouse), time, the Character Controller, sound, the log
- **Input Actions**: named buttons and axes (Jump, Sprint, Crouch, Interact, Fire, Aim, Pause; MoveForward, MoveRight, LookX, LookY), with bindings to the keyboard, mouse and gamepad. The window Window > Input Actions. The file `Config/Input.json`. ZeqouScript: `input.action / actionPressed / actionReleased / axis`
- Project Browser: the **C++** language is available when creating a project. It writes `Source/<Name>` with the examples Spinner, Bouncer and PlayerController, and builds it straight away. Tools > Create C++ Game Module / Open Game Source Folder for existing projects
- Project documentation: `Docs/CppGameCode.md`. Input Actions in the README / Getting Started / AGENTS.md
- The self-test `-cpptest` (with `-newproject=... -language=cpp`): building the module, Spinner in PIE, editing the source, rebuilding and hot reloading during play, checking the new behaviour and the saved prop, and Stop restores the scene
- Fixed: a memory leak when the editor was closed in the middle of `-audiotest`

## 0.9.0 — M9: sound (2026-10-13)
- Our own sound engine: a 48 kHz mixer on its own thread, WASAPI output, buses Master / Music / SFX / UI / Voice with volume, mute and meters, ducking, snapshots, reverb, a limiter. DSP: filters, Freeverb, delay, compressor
- Our own decoders: WAV (PCM, float, IMA / MS ADPCM) and **Ogg Vorbis** (matches the libvorbis reference). Streamed music
- 3D sound: attenuation, panning, cones, Doppler, occlusion by raycast. 64+ voices with virtualization, without clicks
- The components **Audio Source**, **Audio Listener**, **Reverb Zone**. Sound plays in Play-in-Editor (pause mutes it)
- Editor: previews of sounds in the Content Browser, a clip slot in Details, the **Audio Mixer** window, icons and radii in the viewport
- ZeqouScript: `audio.play(...)`, `audio.playAt(...)`, `audio.setVolume(...)`, `entity.playSound()`
- Starter Content: synthesized sounds. In the templates: a hum from the cube (3D), the sea and wind on the island, reverb in the hut
- The self-test `-audiotest` (64 sources on a real device). The flag `-noaudio`

## 0.8.6 — tools, Features, project documentation (2026-10-13)
- The **Features** window (Tools > Features): switching shadows, AO, the sky and fog, bloom, TAA, water, ZeqouScript, character physics, and the Brush / Measure tools on and off. Stored in the .zproj (`disabledFeatures`)
- **Brush**: painting models onto surfaces. Several models with weights (Add Selected), radius, density, spacing, scale variation, random rotation and tilt, alignment to the normal, a maximum slope, offset. Shift erases. `[` and `]` change the radius. One stroke is one undo step. The instances are collected into "Brush Painted"
- **Measure** (M): the distance between two points, with projections on the axes. **Drop to Ground** (End)
- Raycasting against the triangles of meshes (`Engine/Raycast.h`, LOD0 on the CPU). Selecting by clicking in the viewport is now exact
- New Project: the **Documentation** options (README.md, Docs/, AGENTS.md and CLAUDE.md for AI assistants) and a **Git repository** (git init, .gitattributes, a first commit). The flags `-nodocs`, `-git`
- Project Browser: one icon (in the side bar), without a duplicate in the title
- The self-test `-tooltest`. Console: `tool brush|measure|none|drop`, `features`, `create water`

## 0.8.5 — realistic water, the Water template (2026-10-12)
- **Water component** (Create > Create Water): a mesh in the vertex shader with 4 Gerstner waves, the physical sky reflected with Fresnel, sun glints (GGX), screen-space refraction, absorption and scattering by the water's thickness, foam at the shores and on the crests, procedural ripples. The water writes depth (fog and TAA work over it)
- The **Water** template: an island with a beach, a meadow, rocks, a hut and a pier. A sea of 300 × 300 m, a first-person player, a low sun with a path of light on the water. The starter materials M_Sand and M_Rock
- The default tonemapper is ACES (AgX gave a grey, washed-out sky and water)
- The cost on UHD 620: about 1.3 ms for the scene copy and about 1.5 ms for the water (1600 × 900, Lite)

## 0.8.4 — character, camera, First / Third Person templates (2026-10-12)
- A new application icon based on the user's artwork (the sign and the logo with the lettering): `Tools/import_brand_icons.py` → `ZeqouX3D.ico` (16–256), `Branding/ZeqouX3D_Icon.png`, `Branding/ZeqouX3D_Logo.png`. It is used in the window title, the Project Browser and About
- The **Camera** and **Character Controller** components (walking with sliding along walls, steps, gravity, jumping, a reset when falling). The Mesh Renderer has a **Collision** tick
- Play-in-Editor: the view from the game camera, input capture (a click captures, Shift+F1 releases), F8 ejects
- ZeqouScript: the `character` object (`move`, `jump`, `grounded`, `velocity`), `entity.child(name)`, `entity.parent`
- The **First Person** and **Third Person** templates: a polygon arena (stairs, platforms, crates, walls) and ready scripts FirstPersonController / ThirdPersonController / ThirdPersonCamera (also in the Starter Content)

## 0.8.3 — Project Browser (2026-10-12)
- Project tiles: the first letter on a random colour. The colour is stored in the .zproj (`"color"`), and the language as `"language"`
- The New Project page in the UE5 style: a grid of templates with drawn previews (Blank, Sample Scene, First Person, Third Person, Water. The last three are marked "COMING IN 0.8.4/0.8.5"), and the project settings on the right
- Language choice: ZeqouScript (C++ was M10, Visual Script M14, and they were not yet available)
- **Starter Content**: 10 PBR materials with editable parameters, and 5 scripts (Rotator, Bobbing, SimpleMover, LookAt, Blinker). In an empty project, a showcase with the scripts. The flag `-starter` for `-newproject`
- Fixed: the sign of the exposure compensation (now "+" means brighter, as in UE and Unity). By default +1 EV, so day scenes no longer look dark

## 0.8.2 — ZeqouScript (2026-10-12)
- A new language for the engine, **ZeqouScript** (`.zs`): a compiler to bytecode and a stack VM (the Script module). `prop` fields with `@range / @tooltip / @color`, lists, vec3 / quat, a standard library. The reference is `docs/ZeqouScript.md`
- The **Script** component: the script's properties in Details (overrides in bold, Reset to Default), running `start()` / `update(dt)` in Play-in-Editor, and the objects `entity`, `input`, `time`, `find()`
- Hot reload during Play, keeping the state. Errors with the file and the line. Only the one instance that fails stops
- **Script Editor**: syntax highlighting, line numbers, error checking while typing, auto-indent, Tab / Shift+Tab, Ctrl+S
- Content Browser: New Script, the `.zs` icon, a double-click opens the editor, Add to Selected Entity
- ZUI: the `InputTextCode` widget, `PushFont(FontHandle)`. The monospaced Consolas font in the code editor
- The editor's self-test `-scripttest`

## 0.8.1 (2026-10-12)
- The engine version is the closed milestones (0.8.x), with a single source of the version, a version resource and an icon in ZXEditor.exe
- The application icon in the style of the Zeqou ecosystem (`Tools/gen_app_icon.py`), and a new logo in the editor

## 0.8.0 — M8: prefabs, Play-in-Editor, graph materials (2026-10-12)
- Play-in-Editor: Play / Pause / Step / Stop. The world is restored after Stop. The components Rotator and Oscillator
- Prefabs `.zprefab`: instances with overrides, nesting, Apply / Revert / Unpack
- Material Graph `.zmat` (34 node types → HLSL), a node editor, instances `.zmati`, asynchronous compilation (< 1 s)
- Built-in shapes (Cube / Sphere / Cylinder / Plane), a command palette (Ctrl+P), hotkeys in Keys.ini, SVG icons

## 0.7.0 — M7: editor v1
- A shell in the style of UE5 and Unity: our own title bar, a toolbar, docking of panels, a Viewport, an Outliner, Details, the Content Browser, and an Output Log with a console
- Scenes `.zscene`. Undo / Redo for all operations. Projects `.zproj` and the Project Browser. The selection outline. Asset thumbnails

## 0.6.0 — M6: ZUI
- Our own UI: SDF primitives, TTF / WOFF and a rasterizer, an atlas, Inter and Lucide, all the basic widgets, DPI 100–150 %

## 0.5.0 — M5: lighting and shadows
- Clustered Forward+, physical units and exposure, CSM plus an atlas of point and spot shadows, GTAO, a physical sky, fog, tonemapping, FXAA / TAA

## 0.4.0 — M4: meshes, materials, glTF, ECS
- Import of glTF / GLB / OBJ, mesh optimization and LODs, ZECS, hierarchy, PBR materials, an asset system (GUID, DDC, async)

## 0.3.0 — M3: render graph and textures
- Render graph (transient resources, aliasing, barriers), PNG / JPEG / TGA / DDS, mips, BC compression, debug draw, a fly camera

## 0.2.0 — M2: platform and D3D12
- A window, input, the main loop. The RHI and the D3D12 backend (bindless, PSO, fence, timestamps), shader compilation with DXC

## 0.1.0 — M0–M1: foundation and Core
- CMake build, modules. Core: memory, containers, strings, maths, threads and the JobSystem, the log and CVars, ZJSON, serialization, reflection, the VFS, the profiler, deflate
