// Game UI: flex layout and anchors, scaling with the screen, clicks / slider drags / toggles, script events and
// properties, time.scale.

#include "Core/CoreInit.h"
#include "Core/FileSystem/FileSystem.h"
#include "Core/HAL/PlatformFile.h"
#include "Core/String/Path.h"
#include "Core/Testing/Test.h"
#include "Engine/GameRuntime.h"
#include "Engine/GameUI.h"
#include "Engine/Scripting.h"
#include "Scene/Hierarchy.h"
#include "UI/DrawList.h"
#include "UI/UI.h"

using namespace zx;

namespace {

struct UIScene
{
    ecs::World world;
    ui::FontSystem fonts;
    UniquePtr<ui::Context> context;
    GameUISystem ui;
    ecs::Entity canvas;

    UIScene()
    {
        scene::InstallHierarchyHooks(world);
        context.Reset(new ui::Context(fonts));
        context->LoadDefaultFonts(FindEngineRootDirectory() + "/Engine/Content/Fonts");
        canvas = scene::CreateNode(world, "Canvas");
        world.Add(canvas, UICanvas{});
        UIElement root;
        root.padding = Vec4(40.0f);
        world.Add(canvas, root);
    }
    ecs::Entity Widget(StringView name, UIElement el, ecs::Entity parent = {})
    {
        const ecs::Entity e = scene::CreateNode(world, name, Transform(), parent ? parent : canvas);
        world.Add(e, el);
        return e;
    }
    Rect RectOf(ecs::Entity e) const { return world.Get<const UIElement>(e)->rect; }
};

bool Near(f32 a, f32 b) { return Abs(a - b) < 0.6f; }

} // namespace

ZX_TEST(GameUI, FlexLayoutAnchorsAndScaling)
{
    UIScene s;
    // A row bar across the top: fixed 200, a growing middle, fixed 100; gap 10.
    UIElement bar;
    bar.direction = UIDirection::Row;
    bar.size = Vec2(0.0f, 50.0f);
    bar.gap = 10.0f;
    const ecs::Entity row = s.Widget("Bar", bar);
    UIElement a;
    a.size = Vec2(200.0f, 0.0f);
    const ecs::Entity left = s.Widget("Left", a, row);
    UIElement g;
    g.grow = 1.0f;
    const ecs::Entity middle = s.Widget("Middle", g, row);
    UIElement c;
    c.size = Vec2(100.0f, 0.0f);
    const ecs::Entity right = s.Widget("Right", c, row);
    // A box anchored to the bottom-right corner.
    UIElement corner;
    corner.absolute = true;
    corner.anchor = Vec2(1.0f, 1.0f);
    corner.pivot = Vec2(1.0f, 1.0f);
    corner.offset = Vec2(-10.0f, -10.0f);
    corner.size = Vec2(120.0f, 60.0f);
    const ecs::Entity box = s.Widget("Corner", corner);

    s.ui.Layout(s.world, s.fonts, Rect(Vec2(0, 0), Vec2(1920, 1080)));
    // Padding 40: the bar spans 40..1880, stretched across (the canvas aligns Stretch).
    ZX_EXPECT(Near(s.RectOf(row).min.x, 40.0f) && Near(s.RectOf(row).max.x, 1880.0f));
    ZX_EXPECT(Near(s.RectOf(row).Height(), 50.0f));
    ZX_EXPECT(Near(s.RectOf(left).Width(), 200.0f));
    ZX_EXPECT(Near(s.RectOf(right).Width(), 100.0f));
    ZX_EXPECT(Near(s.RectOf(middle).Width(), 1840.0f - 300.0f - 20.0f)); // the rest
    ZX_EXPECT(Near(s.RectOf(middle).min.x, 40.0f + 200.0f + 10.0f));
    ZX_EXPECT(Near(s.RectOf(left).Height(), 50.0f)); // stretched across the row
    ZX_EXPECT(Near(s.RectOf(box).max.x, 1880.0f - 10.0f) && Near(s.RectOf(box).max.y, 1040.0f - 10.0f));

    // Half the screen height: everything at half scale.
    s.ui.Layout(s.world, s.fonts, Rect(Vec2(0, 0), Vec2(960, 540)));
    ZX_EXPECT(Near(s.RectOf(left).Width(), 100.0f));
    ZX_EXPECT(Near(s.RectOf(row).min.x, 20.0f));

    // Text fits its content; hidden widgets take no space.
    UIElement col;
    col.size = Vec2(400.0f, 0.0f);
    col.absolute = true;
    col.anchor = col.pivot = Vec2(0.0f, 0.0f);
    const ecs::Entity column = s.Widget("Column", col);
    UIText t;
    t.text = "Hello";
    t.fontSize = 40.0f;
    const ecs::Entity text = s.Widget("Text", UIElement{}, column);
    s.world.Add(text, t);
    const ecs::Entity hidden = s.Widget("Hidden", UIElement{}, column);
    s.world.Get<UIElement>(hidden)->size = Vec2(0.0f, 300.0f);
    s.world.Get<UIElement>(hidden)->visible = false;
    s.ui.Layout(s.world, s.fonts, Rect(Vec2(0, 0), Vec2(1920, 1080)));
    ZX_EXPECT_GT(s.RectOf(text).Height(), 40.0f);
    ZX_EXPECT_LT(s.RectOf(text).Height(), 60.0f);
    ZX_EXPECT(Near(s.RectOf(column).Height(), s.RectOf(text).Height()));
    // Drawing emits primitives (panel-less text still draws glyphs).
    ui::DrawList dl;
    dl.Reset(&s.fonts, Rect(Vec2(0, 0), Vec2(1920, 1080)));
    s.ui.Draw(s.world, s.fonts, dl, {});
    ZX_EXPECT_GT(dl.PrimitiveCount(), 0u);
}

