// File > Package Project: cooks the project for the packaged paths, writes Content.zpak and copies the ZXGame runtime
// into a standalone folder (Engine/Packaging.h). Runs on a worker thread; the window shows progress and the result.

#include "Core/Console/Console.h"
#include "Core/HAL/PlatformFile.h"
#include "Core/HAL/PlatformMisc.h"
#include "Core/String/Path.h"
#include "Editor.h"
#include "Engine/Packaging.h"
#include "UI/Icons.h"

ZX_DEFINE_LOG_CATEGORY_STATIC(LogPackage, Info);

namespace zx::editor {

namespace {

constexpr StringView PackageTitle = ZX_ICON_PACKAGE "  Package Project";

/// File-system safe executable name.
String ExecutableName(StringView name)
{
    String out;
    for (char c : name)
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == ' ')
            out.Append(c);
    return out.Empty() ? String("Game") : out;
}

} // namespace

String Editor::DefaultPackageDirectory() const
{
    return Path::Join(project.root, String::Format("Builds/Windows/{}", ExecutableName(project.descriptor.name)));
}

bool Editor::IsPackaging() const { return packageJob && !packageJob->done.load(); }

bool Editor::StartPackage(StringView outputDir)
{
    if (IsPackaging() || project.root.Empty())
        return false;
    EndPlay();
    if (packageJob && packageJob->thread.IsJoinable())
        packageJob->thread.Join();
    // The startup scene (project setting), else the open scene; unsaved edits are saved first.
    String scene = project.descriptor.startupScene.Empty() ? String() : Path::Join(project.root, project.descriptor.startupScene);
    if (scene.Empty() || !platform::FileExists(scene))
        scene = scenePath;
    if (!scenePath.Empty() && undo.IsDirty())
        SaveSceneTo(scenePath);
    if (scene.Empty()) {
        ZX_LOG_ERROR(LogPackage, "Package: save the scene first (File > Save Scene)");
        return false;
    }
    PackageOptions o;
    o.name = ExecutableName(project.descriptor.name);
    o.projectRoot = project.root;
    o.contentDir = contentRoot;
    o.engineRoot = engineRoot;
    o.binariesDir = String(platform::GetExecutableDirectory());
    o.outputDir = outputDir.Empty() ? DefaultPackageDirectory() : String(outputDir);
    o.startupScene = Path::MakeRelative(scene, contentRoot);
    if (game && platform::FileExists(game->DllPath()))
        o.gameModuleDll = game->DllPath();
    o.textureMaxSize = packageLiteTextures ? 1024u : 0u;
    o.fullscreen = packageFullscreen;
    o.disabledFeatures = project.descriptor.disabledFeatures;
    packageJob.Reset(new PackageJob());
    PackageJob* job = packageJob.Get();
    job->outputDir = o.outputDir;
    ZX_LOG_INFO(LogPackage, "Packaging '{}' into {}", o.name, o.outputDir);
    job->thread.Start("Package", [job, o] {
        Result<PackageReport> r = PackageProject(o, [job](StringView line) {
            ScopedLock lock(job->mutex);
            job->lines.PushBack(String(line));
        });
        {
            ScopedLock lock(job->mutex);
            if (r)
                job->report = r.Value();
            else
                job->error = r.GetError().message;
        }
        job->done.store(true);
    });
    showPackageWindow = true;
    return true;
}

void Editor::UpdatePackage()
{
    if (!packageJob || !packageJob->done.load() || !packageJob->thread.IsJoinable())
        return;
    packageJob->thread.Join();
    if (!packageJob->error.Empty()) {
        ZX_LOG_ERROR(LogPackage, "{}", packageJob->error);
        selfTestFailed |= m_autotestFrames > 0; // tests that package must not fail silently
        PlayUISound("Error");
    } else {
        ZX_LOG_INFO(LogPackage, "Package ready: {}", packageJob->report.executable);
        PlayUISound("Confirm");
    }
}

void DrawPackagePanel(Editor& ed)
{
    if (!ed.showPackageWindow)
        return;
    ui::SetNextWindowSize(Vec2(520.0f, 360.0f), ui::Cond::FirstUse);
    if (!ui::Begin(PackageTitle, &ed.showPackageWindow)) {
        ui::End();
        return;
    }
    if (ed.project.root.Empty()) {
        ui::TextDisabled("Open a project to package it.");
        ui::End();
        return;
    }
    if (ed.packageOutput.Empty())
        ed.packageOutput = ed.DefaultPackageDirectory();
    ui::TextDisabled("A standalone game: the executable, Content.zpak and Game.json in one folder.");
    ui::Text("Output folder");
    ui::SetNextItemWidth(-80.0f);
    ui::InputText("##packageOutput", ed.packageOutput);
    ui::SameLine(0.0f, 6.0f);
    if (ui::Button("Browse...")) {
        const String picked = platform::PickFolderDialog(ed.window.Get(), "Package into", ed.packageOutput);
        if (!picked.Empty())
            ed.packageOutput = picked;
    }
    ui::Checkbox("Fullscreen (borderless)", &ed.packageFullscreen);
    ui::Checkbox("Lite textures (max 1024 px: smaller package, low-end GPUs)", &ed.packageLiteTextures);
    const bool busy = ed.IsPackaging();
    if (busy) {
        ui::Spinner();
        ui::SameLine(0.0f, 8.0f);
        ui::Text("Packaging...");
    } else if (ui::Button(ZX_ICON_PACKAGE "  Package", Vec2(), ui::ButtonVariant::Primary)) {
        ed.StartPackage(ed.packageOutput);
    }
    if (ed.packageJob) {
        PackageJob& job = *ed.packageJob;
        ui::Separator();
        ScopedLock lock(job.mutex);
        for (const String& line : job.lines)
            ui::TextWrapped(line);
        if (job.done.load()) {
            if (!job.error.Empty()) {
                ui::TextWrapped(String::Format(ZX_ICON_CIRCLE_X "  {}", job.error));
            } else {
                ui::Text(String::Format(ZX_ICON_CIRCLE_CHECK "  {}", Path::FileName(job.report.executable)));
                if (ui::Button(ZX_ICON_FOLDER_OPEN "  Open Folder"))
                    platform::RevealInFileExplorer(job.report.executable);
            }
        }
    }
    ui::End();
}

} // namespace zx::editor
