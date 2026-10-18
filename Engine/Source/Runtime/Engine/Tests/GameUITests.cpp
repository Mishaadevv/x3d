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
#include "Platform/Input.h"
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

ZX_TEST(GameUI, WorldSpaceCanvasFollowsItsEntity)
{
    UIScene s;
    const ecs::Entity tank = scene::CreateNode(s.world, "Tank", Transform(Vec3(0, 0, 10)));
    const ecs::Entity plate = scene::CreateNode(s.world, "Plate", Transform(), tank);
    UICanvas cv;
    cv.worldSpace = true;
    cv.worldOffset = Vec3(0, 2, 0);
    cv.fullSizeDistance = 10.0f;
    cv.maxDistance = 50.0f;
    s.world.Add(plate, cv);
    UIElement root;
    root.size = Vec2(200, 40);
    root.pivot = Vec2(0.5f, 1.0f);
    s.world.Add(plate, root);
    scene::UpdateWorldTransforms(s.world);
    const Mat4 proj = Mat4::PerspectiveInfiniteReversedZ(DegToRad(60.0f), 1920.0f / 1080.0f, 0.1f);
    auto view = [&](Vec3 eye, Vec3 target) { return proj * Mat4::LookAt(eye, target, Vec3::Up()); };
    const Rect screen(Vec2(0, 0), Vec2(1920, 1080));

    // Looking straight at the point above the tank: centered horizontally, its bottom edge at the screen center.
    s.ui.SetCamera(view(Vec3(0, 2, 0), Vec3(0, 2, 10)), Vec3(0, 2, 0));
    s.ui.Layout(s.world, s.fonts, screen);
    const Rect r = s.RectOf(plate);
    ZX_EXPECT(s.world.Get<const UICanvas>(plate)->onScreen);
    ZX_EXPECT(Near(r.Center().x, 960.0f));
    ZX_EXPECT(Near(r.max.y, 540.0f));
    ZX_EXPECT(Near(r.Width(), 200.0f)); // 10 m = full size
    // Twice as far: half the size (still above the tank).
    s.ui.SetCamera(view(Vec3(0, 2, -10), Vec3(0, 2, 10)), Vec3(0, 2, -10));
    s.ui.Layout(s.world, s.fonts, screen);
    ZX_EXPECT(Near(s.RectOf(plate).Width(), 100.0f));
    // Behind the camera, and beyond Max Distance: hidden.
    s.ui.SetCamera(view(Vec3(0, 2, 20), Vec3(0, 2, 40)), Vec3(0, 2, 20));
    s.ui.Layout(s.world, s.fonts, screen);
    ZX_EXPECT(!s.world.Get<const UICanvas>(plate)->onScreen);
    s.ui.SetCamera(view(Vec3(0, 2, -60), Vec3(0, 2, 10)), Vec3(0, 2, -60));
    s.ui.Layout(s.world, s.fonts, screen);
    ZX_EXPECT(!s.world.Get<const UICanvas>(plate)->onScreen);
}

ZX_TEST(GameUI, ScrollViewAndWrap)
{
    UIScene s;
    UIElement list;
    list.size = Vec2(400, 300);
    list.scroll = true;
    list.gap = 0.0f;
    const ecs::Entity view = s.Widget("List", list);
    ecs::Entity rows[10];
    for (u32 i = 0; i < 10; ++i) {
        UIElement row;
        row.size = Vec2(0, 100);
        rows[i] = s.Widget(String::Format("Row {}", i), row, view);
    }
    const Rect screen(Vec2(0, 0), Vec2(1920, 1080));
    s.ui.Layout(s.world, s.fonts, screen);
    ZX_EXPECT(Near(s.RectOf(rows[0]).Height(), 100.0f)); // not squeezed
    ZX_EXPECT(Near(s.world.Get<const UIElement>(view)->contentHeight, 1000.0f));
    const f32 top0 = s.RectOf(rows[0]).min.y;
    // Wheel down over the list: scrolled by 60 units per notch; clamped at the end (1000 - 300).
    s.ui.Interact(s.world, s.RectOf(view).Center(), false, false, false, -2.0f);
    s.ui.Layout(s.world, s.fonts, screen);
    ZX_EXPECT(Near(s.RectOf(rows[0]).min.y, top0 - 120.0f));
    s.ui.Interact(s.world, s.RectOf(view).Center(), false, false, false, -100.0f);
    s.ui.Layout(s.world, s.fonts, screen);
    ZX_EXPECT(Near(s.world.Get<const UIElement>(view)->scrollOffset, 700.0f));

    // Wrap: 5 boxes of 150 in a 400-wide row (gap 10) -> lines of 2, 2, 1.
    UIElement grid;
    grid.direction = UIDirection::Row;
    grid.wrap = true;
    grid.size = Vec2(400, 0);
    grid.gap = 10.0f;
    const ecs::Entity g = s.Widget("Grid", grid);
    ecs::Entity cells[5];
    for (u32 i = 0; i < 5; ++i) {
        UIElement c;
        c.size = Vec2(150, 50);
        cells[i] = s.Widget(String::Format("Cell {}", i), c, g);
    }
    s.ui.Layout(s.world, s.fonts, screen);
    ZX_EXPECT(Near(s.RectOf(cells[1]).min.x - s.RectOf(cells[0]).min.x, 160.0f));
    ZX_EXPECT(Near(s.RectOf(cells[2]).min.x, s.RectOf(cells[0]).min.x));
    ZX_EXPECT(Near(s.RectOf(cells[2]).min.y - s.RectOf(cells[0]).min.y, 60.0f));
    ZX_EXPECT(Near(s.RectOf(cells[4]).min.y - s.RectOf(cells[0]).min.y, 120.0f));
    ZX_EXPECT(Near(s.RectOf(g).Height(), 170.0f)); // 3 lines of 50 + 2 gaps
}

