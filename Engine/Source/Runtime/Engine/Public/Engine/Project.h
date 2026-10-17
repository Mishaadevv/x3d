#pragma once

// Projects: a directory with a <Name>.zproj descriptor, Content/ (assets and scenes) and Saved/ (per-project state).
//
//   <Root>/MyGame.zproj        { "format": "zproj", "version": 1, "name": "MyGame", "engineVersion": "0.1.0",
//                                "startupScene": "Content/Scenes/Main.zscene", "language": "ZeqouScript",
//                                "color": "#3B8CF5" }
//   <Root>/Content/Scenes/      scenes (.zscene); everything under Content/ is shown by the Content Browser
//   <Root>/Saved/              editor layout, logs (not meant for version control)
//
// The recent-projects list is per user (%LOCALAPPDATA%/ZeqouX3D/RecentProjects.json) so the launcher can share it.

#include "Core/Containers/Array.h"
#include "Core/Result.h"
#include "Core/String/String.h"

namespace zx {

inline constexpr i64 ProjectFormatVersion = 1;
inline constexpr StringView ProjectExtension = ".zproj";

struct ProjectDescriptor
{
    String name;
    String engineVersion;
    String startupScene; // relative to the project root ('/' separators); empty = none
    String description;
    String language = "ZeqouScript"; // gameplay language chosen at creation (ProjectLanguageName)
    u32 color = 0;                    // 0xRRGGBB tile color in the Project Browser (0 = derived from the name)
    Array<String> disabledFeatures;   // engine features / editor tools switched off in the Features window
    bool IsFeatureEnabled(StringView id) const
    {
        for (const String& f : disabledFeatures)
            if (f == id)
                return false;
        return true;
    }
};

/// Gameplay languages offered when creating a project.
enum class ProjectLanguage : u8
{
    ZeqouScript, // the engine's own scripting language (docs/ZeqouScript.md)
    Cpp,         // native modules (M10)
    VisualScript // node graphs (M14)
};
StringView ProjectLanguageName(ProjectLanguage language);

/// Tile colors for new projects (one is picked at random); ProjectTileColor falls back to a name hash.
Span<const u32> ProjectTileColors();
u32 ProjectTileColor(const ProjectDescriptor& descriptor);

struct ProjectInfo
{
    String descriptorPath; // physical, normalized
    String root;           // directory of the descriptor
    String contentDir;     // <root>/Content
    ProjectDescriptor descriptor;
};

/// Reads and validates a .zproj (also fills the derived paths).
Result<ProjectInfo> LoadProject(StringView descriptorPath);
bool SaveProjectDescriptor(StringView descriptorPath, const ProjectDescriptor& descriptor);

/// A project name must start with a letter and contain only letters, digits, '_' and '-' (max 48 chars).
/// Returns an empty string if valid, otherwise a user-facing reason.
String ValidateProjectName(StringView name);

/// Creates <parentDirectory>/<name>/ with the descriptor, Content/Scenes and Saved. Fails if the directory exists and
/// is not empty. `settings` supplies language/description/color (color 0 = a random tile color). Returns the loaded project.
Result<ProjectInfo> CreateProject(StringView parentDirectory, StringView name, const ProjectDescriptor& settings = {});

/// Writes the Starter Content into <contentDir>/StarterContent: materials (.zmat with editable parameters) and
/// ready-to-use scripts. Returns the number of files written.
u32 WriteStarterContent(StringView contentDir);
/// Writes the character scripts of the First/Third Person templates into <contentDir>/Scripts
/// (FirstPersonController, ThirdPersonController, ThirdPersonCamera). Returns the number of files written.
u32 WriteGameplayScripts(StringView contentDir);
/// Writes README.md and Docs/ (getting started, ZeqouScript reference) for the team, plus AGENTS.md / CLAUDE.md with
/// instructions for AI coding assistants. Returns the number of files written.
u32 WriteProjectDocs(const ProjectInfo& project);

// --- Recent projects ---------------------------------------------------------------------------------------------------

struct RecentProject
{
    String path; // .zproj
    String name;
    i64 lastOpened = 0; // unix seconds
};

/// `file` defaults to %LOCALAPPDATA%/ZeqouX3D/RecentProjects.json.
Array<RecentProject> LoadRecentProjects(StringView file = {});
bool SaveRecentProjects(Span<const RecentProject> projects, StringView file = {});
/// Moves (or inserts) the project to the front with lastOpened = now and saves the list (max 20 entries).
void TouchRecentProject(const ProjectInfo& project, StringView file = {});
void RemoveRecentProject(StringView descriptorPath, StringView file = {});
String DefaultRecentProjectsFile();
/// Default parent directory for new projects (Documents/ZeqouX3D Projects).
String DefaultProjectsDirectory();

// --- Importing sources into a project ----------------------------------------------------------------------------------

/// Copies a model/texture source into `destDirectory` (created if needed). For .gltf files the referenced buffers and
/// images (relative URIs) are copied too, keeping their relative layout. Existing files are overwritten. Returns the
/// physical path of the copied main file.
Result<String> CopySourceWithDependencies(StringView sourcePath, StringView destDirectory);

// --- Asset packs -------------------------------------------------------------------------------------------------------
//
// Optional model packs shipped with the engine in Engine/Content/Packs/<Folder>/ (Pack.json + License.txt + Models/,
// built by Tools/import_asset_packs.py). New projects copy the packs the user picks into Content/Packs/<Folder>.

struct AssetPackInfo
{
    String folder; // directory name under Engine/Content/Packs (the id, e.g. "StylizedNature")
    String name;
    String description;
    String author;
    String license;
    String preview; // physical path of Preview.jpg/.png (empty = none)
    u32 models = 0;
    u64 bytes = 0;
};

/// Packs available in <engineRoot>/Engine/Content/Packs, sorted by name.
Array<AssetPackInfo> ListAssetPacks(StringView engineRoot);
/// Copies the pack into <contentDir>/Packs/<folder> (Pack.json and the preview stay in the engine). Returns the number
/// of files copied (0 if the pack does not exist).
u32 CopyAssetPack(StringView engineRoot, StringView folder, StringView contentDir);

} // namespace zx
