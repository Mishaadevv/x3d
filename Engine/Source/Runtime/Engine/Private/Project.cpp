#include "Engine/Project.h"
#include "Core/FileSystem/FileSystem.h"
#include "Core/Hash/Hash.h"
#include "Core/HAL/PlatformFile.h"
#include "Core/Log/Log.h"
#include "Core/Serialization/Json.h"
#include "Core/String/Path.h"
#include "Core/Time/Time.h"
#include "Core/Version.h"

ZX_DEFINE_LOG_CATEGORY_STATIC(LogProject, Info);

namespace zx {

namespace {

constexpr usize MaxRecentProjects = 20;

bool IsDirectoryEmpty(StringView dir)
{
    Array<platform::DirectoryEntry> entries;
    platform::ListDirectory(dir, entries);
    return entries.Empty();
}

/// Decodes %XX escapes in a URI path.
String DecodeUri(StringView uri)
{
    auto hex = [](char c) -> i32 {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };
    String out;
    for (usize i = 0; i < uri.Size(); ++i) {
        if (uri[i] == '%' && i + 2 < uri.Size() && hex(uri[i + 1]) >= 0 && hex(uri[i + 2]) >= 0) {
            out += char(hex(uri[i + 1]) * 16 + hex(uri[i + 2]));
            i += 2;
        } else {
            out += uri[i];
        }
    }
    return out;
}

/// Relative dependency paths of a source file (gltf buffers/images, obj material libraries and their textures).
void CollectDependencies(StringView sourcePath, Array<String>& out)
{
    if (Path::HasExtension(sourcePath, ".gltf")) {
        String text;
        JsonValue doc;
        if (!fs::ReadTextFile(sourcePath, text) || !ParseJson(text.View(), doc))
            return;
        for (StringView section : {StringView("buffers"), StringView("images")})
            for (const JsonValue& item : doc.Get(section).Elements()) {
                const StringView uri = item.Get("uri").AsString();
                if (!uri.Empty() && !uri.StartsWith("data:"))
                    out.PushBack(DecodeUri(uri));
            }
        return;
    }
    if (Path::HasExtension(sourcePath, ".obj")) {
        String text;
        if (!fs::ReadTextFile(sourcePath, text))
            return;
        const String dir(Path::Parent(sourcePath));
        StringView rest = text.View();
        while (!rest.Empty()) {
            StringView line;
            rest.Split('\n', line, rest);
            line = line.Trim();
            if (!line.StartsWith("mtllib "))
                continue;
            const String mtl(line.SubStr(7).Trim());
            out.PushBack(mtl);
            String mtlText;
            if (!fs::ReadTextFile(Path::Join(dir, mtl), mtlText))
                continue;
            StringView mrest = mtlText.View();
            while (!mrest.Empty()) {
                StringView ml;
                mrest.Split('\n', ml, mrest);
                ml = ml.Trim();
                if (!ml.StartsWith("map_") && !ml.StartsWith("bump") && !ml.StartsWith("norm"))
                    continue;
                // The texture path is the last token (options such as -bm 1.0 come first).
                const usize space = ml.FindLastOf(" \t");
                if (space != StringView::NPos)
                    out.PushBack(String(ml.SubStr(space + 1)));
            }
        }
    }
}

} // namespace

// --- Descriptor ---------------------------------------------------------------------------------------------------------

Result<ProjectInfo> LoadProject(StringView descriptorPath)
{
    const String path = Path::Normalize(descriptorPath);
    String text;
    if (!fs::ReadTextFile(path, text))
        return Error::Make(ErrorCode::NotFound, "Project file not found: {}", path);
    JsonValue doc;
    JsonParseError error;
    if (!ParseJson(text.View(), doc, &error))
        return Error::Make(ErrorCode::InvalidData, "{}({}:{}): {}", Path::FileName(path), error.line, error.column, error.message);
    if (doc.Get("format").AsString() != "zproj")
        return Error::Make(ErrorCode::InvalidData, "{} is not a ZeqouX3D project", Path::FileName(path));
    const i64 version = doc.Get("version").AsInt();
    if (version < 1 || version > ProjectFormatVersion)
        return Error::Make(ErrorCode::Unsupported, "Project format version {} is not supported (max {})", version, ProjectFormatVersion);
    ProjectInfo p;
    p.descriptorPath = path;
    p.root = Path::Parent(path);
    p.contentDir = Path::Join(p.root, "Content");
    p.descriptor.name = doc.Get("name").AsString(Path::Stem(path));
    p.descriptor.engineVersion = doc.Get("engineVersion").AsString();
    p.descriptor.startupScene = doc.Get("startupScene").AsString();
    p.descriptor.description = doc.Get("description").AsString();
    p.descriptor.language = doc.Get("language").AsString("ZeqouScript");
    for (const JsonValue& f : doc.Get("disabledFeatures").Elements())
        if (f.IsString())
            p.descriptor.disabledFeatures.PushBack(String(f.AsString()));
    const StringView color = doc.Get("color").AsString();
    if (color.Size() == 7 && color[0] == '#')
        for (usize i = 1; i < 7; ++i) {
            const char c = color[i];
            const u32 digit = c >= '0' && c <= '9' ? u32(c - '0') : (c | 32) >= 'a' && (c | 32) <= 'f' ? u32((c | 32) - 'a' + 10) : 0u;
            p.descriptor.color = (p.descriptor.color << 4) | digit;
        }
    return p;
}

bool SaveProjectDescriptor(StringView descriptorPath, const ProjectDescriptor& d)
{
    JsonValue doc = JsonValue::MakeObject();
    doc.Set("format", "zproj");
    doc.Set("version", ProjectFormatVersion);
    doc.Set("name", d.name);
    doc.Set("engineVersion", d.engineVersion.Empty() ? String(EngineVersionString) : d.engineVersion);
    doc.Set("startupScene", d.startupScene);
    if (!d.description.Empty())
        doc.Set("description", d.description);
    if (!d.language.Empty())
        doc.Set("language", d.language);
    if (d.color)
        doc.Set("color", String::Format("#{:06X}", d.color & 0xFFFFFF));
    if (!d.disabledFeatures.Empty()) {
        JsonValue list = JsonValue::MakeArray();
        for (const String& f : d.disabledFeatures)
            list.PushBack(JsonValue(f));
        doc.Set("disabledFeatures", move(list));
    }
    return fs::WriteTextFile(descriptorPath, ToJsonString(doc));
}

String ValidateProjectName(StringView name)
{
    if (name.Empty())
        return "Enter a project name";
    if (name.Size() > 48)
        return "The name is too long (max 48 characters)";
    const char first = name[0];
    if (!((first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z')))
        return "The name must start with a letter";
    for (char c : name)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return "Use only letters, digits, '_' and '-'";
    return {};
}

StringView ProjectLanguageName(ProjectLanguage language)
{
    switch (language) {
    case ProjectLanguage::ZeqouScript: return "ZeqouScript";
    case ProjectLanguage::Cpp: return "C++";
    case ProjectLanguage::VisualScript: return "Visual Script";
    }
    return "ZeqouScript";
}

Span<const u32> ProjectTileColors()
{
    static const u32 Colors[] = {0x3B8CF5, 0x7C4DFF, 0xE0457B, 0xF0883E, 0x22B59A, 0x2FA84F, 0xD9A31A, 0x0EA5C6,
                                 0xC2410C, 0x9333EA, 0xDB2777, 0x4F46E5, 0x059669, 0x64748B};
    return Span<const u32>(Colors, ZX_ARRAY_COUNT(Colors));
}

u32 ProjectTileColor(const ProjectDescriptor& d)
{
    if (d.color)
        return d.color;
    const Span<const u32> colors = ProjectTileColors();
    return colors[HashFnv1a64(d.name.View()) % colors.Size()];
}

Result<ProjectInfo> CreateProject(StringView parentDirectory, StringView name, const ProjectDescriptor& settings)
{
    if (const String reason = ValidateProjectName(name); !reason.Empty())
        return Error::Make(ErrorCode::InvalidArgument, "{}", reason);
    if (parentDirectory.Empty())
        return Error::Make(ErrorCode::InvalidArgument, "Choose a location for the project");
    const String root = Path::Join(Path::Normalize(parentDirectory), name);
    if (platform::DirectoryExists(root) && !IsDirectoryEmpty(root))
        return Error::Make(ErrorCode::AlreadyExists, "Folder '{}' already exists and is not empty", root);
    if (!platform::CreateDirectoryTree(root + "/Content/Scenes") || !platform::CreateDirectoryTree(root + "/Saved"))
        return Error::Make(ErrorCode::AccessDenied, "Can't create the project folder '{}'", root);
    ProjectDescriptor d = settings;
    d.name = name;
    d.engineVersion = EngineVersionString;
    if (!d.color) {
        const Span<const u32> colors = ProjectTileColors();
        d.color = colors[u64(DateTime::UnixTimeNow() * 7919 + i64(HashFnv1a64(name))) % colors.Size()];
    }
    const String descriptor = Path::Join(root, String(name) + String(ProjectExtension));
    if (!SaveProjectDescriptor(descriptor, d))
        return Error::Make(ErrorCode::AccessDenied, "Can't write '{}'", descriptor);
    fs::WriteTextFile(root + "/.gitignore", "Saved/\nIntermediate/\n");
    ZX_LOG_INFO(LogProject, "Created project '{}' in {}", name, root);
    return LoadProject(descriptor);
}

// --- Recent projects ----------------------------------------------------------------------------------------------------

String DefaultRecentProjectsFile()
{
    return Path::Join(Path::Normalize(platform::GetUserLocalAppDataDirectory()), "ZeqouX3D/RecentProjects.json");
}

String DefaultProjectsDirectory()
{
    return Path::Join(Path::Normalize(platform::GetUserDocumentsDirectory()), "ZeqouX3D Projects");
}

Array<RecentProject> LoadRecentProjects(StringView file)
{
    const String path = file.Empty() ? DefaultRecentProjectsFile() : String(file);
    Array<RecentProject> out;
    String text;
    JsonValue doc;
    if (!fs::ReadTextFile(path, text) || !ParseJson(text.View(), doc))
        return out;
    for (const JsonValue& p : doc.Get("projects").Elements()) {
        RecentProject r;
        r.path = p.Get("path").AsString();
        r.name = p.Get("name").AsString(Path::Stem(r.path));
        r.lastOpened = p.Get("lastOpened").AsInt();
        if (!r.path.Empty())
            out.PushBack(move(r));
    }
    return out;
}

bool SaveRecentProjects(Span<const RecentProject> projects, StringView file)
{
    const String path = file.Empty() ? DefaultRecentProjectsFile() : String(file);
    platform::CreateDirectoryTree(Path::Parent(path));
    JsonValue list = JsonValue::MakeArray();
    for (const RecentProject& r : projects) {
        JsonValue o = JsonValue::MakeObject();
        o.Set("path", r.path);
        o.Set("name", r.name);
        o.Set("lastOpened", r.lastOpened);
        list.PushBack(move(o));
    }
    JsonValue doc = JsonValue::MakeObject();
    doc.Set("projects", move(list));
    return fs::WriteTextFile(path, ToJsonString(doc));
}

void TouchRecentProject(const ProjectInfo& project, StringView file)
{
    Array<RecentProject> list = LoadRecentProjects(file);
    for (usize i = 0; i < list.Size(); ++i)
        if (Path::Normalize(list[i].path).View().EqualsIgnoreCase(project.descriptorPath)) {
            list.RemoveAt(i);
            break;
        }
    list.Insert(0, RecentProject{project.descriptorPath, project.descriptor.name, DateTime::UnixTimeNow()});
    if (list.Size() > MaxRecentProjects)
        list.Resize(MaxRecentProjects);
    SaveRecentProjects(list.View(), file);
}

void RemoveRecentProject(StringView descriptorPath, StringView file)
{
    Array<RecentProject> list = LoadRecentProjects(file);
    const String path = Path::Normalize(descriptorPath);
    for (usize i = 0; i < list.Size(); ++i)
        if (Path::Normalize(list[i].path).View().EqualsIgnoreCase(path)) {
            list.RemoveAt(i);
            SaveRecentProjects(list.View(), file);
            return;
        }
}

// --- Import copies ------------------------------------------------------------------------------------------------------

Result<String> CopySourceWithDependencies(StringView sourcePathIn, StringView destDirectoryIn)
{
    const String source = Path::Normalize(sourcePathIn);
    const String dest = Path::Normalize(destDirectoryIn);
    if (!platform::FileExists(source))
        return Error::Make(ErrorCode::NotFound, "Source not found: {}", source);
    if (!platform::CreateDirectoryTree(dest))
        return Error::Make(ErrorCode::AccessDenied, "Can't create '{}'", dest);
    const String target = Path::Join(dest, Path::FileName(source));
    if (!platform::CopyFileAt(source, target))
        return Error::Make(ErrorCode::AccessDenied, "Can't copy '{}'", source);

    Array<String> deps;
    CollectDependencies(source, deps);
    const String sourceDir(Path::Parent(source));
    for (const String& rel : deps) {
        const String normalized = Path::Normalize(rel);
        if (Path::IsAbsolute(normalized) || normalized.View().StartsWith("..") || normalized.View().Contains("/../"))
            return Error::Make(ErrorCode::Unsupported, "'{}' references a file outside its folder ({}); copy the folder manually",
                               Path::FileName(source), rel);
        const String from = Path::Join(sourceDir, normalized);
        const String to = Path::Join(dest, normalized);
        platform::CreateDirectoryTree(Path::Parent(to));
        if (!platform::CopyFileAt(from, to))
            ZX_LOG_WARN(LogProject, "Missing dependency '{}' of {}", rel, Path::FileName(source));
    }
    // Keep the asset GUID stable: carry an existing .zmeta along.
    if (platform::FileExists(source + ".zmeta"))
        platform::CopyFileAt(source + ".zmeta", target + ".zmeta");
    return target;
}

} // namespace zx