ZX_TEST(GameUI, InputFieldTyping)
{
    UIScene s;
    UIElement box;
    box.size = Vec2(400, 50);
    const ecs::Entity field = s.Widget("Chat", box);
    UIInputField in;
    in.maxLength = 8;
    in.clearOnSubmit = true;
    s.world.Add(field, in);
    s.ui.Layout(s.world, s.fonts, Rect(Vec2(0, 0), Vec2(1920, 1080)));
    // Typing does nothing until the field is clicked.
    s.ui.TypeText(s.world, "x", false, false, false);
    ZX_EXPECT(s.world.Get<const UIInputField>(field)->text.Empty());
    s.ui.Interact(s.world, s.RectOf(field).Center(), true, true, false);
    ZX_EXPECT(s.ui.WantsKeyboard());
    s.ui.TypeText(s.world, "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82!", false, false, false); // "Привет!" (Cyrillic, 2-byte)
    ZX_EXPECT_EQ(s.world.Get<const UIInputField>(field)->text, String("\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82!"));
    s.ui.TypeText(s.world, "\x08" "abc", false, false, false); // control characters are ignored; 8 characters max
    ZX_EXPECT_EQ(s.world.Get<const UIInputField>(field)->text, String("\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82!a"));
    s.ui.TypeText(s.world, "", true, false, false); // backspace removes one character
    s.ui.TypeText(s.world, "", true, false, false); // ... a whole 2-byte one
    ZX_EXPECT_EQ(s.world.Get<const UIInputField>(field)->text, String("\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82"));
    s.ui.ClearEvents();
    s.ui.TypeText(s.world, "", false, true, false); // Enter: submitted, cleared, keyboard released
    ZX_REQUIRE(s.ui.Events().Size() == 1u);
    ZX_EXPECT(s.ui.Events()[0].type == UIEvent::Type::Submit);
    ZX_EXPECT_EQ(s.ui.Events()[0].text, String("\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82"));
    ZX_EXPECT(s.world.Get<const UIInputField>(field)->text.Empty());
    ZX_EXPECT(!s.ui.WantsKeyboard());
}

ZX_TEST(GameUI, StyleSheetParsing)
{
    UIStyleSheet sheet;
    const bool ok = ParseUIStyle(R"css(
        /* comment */
        @accent: #3B8CF5;
        Button, Toggle { color: $accent; corner-radius: 12px; }
        Panel.menu Button:hover { scale: 1.05; }
        #Title.big { font-size: 72; }
        Text { colr }
    )css",
                                 sheet);
    ZX_EXPECT(!ok); // "colr" without a value
    ZX_REQUIRE(sheet.rules.Size() == 4u);
    ZX_EXPECT_EQ(sheet.rules[0].selectors.Size(), 2u);
    ZX_EXPECT(sheet.rules[0].properties[0].name == "color");
    ZX_EXPECT(sheet.rules[0].properties[0].value == "#3B8CF5"); // the variable
    ZX_EXPECT(sheet.rules[0].properties[1].name == "cornerradius");
    const UIStyleSelector& hover = sheet.rules[1].selectors[0];
    ZX_REQUIRE(hover.parts.Size() == 2u);
    ZX_EXPECT(hover.parts[0].type == "Panel" && hover.parts[0].classes[0] == "menu");
    ZX_EXPECT(hover.HasStates());
    ZX_EXPECT(sheet.rules[2].selectors[0].specificity > hover.specificity); // #id beats classes
    // Values: CSS order for Vec4, colors, enums.
    Vec4 pad;
    ZX_REQUIRE(ParseStyleValue("8 16", TypeOf<Vec4>(), &pad));
    ZX_EXPECT(pad == Vec4(16.0f, 8.0f, 16.0f, 8.0f));
    LinearColor c;
    ZX_REQUIRE(ParseStyleColor("rgba(255, 0, 0, 0.5)", c));
    ZX_EXPECT(Abs(c.r - 1.0f) < 1e-3f && Abs(c.a - 0.5f) < 0.01f);
    UIJustify j = UIJustify::Start;
    ZX_REQUIRE(ParseStyleValue("space-between", TypeOf<UIJustify>(), &j));
    ZX_EXPECT(j == UIJustify::SpaceBetween);
}

