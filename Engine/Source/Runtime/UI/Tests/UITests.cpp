// Headless interaction tests for the immediate-mode UI: synthetic mouse/keyboard input drives real widgets.

#include "Core/CoreInit.h"
#include "Core/String/Utf.h"
#include "Core/Testing/Test.h"
#include "UI/UI.h"

using namespace zx;
using namespace zx::ui;

namespace {

struct Harness
{
    FontSystem fonts;
    Context ctx{fonts};
    InputState input;
    Array<WindowEvent> events;
    bool ok = false;

    Harness()
    {
        ok = ctx.LoadDefaultFonts(FindEngineRootDirectory() + "/Engine/Content/Fonts");
        SetCurrentContext(&ctx);
    }
    ~Harness() { SetCurrentContext(nullptr); }

    template <typename F>
    void Frame(F&& body)
    {
        FrameInput fi;
        fi.displaySize = Vec2(1280.0f, 800.0f);
        fi.deltaTime = 1.0f / 60.0f;
        fi.input = &input;
        fi.events = events.View();
        ctx.BeginFrame(fi);
        body();
        ctx.EndFrame();
        events.Clear();
        input.BeginFrame();
    }
    void Move(Vec2 p) { input.SetMousePosition(p); }
    void Down() { input.SetMouseButton(MouseButton::Left, true); }
    void Up() { input.SetMouseButton(MouseButton::Left, false); }
    void Key(zx::Key k)
    {
        WindowEvent e;
        e.type = WindowEventType::KeyDown;
        e.key = k;
        events.PushBack(e);
    }
    void Type(StringView text)
    {
        for (u32 cp : Utf8Codepoints(text)) {
            WindowEvent e;
            e.type = WindowEventType::Char;
            e.codepoint = cp;
            events.PushBack(e);
        }
    }
};

/// Standard test window at a fixed position.
bool BeginTestWindow()
{
    SetNextWindowPos(Vec2(100.0f, 100.0f), Cond::Always);
    SetNextWindowSize(Vec2(500.0f, 500.0f), Cond::Always);
    return Begin("Test");
}

} // namespace

ZX_TEST(UI, ButtonClick)
{
    Harness h;
    ZX_REQUIRE(h.ok);
    Rect r;
    u32 clicks = 0;
    auto ui = [&] {
        BeginTestWindow();
        if (Button("Нажми"))
            ++clicks;
        r = GetItemRect();
        End();
    };
    h.Frame(ui); // first frame: window measured/hidden
    h.Frame(ui);
    ZX_REQUIRE(!r.IsEmpty());
    h.Move(r.Center());
    h.Frame(ui);
    h.Down();
    h.Frame(ui);
    ZX_EXPECT_EQ(clicks, 0u); // fires on release
    h.Up();
    h.Frame(ui);
    ZX_EXPECT_EQ(clicks, 1u);
    // Press inside, release outside: no click.
    h.Down();
    h.Frame(ui);
    h.Move(r.max + Vec2(200.0f, 200.0f));
    h.Frame(ui);
    h.Up();
    h.Frame(ui);
    ZX_EXPECT_EQ(clicks, 1u);
    ZX_EXPECT(!h.ctx.WantsKeyboard());
}

ZX_TEST(UI, CheckboxSliderAndDrag)
{
    Harness h;
    ZX_REQUIRE(h.ok);
    bool check = false;
    f32 slider = 0.0f, drag = 1.0f;
    Rect checkRect, sliderRect, dragRect;
    auto ui = [&] {
        BeginTestWindow();
        Checkbox("Флажок", &check);
        checkRect = GetItemRect();
        SliderFloat("Слайдер", &slider, 0.0f, 10.0f);
        sliderRect = GetItemRect();
        DragFloat("Драг", &drag, 0.5f);
        dragRect = GetItemRect();
        End();
    };
    h.Frame(ui);
    h.Frame(ui);
    auto click = [&](Vec2 p) {
        h.Move(p);
        h.Frame(ui);
        h.Down();
        h.Frame(ui);
        h.Up();
        h.Frame(ui);
    };
    click(checkRect.min + Vec2(5.0f, checkRect.Height() * 0.5f));
    ZX_EXPECT(check);
    click(checkRect.min + Vec2(5.0f, checkRect.Height() * 0.5f));
    ZX_EXPECT(!check);
    // Slider: press near the right end of the frame (the label is to the right of the frame).
    h.Move(Vec2(sliderRect.min.x + 4.0f, sliderRect.Center().y));
    h.Frame(ui);
    h.Down();
    h.Frame(ui);
    ZX_EXPECT_NEAR(slider, 0.0f, 0.2f);
    // Dragging far to the right clamps to the maximum.
    h.Move(Vec2(sliderRect.min.x + 10000.0f, sliderRect.Center().y));
    h.Frame(ui);
    ZX_EXPECT_NEAR(slider, 10.0f, 0.01f); // clamped to max
    h.Up();
    h.Frame(ui);
    // Drag: 20 px to the right at speed 0.5 -> +10.
    h.Move(dragRect.min + Vec2(10.0f, dragRect.Height() * 0.5f));
    h.Frame(ui);
    h.Down();
    h.Frame(ui);
    h.Move(dragRect.min + Vec2(30.0f, dragRect.Height() * 0.5f));
    h.Frame(ui);
    h.Up();
    h.Frame(ui);
    ZX_EXPECT_NEAR(drag, 11.0f, 0.01f);
}

