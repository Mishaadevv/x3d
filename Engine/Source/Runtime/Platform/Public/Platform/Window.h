#pragma once

#include "Core/Containers/Array.h"
#include "Core/Containers/Function.h"
#include "Core/Math/Vector.h"
#include "Core/String/String.h"
#include "Platform/Input.h"

namespace zx {

enum class WindowMode : u8
{
    Windowed,
    BorderlessFullscreen, // covers the monitor, no mode change (default for games)
    Fullscreen,           // exclusive mode is handled by the swapchain; the window behaves like borderless
};

enum class CursorShape : u8
{
    Arrow,
    IBeam,
    Hand,
    ResizeAll,
    ResizeEW,
    ResizeNS,
    ResizeNESW,
    ResizeNWSE,
    NotAllowed,
    Wait,
};

struct WindowDesc
{
    String title = "ZeqouX3D";
    i32 width = 1600;  // client area, in logical (96-DPI) units
    i32 height = 900;
    i32 x = -1; // -1 = centered on the primary monitor
    i32 y = -1;
    WindowMode mode = WindowMode::Windowed;
    bool resizable = true;
    bool visible = true;
    bool customTitleBar = false; // editor: draw our own title bar (hit-testing via callback)
};

enum class WindowEventType : u8
{
    Close,
    Resize,       // client size in pixels changed (size = new size; 0x0 when minimized)
    Move,
    FocusGained,
    FocusLost,
    DpiChanged,   // scale = new DPI scale
    KeyDown,
    KeyUp,
    Char,         // codepoint
    MouseDown,
    MouseUp,
    MouseMove,
    MouseWheel,
    FileDrop,     // paths
};

struct WindowEvent
{
    WindowEventType type;
    Key key = Key::Unknown;
    bool repeat = false;
    MouseButton button = MouseButton::Left;
    KeyModifiers modifiers = KeyModifiers::None;
    u32 codepoint = 0;
    IVec2 position;  // mouse / window position (pixels)
    UVec2 size;      // resize
    f32 wheel = 0.0f;
    f32 wheelH = 0.0f;
    f32 dpiScale = 1.0f;
    Array<String> paths; // FileDrop
};

/// Result of custom title bar hit-testing (editor).
enum class HitTestResult : u8
{
    Client,
    Caption,
    ResizeLeft,
    ResizeRight,
    ResizeTop,
    ResizeBottom,
    ResizeTopLeft,
    ResizeTopRight,
    ResizeBottomLeft,
    ResizeBottomRight,
    MinimizeButton,
    MaximizeButton,
    CloseButton,
};

class Window
{
public:
    static Window* Create(const WindowDesc& desc);
    virtual ~Window() = default;

    /// Native handle (HWND).
    virtual void* GetNativeHandle() const = 0;

    virtual void SetTitle(StringView title) = 0;
    virtual void SetMode(WindowMode mode) = 0;
    virtual WindowMode GetMode() const = 0;
    virtual void Show() = 0;
    virtual void Hide() = 0;
    virtual void Minimize() = 0;
    virtual void Maximize() = 0;
    virtual void Restore() = 0;
    virtual bool IsMinimized() const = 0;
    virtual bool IsMaximized() const = 0;
    virtual bool IsFocused() const = 0;
    virtual void Focus() = 0;

    /// Client area size in pixels.
    virtual UVec2 GetClientSize() const = 0;
    virtual IVec2 GetPosition() const = 0;
    virtual void SetClientSize(u32 width, u32 height) = 0;
    virtual void SetPosition(i32 x, i32 y) = 0;
    /// Pixels per logical unit (1.0 at 96 DPI).
    virtual f32 GetDpiScale() const = 0;

    virtual void SetCursorShape(CursorShape shape) = 0;
    virtual void SetCursorVisible(bool visible) = 0;
    /// Locks the cursor to the window center and hides it (FPS camera / viewport fly mode).
    virtual void SetCursorLocked(bool locked) = 0;
    virtual bool IsCursorLocked() const = 0;

    /// Custom title bar hit testing (only when WindowDesc::customTitleBar). Position in client pixels.
    virtual void SetHitTestCallback(Function<HitTestResult(IVec2 clientPos)> callback) = 0;

    /// Events accumulated since the last call (moved out).
    virtual void TakeEvents(Array<WindowEvent>& out) = 0;
    virtual bool ShouldClose() const = 0;
    virtual void RequestClose() = 0;
    virtual void CancelClose() = 0;
};

namespace platform {

/// Processes OS messages for all windows (main thread). Returns false when WM_QUIT was received.
bool PumpMessages();

/// Updates `input` from a window's events (keys, mouse, text) and polls gamepads. Call once per frame
/// after PumpMessages; events are also returned so UI can consume them.
void UpdateInput(Window& window, InputState& input, Array<WindowEvent>& events);

/// Reads XInput gamepads (dead zones applied).
void PollGamepads(InputState& input);

// Clipboard (UTF-8).
String GetClipboardText();
bool SetClipboardText(StringView text);

struct FileDialogFilter
{
    StringView name;     // "Images"
    StringView patterns; // "*.png;*.jpg"
};
/// Native open/save dialogs. Return empty string/array if cancelled. `initialDirectory` (physical, optional) is the
/// folder shown first.
Array<String> OpenFileDialog(Window* owner, StringView title, Span<const FileDialogFilter> filters, bool multiSelect = false,
                             StringView initialDirectory = {});
String SaveFileDialog(Window* owner, StringView title, Span<const FileDialogFilter> filters, StringView defaultName = {},
                      StringView initialDirectory = {});
String PickFolderDialog(Window* owner, StringView title, StringView initialDirectory = {});

/// Opens a URL or file with the default application / reveals a file in Explorer.
void OpenUrl(StringView url);
void RevealInFileExplorer(StringView path);
/// Runs a command line (no console window) in `workingDirectory` and waits up to `timeoutMs`. Returns false if it could
/// not be started or timed out; `exitCode` receives the process exit code.
bool RunProcess(StringView commandLine, StringView workingDirectory, i32* exitCode = nullptr, u32 timeoutMs = 30000);

} // namespace platform
} // namespace zx
