// Outliner (scene hierarchy) and Output Log panels.

#include "Editor.h"
#include "Core/Console/Console.h"
#include "Core/Containers/Algorithm.h"
#include "Engine/ComponentRegistry.h"
#include "Core/String/Path.h"
#include "Engine/SceneRendering.h"

ZX_DEFINE_LOG_CATEGORY_STATIC(LogEditorConsole, Info);

namespace zx::editor {

namespace {

StringView EntityIcon(Editor& ed, ecs::Entity e)
{
    if (ed.world.Has<PrefabInstance>(e))
        return ZX_ICON_PACKAGE;
    if (ed.world.Has<PointLight>(e))
        return ZX_ICON_LIGHTBULB;
    if (ed.world.Has<SpotLight>(e))
        return ZX_ICON_SPOTLIGHT;
    if (ed.world.Has<MeshRenderer>(e))
        return ZX_ICON_BOX;
    // Otherwise the icon of its first user component (Camera, Audio Source, Decal, Reflection Probe...).
    for (const ComponentType& ct : GetComponentTypes())
        if (ct.userAddable && !ct.icon.Empty() && ed.world.HasComponent(e, ct.id))
            return ct.icon;
    if (!scene::GetChildren(ed.world, e).Empty())
        return ZX_ICON_FOLDER;
    return ZX_ICON_CIRCLE_DOT;
}

bool ContainsIgnoreCase(StringView haystack, StringView needle)
{
    if (needle.Empty())
        return true;
    for (usize i = 0; i + needle.Size() <= haystack.Size(); ++i)
        if (haystack.SubStr(i, needle.Size()).EqualsIgnoreCase(needle))
            return true;
    return false;
}

void EntityContextMenu(Editor& ed, ecs::Entity e)
{
    if (ui::BeginPopupContextItem()) {
        ed.Select(e);
        ui::TextDisabled(ed.EntityLabel(e));
        ui::Separator();
        if (ui::MenuItem(ZX_ICON_SCAN "  Focus", "F"))
            ed.FocusSelected();
        if (ui::MenuItem(ZX_ICON_COPY "  Duplicate", "Ctrl+D"))
            ed.DuplicateSelected();
        if (ui::MenuItem(ZX_ICON_PLUS "  Create Child"))
            ed.CreateEmpty(Vec3::Zero(), e);
        if (scene::GetParent(ed.world, e) && ui::MenuItem(ZX_ICON_CORNER_LEFT_UP "  Detach from Parent"))
            ed.Reparent(e, ecs::NullEntity);
        ui::Separator();
        const ecs::Entity instance = FindPrefabInstanceRoot(ed.world, e);
        if (!instance) {
            if (ui::MenuItem(ZX_ICON_PACKAGE "  Create Prefab..."))
                ed.CreatePrefabFrom(e);
        } else if (ui::BeginMenu(ZX_ICON_PACKAGE "  Prefab")) {
            const PrefabInstance* pi = ed.world.Get<const PrefabInstance>(instance);
            ui::TextDisabled(pi ? pi->prefab.View() : StringView());
            ui::Separator();
            if (ui::MenuItem(ZX_ICON_UPLOAD "  Apply Overrides to Prefab"))
                ed.ApplyPrefab(e);
            if (ui::MenuItem(ZX_ICON_ROTATE_CCW "  Revert to Prefab"))
                ed.RevertPrefab(e);
            if (ui::MenuItem(ZX_ICON_FOLDER_SEARCH "  Select Prefab Asset") && pi) {
                ed.panels.browserCurrent = String(Path::Parent(ed.prefabs->PhysicalPath(pi->prefab)));
                ed.panels.browserSelected = ed.prefabs->PhysicalPath(pi->prefab);
                ui::FocusDockedWindow(ContentTitle);
            }
            ui::Separator();
            if (ui::MenuItem(ZX_ICON_UNLINK "  Unpack Prefab"))
                ed.UnpackPrefab(e);
            ui::EndMenu();
        }
        ui::Separator();
        if (ui::MenuItem(ZX_ICON_TRASH_2 "  Delete", "Del"))
            ed.DeleteSelected();
        ui::EndPopup();
    }
}

void EntityDragDrop(Editor& ed, ecs::Entity e)
{
    const u64 packed = e.Pack();
    if (ui::BeginDragDropSource("entity", &packed, sizeof(packed), ed.EntityLabel(e))) {
    }
    if (const void* p = ui::AcceptDragDropPayload("entity")) {
        const ecs::Entity dragged = ecs::Entity::Unpack(*static_cast<const u64*>(p));
        if (dragged != e)
            ed.Reparent(dragged, e);
    }
}

void VisibilityToggle(Editor& ed, ecs::Entity e, const Rect& row)
{
    MeshRenderer* mr = ed.world.Get<MeshRenderer>(e);
    if (!mr)
        return;
    const Vec2 saved = ui::GetCursorPos();
    const f32 fh = ui::GetFrameHeight();
    ui::SetCursorPos(Vec2(row.max.x - fh, row.min.y + (row.Height() - fh) * 0.5f));
    ui::PushID(i32(e.index));
    if (ui::IconButton(mr->visible ? ZX_ICON_EYE : ZX_ICON_EYE_OFF, mr->visible ? "Hide" : "Show")) {
        ed.undo.Begin(mr->visible ? "Hide" : "Show");
        ed.undo.Record(e);
        mr->visible = !mr->visible;
        ed.undo.End();
    }
    ui::PopID();
    ui::SetCursorPos(saved);
}

void TreeRecursive(Editor& ed, ecs::Entity e)
{
    const Span<const ecs::Entity> children = scene::GetChildren(ed.world, e);
    ui::TreeNodeFlags flags = ui::TreeNodeFlags::OpenOnArrow;
    if (children.Empty())
        flags |= ui::TreeNodeFlags::Leaf;
    if (e == ed.selected)
        flags |= ui::TreeNodeFlags::Selected;
    ui::PushID(i32(e.index));
    // Prefab instances in blue (members slightly dimmer), like UE/Unity.
    const bool instanceRoot = ed.world.Has<PrefabInstance>(e), member = ed.world.Has<PrefabLink>(e);
    if (instanceRoot || member)
        ui::PushTextColor(instanceRoot ? Color(0x6C, 0xB4, 0xF5) : Color(0x5A, 0x92, 0xC8));
    const bool open = ui::TreeNode(String::Format("{}  {}##e{}", EntityIcon(ed, e), ed.EntityLabel(e), e.generation), flags);
    if (instanceRoot || member)
        ui::PopTextColor();
    const Rect row = ui::GetItemRect();
    if (ui::IsItemClicked())
        ed.Select(e);
    if (ui::IsItemDoubleClicked())
        ed.FocusSelected();
    EntityDragDrop(ed, e);
    EntityContextMenu(ed, e);
    VisibilityToggle(ed, e, row);
    if (open) {
        // Copy: the callback may reparent (modifies the children array).
        Array<ecs::Entity> copy;
        copy.Append(children);
        for (ecs::Entity c : copy)
            if (ed.world.IsAlive(c))
                TreeRecursive(ed, c);
        ui::TreePop();
    }
    ui::PopID();
}

} // namespace

void DrawOutlinerPanel(Editor& ed)
{
    if (!ui::Begin(OutlinerTitle, &ed.showOutliner)) {
        ui::End();
        return;
    }
    String& s_filter = ed.panels.outlinerFilter;
    const f32 fh = ui::GetFrameHeight();
    ui::SetNextItemWidth(-(fh / ui::GetCurrentContext()->GetScale()) - 6.0f);
    ui::InputText("##outliner_search", s_filter, ui::InputTextFlags::None, ZX_ICON_SEARCH "  Search...");
    ui::SameLine(0.0f, 4.0f);
    if (ui::IconButton(ZX_ICON_PLUS, "Create"))
        ui::OpenPopup("##outliner_create");
    if (ui::BeginPopup("##outliner_create")) {
        const Vec3 spawn = ed.camera.position + ed.camera.Forward() * 5.0f;
        if (ui::MenuItem(ZX_ICON_BOX "  Empty Entity"))
            ed.CreateEmpty(spawn);
        if (ui::MenuItem(ZX_ICON_LIGHTBULB "  Point Light"))
            ed.CreatePointLight(spawn);
        if (ui::MenuItem(ZX_ICON_SPOTLIGHT "  Spot Light"))
            ed.CreateSpotLight(spawn);
        ui::EndPopup();
    }

    if (ui::BeginChild("##outliner_tree", Vec2(0.0f, -24.0f), false)) {
        Array<ecs::Entity> roots;
        ed.world.Query<const scene::EntityName>().Without<scene::Parent>().Each(
            [&](ecs::Entity e, const scene::EntityName&) { roots.PushBack(e); });
        Sort(roots, [](ecs::Entity a, ecs::Entity b) { return a.index < b.index; });
        if (s_filter.Empty()) {
            for (ecs::Entity e : roots)
                if (ed.world.IsAlive(e))
                    TreeRecursive(ed, e);
        } else {
            // Flat list of matches.
            ed.world.Query<const scene::EntityName>().Each([&](ecs::Entity e, const scene::EntityName& n) {
                if (!ContainsIgnoreCase(n.value, s_filter))
                    return;
                ui::PushID(i32(e.index));
                if (ui::Selectable(String::Format("{}  {}", EntityIcon(ed, e), n.value), e == ed.selected))
                    ed.Select(e);
                ui::PopID();
            });
        }
        // Drop on empty space: detach.
        ui::Dummy(Vec2(1.0f, 40.0f));
        if (const void* p = ui::AcceptDragDropPayload("entity"))
            ed.Reparent(ecs::Entity::Unpack(*static_cast<const u64*>(p)), ecs::NullEntity);
        if (ui::IsWindowHovered() && ui::IsKeyPressed(Key::F2, false) && ed.world.IsAlive(ed.selected))
            ui::FocusDockedWindow(DetailsTitle);
    }
    ui::EndChild();
    ui::TextDisabled(String::Format("{} entities{}", ed.world.EntityCount(),
                                    ed.world.IsAlive(ed.selected) ? String::Format("  ({} selected)", ed.EntityLabel(ed.selected)) : String()));
    ui::End();
}

// --- Output Log ---------------------------------------------------------------------------------------------------------

namespace {

/// Command line under the log: Enter executes, Tab completes, Up/Down pick a suggestion or walk the history.
void DrawConsolePrompt(Editor& ed)
{
    ui::Context& ctx = *ui::GetCurrentContext();
    const f32 s = ctx.GetScale();
    const ui::Theme& c = ctx.GetStyle().colors;
    PanelState& ps = ed.panels;
    ui::AlignTextToFramePadding();
    ui::PushTextColor(c.accentHovered);
    ui::Text(ZX_ICON_CHEVRON_RIGHT);
    ui::PopTextColor();
    ui::SameLine(0.0f, 4.0f);
    ui::SetNextItemWidth(-1.0f);
    const bool submit = ui::InputText("##console", ps.consoleInput, ui::InputTextFlags::EnterReturnsTrue | ui::InputTextFlags::KeepFocusOnEnter,
                                      "Console command (help lists them, Tab completes)");
    const Rect inputRect = ui::GetItemRect();
    const bool active = ui::IsItemActive();

    // Suggestions for the first word while it is being typed.
    Array<const ConsoleObject*> suggestions;
    const StringView typed = ps.consoleInput.View().Trim();
    if (active && !typed.Empty() && typed.Find(' ') == StringView::NPos)
        suggestions = Console::AutoComplete(typed, 8);
    if (suggestions.Size() == 1 && StringView(suggestions[0]->GetName()).EqualsIgnoreCase(typed))
        suggestions.Clear(); // already complete
    ps.consoleSuggestion = suggestions.Empty() ? 0 : Clamp(ps.consoleSuggestion, 0, i32(suggestions.Size()) - 1);

    auto setInput = [&](StringView text) {
        ps.consoleInput = text;
        ui::SetActiveTextCursorToEnd();
    };
    if (active) {
        if (ui::IsKeyPressed(Key::Tab, false) && !suggestions.Empty())
            setInput(String::Format("{} ", suggestions[usize(ps.consoleSuggestion)]->GetName()));
        const bool up = ui::IsKeyPressed(Key::Up), down = ui::IsKeyPressed(Key::Down);
        if ((up || down) && !suggestions.Empty()) {
            const i32 n = i32(suggestions.Size());
            ps.consoleSuggestion = (ps.consoleSuggestion + (up ? n - 1 : 1)) % n;
        } else if ((up || down) && !ps.consoleHistory.Empty()) {
            const i32 n = i32(ps.consoleHistory.Size());
            if (up)
                ps.consoleHistoryPos = ps.consoleHistoryPos < 0 ? n - 1 : Max(ps.consoleHistoryPos - 1, 0);
            else
                ps.consoleHistoryPos = ps.consoleHistoryPos < 0 || ps.consoleHistoryPos + 1 >= n ? -1 : ps.consoleHistoryPos + 1;
            setInput(ps.consoleHistoryPos < 0 ? StringView() : ps.consoleHistory[usize(ps.consoleHistoryPos)].View());
        }
    }
    if (submit) {
        const String line(ps.consoleInput.View().Trim());
        if (!line.Empty()) {
            ZX_LOG_INFO(LogEditorConsole, "> {}", line);
            Console::Execute(line);
            if (ps.consoleHistory.Empty() || ps.consoleHistory.Back() != line)
                ps.consoleHistory.PushBack(line);
            if (ps.consoleHistory.Size() > 100)
                ps.consoleHistory.RemoveAt(0);
            ps.logAutoScroll = true;
        }
        ps.consoleInput.Clear();
        ps.consoleHistoryPos = -1;
        ps.consoleSuggestion = 0;
    }

    // Suggestion list above the prompt (foreground layer, keyboard driven).
    if (!suggestions.Empty()) {
        DrawList& dl = ui::GetForegroundDrawList();
        const ui::FontHandle font = ctx.Fonts().FindFont("Inter");
        const ui::FontHandle bold = ctx.Fonts().FindFont("Inter SemiBold");
        const f32 rowH = Round(22.0f * s), pad = Round(6.0f * s);
        const f32 width = Max(inputRect.Width() * 0.7f, 420.0f * s);
        const Rect box(Vec2(inputRect.min.x, inputRect.min.y - rowH * f32(suggestions.Size()) - pad * 2.0f - 4.0f * s),
                       Vec2(inputRect.min.x + width, inputRect.min.y - 4.0f * s));
        dl.AddRect(box, c.popupBg, ui::CornerRadii(5.0f * s), 1.0f, c.border);
        for (usize i = 0; i < suggestions.Size(); ++i) {
            const ConsoleObject* o = suggestions[i];
            const Rect row(Vec2(box.min.x + 4.0f * s, box.min.y + pad + rowH * f32(i)), Vec2(box.max.x - 4.0f * s, box.min.y + pad + rowH * f32(i + 1)));
            if (i32(i) == ps.consoleSuggestion)
                dl.AddRect(row, c.selection, ui::CornerRadii(3.0f * s));
            const f32 nameW = Round(width * 0.38f);
            dl.AddTextInRect(bold, 12.5f * s, Rect(row.min + Vec2(6.0f * s, 0.0f), Vec2(row.min.x + nameW, row.max.y)), c.text, o->GetName());
            const String detail = o->IsCommand() ? String(o->GetHelp())
                                                 : String::Format("= {}   {}", static_cast<const CVarBase*>(o)->GetValueString(), o->GetHelp());
            dl.AddTextInRect(font, 12.0f * s, Rect(Vec2(row.min.x + nameW + 8.0f * s, row.min.y), row.max - Vec2(6.0f * s, 0.0f)), c.textDim,
                             detail);
        }
    }
}

} // namespace

void DrawOutputLogPanel(Editor& ed)
{
    if (!ui::Begin(LogTitle, &ed.showLog)) {
        ui::End();
        return;
    }
    const ui::Theme& c = ui::GetCurrentContext()->GetStyle().colors;
    PanelState& ps = ed.panels;
    bool& s_info = ps.logInfo;
    bool& s_warn = ps.logWarn;
    bool& s_error = ps.logError;
    bool& s_autoScroll = ps.logAutoScroll;
    String& s_search = ps.logSearch;
    if (ui::Button(ZX_ICON_TRASH_2 "  Clear"))
        ed.log->Clear();
    ui::SameLine();
    ui::Checkbox(String::Format(ZX_ICON_INFO " {}##info", ed.log->Counts(LogLevel::Info)), &s_info);
    ui::SameLine();
    ui::PushTextColor(c.warning);
    ui::Checkbox(String::Format(ZX_ICON_TRIANGLE_ALERT " {}##warn", ed.log->Counts(LogLevel::Warning)), &s_warn);
    ui::PopTextColor();
    ui::SameLine();
    ui::PushTextColor(c.error);
    ui::Checkbox(String::Format(ZX_ICON_CIRCLE_X " {}##error", ed.log->Counts(LogLevel::Error) + ed.log->Counts(LogLevel::Fatal)), &s_error);
    ui::PopTextColor();
    ui::SameLine();
    ui::Checkbox("Auto-scroll", &s_autoScroll);
    ui::SameLine();
    ui::SetNextItemWidth(-1.0f);
    ui::InputText("##logsearch", s_search, ui::InputTextFlags::None, ZX_ICON_SEARCH "  Filter...");

    using Line = PanelState::LogLine;
    Array<Line>& s_lines = ps.logLines;
    u32& s_version = ps.logVersion;
    String& s_lastSearch = ps.logLastSearch;
    u32& s_lastMask = ps.logLastMask;
    const u32 mask = (s_info ? 1u : 0u) | (s_warn ? 2u : 0u) | (s_error ? 4u : 0u);
    const bool changed = s_version != ed.log->Version() || s_lastSearch != s_search || s_lastMask != mask;
    if (changed) {
        s_version = ed.log->Version();
        s_lastSearch = s_search;
        s_lastMask = mask;
        s_lines.Clear();
        ed.log->ForEach([&](const EditorLogSink::Entry& e) {
            const bool isError = e.level >= LogLevel::Error, isWarn = e.level == LogLevel::Warning;
            if ((isError && !s_error) || (isWarn && !s_warn) || (!isError && !isWarn && !s_info))
                return;
            if (!s_search.Empty()) {
                bool found = false;
                for (usize i = 0; i + s_search.Size() <= e.text.Size() && !found; ++i)
                    found = e.text.View().SubStr(i, s_search.Size()).EqualsIgnoreCase(s_search);
                if (!found)
                    return;
            }
            const u32 secs = u32(e.time);
            s_lines.PushBack(Line{e.level, String::Format("[{:02}:{:02}:{:02}] {}: {}", secs / 3600, (secs / 60) % 60, secs % 60, e.category, e.text)});
        });
    }
    const f32 scale = ui::GetCurrentContext()->GetScale();
    const f32 promptH = ui::GetFrameHeight() / scale + 6.0f;
    if (ui::BeginChild("##logview", Vec2(0.0f, -promptH), true)) {
        ui::PushFont(ui::FontStyle::Regular, 12.5f);
        const f32 lineH = ui::GetTextLineHeight();
        ui::ListClipper clip(u32(s_lines.Size()), lineH);
        for (u32 i = clip.begin; i < clip.end; ++i) {
            const Line& l = s_lines[i];
            const Color col = l.level >= LogLevel::Error ? c.error : (l.level == LogLevel::Warning ? c.warning : (l.level <= LogLevel::Debug ? c.textDim : c.text));
            ui::TextColored(col, l.text);
        }
        clip.End();
        ui::PopFont();
        if (s_autoScroll && changed)
            ps.logScrollFrames = 3;
        if (ps.logScrollFrames > 0) {
            --ps.logScrollFrames;
            ui::Dummy(Vec2(1.0f, 1.0f));
            ui::SetScrollHereY(1.0f);
        }
    }
    ui::EndChild();
    DrawConsolePrompt(ed);
    ui::End();
}

} // namespace zx::editor