ZX_TEST(GameUI, ClicksDragsAndToggles)
{
    UIScene s;
    UIElement b;
    b.size = Vec2(200.0f, 60.0f);
    const ecs::Entity button = s.Widget("Play", b);
    s.world.Add(button, UIButton{});
    UIElement sl;
    sl.size = Vec2(300.0f, 30.0f);
    const ecs::Entity slider = s.Widget("Volume", sl);
    s.world.Add(slider, UISlider{});
    UIElement tg;
    tg.size = Vec2(60.0f, 30.0f);
    const ecs::Entity toggle = s.Widget("Music", tg);
    s.world.Add(toggle, UIToggle{});
    s.ui.Layout(s.world, s.fonts, Rect(Vec2(0, 0), Vec2(1920, 1080)));

    const Vec2 bc = s.RectOf(button).Center();
    s.ui.Interact(s.world, bc, true, true, false); // press
    ZX_EXPECT(s.ui.MouseOverUI());
    ZX_EXPECT(s.world.Get<UIElement>(button)->pressed);
    ZX_EXPECT(s.ui.Events().Empty()); // clicks fire on release
    s.ui.Interact(s.world, bc, false, false, true);
    ZX_REQUIRE(s.ui.Events().Size() == 1);
    ZX_EXPECT(s.ui.Events()[0].type == UIEvent::Type::Click && s.ui.Events()[0].entity == button);
    s.ui.ClearEvents();
    // Pressed on the button, released elsewhere: no click.
    s.ui.Interact(s.world, bc, true, true, false);
    s.ui.Interact(s.world, Vec2(5, 5), false, false, true);
    ZX_EXPECT(s.ui.Events().Empty());

    // Dragging the slider to its right end.
    const Rect r = s.RectOf(slider);
    s.ui.Interact(s.world, r.Center(), true, true, false);
    s.ui.Interact(s.world, Vec2(r.max.x + 50.0f, r.Center().y), true, false, false);
    s.ui.Interact(s.world, Vec2(r.max.x + 50.0f, r.Center().y), false, false, true);
    ZX_EXPECT_NEAR(s.world.Get<UISlider>(slider)->value, 1.0f, 1e-4f);
    ZX_EXPECT(!s.ui.Events().Empty() && s.ui.Events().Back().type == UIEvent::Type::Changed);
    s.ui.ClearEvents();

    // A toggle flips on click.
    const Vec2 tc = s.RectOf(toggle).Center();
    s.ui.Interact(s.world, tc, true, true, false);
    s.ui.Interact(s.world, tc, false, false, true);
    ZX_EXPECT(s.world.Get<UIToggle>(toggle)->on);
    ZX_REQUIRE(s.ui.Events().Size() == 1);
    ZX_EXPECT_NEAR(s.ui.Events()[0].value, 1.0f, 1e-6f);
    // Hidden or non-interactable widgets ignore the mouse.
    s.ui.ClearEvents();
    s.world.Get<UIButton>(button)->interactable = false;
    s.ui.Interact(s.world, bc, true, true, false);
    s.ui.Interact(s.world, bc, false, false, true);
    ZX_EXPECT(s.ui.Events().Empty());
}

