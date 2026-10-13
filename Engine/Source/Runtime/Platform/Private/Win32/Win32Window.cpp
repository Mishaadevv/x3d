#include "Core/Log/Log.h"
#include "Core/String/Utf.h"
#include "Platform/Platform.h"
#include "Platform/Window.h"

#include "Win32/Win32Include.h"

#include <shellapi.h>
#include <windowsx.h>

// windowsx.h macros that collide with Window method names.
#undef IsMinimized
#undef IsMaximized

ZX_DEFINE_LOG_CATEGORY_STATIC(LogWindow, Info);

namespace zx {

namespace {

const wchar_t* const WindowClassName = L"ZeqouX3DWindow";

// --- Key mapping (set-1 scancodes, layout independent) --------------------------------------------------------------

Key ScancodeToKey(u32 scancode, bool extended, WPARAM vk)
{
    if (vk == VK_PAUSE)
        return Key::Pause;
    if (vk == VK_NUMLOCK)
        return Key::NumLock;
    if (extended) {
        switch (scancode) {
        case 0x1C: return Key::KeypadEnter;
        case 0x1D: return Key::RightControl;
        case 0x35: return Key::KeypadDivide;
        case 0x37: return Key::PrintScreen;
        case 0x38: return Key::RightAlt;
        case 0x47: return Key::Home;
        case 0x48: return Key::Up;
        case 0x49: return Key::PageUp;
        case 0x4B: return Key::Left;
        case 0x4D: return Key::Right;
        case 0x4F: return Key::End;
        case 0x50: return Key::Down;
        case 0x51: return Key::PageDown;
        case 0x52: return Key::Insert;
        case 0x53: return Key::Delete;
        case 0x5B: return Key::LeftSuper;
        case 0x5C: return Key::RightSuper;
        case 0x5D: return Key::Menu;
        default: return Key::Unknown;
        }
    }
    static const Key table[0x59] = {
        Key::Unknown, Key::Escape, Key::Num1, Key::Num2, Key::Num3, Key::Num4, Key::Num5, Key::Num6, Key::Num7,
        Key::Num8, Key::Num9, Key::Num0, Key::Minus, Key::Equals, Key::Backspace, Key::Tab, Key::Q, Key::W, Key::E,
        Key::R, Key::T, Key::Y, Key::U, Key::I, Key::O, Key::P, Key::LeftBracket, Key::RightBracket, Key::Enter,
        Key::LeftControl, Key::A, Key::S, Key::D, Key::F, Key::G, Key::H, Key::J, Key::K, Key::L, Key::Semicolon,
        Key::Apostrophe, Key::Grave, Key::LeftShift, Key::Backslash, Key::Z, Key::X, Key::C, Key::V, Key::B, Key::N,
        Key::M, Key::Comma, Key::Period, Key::Slash, Key::RightShift, Key::KeypadMultiply, Key::LeftAlt, Key::Space,
        Key::CapsLock, Key::F1, Key::F2, Key::F3, Key::F4, Key::F5, Key::F6, Key::F7, Key::F8, Key::F9, Key::F10,
        Key::NumLock, Key::ScrollLock, Key::Keypad7, Key::Keypad8, Key::Keypad9, Key::KeypadSubtract, Key::Keypad4,
        Key::Keypad5, Key::Keypad6, Key::KeypadAdd, Key::Keypad1, Key::Keypad2, Key::Keypad3, Key::Keypad0,
        Key::KeypadDecimal, Key::Unknown, Key::Unknown, Key::Unknown, Key::F11, Key::F12,
    };
    return scancode < ZX_ARRAY_COUNT(table) ? table[scancode] : Key::Unknown;
}

KeyModifiers CurrentModifiers()
{
    KeyModifiers m = KeyModifiers::None;
    if (::GetKeyState(VK_SHIFT) & 0x8000)
        m |= KeyModifiers::Shift;
    if (::GetKeyState(VK_CONTROL) & 0x8000)
        m |= KeyModifiers::Control;
    if (::GetKeyState(VK_MENU) & 0x8000)
        m |= KeyModifiers::Alt;
    if ((::GetKeyState(VK_LWIN) | ::GetKeyState(VK_RWIN)) & 0x8000)
        m |= KeyModifiers::Super;
    return m;
}

LPCWSTR CursorResource(CursorShape s)
{
    switch (s) {
    case CursorShape::IBeam: return IDC_IBEAM;
    case CursorShape::Hand: return IDC_HAND;
    case CursorShape::ResizeAll: return IDC_SIZEALL;
    case CursorShape::ResizeEW: return IDC_SIZEWE;
    case CursorShape::ResizeNS: return IDC_SIZENS;
    case CursorShape::ResizeNESW: return IDC_SIZENESW;
    case CursorShape::ResizeNWSE: return IDC_SIZENWSE;
    case CursorShape::NotAllowed: return IDC_NO;
    case CursorShape::Wait: return IDC_WAIT;
    default: return IDC_ARROW;
    }
}

class Win32Window final : public Window
{
public:
    explicit Win32Window(const WindowDesc& desc) : m_desc(desc) {}
    ~Win32Window() override
    {
        SetCursorLocked(false);
        if (m_hwnd)
            ::DestroyWindow(m_hwnd);
    }