ZX_TEST(UI, TextInputEditing)
{
    Harness h;
    ZX_REQUIRE(h.ok);
    String text = "Привет";
    String committed;
    Rect r;
    bool enter = false;
    auto ui = [&] {
        BeginTestWindow();
        InputText("Поле", text);
        r = GetItemRect();
        if (InputText("Ввод", committed, InputTextFlags::EnterReturnsTrue))
            enter = true;
        End();
    };
    h.Frame(ui);
    h.Frame(ui);
    // Click at the far right of the field -> caret at the end.
    h.Move(Vec2(r.max.x - 4.0f, r.Center().y));
    h.Frame(ui);
    h.Down();
    h.Frame(ui);
    h.Up();
    h.Frame(ui);
    ZX_EXPECT(h.ctx.WantsKeyboard());
    h.Type(", мир");
    h.Frame(ui);
    ZX_EXPECT_EQ(text, "Привет, мир");
    h.Key(zx::Key::Backspace);
    h.Key(zx::Key::Backspace);
    h.Frame(ui);
    ZX_EXPECT_EQ(text, "Привет, м");
    // Ctrl+Backspace deletes the previous word, Ctrl+Z restores it.
    h.input.SetKey(zx::Key::LeftControl, true);
    h.Key(zx::Key::Backspace);
    h.Frame(ui);
    ZX_EXPECT_EQ(text, "Привет, ");
    h.Key(zx::Key::Z);
    h.Frame(ui);
    ZX_EXPECT_EQ(text, "Привет, м");
    // Ctrl+A then typing replaces everything.
    h.Key(zx::Key::A);
    h.Frame(ui);
    h.input.SetKey(zx::Key::LeftControl, false);
    h.Type("Новое");
    h.Frame(ui);
    ZX_EXPECT_EQ(text, "Новое");
    // Shift+Left selects two characters, Delete removes them.
    h.input.SetKey(zx::Key::LeftShift, true);
    h.Key(zx::Key::Left);
    h.Key(zx::Key::Left);
    h.Frame(ui);
    h.input.SetKey(zx::Key::LeftShift, false);
    h.Key(zx::Key::Delete);
    h.Frame(ui);
    ZX_EXPECT_EQ(text, "Нов");
    // Escape reverts to the value at activation and releases focus.
    h.Key(zx::Key::Escape);
    h.Frame(ui);
    ZX_EXPECT_EQ(text, "Привет");
    ZX_EXPECT(!h.ctx.WantsKeyboard());
    ZX_EXPECT(!enter);
}

ZX_TEST(UI, ConsolePromptKeepsFocus)
{
    // A console prompt: Enter submits but keeps the field active; the app replaces the text (history/completion) and
    // moves the caret to the end, so typing continues after it.
    Harness h;
    ZX_REQUIRE(h.ok);
    String line;
    Array<String> submitted;
    Rect r;
    auto ui = [&] {
        BeginTestWindow();
        if (InputText("##prompt", line, InputTextFlags::EnterReturnsTrue | InputTextFlags::KeepFocusOnEnter)) {
            submitted.PushBack(line);
            line.Clear();
        }
        r = GetItemRect();
        End();
    };
    h.Frame(ui);
    h.Move(r.Center());
    h.Frame(ui);
    h.Down();
    h.Frame(ui);
    h.Up();
    h.Frame(ui);
    h.Type("help");
    h.Frame(ui);
    h.Key(zx::Key::Enter);
    h.Frame(ui);
    ZX_EXPECT_EQ(submitted.Size(), usize(1));
    ZX_EXPECT(submitted.Size() == 1 && submitted[0] == "help");
    ZX_EXPECT(line.Empty());
    ZX_EXPECT(h.ctx.WantsKeyboard()); // still focused
    h.Type("r.");
    h.Frame(ui);
    ZX_EXPECT_EQ(line, "r.");
    // Programmatic completion, caret to the end, keep typing.
    line = "r.TAA ";
    SetActiveTextCursorToEnd();
    h.Frame(ui);
    h.Type("0");
    h.Frame(ui);
    ZX_EXPECT_EQ(line, "r.TAA 0");
    h.Key(zx::Key::Enter);
    h.Frame(ui);
    ZX_EXPECT(submitted.Size() == 2 && submitted[1] == "r.TAA 0");
}

