// Windows integration of the installer (registry, shell links, message boxes).

#include "SetupWin32.h"
#include "Core/HAL/PlatformFile.h"
#include "Core/String/Path.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>

namespace zx::setup {

namespace {

constexpr const wchar_t* UninstallKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\ZeqouX3D";
constexpr const wchar_t* AppKey = L"Software\\Zeqou\\ZeqouX3D";

/// UTF-8 -> UTF-16 (null-terminated), '/' -> '\'.
Array<wchar_t> Wide(StringView s, bool backslashes = true)
{
    Array<wchar_t> out;
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.Data(), int(s.Size()), nullptr, 0);
    out.Resize(usize(n) + 1, 0);
    if (n > 0)
        ::MultiByteToWideChar(CP_UTF8, 0, s.Data(), int(s.Size()), out.Data(), n);
    if (backslashes)
        for (wchar_t& c : out)
            if (c == L'/')
                c = L'\\';
    return out;
}

String Narrow(const wchar_t* s)
{
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1)
        return {};
    Array<char> buf;
    buf.Resize(usize(n), 0);
    ::WideCharToMultiByte(CP_UTF8, 0, s, -1, buf.Data(), n, nullptr, nullptr);
    return Path::Normalize(StringView(buf.Data(), usize(n - 1)));
}

String KnownFolder(REFKNOWNFOLDERID id)
{
    PWSTR path = nullptr;
    String out;
    if (SUCCEEDED(::SHGetKnownFolderPath(id, 0, nullptr, &path)))
        out = Narrow(path);
    ::CoTaskMemFree(path);
    return out;
}

String ReadString(HKEY root, const wchar_t* key, const wchar_t* name)
{
    wchar_t buf[1024] = {};
    DWORD size = sizeof(buf) - sizeof(wchar_t);
    if (::RegGetValueW(root, key, name, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS)
        return {};
    return Narrow(buf);
}

void WriteString(HKEY key, const wchar_t* name, StringView value)
{
    const Array<wchar_t> w = Wide(value, false);
    ::RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(w.Data()), DWORD(w.Size() * sizeof(wchar_t)));
}

void WriteDword(HKEY key, const wchar_t* name, DWORD value)
{
    ::RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
}

String Windows(StringView path)
{
    String s(path);
    for (char& c : s)
        if (c == '/')
            c = '\\';
    return s;
}

} // namespace

String DefaultInstallDir() { return Path::Join(platform::GetUserLocalAppDataDirectory(), "Programs/ZeqouX3D"); }

String FindInstalledDir() { return ReadString(HKEY_CURRENT_USER, AppKey, L"InstallDir"); }

String FindInstalledVersion() { return ReadString(HKEY_CURRENT_USER, AppKey, L"Version"); }

bool RegisterInstall(StringView dir, StringView version, u64 sizeBytes)
{
    HKEY key = nullptr;
    if (::RegCreateKeyExW(HKEY_CURRENT_USER, AppKey, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return false;
    WriteString(key, L"InstallDir", Windows(dir));
    WriteString(key, L"Version", version);
    ::RegCloseKey(key);
    if (::RegCreateKeyExW(HKEY_CURRENT_USER, UninstallKey, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return false;
    WriteString(key, L"DisplayName", String::Format("ZeqouX3D {} (Beta)", version));
    WriteString(key, L"DisplayVersion", version);
    WriteString(key, L"Publisher", "Zeqou");
    WriteString(key, L"InstallLocation", Windows(dir));
    WriteString(key, L"DisplayIcon", Windows(Path::Join(dir, "Binaries/Win64/ZXEditor.exe")));
    WriteString(key, L"UninstallString", String::Format("\"{}\" -uninstall", Windows(Path::Join(dir, "Uninstall.exe"))));
    WriteString(key, L"QuietUninstallString", String::Format("\"{}\" -uninstall -silent", Windows(Path::Join(dir, "Uninstall.exe"))));
    WriteString(key, L"URLInfoAbout", "https://github.com/Mishaadevv/x3d");
    WriteDword(key, L"EstimatedSize", DWORD(sizeBytes / 1024));
    WriteDword(key, L"NoModify", 1);
    WriteDword(key, L"NoRepair", 1);
    ::RegCloseKey(key);
    return true;
}

void UnregisterInstall()
{
    ::RegDeleteTreeW(HKEY_CURRENT_USER, UninstallKey);
    ::RegDeleteTreeW(HKEY_CURRENT_USER, AppKey);
    ::RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Zeqou"); // only if empty
}

String DesktopDir() { return KnownFolder(FOLDERID_Desktop); }

String StartMenuDir() { return KnownFolder(FOLDERID_Programs); }

bool CreateShortcut(StringView link, StringView target, StringView workingDir, StringView description)
{
    const HRESULT init = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool ok = false;
    IShellLinkW* shell = nullptr;
    if (SUCCEEDED(::CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, reinterpret_cast<void**>(&shell)))) {
        shell->SetPath(Wide(target).Data());
        shell->SetWorkingDirectory(Wide(workingDir).Data());
        shell->SetDescription(Wide(description, false).Data());
        shell->SetIconLocation(Wide(target).Data(), 0);
        IPersistFile* file = nullptr;
        if (SUCCEEDED(shell->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&file)))) {
            ok = SUCCEEDED(file->Save(Wide(link).Data(), TRUE));
            file->Release();
        }
        shell->Release();
    }
    if (SUCCEEDED(init))
        ::CoUninitialize();
    return ok;
}

u64 FreeDiskSpace(StringView dir)
{
    String d(dir);
    for (u32 i = 0; i < 32 && !d.Empty() && !platform::DirectoryExists(d); ++i) {
        const StringView parent = Path::Parent(d);
        if (parent.Size() == d.Size())
            break;
        d = String(parent);
    }
    ULARGE_INTEGER free = {};
    if (!::GetDiskFreeSpaceExW(Wide(d.Empty() ? StringView("C:/") : d.View()).Data(), &free, nullptr, nullptr))
        return 0;
    return free.QuadPart;
}

void AddDllSearchDirectory(StringView dir) { ::SetDllDirectoryW(Wide(dir).Data()); }

bool AskYesNo(StringView title, StringView text)
{
    return ::MessageBoxW(nullptr, Wide(text, false).Data(), Wide(title, false).Data(), MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND) == IDYES;
}

void ShowInfo(StringView title, StringView text)
{
    ::MessageBoxW(nullptr, Wide(text, false).Data(), Wide(title, false).Data(), MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
}

void DeleteAfterExit(StringView file, StringView dir)
{
    // A hidden cmd waits ~2 s (this process has exited by then), deletes the file and removes the folder if empty.
    String cmd = String::Format("cmd.exe /c ping 127.0.0.1 -n 3 > nul & del /f /q \"{}\" & rmdir \"{}\"", Windows(file), Windows(dir));
    Array<wchar_t> w = Wide(cmd, false);
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    wchar_t system[MAX_PATH] = {};
    ::GetSystemDirectoryW(system, MAX_PATH);
    if (::CreateProcessW(nullptr, w.Data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, system, &si, &pi)) {
        ::CloseHandle(pi.hThread);
        ::CloseHandle(pi.hProcess);
    }
}

bool Launch(StringView exe, StringView workingDir)
{
    Array<wchar_t> cmd = Wide(String::Format("\"{}\"", Windows(exe)), false);
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    if (!::CreateProcessW(nullptr, cmd.Data(), nullptr, nullptr, FALSE, 0, nullptr, Wide(workingDir).Data(), &si, &pi))
        return false;
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    return true;
}

} // namespace zx::setup
