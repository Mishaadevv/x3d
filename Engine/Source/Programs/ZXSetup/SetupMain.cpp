// ZXSetup: the ZeqouX3D installer and uninstaller.
//
// One executable = this program + the engine files appended as a .zpak package ("Files/<install path>") + a 24-byte
// trailer pointing at it. Installs per user (no administrator rights) into %LOCALAPPDATA%/Programs/ZeqouX3D by
// default, with shortcuts, an "Apps & features" entry and Uninstall.exe (this program without the package).
//
//   ZXSetup.exe                                         installer window (the UI runs on the engine's own ZUI)
//   ZXSetup.exe -silent -dir=<path> [-nopacks] [-noshortcuts]   installs without a window (tests); exit code 0 = ok
//   ZXSetup.exe -makeinstaller=<staged dir> -out=<exe>  builds an installer: this exe + the staged files
//   Uninstall.exe [-silent]                             removes the installation it sits in
//   -autotest=<frames> -screenshot=<path> -page=<0..2>  screenshots of the installer pages
//   -autotest=<frames> -autoinstall -dir=<path>          installs through the window (screenshot of the result)

#include "SetupWin32.h"

#include "Core/CoreInit.h"
#include "Core/Console/Console.h"
#include "Core/FileSystem/FileSystem.h"
#include "Core/FileSystem/Pak.h"
#include "Core/HAL/PlatformFile.h"
#include "Core/HAL/PlatformMisc.h"
#include "Core/Image/Png.h"
#include "Core/Log/Log.h"
#include "Core/String/Parse.h"
#include "Core/String/Path.h"
#include "Core/Threading/Mutex.h"
#include "Core/Threading/Thread.h"
#include "Core/Time/Time.h"
#include "Core/Version.h"
#include "Platform/Platform.h"
#include "Platform/Window.h"
#include "RHI/ShaderCompiler.h"
#include "RHI_D3D12/D3D12Backend.h"
#include "Render/TextureLoader.h"
#include "UI/Icons.h"
#include "UI/UI.h"
#include "UI/UIRenderer.h"

#include <atomic>
#include <string.h>

ZX_DEFINE_LOG_CATEGORY_STATIC(LogSetup, Info);

using namespace zx;
using namespace zx::rhi;