    bool Init()
    {
        const HINSTANCE instance = ::GetModuleHandleW(nullptr);
        const Array<wchar_t> title = Utf8ToUtf16(m_desc.title);

        m_style = WS_OVERLAPPEDWINDOW;
        if (!m_desc.resizable)
            m_style &= ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
        const DWORD exStyle = WS_EX_APPWINDOW;

        // Size the window for the DPI of the monitor it will appear on (the primary monitor initially).
        const UINT dpi = ::GetDpiForSystem();
        m_dpiScale = f32(dpi) / 96.0f;
        RECT rect = {0, 0, LONG(f32(m_desc.width) * m_dpiScale), LONG(f32(m_desc.height) * m_dpiScale)};
        ::AdjustWindowRectExForDpi(&rect, m_style, FALSE, exStyle, dpi);
        const i32 w = rect.right - rect.left;
        const i32 h = rect.bottom - rect.top;
        i32 x = m_desc.x, y = m_desc.y;
        if (x < 0 || y < 0) {
            RECT work;
            ::SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
            x = work.left + ((work.right - work.left) - w) / 2;
            y = work.top + ((work.bottom - work.top) - h) / 2;
            if (y < work.top)
                y = work.top;
        }

        m_hwnd = ::CreateWindowExW(exStyle, WindowClassName, title.Data(), m_style, x, y, w, h, nullptr, nullptr,
                                   instance, this);
        if (!m_hwnd) {
            ZX_LOG_ERROR(LogWindow, "CreateWindowEx failed ({})", u32(::GetLastError()));
            return false;
        }
        m_dpiScale = f32(::GetDpiForWindow(m_hwnd)) / 96.0f;
        ::DragAcceptFiles(m_hwnd, TRUE);

        RAWINPUTDEVICE rid = {};
        rid.usUsagePage = 0x01; // generic desktop
        rid.usUsage = 0x02;     // mouse
        rid.hwndTarget = m_hwnd;
        ::RegisterRawInputDevices(&rid, 1, sizeof(rid));

        if (m_desc.customTitleBar) {
            // Trigger WM_NCCALCSIZE so the custom frame takes effect.
            ::SetWindowPos(m_hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
        }
        if (m_desc.mode != WindowMode::Windowed)
            SetMode(m_desc.mode);
        if (m_desc.visible)
            Show();
        return true;
    }

    // --- Window interface ---
    void* GetNativeHandle() const override { return m_hwnd; }

    void SetTitle(StringView title) override
    {
        const Array<wchar_t> w = Utf8ToUtf16(title);
        ::SetWindowTextW(m_hwnd, w.Data());
    }

    void SetMode(WindowMode mode) override
    {
        if (mode == m_mode)
            return;
        if (m_mode == WindowMode::Windowed) {
            m_savedPlacement.length = sizeof(m_savedPlacement);
            ::GetWindowPlacement(m_hwnd, &m_savedPlacement);
        }
        m_mode = mode;
        if (mode == WindowMode::Windowed) {
            ::SetWindowLongPtrW(m_hwnd, GWL_STYLE, LONG_PTR(m_style | WS_VISIBLE));
            ::SetWindowPlacement(m_hwnd, &m_savedPlacement);
            ::SetWindowPos(m_hwnd, nullptr, 0, 0, 0, 0,
                           SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        } else {
            MONITORINFO mi = {};
            mi.cbSize = sizeof(mi);
            ::GetMonitorInfoW(::MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
            ::SetWindowLongPtrW(m_hwnd, GWL_STYLE, LONG_PTR(WS_POPUP | WS_VISIBLE));
            ::SetWindowPos(m_hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                           mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        }
    }
    WindowMode GetMode() const override { return m_mode; }

    void Show() override { ::ShowWindow(m_hwnd, SW_SHOW); }
    void Hide() override { ::ShowWindow(m_hwnd, SW_HIDE); }
    void Minimize() override { ::ShowWindow(m_hwnd, SW_MINIMIZE); }
    void Maximize() override { ::ShowWindow(m_hwnd, SW_MAXIMIZE); }
    void Restore() override { ::ShowWindow(m_hwnd, SW_RESTORE); }
    bool IsMinimized() const override { return ::IsIconic(m_hwnd) != FALSE; }
    bool IsMaximized() const override { return ::IsZoomed(m_hwnd) != FALSE; }
    bool IsFocused() const override { return ::GetForegroundWindow() == m_hwnd; }
    void Focus() override
    {
        ::SetForegroundWindow(m_hwnd);
        ::SetFocus(m_hwnd);
    }

    UVec2 GetClientSize() const override
    {
        RECT r;
        ::GetClientRect(m_hwnd, &r);
        return UVec2(u32(r.right - r.left), u32(r.bottom - r.top));
    }
    IVec2 GetPosition() const override
    {
        RECT r;
        ::GetWindowRect(m_hwnd, &r);
        return IVec2(r.left, r.top);
    }
    void SetClientSize(u32 width, u32 height) override
    {
        RECT rect = {0, 0, LONG(width), LONG(height)};
        const DWORD style = DWORD(::GetWindowLongPtrW(m_hwnd, GWL_STYLE));
        ::AdjustWindowRectExForDpi(&rect, style, FALSE, 0, ::GetDpiForWindow(m_hwnd));
        ::SetWindowPos(m_hwnd, nullptr, 0, 0, rect.right - rect.left, rect.bottom - rect.top,
                       SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    void SetPosition(i32 x, i32 y) override
    {
        ::SetWindowPos(m_hwnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    f32 GetDpiScale() const override { return m_dpiScale; }

    void SetCursorShape(CursorShape shape) override
    {
        m_cursor = shape;
        ApplyCursor();
    }
    void SetCursorVisible(bool visible) override
    {
        m_cursorVisible = visible;
        ApplyCursor();
    }
    void SetCursorLocked(bool locked) override
    {
        if (m_cursorLocked == locked)
            return;
        m_cursorLocked = locked;
        if (locked) {
            ::GetCursorPos(&m_lockRestorePos);
            UpdateCursorClip();
        } else {
            ::ClipCursor(nullptr);
            ::SetCursorPos(m_lockRestorePos.x, m_lockRestorePos.y);
        }
        ApplyCursor();
    }
    bool IsCursorLocked() const override { return m_cursorLocked; }

    void SetHitTestCallback(Function<HitTestResult(IVec2)> callback) override { m_hitTest = move(callback); }

    void TakeEvents(Array<WindowEvent>& out) override
    {
        for (WindowEvent& e : m_events)
            out.PushBack(move(e));
        m_events.Clear();
    }
    bool ShouldClose() const override { return m_shouldClose; }
    void RequestClose() override { m_shouldClose = true; }
    void CancelClose() override { m_shouldClose = false; }

    // --- Message handling ---
    LRESULT HandleMessage(UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg) {
        case WM_CLOSE:
            m_shouldClose = true;
            Push(WindowEvent{WindowEventType::Close});
            return 0;
        case WM_SIZE: {
            WindowEvent e{WindowEventType::Resize};
            e.size = UVec2(LOWORD(lp), HIWORD(lp));
            Push(move(e));
            if (m_cursorLocked)
                UpdateCursorClip();
            return 0;
        }
        case WM_MOVE: {
            WindowEvent e{WindowEventType::Move};
            e.position = IVec2(i16(LOWORD(lp)), i16(HIWORD(lp)));
            Push(move(e));
            if (m_cursorLocked)
                UpdateCursorClip();
            return 0;
        }
        case WM_SETFOCUS:
            Push(WindowEvent{WindowEventType::FocusGained});
            if (m_cursorLocked)
                UpdateCursorClip();
            return 0;
        case WM_KILLFOCUS:
            Push(WindowEvent{WindowEventType::FocusLost});
            ::ClipCursor(nullptr);
            return 0;
        case WM_DPICHANGED: {
            m_dpiScale = f32(HIWORD(wp)) / 96.0f;
            const RECT* suggested = reinterpret_cast<const RECT*>(lp);
            ::SetWindowPos(m_hwnd, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                           suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
            WindowEvent e{WindowEventType::DpiChanged};
            e.dpiScale = m_dpiScale;
            Push(move(e));
            return 0;
        }
        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
            mmi->ptMinTrackSize.x = LONG(320 * m_dpiScale);
            mmi->ptMinTrackSize.y = LONG(200 * m_dpiScale);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1; // the renderer paints everything; avoids flicker
        case WM_SYSCOMMAND:
            if ((wp & 0xFFF0) == SC_KEYMENU)
                return 0; // don't open the system menu on Alt
            break;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
        case WM_KEYUP:
        case WM_SYSKEYUP: {
            const bool down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
            const u32 scancode = (u32(lp) >> 16) & 0xFF;
            const bool extended = (lp & (1 << 24)) != 0;
            Key key = ScancodeToKey(scancode, extended, wp);
            // Windows sends only VK_SHIFT for both shifts when the other is held; scancode disambiguates.
            WindowEvent e{down ? WindowEventType::KeyDown : WindowEventType::KeyUp};
            e.key = key;
            e.repeat = down && (lp & (1 << 30)) != 0;
            e.modifiers = CurrentModifiers();
            Push(move(e));
            if (msg == WM_SYSKEYDOWN && wp == VK_F4)
                break; // keep Alt+F4 working
            if (msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP)
                return 0;
            return 0;
        }
        case WM_CHAR: {
            u32 cp = u32(wp);
            if (cp >= 0xD800 && cp <= 0xDBFF) {
                m_highSurrogate = u16(cp);
                return 0;
            }
            if (cp >= 0xDC00 && cp <= 0xDFFF) {
                if (!m_highSurrogate)
                    return 0;
                cp = 0x10000 + ((u32(m_highSurrogate) - 0xD800) << 10) + (cp - 0xDC00);
                m_highSurrogate = 0;
            }
            WindowEvent e{WindowEventType::Char};
            e.codepoint = cp;
            e.modifiers = CurrentModifiers();
            Push(move(e));
            return 0;
        }
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
        case WM_XBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_RBUTTONUP:
        case WM_MBUTTONUP:
        case WM_XBUTTONUP: {
            const bool down = msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN ||
                              msg == WM_XBUTTONDOWN;
            MouseButton button = MouseButton::Left;
            if (msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP)
                button = MouseButton::Right;
            else if (msg == WM_MBUTTONDOWN || msg == WM_MBUTTONUP)
                button = MouseButton::Middle;
            else if (msg == WM_XBUTTONDOWN || msg == WM_XBUTTONUP)
                button = GET_XBUTTON_WPARAM(wp) == XBUTTON1 ? MouseButton::X1 : MouseButton::X2;
            if (down) {
                if (m_buttonsDown++ == 0)
                    ::SetCapture(m_hwnd);
            } else if (m_buttonsDown > 0 && --m_buttonsDown == 0) {
                ::ReleaseCapture();
            }
            WindowEvent e{down ? WindowEventType::MouseDown : WindowEventType::MouseUp};
            e.button = button;
            e.position = IVec2(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            e.modifiers = CurrentModifiers();
            Push(move(e));
            return msg == WM_XBUTTONDOWN || msg == WM_XBUTTONUP ? TRUE : 0;
        }
        case WM_CAPTURECHANGED:
            m_buttonsDown = 0;
            return 0;
        case WM_MOUSEMOVE: {
            WindowEvent e{WindowEventType::MouseMove};
            e.position = IVec2(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            Push(move(e));
            return 0;
        }
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL: {
            WindowEvent e{WindowEventType::MouseWheel};
            const f32 delta = f32(GET_WHEEL_DELTA_WPARAM(wp)) / f32(WHEEL_DELTA);
            if (msg == WM_MOUSEWHEEL)
                e.wheel = delta;
            else
                e.wheelH = delta;
            POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ::ScreenToClient(m_hwnd, &p);
            e.position = IVec2(p.x, p.y);
            e.modifiers = CurrentModifiers();
            Push(move(e));
            return 0;
        }
        case WM_INPUT: {
            RAWINPUT raw;
            UINT size = sizeof(raw);
            if (::GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) !=
                    UINT(-1) &&
                raw.header.dwType == RIM_TYPEMOUSE && !(raw.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
                m_rawDelta += Vec2(f32(raw.data.mouse.lLastX), f32(raw.data.mouse.lLastY));
            }
            break; // DefWindowProc must run for cleanup
        }
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                ApplyCursor();
                return TRUE;
            }
            break;
        case WM_DROPFILES: {
            HDROP drop = reinterpret_cast<HDROP>(wp);
            WindowEvent e{WindowEventType::FileDrop};
            const UINT count = ::DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < count; ++i) {
                wchar_t path[1024];
                const UINT len = ::DragQueryFileW(drop, i, path, ZX_ARRAY_COUNT(path));
                String p = Utf16ToUtf8(path, len);
                p.ReplaceChar('\\', '/');
                e.paths.PushBack(move(p));
            }
            POINT pt;
            ::DragQueryPoint(drop, &pt);
            e.position = IVec2(pt.x, pt.y);
            ::DragFinish(drop);
            Push(move(e));
            return 0;
        }
        case WM_NCCALCSIZE:
            if (m_desc.customTitleBar && wp == TRUE && m_mode == WindowMode::Windowed) {
                // Keep the resize border but remove the caption: client area = window minus side/bottom borders.
                auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lp);
                const UINT dpi = ::GetDpiForWindow(m_hwnd);
                const int frameX = ::GetSystemMetricsForDpi(SM_CXFRAME, dpi) + ::GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
                const int frameY = ::GetSystemMetricsForDpi(SM_CYFRAME, dpi) + ::GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
                RECT& r = params->rgrc[0];
                r.left += frameX;
                r.right -= frameX;
                r.bottom -= frameY;
                if (IsMaximized())
                    r.top += frameY; // maximized windows extend past the monitor
                return 0;
            }
            break;
        // Custom title bar caption buttons: the app draws them; we handle the clicks so DefWindowProc never paints the
        // classic buttons over the client area (HTMAXBUTTON is still reported for Windows 11 snap layouts).
        case WM_NCMOUSEMOVE:
            if (m_desc.customTitleBar && (wp == HTMINBUTTON || wp == HTMAXBUTTON || wp == HTCLOSE)) {
                POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                ::ScreenToClient(m_hwnd, &p);
                WindowEvent e{WindowEventType::MouseMove};
                e.position = IVec2(p.x, p.y);
                Push(move(e));
                return 0;
            }
            break;
        case WM_NCLBUTTONDOWN:
        case WM_NCLBUTTONDBLCLK:
            if (m_desc.customTitleBar && (wp == HTMINBUTTON || wp == HTMAXBUTTON || wp == HTCLOSE)) {
                m_captionButtonDown = u32(wp);
                return 0;
            }
            break;
        case WM_NCLBUTTONUP:
            if (m_desc.customTitleBar && (wp == HTMINBUTTON || wp == HTMAXBUTTON || wp == HTCLOSE)) {
                if (m_captionButtonDown == u32(wp)) {
                    if (wp == HTMINBUTTON)
                        ::ShowWindow(m_hwnd, SW_MINIMIZE);
                    else if (wp == HTMAXBUTTON)
                        ::ShowWindow(m_hwnd, IsMaximized() ? SW_RESTORE : SW_MAXIMIZE);
                    else
                        ::PostMessageW(m_hwnd, WM_CLOSE, 0, 0);
                }
                m_captionButtonDown = 0;
                return 0;
            }
            m_captionButtonDown = 0;
            break;
        case WM_NCHITTEST:
            if (m_desc.customTitleBar && m_hitTest && m_mode == WindowMode::Windowed) {
                const LRESULT def = ::DefWindowProcW(m_hwnd, msg, wp, lp);
                if (def != HTCLIENT)
                    return def; // borders handled by Windows
                POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                ::ScreenToClient(m_hwnd, &p);
                switch (m_hitTest(IVec2(p.x, p.y))) {
                case HitTestResult::Caption: return HTCAPTION;
                case HitTestResult::ResizeTop: return IsMaximized() ? HTCAPTION : HTTOP;
                case HitTestResult::ResizeTopLeft: return HTTOPLEFT;
                case HitTestResult::ResizeTopRight: return HTTOPRIGHT;
                case HitTestResult::MinimizeButton: return HTMINBUTTON;
                case HitTestResult::MaximizeButton: return HTMAXBUTTON; // enables Win11 snap layouts
                case HitTestResult::CloseButton: return HTCLOSE;
                default: return HTCLIENT;
                }
            }
            break;
        default:
            break;
        }
        return ::DefWindowProcW(m_hwnd, msg, wp, lp);
    }

    Vec2 TakeRawDelta()
    {
        const Vec2 d = m_rawDelta;
        m_rawDelta = Vec2();
        return d;
    }

    HWND m_hwnd = nullptr;

private:
    void Push(WindowEvent e) { m_events.PushBack(move(e)); }

    void ApplyCursor()
    {
        if (!m_cursorVisible || m_cursorLocked) {
            ::SetCursor(nullptr);
            return;
        }
        static HCURSOR cache[16] = {};
        HCURSOR& c = cache[u32(m_cursor)];
        if (!c)
            c = ::LoadCursorW(nullptr, CursorResource(m_cursor));
        ::SetCursor(c);
    }

    void UpdateCursorClip()
    {
        RECT r;
        ::GetClientRect(m_hwnd, &r);
        POINT tl = {r.left, r.top}, br = {r.right, r.bottom};
        ::ClientToScreen(m_hwnd, &tl);
        ::ClientToScreen(m_hwnd, &br);
        // Clip to a small rect in the middle: the cursor can't leave and raw input still reports motion.
        const LONG cx = (tl.x + br.x) / 2, cy = (tl.y + br.y) / 2;
        const RECT clip = {cx - 1, cy - 1, cx + 1, cy + 1};
        ::ClipCursor(&clip);
    }

    WindowDesc m_desc;
    DWORD m_style = 0;
    WindowMode m_mode = WindowMode::Windowed;
    WINDOWPLACEMENT m_savedPlacement = {};
    f32 m_dpiScale = 1.0f;
    Array<WindowEvent> m_events;
    bool m_shouldClose = false;
    u16 m_highSurrogate = 0;
    u32 m_buttonsDown = 0;
    Vec2 m_rawDelta;
    CursorShape m_cursor = CursorShape::Arrow;
    bool m_cursorVisible = true;
    bool m_cursorLocked = false;
    POINT m_lockRestorePos = {};
    Function<HitTestResult(IVec2)> m_hitTest;
    u32 m_captionButtonDown = 0;
};

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        auto* window = static_cast<Win32Window*>(cs->lpCreateParams);
        window->m_hwnd = hwnd;
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, LONG_PTR(window));
        ::EnableNonClientDpiScaling(hwnd);
    }
    auto* window = reinterpret_cast<Win32Window*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (window)
        return window->HandleMessage(msg, wp, lp);
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

namespace platform {

bool RegisterWindowClass()
{
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &WndProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = ::LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1)); // app icon resource id 1 if present
    if (!wc.hIcon)
        wc.hIcon = ::LoadIconW(nullptr, IDI_APPLICATION);
    wc.lpszClassName = WindowClassName;
    return ::RegisterClassExW(&wc) != 0 || ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

void UnregisterWindowClass() { ::UnregisterClassW(WindowClassName, ::GetModuleHandleW(nullptr)); }

bool PumpMessages()
{
    MSG msg;
    while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT)
            return false;
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }
    return true;
}

void UpdateInput(Window& window, InputState& input, Array<WindowEvent>& events)
{
    input.BeginFrame();
    const usize first = events.Size();
    window.TakeEvents(events);
    for (usize i = first; i < events.Size(); ++i) {
        const WindowEvent& e = events[i];
        switch (e.type) {
        case WindowEventType::KeyDown:
            if (e.key != Key::Unknown)
                input.SetKey(e.key, true);
            break;
        case WindowEventType::KeyUp:
            if (e.key != Key::Unknown)
                input.SetKey(e.key, false);
            break;
        case WindowEventType::MouseDown:
            input.SetMouseButton(e.button, true);
            input.SetMousePosition(Vec2(f32(e.position.x), f32(e.position.y)));
            break;
        case WindowEventType::MouseUp:
            input.SetMouseButton(e.button, false);
            break;
        case WindowEventType::MouseMove:
            input.SetMousePosition(Vec2(f32(e.position.x), f32(e.position.y)));
            break;
        case WindowEventType::MouseWheel:
            input.AddWheel(e.wheel, e.wheelH);
            break;
        case WindowEventType::Char:
            if (e.codepoint >= 32 && e.codepoint != 127) {
                char buf[4];
                const usize n = Utf8Encode(e.codepoint, buf);
                input.AddText(StringView(buf, n));
            }
            break;
        case WindowEventType::FocusLost:
            input.ReleaseAll();
            break;
        default:
            break;
        }
    }
    input.AddMouseDelta(static_cast<Win32Window&>(window).TakeRawDelta());
    PollGamepads(input);
}

} // namespace platform

Window* Window::Create(const WindowDesc& desc)
{
    Win32Window* w = New<Win32Window>(desc);
    if (!w->Init()) {
        Delete(w);
        return nullptr;
    }
    return w;
}

} // namespace zx
