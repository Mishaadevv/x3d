#pragma once

#include "Core/Math/Vector.h"

namespace zx {

/// Physical keys (layout independent; named after the US layout position).
enum class Key : u16
{
    Unknown = 0,
    A, B, C, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
    Num0, Num1, Num2, Num3, Num4, Num5, Num6, Num7, Num8, Num9,
    F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    Escape, Tab, CapsLock, Space, Enter, Backspace, Insert, Delete, Home, End, PageUp, PageDown,
    Left, Right, Up, Down,
    LeftShift, RightShift, LeftControl, RightControl, LeftAlt, RightAlt, LeftSuper, RightSuper,
    Grave, Minus, Equals, LeftBracket, RightBracket, Backslash, Semicolon, Apostrophe, Comma, Period, Slash,
    Keypad0, Keypad1, Keypad2, Keypad3, Keypad4, Keypad5, Keypad6, Keypad7, Keypad8, Keypad9,
    KeypadDecimal, KeypadDivide, KeypadMultiply, KeypadSubtract, KeypadAdd, KeypadEnter,
    PrintScreen, ScrollLock, Pause, NumLock, Menu,
    Count
};

const char* KeyName(Key key);

enum class MouseButton : u8
{
    Left,
    Right,
    Middle,
    X1,
    X2,
    Count
};

enum class KeyModifiers : u8
{
    None = 0,
    Shift = 1 << 0,
    Control = 1 << 1,
    Alt = 1 << 2,
    Super = 1 << 3,
};
ZX_ENUM_FLAGS(KeyModifiers)

enum class GamepadButton : u8
{
    A,
    B,
    X,
    Y,
    LeftShoulder,
    RightShoulder,
    Back,
    Start,
    LeftThumb,
    RightThumb,
    DPadUp,
    DPadDown,
    DPadLeft,
    DPadRight,
    Count
};

struct GamepadState
{
    bool connected = false;
    bool buttons[u32(GamepadButton::Count)] = {};
    Vec2 leftStick;  // [-1, 1], dead zone applied
    Vec2 rightStick; // [-1, 1]
    f32 leftTrigger = 0.0f;  // [0, 1]
    f32 rightTrigger = 0.0f; // [0, 1]
};

inline constexpr u32 MaxGamepads = 4;

/// Per-frame input snapshot. Updated by the platform layer from window events.
/// "Pressed"/"Released" are edge states valid for the frame they happened in.
class InputState
{
public:
    bool IsKeyDown(Key k) const { return m_keys[u32(k)]; }
    bool WasKeyPressed(Key k) const { return m_keys[u32(k)] && !m_prevKeys[u32(k)]; }
    bool WasKeyReleased(Key k) const { return !m_keys[u32(k)] && m_prevKeys[u32(k)]; }
    bool IsMouseDown(MouseButton b) const { return m_mouse[u32(b)]; }
    bool WasMousePressed(MouseButton b) const { return m_mouse[u32(b)] && !m_prevMouse[u32(b)]; }
    bool WasMouseReleased(MouseButton b) const { return !m_mouse[u32(b)] && m_prevMouse[u32(b)]; }
    /// Cursor position in window client pixels.
    Vec2 MousePosition() const { return m_mousePos; }
    /// Raw mouse movement this frame (unaccelerated counts, works when the cursor is locked).
    Vec2 MouseDelta() const { return m_mouseDelta; }
    /// Wheel notches this frame (positive = away from user).
    f32 MouseWheel() const { return m_wheel; }
    f32 MouseWheelHorizontal() const { return m_wheelH; }
    KeyModifiers Modifiers() const;
    const GamepadState& Gamepad(u32 index) const { return m_gamepads[index]; }
    bool WasGamepadPressed(u32 index, GamepadButton b) const { return m_gamepads[index].buttons[u32(b)] && !m_prevGamepads[index].buttons[u32(b)]; }
    bool WasGamepadReleased(u32 index, GamepadButton b) const { return !m_gamepads[index].buttons[u32(b)] && m_prevGamepads[index].buttons[u32(b)]; }
    /// Text typed this frame (UTF-8).
    StringView TypedText() const { return StringView(m_text, m_textLength); }

    // --- Called by the platform layer ---
    void BeginFrame();
    void SetKey(Key k, bool down) { m_keys[u32(k)] = down; }
    void SetMouseButton(MouseButton b, bool down) { m_mouse[u32(b)] = down; }
    void SetMousePosition(Vec2 p) { m_mousePos = p; }
    void AddMouseDelta(Vec2 d) { m_mouseDelta += d; }
    void AddWheel(f32 v, f32 h)
    {
        m_wheel += v;
        m_wheelH += h;
    }
    void AddText(StringView utf8);
    void SetGamepad(u32 index, const GamepadState& s) { m_gamepads[index] = s; }
    /// Releases everything (e.g. when the window loses focus).
    void ReleaseAll();

private:
    bool m_keys[u32(Key::Count)] = {};
    bool m_prevKeys[u32(Key::Count)] = {};
    bool m_mouse[u32(MouseButton::Count)] = {};
    bool m_prevMouse[u32(MouseButton::Count)] = {};
    Vec2 m_mousePos;
    Vec2 m_mouseDelta;
    f32 m_wheel = 0.0f;
    f32 m_wheelH = 0.0f;
    GamepadState m_gamepads[MaxGamepads];
    GamepadState m_prevGamepads[MaxGamepads];
    char m_text[256] = {};
    u32 m_textLength = 0;
};

} // namespace zx
