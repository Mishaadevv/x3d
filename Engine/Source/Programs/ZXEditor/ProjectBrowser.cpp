// Project Browser: shown when no project is open. Recent projects as cards, a "New Project" page with templates, and
// opening any .zproj from disk.

#include "Editor.h"
#include "Core/Hash/Hash.h"
#include "Core/HAL/PlatformFile.h"
#include "Core/String/Path.h"
#include "Core/Time/Time.h"
#include "Core/Version.h"
#include "Platform/Platform.h"

namespace zx::editor {

namespace {

constexpr f32 SidebarWidth = 236.0f;
constexpr f32 CardWidth = 228.0f;
constexpr f32 CardHeight = 176.0f;
constexpr f32 ThumbHeight = 104.0f;

bool IsAlnum(char ch) { return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9'); }

/// First letter of the project name, upper case ("myGame" -> "M").
String FirstLetter(StringView name)
{
    for (char ch : name)
        if (IsAlnum(ch))
            return String(1, (ch >= 'a' && ch <= 'z') ? char(ch - 32) : ch);
    return "?";
}

/// Project tile: the first letter on the project's color (stored in its .zproj, picked at random on creation).
void DrawLetterTile(DrawList& dl, const Rect& r, StringView name, u32 rgb, f32 fontSize, f32 radius)
{
    const Color base(u8(rgb >> 16), u8(rgb >> 8), u8(rgb));
    const Color dark(u8(base.r * 0.72f), u8(base.g * 0.72f), u8(base.b * 0.72f));
    dl.AddRectGradient(r, base, dark, ui::GradientDirection::Vertical, ui::CornerRadii(radius));
    ui::Context& ctx = *ui::GetCurrentContext();
    dl.AddTextInRect(ctx.Fonts().FindFont("Inter Bold"), fontSize, r, Color(255, 255, 255, 240), FirstLetter(name), ui::TextAlign::Center, false);
}

bool ContainsIgnoreCase(StringView haystack, StringView needle)
{
    for (usize i = 0; i + needle.Size() <= haystack.Size(); ++i)
        if (haystack.SubStr(i, needle.Size()).EqualsIgnoreCase(needle))
            return true;
    return false;
}

String RelativeTime(i64 unixSeconds)
{
    if (unixSeconds <= 0)
        return "Never opened";
    const i64 d = Max<i64>(0, DateTime::UnixTimeNow() - unixSeconds);
    if (d < 60)
        return "Opened just now";
    if (d < 3600)
        return String::Format("Opened {} min ago", d / 60);
    if (d < 86400)
        return String::Format("Opened {} h ago", d / 3600);
    if (d < 86400 * 2)
        return "Opened yesterday";
    if (d < 86400 * 60)
        return String::Format("Opened {} days ago", d / 86400);
    return String::Format("Opened {} months ago", d / (86400 * 30));
}

void OpenOtherProject(Editor& ed)
{
    static const platform::FileDialogFilter Filters[] = {{"ZeqouX3D project", "*.zproj"}, {"All files", "*.*"}};
    const Array<String> paths = platform::OpenFileDialog(ed.window.Get(), "Open Project", Span<const platform::FileDialogFilter>(Filters, 2),
                                                         false, DefaultProjectsDirectory());
    if (!paths.Empty())
        ed.OpenProject(paths[0]);
}

void DrawSidebar(Editor& ed)
{
    ui::Context& ctx = *ui::GetCurrentContext();
    const f32 s = ctx.GetScale();
    const ui::Theme& c = ctx.GetStyle().colors;
    DrawList& dl = ui::GetWindowDrawList();
    const Rect area = ui::GetWindowContentRect();
    ProjectBrowserState& b = ed.browser;

    // Brand.
    const Vec2 p = ui::GetCursorPos();
    const Rect logo(p + Vec2(Round(4.0f * s), Round(6.0f * s)), p + Vec2(Round(40.0f * s), Round(42.0f * s)));
    if (ed.appIcon) {
        dl.AddImage(ed.AppIconSrv(), logo);
    } else {
        dl.AddRectGradient(logo, Color(0x3B, 0x8C, 0xF5), Color(0x6A, 0x3D, 0xE8), ui::GradientDirection::Vertical, ui::CornerRadii(8.0f * s));
        dl.AddTextInRect(ctx.Fonts().FindFont("Inter Bold"), 21.0f * s, logo, Color(255, 255, 255), "Z", ui::TextAlign::Center, false);
    }
    dl.AddTextInRect(ctx.Fonts().FindFont("Inter Bold"), 16.0f * s,
                     Rect(Vec2(logo.max.x + 10.0f * s, logo.min.y), Vec2(area.max.x, logo.Center().y + 2.0f * s)), c.text, "ZeqouX3D");
    dl.AddTextInRect(ctx.Fonts().FindFont("Inter"), 11.5f * s,
                     Rect(Vec2(logo.max.x + 10.0f * s, logo.Center().y + 2.0f * s), Vec2(area.max.x, logo.max.y)), c.textDim,
                     String::Format("Engine {}", EngineVersionString));
    ui::Dummy(Vec2(10.0f, 56.0f));

    ui::TextDisabled("PROJECTS");
    ui::Spacing();
    const Vec2 navSize(SidebarWidth - 24.0f, 30.0f);
    if (ui::Selectable(ZX_ICON_CLOCK "   Recent Projects", !b.newProjectPage, navSize))
        b.newProjectPage = false;
    if (ui::Selectable(ZX_ICON_FOLDER_PLUS "   New Project", b.newProjectPage, navSize)) {
        b.newProjectPage = true;
        b.error.Clear();
    }
    if (ui::Selectable(ZX_ICON_FOLDER_OPEN "   Open Other...", false, navSize))
        OpenOtherProject(ed);

    // Footer: device info.
    const f32 footer = Round(46.0f * s);
    const Rect fr(Vec2(area.min.x, area.max.y - footer), area.max);
    dl.AddRect(Rect(fr.min, Vec2(fr.max.x, fr.min.y + 1.0f)), c.separator);
    dl.AddTextInRect(ctx.Fonts().FindFont("Inter"), 11.5f * s, Rect(fr.min + Vec2(0.0f, 6.0f * s), Vec2(fr.max.x, fr.Center().y + 3.0f * s)),
                     c.textDim, String::Format(ZX_ICON_CPU "  {}", ed.device->GetCaps().adapter.name));
    dl.AddTextInRect(ctx.Fonts().FindFont("Inter"), 11.5f * s, Rect(Vec2(fr.min.x, fr.Center().y + 3.0f * s), fr.max), c.textDisabled,
                     ed.lite ? "Lite profile (integrated GPU)" : "Full profile");
}

/// One project card; returns true when it was activated (double click / Enter on the selection).
bool ProjectCard(Editor& ed, const RecentProject& project, i32 index, bool exists, u32 color)
{
    ui::Context& ctx = *ui::GetCurrentContext();
    const f32 s = ctx.GetScale();
    const ui::Theme& c = ctx.GetStyle().colors;
    DrawList& dl = ui::GetWindowDrawList();
    ProjectBrowserState& b = ed.browser;

    ui::PushID(project.path);
    const Vec2 pos = ui::GetCursorPos();
    const Rect r(pos, pos + Vec2(Round(CardWidth * s), Round(CardHeight * s)));
    ui::InvisibleButton("##card", Vec2(CardWidth, CardHeight));
    const bool hovered = ui::IsItemHovered();
    const bool selected = b.selected == index;
    bool activate = false;
    if (ui::IsItemClicked())
        b.selected = index;
    if (ui::IsItemDoubleClicked() && exists)
        activate = true;

    const ui::CornerRadii radii(8.0f * s);
    dl.AddRect(r, hovered ? Color(0x2E, 0x2E, 0x2E) : Color(0x29, 0x29, 0x29), radii, 1.0f,
               selected ? c.accent : (hovered ? c.borderStrong : c.border));
    if (selected)
        dl.AddRect(r, Color(0, 0, 0, 0), radii, 2.0f, c.accent);
    const Rect thumb(r.min + Vec2(Round(6.0f * s)), Vec2(r.max.x - Round(6.0f * s), r.min.y + Round(ThumbHeight * s)));
    if (exists) {
        DrawLetterTile(dl, thumb, project.name, color, 46.0f * s, 5.0f * s);
    } else {
        dl.AddRect(thumb, Color(0x22, 0x22, 0x22), ui::CornerRadii(5.0f * s), 1.0f, c.border);
        dl.AddTextInRect(ctx.Fonts().FindFont("Inter"), 30.0f * s, thumb, c.warning, ZX_ICON_TRIANGLE_ALERT, ui::TextAlign::Center, false);
    }
    const f32 x0 = r.min.x + Round(10.0f * s), x1 = r.max.x - Round(10.0f * s);
    f32 y = thumb.max.y + Round(8.0f * s);
    dl.AddTextInRect(ctx.Fonts().FindFont("Inter SemiBold"), 14.0f * s, Rect(Vec2(x0, y), Vec2(x1, y + 20.0f * s)), c.text, project.name);
    y += Round(20.0f * s);
    dl.AddTextInRect(ctx.Fonts().FindFont("Inter"), 11.5f * s, Rect(Vec2(x0, y), Vec2(x1, y + 17.0f * s)), exists ? c.textDim : c.error,
                     exists ? String(Path::Parent(Path::Parent(project.path))) : String("Project not found"));
    y += Round(17.0f * s);
    dl.AddTextInRect(ctx.Fonts().FindFont("Inter"), 11.5f * s, Rect(Vec2(x0, y), Vec2(x1, y + 17.0f * s)), c.textDisabled,
                     RelativeTime(project.lastOpened));
    ui::ItemTooltip(project.path);

    if (ui::BeginPopupContextItem("##card_menu")) {
        b.selected = index;
        if (ui::MenuItem(ZX_ICON_FOLDER_OPEN "  Open", {}, false, exists))
            activate = true;
        if (ui::MenuItem(ZX_ICON_FOLDER_SEARCH "  Show in Explorer", {}, false, exists))
            platform::RevealInFileExplorer(project.path);
        ui::Separator();
        if (ui::MenuItem(ZX_ICON_X "  Remove from List")) {
            RemoveRecentProject(project.path, ed.recentProjectsFile);
            b.recentLoaded = false;
            b.selected = -1;
        }
        ui::EndPopup();
    }
    ui::PopID();
    return activate;
}

void DrawRecentPage(Editor& ed)
{
    ui::Context& ctx = *ui::GetCurrentContext();
    const f32 s = ctx.GetScale();
    const ui::Theme& c = ctx.GetStyle().colors;
    ProjectBrowserState& b = ed.browser;
    if (!b.recentLoaded) {
        b.recent = LoadRecentProjects(ed.recentProjectsFile);
        b.recentLoaded = true;
        b.recentColors.Clear();
        for (const RecentProject& p : b.recent) {
            Result<ProjectInfo> info = LoadProject(p.path);
            ProjectDescriptor d;
            d.name = p.name;
            b.recentColors.PushBack(ProjectTileColor(info ? info->descriptor : d));
        }
        b.selected = b.recent.Empty() ? -1 : 0;
    }

    // Header row: title + search.
    ui::PushFont(ui::FontStyle::SemiBold, 20.0f);
    ui::Text("Recent Projects");
    ui::PopFont();
    ui::SameLine(Max(0.0f, ui::GetWindowContentRect().Width() / s - 260.0f));
    ui::SetNextItemWidth(260.0f);
    ui::InputText("##project_search", b.search, ui::InputTextFlags::None, ZX_ICON_SEARCH "  Search projects...");
    ui::Spacing();
    ui::Spacing();

    const f32 bottomBar = 56.0f;
    const Vec2 avail = ui::GetContentRegionAvail() / s;
    i32 activated = -1;
    if (ui::BeginChild("##project_cards", Vec2(0.0f, avail.y - bottomBar), false)) {
        const f32 width = ui::GetWindowContentRect().Width();
        const f32 gap = Round(14.0f * s);
        const u32 perRow = Max(1u, u32((width + gap) / (Round(CardWidth * s) + gap)));
        u32 col = 0, shown = 0;
        for (usize i = 0; i < b.recent.Size(); ++i) {
            const RecentProject& p = b.recent[i];
            if (!b.search.Empty() && !ContainsIgnoreCase(p.name, b.search) && !ContainsIgnoreCase(p.path, b.search))
                continue;
            if (col > 0)
                ui::SameLine(0.0f, gap / s);
            if (ProjectCard(ed, p, i32(i), platform::FileExists(p.path), i < b.recentColors.Size() ? b.recentColors[i] : 0x3B8CF5u))
                activated = i32(i);
            if (!b.recentLoaded)
                break; // removed from the list inside the card's menu
            col = (col + 1) % perRow;
            ++shown;
            if (col == 0)
                ui::Dummy(Vec2(1.0f, 6.0f));
        }
        if (shown == 0) {
            ui::Dummy(Vec2(1.0f, 40.0f));
            const Rect r = ui::GetWindowContentRect();
            DrawList& dl = ui::GetWindowDrawList();
            const Rect msg(Vec2(r.min.x, r.min.y + 40.0f * s), Vec2(r.max.x, r.min.y + 120.0f * s));
            dl.AddTextInRect(ctx.Fonts().FindFont("Inter"), 40.0f * s, Rect(msg.min, Vec2(msg.max.x, msg.Center().y)), c.textDisabled,
                             ZX_ICON_FOLDER_KANBAN, ui::TextAlign::Center, false);
            dl.AddTextInRect(ctx.Fonts().FindFont("Inter"), 14.0f * s, Rect(Vec2(msg.min.x, msg.Center().y + 8.0f * s), msg.max), c.textDim,
                             b.recent.Empty() ? "No projects yet - create your first one" : "No projects match the search",
                             ui::TextAlign::Center, false);
            ui::Dummy(Vec2(1.0f, 100.0f));
            if (b.recent.Empty()) {
                ui::SetCursorPos(Vec2(Round(r.Center().x - 80.0f * s), ui::GetCursorPos().y));
                if (ui::Button(ZX_ICON_FOLDER_PLUS "  New Project", Vec2(160.0f, 32.0f), ui::ButtonVariant::Primary))
                    b.newProjectPage = true;
            }
        }
    }
    ui::EndChild();

    // Bottom bar: selection + actions.
    const bool valid = b.selected >= 0 && usize(b.selected) < b.recent.Size();
    const bool exists = valid && platform::FileExists(b.recent[b.selected].path);
    ui::Separator();
    ui::Spacing();
    ui::AlignTextToFramePadding();
    if (valid)
        ui::TextDisabled(b.recent[b.selected].path);
    else
        ui::TextDisabled("Select a project");
    ui::SameLine(Max(0.0f, ui::GetWindowContentRect().Width() / s - 236.0f));
    if (ui::Button(ZX_ICON_FOLDER_OPEN "  Browse...", Vec2(110.0f, 32.0f)))
        OpenOtherProject(ed);
    ui::SameLine(0.0f, 8.0f);
    ui::BeginDisabled(!exists);
    if ((ui::Button("Open", Vec2(110.0f, 32.0f), ui::ButtonVariant::Primary) || (ui::IsKeyPressed(Key::Enter, false) && !ctx.WantsKeyboard())) &&
        exists)
        activated = b.selected;
    ui::EndDisabled();

    if (activated >= 0 && !b.error.Empty())
        b.error.Clear();
    if (activated >= 0) {
        const String path = b.recent[activated].path;
        if (!ed.OpenProject(path))
            b.recentLoaded = false;
    }
    if (!b.error.Empty())
        ui::TextColored(c.error, b.error);
}

// --- Templates ---------------------------------------------------------------------------------------------------------

struct TemplateInfo
{
    ProjectTemplate templ;
    StringView title;
    StringView description;
    StringView availableIn; // empty = available now
};

const TemplateInfo Templates[] = {
    {ProjectTemplate::Blank, "Blank", "An empty level with the physical sky and sun. Start from scratch.", {}},
    {ProjectTemplate::Sample, "Sample Scene", "A PBR model (Damaged Helmet) with lights, ready to explore and edit.", {}},
    {ProjectTemplate::FirstPerson, "First Person", "A first-person character with mouse look, walking, sprinting and jumping on a small playground.", {}},
    {ProjectTemplate::ThirdPerson, "Third Person", "A visible character with an orbiting follow camera, walking and jumping on a playground.", {}},
    {ProjectTemplate::Water, "Water", "An island in the sea with realistic water: waves, reflections, refraction, foam. Walk around in first person.", {}},
};

Color Hex(u32 rgb, u8 a = 255) { return Color(u8(rgb >> 16), u8(rgb >> 8), u8(rgb), a); }

Color Mix(Color a, Color b, f32 t)
{
    return Color(u8(a.r + (b.r - a.r) * t), u8(a.g + (b.g - a.g) * t), u8(a.b + (b.b - a.b) * t), u8(a.a + (b.a - a.a) * t));
}

/// Ground plane with perspective lines towards the horizon.
void DrawGround(DrawList& dl, const Rect& r, f32 horizon, Color top, Color bottom, Color lines, f32 s)
{
    const Rect ground(Vec2(r.min.x, horizon), r.max);
    dl.AddRectGradient(ground, top, bottom, ui::GradientDirection::Vertical);
    const Vec2 vp(r.Center().x, horizon);
    for (i32 i = -6; i <= 6; ++i)
        dl.AddLine(vp, Vec2(r.Center().x + f32(i) * r.Width() * 0.22f, r.max.y), lines, Max(1.0f, s));
    for (f32 t = 0.08f; t < 1.0f; t = t * 1.7f + 0.05f) {
        const f32 y = horizon + (r.max.y - horizon) * t;
        dl.AddLine(Vec2(r.min.x, y), Vec2(r.max.x, y), lines, Max(1.0f, s));
    }
}

/// Small illustration of each template (drawn, so it scales with DPI and needs no image files).
void DrawTemplateArt(DrawList& dl, const Rect& r, ProjectTemplate t, f32 s)
{
    dl.PushClipRect(r, 6.0f * s);
    const f32 w = r.Width(), h = r.Height();
    auto at = [&](f32 x, f32 y) { return Vec2(r.min.x + w * x, r.min.y + h * y); };
    switch (t) {
    case ProjectTemplate::Blank: {
        const f32 horizon = r.min.y + h * 0.62f;
        dl.AddRectGradient(Rect(r.min, Vec2(r.max.x, horizon)), Hex(0x3E78C9), Hex(0xBFD8F0), ui::GradientDirection::Vertical);
        for (u32 i = 4; i > 0; --i)
            dl.AddCircle(at(0.72f, 0.3f), h * (0.07f + 0.05f * f32(i)), Hex(0xFFF4D6, u8(26)));
        dl.AddCircle(at(0.72f, 0.3f), h * 0.075f, Hex(0xFFF8E7));
        DrawGround(dl, r, horizon, Hex(0x7A8A6E), Hex(0x4C5A44), Hex(0xFFFFFF, 30), s);
        break;
    }
    case ProjectTemplate::Sample: {
        dl.AddRectGradient(r, Hex(0x2B3140), Hex(0x111318), ui::GradientDirection::Vertical);
        dl.AddCircle(at(0.18f, 0.25f), h * 0.22f, Hex(0xF0A050, 40));
        dl.AddCircle(at(0.85f, 0.35f), h * 0.22f, Hex(0x5A8CF0, 40));
        dl.AddRect(Rect(at(0.3f, 0.8f), at(0.7f, 0.9f)), Hex(0x000000, 90), ui::CornerRadii(h * 0.05f));
        const Vec2 c = at(0.5f, 0.5f);
        const f32 rad = h * 0.3f;
        dl.AddCircle(c, rad, Hex(0x4A4D55));
        dl.AddCircle(c + Vec2(-rad * 0.15f, -rad * 0.15f), rad * 0.8f, Hex(0x6A6E78));
        dl.AddCircle(c + Vec2(-rad * 0.3f, -rad * 0.3f), rad * 0.45f, Hex(0x9095A0));
        dl.AddCircle(c + Vec2(-rad * 0.4f, -rad * 0.42f), rad * 0.13f, Hex(0xF4F0E8));
        dl.AddRect(Rect(c + Vec2(-rad * 0.75f, -rad * 0.05f), c + Vec2(rad * 0.75f, rad * 0.3f)), Hex(0xF0A050, 150), ui::CornerRadii(rad * 0.15f));
        break;
    }
    case ProjectTemplate::FirstPerson: {
        const f32 horizon = r.min.y + h * 0.5f;
        dl.AddRectGradient(Rect(r.min, Vec2(r.max.x, horizon)), Hex(0x5C87C8), Hex(0xC6D9EE), ui::GradientDirection::Vertical);
        DrawGround(dl, r, horizon, Hex(0x8C8270), Hex(0x5A5246), Hex(0xFFFFFF, 26), s);
        // Corridor walls.
        const Vec2 left[] = {at(0, 0.05f), at(0.3f, 0.32f), at(0.3f, 0.62f), at(0, 1)};
        const Vec2 right[] = {at(1, 0.05f), at(1, 1), at(0.7f, 0.62f), at(0.7f, 0.32f)};
        dl.AddConvexPolygon(Span<const Vec2>(left, 4), Hex(0x7E6F60));
        dl.AddConvexPolygon(Span<const Vec2>(right, 4), Hex(0x6A5D50));
        // Crosshair and a hand-held tool in the corner.
        const Vec2 c = at(0.5f, 0.47f);
        const f32 k = h * 0.05f;
        dl.AddLine(c - Vec2(k * 1.6f, 0), c - Vec2(k * 0.5f, 0), Hex(0xFFFFFF), 2.0f * s);
        dl.AddLine(c + Vec2(k * 0.5f, 0), c + Vec2(k * 1.6f, 0), Hex(0xFFFFFF), 2.0f * s);
        dl.AddLine(c - Vec2(0, k * 1.6f), c - Vec2(0, k * 0.5f), Hex(0xFFFFFF), 2.0f * s);
        dl.AddLine(c + Vec2(0, k * 0.5f), c + Vec2(0, k * 1.6f), Hex(0xFFFFFF), 2.0f * s);
        const Vec2 tool[] = {at(0.62f, 1.0f), at(0.74f, 0.66f), at(0.86f, 0.62f), at(0.96f, 1.0f)};
        dl.AddConvexPolygon(Span<const Vec2>(tool, 4), Hex(0x2A2D33));
        dl.AddRect(Rect(at(0.73f, 0.6f), at(0.86f, 0.68f)), Hex(0x3B8CF5), ui::CornerRadii(2.0f * s));
        break;
    }
    case ProjectTemplate::ThirdPerson: {
        const f32 horizon = r.min.y + h * 0.58f;
        dl.AddRectGradient(Rect(r.min, Vec2(r.max.x, horizon)), Hex(0x6A8FD0), Hex(0xD4E2F2), ui::GradientDirection::Vertical);
        DrawGround(dl, r, horizon, Hex(0x6E8F5E), Hex(0x3F5A35), Hex(0xFFFFFF, 26), s);
        const Vec2 feet = at(0.5f, 0.82f);
        dl.AddRect(Rect(feet - Vec2(h * 0.16f, h * 0.035f), feet + Vec2(h * 0.16f, h * 0.035f)), Hex(0x000000, 70), ui::CornerRadii(h * 0.04f));
        const f32 bw = h * 0.13f;
        dl.AddRect(Rect(Vec2(feet.x - bw * 0.5f, feet.y - h * 0.42f), Vec2(feet.x + bw * 0.5f, feet.y)), Hex(0xF0883E), ui::CornerRadii(bw * 0.5f));
        dl.AddCircle(Vec2(feet.x, feet.y - h * 0.5f), h * 0.075f, Hex(0xF6C9A0));
        // Camera orbit.
        dl.AddBezier(at(0.16f, 0.55f), at(0.2f, 0.1f), at(0.8f, 0.1f), at(0.84f, 0.55f), Hex(0xFFFFFF, 150), 1.5f * s);
        dl.AddRect(Rect(at(0.13f, 0.5f), at(0.21f, 0.62f)), Hex(0x1E2228), ui::CornerRadii(2.0f * s));
        break;
    }
    case ProjectTemplate::Water: {
        const f32 horizon = r.min.y + h * 0.5f;
        dl.AddRectGradient(Rect(r.min, Vec2(r.max.x, horizon)), Hex(0x2E3F78), Hex(0xF2A65A), ui::GradientDirection::Vertical);
        dl.AddCircle(Vec2(r.min.x + w * 0.62f, horizon), h * 0.13f, Hex(0xFFE2A8));
        dl.AddRectGradient(Rect(Vec2(r.min.x, horizon), r.max), Hex(0x2C6E8E), Hex(0x0B2A44), ui::GradientDirection::Vertical);
        // Sun glitter and wave crests.
        for (u32 i = 0; i < 7; ++i) {
            const f32 y = horizon + h * (0.04f + 0.06f * f32(i));
            const f32 half = w * (0.03f + 0.02f * f32(i));
            dl.AddLine(Vec2(r.min.x + w * 0.62f - half, y), Vec2(r.min.x + w * 0.62f + half, y), Hex(0xFFD99A, u8(180 - i * 20)), Max(1.0f, 1.5f * s));
        }
        for (u32 band = 0; band < 3; ++band) {
            Array<Vec2> pts;
            const f32 y0 = horizon + h * (0.2f + 0.13f * f32(band));
            for (u32 i = 0; i <= 24; ++i) {
                const f32 x = f32(i) / 24.0f;
                pts.PushBack(Vec2(r.min.x + w * x, y0 + Sin(x * 18.0f + f32(band) * 1.7f) * h * 0.012f * f32(band + 1)));
            }
            dl.AddPolyline(pts.View(), Hex(0x9AD6EA, u8(110 - band * 25)), Max(1.0f, 1.5f * s));
        }
        // Island.
        const Vec2 island[] = {at(0.06f, 0.5f), at(0.14f, 0.43f), at(0.27f, 0.44f), at(0.34f, 0.5f)};
        dl.AddConvexPolygon(Span<const Vec2>(island, 4), Hex(0x24382A));
        break;
    }
    default: break;
    }
    dl.PopClipRect();
}

/// Template tile: illustration + title; "coming in x.y.z" templates are dimmed with a badge.
bool TemplateTile(const TemplateInfo& info, bool selected, Vec2 size)
{
    ui::Context& ctx = *ui::GetCurrentContext();
    const f32 s = ctx.GetScale();
    const ui::Theme& c = ctx.GetStyle().colors;
    DrawList& dl = ui::GetWindowDrawList();
    const Vec2 pos = ui::GetCursorPos();
    const Rect r(pos, pos + Vec2(Round(size.x * s), Round(size.y * s)));
    ui::PushID(info.title);
    const bool clicked = ui::InvisibleButton("##tile", size);
    const bool hovered = ui::IsItemHovered();
    ui::PopID();
    const bool soon = !info.availableIn.Empty();
    const bool version = soon && info.availableIn[0] >= '0' && info.availableIn[0] <= '9';
    const ui::CornerRadii radii(8.0f * s);
    dl.AddRect(r, selected ? WithAlpha(c.accent, 0.12f) : (hovered ? Color(0x2E, 0x2E, 0x2E) : Color(0x27, 0x27, 0x27)), radii,
               selected ? 2.0f : 1.0f, selected ? c.accent : (hovered ? c.borderStrong : c.border));
    const f32 pad = Round(6.0f * s);
    const Rect art(r.min + Vec2(pad), Vec2(r.max.x - pad, r.max.y - Round(34.0f * s)));
    DrawTemplateArt(dl, art, info.templ, s);
    if (soon) {
        dl.AddRect(art, Color(0x18, 0x18, 0x18, 140), ui::CornerRadii(6.0f * s));
        const String badge = version ? String::Format("COMING IN {}", info.availableIn) : String(info.availableIn);
        const ui::FontHandle bold = ctx.Fonts().FindFont("Inter Bold");
        const f32 bw = ctx.Fonts().MeasureWidth(bold, 10.0f * s, badge) + 14.0f * s;
        const Rect b(Vec2(art.max.x - bw - 6.0f * s, art.min.y + 6.0f * s), Vec2(art.max.x - 6.0f * s, art.min.y + 24.0f * s));
        dl.AddRect(b, Color(0x10, 0x10, 0x10, 220), ui::CornerRadii(9.0f * s), 1.0f, Color(255, 255, 255, 50));
        dl.AddTextInRect(bold, 10.0f * s, b, Color(0xF0, 0xC0, 0x60), badge, ui::TextAlign::Center, false);
    }
    dl.AddTextInRect(ctx.Fonts().FindFont("Inter SemiBold"), 13.5f * s, Rect(Vec2(art.min.x + 4.0f * s, art.max.y), Vec2(r.max.x - pad, r.max.y)),
                     soon ? c.textDim : c.text, info.title, ui::TextAlign::Left, false);
    if (soon)
        ui::ItemTooltip(version ? String::Format("{}\nAvailable in version {}", info.description, info.availableIn)
                                : String::Format("{}\nRun Tools/fetch_samples.ps1 to download the sample content", info.description));
    return clicked && !soon;
}

/// One choice row (icon, title, subtitle); disabled rows show when they arrive.
bool ChoiceRow(StringView id, StringView icon, StringView title, StringView subtitle, bool selected, bool enabled, Color tint)
{
    ui::Context& ctx = *ui::GetCurrentContext();
    const f32 s = ctx.GetScale();
    const ui::Theme& c = ctx.GetStyle().colors;
    DrawList& dl = ui::GetWindowDrawList();
    const Vec2 pos = ui::GetCursorPos();
    const f32 width = ui::GetContentRegionAvail().x / s;
    const Rect r(pos, pos + Vec2(Round(width * s), Round(46.0f * s)));
    ui::BeginDisabled(!enabled);
    const bool clicked = ui::InvisibleButton(id, Vec2(width, 46.0f));
    const bool hovered = ui::IsItemHovered();
    ui::EndDisabled();
    dl.AddRect(r, selected ? WithAlpha(c.accent, 0.14f) : (hovered && enabled ? Color(0x2E, 0x2E, 0x2E) : Color(0, 0, 0, 0)),
               ui::CornerRadii(6.0f * s), 1.0f, selected ? c.accent : c.border);
    const Rect iconRect(r.min + Vec2(Round(8.0f * s)), Vec2(r.min.x + Round(38.0f * s), r.max.y - Round(8.0f * s)));
    dl.AddRect(iconRect, WithAlpha(tint, enabled ? 0.2f : 0.08f), ui::CornerRadii(6.0f * s));
    dl.AddTextInRect(ctx.Fonts().FindFont("Inter"), 15.0f * s, iconRect, enabled ? tint : c.textDisabled, icon, ui::TextAlign::Center, false);
    const f32 tx = iconRect.max.x + Round(10.0f * s);
    dl.AddTextInRect(ctx.Fonts().FindFont("Inter SemiBold"), 13.0f * s, Rect(Vec2(tx, r.min.y + 5.0f * s), Vec2(r.max.x - 8.0f * s, r.Center().y + 1.0f * s)),
                     enabled ? c.text : c.textDisabled, title, ui::TextAlign::Left, false);
    dl.AddTextInRect(ctx.Fonts().FindFont("Inter"), 11.0f * s, Rect(Vec2(tx, r.Center().y + 1.0f * s), Vec2(r.max.x - 8.0f * s, r.max.y - 4.0f * s)),
                     c.textDisabled, subtitle, ui::TextAlign::Left, false);
    return clicked && enabled;
}

void DrawNewProjectPage(Editor& ed)
{
    ui::Context& ctx = *ui::GetCurrentContext();
    const f32 s = ctx.GetScale();
    const ui::Theme& c = ctx.GetStyle().colors;
    ProjectBrowserState& b = ed.browser;
    NewProjectOptions& o = b.options;
    if (b.newLocation.Empty())
        b.newLocation = DefaultProjectsDirectory();
    const bool sampleAvailable = platform::FileExists(ed.engineRoot + "/Samples/Content/DamagedHelmet/DamagedHelmet.gltf");
    if (!sampleAvailable && o.templ == ProjectTemplate::Sample)
        o.templ = ProjectTemplate::Blank;

    ui::PushFont(ui::FontStyle::SemiBold, 20.0f);
    ui::Text("New Project");
    ui::PopFont();
    ui::TextDisabled("Pick a template, then set up the project on the right.");
    ui::Spacing();
    ui::Spacing();

    const Vec2 avail = ui::GetContentRegionAvail() / s;
    const f32 panelW = 360.0f, gap = 20.0f;
    const f32 gridW = Max(240.0f, avail.x - panelW - gap);

    // --- Template grid ---
    if (ui::BeginChild("##templates", Vec2(gridW, avail.y - 4.0f), false)) {
        ui::TextDisabled("TEMPLATES");
        ui::Spacing();
        const f32 tileGap = 14.0f;
        const u32 perRow = Max(1u, u32((gridW + tileGap) / (180.0f + tileGap)));
        const f32 tileW = Min(300.0f, (gridW - tileGap * f32(perRow - 1) - 4.0f) / f32(perRow));
        const Vec2 tile(tileW, tileW * 0.56f + 40.0f);
        u32 col = 0;
        for (const TemplateInfo& t : Templates) {
            if (col > 0)
                ui::SameLine(0.0f, tileGap);
            TemplateInfo info = t;
            if (t.templ == ProjectTemplate::Sample && !sampleAvailable)
                info.availableIn = "NEEDS SAMPLE CONTENT";
            if (TemplateTile(info, o.templ == t.templ, tile))
                o.templ = t.templ;
            col = (col + 1) % perRow;
            if (col == 0)
                ui::Dummy(Vec2(1.0f, 6.0f));
        }
    }
    ui::EndChild();
    ui::SameLine(0.0f, gap);

    // --- Settings panel ---
    const Vec2 p = ui::GetCursorPos();
    ui::GetWindowDrawList().AddRect(Rect(p, p + Vec2(Round(panelW * s), Round((avail.y - 4.0f) * s))), Color(0x24, 0x24, 0x24),
                                    ui::CornerRadii(8.0f * s), 1.0f, c.border);
    ui::SetCursorPos(p + Vec2(Round(16.0f * s), Round(14.0f * s)));
    if (ui::BeginChild("##project_settings", Vec2(panelW - 32.0f, avail.y - 32.0f), false)) {
        const TemplateInfo* selected = &Templates[0];
        for (const TemplateInfo& t : Templates)
            if (t.templ == o.templ)
                selected = &t;
        ui::PushFont(ui::FontStyle::SemiBold, 16.0f);
        ui::Text(selected->title);
        ui::PopFont();
        ui::PushTextColor(c.textDim);
        ui::TextWrapped(selected->description);
        ui::PopTextColor();
        ui::Spacing();
        ui::Spacing();

        ui::TextDisabled("LANGUAGE");
        if (ChoiceRow("##lang_zs", ZX_ICON_FILE_CODE, "ZeqouScript", "The engine's own scripting language, hot reload",
                      o.language == ProjectLanguage::ZeqouScript, true, Color(0x4E, 0xC9, 0xB0)))
            o.language = ProjectLanguage::ZeqouScript;
        ui::ItemTooltip("Simple, readable scripts attached to entities (docs/ZeqouScript.md)");
        ChoiceRow("##lang_cpp", ZX_ICON_BRACES, "C++", "Native gameplay modules - coming in M10", o.language == ProjectLanguage::Cpp, false,
                  Color(0x5A, 0xA0, 0xF5));
        ChoiceRow("##lang_vs", ZX_ICON_WORKFLOW, "Visual Script", "Node graphs (ZGraph) - coming in M14", o.language == ProjectLanguage::VisualScript,
                  false, Color(0xB0, 0x7C, 0xF0));
        ui::Spacing();
        ui::Spacing();

        ui::TextDisabled("CONTENT");
        ui::Checkbox("Starter Content", &o.starterContent);
        ui::ItemTooltip("Adds Content/StarterContent: 10 PBR materials with editable parameters and ready-made scripts\n"
                        "(Rotator, Bobbing, SimpleMover, LookAt, Blinker). Blank projects also get a small showcase scene.");
        ui::Checkbox("Documentation", &o.documentation);
        ui::ItemTooltip("README.md and Docs/ (getting started, ZeqouScript reference) for your team,\n"
                        "plus AGENTS.md / CLAUDE.md so AI coding assistants understand the project.");
        ui::Checkbox("Git repository", &o.gitRepository);
        ui::ItemTooltip("git init with .gitignore / .gitattributes and an initial commit (needs Git installed).");
        ui::Spacing();
        ui::Spacing();

        ui::TextDisabled("PROJECT NAME");
        ui::SetNextItemWidth(-1.0f);
        const bool enter = ui::InputText("##new_name", b.newName, ui::InputTextFlags::EnterReturnsTrue);
        ui::Spacing();
        ui::TextDisabled("LOCATION");
        ui::SetNextItemWidth(-(ui::GetFrameHeight() / s + 4.0f));
        ui::InputText("##new_location", b.newLocation);
        ui::SameLine(0.0f, 4.0f);
        if (ui::IconButton(ZX_ICON_FOLDER_OPEN, "Choose the folder")) {
            const String folder = platform::PickFolderDialog(ed.window.Get(), "Choose the project location", b.newLocation);
            if (!folder.Empty())
                b.newLocation = folder;
        }
        const String reason = ValidateProjectName(b.newName);
        ui::PushTextColor(reason.Empty() ? c.textDisabled : c.warning);
        ui::TextWrapped(reason.Empty() ? String::Format(ZX_ICON_FOLDER "  {}", Path::Join(Path::Normalize(b.newLocation), b.newName))
                                       : String::Format(ZX_ICON_TRIANGLE_ALERT "  {}", reason));
        ui::PopTextColor();
        if (!b.error.Empty()) {
            ui::PushTextColor(c.error);
            ui::TextWrapped(String::Format(ZX_ICON_CIRCLE_X "  {}", b.error));
            ui::PopTextColor();
        }
        ui::Spacing();
        ui::Spacing();
        const f32 bw = (ui::GetContentRegionAvail().x / s - 8.0f) * 0.5f;
        if (ui::Button("Cancel", Vec2(bw, 34.0f))) {
            b.newProjectPage = false;
            b.error.Clear();
        }
        ui::SameLine(0.0f, 8.0f);
        ui::BeginDisabled(!reason.Empty());
        const bool create = ui::Button(ZX_ICON_PLUS "  Create", Vec2(bw, 34.0f), ui::ButtonVariant::Primary);
        ui::EndDisabled();
        if ((create || enter) && reason.Empty()) {
            b.error.Clear();
            ed.CreateAndOpenProject(b.newLocation, b.newName, o);
        }
    }
    ui::EndChild();
}

} // namespace

void DrawProjectBrowser(Editor& ed, const Rect& area)
{
    ui::Context& ctx = *ui::GetCurrentContext();
    const f32 s = ctx.GetScale();
    const ui::Theme& c = ctx.GetStyle().colors;
    ui::SetNextWindowPos(area.min / s, ui::Cond::Always);
    ui::SetNextWindowSize(area.Size() / s, ui::Cond::Always);
    ui::Begin("##ProjectBrowser", nullptr,
              ui::WindowFlags::NoTitleBar | ui::WindowFlags::NoResize | ui::WindowFlags::NoMove | ui::WindowFlags::NoScrollbar |
                  ui::WindowFlags::NoCollapse | ui::WindowFlags::NoBringToFront | ui::WindowFlags::NoBackground);
    DrawList& dl = ui::GetWindowDrawList();
    const Rect sidebar(area.min, Vec2(area.min.x + Round(SidebarWidth * s), area.max.y));
    dl.AddRect(sidebar, c.titleBg);
    dl.AddRect(Rect(Vec2(sidebar.max.x, area.min.y), area.max), c.windowBg);
    dl.AddRect(Rect(Vec2(sidebar.max.x - 1.0f, area.min.y), Vec2(sidebar.max.x, area.max.y)), c.border);

    ui::SetCursorPos(sidebar.min + Vec2(Round(12.0f * s), Round(12.0f * s)));
    if (ui::BeginChild("##pb_sidebar", Vec2(SidebarWidth - 24.0f, area.Height() / s - 24.0f), false))
        DrawSidebar(ed);
    ui::EndChild();

    const f32 pad = Round(28.0f * s);
    ui::SetCursorPos(Vec2(sidebar.max.x + pad, area.min.y + Round(20.0f * s)));
    if (ui::BeginChild("##pb_main", Vec2((area.max.x - sidebar.max.x - pad * 2.0f) / s, (area.Height() - Round(32.0f * s)) / s), false)) {
        if (ed.browser.newProjectPage)
            DrawNewProjectPage(ed);
        else
            DrawRecentPage(ed);
    }
    ui::EndChild();
    ui::End();
}

} // namespace zx::editor
