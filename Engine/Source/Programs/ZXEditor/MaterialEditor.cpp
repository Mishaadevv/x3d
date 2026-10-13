#include "MaterialEditor.h"
#include "Editor.h"
#include "Thumbnails.h"
#include "Core/String/Path.h"
#include "Engine/SceneRendering.h"

ZX_DEFINE_LOG_CATEGORY_STATIC(LogMaterialEditor, Info);

namespace zx::editor {

using namespace render;

namespace {

// Node layout in graph units (1 unit = 1 logical pixel at zoom 1).
constexpr f32 NodeWidth = 176.0f;
constexpr f32 HeaderHeight = 26.0f;
constexpr f32 RowHeight = 21.0f;
constexpr f32 PinRadius = 5.0f;
constexpr f32 PreviewHeight = 72.0f; // texture / color swatch area
constexpr usize MaxUndo = 100;

Color CategoryColor(MaterialNodeKind kind)
{
    switch (kind) {
    case MaterialNodeKind::Output: return Color(0xA8, 0x3C, 0x3C);
    case MaterialNodeKind::Value: return Color(0x3C, 0x86, 0x4C);
    case MaterialNodeKind::Texture: return Color(0x7E, 0x4C, 0xB0);
    case MaterialNodeKind::Input: return Color(0x2F, 0x6A, 0xB0);
    case MaterialNodeKind::Math: return Color(0x4A, 0x58, 0x68);
    case MaterialNodeKind::Utility: return Color(0xAE, 0x7A, 0x2A);
    }
    return Color(0x55, 0x55, 0x55);
}

Color PinColor(u32 dims)
{
    switch (dims) {
    case 1: return Color(0x9A, 0xD0, 0x6A);
    case 2: return Color(0x5A, 0xC8, 0xC8);
    case 3: return Color(0xE8, 0xC8, 0x4A);
    case 4: return Color(0xE0, 0x78, 0xC8);
    default: return Color(0xC8, 0xC8, 0xC8);
    }
}

bool HasPreview(const MaterialGraphNode& n) { return n.type == "TextureSample" || n.type == "Color"; }

f32 NodeHeight(const MaterialGraphNode& n, const MaterialNodeType* t)
{
    const usize rows = t ? Max(t->inputs.Size(), t->outputs.Size()) : 1;
    return HeaderHeight + f32(Max<usize>(rows, 1)) * RowHeight + (HasPreview(n) ? PreviewHeight : 0.0f) + 6.0f;
}

struct Canvas
{
    Rect rect;
    Vec2 pan;
    f32 scale = 1.0f; // zoom * DPI scale
    Vec2 ToScreen(Vec2 p) const { return rect.min + (p + pan) * scale; }
    Vec2 ToGraph(Vec2 q) const { return (q - rect.min) / scale - pan; }
};

Vec2 PinPos(const Canvas& c, const MaterialGraphNode& n, u32 pin, bool output)
{
    const Vec2 p(output ? n.position.x + NodeWidth : n.position.x, n.position.y + HeaderHeight + (f32(pin) + 0.5f) * RowHeight);
    return c.ToScreen(p);
}

/// Output width of a node pin for coloring (dynamic pins report 0).
u32 PinDims(const MaterialNodeType* t, u32 pin, bool output)
{
    if (!t)
        return 0;
    const Span<const MaterialPinDesc> pins = output ? t->outputs : t->inputs;
    return pin < pins.Size() ? pins[pin].dims : 0;
}

String Snapshot(const MaterialGraph& g) { return ToJsonString(g.ToJson(), JsonWriteOptions{false}); }

void Restore(MaterialGraph& g, StringView json)
{
    JsonValue doc;
    if (ParseJson(json, doc))
        g.FromJson(doc);
}

MaterialLibrary::Material* Current(Editor& ed)
{
    return ed.materials && ed.materialEditor.open ? ed.materials->Find(ed.materialEditor.material) : nullptr;
}

/// Remembers the graph state for undo (call before a change).
void PushUndo(MaterialEditorState& st, const MaterialGraph& g)
{
    st.undo.PushBack(Snapshot(g));
    if (st.undo.Size() > MaxUndo)
        st.undo.RemoveAt(0);
    st.redo.Clear();
}

/// After a change: mark dirty and push it to the renderer (values immediately, structure after compiling).
void SaveMaterial(Editor& ed, MaterialLibrary::Material& m)
{
    if (ed.materials->Save(m.guid) && ed.thumbnails)
        ed.thumbnails->Invalidate(m.path);
}

void Commit(Editor& ed, MaterialLibrary::Material& m)
{
    m.dirty = true;
    ed.materials->Apply(m.guid);
}

// --- Drawing -------------------------------------------------------------------------------------------------------------

void DrawGrid(DrawList& dl, const Canvas& c, f32 s)
{
    dl.AddRect(c.rect, Color(0x1C, 0x1C, 0x1E));
    const f32 minor = 16.0f * c.scale, major = minor * 8.0f;
    if (minor >= 6.0f * s) {
        const Vec2 origin = c.ToScreen(Vec2::Zero());
        for (f32 x = Fmod(origin.x - c.rect.min.x, minor) + c.rect.min.x; x < c.rect.max.x; x += minor)
            dl.AddLine(Vec2(x, c.rect.min.y), Vec2(x, c.rect.max.y), Color(0x26, 0x26, 0x29), 1.0f);
        for (f32 y = Fmod(origin.y - c.rect.min.y, minor) + c.rect.min.y; y < c.rect.max.y; y += minor)
            dl.AddLine(Vec2(c.rect.min.x, y), Vec2(c.rect.max.x, y), Color(0x26, 0x26, 0x29), 1.0f);
    }
    const Vec2 origin = c.ToScreen(Vec2::Zero());
    for (f32 x = Fmod(origin.x - c.rect.min.x, major) + c.rect.min.x; x < c.rect.max.x; x += major)
        dl.AddLine(Vec2(x, c.rect.min.y), Vec2(x, c.rect.max.y), Color(0x30, 0x30, 0x34), 1.0f);
    for (f32 y = Fmod(origin.y - c.rect.min.y, major) + c.rect.min.y; y < c.rect.max.y; y += major)
        dl.AddLine(Vec2(c.rect.min.x, y), Vec2(c.rect.max.x, y), Color(0x30, 0x30, 0x34), 1.0f);
}

void DrawLink(DrawList& dl, Vec2 from, Vec2 to, Color color, f32 scale)
{
    const f32 dx = Max(Abs(to.x - from.x) * 0.5f, 40.0f * scale);
    dl.AddBezier(from, from + Vec2(dx, 0.0f), to - Vec2(dx, 0.0f), to, color, Max(2.0f * scale, 1.5f));
}

void DrawNode(Editor& ed, DrawList& dl, const Canvas& c, const MaterialGraphNode& n, bool selected, bool error)
{
    ui::Context& ctx = *ui::GetCurrentContext();
    const ui::Theme& th = ctx.GetStyle().colors;
    const MaterialNodeType* t = FindMaterialNodeType(n.type);
    const Rect r(c.ToScreen(n.position), c.ToScreen(n.position + Vec2(NodeWidth, NodeHeight(n, t))));
    if (!r.Intersects(c.rect))
        return;
    const f32 k = c.scale;
    const ui::CornerRadii radii(6.0f * k);
    dl.AddShadow(r, Color(0, 0, 0, 120), 6.0f * k, 10.0f * k, Vec2(0.0f, 3.0f * k));
    dl.AddRect(r, Color(0x2A, 0x2A, 0x2E, 245), radii, error ? 2.0f * k : 1.0f, error ? th.error : selected ? th.accentHovered : Color(0x10, 0x10, 0x12));
    if (selected && !error)
        dl.AddRect(r.Expanded(1.5f * k), Color(0, 0, 0, 0), ui::CornerRadii(7.5f * k), 1.5f * k, th.accentHovered);
    const Rect header(r.min, Vec2(r.max.x, r.min.y + HeaderHeight * k));
    const Color hc = CategoryColor(t ? t->kind : MaterialNodeKind::Math);
    dl.AddRectGradient(header.Expanded(-1.0f), hc, WithAlpha(hc, 0.55f), ui::GradientDirection::Horizontal, ui::CornerRadii::Top(5.0f * k));
    const bool showText = k >= 0.45f;
    const ui::FontHandle semi = ctx.Fonts().FindFont("Inter SemiBold");
    const ui::FontHandle font = ctx.Fonts().FindFont("Inter");
    if (showText) {
        String title(t ? t->displayName : StringView(n.type));
        if (!n.name.Empty())
            title = String::Format("{}  \xE2\x80\xA2  {}", n.name, title);
        dl.AddTextInRect(semi, 12.0f * k, Rect(header.min + Vec2(8.0f * k, 0.0f), header.max - Vec2(6.0f * k, 0.0f)), Color(255, 255, 255), title);
    }
    if (!t)
        return;
    MaterialGraph* g = ed.materials ? &ed.materials->Find(ed.materialEditor.material)->graph : nullptr;
    for (u32 i = 0; i < t->inputs.Size(); ++i) {
        const Vec2 p = PinPos(c, n, i, false);
        const bool linked = g && g->FindInputLink(n.id, i);
        const Color pc = PinColor(t->inputs[i].dims);
        dl.AddCircle(p, PinRadius * k, linked ? pc : Color(0x2A, 0x2A, 0x2E), 1.5f * k, pc);
        if (showText)
            dl.AddTextInRect(font, 11.0f * k, Rect(p + Vec2(9.0f * k, -RowHeight * 0.5f * k), Vec2(r.Center().x + 10.0f * k, p.y + RowHeight * 0.5f * k)),
                             th.text, t->inputs[i].name);
    }
    for (u32 o = 0; o < t->outputs.Size(); ++o) {
        const Vec2 p = PinPos(c, n, o, true);
        const Color pc = PinColor(t->outputs[o].dims);
        dl.AddCircle(p, PinRadius * k, pc, 1.5f * k, pc);
        if (showText)
            dl.AddTextInRect(font, 11.0f * k, Rect(Vec2(r.Center().x - 10.0f * k, p.y - RowHeight * 0.5f * k), Vec2(p.x - 9.0f * k, p.y + RowHeight * 0.5f * k)),
                             th.textDim, t->outputs[o].name, ui::TextAlign::Right);
    }
    if (HasPreview(n)) {
        const f32 top = r.max.y - (PreviewHeight + 6.0f) * k;
        const Rect pr(Vec2(r.min.x + 8.0f * k, top), Vec2(r.max.x - 8.0f * k, r.max.y - 8.0f * k));
        if (n.type == "Color") {
            const Vec4 v = n.value;
            auto ch = [](f32 x) { return u8(Clamp(Pow(Saturate(x), 1.0f / 2.2f) * 255.0f + 0.5f, 0.0f, 255.0f)); };
            dl.AddRect(pr, Color(ch(v.x), ch(v.y), ch(v.z)), ui::CornerRadii(4.0f * k), 1.0f, Color(0x10, 0x10, 0x12));
        } else {
            const u32 srv = n.texture.Empty() || !ed.thumbnails ? rhi::InvalidDescriptor : ed.thumbnails->Get(Path::Join(ed.contentRoot, n.texture));
            const f32 side = pr.Height();
            const Rect square(Vec2(pr.Center().x - side * 0.5f, pr.min.y), Vec2(pr.Center().x + side * 0.5f, pr.max.y));
            if (srv != rhi::InvalidDescriptor)
                dl.AddImage(srv, square, Vec2(0, 0), Vec2(1, 1), Color(255, 255, 255), 4.0f * k);
            else
                dl.AddRect(square, Color(0x1A, 0x1A, 0x1C), ui::CornerRadii(4.0f * k), 1.0f, th.border);
            if (showText && n.texture.Empty())
                dl.AddTextInRect(font, 10.5f * k, square, th.textDisabled, "No texture", ui::TextAlign::Center);
        }
    }
}

// --- Editing helpers --------------------------------------------------------------------------------------------------------

void DeleteSelection(Editor& ed, MaterialEditorState& st, MaterialLibrary::Material& m)
{
    bool any = false;
    for (u32 id : st.selection)
        any |= m.graph.FindNode(id) && m.graph.FindNode(id)->type != "Output";
    if (!any)
        return;
    PushUndo(st, m.graph);
    for (u32 id : st.selection)
        if (const MaterialGraphNode* n = m.graph.FindNode(id); n && n->type != "Output")
            m.graph.RemoveNode(id);
    st.selection.Clear();
    Commit(ed, m);
}

String CopySelection(const MaterialEditorState& st, const MaterialGraph& g)
{
    MaterialGraph part;
    for (u32 id : st.selection)
        if (const MaterialGraphNode* n = g.FindNode(id); n && n->type != "Output")
            part.nodes.PushBack(*n);
    for (const MaterialGraphLink& l : g.links)
        if (part.FindNode(l.fromNode) && part.FindNode(l.toNode))
            part.links.PushBack(l);
    return part.nodes.Empty() ? String() : Snapshot(part);
}

void Paste(Editor& ed, MaterialEditorState& st, MaterialLibrary::Material& m, StringView json, Vec2 at)
{
    MaterialGraph part;
    Restore(part, json);
    if (part.nodes.Empty())
        return;
    PushUndo(st, m.graph);
    Vec2 minPos(1e30f);
    for (const MaterialGraphNode& n : part.nodes)
        minPos = Min(minPos, n.position);
    HashMap<u32, u32> remap;
    st.selection.Clear();
    for (const MaterialGraphNode& n : part.nodes) {
        MaterialGraphNode copy = n;
        copy.id = m.graph.nextId++;
        copy.position = at + (n.position - minPos);
        remap.InsertOrAssign(n.id, copy.id);
        st.selection.PushBack(copy.id);
        m.graph.nodes.PushBack(move(copy));
    }
    for (const MaterialGraphLink& l : part.links)
        m.graph.Connect(*remap.Find(l.fromNode), l.fromPin, *remap.Find(l.toNode), l.toPin);
    Commit(ed, m);
}

void FrameAll(MaterialEditorState& st, const MaterialGraph& g, const Rect& rect, f32 dpi)
{
    if (g.nodes.Empty())
        return;
    Vec2 lo(1e30f), hi(-1e30f);
    for (const MaterialGraphNode& n : g.nodes) {
        lo = Min(lo, n.position);
        hi = Max(hi, n.position + Vec2(NodeWidth, NodeHeight(n, FindMaterialNodeType(n.type))));
    }
    const Vec2 size = hi - lo + Vec2(80.0f);
    const Vec2 view = rect.Size() / dpi;
    st.zoom = Clamp(Min(view.x / size.x, view.y / size.y), 0.3f, 1.25f);
    st.pan = view / (2.0f * st.zoom) - (lo + hi) * 0.5f;
}

/// The "add node" popup (searchable list grouped by category). Returns true if a node was added.
void AddNodePopup(Editor& ed, MaterialEditorState& st, MaterialLibrary::Material& m)
{
    if (!ui::BeginPopup("##mat_add_node"))
        return;
    ui::SetNextItemWidth(240.0f);
    ui::InputText("##mat_add_search", st.addSearch, ui::InputTextFlags::None, ZX_ICON_SEARCH "  Search nodes...");
    ui::Separator();
    StringView category;
    for (const MaterialNodeType& t : GetMaterialNodeTypes()) {
        if (t.kind == MaterialNodeKind::Output)
            continue;
        if (!st.addSearch.Empty()) {
            bool match = false;
            for (usize i = 0; i + st.addSearch.Size() <= t.displayName.Size() && !match; ++i)
                match = t.displayName.SubStr(i, st.addSearch.Size()).EqualsIgnoreCase(st.addSearch);
            if (!match)
                continue;
        }
        if (t.category != category) {
            category = t.category;
            ui::TextDisabled(category);
        }
        if (ui::MenuItem(String::Format("   {}##{}", t.displayName, t.type))) {
            PushUndo(st, m.graph);
            const u32 id = m.graph.AddNode(t.type, st.addPosition)->id;
            if (st.addHasLink) {
                // Connect the dragged link to the first suitable pin of the new node.
                if (st.addLinkFromOutput && !t.inputs.Empty())
                    m.graph.Connect(st.addLinkNode, st.addLinkPin, id, 0);
                else if (!st.addLinkFromOutput && !t.outputs.Empty())
                    m.graph.Connect(id, 0, st.addLinkNode, st.addLinkPin);
            }
            st.selection.Clear();
            st.selection.PushBack(id);
            Commit(ed, m);
            ui::CloseCurrentPopup();
        }
        if (ui::IsItemHovered())
            ui::SetTooltip(t.description);
    }
    ui::EndPopup();
}

// --- Properties ----------------------------------------------------------------------------------------------------------

void PropertiesPanel(Editor& ed, MaterialEditorState& st, MaterialLibrary::Material& m)
{
    if (st.selection.Size() != 1) {
        ui::TextDisabled(st.selection.Empty() ? "Select a node to edit it" : String::Format("{} nodes selected", st.selection.Size()));
        return;
    }
    MaterialGraphNode* n = m.graph.FindNode(st.selection[0]);
    const MaterialNodeType* t = n ? FindMaterialNodeType(n->type) : nullptr;
    if (!n || !t)
        return;
    ui::PushFont(ui::FontStyle::SemiBold);
    ui::Text(t->displayName);
    ui::PopFont();
    ui::TextDisabled(t->description);
    ui::Spacing();

    // Edits are grouped into one undo step per widget interaction.
    const String before = Snapshot(m.graph);
    bool changed = false;
    if (t->kind == MaterialNodeKind::Value || t->kind == MaterialNodeKind::Texture) {
        ui::SetNextItemWidth(-1.0f);
        changed |= ui::InputText("##param_name", n->name, ui::InputTextFlags::None, "Parameter name (optional)");
    }
    if (n->type == "Scalar") {
        ui::SetNextItemWidth(-1.0f);
        changed |= ui::DragFloat("##scalar", &n->value.x, 0.005f, 0.0f, 0.0f, "{:.3f}");
    } else if (n->type == "Color") {
        LinearColor col(n->value.x, n->value.y, n->value.z, n->value.w);
        if (ui::ColorPicker("##color", &col, true)) {
            n->value = Vec4(col.r, col.g, col.b, col.a);
            changed = true;
        }
    } else if (n->type == "TextureSample") {
        ui::TextDisabled("Texture (drop an image from the Content Browser)");
        ui::SetNextItemWidth(-1.0f);
        String tex = n->texture;
        if (ui::InputText("##texture", tex, ui::InputTextFlags::EnterReturnsTrue, "Path under Content")) {
            n->texture = tex;
            changed = true;
        }
        if (const void* payload = ui::AcceptDragDropPayload("asset")) {
            const StringView path(static_cast<const char*>(payload));
            const String rel = Path::MakeRelative(path, ed.contentRoot);
            n->texture = rel.Empty() ? String(path) : rel;
            changed = true;
        }
        bool normal = n->normalMap;
        if (ui::Checkbox("Normal map", &normal)) {
            n->normalMap = normal;
            changed = true;
        }
    } else if (n->type == "TexCoord") {
        Vec2 tiling(n->value.x, n->value.y);
        ui::TextDisabled("Tiling");
        ui::SetNextItemWidth(-1.0f);
        if (ui::DragFloat("##tile_u", &tiling.x, 0.01f, 0.0f, 0.0f, "U {:.2f}") | ui::DragFloat("##tile_v", &tiling.y, 0.01f, 0.0f, 0.0f, "V {:.2f}")) {
            n->value.x = tiling.x;
            n->value.y = tiling.y;
            changed = true;
        }
        bool second = n->value.z >= 0.5f;
        if (ui::Checkbox("Second UV set", &second)) {
            n->value.z = second ? 1.0f : 0.0f;
            changed = true;
        }
    } else if (n->type == "ComponentMask") {
        bool c[4] = {n->value.x != 0.0f, n->value.y != 0.0f, n->value.z != 0.0f, n->value.w != 0.0f};
        const StringView names[4] = {"R", "G", "B", "A"};
        for (u32 i = 0; i < 4; ++i) {
            if (i > 0)
                ui::SameLine();
            if (ui::Checkbox(names[i], &c[i]))
                changed = true;
        }
        n->value = Vec4(c[0] ? 1.0f : 0.0f, c[1] ? 1.0f : 0.0f, c[2] ? 1.0f : 0.0f, c[3] ? 1.0f : 0.0f);
    }

    // Values of unconnected inputs (baked into the shader: editing them recompiles).
    bool header = false;
    for (u32 i = 0; i < t->inputs.Size() && i < n->inputValues.Size(); ++i) {
        if (m.graph.FindInputLink(n->id, i))
            continue;
        if (!header) {
            ui::Spacing();
            ui::SeparatorText("Inputs");
            header = true;
        }
        const u32 dims = Max<u32>(t->inputs[i].dims, 1);
        ui::PushID(i32(i));
        ui::AlignTextToFramePadding();
        ui::TextDisabled(t->inputs[i].name);
        ui::SameLine(110.0f);
        ui::SetNextItemWidth(-1.0f);
        Vec4& v = n->inputValues[i];
        if (dims == 3 && (n->type == "Output" && (i == 0 || i == 4))) {
            LinearColor col(v.x, v.y, v.z, 1.0f);
            if (ui::ColorEdit("##in", &col, false)) {
                v = Vec4(col.r, col.g, col.b, v.w);
                changed = true;
            }
        } else {
            f32* comps[4] = {&v.x, &v.y, &v.z, &v.w};
            for (u32 c = 0; c < dims; ++c) {
                if (c > 0)
                    ui::SameLine(0.0f, 2.0f);
                if (dims > 1)
                    ui::SetNextItemWidth(Max(40.0f, (ui::GetContentRegionAvail().x / ui::GetCurrentContext()->GetScale()) / f32(dims - c) - 2.0f));
                ui::PushID(i32(c));
                if (ui::DragFloat("##v", comps[c], 0.01f, 0.0f, 0.0f, "{:.2f}"))
                    changed = true;
                ui::PopID();
            }
        }
        ui::PopID();
    }

    if (changed) {
        // One undo step per widget interaction (a whole slider drag or text edit).
        if (!st.widgetEdit) {
            st.undo.PushBack(before);
            if (st.undo.Size() > MaxUndo)
                st.undo.RemoveAt(0);
            st.redo.Clear();
            st.widgetEdit = true;
        }
        Commit(ed, m);
    }
    if (!ui::GetCurrentContext()->IsAnyItemActive())
        st.widgetEdit = false;
}

/// Instance editing: the parent's named parameters, each with an override toggle.
void InstancePanel(Editor& ed, MaterialEditorState& st, MaterialLibrary::Material& m)
{
    const MaterialLibrary::Material* parent = ed.materials->Find(m.parent);
    if (!parent) {
        ui::TextColored(ui::GetCurrentContext()->GetStyle().colors.error, "The parent material is missing");
        return;
    }
    ui::AlignTextToFramePadding();
    ui::TextDisabled("Parent");
    ui::SameLine();
    if (ui::Button(String::Format(ZX_ICON_PALETTE "  {}", Path::Stem(parent->path))))
        OpenMaterialEditor(ed, parent->guid);
    ui::ItemTooltip("Open the parent material");
    ui::Spacing();
    ui::SeparatorText("Parameters");
    bool any = false;
    for (const MaterialGraphNode& pn : parent->graph.nodes) {
        if (pn.name.Empty())
            continue;
        any = true;
        ui::PushID(pn.name);
        JsonValue* o = m.overrides.Find(pn.name);
        bool overridden = o != nullptr;
        // The instance's current value (override or inherited) lives in its graph copy.
        MaterialGraphNode* n = nullptr;
        for (MaterialGraphNode& c : m.graph.nodes)
            if (c.name == pn.name)
                n = &c;
        if (ui::Checkbox("##override", &overridden)) {
            if (overridden) {
                JsonValue v = JsonValue::MakeObject();
                if (pn.type == "TextureSample")
                    v.Set("texture", pn.texture);
                else {
                    JsonValue a = JsonValue::MakeArray();
                    for (f32 x : {pn.value.x, pn.value.y, pn.value.z, pn.value.w})
                        a.PushBack(x);
                    v.Set("value", move(a));
                }
                m.overrides.Set(pn.name, move(v));
            } else {
                m.overrides.Remove(pn.name);
            }
            m.dirty = true;
            ed.materials->RebuildInstance(m.guid);
            ui::PopID();
            return; // pointers changed
        }
        ui::ItemTooltip("Override the parent's value");
        ui::SameLine();
        ui::AlignTextToFramePadding();
        if (overridden)
            ui::Text(pn.name);
        else
            ui::TextDisabled(pn.name);
        ui::BeginDisabled(!overridden || !n);
        bool changed = false;
        if (n && pn.type == "Scalar") {
            ui::SameLine(150.0f);
            ui::SetNextItemWidth(-1.0f);
            changed = ui::DragFloat("##v", &n->value.x, 0.005f, 0.0f, 0.0f, "{:.3f}");
        } else if (n && pn.type == "Color") {
            ui::SameLine(150.0f);
            ui::SetNextItemWidth(-1.0f);
            LinearColor col(n->value.x, n->value.y, n->value.z, n->value.w);
            if (ui::ColorEdit("##v", &col, true)) {
                n->value = Vec4(col.r, col.g, col.b, col.a);
                changed = true;
            }
        } else if (n && pn.type == "TextureSample") {
            ui::SameLine(150.0f);
            ui::SetNextItemWidth(-1.0f);
            String tex = n->texture;
            ui::InputText("##tex", tex, ui::InputTextFlags::ReadOnly, "Drop a texture here");
            if (const void* payload = overridden ? ui::AcceptDragDropPayload("asset") : nullptr) {
                const StringView path(static_cast<const char*>(payload));
                const String rel = Path::MakeRelative(path, ed.contentRoot);
                n->texture = rel.Empty() ? String(path) : rel;
                changed = true;
            }
        }
        ui::EndDisabled();
        if (changed && n && o) {
            if (pn.type == "TextureSample") {
                o->Set("texture", n->texture);
            } else {
                JsonValue a = JsonValue::MakeArray();
                for (f32 x : {n->value.x, n->value.y, n->value.z, n->value.w})
                    a.PushBack(x);
                o->Set("value", move(a));
            }
            m.dirty = true;
            ed.materials->Apply(m.guid); // values only: same shader as the parent
        }
        ui::PopID();
    }
    if (!any)
        ui::TextDisabled("The parent has no named parameters. Give Scalar, Color or Texture Sample nodes a name to expose them.");
    ZX_UNUSED(st);
}

} // namespace

// --- Preview -------------------------------------------------------------------------------------------------------------

MaterialPreview::MaterialPreview(rhi::Device& device, RenderAssets& renderAssets) : m_device(device), m_renderAssets(renderAssets) {}

MaterialPreview::~MaterialPreview() { m_device.Destroy(m_texture); }

bool MaterialPreview::Init()
{
    m_renderer.Reset(new ViewportRenderer(m_device, m_renderAssets));
    if (!m_renderer->Init(true, rhi::Format::R8G8B8A8_UNORM))
        return false;
    ViewportSettings& s = m_renderer->Settings();
    s.renderScale = 1.0f;
    s.scene.ao.enabled = false;
    s.taa.enabled = false;
    scene::InstallHierarchyHooks(m_world);
    m_entity = scene::CreateNode(m_world, "Preview");
    m_world.Add(m_entity, MeshRenderer{});
    rhi::TextureDesc td;
    td.width = td.height = Size;
    td.format = rhi::Format::R8G8B8A8_UNORM;
    td.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::ShaderResource;
    td.debugName = "MaterialPreview";
    m_texture = m_device.CreateTexture(td);
    return m_texture != nullptr;
}

void MaterialPreview::SetMaterial(const Guid& material, BuiltinShape shape)
{
    MeshRenderer* mr = m_world.Get<MeshRenderer>(m_entity);
    mr->mesh = builtin::Mesh(shape);
    mr->materials.Resize(1);
    mr->materials[0] = material;
    Transform* t = m_world.Get<Transform>(m_entity);
    t->scale = shape == BuiltinShape::Plane ? Vec3(0.2f) : Vec3(1.0f);
    t->rotation = shape == BuiltinShape::Plane ? Quat::FromAxisAngle(Vec3::UnitX(), DegToRad(-60.0f)) : Quat();
}

void MaterialPreview::AddPasses(RenderGraph& graph, Array<RGTexture>& sampled, f32 dt)
{
    if (!m_texture)
        return;
    scene::UpdateWorldTransforms(m_world);
    const f32 distance = 2.4f;
    const Vec3 eye = Vec3(Sin(yaw) * Cos(pitch), Sin(pitch), -Cos(yaw) * Cos(pitch)) * distance;
    ViewportCamera cam;
    cam.view = Mat4::LookAt(eye, Vec3::Zero(), Vec3::Up());
    cam.position = eye;
    cam.fovY = DegToRad(32.0f);
    cam.nearPlane = 0.05f;
    ViewportEnvironment env;
    env.sunElevation = DegToRad(35.0f);
    env.sunAzimuth = DegToRad(-40.0f);
    env.autoExposure = false;
    const RGTexture target = graph.ImportTexture(m_texture, rhi::ResourceState::ShaderResource);
    m_renderer->AddPasses(graph, m_world, cam, env, target, UVec2(Size, Size), dt);
    sampled.PushBack(target);
}

// --- Panel ------------------------------------------------------------------------------------------------------------------

void OpenMaterialEditor(Editor& ed, const Guid& material)
{
    MaterialEditorState& st = ed.materialEditor;
    if (st.material != material) {
        st.selection.Clear();
        st.undo.Clear();
        st.redo.Clear();
        st.frameRequested = true;
    }
    st.material = material;
    st.open = true;
    if (!st.preview) {
        st.preview.Reset(new MaterialPreview(*ed.device, *ed.renderAssets));
        if (!st.preview->Init())
            st.preview.Reset();
    }
    ui::FocusDockedWindow(MaterialEditorTitle);
}

void RenderMaterialPreview(Editor& ed, RenderGraph& graph, Array<RGTexture>& sampled)
{
    MaterialEditorState& st = ed.materialEditor;
    if (!st.open || !st.preview || !Current(ed))
        return;
    st.preview->SetMaterial(st.material, st.previewShape);
    st.preview->AddPasses(graph, sampled, ed.dt);
}

void DrawMaterialEditorPanel(Editor& ed)
{
    MaterialEditorState& st = ed.materialEditor;
    st.capturesKeys = false;
    if (!st.open)
        return;
    MaterialLibrary::Material* mp = Current(ed);
    if (!mp) {
        st.open = false;
        return;
    }
    MaterialLibrary::Material& m = *mp;
    {
        // First open: 85 % of the window, centered (logical units).
        const f32 dpi = ui::GetCurrentContext()->GetScale();
        const Vec2 display = Vec2(f32(ed.swapChain->GetSize().x), f32(ed.swapChain->GetSize().y)) / dpi;
        const Vec2 size = display * 0.85f;
        ui::SetNextWindowSize(size, ui::Cond::FirstUse);
        ui::SetNextWindowPos((display - size) * 0.5f, ui::Cond::FirstUse);
    }
    bool open = true;
    if (!ui::Begin(MaterialEditorTitle, &open, ui::WindowFlags::NoScrollbar)) {
        ui::End();
        st.open = open;
        return;
    }
    st.open = open;
    ui::Context& ctx = *ui::GetCurrentContext();
    const f32 s = ctx.GetScale();
    const ui::Theme& th = ctx.GetStyle().colors;

    // --- Toolbar ---
    if (ui::Button(ZX_ICON_SAVE "  Save", Vec2::Zero(), m.dirty ? ui::ButtonVariant::Primary : ui::ButtonVariant::Default))
        SaveMaterial(ed, m);
    ui::SameLine();
    ui::AlignTextToFramePadding();
    ui::Text(String::Format("{}{}", Path::Stem(m.path), m.dirty ? "*" : ""));
    ui::SameLine(0.0f, 16.0f);
    {
        const MaterialShaderCache::State state = ed.materials->ShaderState(m.guid);
        if (!m.compiled.Ok())
            ui::TextColored(th.error, String::Format(ZX_ICON_CIRCLE_X "  {} error{}", m.compiled.errors.Size(), m.compiled.errors.Size() == 1 ? "" : "s"));
        else if (state == MaterialShaderCache::State::Compiling)
            ui::TextColored(th.warning, ZX_ICON_LOADER "  Compiling...");
        else if (state == MaterialShaderCache::State::Failed)
            ui::TextColored(th.error, ZX_ICON_CIRCLE_X "  Shader failed (see Output Log)");
        else
            ui::TextColored(th.success, String::Format(ZX_ICON_CHECK "  Compiled ({:.0f} ms)", ed.renderAssets->Shaders().GetCompileMs(m.shader)));
        if (!m.compiled.Ok() && ui::IsItemHovered()) {
            String all;
            for (const String& e : m.compiled.errors)
                all += e + "\n";
            ui::SetTooltip(all);
        }
    }
    if (!m.IsInstance()) {
        ui::SameLine(0.0f, 16.0f);
        i32 blend = i32(m.graph.blend);
        static const StringView Blends[] = {"Opaque", "Masked", "Translucent"};
        ui::SetNextItemWidth(110.0f);
        if (ui::Combo("##blend", &blend, Span<const StringView>(Blends, 3))) {
            PushUndo(st, m.graph);
            m.graph.blend = MaterialBlendMode(blend);
            Commit(ed, m);
        }
        ui::SameLine();
        bool twoSided = m.graph.twoSided, unlit = m.graph.unlit;
        if (ui::Checkbox("Two Sided", &twoSided)) {
            PushUndo(st, m.graph);
            m.graph.twoSided = twoSided;
            Commit(ed, m);
        }
        ui::SameLine();
        if (ui::Checkbox("Unlit", &unlit)) {
            PushUndo(st, m.graph);
            m.graph.unlit = unlit;
            Commit(ed, m);
        }
        if (m.graph.blend == MaterialBlendMode::Masked) {
            ui::SameLine();
            ui::SetNextItemWidth(90.0f);
            if (ui::DragFloat("##cutoff", &m.graph.alphaCutoff, 0.01f, 0.0f, 1.0f, "Cutoff {:.2f}"))
                Commit(ed, m);
        }
    }

    // --- Instances: parameters + preview, no graph ---
    if (m.IsInstance()) {
        const Rect area = ui::GetWindowContentRect();
        const f32 previewW = Round(Min(area.Width() * 0.45f, 420.0f * s));
        const Vec2 start = ui::GetCursorPos() + Vec2(0.0f, Round(6.0f * s));
        ui::SetCursorPos(start);
        if (ui::BeginChild("##instance_params", Vec2((area.max.x - start.x - previewW - 12.0f * s) / s, (area.max.y - start.y) / s), false))
            InstancePanel(ed, st, m);
        ui::EndChild();
        const Rect preview(Vec2(area.max.x - previewW, start.y), Vec2(area.max.x, start.y + previewW));
        if (st.preview)
            ui::GetWindowDrawList().AddImage(st.preview->Srv(), preview, Vec2(0, 0), Vec2(1, 1), Color(255, 255, 255), 6.0f * s);
        ui::SetCursorPos(preview.min);
        ui::InvisibleButton("##inst_preview", preview.Size() / s);
        if (ui::IsItemActive() && st.preview) {
            st.preview->yaw += ed.input.MouseDelta().x * 0.01f;
            st.preview->pitch = Clamp(st.preview->pitch + ed.input.MouseDelta().y * 0.01f, -1.3f, 1.3f);
        }
        st.capturesKeys = false;
        if (ui::IsWindowHovered() && ui::IsShortcutPressed(KeyModifiers::Control, Key::S))
            SaveMaterial(ed, m);
        ui::End();
        return;
    }

    // --- Layout: canvas | side panel ---
    const Rect content = ui::GetWindowContentRect();
    const f32 sideW = Round(300.0f * s);
    const f32 top = ui::GetCursorPos().y + Round(4.0f * s);
    const Rect canvasRect(Vec2(content.min.x, top), Vec2(content.max.x - sideW - Round(6.0f * s), content.max.y));
    const Rect sideRect(Vec2(canvasRect.max.x + Round(6.0f * s), top), content.max);
    Canvas c{canvasRect, st.pan, st.zoom * s};
    if (st.frameRequested && canvasRect.Width() > 50.0f) {
        FrameAll(st, m.graph, canvasRect, s);
        c.pan = st.pan;
        c.scale = st.zoom * s;
        st.frameRequested = false;
    }

    DrawList& dl = ui::GetWindowDrawList();
    ui::SetCursorPos(canvasRect.min);
    ui::InvisibleButton("##mat_canvas", canvasRect.Size() / s);
    const bool hovered = ui::IsItemHovered() || ui::IsItemActive();
    const Vec2 mouse = ui::GetMousePos();
    dl.PushClipRect(canvasRect);
    DrawGrid(dl, c, s);

    // Hit testing (pins first, topmost node last drawn = last in the array).
    u32 hotNode = 0, hotPin = 0;
    bool hotPinOutput = false, hotIsPin = false;
    for (usize i = m.graph.nodes.Size(); i > 0 && hovered; --i) {
        const MaterialGraphNode& n = m.graph.nodes[i - 1];
        const MaterialNodeType* t = FindMaterialNodeType(n.type);
        if (!t)
            continue;
        const f32 pr = (PinRadius + 4.0f) * c.scale;
        for (u32 p = 0; p < t->outputs.Size() && !hotIsPin; ++p)
            if (Length(PinPos(c, n, p, true) - mouse) < pr) {
                hotNode = n.id, hotPin = p, hotPinOutput = true, hotIsPin = true;
            }
        for (u32 p = 0; p < t->inputs.Size() && !hotIsPin; ++p)
            if (Length(PinPos(c, n, p, false) - mouse) < pr) {
                hotNode = n.id, hotPin = p, hotPinOutput = false, hotIsPin = true;
            }
        if (hotIsPin)
            break;
        const Rect r(c.ToScreen(n.position), c.ToScreen(n.position + Vec2(NodeWidth, NodeHeight(n, t))));
        if (r.Contains(mouse)) {
            hotNode = n.id;
            break;
        }
    }

    // --- Mouse ---
    const bool ctrl = HasAnyFlags(ed.input.Modifiers(), KeyModifiers::Control);
    const bool alt = HasAnyFlags(ed.input.Modifiers(), KeyModifiers::Alt);
    if (hovered && ed.input.WasMousePressed(MouseButton::Left)) {
        st.dragStart = mouse;
        st.dragMoved = false;
        if (hotIsPin) {
            if (!hotPinOutput && alt) {
                PushUndo(st, m.graph);
                m.graph.Disconnect(hotNode, hotPin);
                Commit(ed, m);
            } else if (const MaterialGraphLink* link = hotPinOutput ? nullptr : m.graph.FindInputLink(hotNode, hotPin)) {
                // Grab an existing link by its input end: re-route it from its source.
                st.beforeDrag = Snapshot(m.graph);
                st.linkNode = link->fromNode;
                st.linkPin = link->fromPin;
                st.linkFromOutput = true;
                m.graph.Disconnect(hotNode, hotPin);
                st.drag = MaterialEditorState::Drag::Link;
            } else {
                st.beforeDrag = Snapshot(m.graph);
                st.linkNode = hotNode;
                st.linkPin = hotPin;
                st.linkFromOutput = hotPinOutput;
                st.drag = MaterialEditorState::Drag::Link;
            }
        } else if (hotNode) {
            if (ctrl) {
                if (st.selection.Contains(hotNode))
                    st.selection.Remove(hotNode);
                else
                    st.selection.PushBack(hotNode);
            } else if (!st.selection.Contains(hotNode)) {
                st.selection.Clear();
                st.selection.PushBack(hotNode);
            }
            st.beforeDrag = Snapshot(m.graph);
            st.drag = MaterialEditorState::Drag::Nodes;
        } else {
            if (!ctrl)
                st.selection.Clear();
            st.drag = MaterialEditorState::Drag::Box;
        }
    }
    if ((hovered || st.drag == MaterialEditorState::Drag::Pan) &&
        (ed.input.WasMousePressed(MouseButton::Right) || ed.input.WasMousePressed(MouseButton::Middle))) {
        st.rightPress = mouse;
        st.dragMoved = false;
        st.drag = MaterialEditorState::Drag::Pan;
    }
    if (hovered && ed.input.MouseWheel() != 0.0f) {
        const Vec2 before = c.ToGraph(mouse);
        st.zoom = Clamp(st.zoom * Pow(1.15f, ed.input.MouseWheel()), 0.25f, 2.0f);
        c.scale = st.zoom * s;
        st.pan += c.ToGraph(mouse) - before;
        c.pan = st.pan;
    }

    const Vec2 delta = ed.input.MouseDelta();
    switch (st.drag) {
    case MaterialEditorState::Drag::Nodes:
        if (ed.input.IsMouseDown(MouseButton::Left)) {
            if (Length(mouse - st.dragStart) > 3.0f * s)
                st.dragMoved = true;
            if (st.dragMoved)
                for (u32 id : st.selection)
                    if (MaterialGraphNode* n = m.graph.FindNode(id))
                        n->position += delta / c.scale;
        } else {
            if (st.dragMoved) {
                PushUndo(st, m.graph);
                st.undo.Back() = st.beforeDrag; // the state before the move
                m.dirty = true;
            }
            st.beforeDrag.Clear();
            st.drag = MaterialEditorState::Drag::None;
        }
        break;
    case MaterialEditorState::Drag::Box: {
        const Rect box(Min(st.dragStart, mouse), Max(st.dragStart, mouse));
        if (ed.input.IsMouseDown(MouseButton::Left)) {
            dl.AddRect(box, WithAlpha(th.accent, 0.12f), ui::CornerRadii(), 1.0f, WithAlpha(th.accentHovered, 0.8f));
        } else {
            if (box.Width() > 3.0f || box.Height() > 3.0f)
                for (const MaterialGraphNode& n : m.graph.nodes) {
                    const Rect r(c.ToScreen(n.position), c.ToScreen(n.position + Vec2(NodeWidth, NodeHeight(n, FindMaterialNodeType(n.type)))));
                    if (r.Intersects(box) && !st.selection.Contains(n.id))
                        st.selection.PushBack(n.id);
                }
            st.drag = MaterialEditorState::Drag::None;
        }
        break;
    }
    case MaterialEditorState::Drag::Link:
        if (!ed.input.IsMouseDown(MouseButton::Left)) {
            bool connected = false;
            if (hotIsPin && hotNode != st.linkNode && hotPinOutput != st.linkFromOutput) {
                connected = st.linkFromOutput ? m.graph.Connect(st.linkNode, st.linkPin, hotNode, hotPin)
                                              : m.graph.Connect(hotNode, hotPin, st.linkNode, st.linkPin);
            }
            if (connected || Snapshot(m.graph) != st.beforeDrag) {
                st.undo.PushBack(st.beforeDrag);
                st.redo.Clear();
                Commit(ed, m);
            }
            if (!connected && !hotIsPin && Length(mouse - st.dragStart) > 6.0f * s) {
                // Dropped on empty space: offer the node list and connect the new node.
                st.addHasLink = true;
                st.addLinkNode = st.linkNode;
                st.addLinkPin = st.linkPin;
                st.addLinkFromOutput = st.linkFromOutput;
                st.addPosition = c.ToGraph(mouse);
                st.addSearch.Clear();
                ui::OpenPopup("##mat_add_node");
            }
            st.beforeDrag.Clear();
            st.drag = MaterialEditorState::Drag::None;
        }
        break;
    case MaterialEditorState::Drag::Pan:
        if (ed.input.IsMouseDown(MouseButton::Right) || ed.input.IsMouseDown(MouseButton::Middle)) {
            if (Length(mouse - st.rightPress) > 4.0f * s)
                st.dragMoved = true;
            st.pan += delta / c.scale;
            c.pan = st.pan;
        } else {
            if (!st.dragMoved && hovered) {
                if (hotNode && !hotIsPin) {
                    if (!st.selection.Contains(hotNode)) {
                        st.selection.Clear();
                        st.selection.PushBack(hotNode);
                    }
                    ui::OpenPopup("##mat_node_menu");
                } else {
                    st.addHasLink = false;
                    st.addPosition = c.ToGraph(mouse);
                    st.addSearch.Clear();
                    ui::OpenPopup("##mat_add_node");
                }
            }
            st.drag = MaterialEditorState::Drag::None;
        }
        break;
    case MaterialEditorState::Drag::None: break;
    }

    // --- Links ---
    for (const MaterialGraphLink& l : m.graph.links) {
        const MaterialGraphNode* a = m.graph.FindNode(l.fromNode);
        const MaterialGraphNode* b = m.graph.FindNode(l.toNode);
        if (!a || !b)
            continue;
        const u32 dims = PinDims(FindMaterialNodeType(a->type), l.fromPin, true);
        DrawLink(dl, PinPos(c, *a, l.fromPin, true), PinPos(c, *b, l.toPin, false), WithAlpha(PinColor(dims), 0.9f), c.scale);
    }
    if (st.drag == MaterialEditorState::Drag::Link)
        if (const MaterialGraphNode* n = m.graph.FindNode(st.linkNode)) {
            const Vec2 p = PinPos(c, *n, st.linkPin, st.linkFromOutput);
            if (st.linkFromOutput)
                DrawLink(dl, p, mouse, th.accentHovered, c.scale);
            else
                DrawLink(dl, mouse, p, th.accentHovered, c.scale);
        }

    // --- Nodes (selected ones on top) ---
    for (const MaterialGraphNode& n : m.graph.nodes)
        if (!st.selection.Contains(n.id))
            DrawNode(ed, dl, c, n, false, m.compiled.errorNodes.Contains(n.id));
    for (const MaterialGraphNode& n : m.graph.nodes)
        if (st.selection.Contains(n.id))
            DrawNode(ed, dl, c, n, true, m.compiled.errorNodes.Contains(n.id));
    if (hotIsPin) {
        const MaterialGraphNode* n = m.graph.FindNode(hotNode);
        dl.AddCircleOutline(PinPos(c, *n, hotPin, hotPinOutput), (PinRadius + 3.0f) * c.scale, th.accentHovered, 1.5f);
    }
    // Hint in the corner.
    dl.AddTextInRect(ctx.Fonts().FindFont("Inter"), 11.0f * s,
                     Rect(Vec2(canvasRect.min.x + 10.0f * s, canvasRect.max.y - 24.0f * s), canvasRect.max - Vec2(10.0f * s, 4.0f * s)),
                     th.textDisabled, "Right click: add node   Drag pins: connect   Alt+click: disconnect   Wheel: zoom   RMB drag: pan   F: frame");
    dl.PopClipRect();

    // --- Popups ---
    AddNodePopup(ed, st, m);
    if (ui::BeginPopup("##mat_node_menu")) {
        if (ui::MenuItem(ZX_ICON_COPY "  Duplicate", "Ctrl+D"))
            Paste(ed, st, m, CopySelection(st, m.graph), c.ToGraph(st.rightPress) + Vec2(20.0f));
        if (ui::MenuItem(ZX_ICON_CLIPBOARD_COPY "  Copy", "Ctrl+C"))
            st.clipboard = CopySelection(st, m.graph);
        if (ui::MenuItem(ZX_ICON_TRASH_2 "  Delete", "Del"))
            DeleteSelection(ed, st, m);
        ui::EndPopup();
    }

    // --- Keyboard (while the canvas is hovered) ---
    st.capturesKeys = hovered && !ctx.WantsKeyboard();
    if (st.capturesKeys) {
        if (ui::IsKeyPressed(Key::Delete, false) || ui::IsKeyPressed(Key::Backspace, false))
            DeleteSelection(ed, st, m);
        if (ui::IsShortcutPressed(KeyModifiers::Control, Key::C))
            st.clipboard = CopySelection(st, m.graph);
        if (ui::IsShortcutPressed(KeyModifiers::Control, Key::V) && !st.clipboard.Empty())
            Paste(ed, st, m, st.clipboard, c.ToGraph(mouse));
        if (ui::IsShortcutPressed(KeyModifiers::Control, Key::D))
            Paste(ed, st, m, CopySelection(st, m.graph), c.ToGraph(mouse));
        if (ui::IsShortcutPressed(KeyModifiers::Control, Key::S))
            SaveMaterial(ed, m);
        if (ui::IsKeyPressed(Key::F, false))
            st.frameRequested = true;
        const bool undo = ui::IsShortcutPressed(KeyModifiers::Control, Key::Z);
        const bool redo = ui::IsShortcutPressed(KeyModifiers::Control, Key::Y) ||
                          ui::IsShortcutPressed(KeyModifiers::Control | KeyModifiers::Shift, Key::Z);
        if ((undo && !st.undo.Empty()) || (redo && !st.redo.Empty())) {
            Array<String>& from = undo ? st.undo : st.redo;
            Array<String>& to = undo ? st.redo : st.undo;
            to.PushBack(Snapshot(m.graph));
            Restore(m.graph, from.Back());
            from.PopBack();
            st.selection.Clear();
            Commit(ed, m);
        }
    }

    // --- Side panel: preview + properties ---
    ui::SetCursorPos(sideRect.min);
    if (ui::BeginChild("##mat_side", sideRect.Size() / s, false)) {
        const f32 side = ui::GetContentRegionAvail().x;
        const Vec2 p = ui::GetCursorPos();
        const Rect preview(p, p + Vec2(side, side));
        if (st.preview)
            ui::GetWindowDrawList().AddImage(st.preview->Srv(), preview, Vec2(0, 0), Vec2(1, 1), Color(255, 255, 255), 6.0f * s);
        ui::InvisibleButton("##mat_preview", preview.Size() / s);
        if (ui::IsItemActive() && st.preview) {
            st.preview->yaw += ed.input.MouseDelta().x * 0.01f;
            st.preview->pitch = Clamp(st.preview->pitch + ed.input.MouseDelta().y * 0.01f, -1.3f, 1.3f);
        }
        ui::ItemTooltip("Drag to rotate");
        const StringView shapes[4] = {ZX_ICON_CIRCLE, ZX_ICON_BOX, ZX_ICON_CYLINDER, ZX_ICON_SQUARE};
        const BuiltinShape order[4] = {BuiltinShape::Sphere, BuiltinShape::Cube, BuiltinShape::Cylinder, BuiltinShape::Plane};
        for (u32 i = 0; i < 4; ++i) {
            if (i > 0)
                ui::SameLine(0.0f, 2.0f);
            if (ui::IconButton(shapes[i], builtin::ShapeName(order[i]), st.previewShape == order[i]))
                st.previewShape = order[i];
        }
        ui::Spacing();
        ui::SeparatorText("Details");
        PropertiesPanel(ed, st, m);
    }
    ui::EndChild();
    ui::End();
}

} // namespace zx::editor
