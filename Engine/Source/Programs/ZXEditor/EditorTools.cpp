// Brush, Measure and Drop to Ground tools, and the Features window.

#include "EditorTools.h"
#include "Editor.h"
#include "Engine/Gameplay.h"
#include "Engine/Raycast.h"
#include "Engine/SceneSerialization.h"
#include "Render/DebugDraw.h"

namespace zx::editor {

namespace {

const FeatureInfo Features[] = {
    {"shadows", "Shadows", "Rendering", "Sun cascaded shadows and point / spot light shadows"},
    {"ambientOcclusion", "Ambient Occlusion", "Rendering", "Ground-truth ambient occlusion (contact darkening)"},
    {"sky", "Sky & Fog", "Rendering", "Physical sky, sun disk and height fog"},
    {"bloom", "Bloom", "Rendering", "Glow around bright pixels"},
    {"temporalAA", "Temporal Anti-Aliasing", "Rendering", "TAA (FXAA is used when off)"},
    {"water", "Water", "Rendering", "Water surfaces (Water component)"},
    {"decals", "Decals", "Rendering", "Decal components (images projected onto surfaces)"},
    {"reflections", "Reflections", "Rendering", "Sky reflections and Reflection Probe components"},
    {"screenSpaceReflections", "Screen Space Reflections", "Rendering", "Sharp reflections of what is on screen (Full profile)"},
    {"postProcessVolumes", "Post Process Volumes", "Rendering", "Post Process Volume components change the look per area"},
    {"depthOfField", "Depth of Field", "Rendering", "Blur by distance from the focus (set in Post Process Volumes)"},
    {"motionBlur", "Motion Blur", "Rendering", "Camera motion blur while playing (set in Post Process Volumes)"},
    {"scripts", "ZeqouScript", "Gameplay", "Run Script components while playing"},
    {"characters", "Character Physics", "Gameplay", "Move Character Controllers while playing"},
    {"brush", "Brush Tool", "Editor Tools", "Paint models onto surfaces (B)"},
    {"measure", "Measure Tool", "Editor Tools", "Measure distances in the viewport (M)"},
};

/// The painted-instance root an entity belongs to (itself or an ancestor), or null.
ecs::Entity PaintedRoot(ecs::World& world, ecs::Entity e)
{
    for (ecs::Entity p = e; p; p = scene::GetParent(world, p))
        if (world.Has<PaintedInstance>(p))
            return p;
    return ecs::NullEntity;
}

/// World bounds of an entity hierarchy's meshes (invalid if it has none loaded).
AABB HierarchyBounds(Editor& ed, ecs::Entity root)
{
    Array<ecs::Entity> all;
    CollectHierarchy(ed.world, root, all);
    AABB box;
    for (ecs::Entity e : all) {
        const MeshRenderer* mr = ed.world.Get<const MeshRenderer>(e);
        const scene::WorldTransform* wt = ed.world.Get<const scene::WorldTransform>(e);
        const render::GpuMesh* mesh = mr && wt ? ed.renderAssets->GetMesh(mr->mesh) : nullptr;
        if (!mesh)
            continue;
        const AABB b = mesh->bounds.Transformed(wt->matrix);
        box.min = Min(box.min, b.min);
        box.max = Max(box.max, b.max);
    }
    return box;
}

bool RaycastScene(Editor& ed, const Ray& ray, RaycastHit& hit, f32 maxDistance, const RaycastFilter* filter = nullptr)
{
    return RaycastMeshes(ed.world, *ed.renderAssets, ray, hit, maxDistance, filter);
}

/// The group entity painted instances are parented to (created on first use, as part of the current undo step).
ecs::Entity PaintGroup(Editor& ed)
{
    ecs::Entity group;
    ed.world.Query<const scene::EntityName>().Each([&](ecs::Entity e, const scene::EntityName& n) {
        if (!group && n.value == "Brush Painted" && !scene::GetParent(ed.world, e))
            group = e;
    });
    if (!group) {
        group = ed.CreateEntity("Brush Painted", Transform::Identity());
        EnsureEntityGuid(ed.world, group);
        ed.undo.RecordCreated(group);
    }
    return group;
}

/// Creates one instance of `item` at `pos` (world) under the paint group.
ecs::Entity PlaceItem(Editor& ed, u32 itemIndex, Vec3 pos, Quat rotation, f32 scale)
{
    const BrushItem& item = ed.tools.items[itemIndex];
    if (item.entities.Empty())
        return ecs::NullEntity;
    HashMap<Guid, Guid> remap;
    for (const JsonValue& s : item.entities)
        if (const Optional<Guid> g = Guid::Parse(s.Get("guid").AsString()))
            remap.InsertOrAssign(*g, Guid::New());
    Array<EntitySnapshot> snapshots;
    Guid rootGuid;
    for (usize i = 0; i < item.entities.Size(); ++i) {
        JsonValue s = item.entities[i];
        const Optional<Guid> old = Guid::Parse(s.Get("guid").AsString());
        if (!old)
            continue;
        const Guid guid = *remap.Find(*old);
        s.Set("guid", String::Format("{}", guid));
        if (i == 0) {
            rootGuid = guid;
            s.Remove("parent");
        } else if (const JsonValue* p = s.Find("parent")) {
            if (const Optional<Guid> pg = Guid::Parse(p->AsString()); pg && remap.Find(*pg))
                s.Set("parent", String::Format("{}", *remap.Find(*pg)));
        }
        snapshots.PushBack(EntitySnapshot{guid, move(s)});
    }
    const Array<ecs::Entity> created = ApplyEntitySnapshots(ed.world, snapshots.View());
    const ecs::Entity root = FindEntityByGuid(ed.world, rootGuid);
    if (!root)
        return ecs::NullEntity;
    Transform* t = ed.world.Get<Transform>(root);
    if (!t)
        t = ed.world.Add(root, Transform::Identity());
    t->position = pos;
    t->rotation = rotation * t->rotation;
    t->scale = t->scale * scale;
    ed.world.Set(root, PaintedInstance{itemIndex});
    scene::SetParent(ed.world, root, PaintGroup(ed), false);
    for (ecs::Entity e : created)
        ed.undo.RecordCreated(e);
    return root;
}

/// One dab of the brush at the hover point: scatter instances in the disc, on surfaces below the disc.
void PaintDab(Editor& ed)
{
    ToolState& ts = ed.tools;
    const BrushSettings& b = ts.brush;
    f32 totalWeight = 0.0f;
    for (const BrushItem& it : ts.items)
        totalWeight += it.enabled && !it.entities.Empty() ? Max(it.weight, 0.0f) : 0.0f;
    if (totalWeight <= 0.0f)
        return;
    scene::UpdateWorldTransforms(ed.world); // instances restored by undo/redo have no world matrix yet
    // Existing instances (spacing check).
    Array<Vec3> existing;
    ed.world.Query<const PaintedInstance, const scene::WorldTransform>().Each(
        [&](const PaintedInstance&, const scene::WorldTransform& wt) { existing.PushBack(wt.GetPosition()); });
    const RaycastFilter notPainted = [&ed](ecs::Entity e) { return !PaintedRoot(ed.world, e); };
    Vec3 tangent, bitangent;
    OrthonormalBasis(ts.hoverNormal, tangent, bitangent);
    const u32 attempts = Max(1u, u32(Round(b.density * Pi * b.radius * b.radius / 10.0f)));
    for (u32 i = 0; i < attempts; ++i) {
        const f32 angle = ts.rng.NextFloat() * TwoPi;
        const f32 r = Sqrt(ts.rng.NextFloat()) * b.radius;
        const Vec3 sample = ts.hoverPoint + tangent * (Cos(angle) * r) + bitangent * (Sin(angle) * r);
        RaycastHit hit;
        const Ray down(sample + ts.hoverNormal * (b.radius + 1.0f), -ts.hoverNormal);
        if (!RaycastScene(ed, down, hit, b.radius * 2.0f + 2.0f, &notPainted))
            continue;
        if (RadToDeg(Acos(Clamp(hit.normal.y, -1.0f, 1.0f))) > b.maxSlope)
            continue;
        bool tooClose = false;
        for (const Vec3& p : existing)
            tooClose |= DistanceSq(p, hit.point) < b.spacing * b.spacing;
        if (tooClose)
            continue;
        // Pick an item by weight.
        f32 pick = ts.rng.NextFloat() * totalWeight;
        u32 item = 0;
        for (u32 k = 0; k < ts.items.Size(); ++k) {
            const BrushItem& it = ts.items[k];
            if (!it.enabled || it.entities.Empty())
                continue;
            item = k;
            pick -= Max(it.weight, 0.0f);
            if (pick <= 0.0f)
                break;
        }
        const Vec3 up = SafeNormalize(Lerp(Vec3::Up(), hit.normal, Clamp(b.alignToNormal, 0.0f, 1.0f)), Vec3::Up());
        Quat rot = Quat::FromTo(Vec3::Up(), up);
        if (b.maxTilt > 0.0f)
            rot = rot * Quat::FromEulerDegrees(Vec3(ts.rng.Range(-b.maxTilt, b.maxTilt), 0.0f, ts.rng.Range(-b.maxTilt, b.maxTilt)));
        if (b.randomYaw)
            rot = rot * Quat::FromAxisAngle(Vec3::Up(), ts.rng.NextFloat() * TwoPi);
        const f32 scale = ts.rng.Range(Min(b.scaleMin, b.scaleMax), Max(b.scaleMin, b.scaleMax));
        if (PlaceItem(ed, item, hit.point + hit.normal * b.offset, rot, scale)) {
            existing.PushBack(hit.point);
            ++ts.placedThisStroke;
        }
    }
}

/// Shift+drag: removes painted instances inside the brush.
void EraseDab(Editor& ed)
{
    scene::UpdateWorldTransforms(ed.world); // instances restored by undo/redo have no world matrix yet
    const ToolState& ts = ed.tools;
    Array<ecs::Entity> victims;
    ed.world.Query<const PaintedInstance, const scene::WorldTransform>().Each(
        [&](ecs::Entity e, const PaintedInstance&, const scene::WorldTransform& wt) {
            if (DistanceSq(wt.GetPosition(), ts.hoverPoint) <= ts.brush.radius * ts.brush.radius)
                victims.PushBack(e);
        });
    for (ecs::Entity e : victims) {
        if (!ed.world.IsAlive(e))
            continue;
        ed.undo.RecordHierarchy(e);
        scene::DestroyHierarchy(ed.world, e);
    }
}

String FormatMeters(f32 m) { return Abs(m) >= 1000.0f ? String::Format("{:.2f} km", m / 1000.0f) : String::Format("{:.2f} m", m); }

} // namespace

u32 BrushStrokeAt(Editor& ed, Vec3 point, Vec3 normal, bool erase)
{
    ToolState& ts = ed.tools;
    ts.hovering = true;
    ts.hoverPoint = point;
    ts.hoverNormal = SafeNormalize(normal, Vec3::Up());
    ed.undo.Begin(erase ? "Brush Erase" : "Brush Paint");
    if (erase)
        EraseDab(ed);
    else
        PaintDab(ed);
    ed.undo.End();
    u32 count = 0;
    ed.world.Query<const PaintedInstance>().Each([&](const PaintedInstance&) { ++count; });
    return count;
}

bool AddSelectedBrushItem(Editor& ed)
{
    if (!ed.world.IsAlive(ed.selected) || PaintedRoot(ed.world, ed.selected))
        return false;
    BrushItem item;
    item.name = ed.EntityLabel(ed.selected);
    Array<ecs::Entity> all;
    CollectHierarchy(ed.world, ed.selected, all);
    for (ecs::Entity e : all)
        EnsureEntityGuid(ed.world, e);
    for (ecs::Entity e : all) {
        JsonValue s = SerializeEntity(ed.world, e);
        s.Set("guid", String::Format("{}", GetEntityGuid(ed.world, e)));
        item.entities.PushBack(move(s));
    }
    ed.tools.items.PushBack(move(item));
    return true;
}

Span<const FeatureInfo> GetFeatures() { return Span<const FeatureInfo>(Features, ZX_ARRAY_COUNT(Features)); }

void SetActiveTool(Editor& ed, EditorTool tool)
{
    ToolState& ts = ed.tools;
    if ((tool == EditorTool::Brush && !ed.FeatureEnabled("brush")) || (tool == EditorTool::Measure && !ed.FeatureEnabled("measure")))
        tool = EditorTool::None;
    ts.active = ts.active == tool ? EditorTool::None : tool;
    ts.painting = false;
    ts.measureCount = 0;
    if (ts.active != EditorTool::None)
        ui::FocusDockedWindow(ToolTitle);
}

bool UpdateViewportTool(Editor& ed, const Ray& mouseRay, bool hovered, const Function<bool(Vec3, Vec2&), 32>& project, ui::DrawList& dl,
                        f32 scale)
{
    ToolState& ts = ed.tools;
    if (ts.active == EditorTool::None || ed.IsPlaying())
        return false;
    // Where the mouse points: scene triangles, else the ground plane.
    ts.hovering = false;
    if (hovered) {
        RaycastHit hit;
        const RaycastFilter notPainted = [&ed](ecs::Entity e) { return !PaintedRoot(ed.world, e); };
        if (RaycastScene(ed, mouseRay, hit, 5000.0f, ts.active == EditorTool::Brush ? &notPainted : nullptr)) {
            ts.hovering = true;
            ts.hoverPoint = hit.point;
            ts.hoverNormal = hit.normal;
        } else if (f32 t; RayIntersectsPlane(mouseRay, Plane::FromPointNormal(Vec3::Zero(), Vec3::Up()), t) && t > 0.0f && t < 5000.0f) {
            ts.hovering = true;
            ts.hoverPoint = mouseRay.At(t);
            ts.hoverNormal = Vec3::Up();
        }
    }
    const bool pressed = hovered && ed.input.WasMousePressed(MouseButton::Left);
    const bool held = ed.input.IsMouseDown(MouseButton::Left);
    const bool shift = HasAnyFlags(ed.input.Modifiers(), KeyModifiers::Shift);

    if (ts.active == EditorTool::Brush) {
        if (pressed && ts.hovering) {
            ts.painting = true;
            ts.placedThisStroke = 0;
            ed.undo.Begin(shift ? "Brush Erase" : "Brush Paint");
            if (shift)
                EraseDab(ed);
            else
                PaintDab(ed);
            ts.lastDab = ts.hoverPoint;
        } else if (ts.painting && held && ts.hovering &&
                   Distance(ts.hoverPoint, ts.lastDab) >= Max(ts.brush.radius * 0.5f, ts.brush.spacing)) {
            if (shift)
                EraseDab(ed);
            else
                PaintDab(ed);
            ts.lastDab = ts.hoverPoint;
        }
        if (ts.painting && !held) {
            ts.painting = false;
            ed.undo.End();
        }
        // Hint when there is nothing to paint with.
        if (hovered && ts.items.Empty()) {
            const Rect r = ui::GetWindowContentRect();
            ui::Context& ctx = *ui::GetCurrentContext();
            dl.AddTextInRect(ctx.Fonts().FindFont("Inter"), 12.5f * scale, Rect(Vec2(r.min.x, r.min.y + 44.0f * scale), Vec2(r.max.x, r.min.y + 64.0f * scale)),
                             Color(240, 200, 120), "Brush: select a model and press 'Add Selected' in Tool Settings", ui::TextAlign::Center, false);
        }
        return hovered || ts.painting;
    }

    // Measure: click A, click B, click again to restart. The label sits at the middle of the segment.
    if (pressed && ts.hovering) {
        if (ts.measureCount != 1) {
            ts.measureA = ts.hoverPoint;
            ts.measureCount = 1;
        } else {
            ts.measureB = ts.hoverPoint;
            ts.measureCount = 2;
        }
    }
    const bool preview = ts.measureCount == 1 && ts.hovering;
    if (ts.measureCount == 2 || preview) {
        const Vec3 a = ts.measureA, b = ts.measureCount == 2 ? ts.measureB : ts.hoverPoint;
        Vec2 sp;
        if (project((a + b) * 0.5f, sp)) {
            const Vec3 d = b - a;
            const String text = String::Format("{}   (x {:.2f}, y {:.2f}, z {:.2f})", FormatMeters(Length(d)), d.x, d.y, d.z);
            ui::Context& ctx = *ui::GetCurrentContext();
            const ui::FontHandle font = ctx.Fonts().FindFont("Inter SemiBold");
            const f32 w = ctx.Fonts().MeasureWidth(font, 12.5f * scale, text) + 16.0f * scale;
            const Rect box(sp - Vec2(w * 0.5f, 12.0f * scale), sp + Vec2(w * 0.5f, 12.0f * scale));
            dl.AddRect(box, Color(20, 20, 20, 220), ui::CornerRadii(6.0f * scale), 1.0f, Color(255, 210, 90));
            dl.AddTextInRect(font, 12.5f * scale, box, Color(255, 225, 140), text, ui::TextAlign::Center, false);
        }
    }
    return hovered;
}

void DrawToolOverlay(Editor& ed, render::DebugDraw& dd)
{
    const ToolState& ts = ed.tools;
    if (ts.active == EditorTool::Brush && ts.hovering) {
        const bool erase = HasAnyFlags(ed.input.Modifiers(), KeyModifiers::Shift);
        const Color c = erase ? Color(240, 90, 90) : Color(90, 200, 255);
        dd.Circle(ts.hoverPoint + ts.hoverNormal * 0.02f, ts.hoverNormal, ts.brush.radius, c, 48, false);
        dd.Circle(ts.hoverPoint + ts.hoverNormal * 0.02f, ts.hoverNormal, ts.brush.radius * 0.1f, c, 16, false);
        dd.Line(ts.hoverPoint, ts.hoverPoint + ts.hoverNormal * Max(0.5f, ts.brush.radius * 0.3f), c, false);
    }
    if (ts.active == EditorTool::Measure) {
        const Color c(255, 210, 90);
        if (ts.measureCount >= 1)
            dd.Sphere(ts.measureA, 0.06f, c, 12, false);
        if (ts.measureCount == 2) {
            dd.Sphere(ts.measureB, 0.06f, c, 12, false);
            dd.Line(ts.measureA, ts.measureB, c, false);
            // Axis-aligned legs (x, then z, then y).
            const Vec3 a = ts.measureA, b = ts.measureB;
            dd.Line(a, Vec3(b.x, a.y, a.z), Color(200, 80, 80), false);
            dd.Line(Vec3(b.x, a.y, a.z), Vec3(b.x, a.y, b.z), Color(80, 120, 220), false);
            dd.Line(Vec3(b.x, a.y, b.z), b, Color(90, 190, 90), false);
        } else if (ts.measureCount == 1 && ts.hovering) {
            dd.Line(ts.measureA, ts.hoverPoint, Color(255, 210, 90, 160), false);
        }
        if (ts.hovering)
            dd.Circle(ts.hoverPoint + ts.hoverNormal * 0.01f, ts.hoverNormal, 0.08f, c, 16, false);
    }
}

void DrawToolPanel(Editor& ed)
{
    ToolState& ts = ed.tools;
    if (ts.active == EditorTool::None)
        return;
    bool open = true;
    ui::SetNextWindowSize(Vec2(320.0f, 520.0f), ui::Cond::FirstUse);
    if (!ui::Begin(ToolTitle, &open)) {
        ui::End();
        if (!open)
            ts.active = EditorTool::None;
        return;
    }
    if (!open)
        ts.active = EditorTool::None;
    if (ts.active == EditorTool::Measure) {
        ui::Heading(ZX_ICON_RULER "  Measure");
        ui::TextWrapped("Click two points in the viewport. Click again to start a new measurement.");
        ui::Spacing();
        if (ts.measureCount == 2) {
            const Vec3 d = ts.measureB - ts.measureA;
            ui::LabelText("Distance", FormatMeters(Length(d)));
            ui::LabelText("Horizontal", FormatMeters(Length(Vec3(d.x, 0.0f, d.z))));
            ui::LabelText("Height", FormatMeters(d.y));
            ui::LabelText("X / Y / Z", String::Format("{:.3f} / {:.3f} / {:.3f}", d.x, d.y, d.z));
            if (ui::Button(ZX_ICON_X "  Clear"))
                ts.measureCount = 0;
        } else {
            ui::TextDisabled(ts.measureCount == 1 ? "Pick the second point" : "Pick the first point");
        }
        ui::End();
        return;
    }

    // --- Brush ---
    BrushSettings& b = ts.brush;
    ui::Heading(ZX_ICON_PAINTBRUSH "  Brush");
    ui::PushTextColor(ui::GetCurrentContext()->GetStyle().colors.textDisabled);
    ui::TextWrapped("Drag to paint, Shift+drag to erase, [ and ] change the radius. Painted models go under 'Brush Painted'.");
    ui::PopTextColor();
    ui::Spacing();
    ui::TextDisabled("MODELS");
    const bool canAdd = ed.world.IsAlive(ed.selected) && !PaintedRoot(ed.world, ed.selected);
    ui::BeginDisabled(!canAdd);
    if (ui::Button(ZX_ICON_PLUS "  Add Selected", Vec2(-1.0f, 0.0f), ui::ButtonVariant::Primary))
        AddSelectedBrushItem(ed);
    ui::EndDisabled();
    ui::ItemTooltip(canAdd ? "Use the selected entity (and its children) as a brush model" : "Select a model in the scene first");
    i32 remove = -1;
    for (usize i = 0; i < ts.items.Size(); ++i) {
        BrushItem& it = ts.items[i];
        ui::PushID(i32(i));
        ui::Checkbox("##on", &it.enabled);
        ui::SameLine();
        ui::SetNextItemWidth(-(ui::GetFrameHeight() / ui::GetCurrentContext()->GetScale() * 2.0f + 70.0f));
        ui::AlignTextToFramePadding();
        ui::Text(it.name);
        ui::SameLine(Max(0.0f, ui::GetWindowContentRect().Width() / ui::GetCurrentContext()->GetScale() - 100.0f));
        ui::SetNextItemWidth(60.0f);
        ui::DragFloat("##w", &it.weight, 0.02f, 0.0f, 10.0f, "x{:.1f}");
        ui::ItemTooltip("Relative chance");
        ui::SameLine(0.0f, 4.0f);
        if (ui::IconButton(ZX_ICON_TRASH_2, "Remove"))
            remove = i32(i);
        ui::PopID();
    }
    if (remove >= 0)
        ts.items.RemoveAt(usize(remove));
    if (ts.items.Empty())
        ui::TextDisabled("No models yet");
    ui::Spacing();
    ui::Separator();
    ui::TextDisabled("OPTIONS");
    auto row = [](StringView label, StringView tooltip) {
        ui::AlignTextToFramePadding();
        ui::TextColored(ui::GetCurrentContext()->GetStyle().colors.textDim, label);
        if (!tooltip.Empty())
            ui::ItemTooltip(tooltip);
        ui::SameLine(Round(ui::GetContentRegionAvail().x / ui::GetCurrentContext()->GetScale() * 0.42f));
        ui::SetNextItemWidth(-1.0f);
    };
    row("Radius", "Brush radius in meters ([ and ] keys)");
    ui::SliderFloat("##radius", &b.radius, 0.25f, 50.0f, "{:.2f} m");
    row("Density", "Instances per 10 square meters per dab");
    ui::SliderFloat("##density", &b.density, 0.05f, 20.0f, "{:.2f}");
    row("Spacing", "Minimum distance between instances");
    ui::SliderFloat("##spacing", &b.spacing, 0.0f, 10.0f, "{:.2f} m");
    row("Scale min", {});
    ui::SliderFloat("##smin", &b.scaleMin, 0.05f, 5.0f, "{:.2f}");
    row("Scale max", {});
    ui::SliderFloat("##smax", &b.scaleMax, 0.05f, 5.0f, "{:.2f}");
    row("Random yaw", "Random rotation around the up axis");
    ui::Checkbox("##yaw", &b.randomYaw);
    row("Random tilt", "Maximum random tilt");
    ui::SliderFloat("##tilt", &b.maxTilt, 0.0f, 45.0f, "{:.0f} deg");
    row("Align to surface", "0 = always upright, 1 = follow the surface normal");
    ui::SliderFloat("##align", &b.alignToNormal, 0.0f, 1.0f, "{:.2f}");
    row("Max slope", "Skip surfaces steeper than this");
    ui::SliderFloat("##slope", &b.maxSlope, 0.0f, 90.0f, "{:.0f} deg");
    row("Offset", "Move along the surface normal (negative sinks into the ground)");
    ui::SliderFloat("##offset", &b.offset, -2.0f, 2.0f, "{:.2f} m");
    ui::Spacing();
    u32 painted = 0;
    ed.world.Query<const PaintedInstance>().Each([&](const PaintedInstance&) { ++painted; });
    ui::TextDisabled(String::Format("{} painted instance{} in the scene", painted, painted == 1 ? "" : "s"));
    ui::End();
}

void DrawFeaturesPanel(Editor& ed)
{
    if (!ed.showFeatures)
        return;
    ui::SetNextWindowSize(Vec2(420.0f, 560.0f), ui::Cond::FirstUse);
    if (!ui::Begin(FeaturesTitle, &ed.showFeatures)) {
        ui::End();
        return;
    }
    ui::TextWrapped("Switch engine features and editor tools on or off for this project. Saved in the .zproj file.");
    ui::Spacing();
    StringView category;
    for (const FeatureInfo& f : Features) {
        if (f.category != category) {
            category = f.category;
            ui::Spacing();
            ui::PushFont(ui::FontStyle::SemiBold);
            ui::Text(category);
            ui::PopFont();
            ui::Separator();
        }
        bool on = ed.FeatureEnabled(f.id);
        ui::PushID(f.id);
        if (ui::Checkbox(f.name, &on))
            ed.SetFeatureEnabled(f.id, on);
        ui::PushTextColor(ui::GetCurrentContext()->GetStyle().colors.textDisabled);
        ui::TextWrapped(f.description);
        ui::PopTextColor();
        ui::PopID();
    }
    ui::End();
}

bool DropSelectedToGround(Editor& ed)
{
    const ecs::Entity e = ed.selected;
    if (!ed.world.IsAlive(e) || ed.IsPlaying())
        return false;
    scene::UpdateWorldTransforms(ed.world);
    Array<ecs::Entity> mine;
    CollectHierarchy(ed.world, e, mine);
    const AABB box = HierarchyBounds(ed, e);
    const Vec3 origin = scene::ComputeWorldMatrix(ed.world, e).GetTranslation();
    const Vec3 bottom = box.IsValid() ? Vec3(box.Center().x, box.min.y, box.Center().z) : origin;
    const RaycastFilter notMine = [&mine](ecs::Entity x) {
        for (ecs::Entity m : mine)
            if (m == x)
                return false;
        return true;
    };
    RaycastHit hit;
    // From a bit above the bottom (so it also works when slightly sunk in), downwards.
    if (!RaycastScene(ed, Ray(bottom + Vec3(0.0f, 0.5f, 0.0f), Vec3(0.0f, -1.0f, 0.0f)), hit, 10000.0f, &notMine)) {
        // Nothing below: rest on the ground plane.
        if (bottom.y <= 0.0f)
            return false;
        hit.point = Vec3(bottom.x, 0.0f, bottom.z);
    }
    const f32 dy = hit.point.y - bottom.y;
    if (Abs(dy) < 1e-5f)
        return false;
    ed.undo.Begin("Drop to Ground");
    ed.undo.Record(e);
    Transform* t = ed.world.Get<Transform>(e);
    const ecs::Entity parent = scene::GetParent(ed.world, e);
    const Vec3 delta = parent ? Inverse(scene::ComputeWorldMatrix(ed.world, parent)).TransformVector(Vec3(0.0f, dy, 0.0f)) : Vec3(0.0f, dy, 0.0f);
    if (t)
        t->position += delta;
    ed.undo.End();
    return true;
}

} // namespace zx::editor
