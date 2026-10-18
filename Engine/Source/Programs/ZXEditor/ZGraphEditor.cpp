#include "ZGraphEditor.h"
#include "Editor.h"
#include "Core/FileSystem/FileSystem.h"
#include "Core/String/Path.h"
#include "Core/Time/Time.h"

ZX_DEFINE_LOG_CATEGORY_STATIC(LogGraphEditor, Info);

namespace zx::editor {

namespace {

// Node layout in graph units (1 unit = 1 logical pixel at zoom 1).
constexpr f32 NodeWidth = 210.0f;
constexpr f32 HeaderHeight = 26.0f;
constexpr f32 RowHeight = 22.0f;
constexpr f32 PinRadius = 5.0f;
constexpr usize MaxUndo = 100;

Color KindColor(GraphNodeKind k)
{
    switch (k) {
    case GraphNodeKind::Event: return Color(0xA8, 0x34, 0x34);
    case GraphNodeKind::Flow: return Color(0x5A, 0x5E, 0x6E);
    case GraphNodeKind::Action: return Color(0x2F, 0x6A, 0xB0);
    case GraphNodeKind::Pure: return Color(0x3A, 0x84, 0x4E);
    }
    return Color(0x55, 0x55, 0x55);
}

Color TypeColor(GraphPinType t)
{
    switch (t) {
    case GraphPinType::Exec: return Color(0xF0, 0xF0, 0xF0);
    case GraphPinType::Number: return Color(0x9A, 0xD8, 0x6A);
    case GraphPinType::Bool: return Color(0xE0, 0x5A, 0x5A);
    case GraphPinType::String: return Color(0xE0, 0x78, 0xD0);
    case GraphPinType::Vec3: return Color(0xF0, 0xC8, 0x40);
    case GraphPinType::Entity: return Color(0x4E, 0xC8, 0xE8);
    case GraphPinType::Any: return Color(0xB0, 0xB0, 0xB0);
    }
    return Color(0xB0, 0xB0, 0xB0);
}

f32 NodeHeight(const GraphNodeDef* d)
{
    const u32 rows = d ? Max(d->InputCount(), d->OutputCount()) : 1;
    return HeaderHeight + f32(Max<u32>(rows, 1)) * RowHeight + 8.0f;
}

struct Canvas
{
    Rect rect;
    Vec2 pan;
    f32 scale = 1.0f; // zoom * DPI scale
    Vec2 ToScreen(Vec2 p) const { return rect.min + (p + pan) * scale; }
    Vec2 ToGraph(Vec2 q) const { return (q - rect.min) / scale - pan; }
};

Vec2 PinPos(const Canvas& c, const GraphNode& n, u32 index, bool output)
{
    return c.ToScreen(Vec2(output ? n.position.x + NodeWidth : n.position.x, n.position.y + HeaderHeight + (f32(index) + 0.5f) * RowHeight));
}

i32 PinIndex(const GraphNodeDef* d, StringView pin, bool output)
{
    if (!d)
        return -1;
    const u32 count = output ? d->OutputCount() : d->InputCount();
    for (u32 i = 0; i < count; ++i)
        if (pin == (output ? d->outputs[i].name : d->inputs[i].name))
            return i32(i);
    return -1;
}

String PinLabel(StringView name)
{
    if (name == "in" || name == "then")
        return {};
    String s(name);
    if (!s.Empty() && s[0] >= 'a' && s[0] <= 'z')
        s[0] = char(s[0] - 'a' + 'A');
    return s;
}

String Snapshot(const Graph& g) { return WriteGraph(g); }

void Recompile(ZGraphEditorState& st) { CompileGraph(st.graph, st.compiled); }

void PushUndo(ZGraphEditorState& st)
{
    st.undo.PushBack(Snapshot(st.graph));
    if (st.undo.Size() > MaxUndo)
        st.undo.RemoveAt(0);
    st.redo.Clear();
}

void Changed(ZGraphEditorState& st)
{
    st.dirty = true;
    Recompile(st);
}

void Save(ZGraphEditorState& st)
{
    if (fs::WriteTextFile(st.path, WriteGraph(st.graph))) {
        st.dirty = false;
        ZX_LOG_INFO(LogGraphEditor, "Saved {}", Path::FileName(st.path));
    } else {
        ZX_LOG_ERROR(LogGraphEditor, "Can't write {}", st.path);
    }
}

/// Links an output to an input if the types fit (an input takes one wire; an exec output leads to one node).
bool Connect(Graph& g, u32 fromNode, StringView fromPin, u32 toNode, StringView toPin)
{
    const GraphNode* a = g.FindNode(fromNode);
    const GraphNode* b = g.FindNode(toNode);
    const GraphNodeDef* da = a ? FindGraphNode(a->type) : nullptr;
    const GraphNodeDef* db = b ? FindGraphNode(b->type) : nullptr;
    const GraphPinDef* pa = da ? da->Output(fromPin) : nullptr;
    const GraphPinDef* pb = db ? db->Input(toPin) : nullptr;
    if (!pa || !pb || fromNode == toNode || !GraphPinsCompatible(g.PinType(*a, *pa), g.PinType(*b, *pb)))
        return false;
    const bool exec = pa->type == GraphPinType::Exec;
    g.links.RemoveIf([&](const GraphLink& l) {
        return exec ? (l.fromNode == fromNode && l.fromPin == fromPin) : (l.toNode == toNode && l.toPin == toPin);
    });
    g.links.PushBack(GraphLink{fromNode, String(fromPin), toNode, String(toPin)});
    return true;
}

void Disconnect(Graph& g, u32 node, StringView pin, bool output)
{
    g.links.RemoveIf([&](const GraphLink& l) { return output ? (l.fromNode == node && l.fromPin == pin) : (l.toNode == node && l.toPin == pin); });
}

void RemoveNode(Graph& g, u32 id)
{
    g.nodes.RemoveIf([&](const GraphNode& n) { return n.id == id; });
    g.links.RemoveIf([&](const GraphLink& l) { return l.fromNode == id || l.toNode == id; });
}

GraphNode& AddNode(Graph& g, StringView type, Vec2 at)
{
    GraphNode n;
    n.id = g.NextId();
    n.type = String(type);
    n.position = Round(at);
    g.nodes.PushBack(move(n));
    return g.nodes.Back();
}

// --- Drawing -------------------------------------------------------------------------------------------------------------

void DrawGrid(ui::DrawList& dl, const Canvas& c, f32 s)
{
    dl.AddRect(c.rect, Color(0x1A, 0x1B, 0x1F));
    const Vec2 origin = c.ToScreen(Vec2::Zero());
    for (f32 step : {16.0f * c.scale, 128.0f * c.scale}) {
        if (step < 6.0f * s)
            continue;
        const Color col = step > 100.0f * c.scale ? Color(0x2E, 0x30, 0x36) : Color(0x23, 0x24, 0x29);
        for (f32 x = Fmod(origin.x - c.rect.min.x, step) + c.rect.min.x; x < c.rect.max.x; x += step)
            dl.AddLine(Vec2(x, c.rect.min.y), Vec2(x, c.rect.max.y), col, 1.0f);
        for (f32 y = Fmod(origin.y - c.rect.min.y, step) + c.rect.min.y; y < c.rect.max.y; y += step)
            dl.AddLine(Vec2(c.rect.min.x, y), Vec2(c.rect.max.x, y), col, 1.0f);
    }
}

void DrawWire(ui::DrawList& dl, Vec2 from, Vec2 to, Color color, f32 scale, bool exec)
{
    const f32 dx = Max(Abs(to.x - from.x) * 0.5f, 40.0f * scale);
    dl.AddBezier(from, from + Vec2(dx, 0.0f), to - Vec2(dx, 0.0f), to, color, Max((exec ? 3.0f : 2.0f) * scale, 1.5f));
}

void DrawPin(ui::DrawList& dl, Vec2 p, GraphPinType type, bool linked, f32 k)
{
    const Color col = TypeColor(type);
    if (type == GraphPinType::Exec) {
        const f32 r = 6.0f * k;
        const Vec2 a = p + Vec2(-r * 0.7f, -r), b = p + Vec2(r, 0.0f), c = p + Vec2(-r * 0.7f, r);
        if (linked)
            dl.AddTriangle(a, b, c, col);
        else {
            const Vec2 pts[3] = {a, b, c};
            dl.AddPolyline(Span<const Vec2>(pts, 3), col, 1.5f * k, true);
        }
        return;
    }
    dl.AddCircle(p, PinRadius * k, linked ? col : Color(0x22, 0x23, 0x28), 1.5f * k, col);
}

/// The literal shown next to an unlinked input ("= 90").
String ValueText(const GraphNode& n, const GraphPinDef& pin)
{
    const String* v = n.values.Find(pin.name);
    const StringView text = v ? v->View() : StringView(pin.defaultValue);
    if (pin.type == GraphPinType::String)
        return String::Format("\"{}\"", text);
    return String(text);
}

constexpr f64 HitGlowSeconds = 0.45;

/// 0..1: how recently a node ran (debugging).
f32 Glow(const ZGraphEditorState& st, u32 node)
{
    const f64* t = st.hits.Find(node);
    if (!t)
        return 0.0f;
    // Paused: the time stands still, so the last frame's path stays lit.
    const f64 now = st.frozenAt > 0.0 ? st.frozenAt : Clock::NowSeconds();
    const f64 age = now - *t;
    return age < HitGlowSeconds ? f32(1.0 - age / HitGlowSeconds) : 0.0f;
}

void DrawNode(const Graph& g, ui::DrawList& dl, const Canvas& c, const GraphNode& n, bool selected, bool error, f32 glow, bool breakpoint,
              bool breakHere)
{
    ui::Context& ctx = *ui::GetCurrentContext();
    const ui::Theme& th = ctx.GetStyle().colors;
    const GraphNodeDef* d = FindGraphNode(n.type);
    const Rect r(c.ToScreen(n.position), c.ToScreen(n.position + Vec2(NodeWidth, NodeHeight(d))));
    if (!r.Intersects(c.rect))
        return;
    const f32 k = c.scale;
    dl.AddShadow(r, Color(0, 0, 0, 120), 6.0f * k, 10.0f * k, Vec2(0.0f, 3.0f * k));
    dl.AddRect(r, Color(0x26, 0x27, 0x2D, 245), ui::CornerRadii(6.0f * k), error ? 2.0f * k : 1.0f,
               error ? th.error : selected ? th.accentHovered : Color(0x10, 0x10, 0x12));
    if (selected && !error)
        dl.AddRect(r.Expanded(1.5f * k), Color(0, 0, 0, 0), ui::CornerRadii(7.5f * k), 1.5f * k, th.accentHovered);
    // Debugging: the node just ran (fading glow), paused here (steady red).
    if (glow > 0.0f)
        dl.AddRect(r.Expanded(3.0f * k), Color(0, 0, 0, 0), ui::CornerRadii(9.0f * k), 2.5f * k, Color(0xFF, 0xC8, 0x3C, u8(glow * 255.0f)));
    if (breakHere)
        dl.AddRect(r.Expanded(4.0f * k), Color(0, 0, 0, 0), ui::CornerRadii(10.0f * k), 3.0f * k, Color(0xF0, 0x4A, 0x4A));
    const Rect header(r.min, Vec2(r.max.x, r.min.y + HeaderHeight * k));
    const Color hc = d ? KindColor(d->kind) : th.error;
    dl.AddRectGradient(header.Expanded(-1.0f), hc, WithAlpha(hc, 0.5f), ui::GradientDirection::Horizontal, ui::CornerRadii::Top(5.0f * k));
    if (breakpoint) // a red dot on the header, like a code breakpoint
        dl.AddCircle(Vec2(r.min.x - 1.0f * k, header.Center().y), 6.0f * k, Color(0xE0, 0x40, 0x40), 1.5f * k, Color(255, 255, 255));
    if (k < 0.45f)
        return;
    const ui::FontHandle semi = ctx.Fonts().FindFont("Inter SemiBold");
    const ui::FontHandle font = ctx.Fonts().FindFont("Inter");
    String title = d ? String(d->title) : String::Format("? {}", n.type);
    if (d) {
        // Settings in the title: "Get speed", "On Key Pressed (Space)".
        if (const String* v = n.settings.Find("variable"); v && !v->Empty())
            title = String::Format("{} {}", n.type == "pure.getVariable" ? "Get" : "Set", *v);
        for (const char* key : {"key", "event", "widget", "seconds"})
            if (const String* v = n.settings.Find(key); v && !v->Empty() && StringView(key) != "variable")
                title = String::Format("{}  ({})", d->title, *v);
    }
    dl.AddTextInRect(semi, 12.0f * k, Rect(header.min + Vec2(8.0f * k, 0.0f), header.max - Vec2(6.0f * k, 0.0f)), Color(255, 255, 255), title);
    if (!d)
        return;
    for (u32 i = 0; i < d->InputCount(); ++i) {
        const GraphPinDef& pin = d->inputs[i];
        const Vec2 p = PinPos(c, n, i, false);
        const bool linked = g.LinkInto(n.id, pin.name) != nullptr ||
                            (pin.type == GraphPinType::Exec && [&] {
                                for (const GraphLink& l : g.links)
                                    if (l.toNode == n.id && l.toPin == pin.name)
                                        return true;
                                return false;
                            }());
        DrawPin(dl, p, g.PinType(n, pin), linked, k);
        String label = PinLabel(pin.name);
        if (!linked && pin.type != GraphPinType::Exec)
            label = String::Format("{}  {}", label, ValueText(n, pin));
        if (!label.Empty())
            dl.AddTextInRect(font, 11.0f * k, Rect(p + Vec2(10.0f * k, -RowHeight * 0.5f * k), Vec2(r.max.x - 50.0f * k, p.y + RowHeight * 0.5f * k)),
                             linked ? th.text : th.textDim, label);
    }
    Array<const GraphLink*> out;
    for (u32 o = 0; o < d->OutputCount(); ++o) {
        const GraphPinDef& pin = d->outputs[o];
        const Vec2 p = PinPos(c, n, o, true);
        g.LinksFrom(n.id, pin.name, out);
        DrawPin(dl, p, g.PinType(n, pin), !out.Empty(), k);
        const String label = PinLabel(pin.name);
        if (!label.Empty())
            dl.AddTextInRect(font, 11.0f * k, Rect(Vec2(r.Center().x - 20.0f * k, p.y - RowHeight * 0.5f * k), Vec2(p.x - 10.0f * k, p.y + RowHeight * 0.5f * k)),
                             th.textDim, label, ui::TextAlign::Right);
    }
}

// --- Editing ----------------------------------------------------------------------------------------------------------------

void DeleteSelection(ZGraphEditorState& st)
{
    if (st.selection.Empty())
        return;
    PushUndo(st);
    for (u32 id : st.selection)
        RemoveNode(st.graph, id);
    st.selection.Clear();
    Changed(st);
}

String CopySelection(const ZGraphEditorState& st)
{
    Graph part;
    for (u32 id : st.selection)
        if (const GraphNode* n = st.graph.FindNode(id))
            part.nodes.PushBack(*n);
    for (const GraphLink& l : st.graph.links)
        if (part.FindNode(l.fromNode) && part.FindNode(l.toNode))
            part.links.PushBack(l);
    return part.nodes.Empty() ? String() : Snapshot(part);
}

void Paste(ZGraphEditorState& st, StringView text, Vec2 at)
{
    Graph part;
    if (!ParseGraph(text, part) || part.nodes.Empty())
        return;
    PushUndo(st);
    Vec2 minPos(1e30f);
    for (const GraphNode& n : part.nodes)
        minPos = Min(minPos, n.position);
    HashMap<u32, u32> remap;
    st.selection.Clear();
    for (const GraphNode& n : part.nodes) {
        GraphNode copy = n;
        copy.id = st.graph.NextId();
        copy.position = Round(at + (n.position - minPos));
        remap.InsertOrAssign(n.id, copy.id);
        st.selection.PushBack(copy.id);
        st.graph.nodes.PushBack(move(copy));
    }
    for (const GraphLink& l : part.links)
        st.graph.links.PushBack(GraphLink{*remap.Find(l.fromNode), l.fromPin, *remap.Find(l.toNode), l.toPin});
    Changed(st);
}

void FrameAll(ZGraphEditorState& st, const Rect& rect, f32 dpi)
{
    if (st.graph.nodes.Empty()) {
        st.pan = Vec2(40.0f);
        st.zoom = 1.0f;
        return;
    }
    Vec2 lo(1e30f), hi(-1e30f);
    for (const GraphNode& n : st.graph.nodes) {
        lo = Min(lo, n.position);
        hi = Max(hi, n.position + Vec2(NodeWidth, NodeHeight(FindGraphNode(n.type))));
    }
    const Vec2 size = hi - lo + Vec2(80.0f);
    const Vec2 view = rect.Size() / dpi;
    st.zoom = Clamp(Min(view.x / size.x, view.y / size.y), 0.3f, 1.2f);
    st.pan = view / (2.0f * st.zoom) - (lo + hi) * 0.5f;
}

/// A pin of `d` that can take a wire from (or give one to) the dragged pin.
const GraphPinDef* CompatiblePin(const Graph& g, const GraphNode& n, const GraphNodeDef& d, GraphPinType type, bool wantInput)
{
    const u32 count = wantInput ? d.InputCount() : d.OutputCount();
    for (u32 i = 0; i < count; ++i) {
        const GraphPinDef& p = wantInput ? d.inputs[i] : d.outputs[i];
        const GraphPinType pt = g.PinType(n, p);
        if (wantInput ? GraphPinsCompatible(type, pt) : GraphPinsCompatible(pt, type))
            return &p;
    }
    return nullptr;
}

void AddNodePopup(ZGraphEditorState& st)
{
    if (!ui::BeginPopup("##zg_add_node"))
        return;
    ui::SetNextItemWidth(260.0f);
    if (st.addFocus) { // type to search right away
        ui::SetKeyboardFocusHere();
        st.addFocus = false;
    }
    ui::InputText("##zg_add_search", st.addSearch, ui::InputTextFlags::None, ZX_ICON_SEARCH "  Search nodes...");
    ui::Separator();
    if (!ui::BeginChild("##zg_add_list", Vec2(270.0f, 420.0f), false)) { // scrolls: the library is long
        ui::EndChild();
        ui::EndPopup();
        return;
    }
    // The dragged wire's type decides which nodes fit.
    GraphPinType dragType = GraphPinType::Any;
    if (st.addHasLink)
        if (const GraphNode* src = st.graph.FindNode(st.addLinkNode))
            if (const GraphNodeDef* sd = FindGraphNode(src->type))
                if (const GraphPinDef* sp = st.addLinkFromOutput ? sd->Output(st.addLinkPin) : sd->Input(st.addLinkPin))
                    dragType = st.graph.PinType(*src, *sp);
    auto matches = [&](StringView title) {
        if (st.addSearch.Empty())
            return true;
        for (usize i = 0; i + st.addSearch.Size() <= title.Size(); ++i)
            if (title.SubStr(i, st.addSearch.Size()).EqualsIgnoreCase(st.addSearch))
                return true;
        return false;
    };
    auto create = [&](StringView type, StringView variable) {
        PushUndo(st);
        GraphNode& n = AddNode(st.graph, type, st.addPosition);
        if (!variable.Empty())
            n.settings.InsertOrAssign("variable", String(variable));
        const u32 id = n.id;
        if (st.addHasLink)
            if (const GraphNodeDef* d = FindGraphNode(type))
                if (const GraphPinDef* p = CompatiblePin(st.graph, *st.graph.FindNode(id), *d, dragType, st.addLinkFromOutput)) {
                    if (st.addLinkFromOutput)
                        Connect(st.graph, st.addLinkNode, st.addLinkPin, id, p->name);
                    else
                        Connect(st.graph, id, p->name, st.addLinkNode, st.addLinkPin);
                }
        st.selection.Clear();
        st.selection.PushBack(id);
        Changed(st);
        ui::CloseCurrentPopup();
    };
    // Enter takes the first match.
    const bool enter = ui::IsKeyPressed(Key::Enter, false);
    bool taken = false;
    auto first = [&](StringView type, StringView variable) {
        if (enter && !taken) {
            taken = true;
            create(type, variable);
        }
    };
    // Variables first.
    for (const GraphVariable& v : st.graph.variables) {
        if (matches(String::Format("Get {}", v.name)))
            first("pure.getVariable", v.name);
        const String get = String::Format("Get {}", v.name), set = String::Format("Set {}", v.name);
        if (matches(get) && ui::MenuItem(String::Format("   " ZX_ICON_VARIABLE "  {}##zgget{}", get, v.name)))
            create("pure.getVariable", v.name);
        if (matches(set) && (!st.addHasLink || dragType == GraphPinType::Exec || !st.addLinkFromOutput || true) &&
            ui::MenuItem(String::Format("   " ZX_ICON_VARIABLE "  {}##zgset{}", set, v.name)))
            create("action.setVariable", v.name);
    }
    StringView category;
    for (const GraphNodeDef& d : GraphNodeLibrary()) {
        if (!matches(d.title))
            continue;
        if (d.id == StringView("pure.getVariable") || d.id == StringView("action.setVariable"))
            continue; // listed per variable above
        if (st.addHasLink) {
            GraphNode probe;
            probe.type = d.id;
            if (!CompatiblePin(st.graph, probe, d, dragType, st.addLinkFromOutput))
                continue;
        }
        first(d.id, {});
        if (category != d.category) {
            category = d.category;
            ui::TextDisabled(category);
        }
        if (ui::MenuItem(String::Format("   {}##{}", d.title, d.id)))
            create(d.id, {});
        if (ui::IsItemHovered() && d.tooltip && *d.tooltip)
            ui::SetTooltip(d.tooltip);
    }
    ui::EndChild();
    ui::EndPopup();
}

// --- Side panels ---------------------------------------------------------------------------------------------------------------

/// Text fields push one undo step per edit (not per keystroke).
void BeginEdit(ZGraphEditorState& st, StringView field)
{
    if (st.editField != field) {
        PushUndo(st);
        st.editField = String(field);
    }
}

void VariablesPanel(Editor& ed, ZGraphEditorState& st)
{
    const bool live = ed.IsPlaying() && ed.scripts && st.debugEntity;
    const String relative = Path::MakeRelative(st.path, ed.contentRoot);
    static const StringView TypeNames[] = {"Number", "Bool", "String", "Vector", "Entity"};
    static const GraphPinType Types[] = {GraphPinType::Number, GraphPinType::Bool, GraphPinType::String, GraphPinType::Vec3, GraphPinType::Entity};
    ui::SeparatorText("Variables");
    i32 remove = -1;
    for (usize i = 0; i < st.graph.variables.Size(); ++i) {
        GraphVariable& v = st.graph.variables[i];
        ui::PushID(i32(i));
        String name = v.name;
        ui::SetNextItemWidth(-30.0f);
        if (ui::InputText("##name", name)) {
            BeginEdit(st, String::Format("varname{}", i));
            // Nodes using the old name follow the rename.
            for (GraphNode& n : st.graph.nodes)
                if (String* s = n.settings.Find("variable"); s && *s == v.name)
                    *s = name;
            v.name = name;
            Changed(st);
        }
        ui::SameLine(0.0f, 4.0f);
        if (ui::IconButton(ZX_ICON_TRASH_2, "Remove this variable"))
            remove = i32(i);
        i32 type = 0;
        for (u32 t = 0; t < 5; ++t)
            if (Types[t] == v.type)
                type = i32(t);
        ui::SetNextItemWidth(90.0f);
        if (ui::Combo("##type", &type, Span<const StringView>(TypeNames, 5))) {
            PushUndo(st);
            v.type = Types[type];
            v.value = v.type == GraphPinType::Bool ? String("false") : v.type == GraphPinType::Vec3 ? String("0 0 0") : v.type == GraphPinType::Number ? String("0") : String();
            Changed(st);
        }
        ui::SameLine(0.0f, 4.0f);
        if (v.type == GraphPinType::Bool) {
            bool on = v.value == "true";
            if (ui::Checkbox("##value", &on)) {
                PushUndo(st);
                v.value = on ? "true" : "false";
                Changed(st);
            }
        } else if (v.type != GraphPinType::Entity) {
            ui::SetNextItemWidth(-1.0f);
            if (ui::InputText("##value", v.value, ui::InputTextFlags::None, "default")) {
                BeginEdit(st, String::Format("varvalue{}", i));
                Changed(st);
            }
        }
        String current;
        if (live && ed.scripts->ReadField(st.debugEntity, relative, v.name, current))
            ui::TextColored(ui::GetCurrentContext()->GetStyle().colors.accentHovered, String::Format(ZX_ICON_ACTIVITY "  now: {}", current));
        if (v.type != GraphPinType::Entity) {
            bool exposed = v.exposed;
            if (ui::Checkbox("Exposed", &exposed)) {
                PushUndo(st);
                v.exposed = exposed;
                Changed(st);
            }
            ui::ItemTooltip("Editable per entity in Details (like a script's prop)");
        }
        ui::Separator();
        ui::PopID();
    }
    if (remove >= 0) {
        PushUndo(st);
        st.graph.variables.RemoveAt(usize(remove));
        Changed(st);
    }
    if (ui::Button(ZX_ICON_PLUS "  Variable", Vec2(-1.0f, 0.0f))) {
        PushUndo(st);
        String name = "value";
        for (u32 n = 2; st.graph.FindVariable(name); ++n)
            name = String::Format("value{}", n);
        st.graph.variables.PushBack(GraphVariable{name, GraphPinType::Number, "0", false, {}});
        Changed(st);
    }
    ui::Spacing();
    ui::TextDisabled("Add Get / Set nodes from the right-click menu.");
}

void DetailsPanel(ZGraphEditorState& st)
{
    const ui::Theme& th = ui::GetCurrentContext()->GetStyle().colors;
    ui::SeparatorText("Node");
    if (st.selection.Size() != 1) {
        ui::TextDisabled(st.selection.Empty() ? "Select a node to edit it" : String::Format("{} nodes selected", st.selection.Size()));
    } else if (GraphNode* n = st.graph.FindNode(st.selection[0])) {
        const GraphNodeDef* d = FindGraphNode(n->type);
        ui::Text(d ? d->title : n->type.View());
        if (d && d->tooltip && *d->tooltip)
            ui::TextWrapped(d->tooltip);
        ui::Spacing();
        if (d) {
            for (const GraphSettingDef& sdef : d->settings) {
                if (!sdef.name)
                    continue;
                ui::PushID(sdef.name);
                const String* cur = n->settings.Find(sdef.name);
                String value = cur ? *cur : String(sdef.defaultValue);
                ui::AlignTextToFramePadding();
                ui::Text(PinLabel(sdef.name));
                ui::SameLine(90.0f);
                ui::SetNextItemWidth(-1.0f);
                if (StringView(sdef.name) == "variable") {
                    Array<StringView> names;
                    i32 at = -1;
                    for (usize i = 0; i < st.graph.variables.Size(); ++i) {
                        names.PushBack(st.graph.variables[i].name.View());
                        if (st.graph.variables[i].name == value)
                            at = i32(i);
                    }
                    if (ui::Combo("##var", &at, names.View()) && at >= 0) {
                        PushUndo(st);
                        n->settings.InsertOrAssign("variable", st.graph.variables[usize(at)].name);
                        Changed(st);
                    }
                } else if (ui::InputText("##set", value)) {
                    BeginEdit(st, String::Format("set{}{}", n->id, sdef.name));
                    n->settings.InsertOrAssign(sdef.name, value);
                    Changed(st);
                }
                ui::PopID();
            }
            for (u32 i = 0; i < d->InputCount(); ++i) {
                const GraphPinDef& pin = d->inputs[i];
                if (pin.type == GraphPinType::Exec)
                    continue;
                ui::PushID(pin.name);
                ui::AlignTextToFramePadding();
                ui::Text(PinLabel(pin.name));
                ui::SameLine(90.0f);
                if (st.graph.LinkInto(n->id, pin.name)) {
                    ui::TextDisabled(ZX_ICON_LINK "  wired");
                } else {
                    const String* cur = n->values.Find(pin.name);
                    String value = cur ? *cur : String(pin.defaultValue);
                    const GraphPinType type = st.graph.PinType(*n, pin);
                    if (type == GraphPinType::Bool) {
                        bool on = value.EqualsIgnoreCase("true");
                        if (ui::Checkbox("##v", &on)) {
                            PushUndo(st);
                            n->values.InsertOrAssign(pin.name, on ? "true" : "false");
                            Changed(st);
                        }
                    } else {
                        ui::SetNextItemWidth(-1.0f);
                        if (ui::InputText("##v", value, ui::InputTextFlags::None,
                                          type == GraphPinType::Entity ? "self, or an entity name" : type == GraphPinType::Vec3 ? "x y z" : "")) {
                            BeginEdit(st, String::Format("val{}{}", n->id, pin.name));
                            n->values.InsertOrAssign(pin.name, value);
                            Changed(st);
                        }
                    }
                }
                ui::PopID();
            }
        }
    }
    // Problems.
    if (!st.compiled.errors.Empty() || !st.compiled.warnings.Empty()) {
        ui::SeparatorText("Problems");
        ui::PushTextColor(th.error);
        for (const String& e : st.compiled.errors)
            ui::TextWrapped(e);
        ui::PopTextColor();
        ui::PushTextColor(th.warning);
        for (const String& w : st.compiled.warnings)
            ui::TextWrapped(w);
        ui::PopTextColor();
    }
    if (st.showCode) {
        ui::SeparatorText("Generated ZeqouScript");
        String code = st.compiled.source;
        ui::InputTextMultiline("##zg_code", code, Vec2(-1.0f, 360.0f), ui::InputTextFlags::ReadOnly);
    }
}

} // namespace

void ZGraphTrace(Editor& ed, StringView file, ecs::Entity entity, u32 node)
{
    ZGraphEditorState& st = ed.graphEditor;
    if (!st.open || !Path::MakeRelative(st.path, ed.contentRoot).EqualsIgnoreCase(file))
        return;
    if (!st.debugEntity)
        st.debugEntity = entity; // the first one that runs
    if (entity != st.debugEntity)
        return;
    st.hits.InsertOrAssign(node, Clock::NowSeconds());
    if (st.breakpoints.Contains(node) && !st.breakPending && ed.playState == PlayState::Playing) {
        st.breakPending = true;
        st.breakNode = node;
    }
}

void ZGraphAfterTick(Editor& ed)
{
    ZGraphEditorState& st = ed.graphEditor;
    if (!st.breakPending)
        return;
    st.breakPending = false;
    if (ed.playState == PlayState::Playing) {
        ed.TogglePause();
        const GraphNode* n = st.graph.FindNode(st.breakNode);
        const GraphNodeDef* d = n ? FindGraphNode(n->type) : nullptr;
        ZX_LOG_INFO(LogGraphEditor, "Breakpoint: {} '{}' (#{}) - F6 continues, F10 steps a frame", Path::FileName(st.path), d ? d->title : "?", st.breakNode);
    }
}

bool CreateZGraphFile(StringView physicalPath)
{
    Graph g;
    g.name = String(Path::Stem(physicalPath));
    GraphNode start;
    start.id = 1;
    start.type = "event.start";
    start.position = Vec2(0, 0);
    g.nodes.PushBack(start);
    GraphNode update;
    update.id = 2;
    update.type = "event.update";
    update.position = Vec2(0, 140);
    g.nodes.PushBack(update);
    return fs::WriteTextFile(physicalPath, WriteGraph(g));
}

void OpenZGraphEditor(Editor& ed, StringView physicalPath)
{
    ZGraphEditorState& st = ed.graphEditor;
    if (st.open && st.dirty && st.path != physicalPath)
        Save(st); // switching graphs keeps the work
    String text;
    Graph g;
    String error;
    if (!fs::ReadTextFile(physicalPath, text) || !ParseGraph(text, g, &error)) {
        ZX_LOG_ERROR(LogGraphEditor, "Can't open {}: {}", physicalPath, error);
        return;
    }
    const bool same = st.path == physicalPath;
    Array<u32> breakpoints = same ? st.breakpoints : Array<u32>();
    st = ZGraphEditorState();
    st.breakpoints = move(breakpoints);
    st.open = true;
    st.path = String(physicalPath);
    st.graph = move(g);
    st.frameRequested = true;
    Recompile(st);
}

void DrawZGraphEditorPanel(Editor& ed)
{
    ZGraphEditorState& st = ed.graphEditor;
    st.capturesKeys = false;
    if (!st.open)
        return;
    ui::Context& ctx = *ui::GetCurrentContext();
    const f32 s = ctx.GetScale();
    const ui::Theme& th = ctx.GetStyle().colors;
    {
        const Vec2 display = Vec2(f32(ed.swapChain->GetSize().x), f32(ed.swapChain->GetSize().y)) / s;
        const Vec2 size = display * 0.85f;
        ui::SetNextWindowSize(size, ui::Cond::FirstUse);
        ui::SetNextWindowPos((display - size) * 0.5f, ui::Cond::FirstUse);
    }
    bool open = true;
    if (!ui::Begin(ZGraphEditorTitle, &open, ui::WindowFlags::NoScrollbar)) {
        ui::End();
        if (!open && st.dirty)
            Save(st);
        st.open = open;
        return;
    }
    if (!open && st.dirty)
        Save(st); // closing keeps the work
    st.open = open;

    // --- Toolbar ---
    if (ui::Button(ZX_ICON_SAVE "  Save", Vec2::Zero(), st.dirty ? ui::ButtonVariant::Primary : ui::ButtonVariant::Default))
        Save(st);
    ui::SameLine();
    ui::AlignTextToFramePadding();
    ui::Text(String::Format("{}{}", Path::FileName(st.path), st.dirty ? "*" : ""));
    ui::SameLine(0.0f, 16.0f);
    if (!st.compiled.errors.Empty())
        ui::TextColored(th.error, String::Format(ZX_ICON_CIRCLE_X "  {} error{}", st.compiled.errors.Size(), st.compiled.errors.Size() == 1 ? "" : "s"));
    else
        ui::TextColored(th.success, String::Format(ZX_ICON_CHECK "  Compiles ({} nodes)", st.graph.nodes.Size()));
    if (!st.compiled.warnings.Empty()) {
        ui::SameLine(0.0f, 12.0f);
        ui::TextColored(th.warning, String::Format(ZX_ICON_TRIANGLE_ALERT "  {}", st.compiled.warnings.Size()));
        ui::ItemTooltip(st.compiled.warnings[0]);
    }
    ui::SameLine(0.0f, 16.0f);
    ui::Checkbox("Show Code", &st.showCode);
    ui::ItemTooltip("The ZeqouScript this graph compiles to");
    ui::SameLine(0.0f, 8.0f);
    if (ui::Button(ZX_ICON_SCAN "  Frame"))
        st.frameRequested = true;
    // Debugging while playing: whose graph is shown, and where it stopped.
    const String relative = Path::MakeRelative(st.path, ed.contentRoot);
    if (!ed.IsPlaying()) {
        st.hits.Clear();
        st.debugEntity = {};
        st.breakNode = 0;
    } else if (ed.scripts) {
        Array<ecs::Entity> running;
        ed.scripts->InstancesOf(relative, running);
        ui::SameLine(0.0f, 16.0f);
        if (running.Empty()) {
            ui::TextDisabled(ZX_ICON_BUG "  Not running on any entity");
        } else {
            if (!running.Contains(st.debugEntity))
                st.debugEntity = running[0];
            Array<String> names;
            Array<StringView> items;
            i32 current = 0;
            for (usize i = 0; i < running.Size(); ++i) {
                const scene::EntityName* n = ed.world.Get<const scene::EntityName>(running[i]);
                names.PushBack(String::Format(ZX_ICON_BUG "  {}", n ? n->value.View() : StringView("Entity")));
                if (running[i] == st.debugEntity)
                    current = i32(i);
            }
            for (const String& n : names)
                items.PushBack(n.View());
            ui::SetNextItemWidth(170.0f);
            if (ui::Combo("##zg_debug", &current, items.View()) && current >= 0) {
                st.debugEntity = running[usize(current)];
                st.hits.Clear();
            }
            ui::ItemTooltip("The entity whose graph is shown running (highlights, variable values, breakpoints)");
        }
        if (ed.playState == PlayState::Paused && st.breakNode) {
            const GraphNode* n = st.graph.FindNode(st.breakNode);
            const GraphNodeDef* d = n ? FindGraphNode(n->type) : nullptr;
            ui::SameLine(0.0f, 12.0f);
            ui::TextColored(th.error, String::Format(ZX_ICON_CIRCLE_PAUSE "  Breakpoint: {} (#{})   F6 continue, F10 next frame", d ? d->title : "?", st.breakNode));
        }
    }

    // --- Layout: variables | canvas | details ---
    const Rect content = ui::GetWindowContentRect();
    const f32 leftW = Round(230.0f * s), rightW = Round(300.0f * s), gap = Round(6.0f * s);
    const f32 top = ui::GetCursorPos().y + Round(4.0f * s);
    const Rect leftRect(Vec2(content.min.x, top), Vec2(content.min.x + leftW, content.max.y));
    const Rect canvasRect(Vec2(leftRect.max.x + gap, top), Vec2(content.max.x - rightW - gap, content.max.y));
    const Rect rightRect(Vec2(canvasRect.max.x + gap, top), content.max);
    Canvas c{canvasRect, st.pan, st.zoom * s};
    if (st.frameRequested && canvasRect.Width() > 50.0f) {
        FrameAll(st, canvasRect, s);
        c.pan = st.pan;
        c.scale = st.zoom * s;
        st.frameRequested = false;
    }
    if (!ctx.IsAnyItemActive())
        st.editField.Clear();

    ui::DrawList& dl = ui::GetWindowDrawList();
    ui::SetCursorPos(canvasRect.min);
    ui::InvisibleButton("##zg_canvas", canvasRect.Size() / s);
    const bool hovered = ui::IsItemHovered() || ui::IsItemActive();
    const Vec2 mouse = ui::GetMousePos();
    dl.PushClipRect(canvasRect);
    DrawGrid(dl, c, s);

    // Hit testing: pins first, then the topmost node.
    u32 hotNode = 0;
    String hotPin;
    bool hotPinOutput = false, hotIsPin = false;
    for (usize i = st.graph.nodes.Size(); i > 0 && hovered; --i) {
        const GraphNode& n = st.graph.nodes[i - 1];
        const GraphNodeDef* d = FindGraphNode(n.type);
        const f32 pr = (PinRadius + 5.0f) * c.scale;
        if (d) {
            for (u32 p = 0; p < d->OutputCount() && !hotIsPin; ++p)
                if (Length(PinPos(c, n, p, true) - mouse) < pr) {
                    hotNode = n.id, hotPin = d->outputs[p].name, hotPinOutput = true, hotIsPin = true;
                }
            for (u32 p = 0; p < d->InputCount() && !hotIsPin; ++p)
                if (Length(PinPos(c, n, p, false) - mouse) < pr) {
                    hotNode = n.id, hotPin = d->inputs[p].name, hotPinOutput = false, hotIsPin = true;
                }
        }
        if (hotIsPin)
            break;
        const Rect r(c.ToScreen(n.position), c.ToScreen(n.position + Vec2(NodeWidth, NodeHeight(d))));
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
            if (alt) {
                PushUndo(st);
                Disconnect(st.graph, hotNode, hotPin, hotPinOutput);
                Changed(st);
            } else if (const GraphLink* link = hotPinOutput ? nullptr : st.graph.LinkInto(hotNode, hotPin);
                       link && st.graph.FindNode(hotNode) && FindGraphNode(st.graph.FindNode(hotNode)->type) &&
                       FindGraphNode(st.graph.FindNode(hotNode)->type)->Input(hotPin)->type != GraphPinType::Exec) {
                // Grab a data wire by its input end: re-route it from its source.
                st.beforeDrag = Snapshot(st.graph);
                st.linkNode = link->fromNode;
                st.linkPin = link->fromPin;
                st.linkFromOutput = true;
                Disconnect(st.graph, hotNode, hotPin, false);
                st.drag = ZGraphEditorState::Drag::Link;
            } else {
                st.beforeDrag = Snapshot(st.graph);
                st.linkNode = hotNode;
                st.linkPin = hotPin;
                st.linkFromOutput = hotPinOutput;
                st.drag = ZGraphEditorState::Drag::Link;
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
            // Clicked nodes come to the front.
            for (usize i = 0; i < st.graph.nodes.Size(); ++i)
                if (st.graph.nodes[i].id == hotNode && i + 1 < st.graph.nodes.Size()) {
                    GraphNode moved = move(st.graph.nodes[i]);
                    st.graph.nodes.RemoveAt(i);
                    st.graph.nodes.PushBack(move(moved));
                    break;
                }
            st.beforeDrag = Snapshot(st.graph);
            st.drag = ZGraphEditorState::Drag::Nodes;
        } else {
            if (!ctrl)
                st.selection.Clear();
            st.drag = ZGraphEditorState::Drag::Box;
        }
    }
    if ((hovered || st.drag == ZGraphEditorState::Drag::Pan) &&
        (ed.input.WasMousePressed(MouseButton::Right) || ed.input.WasMousePressed(MouseButton::Middle))) {
        st.rightPress = mouse;
        st.dragMoved = false;
        st.drag = ZGraphEditorState::Drag::Pan;
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
    case ZGraphEditorState::Drag::Nodes:
        if (ed.input.IsMouseDown(MouseButton::Left)) {
            if (Length(mouse - st.dragStart) > 3.0f * s)
                st.dragMoved = true;
            if (st.dragMoved)
                for (u32 id : st.selection)
                    if (GraphNode* n = st.graph.FindNode(id))
                        n->position += delta / c.scale;
        } else {
            if (st.dragMoved) {
                for (u32 id : st.selection)
                    if (GraphNode* n = st.graph.FindNode(id))
                        n->position = Round(n->position / 8.0f) * 8.0f; // tidy grid
                st.undo.PushBack(st.beforeDrag);
                st.redo.Clear();
                st.dirty = true;
            }
            st.beforeDrag.Clear();
            st.drag = ZGraphEditorState::Drag::None;
        }
        break;
    case ZGraphEditorState::Drag::Box: {
        const Rect box(Min(st.dragStart, mouse), Max(st.dragStart, mouse));
        if (ed.input.IsMouseDown(MouseButton::Left)) {
            dl.AddRect(box, WithAlpha(th.accent, 0.12f), ui::CornerRadii(), 1.0f, WithAlpha(th.accentHovered, 0.8f));
        } else {
            if (box.Width() > 3.0f || box.Height() > 3.0f)
                for (const GraphNode& n : st.graph.nodes) {
                    const Rect r(c.ToScreen(n.position), c.ToScreen(n.position + Vec2(NodeWidth, NodeHeight(FindGraphNode(n.type)))));
                    if (r.Intersects(box) && !st.selection.Contains(n.id))
                        st.selection.PushBack(n.id);
                }
            st.drag = ZGraphEditorState::Drag::None;
        }
        break;
    }
    case ZGraphEditorState::Drag::Link:
        if (!ed.input.IsMouseDown(MouseButton::Left)) {
            bool connected = false;
            if (hotIsPin && hotNode != st.linkNode && hotPinOutput != st.linkFromOutput)
                connected = st.linkFromOutput ? Connect(st.graph, st.linkNode, st.linkPin, hotNode, hotPin)
                                              : Connect(st.graph, hotNode, hotPin, st.linkNode, st.linkPin);
            if (connected || Snapshot(st.graph) != st.beforeDrag) {
                st.undo.PushBack(st.beforeDrag);
                st.redo.Clear();
                Changed(st);
            }
            if (!connected && !hotIsPin && Length(mouse - st.dragStart) > 6.0f * s) {
                // Dropped on empty canvas: pick a node to connect.
                st.addHasLink = true;
                st.addLinkNode = st.linkNode;
                st.addLinkPin = st.linkPin;
                st.addLinkFromOutput = st.linkFromOutput;
                st.addPosition = c.ToGraph(mouse) - (st.linkFromOutput ? Vec2(0.0f, HeaderHeight) : Vec2(NodeWidth, HeaderHeight));
                st.addSearch.Clear();
                st.addFocus = true;
                ui::OpenPopup("##zg_add_node");
            }
            st.beforeDrag.Clear();
            st.drag = ZGraphEditorState::Drag::None;
        }
        break;
    case ZGraphEditorState::Drag::Pan:
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
                    ui::OpenPopup("##zg_node_menu");
                } else {
                    st.addHasLink = false;
                    st.addPosition = c.ToGraph(mouse);
                    st.addSearch.Clear();
                    st.addFocus = true;
                ui::OpenPopup("##zg_add_node");
                }
            }
            st.drag = ZGraphEditorState::Drag::None;
        }
        break;
    case ZGraphEditorState::Drag::None: break;
    }

    // --- Wires ---
    for (const GraphLink& l : st.graph.links) {
        const GraphNode* a = st.graph.FindNode(l.fromNode);
        const GraphNode* b = st.graph.FindNode(l.toNode);
        if (!a || !b)
            continue;
        const GraphNodeDef* da = FindGraphNode(a->type);
        const GraphNodeDef* db = FindGraphNode(b->type);
        const i32 pa = PinIndex(da, l.fromPin, true), pb = PinIndex(db, l.toPin, false);
        if (pa < 0 || pb < 0)
            continue;
        const GraphPinType type = st.graph.PinType(*a, da->outputs[pa]);
        const bool exec = type == GraphPinType::Exec;
        const f32 glow = exec ? Min(Glow(st, a->id), Glow(st, b->id)) : Glow(st, b->id);
        const Vec2 from = PinPos(c, *a, u32(pa), true), to = PinPos(c, *b, u32(pb), false);
        if (glow > 0.0f) // the wire the game just went along
            DrawWire(dl, from, to, Color(0xFF, 0xC8, 0x3C, u8(glow * 200.0f)), c.scale * 2.2f, exec);
        DrawWire(dl, from, to, WithAlpha(TypeColor(type), 0.9f), c.scale, exec);
    }
    if (st.drag == ZGraphEditorState::Drag::Link)
        if (const GraphNode* n = st.graph.FindNode(st.linkNode)) {
            const i32 pi = PinIndex(FindGraphNode(n->type), st.linkPin, st.linkFromOutput);
            if (pi >= 0) {
                const Vec2 p = PinPos(c, *n, u32(pi), st.linkFromOutput);
                if (st.linkFromOutput)
                    DrawWire(dl, p, mouse, th.accentHovered, c.scale, false);
                else
                    DrawWire(dl, mouse, p, th.accentHovered, c.scale, false);
            }
        }

    // --- Nodes ---
    const bool paused = ed.playState == PlayState::Paused;
    if (paused && st.frozenAt <= 0.0)
        st.frozenAt = Clock::NowSeconds();
    else if (!paused)
        st.frozenAt = 0.0;
    for (const GraphNode& n : st.graph.nodes)
        DrawNode(st.graph, dl, c, n, st.selection.Contains(n.id), st.compiled.errorNodes.Contains(n.id), ed.IsPlaying() ? Glow(st, n.id) : 0.0f,
                 st.breakpoints.Contains(n.id), paused && st.breakNode == n.id);
    if (hotIsPin)
        if (const GraphNode* n = st.graph.FindNode(hotNode)) {
            const i32 pi = PinIndex(FindGraphNode(n->type), hotPin, hotPinOutput);
            if (pi >= 0)
                dl.AddCircleOutline(PinPos(c, *n, u32(pi), hotPinOutput), (PinRadius + 4.0f) * c.scale, th.accentHovered, 1.5f);
        }
    if (st.graph.nodes.Empty())
        dl.AddTextInRect(ctx.Fonts().FindFont("Inter"), 14.0f * s, canvasRect, th.textDisabled, "Right click to add a node", ui::TextAlign::Center);
    dl.AddTextInRect(ctx.Fonts().FindFont("Inter"), 11.0f * s,
                     Rect(Vec2(canvasRect.min.x + 10.0f * s, canvasRect.max.y - 24.0f * s), canvasRect.max - Vec2(10.0f * s, 4.0f * s)), th.textDisabled,
                     "Right click: add node   Drag a pin: connect (drop on empty space: pick a node)   Alt+click a pin: disconnect   Wheel: zoom   "
                     "RMB drag: pan   F: frame");
    dl.PopClipRect();

    // --- Popups ---
    AddNodePopup(st);
    if (ui::BeginPopup("##zg_node_menu")) {
        if (ui::MenuItem(ZX_ICON_COPY "  Duplicate", "Ctrl+D"))
            Paste(st, CopySelection(st), c.ToGraph(st.rightPress) + Vec2(24.0f));
        if (ui::MenuItem(ZX_ICON_CLIPBOARD_COPY "  Copy", "Ctrl+C"))
            st.clipboard = CopySelection(st);
        if (ui::MenuItem(ZX_ICON_CIRCLE_DOT "  Toggle Breakpoint", "F9"))
            for (u32 id : st.selection) {
                if (st.breakpoints.Contains(id))
                    st.breakpoints.Remove(id);
                else
                    st.breakpoints.PushBack(id);
            }
        if (ui::MenuItem(ZX_ICON_UNLINK "  Break Links")) {
            PushUndo(st);
            for (u32 id : st.selection)
                st.graph.links.RemoveIf([&](const GraphLink& l) { return l.fromNode == id || l.toNode == id; });
            Changed(st);
        }
        if (ui::MenuItem(ZX_ICON_TRASH_2 "  Delete", "Del"))
            DeleteSelection(st);
        ui::EndPopup();
    }

    // --- Keyboard (while the canvas is hovered) ---
    st.capturesKeys = hovered && !ctx.WantsKeyboard();
    if (st.capturesKeys) {
        if (ui::IsKeyPressed(Key::Delete, false) || ui::IsKeyPressed(Key::Backspace, false))
            DeleteSelection(st);
        if (ui::IsShortcutPressed(KeyModifiers::Control, Key::C))
            st.clipboard = CopySelection(st);
        if (ui::IsShortcutPressed(KeyModifiers::Control, Key::V) && !st.clipboard.Empty())
            Paste(st, st.clipboard, c.ToGraph(mouse));
        if (ui::IsShortcutPressed(KeyModifiers::Control, Key::D))
            Paste(st, CopySelection(st), c.ToGraph(mouse));
        if (ui::IsShortcutPressed(KeyModifiers::Control, Key::A)) {
            st.selection.Clear();
            for (const GraphNode& n : st.graph.nodes)
                st.selection.PushBack(n.id);
        }
        if (ui::IsKeyPressed(Key::F, false))
            st.frameRequested = true;
        if (ui::IsKeyPressed(Key::F9, false)) // breakpoints on the selected nodes
            for (u32 id : st.selection) {
                if (st.breakpoints.Contains(id))
                    st.breakpoints.Remove(id);
                else
                    st.breakpoints.PushBack(id);
            }
    }
    if (ui::IsWindowHovered() || st.capturesKeys) {
        if (ui::IsShortcutPressed(KeyModifiers::Control, Key::S))
            Save(st);
        const bool undo = ui::IsShortcutPressed(KeyModifiers::Control, Key::Z) && !ctx.WantsKeyboard();
        const bool redo = (ui::IsShortcutPressed(KeyModifiers::Control, Key::Y) || ui::IsShortcutPressed(KeyModifiers::Control | KeyModifiers::Shift, Key::Z)) &&
                          !ctx.WantsKeyboard();
        if ((undo && !st.undo.Empty()) || (redo && !st.redo.Empty())) {
            Array<String>& from = undo ? st.undo : st.redo;
            Array<String>& to = undo ? st.redo : st.undo;
            to.PushBack(Snapshot(st.graph));
            ParseGraph(from.Back(), st.graph);
            from.PopBack();
            st.selection.Clear();
            Changed(st);
        }
        st.capturesKeys = true; // the scene's Ctrl+Z / Delete stay out of the graph window
    }

    // --- Side panels ---
    ui::SetCursorPos(leftRect.min);
    if (ui::BeginChild("##zg_left", leftRect.Size() / s, false))
        VariablesPanel(ed, st);
    ui::EndChild();
    ui::SetCursorPos(rightRect.min);
    if (ui::BeginChild("##zg_right", rightRect.Size() / s, false))
        DetailsPanel(st);
    ui::EndChild();
    ui::End();
}

} // namespace zx::editor
