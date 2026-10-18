#pragma once

// Game UI (retained, like Unity's UI / UE's UMG): widgets are entities under a UI Canvas, so they live in the scene
// (Outliner, Details, prefabs, undo, saving) and scripts find them by name.
//
//   UI Canvas    the screen (root); its UI Element lays out the children. Reference Height = how many UI units the
//                screen is tall (everything scales with the window height).
//   UI Element   the box of every widget: size (0 = fit the content), flex layout of its children (direction,
//                justify, align, gap, padding; children grow into free space), or "absolute" placement by anchor /
//                pivot / offset in the parent (HUD corners).
//   UI Panel, UI Text, UI Image, UI Button, UI Progress Bar, UI Slider, UI Toggle, UI Input Field - what the box
//                shows / does.
//   World Space  a canvas can follow its entity in the 3D world (name plates and health bars over tanks / characters):
//                drawn at the projected point, shrinking with distance, hidden behind the camera or beyond Max Distance.
//   Scroll       an element with Scroll on scrolls its children (mouse wheel, scroll bar) and clips them.
//
//   GameUISystem::Layout(world, fonts, screenRect);   // every frame (editing too: the canvas previews in the viewport)
//   GameUISystem::Interact(world, mouse, down, ...);  // while playing: hover, press, drag; events for scripts
//   GameUISystem::Draw(world, fonts, drawList, textures);
//   GameUISystem::SetCamera(viewProj, position);     // before Layout: world-space canvases
//   GameUISystem::TypeText(world, typed, backspace, enter, escape); // keyboard for the focused UI Input Field
//   Scripts: fn onUIClick(name), fn onUIChanged(name, value), fn onUISubmit(name, text) on the widget or its ancestors;
//   entity.setText / text, setValue / value, setVisible / visible; ui.cursor (show the mouse cursor while playing).

#include "Asset/AssetManager.h"
#include "Core/Containers/Function.h"
#include "Core/Math/Color.h"
#include "Core/Math/Geometry.h"
#include "Core/Reflection/Reflection.h"
#include "Scene/ECS/World.h"

namespace zx {

namespace ui {
class FontSystem;
class DrawList;
} // namespace ui

enum class UIDirection : u8
{
    Column,
    Row,
};
enum class UIJustify : u8
{
    Start,
    Center,
    End,
    SpaceBetween,
    SpaceAround,
};
enum class UIAlign : u8
{
    Stretch,
    Start,
    Center,
    End,
};
enum class UITextAlign : u8
{
    Left,
    Center,
    Right,
};
enum class UIFontWeight : u8
{
    Regular,
    Medium,
    SemiBold,
    Bold,
};
ZX_DECLARE_ENUM(UIDirection);
ZX_DECLARE_ENUM(UIJustify);
ZX_DECLARE_ENUM(UIAlign);
ZX_DECLARE_ENUM(UITextAlign);
ZX_DECLARE_ENUM(UIFontWeight);

struct UICanvas
{
    f32 referenceHeight = 1080.0f; // UI units from the top of the screen to the bottom
    i32 order = 0;                 // higher draws on top
    // World space: the canvas floats over its entity (its root UI Element's pivot sits on the projected point).
    bool worldSpace = false;
    Vec3 worldOffset = Vec3(0.0f, 2.5f, 0.0f); // m above / around the entity (world axes)
    f32 maxDistance = 300.0f;                  // m: hidden farther away
    f32 fullSizeDistance = 15.0f;              // m: closer = full size, farther = smaller (0 = always full size)
    f32 minScale = 0.45f;                      // smallest size far away

