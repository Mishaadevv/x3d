#include "Core/Memory/Memory.h"
#include "Platform/Input.h"

namespace zx {

const char* KeyName(Key key)
{
    static const char* const names[] = {
        "Unknown", "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O", "P", "Q", "R", "S",
        "T", "U", "V", "W", "X", "Y", "Z", "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "F1", "F2", "F3", "F4",
        "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12", "Escape", "Tab", "CapsLock", "Space", "Enter",
        "Backspace", "Insert", "Delete", "Home", "End", "PageUp", "PageDown", "Left", "Right", "Up", "Down",
        "LeftShift", "RightShift", "LeftControl", "RightControl", "LeftAlt", "RightAlt", "LeftSuper", "RightSuper",
        "`", "-", "=", "[", "]", "\\", ";", "'", ",", ".", "/", "Keypad0", "Keypad1", "Keypad2", "Keypad3",
        "Keypad4", "Keypad5", "Keypad6", "Keypad7", "Keypad8", "Keypad9", "KeypadDecimal", "KeypadDivide",
        "KeypadMultiply", "KeypadSubtract", "KeypadAdd", "KeypadEnter", "PrintScreen", "ScrollLock", "Pause",
        "NumLock", "Menu",
    };
    static_assert(ZX_ARRAY_COUNT(names) == usize(Key::Count), "KeyName table out of sync with Key enum");
    return u32(key) < u32(Key::Count) ? names[u32(key)] : "Invalid";
}

KeyModifiers InputState::Modifiers() const
{
    KeyModifiers m = KeyModifiers::None;
    if (IsKeyDown(Key::LeftShift) || IsKeyDown(Key::RightShift))
        m |= KeyModifiers::Shift;
    if (IsKeyDown(Key::LeftControl) || IsKeyDown(Key::RightControl))
        m |= KeyModifiers::Control;
    if (IsKeyDown(Key::LeftAlt) || IsKeyDown(Key::RightAlt))
        m |= KeyModifiers::Alt;
    if (IsKeyDown(Key::LeftSuper) || IsKeyDown(Key::RightSuper))
        m |= KeyModifiers::Super;
    return m;
}

void InputState::BeginFrame()
{
    MemCopy(m_prevKeys, m_keys, sizeof(m_keys));
    MemCopy(m_prevMouse, m_mouse, sizeof(m_mouse));
    m_mouseDelta = Vec2();
    m_wheel = 0.0f;
    m_wheelH = 0.0f;
    m_textLength = 0;
    m_text[0] = '\0';
}

void InputState::AddText(StringView utf8)
{
    if (m_textLength + utf8.Size() + 1 > sizeof(m_text))
        return;
    MemCopy(m_text + m_textLength, utf8.Data(), utf8.Size());
    m_textLength += u32(utf8.Size());
    m_text[m_textLength] = '\0';
}

void InputState::ReleaseAll()
{
    MemZero(m_keys, sizeof(m_keys));
    MemZero(m_mouse, sizeof(m_mouse));
}

} // namespace zx