ZX_TEST(UI, TreeNodeAndPopup)
{
    Harness h;
    ZX_REQUIRE(h.ok);
    bool open = false;
    bool popupVisible = false;
    Rect nodeRect, buttonRect;
    auto ui = [&] {
        BeginTestWindow();
        open = TreeNode("Узел");
        nodeRect = GetItemRect();
        if (open) {
            Text("Ребёнок");
            TreePop();
        }
        if (Button("Меню"))
            OpenPopup("popup");
        buttonRect = GetItemRect();
        popupVisible = false;
        if (BeginPopup("popup")) {
            popupVisible = true;
            Text("Пункт");
            EndPopup();
        }
        End();
    };
    h.Frame(ui);
    h.Frame(ui);
    ZX_EXPECT(!open);
    h.Move(nodeRect.min + Vec2(8.0f, nodeRect.Height() * 0.5f));
    h.Frame(ui);
    h.Down();
    h.Frame(ui);
    h.Up();
    h.Frame(ui);
    ZX_EXPECT(open);
    // Popup opens on click, stays open, closes on a click outside.
    h.Move(buttonRect.Center());
    h.Frame(ui);
    h.Down();
    h.Frame(ui);
    h.Up();
    h.Frame(ui);
    h.Frame(ui);
    ZX_EXPECT(popupVisible);
    h.Frame(ui);
    ZX_EXPECT(popupVisible);
    h.Move(Vec2(1200.0f, 780.0f));
    h.Frame(ui);
    h.Down();
    h.Frame(ui);
    h.Up();
    h.Frame(ui);
    ZX_EXPECT(!popupVisible);
}

ZX_TEST(UI, WindowMoveAndScale)
{
    Harness h;
    ZX_REQUIRE(h.ok);
    Rect windowRect;
    f32 frameHeight = 0.0f;
    auto ui = [&] {
        SetNextWindowPos(Vec2(50.0f, 50.0f));
        SetNextWindowSize(Vec2(300.0f, 200.0f));
        Begin("Movable");
        windowRect = GetWindowRect();
        frameHeight = GetFrameHeight();
        End();
    };
    h.Frame(ui);
    h.Frame(ui);
    const Vec2 start = windowRect.min;
    h.Move(windowRect.min + Vec2(40.0f, 12.0f)); // title bar
    h.Frame(ui);
    h.Down();
    h.Frame(ui);
    h.Move(windowRect.min + Vec2(140.0f, 62.0f));
    h.Frame(ui);
    h.Up();
    h.Frame(ui);
    ZX_EXPECT_NEAR(windowRect.min.x - start.x, 100.0f, 0.5f);
    ZX_EXPECT_NEAR(windowRect.min.y - start.y, 50.0f, 0.5f);
    // UI scale multiplies all metrics.
    const f32 base = frameHeight;
    h.ctx.SetUserScale(1.5f);
    h.Frame(ui);
    ZX_EXPECT_NEAR(frameHeight / base, 1.5f, 0.08f);
}

ZX_TEST(UI, TableSortAndClipper)
{
    Harness h;
    ZX_REQUIRE(h.ok);
    TableSortSpec spec;
    u32 submitted = 0;
    Rect headerRect;
    auto ui = [&] {
        BeginTestWindow();
        if (BeginTable("t", 2, TableFlags::Borders | TableFlags::Sortable)) {
            TableSetupColumn("A");
            TableSetupColumn("B", 80.0f);
            TableHeadersRow();
            headerRect = GetItemRect();
            spec = TableGetSortSpec();
            ListClipper clip(100000, 0.0f);
            submitted = clip.end - clip.begin;
            for (u32 i = clip.begin; i < clip.end; ++i) {
                TableNextRow();
                TableNextColumn();
                Textf("{}", i);
                TableNextColumn();
                Text("x");
            }
            clip.End();
            EndTable();
        }
        End();
    };
    h.Frame(ui);
    h.Frame(ui);
    ZX_EXPECT_GT(submitted, 5u);
    ZX_EXPECT_LT(submitted, 60u); // only the visible rows of 100k
    ZX_EXPECT_EQ(spec.column, -1);
    // Click the last header cell (column B) -> sorted ascending by B.
    h.Move(headerRect.min + Vec2(10.0f, headerRect.Height() * 0.5f));
    h.Frame(ui);
    h.Down();
    h.Frame(ui);
    h.Up();
    h.Frame(ui);
    ZX_EXPECT_EQ(spec.column, 1);
    ZX_EXPECT(spec.ascending);
    ZX_EXPECT(spec.dirty);
    h.Frame(ui);
    ZX_EXPECT(!spec.dirty);
}

