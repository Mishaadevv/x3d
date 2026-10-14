# ZeqouX3D — notes for Claude

3D engine + editor written from scratch (target: Unity URP / UE5-style workflow). User speaks Russian — reply in Russian.

**Master spec: `docs/TZ.md`.** Read the relevant section before any work. Work only within the current milestone (section 11),
tick checkboxes in section 14, log architecture decisions as ADRs in section 15.

## Key facts
- C++20, MSVC 14.44 (VS 2022 BuildTools), CMake + Ninja (bundled with BuildTools), D3D12 + HLSL via DXC (Windows SDK 10.0.26100).
- Zero third-party libraries — only OS / Windows SDK. No exceptions, no RTTI, own containers.
- Namespace `zx`, macros `ZX_*`, members `m_camelCase`, functions `PascalCase`. Code comments in English, docs in Russian.
- Dev machine = minimum target: i5-10310U, 8 GB RAM, Intel UHD 620 (no mesh shaders, no HW RT). Always check RHI caps.
- Left-handed, Y-up, +Z forward, meters, reversed-Z.

## Build
`powershell -ExecutionPolicy Bypass -File Tools/build.ps1 -Config Development [-Test] [-All] [-Target X]` (sets up vcvars itself;
pwsh is not installed). Run long builds in the background. Shipping (LTCG) links slowly - use Development for iteration.
Tests: `Binaries/Win64/<Config>/ZXTests.exe [filter]`; suites named `Manual*` (benchmarks, dumps) run only when filtered explicitly.
Editor smoke test: `Binaries/Win64/Debug/ZXEditor.exe -autotest=60` (exit code 0 = no errors, no leaks; log in Saved/Logs).
Editor flags: -scene=<gltf|zscene>, -resolution=WxH, -resetlayout, -screenshot=<path> (with -autotest), -savescene=<path>
(saves when the autotest ends), -undotest (runs every undoable edit through the editor; exit code 4 on failure).
Projects: no args = Project Browser; -autotest/-scene without -project use Samples/Samples.zproj (Sponza). -project=<zproj>,
-newproject=<dir>/<Name> [-template=sample|firstperson|thirdperson|water] [-starter] [-nodocs] [-git], -projectbrowser [-browserpage=new], -recentfile=<json> (keep tests out of the
user's real recent list in %LOCALAPPDATA%/ZeqouX3D).
-exec="cmd; cmd" runs console commands once loaded (help, stat.gpu, stat.scene, import <path>, create <type>, undo, r.*);
paths inside -exec must not contain spaces (the command-line parser splits quoted values). -browse=<dir under Content>.
Self-tests (exit code 4 on failure): -undotest, -pietest (Play-in-Editor restores the scene), -materialtest (graph material
with texture + params, structural edit < 1 s, value edit without recompiling). -scripttest (Script component: props, start/update in PIE,
hot reload keeps state, Stop restores; writes Content/ScriptSelfTest.zs, deleted on exit), -tooltest (triangle picking,
Drop to Ground, Brush paint/undo/redo/erase, Features toggles). Tools: EditorTools.cpp (Brush/Measure/Features), Engine/Raycast.h.
-audiotest (64 looping 3D sources on the real device: 0 underruns, Stop silences), -noaudio (null audio backend).
-cpptest with `-newproject=<engine>/Saved/TestProjects/CppTest -language=cpp -nodocs -autotest=30`: builds the C++ game
module, runs Spinner in PIE, edits the source, rebuilds + hot-reloads while playing (the TestProjects dir is recreated).
C++ gameplay: Engine/Source/GameSDK/ZeqouGame.h (header-only SDK, Host interface, ZX_GAME_COMPONENT/ZX_GAME_PROP),
Engine/GameModule.h (NativeScript = "C++ Script" component, GameModuleSystem: build via Tools/build_game.ps1, copy-load,
hot reload carrying props), editor EditorGameCode.cpp (Compile F7, Input Actions window, -cpptest); tests ZXTests
GameModule.*. Input Actions: Engine/InputActions.h (Config/Input.json; tests InputActions.*).
Post: Render/PostProcess.h (DOF, motion blur, bloom, tonemap; Shaders/Post/DepthOfField.hlsl), Engine/PostProcessVolume.h
(blending; tests PostProcessVolume.*). -posttest (volumes blend as the camera moves, DOF/motion blur render),
-uiscale=<x> (multiplies the UI scale; tall panels in screenshots). MeshViewer: -dof=<focus m>,<f-stop>, -motionblur=<amount>.
Reflections: Render/Reflections.h (sky cube, probes, SSR; Shaders/Lighting/Reflections.hlsl), ReflectionProbe component
(Engine/SceneRendering.h), -reflectiontest, r.SSR / r.ReflectionProbes, reflections.recapture / reflections.stats.
Decals: Decal component (Engine/SceneRendering.h, GatherDecals; ApplyDecals in ForwardPBR.hlsl), -decaltest.
ZTSR: TemporalAA UpscaleCS (history at output size when renderScale < 1), r.TSR; dynamic resolution
(NextDynamicResolutionScale, r.DynamicResolution*).
Images: Core/Image (ExrReader.cpp = OpenEXR decoder + exr::Encode, tests Exr.* against Tests/Data/Images/exr_* from
Tools/gen_exr_testdata.py; TextureBC67.cpp = BC6H/BC7 encoders+decoders, bit layouts verified on WARP by
RHI.BlockCompressionMatchesHardwareDecoder). Cooker: HDR -> box mips + BC6H + HdrInfo (AnalyzeHdrEnvironment: average,
sun), alpha textures -> BC7; .zmeta "compression". HDRI Sky component (Engine/SceneRendering.h, ViewportRenderer::
ResolveHdriSky, Sky.hlsl SkyViewFromHdriCS, Shaders/Sky/Hdri.hlsli), -hdritest, console hdri.info / hdri.set,
create hdrisky <panorama>, camera x y z yaw pitch (pitch < 0 looks up). ManualExr.* benchmark: ZX_EXR_FILE=<path>.
Physics (M11): Runtime/Physics (PhysicsWorld.h API; Private/AabbTree, Collide.cpp = SAT/clipping + rounded shapes +
mesh BVH, PhysicsWorld.cpp = broadphase, contacts, soft-step solver, islands/sleep); tests ZXTests Physics.*,
ManualPhysics.Pile500Bodies (benchmark). Scene layer: Engine/PhysicsSystem.h (RigidBody, Box/Sphere/Capsule/Mesh
Collider, implicit mesh collision of MeshRenderer.collision, PIE ticking with interpolation; tests PhysicsSystem.*),
-physicstest, console physics.spawn / physics.stats, action <id> runs any editor action (e.g. play.eject).
Characters on physics: Gameplay.cpp TickCharacters(world, dt, PhysicsSystem&) (capsule shape casts, step up/down,
supportNormal under the contact for step edges, platforms, mass-based pushes); tests CharacterPhysics.*. Script/C++
physics API + collision callbacks: Scripting.cpp (physics global, DispatchPhysicsEvents), GameModule.cpp (SDK v2).
Particles: Engine/Particles.h (Particle Emitter component, ParticleSystem CPU simulation owned by the editor, ticked live
for Simulate In Editor emitters), Render/Particles.h + Shaders/Particles/Particles.hlsl (64-byte GpuParticle, premultiplied,
soft depth fade). Starter Content: StarterContent.cpp (materials incl. WorldGrid/Noise patterns, scripts, sounds) +
ZXEditor/StarterAssets.cpp (prefabs built in a scratch world: Effects / Characters / Props), showcase level in
BuildStarterShowcase. Template thumbnails: Engine/Content/Templates/*.png from `python Tools/make_template_thumbs.py`
(captures with -screenshotviewport = viewport only, no grid/gizmos/toolbar; loaded with srgb=false).
Fire: Engine/Fire.h (Flammable component with runtime state fields; FireSystem ticked by the editor in Play before the
particles: heat flux between surface samples, burn band between inner/outer radii, emits through ParticleSystem::SpawnFree
and SetExtraLights). Char is drawn by ForwardPBR ApplyBurn from instance row 7 (burn sphere) + normal rows' w.
Volumetric fog: Render/VolumetricFog.h + Shaders/Volumetric/VolumetricFog.hlsl (inject / integrate / apply; froxel
t = sqrt(viewDepth / distance)); lights + cascades come from SceneRenderer::GetLightingInputs(), sky light from the sky SH.
HeightFog / FogVolume components in Engine/SceneRendering.h; ParticleSystem/FireSystem add extra fog volumes. Particles
sample the lighting + integrated volumes; Heat look = distortion pass (ParticleRenderer::AddPass returns the new color).
Progress window: EditorProgress.cpp (modal "Hold On" while waitForAssets/pendingImports, background panel otherwise;
AssetManager jobsStarted/jobsFinished/CurrentWork). -screenshotnow captures at the autotest frame while loading.
-materialtest's 1 s structural-edit limit can flake in Debug when the machine is busy (rerun before suspecting code).
ForwardPBR PSMain writes normal+roughness only with ZX_NORMAL_OUTPUT (MRT pipeline variants; graph materials compile both).
Reflection meta used by Details: Meta::Category (section headers), Meta::EditCondition("boolField") (override checkbox).
Audio: Runtime/Audio (AudioEngine.cpp mixer, Wasapi.cpp, Wav.cpp, Vorbis.cpp, Dsp.cpp; tests ZXTests Audio.*; Vorbis
references Tests/Data/Audio from Tools/gen_audio_testdata.py), Engine/AudioSystem.h (components, PIE), EditorAudio.cpp.
ZeqouScript (own language, docs/ZeqouScript.md): Runtime/Script (Compiler.cpp = lexer/parser/codegen, VM.cpp = values,
VM, stdlib; tests ZXTests Script.*), engine bindings Engine/Scripting.h (Script component, ScriptSystem; tests Scripting.*),
editor ScriptEditor.cpp (ui::InputTextCode). Version: CMakeLists project(VERSION) + docs/CHANGELOG.md; state it in reports. Useful console commands: create <shape>,
move x y z, material.create/assign/set/edit/code/instance, prefab.create/place/apply/revert, play/stop, palette <query>.
Shortcuts live in Saved/Config/Keys.ini (EditorActions.cpp registry). Material graphs: Render/MaterialGraph.h (codegen),
Shaders/Material/MaterialGraph.hlsli, Engine/Materials.h; generated code: `material.code <name>`.
Layout: Saved/Config/EditorLayout.ini. Scenes/undo: Engine/SceneSerialization.h + SceneUndo.h (tests: ZXTests SceneSerialization.*).
All user-facing text (engine, editor, samples) is English; chat with the user stays Russian.
Sample content (Sponza etc., not in git): `powershell -ExecutionPolicy Bypass -File Tools/fetch_samples.ps1` -> Samples/Content.
MeshViewer: `Binaries/Win64/Development/MeshViewer.exe -autotest=300 -resolution=1600x900 [-screenshot=/Saved/Screenshots/x.png]`
(cooked data cache: Intermediate/DDC; bump asset::CookerVersion when a cooked format or cooker output changes).
MeshViewer flags: -night, -sun=<elev deg>, -sunaz=<deg>, -cam=x,y,z,yawDeg,pitchDeg, -flythrough, -debugview=N, -postview=N,
-noshadows, -nolocalshadows, -nosky, -nofog, -noao, -notaa, -nolights.
MeshViewer has a ZUI overlay (F1, -noui): stats, GPU timings per pass, render toggles.
UIDemo (all ZUI widgets): `UIDemo.exe -autotest=80 -scale=0.8 -screenshot=/Saved/Screenshots/ui.png [-scenario=menu|combo|color|modal|tooltip] [-light]`
(-scale multiplies the system DPI scale: the dev laptop runs at 125 %, so -scale=0.8 = 100 %).
UI headless interaction tests: `ZXTests.exe UI.*` (synthetic mouse/keys drive real widgets).

## Gotchas learned
- Never do slice-based string replacement in docs without asserting the slice is non-empty (an empty `old` destroyed TZ.md once).
- Python heredocs mangle backslash escapes (ESC, newline) in replacement text - use the Edit tool for C++ string literals.
- A ZX_EXPECT_ASSERT statement continues after the assert; never use it on code that is UB afterwards.
- iGPU timing: measure with vsync on. With -novsync the CPU and GPU share the 15 W budget, so lighter GPU work can
  even look slower; per-pass GPU timers come from the render graph (names interned via zx::Name).
- If whole-frame times suddenly jump to ~130 ms while GPU timers stay low, the screen is locked/occluded and DWM throttles
  Present - judge by the per-pass GPU timers then.
- Bash heredocs choke on some Python patch scripts: write the script to the scratchpad and run it instead.
- Git Bash rewrites exe arguments: `/Saved/x.png` becomes `C:/Program Files/Git/Saved/x.png` and `$(pwd)` is `/c/...`
  (the editor then creates `C:/c/...`). Use `MSYS_NO_PATHCONV=1` and `$(cygpath -m "$(pwd)")` for path arguments.
- The editor's command line splits at spaces: pass project paths in 8.3 form (`cygpath -d`), or -newproject fails
  silently and the test runs on the default project.
- Commas inside template arguments break test macros: `ZX_EXPECT_EQ((w.Query<A, B>().Count()), 1u)`.
- ZUI GPU cost is fill-bound on UHD 620 (~0.5-1 Gpix/s blended): keep overlapping layers few; border-only rects and
  shadows are drawn hollow automatically. Lucide icons: macros in UI/Icons.h (regenerate with Tools/gen_lucide_icons.py).

## Publishing
- GitHub: https://github.com/Mishaadevv/x3d (public, branch main, PolyForm Strict). Screenshots for the README live in docs/images.
- Website: the sibling repo ../Zeqou (https://mishaadevv.github.io/zeqou/, page /apps/x3d, docs /docs/x3d) reads the engine
  version from this CMakeLists.txt: after a version bump run `npm run versions` there, commit and push (GitHub Pages deploys).

## Rules
- Definition of done: builds warning-free (Debug + Development), tests pass, no leaks, no D3D12 debug-layer errors.
- Don't delete the source zips (`inter.zip`, `lucide-*.zip`) without asking. Commit only when asked.