ZX_TEST(GameUI, ScriptsGetEventsAndSetWidgets)
{
    const String root = Path::Join(Path::Normalize(platform::GetTempDirectory()), "zx_gameui_test");
    platform::DeleteDirectoryTree(root);
    platform::CreateDirectoryTree(root + "/Scripts");
    ZX_REQUIRE(fs::WriteTextFile(root + "/Scripts/Menu.zs", R"zs(
        script Menu {
            var clicks = 0
            fn start() { ui.cursor = true }
            fn onUIClick(name) {
                clicks += 1
                find("Title").text = name + " " + clicks
                find("Health").value = 0.25
                find("Popup").visible = false
                time.scale = 0
            }
            fn onUIChanged(name, value) { find("Title").text = name + "=" + value }
        }
    )zs"));
    UIScene s;
    s.world.Add(s.canvas, Script{String("Scripts/Menu.zs"), String()});
    const ecs::Entity title = s.Widget("Title", UIElement{});
    s.world.Add(title, UIText{});
    const ecs::Entity health = s.Widget("Health", UIElement{});
    s.world.Add(health, UIProgressBar{});
    const ecs::Entity popup = s.Widget("Popup", UIElement{});
    UIElement b;
    b.size = Vec2(200.0f, 60.0f);
    const ecs::Entity button = s.Widget("Resume", b, popup);
    s.world.Add(button, UIButton{});

    ScriptSystem scripts;
    scripts.SetContentRoot(root);
    u32 errors = 0;
    scripts.onLog = [&](StringView, bool isError) { errors += isError; };
    GameSystems systems;
    systems.scripts = &scripts;
    systems.gameUI = &s.ui;
    BeginGame(s.world, systems);
    TickGame(s.world, systems, 1.0f / 60.0f, nullptr, ListenerPose{});
    ZX_EXPECT(s.ui.cursorRequested); // ui.cursor = true in start()

    s.ui.Layout(s.world, s.fonts, Rect(Vec2(0, 0), Vec2(1920, 1080)));
    const Vec2 bc = s.RectOf(button).Center();
    s.ui.Interact(s.world, bc, true, true, false);
    s.ui.Interact(s.world, bc, false, false, true);
    TickGame(s.world, systems, 1.0f / 60.0f, nullptr, ListenerPose{}); // the button's ancestor (the canvas) handles it
    ZX_EXPECT_EQ(errors, 0u);
    ZX_EXPECT(s.world.Get<UIText>(title)->text == "Resume 1");
    ZX_EXPECT_NEAR(s.world.Get<UIProgressBar>(health)->value, 0.25f, 1e-6f);
    ZX_EXPECT(!s.world.Get<UIElement>(popup)->visible);
    ZX_EXPECT_NEAR(GameTimeScale(systems), 0.0f, 1e-6f); // paused
    ZX_EXPECT(s.ui.Events().Empty());                    // consumed by the tick
    EndGame(systems);
    ZX_EXPECT(!s.ui.cursorRequested);
    platform::DeleteDirectoryTree(root);
}
