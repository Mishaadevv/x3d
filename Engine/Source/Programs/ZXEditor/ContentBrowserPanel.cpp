// Content Browser: folder tree + tile view of the project content directory. Models can be dragged into the viewport
// (or double-clicked) to import and place them.

#include "Editor.h"
#include "Thumbnails.h"
#include "Core/Containers/Algorithm.h"
#include "Core/FileSystem/FileSystem.h"
#include "Core/HAL/PlatformFile.h"
#include "Core/String/Path.h"
#include "Platform/Platform.h"

namespace zx::editor {

namespace {

using FileItem = PanelState::FileItem;


struct FileKind
{
    StringView icon;
    Color color;
    StringView label;
};

FileKind KindOf(const FileItem& item)
{
    if (item.directory)
        return {ZX_ICON_FOLDER, Color(0xE8, 0xB8, 0x4A), "Folder"};
    const StringView ext = Path::Extension(item.name);
    if (ext.EqualsIgnoreCase(".gltf") || ext.EqualsIgnoreCase(".glb") || ext.EqualsIgnoreCase(".obj"))
        return {ZX_ICON_BOX, Color(0x5A, 0xA0, 0xF5), "Model"};
    if (ext.EqualsIgnoreCase(".png") || ext.EqualsIgnoreCase(".jpg") || ext.EqualsIgnoreCase(".jpeg") || ext.EqualsIgnoreCase(".bmp") ||
        ext.EqualsIgnoreCase(".tga") || ext.EqualsIgnoreCase(".ktx2") || ext.EqualsIgnoreCase(".dds"))
        return {ZX_ICON_IMAGE, Color(0x6C, 0xC8, 0x6A), "Texture"};
    if (ext.EqualsIgnoreCase(".hdr") || ext.EqualsIgnoreCase(".exr"))
        return {ZX_ICON_SUNRISE, Color(0xF0, 0xB8, 0x4A), "HDR Image"};
    if (ext.EqualsIgnoreCase(".wav") || ext.EqualsIgnoreCase(".ogg") || ext.EqualsIgnoreCase(".mp3"))
        return {ZX_ICON_FILE_AUDIO, Color(0xB0, 0x7C, 0xF0), "Audio"};
    if (ext.EqualsIgnoreCase(".zmat"))
        return {ZX_ICON_PALETTE, Color(0xE0, 0x78, 0xC8), "Material"};
    if (ext.EqualsIgnoreCase(".zmati"))
        return {ZX_ICON_PALETTE, Color(0xB8, 0x8C, 0xF0), "Material Instance"};
    if (ext.EqualsIgnoreCase(".zanimgraph"))
        return {ZX_ICON_WORKFLOW, Color(0x6C, 0xC8, 0x6A), "Anim Graph"};
    if (ext.EqualsIgnoreCase(".zprefab"))
        return {ZX_ICON_PACKAGE, Color(0x5A, 0xC8, 0xF0), "Prefab"};
    if (ext.EqualsIgnoreCase(".zs"))
        return {ZX_ICON_FILE_CODE, Color(0x4E, 0xC9, 0xB0), "Script"};
    if (ext.EqualsIgnoreCase(".zgraph"))
        return {ZX_ICON_WORKFLOW, Color(0x4E, 0x9C, 0xF0), "Script Graph"};
    if (ext.EqualsIgnoreCase(".zbt"))
        return {ZX_ICON_BRAIN, Color(0xC8, 0x8C, 0xF0), "Behavior Tree"};
    if (ext.EqualsIgnoreCase(".zseq"))
        return {ZX_ICON_FILM, Color(0xE0, 0x78, 0xC8), "Sequence"};
    if (ext.EqualsIgnoreCase(".zscene"))
        return {ZX_ICON_CLAPPERBOARD, Color(0xF0, 0x8C, 0x4A), "Scene"};
    if (ext.EqualsIgnoreCase(".bin"))
        return {ZX_ICON_BINARY, Color(0x8A, 0x8A, 0x8A), "Buffer"};
    if (ext.EqualsIgnoreCase(".hlsl") || ext.EqualsIgnoreCase(".json") || ext.EqualsIgnoreCase(".txt") || ext.EqualsIgnoreCase(".md"))
        return {ZX_ICON_FILE_CODE, Color(0xA8, 0xA8, 0xA8), "Text"};
    return {ZX_ICON_FILE, Color(0x9A, 0x9A, 0x9A), "File"};
}

/// HDR panorama: double click / drag into the viewport makes it the HDRI Sky.
bool IsHdrImage(StringView path) { return Path::HasExtension(path, ".hdr") || Path::HasExtension(path, ".exr"); }

/// Can be placed in the scene (double click / drag into the viewport).
bool IsModel(StringView path)
{
    return Path::HasExtension(path, ".gltf") || Path::HasExtension(path, ".glb") || Path::HasExtension(path, ".obj") ||
           Path::HasExtension(path, ".zprefab");
}

void Refresh(Editor& ed, bool force)
{
    PanelState& b = ed.panels;
    const f64 now = ed.timer.TotalSeconds();
    if (!force && b.browserListedDir == b.browserCurrent && now - b.browserListedAt < 2.0)
        return;
    b.browserItems.Clear();
    Array<platform::DirectoryEntry> entries;
    platform::ListDirectory(b.browserCurrent, entries);
    for (const platform::DirectoryEntry& e : entries) {
        if (e.name.View().StartsWith(".") || Path::HasExtension(e.name, ".zmeta"))
            continue;
        b.browserItems.PushBack(FileItem{e.name, Path::Join(b.browserCurrent, e.name), e.isDirectory, e.size});
    }
    StableSort(b.browserItems, [](const FileItem& x, const FileItem& y) {
        if (x.directory != y.directory)
            return x.directory;
        return x.name.View().CompareIgnoreCase(y.name.View()) < 0;
    });
    b.browserListedDir = b.browserCurrent;
    b.browserListedAt = now;
}

// --- Rename -----------------------------------------------------------------------------------------------------------

void BeginRename(PanelState& b, const String& path, bool directory)
{
    b.renameOpen = true;
    b.renameFocus = true;
    b.renamePath = path;
    b.renameDirectory = directory;
    b.renameName = String(Path::FileName(path));
}

/// The name of the editor window that has this file open ("" if none).
String OpenEditorFor(Editor& ed, StringView path)
{
    if (ed.scriptEditor.open && ed.scriptEditor.path == path)
        return "Script";
    if (ed.graphEditor.open && ed.graphEditor.path == path)
        return "Script Graph";
    if (ed.btEditor.open && ed.btEditor.path == path)
        return "Behavior Tree";
    if (ed.seqEditor.open && ed.seqEditor.path == path)
        return "Sequencer";
    if (ed.animGraphEditor.open && ed.animGraphEditor.path == path)
        return "Anim Graph";
    if (ed.animViewer.open && ed.animViewer.model == path)
        return "Animation Viewer";
    return String();
}

/// Text assets that name other assets by their Content-relative path (scenes, prefabs, graphs, sequences, scripts...).
bool NamesAssetsByPath(StringView fileName)
{
    for (StringView ext : {".zscene", ".zprefab", ".zseq", ".zbt", ".zgraph", ".zanimgraph", ".zmat", ".zmati", ".zstyle", ".zloc", ".zs"})
        if (Path::HasExtension(fileName, ext))
            return true;
    return false;
}

/// Adds the files under `dir` whose text names `rel` (a file, or with `directory` anything below that folder).
void FindReferences(const String& dir, StringView rel, bool directory, Array<String>& out)
{
    Array<platform::DirectoryEntry> entries;
    platform::ListDirectory(dir, entries);
    const String needle = directory ? String::Format("\"{}/", rel) : String::Format("\"{}\"", rel);
    for (const platform::DirectoryEntry& e : entries) {
        if (e.name.View().StartsWith(".") || out.Size() >= 6)
            continue;
        const String path = Path::Join(dir, e.name);
        if (e.isDirectory) {
            FindReferences(path, rel, directory, out);
            continue;
        }
        String text;
        if (NamesAssetsByPath(e.name) && fs::ReadTextFile(path, text) && text.View().Find(needle.View()) != StringView::NPos)
            out.PushBack(path);
    }
}

/// Moves a file or folder inside the project (its .zmeta goes with it so the asset keeps its identity). Returns an
/// error text, or empty on success.
String ApplyRename(Editor& ed, const String& from, StringView newName)
{
    String name = String(newName).Trimmed();
    if (name.Empty())
        return "The name is empty";
    for (char ch : {'/', '\\', ':', '*', '?', '"', '<', '>', '|'})
        if (name.View().Find(ch) != StringView::NPos)
            return "A name can't contain / \\ : * ? \" < > |";
    if (name == "." || name == "..")
        return "Invalid name";
    const String to = Path::Join(Path::Parent(from), name);
    if (to == from)
        return String();
    const bool directory = platform::DirectoryExists(from);
    // Case-only changes are allowed on Windows (same file, different spelling).
    const bool sameFile = to.View().CompareIgnoreCase(from.View()) == 0;
    if (!sameFile && (platform::FileExists(to) || platform::DirectoryExists(to)))
        return "Something with this name already exists here";
    // Keep the extension when renaming a file and the user dropped it.
    String target = to;
    if (!directory && Path::Extension(name).Empty() && !Path::Extension(from).Empty())
        target += Path::Extension(from);
    if (target != to && platform::FileExists(target))
        return "Something with this name already exists here";
    if (ed.IsPlaying())
        return "Stop Play before renaming";
    // A file open in an editor window would keep saving to the old path: ask to close it first.
    const String openIn = OpenEditorFor(ed, from);
    if (!openIn.Empty())
        return String::Format("Close the {} window first", openIn);
    // Scenes, prefabs, graphs and scripts name assets by path: a moved file would leave them pointing at nothing.
    if (from.View().StartsWith(ed.contentRoot.View())) {
        Array<String> users;
        FindReferences(ed.contentRoot, Path::Normalize(Path::MakeRelative(from, ed.contentRoot)), directory, users);
        if (!users.Empty()) {
            String names;
            for (const String& u : users)
                names += String::Format("{}{}", names.Empty() ? "" : ", ", Path::FileName(u));
            return String::Format("Still used by {}: change those references first", names);
        }
    }
    const bool openScene = !ed.scenePath.Empty() && (ed.scenePath == from || (directory && String(ed.scenePath).View().StartsWith(from)));
    if (!platform::MoveFileAt(from, target, false)) {
        // Directories: MoveFileExW can't always move a folder to another name while something has it open.
        return "The file or folder is in use or can't be renamed";
    }
    if (!directory && platform::FileExists(from + ".zmeta"))
        platform::MoveFileAt(from + ".zmeta", target + ".zmeta", true);
    // Files the engine keeps next to a scene (navigation mesh, baked lighting, world partition cells) follow it.
    if (!directory && Path::HasExtension(from, ".zscene")) {
        for (const char* ext : {".znav", ".zgi"})
            if (platform::FileExists(Path::ChangeExtension(from, ext)))
                platform::MoveFileAt(Path::ChangeExtension(from, ext), Path::ChangeExtension(target, ext), true);
        // The cell folder is named after the scene's stem: <Parent>/<Stem>_Cells (see Editor::CellDirectory).
        const String cells = Path::Join(Path::Parent(from), String(Path::Stem(from)) + "_Cells");
        const String cellsTarget = Path::Join(Path::Parent(target), String(Path::Stem(target)) + "_Cells");
        if (platform::DirectoryExists(cells) && !platform::DirectoryExists(cellsTarget))
            platform::MoveFileAt(cells, cellsTarget, false);
    }
    if (ed.panels.browserCurrent == from)
        ed.panels.browserCurrent = target;
    else if (directory && ed.panels.browserCurrent.View().StartsWith(from))
        ed.panels.browserCurrent = target + String(ed.panels.browserCurrent.View().DropLeft(from.Size()));
    if (ed.panels.browserSelected == from)
        ed.panels.browserSelected = target;
    if (openScene) {
        if (ed.scenePath == from)
            ed.scenePath = target;
        else
            ed.scenePath = target + String(ed.scenePath.View().DropLeft(from.Size()));
    }
    ed.panels.browserListedDir.Clear();
    return String();
}

void RenamePopup(Editor& ed)
{
    PanelState& b = ed.panels;
    if (b.renameOpen) {
        ui::OpenPopup("Rename##content_rename");
        b.renameOpen = false;
    }
    bool popupOpen = true;
    if (ui::BeginPopupModal("Rename##content_rename", &popupOpen)) {
        ui::Text(b.renameDirectory ? "Rename folder" : "Rename file");
        ui::SetNextItemWidth(320.0f);
        if (b.renameFocus) {
            ui::SetKeyboardFocusHere();
            b.renameFocus = false;
            b.renameError.Clear();
        }
        const bool entered = ui::InputText("##rename_name", b.renameName,
                                           ui::InputTextFlags::EnterReturnsTrue | ui::InputTextFlags::AutoSelectAll);
        if (!b.renameError.Empty())
            ui::TextColored(Color(0xFF, 0x6B, 0x6B), b.renameError);
        const bool ok = ui::Button("Rename", Vec2::Zero(), ui::ButtonVariant::Primary);
        ui::SameLine();
        const bool cancel = ui::Button("Cancel");
        if (entered || ok) {
            b.renameError = ApplyRename(ed, b.renamePath, b.renameName.View());
            if (b.renameError.Empty())
                ui::CloseCurrentPopup();
        }
        if (cancel || ui::IsKeyPressed(Key::Escape, false))
            ui::CloseCurrentPopup();
        ui::EndPopup();
    }
}

void FolderTree(Editor& ed, const String& dir, StringView label, bool root)
{
    Array<platform::DirectoryEntry> entries;
    platform::ListDirectory(dir, entries);
    bool hasSub = false;
    for (const platform::DirectoryEntry& e : entries)
        hasSub |= e.isDirectory && !e.name.View().StartsWith(".");
    ui::TreeNodeFlags flags = ui::TreeNodeFlags::OpenOnArrow;
    if (!hasSub)
        flags |= ui::TreeNodeFlags::Leaf;
    if (root)
        flags |= ui::TreeNodeFlags::DefaultOpen;
    if (ed.panels.browserCurrent == dir)
        flags |= ui::TreeNodeFlags::Selected;
    const bool open = ui::TreeNode(String::Format("{}  {}##{}", root ? ZX_ICON_HOUSE : ZX_ICON_FOLDER, label, dir), flags);
    if (ui::IsItemClicked())
        ed.panels.browserCurrent = dir;
    if (open) {
        for (const platform::DirectoryEntry& e : entries)
            if (e.isDirectory && !e.name.View().StartsWith("."))
                FolderTree(ed, Path::Join(dir, e.name), e.name, false);
        ui::TreePop();
    }
}

void Breadcrumbs(Editor& ed)
{
    PanelState& b = ed.panels;
    const String rel = Path::MakeRelative(b.browserCurrent, ed.contentRoot);
    if (ui::Button(ZX_ICON_HOUSE "  Content"))
        b.browserCurrent = ed.contentRoot;
    String acc = ed.contentRoot;
    StringView rest = rel == "." ? StringView() : rel.View(); // MakeRelative returns "." for the root itself
    while (!rest.Empty()) {
        StringView part;
        rest.Split('/', part, rest); // no separator: part = remainder, rest = empty
        if (part.Empty())
            continue;
        acc = Path::Join(acc, part);
        ui::SameLine(0.0f, 2.0f);
        ui::TextDisabled(ZX_ICON_CHEVRON_RIGHT);
        ui::SameLine(0.0f, 2.0f);
        if (ui::Button(String::Format("{}##crumb{}", part, acc), Vec2::Zero(), ui::ButtonVariant::Ghost))
            b.browserCurrent = acc;
    }
}

} // namespace

void DrawContentBrowserPanel(Editor& ed)
{
    if (!ui::Begin(ContentTitle, &ed.showContent)) {
        ui::End();
        return;
    }
    PanelState& b = ed.panels;
    if (b.browserCurrent.Empty() || !platform::DirectoryExists(b.browserCurrent))
        b.browserCurrent = ed.contentRoot;
    ui::Context& ctx = *ui::GetCurrentContext();
    const f32 s = ctx.GetScale();
    const ui::Theme& c = ctx.GetStyle().colors;

    // Toolbar.
    if (ui::Button(ZX_ICON_IMPORT "  Import", Vec2::Zero(), ui::ButtonVariant::Primary)) {
        static const platform::FileDialogFilter Filters[] = {{"3D models", "*.gltf;*.glb;*.obj"}, {"All files", "*.*"}};
        for (const String& path : platform::OpenFileDialog(ed.window.Get(), "Import Asset", Span<const platform::FileDialogFilter>(Filters, 2), true))
            ed.ImportAsset(path, ed.camera.position + ed.camera.Forward() * 5.0f);
    }
    ui::SameLine();
    const bool canGoUp = b.browserCurrent != ed.contentRoot && b.browserCurrent.Size() > ed.contentRoot.Size();
    ui::BeginDisabled(!canGoUp);
    if (ui::IconButton(ZX_ICON_ARROW_UP, "Parent folder"))
        b.browserCurrent = String(Path::Parent(b.browserCurrent));
    ui::EndDisabled();
    ui::SameLine();
    if (ui::IconButton(ZX_ICON_REFRESH_CW, "Refresh"))
        Refresh(ed, true);
    ui::SameLine();
    Breadcrumbs(ed);
    ui::SameLine();
    const f32 searchW = 200.0f;
    const f32 availLogical = ui::GetContentRegionAvail().x / s;
    if (availLogical > searchW + 20.0f)
        ui::SameLine(Max(0.0f, ui::GetWindowContentRect().Width() / s - searchW - 16.0f));
    ui::SetNextItemWidth(searchW);
    ui::InputText("##content_search", b.browserFilter, ui::InputTextFlags::None, ZX_ICON_SEARCH "  Search...");

    Refresh(ed, false);
    const Vec2 avail = ui::GetContentRegionAvail() / s;
    f32 rightW = Max(avail.x - b.browserTreeWidth - 6.0f, 50.0f);
    if (ui::BeginChild("##content_tree", Vec2(b.browserTreeWidth, avail.y - 2.0f), true))
        FolderTree(ed, ed.contentRoot, "Content", true);
    ui::EndChild();
    ui::SameLine(0.0f, 0.0f);
    ui::Splitter("##content_split", true, &b.browserTreeWidth, &rightW, 120.0f, 120.0f, 6.0f);
    ui::SameLine(b.browserTreeWidth + 6.0f, 0.0f);
    if (ui::BeginChild("##content_items", Vec2(0.0f, avail.y - 2.0f), true)) {
        const Rect area = ui::GetWindowContentRect();
        const f32 tile = Round(b.browserTileSize * s), gap = Round(6.0f * s);
        const f32 width = area.Width() - Round(16.0f * s);
        const u32 perRow = Max(1u, u32((width + gap) / (tile + gap)));
        DrawList& dl = ui::GetWindowDrawList();
        u32 col = 0, shown = 0;
        const ui::FontHandle font = ctx.Fonts().FindFont("Inter");
        for (const FileItem& item : b.browserItems) {
            if (!b.browserFilter.Empty()) {
                bool found = false;
                for (usize i = 0; i + b.browserFilter.Size() <= item.name.Size() && !found; ++i)
                    found = item.name.View().SubStr(i, b.browserFilter.Size()).EqualsIgnoreCase(b.browserFilter);
                if (!found)
                    continue;
            }
            ++shown;
            if (col > 0)
                ui::SameLine(0.0f, gap / s);
            ui::PushID(item.path);
            const Vec2 pos = ui::GetCursorPos();
            const Rect r(pos, pos + Vec2(tile, tile + Round(30.0f * s)));
            ui::InvisibleButton("##tile", r.Size() / s);
            const FileKind kind = KindOf(item);
            const bool hovered = ui::IsItemHovered();
            const bool selected = b.browserSelected == item.path;
            if (ui::IsItemClicked())
                b.browserSelected = item.path;
            if (ui::IsItemDoubleClicked()) {
                if (item.directory)
                    b.browserCurrent = item.path;
                else if (IsModel(item.path))
                    ed.ImportAsset(item.path, ed.camera.position + ed.camera.Forward() * 5.0f);
                else if (IsHdrImage(item.path) && !ed.IsPlaying())
                    ed.UseHdriSky(Path::MakeRelative(item.path, ed.contentRoot));
                else if (Path::HasExtension(item.path, ".zscene"))
                    ed.RequestSceneAction(SceneAction::Open, item.path);
                else if (Path::HasExtension(item.path, ".zs"))
                    OpenScriptEditor(ed, item.path);
                else if (Path::HasExtension(item.path, ".zanimgraph"))
                    OpenAnimGraphEditor(ed, item.path);
                else if (Path::HasExtension(item.path, ".zgraph"))
                    OpenZGraphEditor(ed, item.path);
                else if (Path::HasExtension(item.path, ".zbt"))
                    OpenBTEditor(ed, item.path);
                else if (Path::HasExtension(item.path, ".zseq"))
                    OpenSequencerEditor(ed, item.path);
                else if ((Path::HasExtension(item.path, ".wav") || Path::HasExtension(item.path, ".ogg")) && ed.audio)
                    ed.audio->TogglePreview(item.path);
                else if (Path::HasExtension(item.path, ".zmat") || Path::HasExtension(item.path, ".zmati")) {
                    MaterialLibrary::Material* m = ed.materials ? ed.materials->FindByPath(item.path) : nullptr;
                    const Guid guid = m ? m->guid : (ed.materials ? ed.materials->LoadFile(item.path) : Guid());
                    if (guid.IsValid())
                        OpenMaterialEditor(ed, guid);
                }
                else
                    platform::OpenUrl(item.path);
            }
            if (!item.directory)
                ui::BeginDragDropSource("asset", item.path.CStr(), item.path.Size() + 1, String::Format("{}  {}", kind.icon, item.name));

            if (selected || hovered)
                dl.AddRect(r, selected ? WithAlpha(c.accent, 0.35f) : c.headerHovered, ui::CornerRadii(4.0f * s));
            // Thumbnail (rendered on demand) or a type-colored card with a large glyph; a type stripe at the bottom.
            const Rect thumb(r.min + Vec2(Round(8.0f * s)), Vec2(r.max.x - Round(8.0f * s), r.min.y + tile - Round(8.0f * s)));
            const u32 preview = item.directory || !ed.thumbnails ? rhi::InvalidDescriptor : ed.thumbnails->Get(item.path);
            if (!item.directory) {
                dl.AddRect(thumb, Color(0x1C, 0x1C, 0x1C), ui::CornerRadii(4.0f * s), 1.0f, c.border);
                if (preview != rhi::InvalidDescriptor)
                    dl.AddImage(preview, thumb.Expanded(-1.0f));
                dl.AddRect(Rect(Vec2(thumb.min.x, thumb.max.y - Round(3.0f * s)), thumb.max), kind.color, ui::CornerRadii::Bottom(4.0f * s));
            }
            if (preview == rhi::InvalidDescriptor)
                dl.AddTextInRect(font, Round((item.directory ? 44.0f : 30.0f) * s), thumb, kind.color, kind.icon, ui::TextAlign::Center, false);
            dl.AddTextInRect(font, 12.0f * s, Rect(Vec2(r.min.x + 3.0f * s, thumb.max.y + 3.0f * s), Vec2(r.max.x - 3.0f * s, r.max.y)),
                             c.text, Path::Stem(item.name), ui::TextAlign::Center);
            // Per-item actions.
            if (ui::BeginPopupContextItem("##tile_menu")) {
                b.browserSelected = item.path;
                ui::TextDisabled(item.name);
                ui::Separator();
                if (ui::MenuItem(ZX_ICON_PENCIL "  Rename", "F2"))
                    BeginRename(b, item.path, item.directory);
                if (IsModel(item.path) && ui::MenuItem(ZX_ICON_CLAPPERBOARD "  Open in Animation Viewer"))
                    OpenAnimationViewer(ed, item.path);
                if (Path::HasExtension(item.path, ".zanimgraph") && ui::MenuItem(ZX_ICON_PENCIL "  Edit"))
                    OpenAnimGraphEditor(ed, item.path);
                const bool material = Path::HasExtension(item.path, ".zmat");
                if ((material || Path::HasExtension(item.path, ".zmati")) && ui::MenuItem(ZX_ICON_PENCIL "  Edit")) {
                    MaterialLibrary::Material* m = ed.materials ? ed.materials->FindByPath(item.path) : nullptr;
                    const Guid guid = m ? m->guid : (ed.materials ? ed.materials->LoadFile(item.path) : Guid());
                    if (guid.IsValid())
                        OpenMaterialEditor(ed, guid);
                }
                if (material && ui::MenuItem(ZX_ICON_COPY_PLUS "  Create Material Instance")) {
                    MaterialLibrary::Material* m = ed.materials ? ed.materials->FindByPath(item.path) : nullptr;
                    if (m) {
                        String path = Path::Join(Path::Parent(item.path), String::Format("{}_Inst.zmati", Path::Stem(item.path)));
                        for (u32 n = 2; platform::FileExists(path); ++n)
                            path = Path::Join(Path::Parent(item.path), String::Format("{}_Inst{}.zmati", Path::Stem(item.path), n));
                        const Guid inst = ed.materials->CreateInstance(m->guid, path);
                        if (inst.IsValid())
                            OpenMaterialEditor(ed, inst);
                        b.browserListedDir.Clear();
                    }
                }
                if ((Path::HasExtension(item.path, ".wav") || Path::HasExtension(item.path, ".ogg")) && ed.audio) {
                    const bool playing = ed.audio->PreviewFile() == item.path;
                    if (ui::MenuItem(playing ? ZX_ICON_SQUARE "  Stop" : ZX_ICON_PLAY "  Play"))
                        ed.audio->TogglePreview(item.path);
                    const bool canAttach = ed.world.IsAlive(ed.selected) && !ed.IsPlaying();
                    if (ui::MenuItem(ZX_ICON_PLUS "  Add Audio Source to Selected", {}, false, canAttach)) {
                        ed.undo.Begin("Add Audio Source");
                        ed.undo.Record(ed.selected);
                        AudioSource src;
                        src.clip = Path::MakeRelative(item.path, ed.contentRoot);
                        ed.world.Set(ed.selected, src);
                        ed.undo.End();
                    }
                }
                if (IsHdrImage(item.path) && ui::MenuItem(ZX_ICON_CLOUD_SUN "  Use as HDRI Sky", {}, false, !ed.IsPlaying()))
                    ed.UseHdriSky(Path::MakeRelative(item.path, ed.contentRoot));
                if (Path::HasExtension(item.path, ".zs") || Path::HasExtension(item.path, ".zgraph")) {
                    if (ui::MenuItem(ZX_ICON_PENCIL "  Edit")) {
                        if (Path::HasExtension(item.path, ".zs"))
                            OpenScriptEditor(ed, item.path);
                        else
                            OpenZGraphEditor(ed, item.path);
                    }
                    const bool canAttach = ed.world.IsAlive(ed.selected) && !ed.IsPlaying();
                    if (ui::MenuItem(ZX_ICON_PLUS "  Add to Selected Entity", {}, false, canAttach))
                        ed.AttachScript(ed.selected, item.path);
                }
                if (ui::MenuItem(ZX_ICON_FOLDER_SEARCH "  Show in Explorer"))
                    platform::RevealInFileExplorer(item.path);
                if (ui::MenuItem(ZX_ICON_CLIPBOARD_COPY "  Copy Path"))
                    platform::SetClipboardText(item.path);
                ui::EndPopup();
            }
            ui::ItemTooltip(String::Format("{}\n{} - {}", item.name, kind.label,
                                           item.directory ? String("folder") : String::Format("{:.1f} KB", f64(item.size) / 1024.0)));
            ui::PopID();
            col = (col + 1) % perRow;
        }
        if (shown == 0)
            ui::TextDisabled(b.browserFilter.Empty() ? "This folder is empty" : "No items match the search");
        // Context menu on the folder: create assets here.
        if (ui::BeginPopupContextWindow("##content_menu")) {
            ui::TextDisabled(Path::FileName(b.browserCurrent));
            ui::Separator();
            if (ui::MenuItem(ZX_ICON_PALETTE "  New Material")) {
                String path = Path::Join(b.browserCurrent, "NewMaterial.zmat");
                for (u32 n = 2; platform::FileExists(path); ++n)
                    path = Path::Join(b.browserCurrent, String::Format("NewMaterial{}.zmat", n));
                const Guid guid = ed.CreateMaterial(path);
                if (guid.IsValid())
                    OpenMaterialEditor(ed, guid);
                Refresh(ed, true);
            }
            if (ui::MenuItem(ZX_ICON_FILE_CODE "  New Script")) {
                String path = Path::Join(b.browserCurrent, "NewScript.zs");
                for (u32 n = 2; platform::FileExists(path); ++n)
                    path = Path::Join(b.browserCurrent, String::Format("NewScript{}.zs", n));
                if (CreateScriptFile(path))
                    OpenScriptEditor(ed, path);
                Refresh(ed, true);
            }
            if (ui::MenuItem(ZX_ICON_WORKFLOW "  New Script Graph")) {
                String path = Path::Join(b.browserCurrent, "NewGraph.zgraph");
                for (u32 n = 2; platform::FileExists(path); ++n)
                    path = Path::Join(b.browserCurrent, String::Format("NewGraph{}.zgraph", n));
                if (CreateZGraphFile(path))
                    OpenZGraphEditor(ed, path);
                Refresh(ed, true);
            }
            ui::ItemTooltip("Visual scripting with nodes (like Blueprints): put it on an entity like a script");
            if (ui::MenuItem(ZX_ICON_BRAIN "  New Behavior Tree")) {
                String path = Path::Join(b.browserCurrent, "NewBehaviorTree.zbt");
                for (u32 n = 2; platform::FileExists(path); ++n)
                    path = Path::Join(b.browserCurrent, String::Format("NewBehaviorTree{}.zbt", n));
                if (CreateBehaviorTreeFile(path))
                    OpenBTEditor(ed, path);
                Refresh(ed, true);
            }
            ui::ItemTooltip("AI decisions as a tree of tasks: put it on an entity with a Behavior Tree component");
            if (ui::MenuItem(ZX_ICON_FILM "  New Sequence")) {
                String path = Path::Join(b.browserCurrent, "NewSequence.zseq");
                for (u32 n = 2; platform::FileExists(path); ++n)
                    path = Path::Join(b.browserCurrent, String::Format("NewSequence{}.zseq", n));
                if (CreateSequenceFile(path))
                    OpenSequencerEditor(ed, path);
                Refresh(ed, true);
            }
            ui::ItemTooltip("A cinematic timeline (camera cuts, movement, animation, sounds, events): play it with a Sequence Player component");
            if (ui::MenuItem(ZX_ICON_WORKFLOW "  New Anim Graph")) {
                CreateAnimGraph(ed, b.browserCurrent);
                Refresh(ed, true);
            }
            if (ui::MenuItem(ZX_ICON_FOLDER_PLUS "  New Folder")) {
                String path = Path::Join(b.browserCurrent, "NewFolder");
                for (u32 n = 2; platform::DirectoryExists(path); ++n)
                    path = Path::Join(b.browserCurrent, String::Format("NewFolder{}", n));
                platform::CreateDirectoryTree(path);
                Refresh(ed, true);
            }
            ui::Separator();
            if (ui::MenuItem(ZX_ICON_FOLDER_SEARCH "  Show in Explorer"))
                platform::OpenUrl(b.browserCurrent);
            ui::EndPopup();
        }
    }
    ui::EndChild();
    // F2 renames the selected item while the pointer is over the browser (hovered window = the one being used).
    if (ui::IsWindowHovered() && !ed.IsPlaying() && !ui::GetCurrentContext()->WantsKeyboard() && ui::IsKeyPressed(Key::F2, false) &&
        !b.browserSelected.Empty() && b.browserSelected != ed.contentRoot &&
        (platform::FileExists(b.browserSelected) || platform::DirectoryExists(b.browserSelected)))
        BeginRename(b, b.browserSelected, platform::DirectoryExists(b.browserSelected));
    RenamePopup(ed);
    ui::End();
}

} // namespace zx::editor
