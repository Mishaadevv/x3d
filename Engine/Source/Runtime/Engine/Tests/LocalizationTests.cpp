// Localization: .zloc tables (comments, escapes, @language), overriding Starter Content, fallbacks, arguments, the
// language choice, UI Text keys, tr() / lang in scripts, writing tables back.

#include "Core/CoreInit.h"
#include "Core/FileSystem/FileSystem.h"
#include "Core/HAL/PlatformFile.h"
#include "Core/String/Path.h"
#include "Core/Testing/Test.h"
#include "Engine/GameUI.h"
#include "Engine/Localization.h"
#include "Engine/Scripting.h"
#include "Scene/Hierarchy.h"
#include "UI/UI.h"

using namespace zx;

namespace {

struct LocDir
{
    String root;
    LocDir()
    {
        root = Path::Join(Path::Normalize(platform::GetTempDirectory()), "zx_loc_test");
        platform::DeleteDirectoryTree(root);
        fs::WriteTextFile(root + "/StarterContent/Localization/en.zloc", "@language: English\nmenu.play = Play\nmenu.quit = Quit\nhud.score = Score: {0} / {1}\n");
        fs::WriteTextFile(root + "/StarterContent/Localization/ru.zloc", "@language: Русский\nmenu.play = Играть\n");
        // The project's table overrides a starter text and adds its own.
        fs::WriteTextFile(root + "/Localization/en.zloc", "# comment\nmenu.play = Start Game\nintro = Line one\\nLine two\n");
    }
    ~LocDir() { platform::DeleteDirectoryTree(root); }
};

} // namespace

ZX_TEST(Localization, TablesFallbacksAndArguments)
{
    LocDir dir;
    Localization loc;
    loc.Load(dir.root);
    ZX_REQUIRE(loc.Languages().Size() == 2u);
    ZX_EXPECT(loc.Find(StringView("ru"))->name == "Русский");
    ZX_REQUIRE(loc.SetLanguage("en"));
    ZX_EXPECT(*loc.Translate("menu.play") == "Start Game"); // the project's table wins
    ZX_EXPECT(*loc.Translate("intro") == "Line one\nLine two");
    ZX_EXPECT(loc.Format("hud.score", Span<const String>({String("7"), String("10")})) == "Score: 7 / 10");
    ZX_REQUIRE(loc.SetLanguage("RU"));
    ZX_EXPECT(loc.Current() == "ru");
    ZX_EXPECT(*loc.Translate("menu.play") == "Играть");
    ZX_EXPECT(*loc.Translate("menu.quit") == "Quit"); // missing in Russian: English
    ZX_EXPECT(loc.Format("no.such.key") == "no.such.key");
    ZX_EXPECT(!loc.SetLanguage("de"));
    // The player's choice is remembered.
    loc.preferenceFile = dir.root + "/Saved/Language.txt";
    loc.SetLanguage("ru");
    Localization again;
    again.preferenceFile = loc.preferenceFile;
    again.Load(dir.root);
    ZX_EXPECT(again.Current() == "ru");
    // Editing and writing back.
    loc.SetText("ru", "menu.quit", "Выход");
    loc.AddLanguage("de", "Deutsch");
    loc.SetText("de", "menu.play", "Spielen");
    ZX_REQUIRE(loc.Save(dir.root));
    Localization saved;
    saved.Load(dir.root);
    ZX_REQUIRE(saved.SetLanguage("de"));
    ZX_EXPECT(*saved.Translate("menu.play") == "Spielen");
    ZX_REQUIRE(saved.SetLanguage("ru"));
    ZX_EXPECT(*saved.Translate("menu.quit") == "Выход");
    ZX_EXPECT(saved.AllKeys().Size() == 4u);
}

ZX_TEST(Localization, UITextKeysAndScripts)
{
    LocDir dir;
    Localization loc;
    loc.Load(dir.root);
    loc.SetLanguage("en");
    ecs::World world;
    scene::InstallHierarchyHooks(world);
    ui::FontSystem fonts;
    ui::Context context(fonts);
    context.LoadDefaultFonts(FindEngineRootDirectory() + "/Engine/Content/Fonts");
    GameUISystem ui;
    ui.localization = &loc;
    const ecs::Entity canvas = scene::CreateNode(world, "Canvas");
    world.Add(canvas, UICanvas{});
    world.Add(canvas, UIElement{});
    const ecs::Entity label = scene::CreateNode(world, "Label", Transform(), canvas);
    world.Add(label, UIElement{});
    UIText t;
    t.text = "fallback";
    t.key = "menu.play";
    world.Add(label, t);
    const UIText* missing = nullptr;
    {
        const ecs::Entity e = scene::CreateNode(world, "Missing", Transform(), canvas);
        world.Add(e, UIElement{});
        UIText m;
        m.text = "kept";
        m.key = "no.such.key";
        world.Add(e, m);
        missing = world.Get<const UIText>(e);
    }
    const Rect screen(Vec2(0, 0), Vec2(1280, 720));
    ui.Layout(world, fonts, screen);
    ZX_EXPECT(world.Get<const UIText>(label)->text == "Start Game");
    ZX_EXPECT(missing->text == "kept");
    loc.SetLanguage("ru");
    ui.Layout(world, fonts, screen);
    ZX_EXPECT(world.Get<const UIText>(label)->text == "Играть");

    // Scripts: tr() with arguments, lang.current / set / list / name.
    const String scriptPath = dir.root + "/Scripts/Lang.zs";
    ZX_REQUIRE(fs::WriteTextFile(scriptPath, R"zs(
script Lang {
    fn start() {
        find("Label").text = tr("hud.score", 3, 5)
        log(lang.current + " " + str(len(lang.list())) + " " + lang.name("en"))
        lang.set("en")
    }
}
)zs"));
    ScriptSystem scripts;
    scripts.SetContentRoot(dir.root);
    scripts.localization = &loc;
    const ecs::Entity holder = scene::CreateNode(world, "Holder");
    world.Add(holder, Script{String("Scripts/Lang.zs"), String()});
    scripts.Begin(world);
    scripts.Tick(world, 0.016f, nullptr);
    ZX_EXPECT(world.Get<const UIText>(label)->text == "Score: 3 / 5"); // English fallback (no Russian text)
    ZX_EXPECT(world.Get<const UIText>(label)->key.Empty());           // set by the script: no longer translated
    ZX_EXPECT(loc.Current() == "en");
    scripts.End();
}