ZX_TEST(GameUI, StylesApplyByTypeClassAndState)
{
    UIScene s;
    const String root = Path::Join(Path::Normalize(platform::GetTempDirectory()), "zx_gameui_style");
    platform::CreateDirectoryTree(root);
    ZX_REQUIRE(fs::WriteTextFile(root + "/Menu.zstyle", R"css(
        Button { color: #102030; corner-radius: 20; padding: 10 20; }
        Button:hover { scale: 1.2; color: #FF0000; }
        Text.title { font-size: 64; weight: Bold; }
        .faded { opacity: 0.5; }
        Text { colour: red; }
    )css"));
    s.ui.contentRoot = root;
    s.world.Get<UICanvas>(s.canvas)->style = "Menu.zstyle";
    UIElement be;
    be.size = Vec2(200.0f, 60.0f);
    const ecs::Entity button = s.Widget("Play", be);
    s.world.Add(button, UIButton{});
    UIElement te;
    te.styleClass = "title faded";
    const ecs::Entity title = s.Widget("Title", te);
    s.world.Add(title, UIText{});
    const Rect screen(Vec2(0, 0), Vec2(1920, 1080));
    s.ui.Layout(s.world, s.fonts, screen);
    const UIButton& b = *s.world.Get<const UIButton>(button);
    ZX_EXPECT(Abs(b.cornerRadius - 20.0f) < 1e-4f);
    ZX_EXPECT(s.world.Get<const UIElement>(button)->padding == Vec4(20.0f, 10.0f, 20.0f, 10.0f));
    ZX_EXPECT(Abs(s.world.Get<const UIText>(title)->fontSize - 64.0f) < 1e-4f);
    ZX_EXPECT(s.world.Get<const UIText>(title)->weight == UIFontWeight::Bold);
    ZX_EXPECT(Abs(s.world.Get<const UIElement>(title)->drawOpacity - 0.5f) < 1e-3f);
    ZX_EXPECT(!s.ui.StyleErrors().Empty()); // "colour" isn't a property
    // Hover: the state rule applies for the frame (scale + color), then the widget gets its own values back.
    const Rect before = s.RectOf(button);
    s.ui.Interact(s.world, before.Center(), false, false, false);
    s.ui.Layout(s.world, s.fonts, screen);
    ZX_EXPECT(s.RectOf(button).Width() > before.Width() * 1.15f); // drawn 1.2x around the center
    ZX_EXPECT(Abs(s.RectOf(button).Center().x - before.Center().x) < 0.6f);
    ZX_EXPECT(b.color.r > 0.9f);
    ui::DrawList dl;
    dl.Reset(&s.fonts, screen);
    s.ui.Draw(s.world, s.fonts, dl, {});
    ZX_EXPECT(b.color.r < 0.1f); // restored after the frame
    ZX_EXPECT(Abs(s.world.Get<const UIElement>(button)->scale - 1.0f) < 1e-5f);
    platform::DeleteDirectoryTree(root);
}

ZX_TEST(GameUI, TransitionsAnimationsAndTweens)
{
    UIScene s;
    UIElement e;
    e.size = Vec2(100.0f, 100.0f);
    e.transition = 0.2f;
    const ecs::Entity box = s.Widget("Box", e);
    s.world.Add(box, UIPanel{});
    UIAnimation anim;
    anim.preset = UIAnimationPreset::FadeIn;
    anim.duration = 0.5f;
    const ecs::Entity fading = s.Widget("Fading", e);
    s.world.Add(fading, anim);
    const Rect screen(Vec2(0, 0), Vec2(1920, 1080));
    s.ui.Layout(s.world, s.fonts, screen);
    // Transition: Scale 2 is reached smoothly while playing.
    s.world.Get<UIElement>(box)->scale = 2.0f;
    s.ui.Update(s.world, 0.05f);
    s.ui.Layout(s.world, s.fonts, screen);
    s.ui.Update(s.world, 0.05f);
    const f32 mid = s.world.Get<const UIElement>(box)->shownScale;
    ZX_EXPECT(mid > 1.1f && mid < 1.9f);
    for (u32 i = 0; i < 20; ++i)
        s.ui.Update(s.world, 0.05f);
    ZX_EXPECT(Abs(s.world.Get<const UIElement>(box)->shownScale - 2.0f) < 0.01f);
    // Fade In finished after its duration (it started on the first Update).
    ZX_EXPECT(Abs(s.world.Get<const UIElement>(fading)->animOpacity - 1.0f) < 1e-3f);
    s.world.Get<UIElement>(fading)->visible = false;
    s.ui.Update(s.world, 0.05f);
    s.world.Get<UIElement>(fading)->visible = true; // shown again: plays again
    s.ui.Update(s.world, 0.1f);
    ZX_EXPECT(s.world.Get<const UIElement>(fading)->animOpacity < 0.6f);
    // Tweens: opacity to 0 in 0.4 s (Linear), a color, unknown properties.
    const f32 zero = 0.0f;
    ZX_REQUIRE(s.ui.Animate(s.world, box, "opacity", Span<const f32>(&zero, 1), 0.4f, UIEasing::Linear, 0.0f));
    s.ui.Update(s.world, 0.2f);
    ZX_EXPECT(Abs(s.world.Get<const UIElement>(box)->opacity - 0.5f) < 0.01f);
    s.ui.Update(s.world, 0.3f);
    ZX_EXPECT(s.world.Get<const UIElement>(box)->opacity == 0.0f);
    const f32 red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    ZX_REQUIRE(s.ui.Animate(s.world, box, "background", Span<const f32>(red, 4), 0.0f, UIEasing::Linear, 0.0f));
    s.ui.Update(s.world, 0.01f);
    ZX_EXPECT(s.world.Get<const UIPanel>(box)->color.r == 1.0f);
    String error;
    ZX_EXPECT(!s.ui.Animate(s.world, box, "wobble", Span<const f32>(&zero, 1), 1.0f, UIEasing::Linear, 0.0f, &error));
    ZX_EXPECT(!error.Empty());
    // Easing ends.
    for (u32 k = 0; k <= u32(UIEasing::Bounce); ++k) {
        ZX_EXPECT(Abs(ApplyEasing(UIEasing(k), 0.0f)) < 1e-4f);
        ZX_EXPECT(Abs(ApplyEasing(UIEasing(k), 1.0f) - 1.0f) < 1e-4f);
    }
}

ZX_TEST(GameUI, GamepadAndKeyboardNavigation)
{
    UIScene s;
    UIElement row;
    row.direction = UIDirection::Row;
    row.gap = 20.0f;
    const ecs::Entity bar = s.Widget("Row", row);
    UIElement be;
    be.size = Vec2(200.0f, 60.0f);
    const ecs::Entity a = s.Widget("A", be, bar);
    s.world.Add(a, UIButton{});
    const ecs::Entity b = s.Widget("B", be, bar);
    s.world.Add(b, UIButton{});
    const ecs::Entity slider = s.Widget("Volume", be);
    s.world.Add(slider, UISlider{});
    const Rect screen(Vec2(0, 0), Vec2(1920, 1080));
    InputState input;
    auto press = [&](Key k) {
        input.BeginFrame();
        input.SetKey(k, true);
        s.ui.Layout(s.world, s.fonts, screen);
        s.ui.Navigate(s.world, input, 0.016f);
        input.BeginFrame();
        input.SetKey(k, false);
    };
    s.ui.Layout(s.world, s.fonts, screen);
    press(Key::Right); // the first press shows the focus on the first widget
    ZX_EXPECT(s.ui.NavFocus() == a && s.ui.NavActive());
    ZX_EXPECT(s.world.Get<const UIElement>(a)->focused && s.world.Get<const UIElement>(a)->hovered);
    press(Key::Right);
    ZX_EXPECT(s.ui.NavFocus() == b);
    press(Key::Down);
    ZX_EXPECT(s.ui.NavFocus() == slider);
    // Left / right on a slider change its value.
    const f32 v0 = s.world.Get<const UISlider>(slider)->value;
    press(Key::Right);
    ZX_EXPECT(s.world.Get<const UISlider>(slider)->value > v0);
    press(Key::Up);
    s.ui.ClearEvents();
    press(Key::Enter); // presses the focused button
    bool clicked = false;
    for (const UIEvent& ev : s.ui.Events())
        clicked = clicked || (ev.type == UIEvent::Type::Click && (ev.entity == a || ev.entity == b));
    ZX_EXPECT(clicked);
    press(Key::Escape);
    bool back = false;
    for (const UIEvent& ev : s.ui.Events())
        back = back || ev.type == UIEvent::Type::Back;
    ZX_EXPECT(back);
    // The mouse takes over: the focus ring hides.
    input.BeginFrame();
    input.AddMouseDelta(Vec2(5.0f, 0.0f));
    s.ui.Navigate(s.world, input, 0.016f);
    ZX_EXPECT(!s.ui.NavActive());
}
