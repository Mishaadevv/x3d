// Editor console variables and commands (typed into the Output Log prompt; `help` lists them).
// Render CVars push their value into the viewport settings when set from the console; the viewport toolbar keeps
// editing the settings directly.

#include "Asset/Nano.h"
#include "Engine/TimeOfDay.h"
#include "Engine/ZGraph.h"
#include "Core/FileSystem/FileSystem.h"
#include "Editor.h"
#include "EditorActions.h"
#include "Core/Console/Console.h"
#include "Core/Containers/Algorithm.h"
#include "Core/Math/Random.h"
#include "Core/Profiler/Profiler.h"
#include "Core/Threading/Thread.h"
#include "Core/Time/Time.h"
#include "Engine/BuiltinAssets.h"
#include "Engine/Prefab.h"
#include "Core/String/Parse.h"
#include "Core/String/Path.h"
#include "Core/HAL/PlatformFile.h"
#include "RHI/EngineProfile.h"

ZX_DEFINE_LOG_CATEGORY_STATIC(LogCmd, Info);

namespace zx::editor {

namespace {

Editor* g_editor = nullptr;

CVar<f32> CVarScreenPercentage("r.ScreenPercentage", 100.0f, "Viewport render resolution in percent (25..200)");
CVar<bool> CVarTAA("r.TAA", true, "Temporal anti-aliasing (FXAA when off)");
CVar<bool> CVarAO("r.AO", true, "Ground-truth ambient occlusion");
CVar<bool> CVarShadows("r.Shadows", true, "Sun cascaded shadows");
CVar<bool> CVarSky("r.Sky", true, "Physical sky and height fog");
CVar<bool> CVarBloom("r.Bloom", true, "Bloom");
CVar<bool> CVarTSR("r.TSR", true, "ZTSR temporal upscaling when r.ScreenPercentage is below 100");
CVar<bool> CVarDynRes("r.DynamicResolution", false, "Render scale follows the GPU frame time (r.DynamicResolution.TargetMs)");
CVar<f32> CVarDynResTarget("r.DynamicResolution.TargetMs", 16.6f, "Dynamic resolution target GPU frame time (ms)");
CVar<f32> CVarDynResMin("r.DynamicResolution.MinPercentage", 50.0f, "Lowest render scale of dynamic resolution (percent)");
CVar<bool> CVarSSR("r.SSR", false, "Screen-space reflections (on by default in the Full profile)");
CVar<bool> CVarGpuDriven("r.GPUDriven", true, "GPU culling + ExecuteIndirect for opaque geometry (CPU draw lists when off)");
CVar<bool> CVarVsm("r.Shadows.Virtual", false, "Virtual shadow maps for the sun: sharp shadows near and far, pages cached until something moves");
CVar<f32> CVarVsmNormalBias("r.Shadows.Virtual.NormalBias", 1.5f, "Virtual shadow receiver offset along the normal, texels");
CVar<f32> CVarVsmDistance("r.Shadows.Virtual.Distance", 250.0f, "Reach of the virtual shadow maps around the camera, meters (a Directional Light's Shadow Distance wins)");
CVar<f32> CVarVsmResolution("r.Shadows.Virtual.Resolution", 1.0f, "Virtual shadow texels per pixel footprint (< 1 coarser, Lite 0.5)");
CVar<f32> CVarNanoShadowError("r.Nano.ShadowPixelError", 2.0f, "Virtual geometry LOD error allowed in shadow maps, texels");
CVar<bool> CVarNano("r.Nano", true, "Virtual geometry: meshes cooked with clusters are drawn cluster by cluster (fallback LODs when off)");
CVar<f32> CVarNanoPixelError("r.Nano.PixelError", 1.0f, "Virtual geometry LOD error allowed on screen, pixels (Lite: 2)");
CVar<i32> CVarNanoBudget("r.Nano.TriangleBudget", 3000, "Virtual geometry triangle budget of the main view, thousands (over it detail drops)");
CVar<bool> CVarOcclusion("r.OcclusionCulling", true, "Two-phase HiZ occlusion culling (GPU-driven path)");
CVar<i32> CVarShadowFilter("r.ShadowFilter", 1, "Sun shadow filter: 0 hard, 1 4 taps, 2 9 taps, 3 PCSS (soft, contact-hardening)");
CVar<bool> CVarContactShadows("r.ContactShadows", true, "Screen-space contact shadows of the sun");
CVar<i32> CVarRendererPath("r.Renderer.Path", 0, "Opaque rendering path: 0 forward+ (clustered), 1 deferred (G-buffer + lighting pass)");
CVar<i32> CVarGIMode("r.GI.Mode", 0, "0 dynamic probes (traced every frame), 1 baked probes (gi.bake; no GPU time; dynamic when the scene has none)");
CVar<bool> CVarGIScreen("r.GI.ScreenSpace", false, "Screen-space GI: short-range bounce from what is on screen (Full profile; needs the deferred normals)");
CVar<bool> CVarGILumen("r.GI.Lumen", false, "Lumen-lite prototype: per-pixel GI rays (screen, then the distance field) over the probes - high-end GPUs (deferred path)");
CVar<bool> CVarGI("r.GI", true, "Dynamic global illumination: light bounces off surfaces (probes traced through a distance field of the scene)");
CVar<f32> CVarGIIntensity("r.GI.Intensity", 1.0f, "Multiplies the bounced (indirect) diffuse light");
CVar<i32> CVarGIRays("r.GI.Rays", 64, "Rays per GI probe update (16..256)");
CVar<i32> CVarGIStride("r.GI.Stride", 2, "One of every N GI probes is updated per frame (1 = all: fastest response, most GPU time)");
CVar<f32> CVarGISpacing("r.GI.Spacing", 1.4f, "Meters between GI probes");
CVar<bool> CVarProbes("r.ReflectionProbes", true, "Reflection probes and the sky reflection cube");
CVar<f32> CVarExposure("r.ExposureCompensation", 1.0f, "Exposure compensation in EV (+ = brighter)");
CVar<i32> CVarViewMode("r.ViewMode", 0, "0 lit, 1 base color, 2 normals, 3 roughness/metallic, 4 LODs, 5 cascades, 6 clusters, 7 AO, 8 local shadows, 9 indirect light (GI), 10 lightmap UV density");
CVar<i32> CVarForceLod("r.ForceLOD", -1, "Force a mesh LOD (-1 = automatic)");
CVar<f32> CVarCameraSpeed("editor.CameraSpeed", 4.0f, "Viewport fly speed (m/s)");
CVar<bool> CVarShowGrid("editor.ShowGrid", true, "Viewport grid");
CVar<bool> CVarUISounds("editor.UISounds", false, "Editor feedback sounds (Play / Stop, builds, packaging); off by default");

Editor* Ed()
{
    return g_editor;
}

/// The entity with this name (any case; '_' stands for a space, since console arguments split at spaces).
ecs::Entity FindNamed(StringView name)
{
    String spaced(name);
    for (char& c : spaced)
        if (c == '_')
            c = ' ';
    ecs::Entity found;
    if (Editor* ed = Ed())
        ed->world.Query<const scene::EntityName>().Each([&](ecs::Entity e, const scene::EntityName& n) {
            if (!found && (n.value.View().EqualsIgnoreCase(name) || n.value.View().EqualsIgnoreCase(spaced)))
                found = e;
        });
    return found;
}

ConCommand CmdPackage("package", "Packages the project into a standalone game: package [output folder] (default Builds/Windows/<Name>)",
                      [](Span<const StringView> args) {
                          if (Ed())
                              Ed()->StartPackage(args.Empty() ? StringView() : args[0]);
                      });
ConCommand CmdClear("clear", "Clears the Output Log", [](Span<const StringView>) {
    if (Ed())
        Ed()->log->Clear();
});

ConCommand CmdQuit("quit", "Closes the editor (asks to save changes)", [](Span<const StringView>) {
    if (Ed())
        Ed()->RequestSceneAction(SceneAction::Quit);
});

ConCommand CmdStatGpu("stat.gpu", "Prints the GPU time of every render pass of the last frame", [](Span<const StringView>) {
    if (!Ed())
        return;
    for (const rhi::GpuTiming& t : Ed()->device->GetGpuTimings())
        ZX_LOG_INFO(LogCmd, "  {:<22} {:.3f} ms", t.name, t.milliseconds);
});

ConCommand CmdStatCpu("stat.cpu", "Prints the CPU time of the main thread's profiler zones in the last frame (largest first)",
                      [](Span<const StringView>) {
                          const u64 frame = profiler::CurrentFrame() - 1;
                          Array<profiler::Zone> zones;
                          if (!profiler::GetFrameZones(frame, zones))
                              return;
                          const u32 main = Thread::CurrentId();
                          HashMap<const char*, f64> sums;
                          for (const profiler::Zone& z : zones)
                              if (z.threadId == main)
                                  *sums.Insert(z.name, 0.0).first += Clock::TicksToMilliseconds(z.endTicks - z.startTicks);
                          Array<Pair<const char*, f64>> sorted;
                          for (const auto& kv : sums)
                              sorted.PushBack({kv.first, kv.second});
                          Sort(sorted, [](const auto& a, const auto& b) { return a.second > b.second; });
                          for (usize i = 0; i < Min<usize>(sorted.Size(), 20); ++i)
                              ZX_LOG_INFO(LogCmd, "  CPU {:<28} {:7.2f} ms", sorted[i].first, sorted[i].second);
                      });

ConCommand CmdStatScene("stat.scene", "Prints viewport and scene statistics", [](Span<const StringView>) {
    Editor* ed = Ed();
    if (!ed)
        return;
    const render::SceneRenderer::Stats& st = ed->viewport->Scene().GetStats();
    const render::RenderAssets::Stats ra = ed->renderAssets->GetStats();
    ZX_LOG_INFO(LogCmd, "  Viewport {}x{} (render {}x{}), {} entities", ed->viewportSize.x, ed->viewportSize.y,
                ed->viewport->RenderSize().x, ed->viewport->RenderSize().y, ed->world.EntityCount());
    ZX_LOG_INFO(LogCmd, "  Draws {} (+{} shadow), {:.0f}k triangles", st.draws, st.shadowDraws, f64(st.triangles) / 1000.0);
    if (st.gpuDriven)
        ZX_LOG_INFO(LogCmd, "  GPU-driven: {} cull items, {} groups, {} indirect calls", st.gpuItems, st.gpuGroups, st.indirectCalls);
    if (ra.nanoPoolPages > 0)
        ZX_LOG_INFO(LogCmd, "  Cluster streaming: {} / {} pool pages resident ({:.0f} / {:.0f} MB), {} wanted, {} loaded last frame, {:.0f} MB of pages on disk",
                    ra.nanoResidentPages, ra.nanoPoolPages, f64(ra.nanoResidentPages) * 0.125, f64(ra.nanoPoolPages) * 0.125,
                    ra.nanoWantedPages, ra.nanoStreamedPages, f64(ra.nanoFileBytes) / (1024.0 * 1024.0));
    if (st.nanoInstances > 0)
        ZX_LOG_INFO(LogCmd, "  Virtual geometry: {} instances, {:.1f}M source triangles; {} clusters tested, {} drawn, {:.0f}k triangles "
                            "(+{:.0f}k shadow), {} bins, error x{:.2f}{}",
                    st.nanoInstances, f64(st.nanoSourceTriangles) / 1e6, st.nanoTested, st.nanoClusters, f64(st.nanoTriangles) / 1000.0,
                    f64(st.nanoShadowTriangles) / 1000.0, st.nanoBins, st.nanoErrorScale, st.nanoOverflow ? " (over budget)" : "");
    ZX_LOG_INFO(LogCmd, "  Sun cascades re-rendered this frame: {}", st.cascadesUpdated);
    if (st.virtualShadows)
        ZX_LOG_INFO(LogCmd, "  Virtual shadows: {} pages resident, {} needed, {} rendered this frame ({} draws), {} waiting{}",
                    st.vsmMappedPages, st.vsmRequestedPages, st.vsmRenderedPages, st.vsmDraws, st.vsmPendingPages,
                    st.vsmBuilding ? " (rebuilding for the turned sun)" : "");
    if (ed->viewport->GI().GetStats().baked)
        ZX_LOG_INFO(LogCmd, "  GI: baked probes (no tracing)");
    if (const render::GlobalIllumination::Stats& gi = ed->viewport->GI().GetStats(); gi.active)
        ZX_LOG_INFO(LogCmd, "  GI: {} probes ({} updated, {} rays this frame), distance field {:.2f} m voxels{}", gi.probes, gi.probesUpdated,
                    gi.rays, gi.finestVoxel,
                    gi.levelRebuilt != ~0u ? String::Format(", level {} rebuilt ({} sections, {} triangles)", gi.levelRebuilt, gi.voxelizedSections,
                                                            gi.voxelizedTriangles)
                                           : String());
    ZX_LOG_INFO(LogCmd, "  GPU assets: {} meshes ({:.1f} MB), {} textures ({:.1f} MB), {} materials", ra.meshes,
                f64(ra.meshBytes) / (1024.0 * 1024.0), ra.textures, f64(ra.textureBytes) / (1024.0 * 1024.0), ra.materials);
});

ConCommand CmdStressInstances("stress.instances",
                               "Spawns N small built-in shapes in a grid around the camera (renderer stress test, outside undo): stress.instances 100000",
                               [](Span<const StringView> args) {
                                   Editor* ed = Ed();
                                   if (!ed)
                                       return;
                                   const u32 n = args.Empty() ? 10000u : u32(Clamp<i64>(ParseInt(args[0]).ValueOr(10000), 1, 1000000));
                                   const u32 side = u32(Ceil(Sqrt(f32(n))));
                                   const f32 spacing = 0.9f;
                                   const Vec3 origin = ed->camera.position + Vec3(-f32(side) * spacing * 0.5f, -1.0f, 2.0f);
                                   Random rng(7);
                                   const BuiltinShape shapes[3] = {BuiltinShape::Cube, BuiltinShape::Sphere, BuiltinShape::Cylinder};
                                   for (u32 i = 0; i < n; ++i) {
                                       Transform t(origin + Vec3(f32(i % side) * spacing, rng.Range(0.0f, 0.4f), f32(i / side) * spacing),
                                                   Quat::FromEulerDegrees(Vec3(0.0f, rng.Range(0.0f, 360.0f), 0.0f)), Vec3(0.35f));
                                       const ecs::Entity e = scene::CreateNode(ed->world, StringView(), t);
                                       MeshRenderer mr;
                                       mr.mesh = builtin::Mesh(shapes[i % 3]);
                                       mr.collision = false;
                                       ed->world.Add(e, mr);
                                   }
                                   scene::UpdateWorldTransforms(ed->world);
                                   ZX_LOG_INFO(LogCmd, "Spawned {} instances", n);
                               });

ConCommand CmdStressNano("stress.nano",
                          "Virtual geometry stress test: N instances of a procedural rock with 1.3M triangles each (outside undo): "
                          "stress.nano 770 (= 1 billion source triangles) [subdivisions 4..8] [different rocks 1..16]",
                          [](Span<const StringView> args) {
                              Editor* ed = Ed();
                              if (!ed)
                                  return;
                              const u32 n = args.Empty() ? 770u : u32(Clamp<i64>(ParseInt(args[0]).ValueOr(770), 1, 100000));
                              const u32 subdivisions = args.Size() > 1 ? u32(Clamp<i64>(ParseInt(args[1]).ValueOr(8), 4, 8)) : 8u;
                              const u32 variants = args.Size() > 2 ? u32(Clamp<i64>(ParseInt(args[2]).ValueOr(1), 1, 16)) : 1u;
                              static const Guid ns(0x5A584E616E6F5374ull, 0x72657373526F636Bull);
                              Array<asset::AssetId> ids;
                              for (u32 v = 0; v < variants; ++v) {
                                  const asset::AssetId id = Guid::FromName(ns, String::Format("Rock{}.{}", subdivisions, v).View());
                                  ids.PushBack(id);
                                  if (ed->assets->GetState(id) == asset::AssetState::Loaded)
                                      continue;
                                  const Stopwatch timer;
                                  asset::MeshData rock = asset::nano::MakeDetailedRock(subdivisions, v + 1);
                                  asset::CookedMesh cooked = asset::CookMesh(rock, true, asset::NanoMode::On);
                                  ZX_LOG_INFO(LogCmd, "Rock: {} triangles, {} clusters in {} levels, cooked in {:.1f} s", cooked.nano.leafTriangles,
                                              cooked.nano.clusters.Size(), cooked.nano.levels, timer.ElapsedMilliseconds() / 1000.0);
                                  ed->assets->AddMemoryMesh(id, move(cooked));
                              }
                              const u32 side = u32(Ceil(Sqrt(f32(n))));
                              const f32 spacing = 7.0f;
                              const Vec3 forward = SafeNormalize(ed->camera.Forward() * Vec3(1.0f, 0.0f, 1.0f), Vec3(0, 0, 1));
                              const Vec3 right = Cross(Vec3(0, 1, 0), forward);
                              const Vec3 origin = ed->camera.position + forward * 8.0f - right * (f32(side) * spacing * 0.5f);
                              const f32 groundY = 0.0f; // rocks rest half-buried on the ground plane
                              Random rng(11);
                              for (u32 i = 0; i < n; ++i) {
                                  Vec3 p = origin + right * (f32(i % side) * spacing + rng.Range(-1.5f, 1.5f)) +
                                           forward * (f32(i / side) * spacing + rng.Range(-1.5f, 1.5f));
                                  p.y = groundY;
                                  Transform t(p, Quat::FromEulerDegrees(Vec3(rng.Range(-20.0f, 20.0f), rng.Range(0.0f, 360.0f), 0.0f)),
                                              Vec3(rng.Range(1.5f, 3.0f)));
                                  const ecs::Entity e = scene::CreateNode(ed->world, StringView(), t);
                                  MeshRenderer mr;
                                  mr.mesh = ids[i % variants];
                                  mr.collision = false;
                                  ed->world.Add(e, mr);
                              }
                              scene::UpdateWorldTransforms(ed->world);
                              ZX_LOG_INFO(LogCmd, "Spawned {} rocks", n);
                          });

ConCommand CmdTime("time", "Time of Day: time <hour 0..24 | dawn | sunrise | morning | noon | golden | sunset | dusk | midnight> [day length in minutes] - adds a Time of Day to the scene if it has none",
                   [](Span<const StringView> args) {
                       Editor* ed = Ed();
                       if (!ed)
                           return;
                       TimeOfDay* clock = nullptr;
                       ed->world.Query<TimeOfDay>().Each([&](TimeOfDay& t) {
                           if (!clock)
                               clock = &t;
                       });
                       if (!clock) {
                           const ecs::Entity e = scene::CreateNode(ed->world, "Time of Day", Transform());
                           ed->world.Add(e, TimeOfDay());
                           clock = ed->world.Get<TimeOfDay>(e);
                       }
                       if (!clock)
                           return;
                       if (!args.Empty()) {
                           // A number, or a moment of the day: dawn, sunrise, morning, noon, golden, sunset, dusk, midnight.
                           clock->hour = Clamp(ParseFloat(args[0]).ValueOr(clock->hour), 0.0f, 24.0f);
                           for (u32 m = 0; m <= u32(DayMoment::Midnight); ++m)
                               if (const StringView name = DayMomentName(DayMoment(m));
                                   !args[0].Empty() && args[0].Size() <= name.Size() && name.Left(args[0].Size()).EqualsIgnoreCase(args[0]))
                                   clock->hour = HourOf(*clock, DayMoment(m));
                       }
                       if (args.Size() > 1)
                           clock->dayLength = Max(ParseFloat(args[1]).ValueOr(clock->dayLength), 0.0f);
                       const CelestialState sky = ComputeCelestial(*clock);
                       ZX_LOG_INFO(LogCmd, "Time {} (day length {} min), sun {:.1f} deg, moon {:.1f} deg ({:.0f} % lit)", FormatHour(clock->hour),
                                   clock->dayLength, RadToDeg(Asin(sky.toSun.y)), RadToDeg(Asin(sky.toMoon.y)), sky.moonPhase * 100.0f);
                   });

ConCommand CmdBakeLighting("gi.bake", "Bakes the lighting of the static scene into probes saved next to the scene: gi.bake [spacing m] [iterations]",
                            [](Span<const StringView> args) {
                                if (Editor* ed = Ed())
                                    ed->BakeLighting(args.Size() > 0 ? ParseFloat(args[0]).ValueOr(0.0f) : 0.0f,
                                                     args.Size() > 1 ? u32(ParseInt(args[1]).ValueOr(0)) : 0u);
                            });

ConCommand CmdClearBake("gi.clearbake", "Deletes the scene's baked lighting", [](Span<const StringView>) {
    if (Editor* ed = Ed()) {
        platform::DeleteFileAt(ed->BakedLightingPath());
        ed->viewport->GI().SetBaked(nullptr);
        ZX_LOG_INFO(LogCmd, "Baked lighting removed");
    }
});

ConCommand CmdStressParticles("stress.particles",
                               "Adds a GPU-simulated spark fountain in front of the camera keeping about N particles alive (bouncing off the scene): stress.particles 200000",
                               [](Span<const StringView> args) {
                                   Editor* ed = Ed();
                                   if (!ed)
                                       return;
                                   const u32 n = args.Empty() ? 100000u : u32(Clamp<i64>(ParseInt(args[0]).ValueOr(100000), 1, 250000));
                                   const ecs::Entity e = scene::CreateNode(ed->world, "GPU Sparks", Transform(ed->camera.position + SafeNormalize(ed->camera.Forward() * Vec3(1.0f, 0.0f, 1.0f), Vec3(0, 0, 1)) * 6.0f + Vec3(0.0f, -1.5f, 0.0f)));
                                   ParticleEmitter pe;
                                   pe.gpuSimulation = true;
                                   pe.lifetime = 4.0f;
                                   pe.lifetimeVariation = 0.2f;
                                   pe.rate = f32(n) / pe.lifetime;
                                   pe.shape = EmitterShape::Cone;
                                   pe.radius = 0.05f;
                                   pe.coneAngle = 0.45f;
                                   pe.speed = 7.0f;
                                   pe.speedVariation = 0.35f;
                                   pe.acceleration = Vec3(0.0f, -9.81f, 0.0f);
                                   pe.drag = 0.1f;
                                   pe.turbulence = 0.0f;
                                   pe.startSize = 0.04f;
                                   pe.endSize = 0.015f;
                                   pe.startColor = Vec3(1.0f, 0.7f, 0.3f);
                                   pe.endColor = Vec3(1.0f, 0.15f, 0.02f);
                                   pe.endAlpha = 0.0f;
                                   pe.fadeIn = 0.0f;
                                   pe.look = ParticleLook::Spark;
                                   pe.blend = ParticleBlend::Additive;
                                   pe.intensity = 2.0f;
                                   pe.collision = ParticleCollision::Bounce;
                                   pe.bounciness = 0.45f;
                                   ed->world.Add(e, pe);
                                   scene::UpdateWorldTransforms(ed->world);
                                   ZX_LOG_INFO(LogCmd, "GPU spark fountain: {:.0f} particles/s", pe.rate);
                               });

ConCommand CmdAnimViewer("anim.viewer",
                         "Opens a model in the Animation Viewer: anim.viewer <path of a .glb / .gltf> [clip [time]] (time pauses there)",
                         [](Span<const StringView> args) {
                             if (!Ed() || args.Empty())
                                 return;
                             const String path = Path::IsAbsolute(args[0]) || platform::FileExists(args[0]) ? String(args[0])
                                                                                                           : Path::Join(Ed()->contentRoot, args[0]);
                             OpenAnimationViewer(*Ed(), path);
                             if (args.Size() > 1) {
                                 Ed()->animViewer.pendingClip = String(args[1]);
                                 Ed()->animViewer.pendingTime = args.Size() > 2 ? ParseFloat(args[2]).ValueOr(0.0f) : -1.0f;
                             }
                         });

ConCommand CmdAnimGraph("anim.graph",
                        "Opens a .zanimgraph in the AnimGraph Editor: anim.graph <path> [layer [state]] (Content-relative or absolute; layer 0 = "
                        "base, state = name to select)",
                        [](Span<const StringView> args) {
                            if (!Ed() || args.Empty())
                                return;
                            const String path = Path::IsAbsolute(args[0]) ? String(args[0]) : Path::Join(Ed()->contentRoot, args[0]);
                            OpenAnimGraphEditor(*Ed(), path);
                            AnimGraphEditorState& st = Ed()->animGraphEditor;
                            if (args.Size() > 1 && st.open) {
                                const i64 layer = ParseInt(args[1]).ValueOr(-1);
                                if (layer >= 0 && usize(layer) <= st.graph.layers.Size())
                                    st.layer = u32(layer);
                                if (args.Size() > 2)
                                    st.selectedState = st.graph.Machine(st.layer).FindState(args[2]);
                            }
                        });

ConCommand CmdCarDrive("car.drive",
                       "Drives every soft-body car instead of its script: car.drive <throttle> <steer> [brake] (car.drive off: scripts again)",
                       [](Span<const StringView> args) {
                           if (!Ed() || args.Empty())
                               return;
                           SoftBodySystem& cars = Ed()->softBodies;
                           cars.overrideInput = !args[0].EqualsIgnoreCase("off");
                           cars.forcedInput.throttle = ParseFloat(args[0]).ValueOr(0.0f);
                           cars.forcedInput.steer = args.Size() > 1 ? ParseFloat(args[1]).ValueOr(0.0f) : 0.0f;
                           cars.forcedInput.brake = args.Size() > 2 ? ParseFloat(args[2]).ValueOr(0.0f) : 0.0f;
                       });

ConCommand CmdWaterPreset("water.preset", "Applies a water preset to every Water: water.preset <ocean|lake|pool|storm|tropical>",
                           [](Span<const StringView> args) {
                               if (!Ed() || args.Empty())
                                   return;
                               static const char* const Names[] = {"custom", "ocean", "lake", "pool", "storm", "tropical"};
                               for (u32 i = 0; i < ZX_ARRAY_COUNT(Names); ++i)
                                   if (args[0].EqualsIgnoreCase(Names[i]))
                                       Ed()->world.Query<Water>().Each([&](ecs::Entity, Water& w) { ApplyWaterPreset(w, WaterPreset(i)); });
                           });

ConCommand CmdCloudPreset("clouds.preset",
                           "Applies a cloud preset to every Volumetric Clouds: clouds.preset <clear|fairweather|scattered|cloudy|overcast|storm>",
                           [](Span<const StringView> args) {
                               if (!Ed() || args.Empty())
                                   return;
                               static const char* const Names[] = {"custom", "clear", "fairweather", "scattered", "cloudy", "overcast", "storm"};
                               for (u32 i = 0; i < ZX_ARRAY_COUNT(Names); ++i)
                                   if (args[0].EqualsIgnoreCase(Names[i]))
                                       Ed()->world.Query<VolumetricClouds>().Each(
                                           [&](ecs::Entity, VolumetricClouds& c) { ApplyCloudPreset(c, CloudPreset(i)); });
                           });

ConCommand CmdSun("sun", "Turns the scene's sun: sun <elevation deg> [azimuth deg]", [](Span<const StringView> args) {
    Editor* ed = Ed();
    if (!ed || args.Empty())
        return;
    ed->environment.sunElevation = DegToRad(ParseFloat(args[0]).ValueOr(30.0f));
    if (args.Size() > 1)
        ed->environment.sunAzimuth = DegToRad(ParseFloat(args[1]).ValueOr(0.0f));
    ed->world.Query<const DirectionalLight, Transform>().Each([&](ecs::Entity, const DirectionalLight&, Transform& t) {
        t.rotation = Editor::SunRotation(ed->environment.sunElevation, ed->environment.sunAzimuth);
    });
});

ConCommand CmdGraphicsEra("graphics.era", "The project's graphics generation: graphics.era <modern|2017|2011|2008>",
                           [](Span<const StringView> args) {
                               if (Editor* ed = Ed(); ed && !args.Empty())
                                   ed->SetGraphicsEra(args[0]);
                           });

ConCommand CmdTerrainSculpt("terrain.sculpt",
                             "Sculpts / paints the terrain at a point like a held Terrain tool stroke: terrain.sculpt "
                             "<raise|lower|smooth|flatten|setheight|noise|terrace|erode|grass|rock|dirt|sand|erase> <x> <z> [seconds] [radius] "
                             "[strength] (setheight: target = the sixth argument)",
                             [](Span<const StringView> args) {
                                 Editor* ed = Ed();
                                 if (!ed || args.Size() < 3)
                                     return;
                                 static const char* const Names[] = {"raise",  "lower", "smooth", "flatten", "setheight", "noise", "terrace",
                                                                     "erode",  "ramp",  "grass",  "rock",    "dirt",      "sand",  "erase"};
                                 i32 mode = -1;
                                 for (u32 i = 0; i < ZX_ARRAY_COUNT(Names); ++i)
                                     if (args[0].EqualsIgnoreCase(Names[i]))
                                         mode = i32(i);
                                 if (mode < 0) {
                                     ZX_LOG_WARN(LogCmd, "terrain.sculpt: unknown mode '{}'", args[0]);
                                     return;
                                 }
                                 const f32 x = ParseFloat(args[1]).ValueOr(0.0f), z = ParseFloat(args[2]).ValueOr(0.0f);
                                 const f32 seconds = args.Size() > 3 ? ParseFloat(args[3]).ValueOr(1.0f) : 1.0f;
                                 ToolState& ts = ed->tools;
                                 const TerrainBrushSettings saved = ts.terrain;
                                 ts.terrain.mode = TerrainBrushMode(mode);
                                 if (args.Size() > 4)
                                     ts.terrain.radius = ParseFloat(args[4]).ValueOr(ts.terrain.radius);
                                 if (args.Size() > 5)
                                     ts.terrain.strength = ParseFloat(args[5]).ValueOr(ts.terrain.strength);
                                 if (args.Size() > 6)
                                     ts.terrain.targetHeight = ParseFloat(args[6]).ValueOr(ts.terrain.targetHeight);
                                 ecs::Entity terrain;
                                 Vec3 hit;
                                 if (!ed->terrains.Raycast(ed->world, Ray(Vec3(x, 5000.0f, z), Vec3(0, -1, 0)), 10000.0f, terrain, hit)) {
                                     ZX_LOG_WARN(LogCmd, "terrain.sculpt: no terrain at {} {}", x, z);
                                     ts.terrain = saved;
                                     return;
                                 }
                                 ed->undo.Begin("Sculpt Terrain");
                                 ed->undo.Record(terrain);
                                 if (ed->terrains.BeginEdit(ed->world, terrain)) {
                                     ts.flattenHeight = InverseAffine(scene::ComputeWorldMatrix(ed->world, terrain)).TransformPointAffine(hit).y;
                                     for (u32 i = 0; i < u32(seconds * 60.0f); ++i)
                                         TerrainDab(*ed, terrain, hit, 1.0f / 60.0f);
                                     ed->terrains.Commit(ed->world, terrain);
                                 }
                                 ed->undo.End();
                                 ts.terrain = saved;
                             });

/// The scene's first terrain (or the selected one).
ecs::Entity ConsoleTerrain(Editor& ed)
{
    if (ed.world.IsAlive(ed.selected) && ed.world.Has<Terrain>(ed.selected))
        return ed.selected;
    ecs::Entity found;
    ed.world.Query<const Terrain>().Each([&](ecs::Entity e, const Terrain&) {
        if (!found)
            found = e;
    });
    return found;
}

ConCommand CmdTerrainRamp("terrain.ramp", "Ramp between two ground points as wide as the radius: terrain.ramp <x0> <z0> <x1> <z1> [radius]",
                          [](Span<const StringView> args) {
                              Editor* ed = Ed();
                              if (!ed || args.Size() < 4)
                                  return;
                              const ecs::Entity t = ConsoleTerrain(*ed);
                              Vec3 p[2];
                              for (u32 i = 0; i < 2; ++i) {
                                  const f32 x = ParseFloat(args[i * 2]).ValueOr(0.0f), z = ParseFloat(args[i * 2 + 1]).ValueOr(0.0f);
                                  ecs::Entity hitTerrain;
                                  if (!t || !ed->terrains.Raycast(ed->world, Ray(Vec3(x, 5000.0f, z), Vec3(0, -1, 0)), 10000.0f, hitTerrain, p[i])) {
                                      ZX_LOG_WARN(LogCmd, "terrain.ramp: no terrain at {} {}", x, z);
                                      return;
                                  }
                              }
                              const f32 savedRadius = ed->tools.terrain.radius;
                              if (args.Size() > 4)
                                  ed->tools.terrain.radius = ParseFloat(args[4]).ValueOr(savedRadius);
                              TerrainRamp(*ed, t, p[0], p[1]);
                              ed->tools.terrain.radius = savedRadius;
                          });

ConCommand CmdTerrainOp("terrain.op", "Whole-terrain operation: terrain.op <flatten [height]|smooth|reset|clearpaint|slope|import <file> [range]|export <file>>",
                        [](Span<const StringView> args) {
                            Editor* ed = Ed();
                            if (!ed || args.Empty())
                                return;
                            const ecs::Entity t = ConsoleTerrain(*ed);
                            if (!t) {
                                ZX_LOG_WARN(LogCmd, "terrain.op: no terrain in the scene");
                                return;
                            }
                            const StringView op = args[0];
                            if (op.EqualsIgnoreCase("flatten")) {
                                const f32 saved = ed->tools.terrain.targetHeight;
                                if (args.Size() > 1)
                                    ed->tools.terrain.targetHeight = ParseFloat(args[1]).ValueOr(saved);
                                TerrainOperation(*ed, t, TerrainOp::FlattenAll);
                                ed->tools.terrain.targetHeight = saved;
                            } else if (op.EqualsIgnoreCase("smooth"))
                                TerrainOperation(*ed, t, TerrainOp::SmoothAll);
                            else if (op.EqualsIgnoreCase("reset"))
                                TerrainOperation(*ed, t, TerrainOp::ResetSculpt);
                            else if (op.EqualsIgnoreCase("clearpaint"))
                                TerrainOperation(*ed, t, TerrainOp::ClearPaint);
                            else if (op.EqualsIgnoreCase("slope"))
                                TerrainOperation(*ed, t, TerrainOp::PaintBySlope);
                            else if (op.EqualsIgnoreCase("import") && args.Size() > 1) {
                                if (!ImportTerrainHeightmap(*ed, t, args[1], args.Size() > 2 ? ParseFloat(args[2]).ValueOr(100.0f) : 100.0f))
                                    ZX_LOG_WARN(LogCmd, "terrain.op: can't import '{}'", args[1]);
                            } else if (op.EqualsIgnoreCase("export") && args.Size() > 1) {
                                if (!ExportTerrainHeightmap(*ed, t, args[1]))
                                    ZX_LOG_WARN(LogCmd, "terrain.op: can't export '{}'", args[1]);
                            } else
                                ZX_LOG_WARN(LogCmd, "terrain.op: unknown operation '{}'", op);
                        });

ConCommand CmdInputPress("input.press", "Presses a key for the game in Play (captures the game input): input.press <Tab|Enter|Escape|Space|Up|Down|Left|Right|A..Z|0..9> [frames]",
                          [](Span<const StringView> args) {
                              Editor* ed = Ed();
                              if (!ed || args.Empty())
                                  return;
                              static const struct
                              {
                                  const char* name;
                                  Key key;
                              } Named[] = {{"tab", Key::Tab},     {"enter", Key::Enter}, {"escape", Key::Escape}, {"space", Key::Space},
                                           {"up", Key::Up},       {"down", Key::Down},   {"left", Key::Left},     {"right", Key::Right},
                                           {"backspace", Key::Backspace}};
                              Key key = Key::Unknown;
                              for (const auto& n : Named)
                                  if (args[0].EqualsIgnoreCase(n.name))
                                      key = n.key;
                              if (key == Key::Unknown && args[0].Size() == 1) {
                                  const char c = args[0][0];
                                  if (c >= 'a' && c <= 'z')
                                      key = Key(u32(Key::A) + u32(c - 'a'));
                                  else if (c >= 'A' && c <= 'Z')
                                      key = Key(u32(Key::A) + u32(c - 'A'));
                                  else if (c >= '0' && c <= '9')
                                      key = Key(u32(Key::Num0) + u32(c - '0'));
                              }
                              if (key == Key::Unknown) {
                                  ZX_LOG_WARN(LogCmd, "input.press: unknown key '{}'", args[0]);
                                  return;
                              }
                              ed->SetGameInputCaptured(true);
                              ed->simulatedKey = key;
                              ed->simulatedFrames = args.Size() > 1 ? u32(Max<i64>(1, ParseInt(args[1]).ValueOr(2))) : 2u;
                          });

ConCommand CmdLang("lang", "Game UI language: lang (lists them) / lang <code> (en, ru, ...) - previews it in the viewport",
                   [](Span<const StringView> args) {
                       Editor* ed = Ed();
                       if (!ed)
                           return;
                       if (args.Empty()) {
                           for (const Localization::Language& l : ed->localization.Languages())
                               ZX_LOG_INFO(LogCmd, "{} {} ({}){}", l.code == ed->localization.Current() ? ">" : " ", l.code, l.name,
                                           String::Format(", {} texts", l.texts.Size()));
                           return;
                       }
                       if (!ed->localization.SetLanguage(args[0]))
                           ZX_LOG_WARN(LogCmd, "lang: no table for '{}' (Content/Localization/{}.zloc)", args[0], args[0]);
                   });

ConCommand CmdGraphCode("graph.code", "Shows the ZeqouScript a script graph compiles to: graph.code <Content path .zgraph>",
                         [](Span<const StringView> args) {
                             Editor* ed = Ed();
                             if (!ed || args.Empty())
                                 return;
                             String text;
                             Graph graph;
                             String error;
                             if (!fs::ReadTextFile(Path::Join(ed->contentRoot, args[0]), text) || !ParseGraph(text, graph, &error)) {
                                 ZX_LOG_WARN(LogCmd, "graph.code: can't read '{}' {}", args[0], error);
                                 return;
                             }
                             GraphCompileResult r;
                             CompileGraph(graph, r);
                             for (const String& e : r.errors)
                                 ZX_LOG_ERROR(LogCmd, "{}", e);
                             for (const String& w : r.warnings)
                                 ZX_LOG_WARN(LogCmd, "{}", w);
                             ZX_LOG_INFO(LogCmd, "{}", r.source);
                         });

ConCommand CmdScriptAttach("script.attach", "Puts a script (.zs) or script graph (.zgraph) on the selected entity: script.attach <Content path>",
                            [](Span<const StringView> args) {
                                Editor* ed = Ed();
                                if (!ed || args.Empty() || !ed->world.IsAlive(ed->selected)) {
                                    ZX_LOG_WARN(LogCmd, "script.attach: select an entity first");
                                    return;
                                }
                                ed->undo.Begin("Add Script");
                                ed->undo.Record(ed->selected);
                                ed->world.Set(ed->selected, Script{String(args[0]), String()});
                                ed->undo.End();
                            });

ConCommand CmdGraphEdit("graph.edit", "Opens a script graph in the graph editor: graph.edit <Content path .zgraph>", [](Span<const StringView> args) {
    if (Editor* ed = Ed(); ed && !args.Empty())
        OpenZGraphEditor(*ed, Path::Join(ed->contentRoot, args[0]));
});

ConCommand CmdGraphBreak("graph.break", "Toggles a breakpoint on a node of the open script graph: graph.break <node id>", [](Span<const StringView> args) {
    Editor* ed = Ed();
    if (!ed || args.Empty() || !ed->graphEditor.open)
        return;
    const u32 id = u32(ParseInt(args[0]).ValueOr(0));
    Array<u32>& b = ed->graphEditor.breakpoints;
    if (b.Contains(id))
        b.Remove(id);
    else
        b.PushBack(id);
});

ConCommand CmdGraphNew("graph.new", "Writes an example script graph (a spinner): graph.new <Content path .zgraph>", [](Span<const StringView> args) {
    Editor* ed = Ed();
    if (!ed || args.Empty())
        return;
    if (fs::WriteTextFile(Path::Join(ed->contentRoot, args[0]), WriteGraph(MakeExampleGraph())))
        ZX_LOG_INFO(LogCmd, "Wrote {}", args[0]);
});

ConCommand CmdUndo("undo", "Undoes the last scene edit", [](Span<const StringView>) {
    if (Ed())
        Ed()->Undo();
});

ConCommand CmdRedo("redo", "Redoes the last undone scene edit", [](Span<const StringView>) {
    if (Ed())
        Ed()->Redo();
});

ConCommand CmdSave("scene.save", "Saves the current scene", [](Span<const StringView>) {
    if (Ed())
        Ed()->SaveScene(false);
});

ConCommand CmdNew("scene.new", "Starts a new scene (asks to save changes)", [](Span<const StringView>) {
    if (Ed())
        Ed()->RequestSceneAction(SceneAction::New);
});

ConCommand CmdOpen("scene.open", "Opens a scene: scene.open <path relative to Content or absolute>", [](Span<const StringView> args) {
    Editor* ed = Ed();
    if (!ed)
        return;
    if (args.Empty()) {
        ed->RequestSceneAction(SceneAction::Open);
        return;
    }
    ed->RequestSceneAction(SceneAction::Open, Path::IsAbsolute(args[0]) ? String(args[0]) : Path::Join(ed->contentRoot, args[0]));
});

ConCommand CmdSelect("select", "Selects an entity by name: select <name>", [](Span<const StringView> args) {
    if (!Ed() || args.Empty())
        return;
    const ecs::Entity e = FindNamed(args[0]);
    if (!e)
        ZX_LOG_WARN(LogCmd, "No entity named '{}'", args[0]);
    Ed()->Select(e);
});

ConCommand CmdFocus("focus", "Moves the camera to the selection (or to: focus <name>)", [](Span<const StringView> args) {
    if (!Ed())
        return;
    if (!args.Empty())
        Ed()->Select(FindNamed(args[0]));
    Ed()->FocusSelected();
});

ConCommand CmdImport("import", "Imports a model into the project and places it in front of the camera (or at x y z): import <path> [x y z]",
                     [](Span<const StringView> args) {
                         Editor* ed = Ed();
                         if (!ed || args.Empty())
                             return;
                         const Vec3 at = args.Size() >= 4 ? Vec3(ParseFloat(args[1]).ValueOr(0.0f), ParseFloat(args[2]).ValueOr(0.0f), ParseFloat(args[3]).ValueOr(0.0f))
                                                          : ed->camera.position + ed->camera.Forward() * 5.0f;
                         // Paths under Content may be given relative to it (Packs/StylizedNature/Pine.gltf).
                         const String underContent = Path::Join(ed->contentRoot, args[0]);
                         ed->ImportAsset(!Path::IsAbsolute(args[0]) && platform::FileExists(underContent) ? underContent : String(args[0]), at);
                     });

ConCommand CmdEngineProfile("profile.switch",
                            "Engine profile for the next start: profile.switch [lite|full|auto] [restart] (no args prints it)",
                            [](Span<const StringView> args) {
                                Editor* ed = Ed();
                                if (!ed)
                                    return;
                                if (args.Empty()) {
                                    ZX_LOG_INFO(LogCmd, "Engine profile: {} (recommended {}, next start: {})", ed->lite ? "Lite" : "Full",
                                                EngineProfileName(RecommendedEngineProfile(ed->device->GetCaps())),
                                                LoadEngineProfilePreference());
                                    return;
                                }
                                if (args[0] != "lite" && args[0] != "full" && args[0] != "auto") {
                                    ZX_LOG_WARN(LogCmd, "profile.switch: expected lite, full or auto");
                                    return;
                                }
                                SaveEngineProfilePreference(args[0]);
                                ZX_LOG_INFO(LogCmd, "Engine profile '{}' applies after a restart", args[0]);
                                if (args.Size() > 1 && args[1] == "restart")
                                    ed->RequestRestart();
                            });

ConCommand CmdAudioPlay("audio.play", "Plays a sound file once (path under Content): audio.play Sounds/x.wav", [](Span<const StringView> args) {
    Editor* ed = Ed();
    if (ed && ed->audio && !args.Empty())
        ed->audio->TogglePreview(args[0]);
});

ConCommand CmdAudioStats("audio.stats", "Prints the audio device and mixer state", [](Span<const StringView>) {
    Editor* ed = Ed();
    if (!ed || !ed->audioEngine)
        return;
    const audio::AudioStats s = ed->audioEngine->GetStats();
    ZX_LOG_INFO(LogCmd, "Audio: device '{}', {} voices ({} audible, {} virtual, {} streams), {} underruns, mix {:.3f} ms",
                s.deviceOpen ? s.deviceName : String("none"), s.voices, s.audible, s.virtualVoices, s.streams, s.underruns, s.mixCpuMs);
});

ConCommand CmdCamera("camera", "Moves the editor camera: camera <x> <y> <z> [yaw deg] [pitch deg] (no arguments: prints it)",
                     [](Span<const StringView> args) {
    Editor* ed = Ed();
    if (!ed)
        return;
    if (args.Size() >= 3) {
        ed->camera.position = Vec3(ParseFloat(args[0]).ValueOr(0.0f), ParseFloat(args[1]).ValueOr(0.0f), ParseFloat(args[2]).ValueOr(0.0f));
        if (args.Size() >= 4)
            ed->camera.yaw = DegToRad(ParseFloat(args[3]).ValueOr(0.0f));
        if (args.Size() >= 5)
            ed->camera.pitch = DegToRad(ParseFloat(args[4]).ValueOr(0.0f));
        ed->viewport->ResetHistory();
    }
    ZX_LOG_INFO(LogCmd, "camera {:.2f} {:.2f} {:.2f} {:.1f} {:.1f}", ed->camera.position.x, ed->camera.position.y, ed->camera.position.z,
                RadToDeg(ed->camera.yaw), RadToDeg(ed->camera.pitch));
});

ConCommand CmdHdriInfo("hdri.info", "HDRI Sky: panorama, intensity, the sun it lights the scene with", [](Span<const StringView>) {
    Editor* ed = Ed();
    if (!ed)
        return;
    const render::HdriSkySettings& h = ed->viewport->LastHdri();
    if (!h.IsActive()) {
        ZX_LOG_INFO(LogCmd, "HDRI Sky: inactive (physical sky)");
        return;
    }
    const render::SceneLighting& l = ed->viewport->Lighting();
    ZX_LOG_INFO(LogCmd,
                "HDRI Sky: {} px wide, {:.0f} nits per unit, rotation {:.1f} deg; sky {:.0f} nits average; sun {}{:.0f} lx, color ({:.2f}, {:.2f}, "
                "{:.2f}), from ({:.2f}, {:.2f}, {:.2f})",
                h.width, h.intensity, RadToDeg(h.rotation), l.skyLuminance, ed->viewport->HdriSunFound() ? "found, " : "", l.sunIlluminance,
                l.sunColor.x, l.sunColor.y, l.sunColor.z, -l.sunDirection.x, -l.sunDirection.y, -l.sunDirection.z);
});

ConCommand CmdHdriSet("hdri.set", "Edits the HDRI Sky: hdri.set rotation|intensity <value>, hdri.set sun FromHDRI|SceneSun|None",
                      [](Span<const StringView> args) {
    Editor* ed = Ed();
    if (!ed || args.Size() < 2)
        return;
    HdriSky* sky = nullptr;
    ed->world.Query<HdriSky>().Each([&](HdriSky& s) {
        if (!sky)
            sky = &s;
    });
    if (!sky) {
        ZX_LOG_WARN(LogCmd, "No HDRI Sky in the scene (create hdrisky <panorama>)");
        return;
    }
    if (args[0].EqualsIgnoreCase("rotation"))
        sky->rotation = ParseFloat(args[1]).ValueOr(sky->rotation);
    else if (args[0].EqualsIgnoreCase("intensity"))
        sky->intensity = ParseFloat(args[1]).ValueOr(sky->intensity);
    else if (args[0].EqualsIgnoreCase("sun"))
        sky->sun = args[1].EqualsIgnoreCase("None") ? HdriSunMode::None
                   : args[1].EqualsIgnoreCase("SceneSun") ? HdriSunMode::SceneSun
                                                            : HdriSunMode::FromHDRI;
    else
        ZX_LOG_WARN(LogCmd, "Unknown HDRI Sky field '{}' (rotation, intensity, sun)", args[0]);
});

ConCommand CmdPhysicsSpawn("physics.spawn", "Drops Rigid Body cubes and spheres in front of the camera: physics.spawn [count]",
                           [](Span<const StringView> args) {
    Editor* ed = Ed();
    if (!ed || ed->IsPlaying())
        return;
    const u32 count = Clamp(u32(args.Empty() ? 20 : ParseInt(args[0]).ValueOr(20)), 1u, 2000u);
    const Vec3 forward = SafeNormalize(Vec3(ed->camera.Forward().x, 0.0f, ed->camera.Forward().z), Vec3(0, 0, 1));
    const Vec3 base = ed->camera.position + forward * 6.0f;
    const u32 side = Max(1u, u32(Sqrt(f32(count) / 4.0f) + 0.5f));
    for (u32 i = 0; i < count; ++i) {
        const u32 layer = i / (side * side), cell = i % (side * side);
        const Vec3 p = base + Vec3((f32(cell % side) - f32(side) * 0.5f) * 1.2f + (layer & 1) * 0.3f, 2.0f + f32(layer) * 1.2f,
                                   (f32(cell / side) - f32(side) * 0.5f) * 1.2f);
        ed->CreateShape(i % 3 == 2 ? BuiltinShape::Sphere : BuiltinShape::Cube, p);
        ed->world.Add(ed->selected, RigidBody{});
    }
    ed->Select(ecs::NullEntity);
    ZX_LOG_INFO(LogCmd, "Spawned {} rigid bodies (press Play)", count);
});

ConCommand CmdPhysicsStats("physics.stats", "Physics while playing: bodies, awake, contacts, step time", [](Span<const StringView>) {
    Editor* ed = Ed();
    if (!ed || !ed->physics.IsRunning()) {
        ZX_LOG_INFO(LogCmd, "Physics runs while playing");
        return;
    }
    const physics::PhysicsStats& s = ed->physics.World()->Stats();
    ZX_LOG_INFO(LogCmd, "Physics: {} bodies ({} dynamic, {} awake), {} pairs, {} contacts ({} points), {} islands, step {:.2f} ms", s.bodies, s.dynamic,
                s.awake, s.pairs, s.contacts, s.points, s.islands, s.stepMs);
});

ConCommand CmdRecapture("reflections.recapture", "Recaptures every reflection probe", [](Span<const StringView>) {
    if (Editor* ed = Ed())
        ed->viewport->Reflections().RecaptureAll();
});

ConCommand CmdReflections("reflections.stats", "Reflection probes: count, captured, capture in progress", [](Span<const StringView>) {
    if (Editor* ed = Ed()) {
        const render::ReflectionRenderer& r = ed->viewport->Reflections();
        ZX_LOG_INFO(LogCmd, "Reflection probes: {} ({} captured{}), SSR {}", r.ProbeCount(), r.CapturedProbes(),
                    r.IsCapturing() ? ", capturing" : "", ed->viewport->Settings().reflections.ssr ? "on" : "off");
    }
});

ConCommand CmdMixer("mixer", "Opens the Audio Mixer", [](Span<const StringView>) {
    if (Editor* ed = Ed())
        ed->showMixer = true;
});

ConCommand CmdFeatures("features", "Opens the Features window (engine features and tools per project)", [](Span<const StringView>) {
    if (Editor* ed = Ed())
        ed->showFeatures = true;
});

ConCommand CmdTool("tool", "Activates an editor tool: tool <brush|measure|terrain|ui|none>; tool drop = Drop to Ground", [](Span<const StringView> args) {
    Editor* ed = Ed();
    if (!ed || args.Empty())
        return;
    if (args[0].EqualsIgnoreCase("drop")) {
        DropSelectedToGround(*ed);
        return;
    }
    const EditorTool tool = args[0].EqualsIgnoreCase("brush")     ? EditorTool::Brush
                            : args[0].EqualsIgnoreCase("measure") ? EditorTool::Measure
                            : args[0].EqualsIgnoreCase("terrain") ? EditorTool::Terrain
                            : args[0].EqualsIgnoreCase("ui")      ? EditorTool::UIDesigner
                                                                  : EditorTool::None;
    ed->tools.active = EditorTool::None;
    if (tool != EditorTool::None)
        SetActiveTool(*ed, tool);
});

ConCommand CmdCreate("create", "Creates an object in front of the camera: create <entity|pointlight|spotlight|water|audiosource|reverbzone|decal|terrain|heightfog [density]|fogvolume|sun|skylight|clouds|fluidfire|fluidsmoke|cube|sphere|cylinder|plane>",
                     [](Span<const StringView> args) {
    Editor* ed = Ed();
    if (!ed)
        return;
    const StringView kind = args.Empty() ? StringView("entity") : args[0];
    const Vec3 at = ed->camera.position + ed->camera.Forward() * 5.0f;
    if (kind.EqualsIgnoreCase("pointlight"))
        ed->CreatePointLight(at);
    else if (kind.EqualsIgnoreCase("spotlight"))
        ed->CreateSpotLight(at);
    else if (kind.EqualsIgnoreCase("water"))
        ed->CreateWater(at);
    else if (kind.EqualsIgnoreCase("audiosource"))
        ed->CreateAudioSource(at);
    else if (kind.EqualsIgnoreCase("reverbzone"))
        ed->CreateReverbZone(at);
    else if (kind.EqualsIgnoreCase("postvolume") || kind.EqualsIgnoreCase("postprocessvolume"))
        ed->CreatePostProcessVolume(at);
    else if (kind.EqualsIgnoreCase("reflectionprobe") || kind.EqualsIgnoreCase("probe"))
        ed->CreateReflectionProbe(at);
    else if (kind.EqualsIgnoreCase("decal"))
        ed->CreateDecal(at);
    else if (kind.EqualsIgnoreCase("terrain"))
        ed->CreateTerrain(args.Size() > 1 && args[1].EqualsIgnoreCase("flat"));
    else if (kind.EqualsIgnoreCase("wind") || kind.EqualsIgnoreCase("windzone"))
        ed->CreateWindZone();
    else if (kind.StartsWith("ui."))
        ed->CreateUIWidget(kind.SubStr(3));
    else if (kind.EqualsIgnoreCase("heightfog") || kind.EqualsIgnoreCase("fog"))
        ed->CreateHeightFog(args.Size() > 1 ? ParseFloat(args[1]).ValueOr(0.0f) : 0.0f);
    else if (kind.EqualsIgnoreCase("fogvolume"))
        ed->CreateFogVolume(at);
    else if (kind.EqualsIgnoreCase("clouds"))
        ed->CreateClouds();
    else if (kind.EqualsIgnoreCase("sun") || kind.EqualsIgnoreCase("directionallight"))
        ed->CreateSun();
    else if (kind.EqualsIgnoreCase("skylight"))
        ed->CreateSkyLight();
    else if (kind.EqualsIgnoreCase("fluidfire") || kind.EqualsIgnoreCase("fluidsmoke"))
        ed->CreateFluid(at, kind.EqualsIgnoreCase("fluidfire"));
    else if (kind.EqualsIgnoreCase("hdrisky") || kind.EqualsIgnoreCase("hdri"))
        ed->UseHdriSky(args.Size() > 1 ? args[1] : StringView());
    else if (kind.EqualsIgnoreCase("entity"))
        ed->CreateEmpty(at);
    else if (kind.EqualsIgnoreCase("cube"))
        ed->CreateShape(BuiltinShape::Cube, at);
    else if (kind.EqualsIgnoreCase("sphere"))
        ed->CreateShape(BuiltinShape::Sphere, at);
    else if (kind.EqualsIgnoreCase("cylinder"))
        ed->CreateShape(BuiltinShape::Cylinder, at);
    else if (kind.EqualsIgnoreCase("plane"))
        ed->CreateShape(BuiltinShape::Plane, at);
    else
        ZX_LOG_WARN(LogCmd, "Unknown object type '{}' (entity, pointlight, spotlight)", kind);
});

ConCommand CmdPlay("play", "Starts Play-in-Editor", [](Span<const StringView>) {
    if (Ed())
        Ed()->BeginPlay();
});

ConCommand CmdStop("stop", "Stops Play-in-Editor and restores the scene", [](Span<const StringView>) {
    if (Ed())
        Ed()->EndPlay();
});

ConCommand CmdPause("pause", "Pauses / resumes Play-in-Editor", [](Span<const StringView>) {
    if (Ed())
        Ed()->TogglePause();
});

ConCommand CmdStep("step", "Advances the game by one frame (starts paused play if needed)", [](Span<const StringView>) {
    if (Ed())
        Ed()->StepFrame();
});

ConCommand CmdPrefabCreate("prefab.create", "Makes the selection a prefab: prefab.create <name> (saved to Content/Prefabs)",
                            [](Span<const StringView> args) {
                                Editor* ed = Ed();
                                if (!ed || args.Empty() || !ed->world.IsAlive(ed->selected))
                                    return;
                                ed->CreatePrefabAt(ed->selected, String::Format("{}/Prefabs/{}.zprefab", ed->contentRoot, args[0]));
                            });

ConCommand CmdPrefabPlace("prefab.place", "Places a prefab in front of the camera: prefab.place <path under Content>",
                           [](Span<const StringView> args) {
                               Editor* ed = Ed();
                               if (!ed || args.Empty())
                                   return;
                               ed->InstantiatePrefab(Path::Join(ed->contentRoot, args[0]), ed->camera.position + ed->camera.Forward() * 5.0f);
                           });

ConCommand CmdPrefabApply("prefab.apply", "Applies the selected instance's overrides to its prefab", [](Span<const StringView>) {
    if (Ed())
        Ed()->ApplyPrefab(Ed()->selected);
});

ConCommand CmdPrefabOverrides("prefab.overrides", "Lists prefab overrides (Component.field) of the selection, or of every prefab entity: prefab.overrides [all]",
                               [](Span<const StringView> args) {
    Editor* ed = Ed();
    if (!ed || !ed->prefabs)
        return;
    Array<ecs::Entity> targets;
    if (!args.Empty() && args[0].EqualsIgnoreCase("all"))
        ed->world.Query<const scene::EntityName>().Each([&](ecs::Entity e, const scene::EntityName&) {
            if (FindPrefabInstanceRoot(ed->world, e))
                targets.PushBack(e);
        });
    else if (ed->world.IsAlive(ed->selected))
        targets.PushBack(ed->selected);
    for (ecs::Entity e : targets) {
        Array<String> overrides;
        GetPrefabOverrides(ed->world, e, *ed->prefabs, overrides);
        if (overrides.Empty() && targets.Size() > 1)
            continue;
        const scene::EntityName* n = ed->world.Get<const scene::EntityName>(e);
        ZX_LOG_INFO(LogCmd, "{}: {} override(s)", n ? n->value.View() : StringView("?"), overrides.Size());
        for (const String& o : overrides)
            ZX_LOG_INFO(LogCmd, "  {}", o);
    }
});

ConCommand CmdPrefabRevert("prefab.revert", "Reverts the selected instance to its prefab", [](Span<const StringView>) {
    if (Ed())
        Ed()->RevertPrefab(Ed()->selected);
});

ConCommand CmdPrefabUnpack("prefab.unpack", "Turns the selected instance back into plain entities", [](Span<const StringView>) {
    if (Ed())
        Ed()->UnpackPrefab(Ed()->selected);
});

ConCommand CmdMove("move", "Sets the selection's local position: move <x> <y> <z>", [](Span<const StringView> args) {
    Editor* ed = Ed();
    if (!ed || args.Size() < 3 || !ed->world.Has<Transform>(ed->selected))
        return;
    ed->undo.Begin("Move");
    ed->undo.Record(ed->selected);
    ed->world.Get<Transform>(ed->selected)->position =
        Vec3(ParseFloat(args[0]).ValueOr(0.0f), ParseFloat(args[1]).ValueOr(0.0f), ParseFloat(args[2]).ValueOr(0.0f));
    ed->undo.End();
});

MaterialLibrary::Material* FindMaterial(StringView name)
{
    if (!Ed() || !Ed()->materials)
        return nullptr;
    for (const UniquePtr<MaterialLibrary::Material>& m : Ed()->materials->All())
        if (Path::Stem(m->path).EqualsIgnoreCase(name))
            return m.Get();
    ZX_LOG_WARN(LogCmd, "No material named '{}'", name);
    return nullptr;
}

ConCommand CmdMaterialCreate("material.create", "Creates a material graph: material.create <name> (Content/Materials)",
                              [](Span<const StringView> args) {
                                  if (Ed() && !args.Empty())
                                      Ed()->CreateMaterial(String::Format("{}/Materials/{}.zmat", Ed()->contentRoot, args[0]));
                              });

ConCommand CmdMaterialAssign("material.assign", "Assigns a material to the selected mesh: material.assign <name>",
                              [](Span<const StringView> args) {
                                  if (args.Empty())
                                      return;
                                  if (MaterialLibrary::Material* m = FindMaterial(args[0]))
                                      Ed()->AssignMaterial(Ed()->selected, m->guid);
                              });

ConCommand CmdMaterialInstance("material.instance", "Creates an instance of a material: material.instance <parent> <name>",
                                [](Span<const StringView> args) {
                                    if (args.Size() < 2)
                                        return;
                                    if (MaterialLibrary::Material* m = FindMaterial(args[0]))
                                        Ed()->materials->CreateInstance(
                                            m->guid, String::Format("{}/Materials/{}.zmati", Ed()->contentRoot, args[1]));
                                });

ConCommand CmdMaterialEdit("material.edit", "Opens a material in the Material Editor: material.edit <name>", [](Span<const StringView> args) {
    if (args.Empty())
        return;
    if (MaterialLibrary::Material* m = FindMaterial(args[0]))
        OpenMaterialEditor(*Ed(), m->guid);
});

ConCommand CmdMaterialCode("material.code", "Prints the HLSL generated for a material graph: material.code <name>",
                            [](Span<const StringView> args) {
                                if (args.Empty())
                                    return;
                                if (MaterialLibrary::Material* m = FindMaterial(args[0])) {
                                    StringView rest = m->compiled.code.View();
                                    while (!rest.Empty()) {
                                        StringView line;
                                        rest.Split('\n', line, rest);
                                        ZX_LOG_INFO(LogCmd, "{}", line);
                                    }
                                }
                            });

ConCommand CmdMaterialSet("material.set", "Sets a named value node: material.set <material> <node name> <x> [y z w]",
                           [](Span<const StringView> args) {
                               if (args.Size() < 3)
                                   return;
                               MaterialLibrary::Material* m = FindMaterial(args[0]);
                               if (!m)
                                   return;
                               auto squash = [](StringView v) {
                                   String out;
                                   for (char c : v)
                                       if (c != ' ' && c != '_')
                                           out += c;
                                   return out;
                               };
                               for (render::MaterialGraphNode& n : m->graph.nodes)
                                   if (!n.name.Empty() && squash(n.name).View().EqualsIgnoreCase(squash(args[1]))) {
                                       f32 v[4] = {n.value.x, n.value.y, n.value.z, n.value.w};
                                       for (usize i = 2; i < args.Size() && i < 6; ++i)
                                           v[i - 2] = ParseFloat(args[i]).ValueOr(v[i - 2]);
                                       n.value = Vec4(v[0], v[1], v[2], v[3]);
                                       m->dirty = true;
                                       if (m->IsInstance()) {
                                           JsonValue o = JsonValue::MakeObject();
                                           JsonValue a = JsonValue::MakeArray();
                                           for (f32 x : v)
                                               a.PushBack(x);
                                           o.Set("value", move(a));
                                           m->overrides.Set(n.name, move(o));
                                       }
                                       Ed()->materials->Apply(m->guid);
                                       return;
                                   }
                               ZX_LOG_WARN(LogCmd, "Material '{}' has no node named '{}'", args[0], args[1]);
                           });

ConCommand CmdPalette("palette", "Opens the command palette: palette [query]", [](Span<const StringView> args) {
    if (!Ed())
        return;
    OpenCommandPalette(*Ed(), false);
    for (usize i = 0; i < args.Size(); ++i)
        Ed()->palette.query += i ? String::Format(" {}", args[i]) : String(args[i]);
});

ConCommand CmdAction("action", "Runs an editor action by id (as in Keys.ini): action play.eject", [](Span<const StringView> args) {
    if (!Ed() || args.Empty())
        return;
    if (!RunAction(*Ed(), args[0]))
        ZX_LOG_WARN(LogCmd, "Unknown or unavailable action '{}'", args[0]);
});

ConCommand CmdResetLayout("editor.ResetLayout", "Restores the default panel layout", [](Span<const StringView>) {
    if (Ed())
        Ed()->requestResetLayout = true;
});

ConCommand CmdProject("project.info", "Prints the open project's paths", [](Span<const StringView>) {
    Editor* ed = Ed();
    if (!ed)
        return;
    ZX_LOG_INFO(LogCmd, "  Project {} ({})", ed->project.descriptor.name, ed->project.descriptorPath);
    ZX_LOG_INFO(LogCmd, "  Content {}", ed->contentRoot);
    ZX_LOG_INFO(LogCmd, "  Scene   {}", ed->scenePath.Empty() ? String("(unsaved)") : ed->scenePath);
});

/// Initializes the CVar from the current setting, then forwards console changes to `apply`. A value set with a higher
/// priority before (e.g. +r.TAA=0 on the command line) wins and is applied right away.
template <typename T>
void Bind(CVar<T>& cvar, T current, void (*apply)(T))
{
    cvar.SetOnChanged(nullptr);
    if (!cvar.Set(current, CVarSetBy::Code))
        apply(cvar.Get());
    cvar.SetOnChanged([&cvar, apply](CVarBase&) {
        if (Ed())
            apply(cvar.Get());
    });
}

} // namespace

void RegisterEditorConsole(Editor& ed)
{
    g_editor = &ed;
    ViewportSettings& s = ed.viewport->Settings();
    Bind<f32>(CVarScreenPercentage, s.renderScale * 100.0f, [](f32 v) {
        Ed()->viewport->Settings().renderScale = Clamp(v, 25.0f, 200.0f) / 100.0f;
        Ed()->viewport->ResetHistory();
    });
    Bind<bool>(CVarTAA, s.taa.enabled, [](bool v) {
        Ed()->viewport->Settings().taa.enabled = v;
        Ed()->viewport->ResetHistory();
    });
    Bind<bool>(CVarAO, s.scene.ao.enabled, [](bool v) { Ed()->viewport->Settings().scene.ao.enabled = v; });
    Bind<bool>(CVarShadows, s.scene.shadows.enabled, [](bool v) { Ed()->viewport->Settings().scene.shadows.enabled = v; });
    Bind<bool>(CVarSky, s.useSky, [](bool v) { Ed()->viewport->Settings().useSky = v; });
    Bind<bool>(CVarBloom, s.post.bloom, [](bool v) { Ed()->viewport->Settings().post.bloom = v; });
    Bind<bool>(CVarTSR, s.taa.upscale, [](bool v) {
        Ed()->viewport->Settings().taa.upscale = v;
        Ed()->viewport->ResetHistory();
    });
    Bind<bool>(CVarDynRes, s.dynamicResolution.enabled, [](bool v) { Ed()->viewport->Settings().dynamicResolution.enabled = v; });
    Bind<f32>(CVarDynResTarget, s.dynamicResolution.targetMs,
              [](f32 v) { Ed()->viewport->Settings().dynamicResolution.targetMs = Clamp(v, 4.0f, 100.0f); });
    Bind<f32>(CVarDynResMin, s.dynamicResolution.minScale * 100.0f,
              [](f32 v) { Ed()->viewport->Settings().dynamicResolution.minScale = Clamp(v, 25.0f, 100.0f) / 100.0f; });
    Bind<bool>(CVarSSR, s.reflections.ssr, [](bool v) { Ed()->viewport->Settings().reflections.ssr = v; });
    Bind<bool>(CVarGpuDriven, s.scene.gpuDriven, [](bool v) { Ed()->viewport->Settings().scene.gpuDriven = v; });
    Bind<bool>(CVarVsm, s.scene.shadows.virtualShadows, [](bool v) { Ed()->viewport->Settings().scene.shadows.virtualShadows = v; });
    Bind<f32>(CVarVsmNormalBias, s.scene.shadows.vsmNormalBias, [](f32 v) { Ed()->viewport->Settings().scene.shadows.vsmNormalBias = Clamp(v, 0.0f, 16.0f); });
    Bind<f32>(CVarVsmDistance, s.scene.shadows.vsmDistance, [](f32 v) { Ed()->viewport->Settings().scene.shadows.vsmDistance = Clamp(v, 5.0f, 2000.0f); });
    Bind<f32>(CVarVsmResolution, s.scene.shadows.vsmResolution, [](f32 v) { Ed()->viewport->Settings().scene.shadows.vsmResolution = Clamp(v, 0.125f, 4.0f); });
    Bind<f32>(CVarNanoShadowError, s.scene.nano.shadowPixelError, [](f32 v) { Ed()->viewport->Settings().scene.nano.shadowPixelError = Clamp(v, 0.25f, 32.0f); });
    Bind<bool>(CVarNano, s.scene.nano.enabled, [](bool v) { Ed()->viewport->Settings().scene.nano.enabled = v; });
    Bind<f32>(CVarNanoPixelError, s.scene.nano.pixelError,
              [](f32 v) { Ed()->viewport->Settings().scene.nano.pixelError = Clamp(v, 0.25f, 32.0f); });
    Bind<i32>(CVarNanoBudget, i32(s.scene.nano.triangleBudget / 1000),
              [](i32 v) { Ed()->viewport->Settings().scene.nano.triangleBudget = u32(Clamp(v, 100, 20000)) * 1000u; });
    Bind<bool>(CVarOcclusion, s.scene.occlusionCulling, [](bool v) { Ed()->viewport->Settings().scene.occlusionCulling = v; });
    Bind<i32>(CVarShadowFilter, i32(s.scene.shadows.filter), [](i32 v) { Ed()->viewport->Settings().scene.shadows.filter = u32(Clamp(v, 0, 3)); });
    Bind<bool>(CVarContactShadows, s.scene.contactShadows.enabled, [](bool v) { Ed()->viewport->Settings().scene.contactShadows.enabled = v; });
    Bind<i32>(CVarRendererPath, s.scene.deferred ? 1 : 0, [](i32 v) { Ed()->viewport->Settings().scene.deferred = v == 1; });
    Bind<i32>(CVarGIMode, i32(s.gi.mode), [](i32 v) { Ed()->viewport->Settings().gi.mode = v == 1 ? render::GIMode::Baked : render::GIMode::Dynamic; });
    Bind<bool>(CVarGIScreen, s.gi.screenSpace, [](bool v) { Ed()->viewport->Settings().gi.screenSpace = v; });
    Bind<bool>(CVarGILumen, s.gi.lumen, [](bool v) { Ed()->viewport->Settings().gi.lumen = v; });
    Bind<bool>(CVarGI, s.gi.enabled, [](bool v) { Ed()->viewport->Settings().gi.enabled = v; });
    Bind<f32>(CVarGIIntensity, s.gi.intensity, [](f32 v) { Ed()->viewport->Settings().gi.intensity = Clamp(v, 0.0f, 10.0f); });
    Bind<i32>(CVarGIRays, i32(s.gi.raysPerProbe), [](i32 v) { Ed()->viewport->Settings().gi.raysPerProbe = u32(Clamp(v, 16, 256)); });
    Bind<i32>(CVarGIStride, i32(s.gi.probeStride), [](i32 v) { Ed()->viewport->Settings().gi.probeStride = u32(Clamp(v, 1, 16)); });
    Bind<f32>(CVarGISpacing, s.gi.probeSpacing, [](f32 v) { Ed()->viewport->Settings().gi.probeSpacing = Clamp(v, 0.25f, 16.0f); });
    Bind<bool>(CVarProbes, s.reflections.probes, [](bool v) {
        Ed()->viewport->Settings().reflections.probes = v;
        Ed()->viewport->Settings().reflections.sky = v;
    });
    Bind<f32>(CVarExposure, s.post.exposureCompensation, [](f32 v) { Ed()->viewport->Settings().post.exposureCompensation = v; });
    Bind<i32>(CVarViewMode, i32(s.scene.debugMode), [](i32 v) { Ed()->viewport->Settings().scene.debugMode = u32(Clamp(v, 0, 10)); });
    Bind<i32>(CVarForceLod, s.scene.forcedLod, [](i32 v) { Ed()->viewport->Settings().scene.forcedLod = v; });
    Bind<f32>(CVarCameraSpeed, ed.camera.moveSpeed, [](f32 v) { Ed()->camera.moveSpeed = Clamp(v, 0.1f, 500.0f); });
    Bind<bool>(CVarShowGrid, ed.showGrid, [](bool v) { Ed()->showGrid = v; });
    Bind<bool>(CVarUISounds, ed.uiSounds, [](bool v) { Ed()->uiSounds = v; });
}

void UnregisterEditorConsole()
{
    g_editor = nullptr;
    for (CVarBase* c : {static_cast<CVarBase*>(&CVarScreenPercentage), static_cast<CVarBase*>(&CVarTAA), static_cast<CVarBase*>(&CVarAO),
                        static_cast<CVarBase*>(&CVarShadows), static_cast<CVarBase*>(&CVarSky), static_cast<CVarBase*>(&CVarBloom), static_cast<CVarBase*>(&CVarSSR), static_cast<CVarBase*>(&CVarTSR),
                        static_cast<CVarBase*>(&CVarDynRes), static_cast<CVarBase*>(&CVarDynResTarget), static_cast<CVarBase*>(&CVarDynResMin), static_cast<CVarBase*>(&CVarProbes),
                        static_cast<CVarBase*>(&CVarGpuDriven), static_cast<CVarBase*>(&CVarRendererPath), static_cast<CVarBase*>(&CVarShadowFilter), static_cast<CVarBase*>(&CVarContactShadows), static_cast<CVarBase*>(&CVarOcclusion), static_cast<CVarBase*>(&CVarUISounds),
                        static_cast<CVarBase*>(&CVarExposure), static_cast<CVarBase*>(&CVarViewMode), static_cast<CVarBase*>(&CVarGI), static_cast<CVarBase*>(&CVarGIMode), static_cast<CVarBase*>(&CVarGIScreen), static_cast<CVarBase*>(&CVarGILumen),
                        static_cast<CVarBase*>(&CVarGIIntensity), static_cast<CVarBase*>(&CVarGIRays), static_cast<CVarBase*>(&CVarGIStride),
                        static_cast<CVarBase*>(&CVarGISpacing),
                        static_cast<CVarBase*>(&CVarForceLod), static_cast<CVarBase*>(&CVarCameraSpeed),
                        static_cast<CVarBase*>(&CVarShowGrid)})
        c->SetOnChanged(nullptr);
}

} // namespace zx::editor