namespace {

// --- Payload ---------------------------------------------------------------------------------------------------------

constexpr char TrailerMagic[8] = {'Z', 'X', 'S', 'E', 'T', 'U', 'P', '1'};
struct Trailer
{
    u64 offset; // where the package starts (= size of the program in front of it)
    u64 size;
    char magic[8];
};
static_assert(sizeof(Trailer) == 24);

constexpr StringView FilesRoot = "Files";
constexpr StringView PacksDir = "Engine/Content/Packs/";
constexpr StringView ManifestName = "Uninstall.dat";
constexpr StringView UninstallerName = "Uninstall.exe";
constexpr StringView EditorExe = "Binaries/Win64/ZXEditor.exe";

/// Size of the program part of `exe` and the package offset (0 = no package appended).
u64 FindPayload(StringView exe, u64& stubSize)
{
    platform::File f;
    stubSize = 0;
    if (!f.Open(exe, platform::FileMode::Read))
        return 0;
    const u64 size = f.Size();
    stubSize = size;
    Trailer t{};
    if (size < sizeof(t) || f.ReadAt(&t, sizeof(t), size - sizeof(t)) != sizeof(t) || memcmp(t.magic, TrailerMagic, 8) != 0 ||
        t.offset == 0 || t.offset + t.size + sizeof(t) != size)
        return 0;
    stubSize = t.offset;
    return t.offset;
}

bool ReadPrefix(StringView path, u64 bytes, Array<u8>& out)
{
    platform::File f;
    if (!f.Open(path, platform::FileMode::Read))
        return false;
    out.ResizeUninitialized(usize(bytes));
    return f.ReadAt(out.Data(), out.Size(), 0) == out.Size();
}

bool WriteWholeFile(StringView path, ByteSpan data)
{
    platform::CreateDirectoryTree(Path::Parent(path));
    platform::File f;
    if (!f.Open(path, platform::FileMode::Write))
        return false;
    return data.Empty() || f.Write(data.Data(), data.Size()) == data.Size();
}

struct PayloadFile
{
    String path; // relative to the install folder
    u64 size = 0;
};

void ListPayload(const fs::PakArchive& pak, StringView dir, StringView rel, Array<PayloadFile>& out)
{
    Array<platform::DirectoryEntry> entries;
    if (!pak.List(dir, entries))
        return;
    for (const platform::DirectoryEntry& e : entries) {
        const String child = Path::Join(dir, e.name), childRel = rel.Empty() ? String(e.name) : Path::Join(rel, e.name);
        if (e.isDirectory)
            ListPayload(pak, child, childRel, out);
        else
            out.PushBack(PayloadFile{childRel, e.size});
    }
}

void ListDirectoryFiles(StringView root, StringView rel, Array<String>& out)
{
    Array<platform::DirectoryEntry> entries;
    if (!platform::ListDirectory(rel.Empty() ? String(root) : Path::Join(root, rel), entries))
        return;
    for (const platform::DirectoryEntry& e : entries) {
        const String childRel = rel.Empty() ? String(e.name) : Path::Join(rel, e.name);
        if (e.isDirectory)
            ListDirectoryFiles(root, childRel, out);
        else
            out.PushBack(childRel);
    }
}

/// -makeinstaller: this executable + the staged files as a package + the trailer.
int MakeInstaller(StringView staged, StringView outPath)
{
    Array<String> files;
    ListDirectoryFiles(staged, {}, files);
    if (files.Empty()) {
        ZX_LOG_ERROR(LogSetup, "Nothing to package in '{}'", staged);
        return 1;
    }
    const String pakPath = String(outPath) + ".zpak";
    {
        fs::PakWriter w;
        if (!w.Open(pakPath))
            return 1;
        for (const String& rel : files) {
            Array<u8> bytes;
            if (!fs::ReadFile(Path::Join(staged, rel), bytes) || !w.AddFile(Path::Join(FilesRoot, rel), bytes.View())) {
                ZX_LOG_ERROR(LogSetup, "Can't add '{}'", rel);
                return 1;
            }
        }
        if (!w.Finish())
            return 1;
        ZX_LOG_INFO(LogSetup, "Package: {} files, {:.1f} MB -> {:.1f} MB", w.FileCount(), f64(w.RawBytes()) / 1048576.0, f64(w.PackedBytes()) / 1048576.0);
    }
    u64 stubSize = 0;
    FindPayload(platform::GetExecutablePath(), stubSize);
    Array<u8> stub, pak;
    if (!ReadPrefix(platform::GetExecutablePath(), stubSize, stub) || !fs::ReadFile(pakPath, pak))
        return 1;
    Trailer t{};
    t.offset = stub.Size();
    t.size = pak.Size();
    memcpy(t.magic, TrailerMagic, 8);
    platform::File f;
    const bool ok = f.Open(outPath, platform::FileMode::Write) && f.Write(stub.Data(), stub.Size()) == stub.Size() &&
                    f.Write(pak.Data(), pak.Size()) == pak.Size() && f.Write(&t, sizeof(t)) == sizeof(t);
    f.Close();
    platform::DeleteFileAt(pakPath);
    if (!ok) {
        ZX_LOG_ERROR(LogSetup, "Can't write '{}'", outPath);
        return 1;
    }
    ZX_LOG_INFO(LogSetup, "Installer: {} ({:.1f} MB)", outPath, f64(stub.Size() + pak.Size() + sizeof(t)) / 1048576.0);
    return 0;
}

// --- Install / uninstall -----------------------------------------------------------------------------------------------

struct InstallOptions
{
    String dir;
    bool packs = true;
    bool desktopShortcut = true;
    bool startMenuShortcut = true;
};

struct InstallProgress
{
    std::atomic<u64> bytesDone{0};
    u64 bytesTotal = 0;
    std::atomic<bool> cancel{false};
    std::atomic<bool> finished{false};
    bool ok = false;
    Mutex mutex;
    String current; // file being written (guarded)
    String error;
};

String EnginePath(StringView dir, StringView rel) { return Path::Join(dir, rel); }

/// Engine-owned folders inside the install folder (removed on uninstall / replaced on update; projects never live here).
constexpr StringView OwnedDirs[] = {"Binaries", "Engine", "Tools", "Intermediate", "docs"};

bool IsEditorRunning(StringView dir)
{
    // A running exe can't be renamed.
    const String exe = EnginePath(dir, EditorExe);
    if (!platform::FileExists(exe))
        return false;
    const String moved = exe + ".check";
    if (!platform::MoveFileAt(exe, moved, false))
        return true;
    platform::MoveFileAt(moved, exe, false);
    return false;
}

void Install(const fs::PakArchive& pak, u64 stubSize, const InstallOptions& o, InstallProgress& p)
{
    auto fail = [&](String message) {
        ScopedLock lock(p.mutex);
        p.error = move(message);
        ZX_LOG_ERROR(LogSetup, "{}", p.error);
    };
    Array<PayloadFile> files;
    ListPayload(pak, FilesRoot, {}, files);
    if (!o.packs)
        files.RemoveIf([](const PayloadFile& f) { return f.path.View().StartsWith(PacksDir); });
    const bool existed = platform::DirectoryExists(o.dir);
    const bool update = platform::FileExists(EnginePath(o.dir, ManifestName));
    if (IsEditorRunning(o.dir)) {
        fail("ZeqouX3D is running from this folder. Close the editor and try again.");
        p.finished = true;
        return;
    }
    if (!platform::CreateDirectoryTree(o.dir)) {
        fail(String::Format("Can't create the folder {}", o.dir));
        p.finished = true;
        return;
    }
    // An update replaces the engine's folders (files removed in the new version must go); Saved (settings, logs) stays.
    if (update)
        for (StringView d : OwnedDirs)
            if (d != "Intermediate")
                platform::DeleteDirectoryTree(EnginePath(o.dir, d));
    String manifest;
    Array<u8> bytes;
    for (const PayloadFile& f : files) {
        if (p.cancel)
            break;
        {
            ScopedLock lock(p.mutex);
            p.current = f.path;
        }
        if (!pak.Read(Path::Join(FilesRoot, f.path), bytes) || !WriteWholeFile(EnginePath(o.dir, f.path), bytes.View())) {
            fail(String::Format("Can't write {}", EnginePath(o.dir, f.path)));
            break;
        }
        manifest += f.path;
        manifest += "\n";
        p.bytesDone += f.size;
    }
    bool ok = p.error.Empty() && !p.cancel;
    if (ok) {
        // The uninstaller is this program without the package.
        Array<u8> stub;
        manifest += String(UninstallerName) + "\n";
        ok = ReadPrefix(platform::GetExecutablePath(), stubSize, stub) && WriteWholeFile(EnginePath(o.dir, UninstallerName), stub.View()) &&
             WriteWholeFile(EnginePath(o.dir, ManifestName), ByteSpan(reinterpret_cast<const u8*>(manifest.Data()), manifest.Size()));
        if (!ok)
            fail("Can't write the uninstaller");
    }
    if (ok) {
        const String editor = EnginePath(o.dir, EditorExe);
        const String workDir = Path::Parent(editor);
        const String links[2] = {setup::DesktopDir(), setup::StartMenuDir()};
        const bool want[2] = {o.desktopShortcut, o.startMenuShortcut};
        for (u32 i = 0; i < 2; ++i) {
            const String link = Path::Join(links[i], "ZeqouX3D.lnk");
            if (want[i] && !links[i].Empty())
                setup::CreateShortcut(link, editor, workDir, "ZeqouX3D Editor (Beta)");
            else if (!links[i].Empty())
                platform::DeleteFileAt(link); // an update without it
        }
        setup::RegisterInstall(o.dir, EngineVersionString, p.bytesTotal + stubSize);
        ZX_LOG_INFO(LogSetup, "Installed {} files ({:.1f} MB) into {}", files.Size(), f64(p.bytesDone.load()) / 1048576.0, o.dir);
    } else if (!existed) {
        platform::DeleteDirectoryTree(o.dir); // canceled / failed fresh install: leave nothing behind
    }
    p.ok = ok;
    p.finished = true;
}

u64 PayloadBytes(const fs::PakArchive& pak, bool packs)
{
    Array<PayloadFile> files;
    ListPayload(pak, FilesRoot, {}, files);
    u64 total = 0;
    for (const PayloadFile& f : files)
        if (packs || !f.path.View().StartsWith(PacksDir))
            total += f.size;
    return total;
}

int Uninstall(bool silent)
{
    const String dir(platform::GetExecutableDirectory());
    if (!platform::FileExists(EnginePath(dir, ManifestName)) || !platform::FileExists(EnginePath(dir, EditorExe))) {
        if (!silent)
            platform::ShowErrorMessageBox("ZeqouX3D Uninstall", "No ZeqouX3D installation was found next to this uninstaller.");
        return 1;
    }
    if (!silent && !setup::AskYesNo("Uninstall ZeqouX3D",
                                     String::Format("Remove ZeqouX3D {} from\n{}?\n\nYour projects are not removed.", EngineVersionString, dir)))
        return 0;
    if (IsEditorRunning(dir)) {
        if (!silent)
            platform::ShowErrorMessageBox("ZeqouX3D Uninstall", "ZeqouX3D is running. Close the editor and try again.");
        return 1;
    }
    String manifest;
    fs::ReadTextFile(EnginePath(dir, ManifestName), manifest);
    for (StringView d : OwnedDirs)
        platform::DeleteDirectoryTree(EnginePath(dir, d));
    platform::DeleteDirectoryTree(EnginePath(dir, "Saved"));
    StringView rest = manifest.View();
    while (!rest.Empty()) {
        StringView line, next;
        rest.Split('\n', line, next);
        rest = next;
        line = line.Trim();
        if (!line.Empty() && line != UninstallerName && !line.Contains(".."))
            platform::DeleteFileAt(EnginePath(dir, line));
    }
    platform::DeleteFileAt(EnginePath(dir, ManifestName));
    for (const String& folder : {setup::DesktopDir(), setup::StartMenuDir()})
        if (!folder.Empty())
            platform::DeleteFileAt(Path::Join(folder, "ZeqouX3D.lnk"));
    setup::UnregisterInstall();
    setup::DeleteAfterExit(EnginePath(dir, UninstallerName), dir);
    ZX_LOG_INFO(LogSetup, "Uninstalled from {}", dir);
    if (!silent)
        setup::ShowInfo("ZeqouX3D Uninstall", "ZeqouX3D was removed from your computer.");
    return 0;
}

int SilentInstall(const fs::PakArchive& pak, u64 stubSize)
{
    InstallOptions o;
    StringView dir;
    o.dir = CommandLine::GetValue("dir", dir) ? Path::Normalize(platform::GetAbsolutePath(dir)) : setup::DefaultInstallDir();
    o.packs = !CommandLine::HasFlag("nopacks");
    o.desktopShortcut = o.startMenuShortcut = !CommandLine::HasFlag("noshortcuts");
    InstallProgress p;
    p.bytesTotal = PayloadBytes(pak, o.packs);
    Install(pak, stubSize, o, p);
    return p.ok ? 0 : 1;
}

// --- Installer window --------------------------------------------------------------------------------------------------

enum class Page : u8
{
    Welcome,
    License,
    Options,
    Installing,
    Done,
    Failed,
};

struct SetupUI
{
    Page page = Page::Welcome;
    InstallOptions options;
    String license;
    bool accepted = false;
    bool launch = true;
    String previousVersion;
    u64 bytesAll = 0, bytesNoPacks = 0;
    InstallProgress* progress = nullptr;
    Thread worker;
    bool quit = false;
    u32 logoSrv = InvalidDescriptor;
};

String Megabytes(u64 bytes) { return bytes >= (u64(1) << 30) ? String::Format("{:.1f} GB", f64(bytes) / 1073741824.0) : String::Format("{:.0f} MB", f64(bytes) / 1048576.0); }

constexpr f32 SidebarWidth = 236.0f;

void DrawSidebar(SetupUI& s, const Rect& win)
{
    ui::Context& ctx = *ui::GetCurrentContext();
    const f32 k = ctx.GetScale();
    const ui::Theme& c = ctx.GetStyle().colors;
    ui::DrawList& dl = ui::GetWindowDrawList();
    const Rect bar(win.min, Vec2(win.min.x + Round(SidebarWidth * k), win.max.y));
    dl.AddRectGradient(bar, Color(0x17, 0x1D, 0x2B), Color(0x10, 0x11, 0x16), ui::GradientDirection::Vertical);
    dl.AddRect(Rect(Vec2(bar.max.x - 1.0f, bar.min.y), bar.max), c.separator);
    // Brand.
    const f32 logo = Round(84.0f * k);
    const Rect lr(Vec2(bar.Center().x - logo * 0.5f, bar.min.y + Round(34.0f * k)), Vec2(bar.Center().x + logo * 0.5f, bar.min.y + Round(34.0f * k) + logo));
    if (s.logoSrv != InvalidDescriptor)
        dl.AddImage(s.logoSrv, lr);
    f32 y = lr.max.y + Round(14.0f * k);
    dl.AddTextInRect(ctx.Fonts().FindFont("Inter Bold"), 21.0f * k, Rect(Vec2(bar.min.x, y), Vec2(bar.max.x, y + 28.0f * k)), c.text, "ZeqouX3D",
                     ui::TextAlign::Center);
    y += Round(28.0f * k);
    dl.AddTextInRect(ctx.Fonts().FindFont("Inter"), 12.5f * k, Rect(Vec2(bar.min.x, y), Vec2(bar.max.x, y + 18.0f * k)), c.textDim,
                     String::Format("Version {}", EngineVersionString), ui::TextAlign::Center);
    y += Round(26.0f * k);
    const Rect badge(Vec2(bar.Center().x - 30.0f * k, y), Vec2(bar.Center().x + 30.0f * k, y + 20.0f * k));
    dl.AddRect(badge, Color(c.warning.r, c.warning.g, c.warning.b, 40), ui::CornerRadii(10.0f * k), 1.0f, c.warning);
    dl.AddTextInRect(ctx.Fonts().FindFont("Inter Bold"), 11.0f * k, badge, c.warning, "BETA", ui::TextAlign::Center);
    // Steps.
    static const char* const Steps[] = {"Welcome", "License", "Options", "Install", "Finish"};
    const u32 current = Min(u32(s.page), 4u);
    y = badge.max.y + Round(38.0f * k);
    for (u32 i = 0; i < 5; ++i) {
        const bool done = i < current || s.page == Page::Done, active = i == current && s.page != Page::Done;
        const Vec2 dot(bar.min.x + Round(40.0f * k), y + 11.0f * k);
        if (i < 4)
            dl.AddRect(Rect(Vec2(dot.x - 1.0f * k, dot.y + 9.0f * k), Vec2(dot.x + 1.0f * k, dot.y + 25.0f * k)), done ? c.accent : c.border);
        if (done)
            dl.AddCircle(dot, 9.0f * k, c.accent);
        else
            dl.AddCircle(dot, 9.0f * k, active ? Color(0x26, 0x7A, 0xE8, 50) : Color(0, 0, 0, 0), 1.5f * k, active ? c.accentHovered : c.borderStrong);
        if (done)
            dl.AddTextInRect(ctx.Fonts().FindFont("Inter Bold"), 11.0f * k, Rect(dot - Vec2(9.0f * k), dot + Vec2(9.0f * k)), c.accentText, ZX_ICON_CHECK,
                             ui::TextAlign::Center);
        dl.AddTextInRect(ctx.Fonts().FindFont(active ? "Inter SemiBold" : "Inter"), 13.5f * k,
                         Rect(Vec2(dot.x + 20.0f * k, y), Vec2(bar.max.x, y + 22.0f * k)), active ? c.text : done ? c.textDim : c.textDisabled, Steps[i]);
        y += Round(34.0f * k);
    }
    dl.AddTextInRect(ctx.Fonts().FindFont("Inter"), 11.0f * k, Rect(Vec2(bar.min.x, bar.max.y - 34.0f * k), Vec2(bar.max.x, bar.max.y - 14.0f * k)),
                     c.textDisabled, "github.com/Mishaadevv/x3d", ui::TextAlign::Center);
}

/// A tinted box with an icon, a bold title and wrapped text (height fixed by the caller).
void Callout(Color color, StringView icon, StringView title, StringView text, f32 height)
{
    ui::Context& ctx = *ui::GetCurrentContext();
    const f32 k = ctx.GetScale();
    ui::DrawList& dl = ui::GetWindowDrawList();
    const Vec2 p = ui::GetCursorPos();
    const f32 w = ui::GetContentRegionAvail().x;
    const Rect r(p, p + Vec2(w, Round(height * k)));
    dl.AddRect(r, Color(color.r, color.g, color.b, 26), ui::CornerRadii(6.0f * k), 1.0f, Color(color.r, color.g, color.b, 120));
    dl.AddTextInRect(ctx.Fonts().FindFont("Inter"), 16.0f * k, Rect(p + Vec2(14.0f, 12.0f) * k, p + Vec2(36.0f, 32.0f) * k), color, icon);
    dl.AddTextInRect(ctx.Fonts().FindFont("Inter SemiBold"), 14.0f * k, Rect(p + Vec2(42.0f, 12.0f) * k, Vec2(r.max.x, p.y + 32.0f * k)), color, title);
    dl.AddText(ctx.Fonts().FindFont("Inter"), 13.0f * k, p + Vec2(42.0f, 36.0f) * k, ctx.GetStyle().colors.text, text, w - 58.0f * k);
    ui::Dummy(Vec2(w / k, height));
}

void StartInstall(SetupUI& s, const fs::PakArchive& pak, u64 stubSize)
{
    s.progress = new InstallProgress();
    s.progress->bytesTotal = s.options.packs ? s.bytesAll : s.bytesNoPacks;
    s.page = Page::Installing;
    InstallProgress* p = s.progress;
    const InstallOptions o = s.options;
    s.worker.Start("Install", [&pak, stubSize, o, p] { Install(pak, stubSize, o, *p); });
}

void DrawPage(SetupUI& s)
{
    ui::Context& ctx = *ui::GetCurrentContext();
    const f32 k = ctx.GetScale();
    const ui::Theme& c = ctx.GetStyle().colors;
    auto heading = [](StringView text) {
        ui::PushFont(ui::FontStyle::SemiBold, 22.0f);
        ui::Text(text);
        ui::PopFont();
        ui::Spacing();
    };
    switch (s.page) {
    case Page::Welcome:
        heading("Welcome to ZeqouX3D");
        ui::TextWrapped("This installs the ZeqouX3D editor and engine: a 3D engine written from scratch in C++ and Direct3D 12, with "
                        "an editor in the spirit of Unreal Engine and Unity. Make a project from a template and start building your game.");
        ui::Spacing();
        ui::Spacing();
        Callout(c.warning, ZX_ICON_TRIANGLE_ALERT, "This is a beta, not a complete 3D engine yet",
                "Many features are still missing or unfinished. Expect bugs and crashes, and projects that need changes to open in "
                "later versions. Keep backups of your work, and please report what breaks on GitHub.",
                104.0f);
        ui::Spacing();
        ui::Spacing();
        ui::PushFont(ui::FontStyle::SemiBold, 14.0f);
        ui::Text("What you get");
        ui::PopFont();
        ui::BulletText("Editor: scenes, materials, terrain, water, clouds, particles, animation, physics");
        ui::BulletText("Gameplay in ZeqouScript (built in) or C++ (needs Visual Studio 2022 Build Tools)");
        ui::BulletText("Starter Content, project templates and free asset packs (Quaternius, CC0)");
        ui::BulletText("Package Project: export your game as a standalone Windows program");
        ui::Spacing();
        ui::PushTextColor(c.textDim);
        ui::TextWrapped("Requires Windows 10 / 11 64-bit, a DirectX 12 GPU (Intel UHD 620 or better) and 8 GB of RAM.");
        ui::PopTextColor();
        break;
    case Page::License:
        heading("License Agreement");
        ui::TextWrapped("ZeqouX3D is licensed under the PolyForm Strict License 1.0.0. Please read it before installing.");
        ui::Spacing();
        if (ui::BeginChild("##license", Vec2(-1.0f, 318.0f), true))
            ui::TextWrapped(s.license);
        ui::EndChild();
        ui::Spacing();
        ui::Checkbox("I accept the terms of the license", &s.accepted);
        break;
    case Page::Options: {
        heading("Install Options");
        ui::Text("Install location");
        const f32 browse = 96.0f;
        ui::SetNextItemWidth(ui::GetContentRegionAvail().x / k - browse - 8.0f);
        ui::InputText("##dir", s.options.dir);
        ui::SameLine(0.0f, 8.0f);
        if (ui::Button(ZX_ICON_FOLDER_OPEN "  Browse...", Vec2(browse, 0.0f))) {
            const String picked = platform::PickFolderDialog(nullptr, "Install ZeqouX3D to", s.options.dir);
            if (!picked.Empty())
                s.options.dir = Path::Join(Path::Normalize(picked), "ZeqouX3D");
        }
        const u64 need = s.options.packs ? s.bytesAll : s.bytesNoPacks;
        const u64 free = setup::FreeDiskSpace(s.options.dir);
        ui::TextColored(free != 0 && free < need ? c.error : c.textDim,
                        String::Format("Required: {}      Available: {}", Megabytes(need), free ? Megabytes(free) : String("unknown")));
        if (!s.previousVersion.Empty() && Path::Normalize(s.options.dir).EqualsIgnoreCase(Path::Normalize(setup::FindInstalledDir())))
            ui::TextColored(c.accentHovered, String::Format(ZX_ICON_INFO "  ZeqouX3D {} is installed here and will be updated. Your settings are kept.",
                                                            s.previousVersion));
        ui::Spacing();
        ui::Spacing();
        ui::PushFont(ui::FontStyle::SemiBold, 14.0f);
        ui::Text("Components");
        ui::PopFont();
        bool editor = true;
        ui::BeginDisabled();
        ui::Checkbox("ZeqouX3D Editor, engine and Starter Content", &editor);
        ui::EndDisabled();
        ui::Checkbox(String::Format("Asset packs: Medieval Village, Stylized Nature, Survival ({})", Megabytes(s.bytesAll - s.bytesNoPacks)), &s.options.packs);
        ui::ItemTooltip("Free CC0 models by Quaternius, offered when you create a project");
        ui::Spacing();
        ui::PushFont(ui::FontStyle::SemiBold, 14.0f);
        ui::Text("Shortcuts");
        ui::PopFont();
        ui::Checkbox("Desktop shortcut", &s.options.desktopShortcut);
        ui::Checkbox("Start menu shortcut", &s.options.startMenuShortcut);
        break;
    }
    case Page::Installing: {
        heading("Installing...");
        const InstallProgress& p = *s.progress;
        const f32 fraction = p.bytesTotal ? f32(f64(p.bytesDone.load()) / f64(p.bytesTotal)) : 0.0f;
        ui::TextWrapped(String::Format("Copying ZeqouX3D {} to {}", EngineVersionString, s.options.dir));
        ui::Spacing();
        ui::ProgressBar(fraction, Vec2(-1.0f, 22.0f), String::Format("{} %", i32(fraction * 100.0f)));
        String current;
        {
            ScopedLock lock(s.progress->mutex);
            current = s.progress->current;
        }
        ui::TextDisabled(String::Format("{} / {}   {}", Megabytes(p.bytesDone.load()), Megabytes(p.bytesTotal), current));
        if (p.cancel)
            ui::TextColored(c.warning, "Canceling...");
        break;
    }
    case Page::Done:
        heading("ZeqouX3D is ready");
        ui::TextWrapped(String::Format("ZeqouX3D {} was installed to {}.", EngineVersionString, s.options.dir));
        ui::Spacing();
        ui::TextWrapped("Start the editor, choose New Project and pick a template: Empty, First Person, Third Person, Water, Physics, "
                        "Vehicle and more.");
        ui::Spacing();
        ui::Spacing();
        Callout(c.accentHovered, ZX_ICON_INFO, "Remember: this is a beta",
                "Not everything works yet. If something breaks, please open an issue on GitHub with the log from the Saved/Logs folder.",
                84.0f);
        ui::Spacing();
        ui::Checkbox("Launch ZeqouX3D Editor", &s.launch);
        break;
    case Page::Failed: {
        heading(s.progress && s.progress->cancel ? "Installation canceled" : "Installation failed");
        String error;
        if (s.progress) {
            ScopedLock lock(s.progress->mutex);
            error = s.progress->error;
        }
        if (!error.Empty())
            ui::TextColored(c.error, error);
        ui::Spacing();
        ui::TextWrapped("Nothing was changed on your computer, or the previous installation is incomplete - run the installer again.");
        break;
    }
    }
}

/// The button row at the bottom: Back / Next / Cancel.
void DrawButtons(SetupUI& s, const fs::PakArchive& pak, u64 stubSize, const Rect& area)
{
    ui::Context& ctx = *ui::GetCurrentContext();
    const f32 k = ctx.GetScale();
    ui::DrawList& dl = ui::GetWindowDrawList();
    const f32 h = Round(64.0f * k);
    dl.AddRect(Rect(Vec2(area.min.x, area.max.y - h), Vec2(area.max.x, area.max.y - h + 1.0f)), ctx.GetStyle().colors.separator);
    const f32 bw = 104.0f, gap = 8.0f;
    auto at = [&](u32 slot) { // slots from the right
        ui::SetCursorPos(Vec2(area.max.x - (28.0f + f32(slot + 1) * bw + f32(slot) * gap) * k, area.max.y - h + 16.0f * k));
    };
    const bool installing = s.page == Page::Installing;
    if (s.page == Page::Done || s.page == Page::Failed) {
        at(0);
        if (ui::Button(s.page == Page::Done ? "Finish" : "Close", Vec2(bw, 32.0f), ui::ButtonVariant::Primary)) {
            if (s.page == Page::Done && s.launch)
                setup::Launch(EnginePath(s.options.dir, EditorExe), Path::Join(s.options.dir, "Binaries/Win64"));
            s.quit = true;
        }
        return;
    }
    at(0);
    ui::BeginDisabled(installing && s.progress->cancel);
    if (ui::Button("Cancel", Vec2(bw, 32.0f))) {
        if (installing)
            s.progress->cancel = true;
        else
            s.quit = true;
    }
    ui::EndDisabled();
    if (installing)
        return;
    at(1);
    const bool validDir = !s.options.dir.Empty() && Path::IsAbsolute(s.options.dir);
    const bool canNext = (s.page != Page::License || s.accepted) && (s.page != Page::Options || validDir);
    ui::BeginDisabled(!canNext);
    if (ui::Button(s.page == Page::Options ? ZX_ICON_DOWNLOAD "  Install" : "Next", Vec2(bw, 32.0f), ui::ButtonVariant::Primary)) {
        if (s.page == Page::Options) {
            s.options.dir = Path::Normalize(s.options.dir);
            StartInstall(s, pak, stubSize);
        } else
            s.page = Page(u32(s.page) + 1);
    }
    ui::EndDisabled();
    if (s.page != Page::Welcome) {
        at(2);
        if (ui::Button("Back", Vec2(bw, 32.0f)))
            s.page = Page(u32(s.page) - 1);
    }
}

int RunInstaller(const fs::PakArchive& pak, u64 stubSize)
{
    SetupUI s;
    s.options.dir = setup::FindInstalledDir();
    s.previousVersion = setup::FindInstalledVersion();
    if (s.options.dir.Empty())
        s.options.dir = setup::DefaultInstallDir();
    s.bytesAll = PayloadBytes(pak, true);
    s.bytesNoPacks = PayloadBytes(pak, false);
    {
        Array<u8> text;
        if (pak.Read("Files/LICENSE", text))
            s.license = String(StringView(reinterpret_cast<const char*>(text.Data()), text.Size()));
        s.license.ReplaceAll("\r", "");
    }
    StringView pageValue;
    if (CommandLine::GetValue("page", pageValue))
        s.page = Page(Clamp<i64>(ParseInt(pageValue).ValueOr(0), 0, 2));

    WindowDesc wd;
    wd.title = String::Format("ZeqouX3D {} Setup", EngineVersionString);
    wd.width = 880;
    wd.height = 580;
    wd.resizable = false;
    UniquePtr<Window> window(Window::Create(wd));
    if (!window)
        return 1;
    DeviceDesc dd;
    dd.enableDebugLayer = ZX_BUILD_DEBUG != 0;
    UniquePtr<Device> device = d3d12::CreateD3D12Device(dd);
    if (!device) {
        platform::ShowErrorMessageBox("ZeqouX3D Setup", "ZeqouX3D needs a graphics card with DirectX 12 support (Intel UHD 620, "
                                                        "GeForce GTX 900, Radeon RX 400 or newer). No such device was found.");
        return 1;
    }
    UVec2 size = window->GetClientSize();
    UniquePtr<SwapChain> swapChain = device->CreateSwapChain(SwapChainDesc{window->GetNativeHandle(), size.x, size.y, 3, Format::R8G8B8A8_UNORM, true});
    if (!swapChain)
        return 1;
    ui::FontSystem fonts;
    ui::Context ctx(fonts);
    if (!ctx.LoadDefaultFonts()) {
        platform::ShowErrorMessageBox("ZeqouX3D Setup", "The installer is damaged (fonts missing). Download it again.");
        return 1;
    }
    ui::SetCurrentContext(&ctx);
    ctx.GetStyle().colors.childBg = ctx.GetStyle().colors.windowBg;
    ui::UIRenderer uiRenderer(*device);
    if (!uiRenderer.Init(swapChain->GetFormat()))
        return 1;
    Texture* logo = nullptr;
    {
        render::TextureLoadOptions o;
        o.compression = render::TextureCompression::None;
        if (Result<Texture*> t = render::LoadTexture(*device, "/Engine/Branding/ZeqouX3D_Icon.png", o)) {
            logo = t.Value();
            s.logoSrv = logo->GetSrvIndex();
        }
    }

    StringView autotestValue, screenshotPath;
    const bool autotest = CommandLine::GetValue("autotest", autotestValue);
    const u64 autotestFrames = u64(Max<i64>(2, ParseInt(autotestValue).ValueOr(60)));
    const bool takeScreenshot = CommandLine::GetValue("screenshot", screenshotPath);
    Buffer* screenshotBuffer = nullptr;
    u64 screenshotPitch = 0;
    UVec2 screenshotSize;

    InputState input;
    Array<WindowEvent> events;
    FrameTimer timer;
    u64 frames = 0;
    while (!device->IsDeviceLost() && !s.quit) {
        if (!platform::PumpMessages())
            break;
        const f32 dt = timer.Tick();
        events.Clear();
        platform::UpdateInput(*window, input, events);
        if (window->ShouldClose()) {
            if (s.page == Page::Installing)
                s.progress->cancel = true; // finishes the current file, cleans up, then the window closes
            else
                s.quit = true;
        }
        if (autotest && frames == 10 && CommandLine::HasFlag("autoinstall")) {
            StringView dir;
            if (CommandLine::GetValue("dir", dir))
                s.options.dir = Path::Normalize(platform::GetAbsolutePath(dir));
            s.options.desktopShortcut = s.options.startMenuShortcut = false;
            s.launch = false;
            StartInstall(s, pak, stubSize);
        }
        if (s.page == Page::Installing && s.progress->finished) {
            s.worker.Join();
            s.page = s.progress->ok ? Page::Done : Page::Failed;
            if (window->ShouldClose())
                s.quit = true;
        }
        const UVec2 clientSize = window->GetClientSize();
        if (clientSize.x == 0 || clientSize.y == 0 || window->IsMinimized()) {
            Thread::SleepMs(16);
            continue;
        }
        if (clientSize != swapChain->GetSize())
            swapChain->Resize(clientSize.x, clientSize.y);
        const UVec2 sz = swapChain->GetSize();

        ui::FrameInput fi;
        fi.displaySize = Vec2(f32(sz.x), f32(sz.y));
        fi.deltaTime = dt;
        fi.dpiScale = window->GetDpiScale();
        fi.input = &input;
        fi.events = events.View();
        fi.hasFocus = window->IsFocused() || autotest;
        ctx.BeginFrame(fi);
        if (ui::Begin("##Setup", nullptr, ui::WindowFlags::Fullscreen | ui::WindowFlags::NoScrollbar)) {
            const Rect win = ui::GetWindowRect();
            const f32 k = ctx.GetScale();
            DrawSidebar(s, win);
            const Rect content(Vec2(win.min.x + Round(SidebarWidth * k), win.min.y), win.max);
            ui::SetCursorPos(content.min + Vec2(36.0f, 30.0f) * k);
            if (ui::BeginChild("##Page", Vec2((content.Width() / k) - 72.0f, (content.Height() / k) - 30.0f - 64.0f - 12.0f)))
                DrawPage(s);
            ui::EndChild();
            DrawButtons(s, pak, stubSize, content);
        }
        ui::End();
        ctx.EndFrame();
        window->SetCursorShape(ctx.GetCursorShape());

        device->BeginFrame();
        CommandList* cmd = device->BeginCommandList();
        Texture* backBuffer = swapChain->GetCurrentBackBuffer();
        uiRenderer.Prepare(*cmd, fonts, ctx.GetDrawLists());
        const ColorAttachment color{backBuffer, LoadOp::Clear, StoreOp::Store, LinearColor(0.01f, 0.01f, 0.012f, 1)};
        cmd->BeginRenderPass(Span<const ColorAttachment>(&color, 1));
        uiRenderer.Draw(*cmd, sz);
        cmd->EndRenderPass();
        if (takeScreenshot && autotest && frames == autotestFrames - 1 && !screenshotBuffer) {
            screenshotSize = sz;
            screenshotBuffer =
                device->CreateBuffer(BufferDesc{AlignUp(u64(sz.x) * 4, u64(256)) * sz.y, BufferUsage::CopyDest, MemoryType::Readback, 0, "Screenshot"});
            screenshotPitch = cmd->CopyTextureToBuffer(screenshotBuffer, 0, backBuffer);
        }
        cmd->Barrier(backBuffer, ResourceState::Present);
        device->Submit(cmd);
        swapChain->Present();
        device->EndFrame();
        ++frames;
        if (autotest && frames >= autotestFrames && s.page != Page::Installing)
            s.quit = true;
    }
    if (s.worker.IsJoinable()) {
        s.progress->cancel = true;
        s.worker.Join();
    }
    device->WaitIdle();
    if (screenshotBuffer) {
        const bool ok = png::WriteFile(screenshotPath, static_cast<const u8*>(screenshotBuffer->GetMappedData()), screenshotSize.x, screenshotSize.y, 4,
                                       usize(screenshotPitch));
        ZX_LOG_INFO(LogSetup, "Screenshot {} -> {}", ok ? "saved" : "FAILED", screenshotPath);
        device->Destroy(screenshotBuffer);
    }
    if (logo)
        device->Destroy(logo);
    const bool ok = s.progress ? s.progress->ok : false;
    delete s.progress;
    ui::SetCurrentContext(nullptr);
    return s.page == Page::Failed || (CommandLine::HasFlag("autoinstall") && !ok) ? 1 : 0;
}

int Run()
{
    StringView staged, out;
    if (CommandLine::GetValue("makeinstaller", staged)) {
        if (!CommandLine::GetValue("out", out)) {
            ZX_LOG_ERROR(LogSetup, "-makeinstaller needs -out=<exe>");
            return 1;
        }
        return MakeInstaller(Path::Normalize(platform::GetAbsolutePath(staged)), Path::Normalize(platform::GetAbsolutePath(out)));
    }
    const StringView exe = platform::GetExecutablePath();
    u64 stubSize = 0;
    const u64 offset = FindPayload(exe, stubSize);
    if (CommandLine::HasFlag("uninstall") || (offset == 0 && Path::FileName(exe).EqualsIgnoreCase(UninstallerName)))
        return Uninstall(CommandLine::HasFlag("silent"));
    RefPtr<fs::PakArchive> pak = offset ? fs::OpenPak(exe, offset) : RefPtr<fs::PakArchive>();
    if (!pak) {
        platform::ShowErrorMessageBox("ZeqouX3D Setup", "This installer is damaged or incomplete. Download it again.");
        return 1;
    }
    if (CommandLine::HasFlag("silent"))
        return SilentInstall(*pak, stubSize);
    // The window draws with the engine's UI: fonts, logo and shaders come from the package, the shader compiler is
    // unpacked into a temporary folder.
    fs::MountSourceAt("/Engine", fs::MakePakSource(pak, "Files/Engine/Content"), 10);
    fs::MountSourceAt("/Shaders", fs::MakePakSource(pak, "Files/Engine/Shaders"), 10);
    const String bin = Path::Join(platform::GetTempDirectory(), "ZeqouX3D-Setup/bin");
    for (StringView dll : {StringView("dxcompiler.dll"), StringView("dxil.dll")}) {
        Array<u8> bytes;
        if (pak->Read(Path::Join("Files/Binaries/Win64", dll), bytes))
            WriteWholeFile(Path::Join(bin, dll), bytes.View());
    }
    setup::AddDllSearchDirectory(bin);
    if (!ShaderCompiler::Initialize()) {
        platform::ShowErrorMessageBox("ZeqouX3D Setup", "The installer could not start its shader compiler.");
        return 1;
    }
    const int result = RunInstaller(*pak, stubSize);
    ShaderCompiler::Shutdown();
    return result;
}

} // namespace

int main()
{
    const String saved = Path::Join(platform::GetTempDirectory(), "ZeqouX3D-Setup");
    if (!CoreInit(CoreInitParams{.appName = "Setup", .savedDirectory = saved, .mountEngineDirectories = false}))
        return 1;
    platform::InitializePlatform();
    const int result = Run();
    platform::ShutdownPlatform();
    const u64 leaks = CoreShutdown();
    return result != 0 ? result : (leaks == 0 ? 0 : 2);
}
