#include "Core/Testing/Test.h"
#include "Core/Serialization/Json.h"
#include "Engine/ViewportRenderer.h"
#include "Engine/SceneUndo.h"
#include "Engine/ViewportCVars.h"
#include "Core/Console/Console.h"

using namespace zx;

ZX_TEST(ViewportEnvironment, GraphicsRoundTripAndLegacyDefaults)
{
    ViewportEnvironment source;
    source.autoExposure = false;
    source.manualExposure = true;
    source.manualEV100 = 7.5f;
    source.fog = false;
    JsonValue json = JsonValue::MakeObject();
    WriteEnvironmentGraphics(json, source);
    ViewportEnvironment loaded;
    ReadEnvironmentGraphics(json, loaded);
    ZX_EXPECT(!loaded.autoExposure && loaded.manualExposure && !loaded.fog);
    ZX_EXPECT_NEAR(loaded.manualEV100, 7.5f, 1e-5f);
    ZX_EXPECT_EQ(loaded.giProbeSpacing, 0.0f); // not set: the engine profile's
    source.giProbeSpacing = 0.8f;
    WriteEnvironmentGraphics(json, source);
    ReadEnvironmentGraphics(json, loaded);
    ZX_EXPECT_NEAR(loaded.giProbeSpacing, 0.8f, 1e-5f);
    json.Set("giProbeSpacing", 0.01f); // clamped like r.GI.Spacing
    ReadEnvironmentGraphics(json, loaded);
    ZX_EXPECT_NEAR(loaded.giProbeSpacing, 0.25f, 1e-5f);

    // Old scenes without the new keys retain their sky-based exposure and fog.
    JsonValue legacy = JsonValue::MakeObject();
    legacy.Set("autoExposure", false);
    ReadEnvironmentGraphics(legacy, loaded);
    ZX_EXPECT(!loaded.autoExposure && !loaded.manualExposure && loaded.fog);
    ZX_EXPECT_NEAR(loaded.manualEV100, 13.5f, 1e-5f);
    legacy.Set("manualEV100", 100.0f);
    ReadEnvironmentGraphics(legacy, loaded);
    ZX_EXPECT_NEAR(loaded.manualEV100, 20.0f, 1e-5f);
}

ZX_TEST(ViewportEnvironment, ExposurePriority)
{
    ViewportEnvironment env;
    env.manualExposure = true;
    env.manualEV100 = 8.0f;
    ZX_EXPECT_NEAR(ResolveEnvironmentExposure(env, 12.0f, true, 10.0f), 10.0f, 1e-5f);
    ZX_EXPECT_NEAR(ResolveEnvironmentExposure(env, 12.0f, false, 10.0f), 12.0f, 1e-5f);
    env.autoExposure = false;
    ZX_EXPECT_NEAR(ResolveEnvironmentExposure(env, 12.0f, true, 10.0f), 8.0f, 1e-5f);
    env.manualExposure = false;
    ZX_EXPECT_NEAR(ResolveEnvironmentExposure(env, 12.0f, true, 10.0f), 12.0f, 1e-5f);
}

ZX_TEST(ViewportEnvironment, ExternalSceneChangeIsDirtyUntilSavedOrCleared)
{
    ecs::World world;
    SceneUndoStack undo(world);
    ZX_EXPECT(!undo.IsDirty());
    undo.MarkExternalChange();
    ZX_EXPECT(undo.IsDirty());
    ZX_EXPECT(!undo.CanUndo());
    undo.MarkSaved();
    ZX_EXPECT(!undo.IsDirty());
    undo.MarkExternalChange();
    undo.Clear();
    ZX_EXPECT(!undo.IsDirty());
}

ZX_TEST(ViewportEnvironment, RenderCVarsLiveInTheRuntime)
{
    // r.* variables belong to the Engine module (not the editor), so a packaged game and a project's startupCommands
    // can set them. Unbound (no viewport) they still take values.
    BindViewportCVars(nullptr);
    for (StringView name : {StringView("r.GI.Spacing"), StringView("r.ScreenPercentage"), StringView("r.Nano"), StringView("r.ViewMode")})
        ZX_EXPECT(Console::FindCVar(name) != nullptr);
    ZX_EXPECT(Console::Execute("r.GI.Spacing 0.9", CVarSetBy::Console));
    ZX_EXPECT(Console::FindCVar("r.GI.Spacing")->GetValueString().StartsWith("0.9"));
}

ZX_TEST(ViewportEnvironment, OvercastHdriKeepsTheScenesDirectionalLight)
{
    // An overcast panorama has no sun to take: with Sun = From HDRI the scene's Directional Light stays the sun (it
    // used to be switched off, so the scene had neither direct light nor shadows while the cascades still rendered).
    ZX_EXPECT(HdriSunSource(HdriSunMode::FromHDRI, true, true) == SunSource::Panorama);
    ZX_EXPECT(HdriSunSource(HdriSunMode::FromHDRI, true, false) == SunSource::Panorama);
    ZX_EXPECT(HdriSunSource(HdriSunMode::FromHDRI, false, true) == SunSource::Scene);
    ZX_EXPECT(HdriSunSource(HdriSunMode::FromHDRI, false, false) == SunSource::None);
    ZX_EXPECT(HdriSunSource(HdriSunMode::SceneSun, true, false) == SunSource::Scene);
    ZX_EXPECT(HdriSunSource(HdriSunMode::None, true, true) == SunSource::None);
}
