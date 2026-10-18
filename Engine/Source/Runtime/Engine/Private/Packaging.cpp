#include "Engine/Packaging.h"
#include "Asset/AssetManager.h"
#include "Core/FileSystem/Pak.h"
#include "Core/Log/Log.h"
#include "Core/Serialization/Json.h"
#include "Core/String/Path.h"
#include "Core/Time/Time.h"

ZX_DEFINE_LOG_CATEGORY_STATIC(LogPackage, Info);

namespace zx {

namespace {

bool IsAssetSource(StringView path)
{
    for (StringView ext : {"gltf", "glb", "obj", "png", "jpg", "jpeg", "tga", "bmp", "hdr", "exr", "dds"})
        if (Path::HasExtension(path, ext))
            return true;
    return false;
}

/// Every file under a physical directory (paths relative to it).
void ListFiles(const String& root, const String& rel, Array<String>& out)
{
    Array<platform::DirectoryEntry> entries;
    platform::ListDirectory(rel.Empty() ? root : Path::Join(root, rel), entries);
    for (const platform::DirectoryEntry& e : entries) {
        const String child = rel.Empty() ? e.name : Path::Join(rel, e.name);
        if (e.isDirectory)
            ListFiles(root, child, out);
        else
            out.PushBack(child);
    }
}

/// Mounts for the cook, removed again when done.
struct CookMounts
{
    CookMounts(const String& content, const String& ddc)
    {
        fs::Mount("/Game", content, 100, false);
        fs::Mount("/DDC", ddc, 100, true);
    }
    ~CookMounts()
    {
        fs::Unmount("/Game");
        fs::Unmount("/DDC");
    }
};

} // namespace

Result<PackageReport> PackageProject(const PackageOptions& o, const Function<void(StringView)>& status)
{
    Stopwatch timer;
    auto say = [&](StringView line) {
        ZX_LOG_INFO(LogPackage, "{}", line);
        if (status)
            status(line);
    };
    PackageReport report;
    if (o.name.Empty() || o.outputDir.Empty() || !platform::DirectoryExists(o.contentDir))
        return Error::Make(ErrorCode::InvalidArgument, "Package: missing name, output folder or content folder");
    if (o.startupScene.Empty() || !platform::FileExists(Path::Join(o.contentDir, o.startupScene)))
        return Error::Make(ErrorCode::NotFound, "Package: the startup scene '{}' does not exist (save a scene first)", o.startupScene);
    const String runtimeExe = Path::Join(o.binariesDir, "ZXGame.exe");
    if (!platform::FileExists(runtimeExe))
        return Error::Make(ErrorCode::NotFound, "Package: {} is missing (build the ZXGame target)", runtimeExe);
    if (Path::IsUnder(Path::Normalize(o.outputDir), Path::Normalize(o.contentDir)))
        return Error::Make(ErrorCode::InvalidArgument, "Package: the output folder must not be inside Content");

    // 1. Cook every asset source of the project for the packaged paths, into a clean staging cache.
    const String staging = Path::Join(o.projectRoot, "Intermediate/PackageDDC");
    platform::DeleteDirectoryTree(staging);
    platform::CreateDirectoryTree(staging);
    Array<String> contentFiles;
    ListFiles(o.contentDir, String(), contentFiles);
    {
        CookMounts mounts(o.contentDir, staging);
        asset::AssetManagerSettings settings;
        settings.ddcDirectory = "/DDC";
        settings.textureMaxSize = o.textureMaxSize;
        settings.writeMetaFiles = false;
        asset::AssetManager cooker(settings);
        u32 sources = 0;
        for (const String& rel : contentFiles)
            if (IsAssetSource(rel)) {
                cooker.LoadPath(Path::Join("/Game", rel));
                ++sources;
            }
        say(String::Format("Cooking {} asset sources...", sources));
        // Sub-assets (meshes, materials, textures of models) appear as their sources cook: load until nothing new.
        usize known = 0;
        for (u32 round = 0; round < 8; ++round) {
            cooker.WaitIdle();
            Array<asset::AssetId> ids;
            cooker.GetAssetIds(ids);
            if (ids.Size() == known)
                break;
            known = ids.Size();
            for (const asset::AssetId& id : ids)
                cooker.Load(id);
            cooker.WaitIdle();
            for (const asset::AssetId& id : ids)
                cooker.ReleaseCpuData(id);
        }
        u32 failed = 0;
        Array<asset::AssetId> ids;
        cooker.GetAssetIds(ids);
        for (const asset::AssetId& id : ids)
            if (cooker.GetState(id) == asset::AssetState::Failed)
                ++failed;
        report.cookedAssets = u32(ids.Size()) - failed;
        say(String::Format("Cooked {} assets{}", report.cookedAssets, failed ? String::Format(" ({} failed - see the log)", failed) : String()));
    }

    // 2. The package.
    platform::CreateDirectoryTree(o.outputDir);
    const String pakPath = Path::Join(o.outputDir, "Content.zpak");
    fs::PakWriter pak;
    if (!pak.Open(pakPath))
        return Error::Make(ErrorCode::AccessDenied, "Package: can't write {}", pakPath);
    auto addTree = [&](const String& physicalRoot, StringView pakRoot) -> bool {
        if (!platform::DirectoryExists(physicalRoot))
            return true;
        Array<String> files;
        ListFiles(physicalRoot, String(), files);
        for (const String& rel : files) {
            Array<u8> bytes;
            platform::FileInfo info;
            const String full = Path::Join(physicalRoot, rel);
            if (!fs::ReadFile(full, bytes) || !pak.AddFile(String::Format("{}/{}", pakRoot, rel), bytes.View(), platform::GetFileInfo(full, info) ? info.modifiedTime : 0))
                return false;
        }
        return true;
    };
    say("Writing Content.zpak...");
    bool ok = addTree(o.contentDir, "Game") && addTree(staging, "DDC") && addTree(Path::Join(o.engineRoot, "Engine/Shaders"), "Shaders") &&
              addTree(Path::Join(o.engineRoot, "Engine/Content/Fonts"), "Fonts") &&
              addTree(Path::Join(o.engineRoot, "Saved/ShaderCache"), "ShaderCache");
    const String inputConfig = Path::Join(o.projectRoot, "Config/Input.json");
    if (ok && platform::FileExists(inputConfig)) {
        Array<u8> bytes;
        ok = fs::ReadFile(inputConfig, bytes) && pak.AddFile("Project/Config/Input.json", bytes.View());
    }
    report.files = pak.FileCount();
    report.rawBytes = pak.RawBytes();
    ok = pak.Finish() && ok;
    if (!ok)
        return Error::Make(ErrorCode::AccessDenied, "Package: writing {} failed (disk full?)", pakPath);
    platform::FileInfo pakInfo;
    report.packageBytes = platform::GetFileInfo(pakPath, pakInfo) ? pakInfo.size : 0;

    // 3. The runtime, its DLLs and the configuration.
    say("Copying the runtime...");
    report.executable = Path::Join(o.outputDir, o.name + ".exe");
    if (!platform::CopyFileAt(runtimeExe, report.executable, true))
        return Error::Make(ErrorCode::AccessDenied, "Package: can't write {}", report.executable);
    for (StringView dll : {"dxcompiler.dll", "dxil.dll"})
        if (platform::FileExists(Path::Join(o.binariesDir, dll)))
            platform::CopyFileAt(Path::Join(o.binariesDir, dll), Path::Join(o.outputDir, dll), true);
    String module;
    if (!o.gameModuleDll.Empty() && platform::FileExists(o.gameModuleDll)) {
        module = String(Path::FileName(o.gameModuleDll));
        platform::CopyFileAt(o.gameModuleDll, Path::Join(o.outputDir, module), true);
    }
    JsonValue config = JsonValue::MakeObject();
    config.Set("name", JsonValue(o.name));
    config.Set("scene", JsonValue(String::Format("/Game/{}", o.startupScene)));
    config.Set("module", JsonValue(module));
    config.Set("textureMaxSize", JsonValue(i64(o.textureMaxSize)));
    config.Set("fullscreen", JsonValue(o.fullscreen));
    JsonValue resolution = JsonValue::MakeArray();
    resolution.PushBack(JsonValue(i64(1600)));
    resolution.PushBack(JsonValue(i64(900)));
    config.Set("resolution", move(resolution));
    JsonValue features = JsonValue::MakeArray();
    for (const String& f : o.disabledFeatures)
        features.PushBack(JsonValue(f));
    config.Set("disabledFeatures", move(features));
    config.Set("graphicsEra", JsonValue(o.graphicsEra));
    if (!fs::WriteTextFile(Path::Join(o.outputDir, "Game.json"), ToJsonString(config)))
        return Error::Make(ErrorCode::AccessDenied, "Package: can't write Game.json");
    report.seconds = timer.ElapsedSeconds();
    say(String::Format("Packaged {} in {:.1f} s: {} files, {:.1f} MB -> {:.1f} MB", o.name, report.seconds, report.files,
                       f64(report.rawBytes) / (1024.0 * 1024.0), f64(report.packageBytes) / (1024.0 * 1024.0)));
    return report;
}

} // namespace zx
