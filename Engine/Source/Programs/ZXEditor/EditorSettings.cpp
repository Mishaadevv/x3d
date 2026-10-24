#include "EditorSettings.h"
#include "Editor.h"
#include "Core/Console/Console.h"
#include "Core/FileSystem/FileSystem.h"
#include "Core/Reflection/ReflectionSerialization.h"
#include "Core/Serialization/Json.h"
#include "RHI/EngineProfile.h"

namespace zx::editor {

ZX_REFLECT_ENUM_BEGIN(AntiAliasing)
    ZX_ENUM_VALUE(Temporal)
    ZX_ENUM_VALUE(FXAA)
    ZX_ENUM_VALUE(None)
ZX_REFLECT_ENUM_END()

ZX_REFLECT_ENUM_BEGIN(ShadowMethod)
    ZX_ENUM_VALUE(Default)
    ZX_ENUM_VALUE(Cascades)
    ZX_ENUM_VALUE(Virtual)
ZX_REFLECT_ENUM_END()

ZX_REFLECT_ENUM_BEGIN(GIModeSetting)
    ZX_ENUM_VALUE(Default)
    ZX_ENUM_VALUE(Dynamic)
    ZX_ENUM_VALUE(Baked)
ZX_REFLECT_ENUM_END()

ZX_REFLECT_ENUM_BEGIN(QualityLevel)
    ZX_ENUM_VALUE(Default)
    ZX_ENUM_VALUE(Low)
    ZX_ENUM_VALUE(Medium)
    ZX_ENUM_VALUE(High)
    ZX_ENUM_VALUE(Ultra)
ZX_REFLECT_ENUM_END()

ZX_REFLECT_STRUCT_BEGIN(EditorSettings)
    ZX_FIELD(uiScale, Meta::Category("Editor"), Meta::DisplayName("UI Scale"), Meta::Range(0.5, 3), Meta::Tooltip("Size of the editor interface (x the Windows display scale)"))
    ZX_FIELD(uiSounds, Meta::DisplayName("UI Sounds"), Meta::Tooltip("Feedback sounds: Play / Stop, builds, packaging"))
    ZX_FIELD(autosaveMinutes, Meta::Range(0, 60), Meta::Tooltip("Save the open scene every N minutes (0 = off)"))
    ZX_FIELD(cameraSpeed, Meta::Category("Viewport"), Meta::Range(0.1, 500), Meta::Units("m/s"), Meta::Tooltip("Fly speed (the mouse wheel changes it while flying)"))
    ZX_FIELD(mouseSensitivity, Meta::Range(0.1, 5), Meta::Tooltip("How fast the camera turns with the mouse"))
    ZX_FIELD(fieldOfView, Meta::Range(20, 120), Meta::Units("deg"), Meta::Tooltip("Vertical field of view of the editor camera"))
    ZX_FIELD(showGrid)
    ZX_FIELD(snapEnabled, Meta::Tooltip("Move, rotate and scale in steps"))
    ZX_FIELD(snapStep, Meta::Range(0.01, 100), Meta::Units("m"), Meta::EditCondition("snapEnabled"))
    ZX_FIELD(rotateSnap, Meta::Range(1, 90), Meta::Units("deg"), Meta::EditCondition("snapEnabled"))
    ZX_FIELD(scaleSnap, Meta::Range(0.01, 1), Meta::EditCondition("snapEnabled"))
    ZX_FIELD(scaleFromOneSide, Meta::Tooltip("Dragging a scale handle stretches the object towards the handle; the opposite side "
                                              "stays put (the center handle keeps the bottom on the ground). Off: scales around the pivot. "
                                              "Alt while dragging does the other one."))
    ZX_FIELD(renderScale, Meta::Category("Rendering (this computer)"), Meta::Range(0, 200), Meta::Units("%"),
             Meta::Tooltip("Resolution of the 3D view (0 = the engine profile's: Lite 70 %, Full 100 %)"))
    ZX_FIELD(antiAliasing, Meta::Tooltip("Temporal: smoothest. FXAA: sharper, cheaper. None: jagged edges"))
    ZX_FIELD(shadowMethod, Meta::Tooltip("Sun shadows: Cascades = cheap, softer far away; Virtual = sharp near and far "
                                         "(virtual shadow maps; best on dedicated GPUs). Default = the engine profile's"))
    ZX_FIELD(shadowQuality, Meta::Tooltip("Resolution, reach and softness of the sun's shadows (Default = the engine profile's; "
                                          "Ultra = soft shadows that harden at the contact)"))
    ZX_FIELD(contactShadows, Meta::Tooltip("Short screen-space shadows where objects touch (a Directional Light can switch them off too)"))
    ZX_FIELD(ambientOcclusion, Meta::Tooltip("Contact darkening in corners and creases"))
    ZX_FIELD(globalIllumination, Meta::Tooltip("Light bounces off walls, floors and objects in real time: lit rooms, dark corners, "
                                               "color bleeding (dynamic GI probes)"))
    ZX_FIELD(giMode, Meta::DisplayName("GI Mode"), Meta::EditCondition("globalIllumination"),
             Meta::Tooltip("Dynamic: probes traced every frame (moving sun and lights). Baked: the lighting baked with "
                           "File > Bake Lighting, no GPU time (dynamic when the scene has no bake). Default = the engine profile's"))
    ZX_FIELD(giQuality, Meta::DisplayName("GI Quality"), Meta::EditCondition("globalIllumination"),
             Meta::Tooltip("How many light probes, how many rays each, how often they update (Default = the engine profile's). "
                           "Higher: finer bounced light that reacts faster, more GPU time. Ultra adds per-pixel rays (Lumen-lite prototype)"))
    ZX_FIELD(screenSpaceGI, Meta::DisplayName("Screen Space GI"), Meta::EditCondition("globalIllumination"),
             Meta::Tooltip("Short-range bounced light from what is on screen (colored objects tint the floor next to them). "
                           "Full profile; costs GPU time"))
    ZX_FIELD(reflections, Meta::Tooltip("Screen-space reflections (where the profile renders them)"))
    ZX_FIELD(volumetricFog, Meta::Tooltip("Light shafts and glowing fog around lights"))
    ZX_FIELD(clouds, Meta::Tooltip("Volumetric clouds"))
    ZX_FIELD(bloom, Meta::Tooltip("Glow around bright lights"))
    ZX_FIELD(depthOfField)
    ZX_FIELD(motionBlur, Meta::Tooltip("While playing"))
    ZX_FIELD(vsync, Meta::Tooltip("Wait for the monitor's refresh: no tearing, at most its refresh rate"))
    ZX_FIELD(masterVolume, Meta::Category("Audio"), Meta::Range(0, 1))
    ZX_FIELD(musicVolume, Meta::Range(0, 1))
    ZX_FIELD(effectsVolume, Meta::Range(0, 1))
ZX_REFLECT_STRUCT_END()

namespace {

constexpr StringView SettingsFile = "/Saved/Config/EditorSettings.json";

/// What the engine profile set before any user setting (captured on the first Apply).
struct ProfileDefaults
{
    bool captured = false;
    f32 renderScale = 1.0f;
    render::ShadowSettings shadows;
    render::GISettings gi;
};
ProfileDefaults g_defaults;

i32 g_section = 0;
// The shadow settings last applied: they are written only when the setting changes, so r.* cvars keep working.
i32 g_appliedQuality = -1;
i32 g_appliedMethod = -1;
i32 g_appliedContact = -1;
i32 g_appliedGI = -1;
i32 g_appliedGIMode = -1;
i32 g_appliedSSGI = -1;
f32 g_autosaveTimer = 0.0f;

} // namespace

void LoadEditorSettings(Editor& ed)
{
    String text;
    JsonValue doc;
    if (fs::ReadTextFile(SettingsFile, text) && ParseJson(text.View(), doc))
        Reflection::FromJson(doc, ed.prefs);
    // The settings mirror these editor fields (toolbar buttons change them too).
    ed.camera.moveSpeed = ed.prefs.cameraSpeed;
    ed.camera.fovY = DegToRad(Clamp(ed.prefs.fieldOfView, 20.0f, 120.0f));
    ed.showGrid = ed.prefs.showGrid;
    ed.snapEnabled = ed.prefs.snapEnabled;
    ed.snapStep = ed.prefs.snapStep;
    ed.uiSounds = ed.prefs.uiSounds;
}

void SaveEditorSettings(const Editor& ed) { fs::WriteTextFile(SettingsFile, ToJsonString(Reflection::ToJson(ed.prefs))); }

void ApplyEditorSettings(Editor& ed)
{
    EditorSettings& s = ed.prefs;
    // Fields the toolbar / viewport also change: the editor's values are the truth, the settings remember them.
    const EditorSettings before = s;
    s.cameraSpeed = ed.camera.moveSpeed;
    s.showGrid = ed.showGrid;
    s.snapEnabled = ed.snapEnabled;
    s.snapStep = ed.snapStep;
    s.uiSounds = ed.uiSounds;
    if (Reflection::ToJson(before) != Reflection::ToJson(s))
        SaveEditorSettings(ed);
    ed.camera.lookSensitivity = 0.0025f * Clamp(s.mouseSensitivity, 0.1f, 5.0f);
    ed.camera.fovY = DegToRad(Clamp(s.fieldOfView, 20.0f, 120.0f));

    // Rendering of the editor viewport.
    ViewportSettings& vs = ed.viewport->Settings();
    if (!g_defaults.captured) {
        g_defaults.captured = true;
        g_defaults.renderScale = vs.renderScale;
        g_defaults.shadows = vs.scene.shadows;
        g_defaults.gi = vs.gi;
    }
    if (!vs.dynamicResolution.enabled)
        vs.renderScale = s.renderScale > 0 ? Clamp(f32(s.renderScale) / 100.0f, 0.25f, 2.0f) : g_defaults.renderScale;
    render::ShadowSettings& sh = vs.scene.shadows;
    if (i32(s.shadowQuality) != g_appliedQuality) {
        g_appliedQuality = i32(s.shadowQuality);
        // Cascades: count, resolution, reach, filter; virtual maps: texels per pixel footprint, reach, pages per frame.
        switch (s.shadowQuality) {
        case QualityLevel::Default:
            sh.cascades = g_defaults.shadows.cascades;
            sh.resolution = g_defaults.shadows.resolution;
            sh.distance = g_defaults.shadows.distance;
            sh.filter = g_defaults.shadows.filter;
            sh.vsmResolution = g_defaults.shadows.vsmResolution;
            sh.vsmDistance = g_defaults.shadows.vsmDistance;
            sh.vsmPagesPerFrame = g_defaults.shadows.vsmPagesPerFrame;
            break;
        case QualityLevel::Low:
            sh.cascades = 2, sh.resolution = 1024, sh.distance = 50.0f, sh.filter = 1;
            sh.vsmResolution = 0.5f, sh.vsmDistance = 120.0f, sh.vsmPagesPerFrame = 24;
            break;
        case QualityLevel::Medium:
            sh.cascades = 3, sh.resolution = 1024, sh.distance = 80.0f, sh.filter = 2;
            sh.vsmResolution = 0.75f, sh.vsmDistance = 180.0f, sh.vsmPagesPerFrame = 32;
            break;
        case QualityLevel::High:
            sh.cascades = 4, sh.resolution = 2048, sh.distance = 120.0f, sh.filter = 2;
            sh.vsmResolution = 1.0f, sh.vsmDistance = 250.0f, sh.vsmPagesPerFrame = 48;
            break;
        case QualityLevel::Ultra:
            sh.cascades = 4, sh.resolution = 2048, sh.distance = 200.0f, sh.filter = 3;
            sh.vsmResolution = 1.0f, sh.vsmDistance = 400.0f, sh.vsmPagesPerFrame = 64;
            break;
        }
    }
    if (i32(s.shadowMethod) != g_appliedMethod) {
        g_appliedMethod = i32(s.shadowMethod);
        sh.virtualShadows = s.shadowMethod == ShadowMethod::Default ? g_defaults.shadows.virtualShadows : s.shadowMethod == ShadowMethod::Virtual;
    }
    if (i32(s.giQuality) != g_appliedGI) {
        g_appliedGI = i32(s.giQuality);
        // Probe grid (count x spacing = reach around the camera), rays per update, one of N probes per frame.
        render::GISettings& gi = vs.gi;
        const render::GISettings& d = g_defaults.gi;
        switch (s.giQuality) {
        case QualityLevel::Default: gi.probeCounts = d.probeCounts, gi.probeSpacing = d.probeSpacing, gi.raysPerProbe = d.raysPerProbe, gi.probeStride = d.probeStride; break;
        case QualityLevel::Low: gi.probeCounts = UVec3(16, 8, 16), gi.probeSpacing = 2.0f, gi.raysPerProbe = 32, gi.probeStride = 8; break;
        case QualityLevel::Medium: gi.probeCounts = UVec3(20, 10, 20), gi.probeSpacing = 1.6f, gi.raysPerProbe = 64, gi.probeStride = 4; break;
        case QualityLevel::High: gi.probeCounts = UVec3(28, 12, 28), gi.probeSpacing = 1.4f, gi.raysPerProbe = 64, gi.probeStride = 2; break;
        case QualityLevel::Ultra: gi.probeCounts = UVec3(32, 14, 32), gi.probeSpacing = 1.2f, gi.raysPerProbe = 128, gi.probeStride = 1; break;
        }
        gi.lumen = s.giQuality == QualityLevel::Ultra; // per-pixel rays: the most expensive, the most detailed
    }
    if (i32(s.giMode) != g_appliedGIMode) {
        g_appliedGIMode = i32(s.giMode);
        vs.gi.mode = s.giMode == GIModeSetting::Default ? g_defaults.gi.mode : (s.giMode == GIModeSetting::Baked ? render::GIMode::Baked : render::GIMode::Dynamic);
    }
    if (i32(s.screenSpaceGI) != g_appliedSSGI) {
        g_appliedSSGI = i32(s.screenSpaceGI);
        vs.gi.screenSpace = s.screenSpaceGI && g_defaults.gi.screenSpace;
    }
    if (i32(s.contactShadows) != g_appliedContact) {
        g_appliedContact = i32(s.contactShadows);
        vs.scene.contactShadows.enabled = s.contactShadows;
    }
    vs.post.fxaa = s.antiAliasing != AntiAliasing::None;
    ViewportFeatures& f = ed.viewport->features;
    f.temporalAA &= s.antiAliasing == AntiAliasing::Temporal;
    f.ambientOcclusion &= s.ambientOcclusion;
    f.globalIllumination &= s.globalIllumination;
    f.screenSpaceReflections &= s.reflections;
    f.volumetricFog &= s.volumetricFog;
    f.clouds &= s.clouds;
    f.bloom &= s.bloom;
    f.depthOfField &= s.depthOfField;
    f.motionBlur &= s.motionBlur;
    if (ed.swapChain && ed.swapChain->GetVSync() != s.vsync && !CommandLine::HasFlag("novsync"))
        ed.swapChain->SetVSync(s.vsync);

    // Audio.
    if (ed.audioEngine) {
        ed.audioEngine->SetBusVolume(audio::Bus::Master, Clamp(s.masterVolume, 0.0f, 1.0f));
        ed.audioEngine->SetBusVolume(audio::Bus::Music, Clamp(s.musicVolume, 0.0f, 1.0f));
        ed.audioEngine->SetBusVolume(audio::Bus::SFX, Clamp(s.effectsVolume, 0.0f, 1.0f));
    }

    // Autosave.
    if (s.autosaveMinutes > 0 && !ed.IsPlaying() && !ed.scenePath.Empty() && ed.undo.IsDirty()) {
        g_autosaveTimer += ed.dt;
        if (g_autosaveTimer >= f32(s.autosaveMinutes) * 60.0f) {
            g_autosaveTimer = 0.0f;
            ed.SaveScene(false);
        }
    } else {
        g_autosaveTimer = 0.0f;
    }
}

void DrawSettingsWindow(Editor& ed)
{
    if (!ed.showSettings)
        return;
    ui::SetNextWindowSize(Vec2(760.0f, 620.0f), ui::Cond::FirstUse);
    if (!ui::Begin(SettingsTitle, &ed.showSettings)) {
        ui::End();
        return;
    }
    struct Section
    {
        const char* icon;
        const char* name;
        const char* category; // reflection category shown in this section ("" = special)
    };
    static const Section Sections[] = {
        {ZX_ICON_LAYOUT_DASHBOARD, "Editor", "Editor"},
        {ZX_ICON_VIDEO, "Viewport", "Viewport"},
        {ZX_ICON_MONITOR, "Rendering", "Rendering (this computer)"},
        {ZX_ICON_VOLUME_2, "Audio", "Audio"},
        {ZX_ICON_FOLDER_KANBAN, "Project", ""},
    };
    // Sidebar.
    if (ui::BeginChild("##settings_nav", Vec2(170.0f, 0.0f), false)) {
        for (i32 i = 0; i < i32(ZX_ARRAY_COUNT(Sections)); ++i)
            if (ui::Selectable(String::Format("{}  {}", Sections[i].icon, Sections[i].name), g_section == i, Vec2(0.0f, 30.0f)))
                g_section = i;
    }
    ui::EndChild();
    ui::SameLine(0.0f, 12.0f);
    if (ui::BeginChild("##settings_page", Vec2(0.0f, 0.0f), false)) {
        const Section& sec = Sections[g_section];
        ui::PushFont(ui::FontStyle::SemiBold, 18.0f);
        ui::Text(sec.name);
        ui::PopFont();
        ui::Spacing();
        if (sec.category[0] != '\0') {
            // The fields of this category (the property editor draws every field: copy only this section's into a
            // scratch object would lose the edit conditions, so draw all and skip the others' headers).
            const TypeInfo& type = ZxTypeOf(static_cast<EditorSettings*>(nullptr));
            const bool changed = PropertyEditor(type, &ed.prefs, "settings_", sec.category);
            if (changed) {
                // Values the editor owns follow the settings right away.
                ed.camera.moveSpeed = ed.prefs.cameraSpeed;
                ed.showGrid = ed.prefs.showGrid;
                ed.snapEnabled = ed.prefs.snapEnabled;
                ed.snapStep = ed.prefs.snapStep;
                ed.uiSounds = ed.prefs.uiSounds;
                SaveEditorSettings(ed);
            }
            if (g_section == 2) {
                ui::Spacing();
                ui::TextDisabled(String::Format("Engine profile: {} - the defaults these settings start from.", ed.lite ? "Lite" : "Full"));
                if (ui::Button(ZX_ICON_GAUGE "  Change Engine Profile..."))
                    ed.showProfileDialog = true;
            }
            if (g_section == 0 && ed.prefs.uiScale != 1.0f) {
                ui::Spacing();
                ui::TextDisabled("The interface scale applies right away.");
            }
        } else {
            // Project settings (saved in the .zproj).
            if (ed.project.descriptorPath.Empty()) {
                ui::TextDisabled("No project open.");
            } else {
                ui::LabelText("Name", ed.project.descriptor.name);
                ui::LabelText("Folder", ed.project.root);
                ui::LabelText("Language", ed.project.descriptor.language);
                ui::LabelText("Startup Scene", ed.project.descriptor.startupScene.Empty() ? String("(none)") : ed.project.descriptor.startupScene);
                ui::Spacing();
                ui::SeparatorText("Graphics");
                static const char* const Eras[][2] = {{"modern", "Modern - everything the engine can do"},
                                                      {"2017", "2017 - Battlefield 1, Horizon Zero Dawn"},
                                                      {"2011", "2011 - Crysis 2, Battlefield 3, Skyrim"},
                                                      {"2008", "2008 - Crysis, GTA IV, Fallout 3"}};
                const GraphicsEra current = ParseGraphicsEra(ed.project.descriptor.graphicsEra.View());
                for (const auto& era : Eras)
                    if (ui::RadioButton(era[1], ParseGraphicsEra(era[0]) == current))
                        ed.SetGraphicsEra(era[0]);
                ui::Spacing();
                ui::SeparatorText("Engine");
                if (ui::Button(ZX_ICON_TOGGLE_RIGHT "  Features..."))
                    ed.showFeatures = true;
                ui::SameLine();
                ui::TextDisabled("Switch engine features and editor tools on or off for this project.");
                if (ui::Button(ZX_ICON_GAUGE "  Engine Profile..."))
                    ed.showProfileDialog = true;
                ui::SameLine();
                ui::TextDisabled(ed.lite ? "Lite (this computer)" : "Full (this computer)");
            }
        }
    }
    ui::EndChild();
    ui::End();
}

} // namespace zx::editor
