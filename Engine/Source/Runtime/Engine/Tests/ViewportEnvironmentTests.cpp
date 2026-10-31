#include "Core/Testing/Test.h"
#include "Core/Serialization/Json.h"
#include "Engine/ViewportRenderer.h"
#include "Engine/SceneUndo.h"

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
