// Gamepads (XInput), clipboard, shell dialogs.

#include "Core/String/Utf.h"
#include "Platform/Window.h"

#include "Win32/Win32Include.h"

#include <ShObjIdl.h>
#include <Xinput.h>
#include <shellapi.h>

namespace zx::platform {

// --- Gamepads -------------------------------------------------------------------------------------------------------

namespace {

Vec2 ApplyRadialDeadZone(i16 x, i16 y, i16 deadZone)
{
    Vec2 v(f32(x) / 32767.0f, f32(y) / 32767.0f);
    const f32 len = Length(v);
    const f32 dz = f32(deadZone) / 32767.0f;
    if (len <= dz)
        return Vec2();
    const f32 scaled = Saturate((len - dz) / (1.0f - dz));
    return v * (scaled / len);
}

f32 ApplyTriggerThreshold(u8 v)
{
    if (v <= XINPUT_GAMEPAD_TRIGGER_THRESHOLD)
        return 0.0f;
    return f32(v - XINPUT_GAMEPAD_TRIGGER_THRESHOLD) / f32(255 - XINPUT_GAMEPAD_TRIGGER_THRESHOLD);
}

// Polling a disconnected pad is slow (~1 ms); retry disconnected slots only every second.
u64 g_nextDisconnectedPoll[MaxGamepads] = {};
bool g_wasConnected[MaxGamepads] = {};

} // namespace

void PollGamepads(InputState& input)
{
    const u64 now = ::GetTickCount64();
    for (DWORD i = 0; i < MaxGamepads; ++i) {
        if (!g_wasConnected[i] && now < g_nextDisconnectedPoll[i])
            continue;
        XINPUT_STATE state = {};
        GamepadState pad;
        if (::XInputGetState(i, &state) != ERROR_SUCCESS) {
            g_wasConnected[i] = false;
            g_nextDisconnectedPoll[i] = now + 1000;
            input.SetGamepad(i, pad);
            continue;
        }
        g_wasConnected[i] = true;
        const XINPUT_GAMEPAD& g = state.Gamepad;
        pad.connected = true;
        const WORD masks[] = {XINPUT_GAMEPAD_A,         XINPUT_GAMEPAD_B,          XINPUT_GAMEPAD_X,
                              XINPUT_GAMEPAD_Y,         XINPUT_GAMEPAD_LEFT_SHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER,
                              XINPUT_GAMEPAD_BACK,      XINPUT_GAMEPAD_START,      XINPUT_GAMEPAD_LEFT_THUMB,
                              XINPUT_GAMEPAD_RIGHT_THUMB, XINPUT_GAMEPAD_DPAD_UP,  XINPUT_GAMEPAD_DPAD_DOWN,
                              XINPUT_GAMEPAD_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_RIGHT};
        static_assert(ZX_ARRAY_COUNT(masks) == usize(GamepadButton::Count));
        for (usize b = 0; b < ZX_ARRAY_COUNT(masks); ++b)
            pad.buttons[b] = (g.wButtons & masks[b]) != 0;
        pad.leftStick = ApplyRadialDeadZone(g.sThumbLX, g.sThumbLY, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
        pad.rightStick = ApplyRadialDeadZone(g.sThumbRX, g.sThumbRY, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
        pad.leftTrigger = ApplyTriggerThreshold(g.bLeftTrigger);
        pad.rightTrigger = ApplyTriggerThreshold(g.bRightTrigger);
        input.SetGamepad(i, pad);
    }
}

// --- Clipboard ------------------------------------------------------------------------------------------------------

String GetClipboardText()
{
    String result;
    if (!::OpenClipboard(nullptr))
        return result;
    if (HANDLE h = ::GetClipboardData(CF_UNICODETEXT)) {
        if (const wchar_t* text = static_cast<const wchar_t*>(::GlobalLock(h))) {
            result = Utf16ToUtf8(text);
            ::GlobalUnlock(h);
        }
    }
    ::CloseClipboard();
    result.ReplaceAll("\r\n", "\n");
    return result;
}

bool SetClipboardText(StringView text)
{
    const Array<wchar_t> wide = Utf8ToUtf16(text);
    if (!::OpenClipboard(nullptr))
        return false;
    ::EmptyClipboard();
    const usize bytes = (wide.Size() + 1) * sizeof(wchar_t);
    HGLOBAL mem = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
    bool ok = false;
    if (mem) {
        void* dst = ::GlobalLock(mem);
        MemCopy(dst, wide.Data(), bytes);
        ::GlobalUnlock(mem);
        ok = ::SetClipboardData(CF_UNICODETEXT, mem) != nullptr;
        if (!ok)
            ::GlobalFree(mem);
    }
    ::CloseClipboard();
    return ok;
}

// --- Shell dialogs --------------------------------------------------------------------------------------------------

namespace {

template <typename T>
struct ComPtr
{
    T* p = nullptr;
    ~ComPtr()
    {
        if (p)
            p->Release();
    }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

String ItemPath(IShellItem* item)
{
    PWSTR path = nullptr;
    String result;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
        result = Utf16ToUtf8(path);
        result.ReplaceChar('\\', '/');
        ::CoTaskMemFree(path);
    }
    return result;
}

void ApplyFilters(IFileDialog* dialog, Span<const FileDialogFilter> filters, Array<Array<wchar_t>>& storage)
{
    if (filters.Empty())
        return;
    Array<COMDLG_FILTERSPEC> specs;
    for (const FileDialogFilter& f : filters) {
        storage.PushBack(Utf8ToUtf16(f.name));
        const wchar_t* name = storage.Back().Data();
        storage.PushBack(Utf8ToUtf16(f.patterns));
        specs.PushBack(COMDLG_FILTERSPEC{name, storage.Back().Data()});
    }
    dialog->SetFileTypes(UINT(specs.Size()), specs.Data());
}

HWND OwnerHandle(Window* owner) { return owner ? static_cast<HWND>(owner->GetNativeHandle()) : nullptr; }

void ApplyInitialDirectory(IFileDialog* dialog, StringView directory)
{
    if (directory.Empty())
        return;
    String path(directory);
    path.ReplaceChar('/', '\\');
    const Array<wchar_t> w = Utf8ToUtf16(path);
    ComPtr<IShellItem> folder;
    if (SUCCEEDED(::SHCreateItemFromParsingName(w.Data(), nullptr, IID_PPV_ARGS(&folder))))
        dialog->SetFolder(folder.p);
}

} // namespace

Array<String> OpenFileDialog(Window* owner, StringView title, Span<const FileDialogFilter> filters, bool multiSelect,
                             StringView initialDirectory)
{
    Array<String> result;
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
        return result;
    Array<Array<wchar_t>> storage;
    ApplyFilters(dialog.p, filters, storage);
    const Array<wchar_t> wtitle = Utf8ToUtf16(title);
    dialog->SetTitle(wtitle.Data());
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | (multiSelect ? FOS_ALLOWMULTISELECT : 0));
    ApplyInitialDirectory(dialog.p, initialDirectory);
    if (FAILED(dialog->Show(OwnerHandle(owner))))
        return result;
    ComPtr<IShellItemArray> items;
    if (SUCCEEDED(dialog->GetResults(&items))) {
        DWORD count = 0;
        items->GetCount(&count);
        for (DWORD i = 0; i < count; ++i) {
            ComPtr<IShellItem> item;
            if (SUCCEEDED(items->GetItemAt(i, &item)))
                result.PushBack(ItemPath(item.p));
        }
    }
    return result;
}

String SaveFileDialog(Window* owner, StringView title, Span<const FileDialogFilter> filters, StringView defaultName,
                      StringView initialDirectory)
{
    ComPtr<IFileSaveDialog> dialog;
    if (FAILED(::CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
        return String();
    Array<Array<wchar_t>> storage;
    ApplyFilters(dialog.p, filters, storage);
    const Array<wchar_t> wtitle = Utf8ToUtf16(title);
    dialog->SetTitle(wtitle.Data());
    if (!defaultName.Empty()) {
        const Array<wchar_t> wname = Utf8ToUtf16(defaultName);
        dialog->SetFileName(wname.Data());
    }
    ApplyInitialDirectory(dialog.p, initialDirectory);
    if (FAILED(dialog->Show(OwnerHandle(owner))))
        return String();
    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(&item)))
        return String();
    return ItemPath(item.p);
}

String PickFolderDialog(Window* owner, StringView title, StringView initialDirectory)
{
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
        return String();
    const Array<wchar_t> wtitle = Utf8ToUtf16(title);
    dialog->SetTitle(wtitle.Data());
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    ApplyInitialDirectory(dialog.p, initialDirectory);
    if (FAILED(dialog->Show(OwnerHandle(owner))))
        return String();
    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(&item)))
        return String();
    return ItemPath(item.p);
}