ZX_TEST(UI, DragAndDrop)
{
    Harness h;
    ZX_REQUIRE(h.ok);
    i32 dropped = -1;
    Rect source, target;
    auto ui = [&] {
        BeginTestWindow();
        Button("Источник");
        source = GetItemRect();
        const i32 payload = 42;
        if (BeginDragDropSource("int", &payload, sizeof(payload), "42")) {
        }
        Button("Цель");
        target = GetItemRect();
        if (const void* p = AcceptDragDropPayload("int"))
            dropped = *static_cast<const i32*>(p);
        End();
    };
    h.Frame(ui);
    h.Frame(ui);
    h.Move(source.Center());
    h.Frame(ui);
    h.Down();
    h.Frame(ui);
    h.Move(source.Center() + Vec2(0.0f, 10.0f));
    h.Frame(ui);
    ZX_EXPECT(IsDragDropActive("int"));
    h.Move(target.Center());
    h.Frame(ui);
    h.Up();
    h.Frame(ui);
    ZX_EXPECT_EQ(dropped, 42);
    h.Frame(ui);
    ZX_EXPECT(!IsDragDropActive());
}

ZX_TEST(UI, IdsAndLabels)
{
    Harness h;
    ZX_REQUIRE(h.ok);
    ID a = 0, b = 0, c = 0, d = 0;
    auto ui = [&] {
        BeginTestWindow();
        a = GetID("Кнопка");
        b = GetID("Кнопка##2");
        PushID(7);
        c = GetID("Кнопка");
        PopID();
        PushID(7);
        d = GetID("Кнопка");
        PopID();
        End();
    };
    h.Frame(ui);
    ZX_EXPECT_NE(a, b);
    ZX_EXPECT_NE(a, c);
    ZX_EXPECT_EQ(c, d);
}

ZX_TEST(UI, DockingLayoutAndPersistence)
{
    Harness h;
    ZX_REQUIRE(h.ok);
    Rect left, right;
    bool rightVisible = false;
    auto ui = [&] {
        const ID root = DockSpace("Main", Rect(Vec2(0.0f, 0.0f), Vec2(1000.0f, 600.0f)));
        if (!DockSpaceHasLayout("Main")) {
            ID rest;
            const ID r = DockSplitNode(root, DockDir::Right, 0.3f, &rest);
            DockWindow("Left", rest);
            DockWindow("Right", r);
            DockWindow("RightTab", r);
        }
        Begin("Left");
        left = GetWindowRect();
        End();
        rightVisible = Begin("Right");
        right = GetWindowRect();
        End();
        Begin("RightTab");
        End();
    };
    h.Frame(ui);
    h.Frame(ui);
    h.Frame(ui);
    ZX_EXPECT_NEAR(left.min.x, 0.0f, 1.0f);
    ZX_EXPECT_NEAR(right.max.x, 1000.0f, 1.0f);
    ZX_EXPECT_NEAR(right.Width() / 1000.0f, 0.3f, 0.02f);
    ZX_EXPECT_GT(left.min.y, 10.0f); // below the tab bar
    ZX_EXPECT(rightVisible);         // first tab selected
    const String saved = SaveDockLayout();
    ZX_EXPECT(saved.View().Find("tab ") != StringView::NPos);
    ZX_REQUIRE(LoadDockLayout(saved));
    h.Frame(ui);
    h.Frame(ui);
    ZX_EXPECT_NEAR(right.Width() / 1000.0f, 0.3f, 0.02f);
    ZX_EXPECT(DockSpaceHasLayout("Main"));
    ZX_EXPECT(!LoadDockLayout("node 1 2 3"));
}

ZX_TEST(UI, DockedWindowsTakeTheMouse)
{
    // Docked windows (and their dock host) sit at negative window orders; with no modal open they must still be
    // hovered and clicked (a -1 "no modal" default once made every docked panel of the editor unclickable).
    Harness h;
    ZX_REQUIRE(h.ok);
    u32 clicks = 0;
    Rect button;
    auto ui = [&] {
        const ID root = DockSpace("Main", Rect(Vec2(0.0f, 0.0f), Vec2(1000.0f, 600.0f)));
        if (!DockSpaceHasLayout("Main"))
            DockWindow("Panel", root);
        Begin("Panel");
        if (Button("Press"))
            ++clicks;
        button = GetItemRect();
        End();
    };
    h.Frame(ui);
    h.Frame(ui);
    h.Move(button.Center());
    h.Frame(ui);
    h.Down();
    h.Frame(ui);
    h.Up();
    h.Frame(ui);
    ZX_EXPECT_EQ(clicks, 1u);
}