    // Runtime (not saved).
    bool onScreen = true;
    f32 depth = 0.0f; // m from the camera (world space)
};

struct UIElement
{
    bool visible = true;
    Vec2 size;          // UI units; 0 = fit the content (stretched across by a Stretch parent)
    f32 grow = 0.0f;    // share of the parent's free space along its direction
    Vec4 margin;        // left, top, right, bottom
    // Absolute placement (HUD corners, centered popups) instead of the parent's layout.
    bool absolute = false;
    Vec2 anchor = Vec2(0.5f, 0.5f); // point of the parent (0..1, y down)
    Vec2 pivot = Vec2(0.5f, 0.5f);  // point of this box put on the anchor
    Vec2 offset;                    // UI units from the anchor
    // Layout of the children.
    UIDirection direction = UIDirection::Column;
    UIJustify justify = UIJustify::Start;
    UIAlign align = UIAlign::Stretch;
    f32 gap = 0.0f;
    Vec4 padding; // left, top, right, bottom
    bool clip = false; // children are cut at the box
    bool wrap = false;   // Row: children that don't fit start a new line (tag lists, inventories)
    bool scroll = false; // Column: children scroll (wheel, scroll bar) instead of squeezing; clipped

    // Runtime (not saved).
    Rect rect;     // screen pixels after Layout
    Vec2 measured; // UI units (content size)
    bool hovered = false, pressed = false;
    f32 scrollOffset = 0.0f;  // UI units scrolled (Scroll)
    f32 contentHeight = 0.0f; // UI units of the scrolled content
};

struct UIPanel
{
    LinearColor color = LinearColor(0.02f, 0.025f, 0.035f, 0.78f);
    f32 cornerRadius = 10.0f;
    LinearColor borderColor = LinearColor(1.0f, 1.0f, 1.0f, 0.0f);
    f32 borderWidth = 0.0f;
    f32 shadow = 0.0f; // blur of a drop shadow (0 = none)
};

struct UIText
{
    String text = "Text";
    f32 fontSize = 32.0f; // UI units
    LinearColor color = LinearColor(1.0f, 1.0f, 1.0f, 1.0f);
    UIFontWeight weight = UIFontWeight::Medium;
    UITextAlign align = UITextAlign::Left;
    bool wrap = false;   // break lines at the box width
    bool shadow = true;  // a soft dark outline for readability over the game
};

struct UIImage
{
    asset::AssetId texture;
    LinearColor tint = LinearColor(1.0f, 1.0f, 1.0f, 1.0f);
    f32 cornerRadius = 0.0f;
};

struct UIButton
{
    LinearColor color = LinearColor(0.12f, 0.14f, 0.18f, 0.9f);
    LinearColor hoverColor = LinearColor(0.2f, 0.36f, 0.62f, 0.95f);
    LinearColor pressedColor = LinearColor(0.1f, 0.24f, 0.45f, 1.0f);
    f32 cornerRadius = 10.0f;
    bool interactable = true;
    String clickSound = "StarterContent/Sounds/UI/Click";
};

struct UIProgressBar
{
    f32 value = 0.5f; // 0..1
    LinearColor backColor = LinearColor(0.0f, 0.0f, 0.0f, 0.55f);
    LinearColor fillColor = LinearColor(0.2f, 0.85f, 0.35f, 1.0f);
    f32 cornerRadius = 6.0f;
};

struct UISlider
{
    f32 value = 0.5f;
    f32 minValue = 0.0f, maxValue = 1.0f;
    LinearColor trackColor = LinearColor(0.0f, 0.0f, 0.0f, 0.55f);
    LinearColor fillColor = LinearColor(0.25f, 0.55f, 1.0f, 1.0f);
    LinearColor handleColor = LinearColor(1.0f, 1.0f, 1.0f, 1.0f);
    bool interactable = true;
};

struct UIToggle
{
    bool on = false;
    LinearColor offColor = LinearColor(0.0f, 0.0f, 0.0f, 0.55f);
    LinearColor onColor = LinearColor(0.25f, 0.55f, 1.0f, 1.0f);
    LinearColor knobColor = LinearColor(1.0f, 1.0f, 1.0f, 1.0f);
    bool interactable = true;
};

/// A text box: click to type (chat, names, passwords). Enter submits (onUISubmit), Escape leaves it.
struct UIInputField
{
    String text;
    String placeholder = "Type here...";
    i32 maxLength = 64;     // characters
    bool password = false;  // shown as dots
    f32 fontSize = 28.0f;
    LinearColor textColor = LinearColor(1.0f, 1.0f, 1.0f, 1.0f);
    LinearColor placeholderColor = LinearColor(1.0f, 1.0f, 1.0f, 0.4f);
    LinearColor backColor = LinearColor(0.0f, 0.0f, 0.0f, 0.55f);
    LinearColor focusColor = LinearColor(0.25f, 0.55f, 1.0f, 1.0f); // border while typing
    f32 cornerRadius = 8.0f;
    bool clearOnSubmit = false; // chat: empty after Enter
    bool interactable = true;
};

ZX_DECLARE_TYPE(UICanvas);
ZX_DECLARE_TYPE(UIInputField);
ZX_DECLARE_TYPE(UIElement);
ZX_DECLARE_TYPE(UIPanel);
ZX_DECLARE_TYPE(UIText);
ZX_DECLARE_TYPE(UIImage);
ZX_DECLARE_TYPE(UIButton);
ZX_DECLARE_TYPE(UIProgressBar);
ZX_DECLARE_TYPE(UISlider);
ZX_DECLARE_TYPE(UIToggle);

struct UIEvent
{
    enum class Type : u8
    {
        Click,
        Changed,
        Submit, // Enter in a UI Input Field (text = its text)
    };
    Type type = Type::Click;
    ecs::Entity entity;
    f32 value = 0.0f;
    String text;
};

class GameUISystem
{
public:
    /// Lays out every canvas on `screen` (target pixels).
    void Layout(ecs::World& world, ui::FontSystem& fonts, const Rect& screen);
    /// Camera of the frame (world-space canvases); without one they stay hidden.
    void SetCamera(const Mat4& viewProj, Vec3 position)
    {
        m_viewProj = viewProj;
        m_cameraPosition = position;
        m_hasCamera = true;
    }
    /// Mouse in target pixels: hover, clicks (on release over the pressed widget), slider drags, toggles, input field
    /// focus; `wheel` (notches) scrolls the Scroll element under the mouse.
    void Interact(ecs::World& world, Vec2 mouse, bool down, bool pressed, bool released, f32 wheel = 0.0f);
    /// Keyboard for the focused UI Input Field: typed text (UTF-8), Backspace, Enter (submit), Escape (leave).
    void TypeText(ecs::World& world, StringView typed, bool backspace, bool enter, bool escape);
    /// An input field has the keyboard (the game should ignore its keys meanwhile).
    bool WantsKeyboard() const { return bool(m_focused); }
    ecs::Entity FocusedInput() const { return m_focused; }
    /// Seconds for the caret blink (hosts advance it).
    f32 time = 0.0f;
    /// Draws every visible canvas (lowest order first). `textureSrv` resolves UI Image textures (0 / invalid: skip).
    void Draw(ecs::World& world, ui::FontSystem& fonts, ui::DrawList& dl, const Function<u32(asset::AssetId)>& textureSrv);
    /// Events since the last ClearEvents (Interact adds them).
    Span<const UIEvent> Events() const { return m_events.View(); }
    void ClearEvents() { m_events.Clear(); }
    /// True while the mouse is over an interactive widget (the game may ignore clicks then).
    bool MouseOverUI() const { return m_mouseOver; }
    /// Scripts ask for the mouse cursor (menus); the host frees and shows it while playing.
    bool cursorRequested = false;
    void Reset()
    {
        m_events.Clear();
        m_pressed = {};
        m_focused = {};
        cursorRequested = false;
    }

private:
    Array<UIEvent> m_events;
    ecs::Entity m_pressed;
    ecs::Entity m_focused;
    bool m_mouseOver = false;
    Mat4 m_viewProj = Mat4::Identity();
    Vec3 m_cameraPosition;
    bool m_hasCamera = false;
};

/// Linear UI color -> display (sRGB) color for the 2D renderer.
Color ToDisplayColor(const LinearColor& c);

} // namespace zx
