#pragma once

// File > Settings: the editor's preferences on this computer (Saved/Config/EditorSettings.json) and the open project's
// settings (.zproj). Every value is a reflected field, drawn by the Details property editor.
//
//   Editor     UI scale, sounds, autosave
//   Viewport   camera speed / sensitivity / field of view, grid, snapping, transform tools (scale from one side)
//   Rendering  quality of the editor viewport: render scale, anti-aliasing, shadows, effects, vsync
//   Audio      volumes
//   Project    graphics generation, engine features, engine profile

#include "Core/Reflection/Reflection.h"
#include "UI/Icons.h"

namespace zx::editor {

class Editor;

enum class AntiAliasing : u8
{
    Temporal, // TAA: smooth, slightly soft
    FXAA,     // fast, sharper, some shimmering
    None,
};
ZX_DECLARE_ENUM(AntiAliasing);

enum class ShadowMethod : u8
{
    Default,  // what the engine profile picks (Full: virtual, Lite: cascades)
    Cascades, // a few shadow maps of growing size around the camera: cheap, softer far away
    Virtual,  // virtual shadow maps: sharp shadows near and far, pages cached until something moves
};
ZX_DECLARE_ENUM(ShadowMethod);

enum class QualityLevel : u8
{
    Default, // what the engine profile picks (Lite / Full)
    Low,
    Medium,
    High,
    Ultra,
};
ZX_DECLARE_ENUM(QualityLevel);

struct EditorSettings
{
    // Editor.
    f32 uiScale = 1.0f;          // multiplies the system DPI scale
    bool uiSounds = false;       // feedback sounds (Play / Stop, builds, packaging)
    i32 autosaveMinutes = 0;     // save the open scene every N minutes (0 = off)
    // Viewport.
    f32 cameraSpeed = 5.0f;      // m/s (the mouse wheel changes it while flying)
    f32 mouseSensitivity = 1.0f;
    f32 fieldOfView = 60.0f;     // degrees
    bool showGrid = true;
    bool snapEnabled = false;
    f32 snapStep = 0.5f;         // m
    f32 rotateSnap = 15.0f;      // degrees
    f32 scaleSnap = 0.1f;
    bool scaleFromOneSide = true; // dragging a scale handle stretches the object one way (the opposite side stays put)
    // Rendering (this computer, the editor viewport).
    i32 renderScale = 0;          // % of the viewport resolution (0 = the profile's)
    AntiAliasing antiAliasing = AntiAliasing::Temporal;
    ShadowMethod shadowMethod = ShadowMethod::Default;
    QualityLevel shadowQuality = QualityLevel::Default;
    bool contactShadows = true;
    bool ambientOcclusion = true;
    bool globalIllumination = true; // dynamic GI: bounced light
    QualityLevel giQuality = QualityLevel::Default;
    bool reflections = true;      // screen-space reflections (where the profile has them)
    bool volumetricFog = true;
    bool clouds = true;
    bool bloom = true;
    bool depthOfField = true;
    bool motionBlur = true;       // while playing
    bool vsync = true;
    // Audio.
    f32 masterVolume = 1.0f;
    f32 musicVolume = 1.0f;
    f32 effectsVolume = 1.0f;
};
ZX_DECLARE_TYPE(EditorSettings);

/// Reads Saved/Config/EditorSettings.json (missing: defaults).
void LoadEditorSettings(Editor& ed);
void SaveEditorSettings(const Editor& ed);
/// Applies the settings to the editor, the viewport and the audio (every frame; cheap).
void ApplyEditorSettings(Editor& ed);
/// File > Settings window.
void DrawSettingsWindow(Editor& ed);

inline constexpr StringView SettingsTitle = ZX_ICON_SETTINGS "  Settings";

} // namespace zx::editor