void OpenUrl(StringView url)
{
    const Array<wchar_t> w = Utf8ToUtf16(url);
    ::ShellExecuteW(nullptr, L"open", w.Data(), nullptr, nullptr, SW_SHOWNORMAL);
}

void RevealInFileExplorer(StringView path)
{
    String p(path);
    p.ReplaceChar('/', '\\');
    const String args = String("/select,\"") + p + "\"";
    const Array<wchar_t> w = Utf8ToUtf16(args);
    ::ShellExecuteW(nullptr, L"open", L"explorer.exe", w.Data(), nullptr, SW_SHOWNORMAL);
}

bool RunProcess(StringView commandLine, StringView workingDirectory, i32* exitCode, u32 timeoutMs)
{
    Array<wchar_t> cmd = Utf8ToUtf16(commandLine); // CreateProcessW may modify the buffer
    const Array<wchar_t> dir = Utf8ToUtf16(workingDirectory);
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    if (!::CreateProcessW(nullptr, cmd.Data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                          workingDirectory.Empty() ? nullptr : dir.Data(), &si, &pi))
        return false;
    const bool finished = ::WaitForSingleObject(pi.hProcess, timeoutMs) == WAIT_OBJECT_0;
    DWORD code = 1;
    if (finished)
        ::GetExitCodeProcess(pi.hProcess, &code);
    else
        ::TerminateProcess(pi.hProcess, 1);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    if (exitCode)
        *exitCode = i32(code);
    return finished;
}

} // namespace zx::platform
