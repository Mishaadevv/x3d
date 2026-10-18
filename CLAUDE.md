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
-newproject=<dir>/<Name> [-template=sample|firstperson|thirdperson|water|physics|vehicle|empty|2017|2011|2008] [-starter] [-nodocs] [-git], -projectbrowser [-browserpage=new], -recentfile=<json> (keep tests out of the
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
Graphics eras (0.13.12): ProjectDescriptor::graphicsEra -> ViewportRenderer::graphicsEra; ApplyGraphicsEra
(Engine/ViewportRenderer.h) limits a copy of the settings / features for each frame (Modern = untouched); Game.json
graphicsEra for ZXGame; Features window "Graphics", console graphics.era; Tonemapper::Filmic (Hable).
Particles (0.13.11): ParticleEmitter midColor / sizeCurve / alphaCurve / burstInterval / startDelay / orbit / Ring /
lightFlash / stretch (GpuParticle flags bits 8-15 = stretch in ms); effect prefabs in StarterAssets.cpp (Flames /
Smoke / Embers / Sparks helpers, P_Explosion, P_Magic portal, P_Snow).
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
Clouds (v2, 0.13.10): Render/Clouds.h + Shaders/Sky/Clouds.hlsl (weather map + typed height profiles + Perlin-Worley /
Worley noise generated on the GPU in slices with mips; spherical shell; multiple-scattering octaves; checkerboard 2x2
raymarch at 1/4 (Lite) or 1/2 res + temporal variance clipping; depth-aware composite). VolumetricClouds presets
(CloudPreset, ApplyCloudPreset / UpdateCloudPresets), console clouds.preset / sun. GPU timing: scratchpad gpuavg.py. Cloud shadow map: written by AddShadowPass at the very end of the frame, read next
frame through SceneLighting::cloudShadow (forward + volumetric fog). VolumetricClouds component (Engine/SceneRendering.h).
GPU faults: DRED ops/allocations are logged on device loss; -gpuvalidation enables GPU-based validation.
Fluids: Render/Fluid.h + Shaders/Volumetric/Fluid.hlsl (per-volume grids keyed by entity, velocity always in [0],
scalars ping-pong; ~25 compute passes per step; FluidDraw raymarch after the volumetric fog). FluidVolume component
(Engine/SceneRendering.h); ViewportEnvironment::playing decides which volumes simulate.
Materials: graph output pin 8 = Subsurface (SurfaceData::subsurface, SubsurfaceLight in BRDF.hlsli); ParallaxOcclusion node
(MaterialGraph.hlsli, needs MaterialInputs::tangent). Color LUTs: PostProcessVolume::colorLut -> FindColorLut ->
PostSettings::colorLut (applied after tonemapping). LUT PNGs must not be block-compressed (ship a .zmeta with compression None).
Flipbooks: ParticleEmitter::flipbook (Content path, resolved by ViewportRenderer through decalTextures); GpuParticle
flags / texture / packed shape (shape | cols << 8 | rows << 16), seed = animation position. Atlases baked by
Tools/bake_flipbooks.py into Engine/Content/StarterContent/Flipbooks (copied into new projects).
Velocity: RenderInstance::id/prevWorld (ViewportRenderer pairs instances by entity), instance stride 192 (prev world
rows at +128), SceneRenderer Velocity pass (Shaders/Forward/Velocity.hlsl, RG16F, sentinel -1000 = use the camera) only
for moved instances; read by MotionBlurCS and TAA. -exec chains accept "wait N".
Convex hulls: Physics/Private/ConvexHull.h (quickhull, <= 32 vertices, GJK), ShapeType::ConvexHull (PhysicsWorld::
CreateConvexHull); Collide.cpp Poly = box / hull / triangle (SAT + clipping), PolyRounded for spheres/capsules.
MeshCollider::convex -> PhysicsSystem::HullOf (cached per mesh + scale); bodies whose convex mesh is loading wait in
m_pendingBodies. Tests ZXTests Hull.*; -physicstest drops a convex cylinder.
Height fields: Collide.h HeightField (Query by cells, Raycast = 2D DDA, SolidBelow -> BelowSurface pushes sunk
shapes up), PhysicsWorld::CreateHeightField/UpdateHeightField; tests HeightField.*. Terrain: Engine/Terrain.h (component,
GenerateTerrainHeights, TerrainSystem ticked by the editor: CookTerrainMesh -> AddMemoryMesh(TerrainMeshId(guid)) +
RenderAssets::ReloadMesh, keeps the entity's MeshRenderer in sync); PhysicsSystem adds a HeightField; tests Terrain.*.
M_Terrain in StarterContent.cpp (MakeTerrainMaterial). Console: create terrain.
Terrain editing (0.13.14): Terrain::hills / sculpt / paint / dataId (base64 i16 2 cm offsets, RGBA8 weights rock/dirt/
sand/grass since 0.13.16, legacy RGB decoded; Hidden);
ComputeTerrainHeights (physics), TerrainSystem::BeginEdit / MarkDirty / Commit / HeightAt / Raycast; mesh vertex color =
inverted paint weights (M_Terrain OneMinus). Editor EditorTool::Terrain + TerrainDab (EditorTools.cpp), console
terrain.sculpt. File > Settings: EditorSettings.h/.cpp (reflected prefs, PropertyEditor onlyCategory), Editor::prefs;
scale gizmo one-sided (prefs.scaleFromOneSide, LocalBounds in ViewportPanel.cpp).
Terrain brush v2 (0.13.16): TerrainBrushMode (IsTerrainPaintMode), TerrainRamp / TerrainOperation / Import|Export
TerrainHeightmap in EditorTools.cpp; Editor::CreateTerrain(flat); console terrain.ramp / terrain.op.
Release (0.13.16): `powershell -ExecutionPolicy Bypass -File Tools/make_release.ps1 [-SkipBuild]` -> Build/Release
(staged install folder, <ver>-Beta-Setup.exe, Portable.zip). Installer = Programs/ZXSetup (SetupMain.cpp: payload =
.zpak "Files/..." appended to the exe + 24-byte trailer, PakArchive::Open(path, baseOffset); window on ZUI with fonts /
shaders from the payload and dxcompiler unpacked to %TEMP%/ZeqouX3D-Setup/bin; SetupWin32.cpp = HKCU uninstall entry,
shortcuts). Flags: -silent -dir= [-nopacks] [-noshortcuts], -makeinstaller=<stage> -out=<exe> (8.3 paths!),
-autotest -page=N / -autoinstall -dir=; Uninstall.exe [-silent]. Logs in %TEMP%/ZeqouX3D-Setup/Logs/Setup.log.
GitHub releases: tag v<version>, NOT prerelease (links use /releases/latest), assets Setup.exe + Portable.zip.
Solver islands: PhysicsWorld::Step builds SolverIsland lists (bodies / contacts / joints) and runs phase1 / kinematic
integration / phase2 per substep, in parallel only when no island holds half the work (stats.parallelIslands); only
active solver bodies are written (shared static / kinematic ones are read-only). Queries: ClosestPoint (1 mm probe
sphere), RaycastBatch; PhysicsSettings::stayEvents (PhysicsSystem turns it on). Tests ZXTests Queries.*,
ManualPhysics.ManyIslands. Editor Show -> Physics Debug (DrawPhysicsDebug). CylinderCollider = hull via
PhysicsSystem::HullOf(nullptr, size). Template physics = BuildPhysicsLevel (Physics Playground; scripts PhysicsGun.zs,
Scanner.zs). Tools/make_template_thumbs.py accepts template names to capture only those.
Packaging: Core FileSystem/Pak.h (.zpak = 64 KB ZLZ blocks + TOC; MakePakSource mounts a pak prefix), Engine/
Packaging.h (PackageProject: cook with Content mounted at /Game into <project>/Intermediate/PackageDDC, pak Game/ DDC/
Shaders/ ShaderCache/ Project/Config, copy ZXGame.exe as <Name>.exe + dxcompiler/dxil + module DLL, Game.json),
Engine/GameRuntime.h (BeginGame/TickGame/EndGame shared by PIE and ZXGame), Programs/ZXGame (runtime; waits for meshes,
uploads and material shaders before BeginGame; logs in %LOCALAPPDATA%/<Name>/Saved). Editor EditorPackage.cpp (File >
Package Project, console `package [dir]`). Tests ZXTests Pak.*; scratchpad packagetest.sh packages + runs from %TEMP%.
Sound pass: Engine/Surfaces.h (SurfaceOf: Terrain -> Grass, Water, else first material name; ScriptSystem::materialName
hook set by the editor / ZXGame), scripts character.surface / hit.surface; ImpactSound::rollSound loops driven by
TouchStay events + AudioSystem::velocityOf (set in BeginGame); AddAtmosphere adds the Ambience (Birds_Loop); editor
PlayUISound (StarterContent/Sounds/UI, cvar editor.UISounds). Sounds come from Tools/import_starter_sounds.py <dl dir>.
GPU-driven (M12): Render/Private/SceneRendererGpu.cpp (PrepareGpu: instance rows + cull items / group / slot tables,
AddCullPass, AddHiZPass, DrawIndirect) + Shaders/Forward/GpuCull.hlsl; ForwardPBR VSMain reads the instance from
g_View.visibleLists when g_Draw.drawId is valid (RHI DrawIndexedIndirect, IndirectDrawArgs). Settings gpuDriven /
occlusionCulling (r.GPUDriven, r.OcclusionCulling); capture views turn occlusion off. Benchmark:
`-exec="wait 60;stress.instances 100000;wait 200;stat.cpu;stat.gpu"` (RenderInstance::prevWorld points into
ViewportRenderer's motion history; the 100k frame is memory-bandwidth bound - avoid extra passes over instances).
Sun shadows: ShadowSettings::filter 3 = PCSS (SampleCascadePCSS in ForwardPBR, cascadeDepthRange / sunSourceTan),
ContactShadowSettings + Shaders/Lighting/ContactShadows.hlsl (R8 after the pre-pass, multiplies the sun shadow);
cascade cache invalidation by moved casters (SceneRenderer::SetMovedInstances from ViewportRenderer's pairing).
Cvars r.ShadowFilter, r.ContactShadows; stat.scene prints cascades re-rendered this frame.
GPU particles: ParticleEmitter::gpuSimulation -> ParticleSystem keeps a persistent slot + generation per emitter
(GatherGpu -> render::GpuEmitter rows, tick / resetGeneration); ParticleRenderer runs Shaders/Particles/ParticleSim.hlsl
(InitCS / EmitCS / SimulateCS / ArgsCS, two SoA state buffers compacted every step, counters 64 bytes apart) and draws
the GpuParticle render buffer with RHI DrawIndirect. Console stress.particles N; prefab Effects/P_SparkShower.
RHI D3D12: a UAV->UAV transition request emits a UAV barrier (consecutive passes writing one resource).
GPU timing: average several `stat.gpu` samples (scratchpad gpuavg.sh); the exit-time "GPU ..." lines are one frame.
Deferred path: SceneRenderSettings::deferred (r.Renderer.Path, Full profile default): VariantGBuffer pipelines
(PSGBuffer, SetGBufferFormats; graph materials compile gbuffer / gbufferAlphaTest), passes GBuffer + DeferredLighting
(DeferredVS/PS in ForwardPBR.hlsl: depth -> world via ViewData::invViewProj). Forward and deferred share
EvaluateSurface / LightSurface; the regression compares their images (TAA off).
Animation (M13): Asset/Animation.h (Skeleton, AnimationClip, anim::SampleClip / BlendPoses / LocalToModel /
SkinningMatrices / PackSkin; glTF skins + animations in GltfImporter ImportSkins / ImportAnimations, sub-assets
"skeleton/i", "animation/i", PrefabNode skeleton / skinToNode / animations), Engine/Animation.h (Animator component,
AnimationSystem ticked by the editor and ZXGame; Palette -> RenderInstance::skinMatrices), Shaders/Forward/Skinning.hlsl
(SceneRenderer PrepareSkinning / AddSkinningPass, DrawItem vertex buffer overrides, SkinnedVertexBuffer for the outline).
Starter character Engine/Content/StarterContent/Characters/SK_Human.glb (+ .zmeta scale) from
Tools/convert_starter_character.py (Blender 4.5 at C:/Program Files/Blender Foundation). Tests ZXTests Animation.*,
AnimationSystem.*.
AnimGraph: Engine/AnimGraph.h (.zanimgraph JSON: ParseAnimGraph / WriteAnimGraph / MakeLocomotionGraph), run by
AnimationSystem (Animator::graph, contentRoot set by the host, SetParam / SetTrigger / CurrentState, CharacterState of
the entity or an ancestor -> Speed / VerticalSpeed / Grounded). Starter Characters/Locomotion.zanimgraph is written by
WriteStarterContent; the Third Person template puts SK_Human under the player (Editor::AddAnimatedCharacter).
IK / events: asset::anim::SolveTwoBoneIK / RotateGlobal / GlobalRotation; AnimationSystem::ApplyIK (foot IK through
AnimationSystem::raycast set by the host = physics, ignoring the character entity; look-at via SetLookAt), Events()
-> ScriptSystem::DispatchAnimEvents (TickGame start: onAnimEvent on the entity and its ancestors).
Animation editors: ZXEditor/AnimationEditor.cpp (AnimationPreview = own world + AnimationSystem + ViewportRenderer;
Animation Viewer; AnimGraph Editor with canvas, ParamsPanel / StatePanel / TransitionPanel, SaveGraph ->
AnimationSystem::ReloadGraph). Console anim.viewer <model>, anim.graph <graph> [layer [state]].
AnimGraph layers (0.12.5): AnimGraph derives from AnimStateMachine (base layer) + layers (mask bones, weight / weightParam,
override / additive vs. the state's first frame; states without motion silence the layer); runtime State::machines
(MachineRun per layer), TickMachine returns the layer presence; BlendSpace2DWeights (gradient band). Editor: layer tabs,
st.layer, undo / redo / layer deletion run at the end of the frame (they reallocate the machines).
Root motion (0.13.0): Animator::rootMotion / rootBone; State::rootOffset (subtracted from the root bone) / rootDelta
(StateRootMotion, asset::anim::RootMotionDelta) -> ApplyRootMotion -> CharacterState::rootMotion (TickCharacters adds
it to the velocity) or the top-level entity's Transform. Morph targets: MeshData::morphs (PermuteVertices keeps them
with the vertices), CookedMesh::morphNames / morphDeltas (16 B per vertex per target, PackMorphNormal), clip
MorphTrack (glTF weights), GpuMesh::morphBuffer, RenderInstance::morphWeights (AnimationSystem::MorphWeights or the
MorphTargets component), applied in Skinning.hlsl (skin = ~0: morph-only). Starter slime: Tools/make_starter_blob.py
(Blender, NLA tracks merge bone + shape key actions), Editor::AddSlime, StarterContent/Scripts/Slime.zs.
Soft bodies (0.13.1): Engine/SoftBody.h (SoftBody component with runtime palette, SoftBodySystem: Update = mesh
SoftBodyCarMeshId + wheels at rest, Tick in TickGame before the physics step; node-beam XPBD, plastic beams, contact
planes from PhysicsSystem::ClosestPoint once per frame, raycast suspension; CarRig()/BuildCarMesh() are built on demand -
no statics holding engine allocations, they outlive the leak check). GameSystems::softBodies, ScriptSystem::softBodies
(entity.carInput/...), Gather skins SoftBody meshes with SoftBody::palette. Template Vehicle (Editor::BuildVehicleLevel,
prefab Characters/P_SoftCar from StarterAssets.cpp), scripts SoftBodyCar.zs / CarCamera.zs, console car.drive. Tests
ZXTests SoftBody.*. Editor `camera x y z yaw pitch`: yaw = atan2(dx, dz) of the view direction.
Game UI (M14): Engine/GameUI.h (UICanvas / UIElement / UIPanel / UIText / UIImage / UIButton / UIProgressBar / UISlider /
UIToggle components; GameUISystem Layout -> Interact -> Draw into a ui::DrawList; events consumed by TickGame ->
ScriptSystem::DispatchUIEvents). Editor: Editor::gameUI drawn in ViewportPanel over the viewport image (showGameUI,
action view.gameUI), Interact only while the game shows the cursor (ui.cursor); CreateUIWidget / create ui.<kind>.
ZXGame: m_fonts + ui::Context (fonts) + UIRenderer pass; Packaging adds Engine/Content/Fonts as pak "Fonts".
UI v2 (0.13.13): UICanvas::worldSpace (GameUISystem::SetCamera before Layout; onScreen / depth runtime), UIElement
scroll / wrap, UIInputField (GameUISystem::TypeText, WantsKeyboard gates game input, UIEvent::Submit -> onUISubmit).
ZGraph (0.13.21): Engine/ZGraph.h (.zgraph JSON: ParseGraph / WriteGraph; GraphNodeLibrary = node defs with code
templates {pin} {@setting} {#id}; CompileGraph -> ZeqouScript source + lineNodes for errors; MakeExampleGraph).
ScriptSystem::CheckForChanges compiles .zgraph through it (Script component takes .zs or .zgraph). Tests ZGraph.*
(every library node must generate compilable script). Console graph.code / graph.new / script.attach.
ZNano virtual geometry (M15, 0.14.1): Asset/Nano.h (Build = cluster DAG, AppendFallbackLods, SelectCut /
ProjectedError on the CPU, MakeDetailedRock), CookedMesh::nano (NanoClusterHeader 80 B, sorted by parentError desc),
CookMesh NanoMode (Auto >= 50k static triangles, .zmeta virtualGeometry). GPU: GpuMesh::nano* (RenderAssets),
Render/Private/SceneRendererNano.cpp (PrepareNano: bins + per-view instance prefixes; AddNanoCullPass = Clear / Cull /
Args / Expand in Shaders/Forward/Nano.hlsl; DrawNano; budget feedback via readback) + ForwardPBR VSMain under ZX_NANO
(index = visible record << 8 | local vertex; drawId = bin). Regions: 0 main phase 1, 1 phase 2, 2 shadows (culled
right before each cascade). Common/HiZ.hlsli shared with GpuCull. Console stress.nano N [subdiv], r.Nano*,
r.ViewMode 4 colors clusters. Tests Nano.*, ManualNano.RockDagStats. -gpuvalidation TDRs on UHD 620 even without nano.
M14 acceptance (0.14.0): Engine/Private/ZGraphDemo.cpp (MakeCoinGraph / MakeGateGraph / MakeGameHUDGraph, a
Builder helper) written by Editor::BuildCoinGame into the Third Person template (Content/Scripts/Graphs); scripts
game.get/set/add (ScriptSystem::m_gameValues, GameValue) and entity.moveTo (m_moves, ticked in Tick); ZGraph actions
with a target other than self are wrapped in `if target != nil`. Test ZGraph.ThirdPersonMiniGame.
ZGraph debugging (0.13.23): CompileGraph(..., trace) emits __trace(node) per event / statement; ScriptSystem::
traceGraphs (editor on, ZXGame off) + onGraphTrace(file, entity, node) via m_current (set in RebindGlobals);
InstancesOf / ReadField for live values; editor ZGraphTrace / ZGraphAfterTick (breakpoint -> TogglePause after
TickGame), hits glow (frozen while paused). Console graph.break.
ZGraph editor (0.13.22): ZXEditor/ZGraphEditor.cpp (Editor::graphEditor, window "Script Graph"; canvas like the
Material Editor; snapshot undo = WriteGraph; Graph::PinType gives Get / Set Variable pins their variable's type;
GraphCompileResult::errorNodes). Console graph.edit. Real-mouse checks: scratchpad uidrag2.py also has "type:text"
and "click:x,y" tokens.
UI Designer (0.13.20): EditorTool::UIDesigner (EditorUIDesigner.cpp: UIDesignerScreen preview rect used for
gameUI.Layout in ViewportPanel, UpdateUIDesigner after gameUI.Draw - PickChain / PickTarget Figma-like picking,
handles, reorder via scene::Children (all siblings recorded for undo), DrawUIDesignerPanel); Context::SetCursorShape.
Real-mouse UI tests: the Win32 layer drops a held button that GetAsyncKeyState says is up, so PostMessage drags don't
work - use scratchpad uidrag2.py (SetCursorPos + mouse_event, foreground window; tokens "a>b", "dbl:x,y",
"key:ctrl+z"; coords = screenshot pixels; the 1600x900 window is clamped to 1600x938 on the 125 % laptop).
Localization (0.13.19): Engine/Localization.h (Load scans <content>/StarterContent/Localization then Localization,
*.zloc per language; Translate / Format, SetLanguage + preferenceFile, Poll hot reload, editing SetText / Save);
UIText::key / UIInputField::placeholderKey resolved in GameUISystem::Layout (localization pointer); scripts tr() /
lang (ScriptSystem::localization); editor Editor::localization + EditorLocalization.cpp (Window > Localization),
console lang; ZXGame preference Saved/Config/Language.txt; platform::GetUserLanguage. Tests Localization.*.
UI styles / look / navigation (0.13.18): Engine/UIStyle.h (ParseUIStyle, ParseStyleValue: .zstyle CSS-like sheets),
GameUILook.cpp (GameUISystem::ApplyStyles in Layout - base rules write fields, state rules are Overrides undone by
RestoreOverrides at the end of Draw / start of Update; UIElement opacity / scale / translate / transition -> runtime
shown* / anim* -> pixelScale / drawOpacity used by DrawPass; UIAnimation presets, Animate tweens, Navigate with focus
ring; UIEvent::Back -> onUIBack). GameUISystem::contentRoot set by hosts, Update called by TickGame. Starter
StarterContent/UI/Game.zstyle (Vehicle HUD). Console input.press <key> (simulated key in Play, captures game input).
time.scale = ScriptSystem::timeScale -> GameTimeScale(); hosts scale particles / animation with it. Tests GameUI.*.
Engine module depends on UI now.
Wind (0.13.6): Engine/Wind.h (WindZone / WindResponse components, WindClock singleton advanced by TickWind in the editor
and ZXGame, WindAt / WindFor / HasWind / WindDragForce). Consumers: ParticleSystem (Emitter::wind; drag relaxes the
velocity towards it; GpuEmitter::wind at byte 228, ParticleSim.hlsl), Fire smoke volumes, GatherFogVolumes / FindClouds
/ GatherWater (only when the scene has a Wind Zone or the entity a custom Wind Response), TickGame (Rigid Bodies with a
Wind Response), SoftBody air drag. AddAtmosphere adds a 4 m/s "Wind" zone to templates. Tests Wind.*.
Water v2 (0.13.7): Render/Water.h + Shaders/Water/Ocean.hlsl (Evolve / FFT / Assemble / Mip; foam from the Jacobian)
+ Shaders/Water/Water.hlsl (rings or grid in the VS, SSR, sun shadows via LightingInputs, baked caustic / foam
textures); Water component presets (WaterPreset, ApplyWaterPreset / UpdateWaterPresets), console water.preset.
Autotest ignores the real mouse/keyboard unless -autotestinput.
Water gameplay (0.13.9): Render/OceanWaves.h (BuildOceanSpectrum shared with the GPU; OceanWaves = CPU FFT of the two
long cascades), Engine/WaterPhysics.h (WaterSystem::Update / Sample / TickBuoyancy / TickSplashes, Buoyancy component;
GameSystems::water; waves run on EnvironmentTime = the wind clock). PhysicsSystem::beforeStep runs per fixed step (TickGame
sets it: wind drag + buoyancy). Underwater: WaterRenderer::AddUnderwaterPass after the fog passes (UnderwaterPS),
ShadeFromBelow in WaterPS. Tests Water.* (spectrum, wave height, velocity, floating plank).
Asset packs (0.13.8): Engine/Content/Packs/<Folder> (Pack.json, License.txt, Models/) built by
`python Tools/import_asset_packs.py <dir with the Quaternius zips>` (Blender for the Survival .blend files);
Engine/Project.h ListAssetPacks / CopyAssetPack, NewProjectOptions::assetPacks, -packs=all|A,B. Engine profile choice:
RHI/EngineProfile.h Save/LoadEngineProfilePreference (%LOCALAPPDATA%/ZeqouX3D/EngineProfile.txt; tests must not leave it
behind), Editor::RequestRestart, DrawProfileDialog (badge / browser footer / help.engineProfile), console profile.switch.
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
- GitHub: https://github.com/Mishaadevv/x3d (public, branch main, ZeqouX3D License = PolyForm Strict 1.0.0 + Game Exception: games made with it may be sold / distributed with the packaged runtime). Screenshots for the README live in docs/images.
- Website: the sibling repo ../Zeqou (https://mishaadevv.github.io/zeqou/, page /apps/x3d, docs /docs/x3d) reads the engine
  version from this CMakeLists.txt: after a version bump run `npm run versions` there, commit and push (GitHub Pages deploys).

## Rules
- Definition of done: builds warning-free (Debug + Development), tests pass, no leaks, no D3D12 debug-layer errors.
- Don't delete the source zips (`inter.zip`, `lucide-*.zip`) without asking. Commit only when asked.
