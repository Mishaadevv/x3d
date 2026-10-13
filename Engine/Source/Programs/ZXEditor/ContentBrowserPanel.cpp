// Content Browser: folder tree + tile view of the project content directory. Models can be dragged into the viewport
// (or double-clicked) to import and place them.

#include "Editor.h"
#include "Thumbnails.h"
#include "Core/Containers/Algorithm.h"
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
        ext.EqualsIgnoreCase(".ktx2") || ext.EqualsIgnoreCase(".dds"))
        return {ZX_ICON_IMAGE, Color(0x6C, 0xC8, 0x6A), "Texture"};
    if (ext.EqualsIgnoreCase(".wav") || ext.EqualsIgnoreCase(".ogg") || ext.EqualsIgnoreCase(".mp3"))
        return {ZX_ICON_FILE_AUDIO, Color(0xB0, 0x7C, 0xF0), "Audio"};
    if (ext.EqualsIgnoreCase(".zmat"))
        return {ZX_ICON_PALETTE, Color(0xE0, 0x78, 0xC8), "Material"};
    if (ext.EqualsIgnoreCase(".zmati"))
        return {ZX_ICON_PALETTE, Color(0xB8, 0x8C, 0xF0), "Material Instance"};
    if (ext.EqualsIgnoreCase(".zprefab"))
        return {ZX_ICON_PACKAGE, Color(0x5A, 0xC8, 0xF0), "Prefab"};
    if (ext.EqualsIgnoreCase(".zs"))
        return {ZX_ICON_FILE_CODE, Color(0x4E, 0xC9, 0xB0), "Script"};
    if (ext.EqualsIgnoreCase(".zscene"))
        return {ZX_ICON_CLAPPERBOARD, Color(0xF0, 0x8C, 0x4A), "Scene"};
    if (ext.EqualsIgnoreCase(".bin"))
        return {ZX_ICON_BINARY, Color(0x8A, 0x8A, 0x8A), "Buffer"};
    if (ext.EqualsIgnoreCase(".hlsl") || ext.EqualsIgnoreCase(".json") || ext.EqualsIgnoreCase(".txt") || ext.EqualsIgnoreCase(".md"))
        return {ZX_ICON_FILE_CODE, Color(0xA8, 0xA8, 0xA8), "Text"};
    return {ZX_ICON_FILE, Color(0x9A, 0x9A, 0x9A), "File"};
}

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
                else if (Path::HasExtension(item.path, ".zscene"))
                    ed.RequestSceneAction(SceneAction::Open, item.path);
                else if (Path::HasExtension(item.path, ".zs"))
                    OpenScriptEditor(ed, item.path);
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
                if (Path::HasExtension(item.path, ".zs")) {
                    if (ui::MenuItem(ZX_ICON_PENCIL "  Edit"))
                        OpenScriptEditor(ed, item.path);
                    const bool canAttach = ed.world.IsAlive(ed.selected) && !ed.IsPlaying();
                    if (ui::MenuItem(ZX_ICON_PLUS "  Add to Selected Entity", {}, false, canAttach)) {
                        ed.undo.Begin("Add Script");
                        ed.undo.Record(ed.selected);
                        ed.world.Set(ed.selected, Script{Path::MakeRelative(item.path, ed.contentRoot), String()});
                        ed.undo.End();
                    }
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
    ui::End();
}

} // namespace zx::editor
