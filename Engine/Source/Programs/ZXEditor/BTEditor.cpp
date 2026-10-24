#include "BTEditor.h"
#include "Editor.h"
#include "Core/FileSystem/FileSystem.h"
#include "Core/String/Path.h"

ZX_DEFINE_LOG_CATEGORY_STATIC(LogBTEditor, Info);

namespace zx::editor {

namespace {

// Node layout in tree units (1 unit = 1 logical pixel at zoom 1).
constexpr f32 NodeWidth = 168.0f;
constexpr f32 NodeHeight = 52.0f;
constexpr f32 HeaderHeight = 24.0f;
constexpr f32 PinRadius = 6.0f;
constexpr usize MaxUndo = 100;

struct Canvas
{
    Rect rect;
    Vec2 pan;
    f32 scale = 1.0f;
    Vec2 ToScreen(Vec2 p) const { return rect.min + (p + pan) * scale; }
    Vec2 ToGraph(Vec2 q) const { return (q - rect.min) / scale - pan; }
};

struct TypeInfo
{
    BTNodeType type;
    const char* icon;
    const char* category;
    const char* tooltip;
};
constexpr TypeInfo Types[] = {
    {BTNodeType::Selector, ZX_ICON_GIT_FORK, "Composites", "Runs the first child that doesn't fail. Higher (left) children take over from a running lower one as soon as they can run"},
    {BTNodeType::Sequence, ZX_ICON_LIST_ORDERED, "Composites", "Runs the children left to right until one fails. Conditions before the running child are checked again every tick"},
    {BTNodeType::Parallel, ZX_ICON_COLUMNS_2, "Composites", "Runs all children at once; succeeds when Count of them succeeded (0 = all), fails when one fails"},
    {BTNodeType::Inverter, ZX_ICON_ARROW_RIGHT_LEFT, "Decorators", "Turns success into failure and back"},
    {BTNodeType::Succeeder, ZX_ICON_CHECK_CHECK, "Decorators", "Always succeeds when the child finishes"},
    {BTNodeType::Repeat, ZX_ICON_REPEAT, "Decorators", "Runs the child again and again (Count times, 0 = forever); stops when it fails"},
    {BTNodeType::Cooldown, ZX_ICON_TIMER, "Decorators", "After the child finished, fails for Seconds before it may run again"},
    {BTNodeType::Wait, ZX_ICON_HOURGLASS, "Tasks", "Waits Seconds (+- Deviation at random)"},
    {BTNodeType::MoveTo, ZX_ICON_FOOTPRINTS, "Tasks", "Walks to the blackboard Key (a point or an entity, followed as it moves) with the Nav Mesh Agent"},
    {BTNodeType::Stop, ZX_ICON_OCTAGON_X, "Tasks", "Stops walking"},
    {BTNodeType::Condition, ZX_ICON_CIRCLE_HELP, "Tasks", "Succeeds if the blackboard Key compares to Value (set / notset / == != < > <= >=)"},
    {BTNodeType::SetValue, ZX_ICON_PENCIL, "Tasks", "Writes Value into the blackboard Key"},
    {BTNodeType::ClearValue, ZX_ICON_ERASER, "Tasks", "Removes the blackboard Key"},
    {BTNodeType::RandomPoint, ZX_ICON_DICES, "Tasks", "Writes a random reachable point within Radius into Key (wandering)"},
    {BTNodeType::NextWaypoint, ZX_ICON_FLAG, "Tasks", "Writes the next child of the entity named Text (a waypoint group) into Key"},
    {BTNodeType::FaceTarget, ZX_ICON_CROSSHAIR, "Tasks", "Turns to face Key"},
    {BTNodeType::Event, ZX_ICON_ZAP, "Tasks", "Calls fn onAIEvent(Text) in the entity's scripts"},
    {BTNodeType::Log, ZX_ICON_MESSAGE_SQUARE, "Tasks", "Writes Text to the log"},
};

const TypeInfo& InfoOf(BTNodeType t)
{
    for (const TypeInfo& i : Types)
        if (i.type == t)
            return i;
    return Types[0];
}

bool HasOutput(BTNodeType t) { return BTIsComposite(t) || BTIsDecorator(t); }

Color HeaderColor(BTNodeType t)
{
    if (BTIsComposite(t))
        return Color(0x4A, 0x55, 0x68);
    if (BTIsDecorator(t))
        return Color(0x6B, 0x4A, 0xB8);
    if (t == BTNodeType::Condition)
        return Color(0x3A, 0x84, 0x4E);
    return Color(0x2F, 0x6A, 0xB0);
}

String ValueText(const BBValue& v)
{
    switch (v.type) {
    case BBType::Bool: return String(v.number != 0.0 ? "true" : "false");
    case BBType::Number: return String::Format("{}", v.number);
    case BBType::Vec3: return String::Format("({}, {}, {})", v.vec.x, v.vec.y, v.vec.z);
    case BBType::String: return String::Format("\"{}\"", v.text);
    case BBType::Entity: return String("entity");
    default: return String("-");
    }
}

/// The node's settings in one line ("targetVisible == true", "2.5 s +- 0.5").
String Summary(const BTNode& n)
{
    switch (n.type) {
    case BTNodeType::Parallel: return n.count > 0 ? String::Format("{} must succeed", n.count) : String("all must succeed");
    case BTNodeType::Repeat: return n.count > 0 ? String::Format("{} times", n.count) : String("forever");
    case BTNodeType::Cooldown: return String::Format("{} s", n.seconds);
    case BTNodeType::Wait: return n.deviation > 0.0f ? String::Format("{} s +- {}", n.seconds, n.deviation) : String::Format("{} s", n.seconds);
    case BTNodeType::MoveTo: return String::Format("to {}  (within {} m)", n.key.Empty() ? StringView("?") : n.key.View(), n.radius);
    case BTNodeType::Condition: {
        const StringView op = n.op.Empty() ? StringView("set") : n.op.View();
        if (op == "set" || op == "notset")
            return String::Format("{} is {}", n.key, op);
        return String::Format("{} {} {}", n.key, op, ValueText(n.value));
    }
    case BTNodeType::SetValue: return String::Format("{} = {}", n.key, ValueText(n.value));
    case BTNodeType::ClearValue: return String::Format("clear {}", n.key);
    case BTNodeType::RandomPoint: return String::Format("{} within {} m", n.key, n.radius);
    case BTNodeType::NextWaypoint: return String::Format("{} of \"{}\"", n.key, n.text);
    case BTNodeType::FaceTarget: return String::Format("face {}", n.key);
    case BTNodeType::Event: return String::Format("onAIEvent(\"{}\")", n.text);
    case BTNodeType::Log: return String::Format("\"{}\"", n.text);
    default: return String();
    }
}

Rect NodeRect(const Canvas& c, const BTNode& n) { return Rect(c.ToScreen(n.editorPosition), c.ToScreen(n.editorPosition + Vec2(NodeWidth, NodeHeight))); }
Vec2 InPin(const Canvas& c, const BTNode& n) { return c.ToScreen(n.editorPosition + Vec2(NodeWidth * 0.5f, 0.0f)); }
Vec2 OutPin(const Canvas& c, const BTNode& n) { return c.ToScreen(n.editorPosition + Vec2(NodeWidth * 0.5f, NodeHeight)); }

String Snapshot(const BehaviorTreeAsset& t) { return WriteBehaviorTree(t); }

void Recheck(BTEditorState& st)
{
    st.problems.Clear();
    CheckBehaviorTree(st.tree, st.problems);
}

void PushUndo(BTEditorState& st)
{
    st.undo.PushBack(Snapshot(st.tree));
    if (st.undo.Size() > MaxUndo)
        st.undo.RemoveAt(0);
    st.redo.Clear();
}

void Changed(BTEditorState& st)
{
    st.dirty = true;
    Recheck(st);
}

i32 ParentOf(const BehaviorTreeAsset& t, u32 node)
{
    for (u32 i = 0; i < t.nodes.Size(); ++i)
        if (t.nodes[i].children.Contains(node))
            return i32(i);
    return -1;
}

bool IsAncestor(const BehaviorTreeAsset& t, u32 ancestor, u32 node)
{
    for (i32 p = ParentOf(t, node); p >= 0; p = ParentOf(t, u32(p)))
        if (u32(p) == ancestor)
            return true;
    return false;
}

/// Children run left to right: keep them ordered by their x position.
void SortChildren(BehaviorTreeAsset& t, u32 parent)
{
    Array<u32>& ch = t.nodes[parent].children;
    for (usize i = 1; i < ch.Size(); ++i)
        for (usize j = i; j > 0 && t.nodes[ch[j]].editorPosition.x < t.nodes[ch[j - 1]].editorPosition.x; --j)
            swap(ch[j], ch[j - 1]);
}

void SortAll(BehaviorTreeAsset& t)
{
    for (u32 i = 0; i < t.nodes.Size(); ++i)
        SortChildren(t, i);
}

/// Makes `child` a child of `parent` (taken from its old parent; a decorator's old child is let go). False if it
/// would make a loop or the parent takes no children.
bool Connect(BehaviorTreeAsset& t, u32 parent, u32 child)
{
    if (parent == child || child == 0 || !HasOutput(t.nodes[parent].type) || IsAncestor(t, child, parent))
        return false;
    if (const i32 old = ParentOf(t, child); old >= 0)
        t.nodes[usize(old)].children.Remove(child);
    if (BTIsDecorator(t.nodes[parent].type))
        t.nodes[parent].children.Clear();
    t.nodes[parent].children.PushBack(child);
    SortChildren(t, parent);
    return true;
}

void Disconnect(BehaviorTreeAsset& t, u32 child)
{
    if (const i32 p = ParentOf(t, child); p >= 0)
        t.nodes[usize(p)].children.Remove(child);
}

/// Removes nodes (never the root) and renumbers the references.
void RemoveNodes(BehaviorTreeAsset& t, Span<const u32> nodes)
{
    Array<i32> remap;
    remap.Resize(t.nodes.Size(), 0);
    Array<BTNode> kept;
    for (u32 i = 0; i < t.nodes.Size(); ++i) {
        const bool gone = i != 0 && [&] {
            for (u32 n : nodes)
                if (n == i)
                    return true;
            return false;
        }();
        remap[i] = gone ? -1 : i32(kept.Size());
        if (!gone)
            kept.PushBack(t.nodes[i]);
    }
    for (BTNode& n : kept) {
        Array<u32> children;
        for (u32 c : n.children)
            if (remap[c] >= 0)
                children.PushBack(u32(remap[c]));
        n.children = move(children);
    }
    t.nodes = move(kept);
}

/// Makes `node` the root (index 0), swapping places with the old root.
void SetRoot(BehaviorTreeAsset& t, u32 node)
{
    if (node == 0 || node >= t.nodes.Size())
        return;
    Disconnect(t, node);
    swap(t.nodes[0], t.nodes[node]);
    for (BTNode& n : t.nodes)
        for (u32& c : n.children)
            c = c == 0 ? node : c == node ? 0 : c;
}

void Save(Editor& ed, BTEditorState& st)
{
    SortAll(st.tree);
    if (fs::WriteTextFile(st.path, WriteBehaviorTree(st.tree))) {
        st.dirty = false;
        ed.behavior.Reload(Path::MakeRelative(st.path, ed.contentRoot)); // running AIs pick it up on the next tick
        ZX_LOG_INFO(LogBTEditor, "Saved {}", Path::FileName(st.path));
    } else {
        ZX_LOG_ERROR(LogBTEditor, "Can't write {}", st.path);
    }
}

void FrameAll(BTEditorState& st, const Rect& rect, f32 dpi)
{
    Vec2 lo(1e30f), hi(-1e30f);
    for (const BTNode& n : st.tree.nodes) {
        lo = Min(lo, n.editorPosition);
        hi = Max(hi, n.editorPosition + Vec2(NodeWidth, NodeHeight));
    }
    if (st.tree.nodes.Empty()) {
        st.pan = Vec2(40.0f);
        st.zoom = 1.0f;
        return;
    }
    const Vec2 size = hi - lo + Vec2(80.0f);
    const Vec2 view = rect.Size() / dpi;
    st.zoom = Clamp(Min(view.x / size.x, view.y / size.y), 0.35f, 1.2f);
    st.pan = view / (2.0f * st.zoom) - (lo + hi) * 0.5f;
}

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

void DrawWire(ui::DrawList& dl, Vec2 from, Vec2 to, Color color, f32 scale, f32 width = 2.5f)
{
    const f32 dy = Max(Abs(to.y - from.y) * 0.5f, 30.0f * scale);
    dl.AddBezier(from, from + Vec2(0.0f, dy), to - Vec2(0.0f, dy), to, color, Max(width * scale, 1.5f));
}

void DrawNode(ui::DrawList& dl, const Canvas& c, const BehaviorTreeAsset& t, u32 index, bool selected, BTStatus status, bool live)
{
    ui::Context& ctx = *ui::GetCurrentContext();
    const ui::Theme& th = ctx.GetStyle().colors;
    const BTNode& n = t.nodes[index];
    const Rect r = NodeRect(c, n);
    if (!r.Intersects(c.rect))
        return;
    const f32 k = c.scale;
    dl.AddShadow(r, Color(0, 0, 0, 120), 6.0f * k, 10.0f * k, Vec2(0.0f, 3.0f * k));
    dl.AddRect(r, Color(0x26, 0x27, 0x2D, 245), ui::CornerRadii(6.0f * k), 1.0f, selected ? th.accentHovered : Color(0x10, 0x10, 0x12));
    if (selected)
        dl.AddRect(r.Expanded(1.5f * k), Color(0, 0, 0, 0), ui::CornerRadii(7.5f * k), 1.5f * k, th.accentHovered);
    // Live state of the watched AI.
    if (live && status != BTStatus::Idle) {
        const Color sc = status == BTStatus::Running ? Color(0xFF, 0xC8, 0x3C) : status == BTStatus::Success ? Color(0x4C, 0xC8, 0x6A) : Color(0xE8, 0x4C, 0x4C);
        dl.AddRect(r.Expanded(3.0f * k), Color(0, 0, 0, 0), ui::CornerRadii(9.0f * k), (status == BTStatus::Running ? 3.0f : 1.5f) * k, sc);
    }
    const Rect header(r.min, Vec2(r.max.x, r.min.y + HeaderHeight * k));
    const Color hc = HeaderColor(n.type);
    dl.AddRectGradient(header.Expanded(-1.0f), hc, WithAlpha(hc, 0.55f), ui::GradientDirection::Horizontal, ui::CornerRadii::Top(5.0f * k));
    // Pins: one in on top (not the root), one out at the bottom (composites, decorators).
    if (index != 0)
        dl.AddCircle(InPin(c, n), PinRadius * k, ParentOf(t, index) >= 0 ? Color(0xD0, 0xD0, 0xD0) : Color(0x22, 0x23, 0x28), 1.5f * k, Color(0xD0, 0xD0, 0xD0));
    if (HasOutput(n.type))
        dl.AddCircle(OutPin(c, n), PinRadius * k, n.children.Empty() ? Color(0x22, 0x23, 0x28) : Color(0xD0, 0xD0, 0xD0), 1.5f * k, Color(0xD0, 0xD0, 0xD0));
    if (k < 0.3f)
        return;
    const ui::FontHandle semi = ctx.Fonts().FindFont("Inter SemiBold");
    const ui::FontHandle font = ctx.Fonts().FindFont("Inter");
    const TypeInfo& info = InfoOf(n.type);
    const String title = n.name.Empty() ? String::Format("{}  {}", info.icon, BTNodeTypeName(n.type))
                                        : String::Format("{}  {}", info.icon, n.name);
    dl.AddTextInRect(semi, 12.0f * k, Rect(header.min + Vec2(8.0f * k, 0.0f), header.max - Vec2(30.0f * k, 0.0f)), Color(255, 255, 255), title);
    // Order among its siblings (they run left to right), or ROOT.
    String badge;
    if (index == 0)
        badge = "ROOT";
    else if (const i32 p = ParentOf(t, index); p >= 0 && t.nodes[usize(p)].children.Size() > 1)
        for (usize i = 0; i < t.nodes[usize(p)].children.Size(); ++i)
            if (t.nodes[usize(p)].children[i] == index)
                badge = String::Format("{}", i + 1);
    if (!badge.Empty())
        dl.AddTextInRect(semi, 10.0f * k, Rect(header.min, header.max - Vec2(8.0f * k, 0.0f)), WithAlpha(Color(255, 255, 255), 0.75f), badge,
                         ui::TextAlign::Right);
    String sub = Summary(n);
    if (sub.Empty() || !n.name.Empty())
        sub = sub.Empty() ? String(BTNodeTypeName(n.type)) : String::Format("{}: {}", BTNodeTypeName(n.type), sub);
    dl.AddTextInRect(font, 11.0f * k, Rect(Vec2(r.min.x + 8.0f * k, header.max.y), r.max - Vec2(8.0f * k, 0.0f)), th.textDim, sub);
}

void AddNodePopup(BTEditorState& st)
{
    if (!ui::BeginPopup("##bt_add_node"))
        return;
    ui::SetNextItemWidth(240.0f);
    if (st.addFocus) {
        ui::SetKeyboardFocusHere();
        st.addFocus = false;
    }
    ui::InputText("##bt_add_search", st.addSearch, ui::InputTextFlags::None, ZX_ICON_SEARCH "  Search nodes...");
    ui::Separator();
    auto matches = [&](StringView title) {
        if (st.addSearch.Empty())
            return true;
        for (usize i = 0; i + st.addSearch.Size() <= title.Size(); ++i)
            if (title.SubStr(i, st.addSearch.Size()).EqualsIgnoreCase(st.addSearch))
                return true;
        return false;
    };
    const bool enter = ui::IsKeyPressed(Key::Enter, false);
    bool taken = false;
    StringView category;
    for (const TypeInfo& info : Types) {
        const StringView name = BTNodeTypeName(info.type);
        if (!matches(name))
            continue;
        if (category != info.category) {
            category = info.category;
            ui::TextDisabled(category);
        }
        const bool clicked = ui::MenuItem(String::Format("   {}  {}##bt{}", info.icon, name, name));
        if (ui::IsItemHovered())
            ui::SetTooltip(info.tooltip);
        if (clicked || (enter && !taken)) {
            taken = true;
            PushUndo(st);
            BTNode n;
            n.type = info.type;
            n.editorPosition = Round(st.addPosition / 8.0f) * 8.0f;
            if (n.type == BTNodeType::Condition)
                n.op = "set";
            st.tree.nodes.PushBack(move(n));
            const u32 id = u32(st.tree.nodes.Size() - 1);
            if (st.addParent >= 0)
                Connect(st.tree, u32(st.addParent), id);
            st.selection.Clear();
            st.selection.PushBack(id);
            Changed(st);
            ui::CloseCurrentPopup();
        }
    }
    ui::EndPopup();
}

void BeginEdit(BTEditorState& st, StringView field)
{
    if (st.editField == field)
        return;
    st.editField = String(field);
    PushUndo(st);
}

/// Type + value of a blackboard value (Condition / SetValue operands, blackboard defaults).
bool ValueEditor(BTEditorState& st, StringView id, BBValue& v)
{
    bool changed = false;
    static const StringView typeNames[] = {"None", "Bool", "Number", "Vec3", "String"};
    i32 type = Min(i32(v.type), 4);
    ui::SetNextItemWidth(80.0f);
    if (ui::Combo(String::Format("##type{}", id), &type, Span<const StringView>(typeNames, 5))) {
        PushUndo(st);
        const BBType t = BBType(type);
        v = t == BBType::Bool ? BBValue::Bool(true) : t == BBType::Number ? BBValue::Number(0.0) : t == BBType::Vec3 ? BBValue::Vector(Vec3())
          : t == BBType::String ? BBValue::Text("") : BBValue();
        changed = true;
    }
    ui::SameLine();
    ui::SetNextItemWidth(-1.0f);
    switch (v.type) {
    case BBType::Bool: {
        bool on = v.number != 0.0;
        if (ui::Checkbox(String::Format("##b{}", id), &on)) {
            PushUndo(st);
            v.number = on ? 1.0 : 0.0;
            changed = true;
        }
        break;
    }
    case BBType::Number: {
        f32 x = f32(v.number);
        if (ui::DragFloat(String::Format("##n{}", id), &x, 0.1f, 0.0f, 0.0f, "{:.2f}")) {
            BeginEdit(st, String::Format("n{}", id));
            v.number = x;
            changed = true;
        }
        break;
    }
    case BBType::Vec3: {
        f32 xyz[3] = {v.vec.x, v.vec.y, v.vec.z};
        if (ui::DragFloat3(String::Format("##v{}", id), xyz, 0.1f)) {
            BeginEdit(st, String::Format("v{}", id));
            v.vec = Vec3(xyz[0], xyz[1], xyz[2]);
            changed = true;
        }
        break;
    }
    case BBType::String:
        if (ui::InputText(String::Format("##s{}", id), v.text)) {
            BeginEdit(st, String::Format("s{}", id));
            changed = true;
        }
        break;
    default: ui::TextDisabled("(no value)"); break;
    }
    return changed;
}

void Label(StringView text)
{
    ui::AlignTextToFramePadding();
    ui::Text(text);
    ui::SameLine(90.0f);
    ui::SetNextItemWidth(-1.0f);
}

void BlackboardPanel(Editor& ed, BTEditorState& st)
{
    ui::SeparatorText("Blackboard");
    ui::TextDisabled("Starting values of every AI");
    i32 remove = -1;
    for (usize i = 0; i < st.tree.defaults.Size(); ++i) {
        auto& d = st.tree.defaults[i];
        ui::PushID(i32(i));
        ui::SetNextItemWidth(-30.0f);
        if (ui::InputText("##key", d.first)) {
            BeginEdit(st, String::Format("key{}", i));
            Changed(st);
        }
        ui::SameLine();
        if (ui::IconButton(ZX_ICON_TRASH_2, "Remove"))
            remove = i32(i);
        if (ValueEditor(st, String::Format("bb{}", i), d.second))
            Changed(st);
        ui::Spacing();
        ui::PopID();
    }
    if (remove >= 0) {
        PushUndo(st);
        st.tree.defaults.RemoveAt(usize(remove));
        Changed(st);
    }
    if (ui::Button(ZX_ICON_PLUS "  Add Value")) {
        PushUndo(st);
        String key = "value";
        for (u32 n = 2;; ++n) {
            bool used = false;
            for (const auto& d : st.tree.defaults)
                used |= d.first == key;
            if (!used)
                break;
            key = String::Format("value{}", n);
        }
        st.tree.defaults.PushBack({key, BBValue::Bool(false)});
        Changed(st);
    }
    ui::TextDisabled("Sight writes target, targetVisible,\ntargetLastSeen");
    // Live values of the watched AI.
    if (ed.IsPlaying() && ed.world.IsAlive(st.debugEntity))
        if (const BehaviorTree* bt = ed.world.Get<const BehaviorTree>(st.debugEntity)) {
            ui::SeparatorText("Live");
            for (const auto& [key, v] : bt->blackboard) {
                ui::Text(key);
                ui::SameLine(110.0f);
                ui::TextDisabled(v.type == BBType::Entity ? String::Format("entity {}", v.entity.index) : ValueText(v));
            }
        }
}

void DetailsPanel(BTEditorState& st)
{
    const ui::Theme& th = ui::GetCurrentContext()->GetStyle().colors;
    ui::SeparatorText("Node");
    if (st.selection.Size() != 1 || st.selection[0] >= st.tree.nodes.Size()) {
        ui::TextDisabled(st.selection.Empty() ? "Select a node to edit it" : String::Format("{} nodes selected", st.selection.Size()));
    } else {
        const u32 id = st.selection[0];
        BTNode& n = st.tree.nodes[id];
        const TypeInfo& info = InfoOf(n.type);
        ui::Text(String::Format("{}  {}", info.icon, BTNodeTypeName(n.type)));
        ui::TextWrapped(info.tooltip);
        ui::Spacing();
        Label("Name");
        if (ui::InputText("##name", n.name, ui::InputTextFlags::None, "(shown on the node)")) {
            BeginEdit(st, String::Format("name{}", id));
            Changed(st);
        }
        const bool usesKey = n.type == BTNodeType::MoveTo || n.type == BTNodeType::Condition || n.type == BTNodeType::SetValue ||
                             n.type == BTNodeType::ClearValue || n.type == BTNodeType::RandomPoint || n.type == BTNodeType::NextWaypoint ||
                             n.type == BTNodeType::FaceTarget;
        if (usesKey) {
            Label("Key");
            if (ui::InputText("##key", n.key, ui::InputTextFlags::None, "blackboard key")) {
                BeginEdit(st, String::Format("key{}", id));
                Changed(st);
            }
        }
        if (n.type == BTNodeType::Condition) {
            static const StringView ops[] = {"set", "notset", "==", "!=", "<", ">", "<=", ">="};
            i32 at = 0;
            for (i32 i = 0; i < 8; ++i)
                if (n.op == ops[i])
                    at = i;
            Label("Compare");
            if (ui::Combo("##op", &at, Span<const StringView>(ops, 8))) {
                PushUndo(st);
                n.op = String(ops[at]);
                Changed(st);
            }
        }
        if (n.type == BTNodeType::Condition || n.type == BTNodeType::SetValue) {
            ui::AlignTextToFramePadding();
            ui::Text("Value");
            ui::SameLine(90.0f);
            if (ValueEditor(st, String::Format("node{}", id), n.value))
                Changed(st);
        }
        if (n.type == BTNodeType::Wait || n.type == BTNodeType::Cooldown) {
            Label("Seconds");
            if (ui::DragFloat("##sec", &n.seconds, 0.05f, 0.0f, 3600.0f, "{:.2f}")) {
                BeginEdit(st, String::Format("sec{}", id));
                Changed(st);
            }
        }
        if (n.type == BTNodeType::Wait) {
            Label("Deviation");
            if (ui::DragFloat("##dev", &n.deviation, 0.05f, 0.0f, 3600.0f, "{:.2f}")) {
                BeginEdit(st, String::Format("dev{}", id));
                Changed(st);
            }
        }
        if (n.type == BTNodeType::MoveTo || n.type == BTNodeType::RandomPoint) {
            Label("Radius");
            if (ui::DragFloat("##rad", &n.radius, 0.05f, 0.0f, 1000.0f, "{:.2f} m")) {
                BeginEdit(st, String::Format("rad{}", id));
                Changed(st);
            }
        }
        if (n.type == BTNodeType::Repeat || n.type == BTNodeType::Parallel) {
            Label("Count");
            if (ui::DragInt("##count", &n.count, 0.1f, 0, 1000)) {
                BeginEdit(st, String::Format("count{}", id));
                Changed(st);
            }
        }
        if (n.type == BTNodeType::Event || n.type == BTNodeType::Log || n.type == BTNodeType::NextWaypoint) {
            Label(n.type == BTNodeType::NextWaypoint ? "Group" : n.type == BTNodeType::Event ? "Event" : "Text");
            if (ui::InputText("##text", n.text, ui::InputTextFlags::None, n.type == BTNodeType::NextWaypoint ? "entity whose children are the points" : "")) {
                BeginEdit(st, String::Format("text{}", id));
                Changed(st);
            }
        }
    }
    if (!st.problems.Empty()) {
        ui::SeparatorText("Problems");
        ui::PushTextColor(th.warning);
        for (const String& p : st.problems)
            ui::TextWrapped(p);
        ui::PopTextColor();
    }
}

} // namespace

bool CreateBehaviorTreeFile(StringView physicalPath)
{
    BehaviorTreeAsset t;
    t.name = String(Path::Stem(physicalPath));
    BTNode root;
    root.type = BTNodeType::Selector;
    root.name = "Root";
    t.nodes.PushBack(root);
    return fs::WriteTextFile(physicalPath, WriteBehaviorTree(t));
}

void OpenBTEditor(Editor& ed, StringView physicalPath)
{
    BTEditorState& st = ed.btEditor;
    if (st.open && st.dirty && st.path != physicalPath)
        Save(ed, st);
    String text, error;
    BehaviorTreeAsset t;
    if (!fs::ReadTextFile(physicalPath, text) || !ParseBehaviorTree(text.View(), t, &error, false)) {
        ZX_LOG_ERROR(LogBTEditor, "Can't open {}: {}", physicalPath, error);
        return;
    }
    st = BTEditorState();
    st.open = true;
    st.path = String(physicalPath);
    st.tree = move(t);
    st.frameRequested = true;
    Recheck(st);
}

void DrawBTEditorPanel(Editor& ed)
{
    BTEditorState& st = ed.btEditor;
    st.capturesKeys = false;
    if (!st.open)
        return;
    ui::Context& ctx = *ui::GetCurrentContext();
    const f32 s = ctx.GetScale();
    const ui::Theme& th = ctx.GetStyle().colors;
    {
        const Vec2 display = Vec2(f32(ed.swapChain->GetSize().x), f32(ed.swapChain->GetSize().y)) / s;
        const Vec2 size = display * Vec2(0.94f, 0.88f);
        ui::SetNextWindowSize(size, ui::Cond::FirstUse);
        ui::SetNextWindowPos((display - size) * 0.5f, ui::Cond::FirstUse);
    }
    bool open = true;
    if (!ui::Begin(BTEditorTitle, &open, ui::WindowFlags::NoScrollbar)) {
        ui::End();
        if (!open && st.dirty)
            Save(ed, st);
        st.open = open;
        return;
    }
    if (!open && st.dirty)
        Save(ed, st);
    st.open = open;

    // --- Toolbar ---
    if (ui::Button(ZX_ICON_SAVE "  Save", Vec2::Zero(), st.dirty ? ui::ButtonVariant::Primary : ui::ButtonVariant::Default))
        Save(ed, st);
    ui::SameLine();
    ui::AlignTextToFramePadding();
    ui::Text(String::Format("{}{}", Path::FileName(st.path), st.dirty ? "*" : ""));
    ui::SameLine(0.0f, 16.0f);
    if (st.problems.Empty())
        ui::TextColored(th.success, String::Format(ZX_ICON_CHECK "  Ready ({} nodes)", st.tree.nodes.Size()));
    else {
        ui::TextColored(th.warning, String::Format(ZX_ICON_TRIANGLE_ALERT "  {} problem{}", st.problems.Size(), st.problems.Size() == 1 ? "" : "s"));
        ui::ItemTooltip(st.problems[0]);
    }
    ui::SameLine(0.0f, 16.0f);
    if (ui::Button(ZX_ICON_SCAN "  Frame"))
        st.frameRequested = true;
    ui::SameLine();
    if (ui::Button(ZX_ICON_NETWORK "  Arrange")) {
        PushUndo(st);
        LayoutBehaviorTree(st.tree, Vec2(NodeWidth, NodeHeight), Vec2(16.0f, 48.0f));
        Changed(st);
        st.frameRequested = true;
    }
    ui::ItemTooltip("Lays the tree out top-down (children keep their order)");
    // Debugging while playing: the AI whose tree is shown.
    const String relative = Path::MakeRelative(st.path, ed.contentRoot);
    bool live = false;
    if (!ed.IsPlaying()) {
        st.debugEntity = {};
    } else {
        Array<ecs::Entity> running;
        ed.world.Query<const BehaviorTree>().Each([&](ecs::Entity e, const BehaviorTree& bt) {
            if (Path::Normalize(bt.tree) == Path::Normalize(relative))
                running.PushBack(e);
        });
        ui::SameLine(0.0f, 16.0f);
        if (running.Empty()) {
            ui::TextDisabled(ZX_ICON_BUG "  No AI runs this tree");
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
            if (ui::Combo("##bt_debug", &current, items.View()) && current >= 0)
                st.debugEntity = running[usize(current)];
            ui::ItemTooltip("The AI whose tree is shown running (amber = running, green / red = last result)");
            live = !st.dirty;
            if (st.dirty) {
                ui::SameLine(0.0f, 8.0f);
                ui::TextDisabled("(save to see it run)");
            }
        }
    }
    Span<const BehaviorTree::NodeState> states = live ? BehaviorSystem::NodeStates(ed.world, st.debugEntity) : Span<const BehaviorTree::NodeState>();
    if (states.Size() != st.tree.nodes.Size())
        states = {};

    // --- Layout: blackboard | canvas | details ---
    const Rect content = ui::GetWindowContentRect();
    const f32 leftW = Round(210.0f * s), rightW = Round(250.0f * s), gap = Round(6.0f * s);
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
    ui::InvisibleButton("##bt_canvas", canvasRect.Size() / s);
    const bool hovered = ui::IsItemHovered() || ui::IsItemActive();
    const Vec2 mouse = ui::GetMousePos();
    dl.PushClipRect(canvasRect);
    DrawGrid(dl, c, s);

    // Hit testing: output pins first, then the topmost node.
    i32 hotNode = -1;
    bool hotOut = false, hotIn = false;
    for (usize i = st.tree.nodes.Size(); i > 0 && hovered; --i) {
        const BTNode& n = st.tree.nodes[i - 1];
        const f32 pr = Max((PinRadius + 5.0f) * c.scale, 9.0f * s); // stays grabbable when zoomed out
        if (HasOutput(n.type) && Length(OutPin(c, n) - mouse) < pr) {
            hotNode = i32(i - 1);
            hotOut = true;
            break;
        }
        // The input pin only above the node's top edge (a click on the node itself selects it).
        if (i - 1 != 0 && Length(InPin(c, n) - mouse) < pr && mouse.y < InPin(c, n).y + 2.0f * s) {
            hotNode = i32(i - 1);
            hotIn = true;
            break;
        }
        if (NodeRect(c, n).Contains(mouse)) {
            hotNode = i32(i - 1);
            break;
        }
    }

    // --- Mouse ---
    const bool ctrl = HasAnyFlags(ed.input.Modifiers(), KeyModifiers::Control);
    const bool alt = HasAnyFlags(ed.input.Modifiers(), KeyModifiers::Alt);
    if (hovered && ed.input.WasMousePressed(MouseButton::Left)) {
        st.dragStart = mouse;
        st.dragMoved = false;
        if (hotOut && !alt) {
            st.beforeDrag = Snapshot(st.tree);
            st.linkParent = u32(hotNode);
            st.drag = BTEditorState::Drag::Link;
        } else if ((hotIn || hotOut) && alt) {
            PushUndo(st);
            if (hotIn)
                Disconnect(st.tree, u32(hotNode));
            else
                st.tree.nodes[usize(hotNode)].children.Clear();
            Changed(st);
        } else if (hotIn) {
            // Grab a node's parent link by its input: re-attach it elsewhere.
            if (const i32 p = ParentOf(st.tree, u32(hotNode)); p >= 0) {
                st.beforeDrag = Snapshot(st.tree);
                Disconnect(st.tree, u32(hotNode));
                st.linkParent = u32(p);
                st.drag = BTEditorState::Drag::Link;
            }
        } else if (hotNode >= 0) {
            if (ctrl) {
                if (st.selection.Contains(u32(hotNode)))
                    st.selection.Remove(u32(hotNode));
                else
                    st.selection.PushBack(u32(hotNode));
            } else if (!st.selection.Contains(u32(hotNode))) {
                st.selection.Clear();
                st.selection.PushBack(u32(hotNode));
            }
            st.beforeDrag = Snapshot(st.tree);
            st.drag = BTEditorState::Drag::Nodes;
        } else {
            if (!ctrl)
                st.selection.Clear();
            st.drag = BTEditorState::Drag::Box;
        }
    }
    if ((hovered || st.drag == BTEditorState::Drag::Pan) &&
        (ed.input.WasMousePressed(MouseButton::Right) || ed.input.WasMousePressed(MouseButton::Middle))) {
        st.rightPress = mouse;
        st.dragMoved = false;
        st.drag = BTEditorState::Drag::Pan;
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
    case BTEditorState::Drag::Nodes:
        if (ed.input.IsMouseDown(MouseButton::Left)) {
            if (Length(mouse - st.dragStart) > 3.0f * s)
                st.dragMoved = true;
            if (st.dragMoved)
                for (u32 id : st.selection)
                    if (id < st.tree.nodes.Size())
                        st.tree.nodes[id].editorPosition += delta / c.scale;
        } else {
            if (st.dragMoved) {
                for (u32 id : st.selection)
                    if (id < st.tree.nodes.Size())
                        st.tree.nodes[id].editorPosition = Round(st.tree.nodes[id].editorPosition / 8.0f) * 8.0f;
                SortAll(st.tree); // moving a node sideways changes when it runs
                st.undo.PushBack(st.beforeDrag);
                st.redo.Clear();
                Changed(st);
            }
            st.beforeDrag.Clear();
            st.drag = BTEditorState::Drag::None;
        }
        break;
    case BTEditorState::Drag::Box: {
        const Rect box(Min(st.dragStart, mouse), Max(st.dragStart, mouse));
        if (ed.input.IsMouseDown(MouseButton::Left)) {
            dl.AddRect(box, WithAlpha(th.accent, 0.12f), ui::CornerRadii(), 1.0f, WithAlpha(th.accentHovered, 0.8f));
        } else {
            if (box.Width() > 3.0f || box.Height() > 3.0f)
                for (u32 i = 0; i < st.tree.nodes.Size(); ++i)
                    if (NodeRect(c, st.tree.nodes[i]).Intersects(box) && !st.selection.Contains(i))
                        st.selection.PushBack(i);
            st.drag = BTEditorState::Drag::None;
        }
        break;
    }
    case BTEditorState::Drag::Link:
        if (!ed.input.IsMouseDown(MouseButton::Left)) {
            bool connected = false;
            if (hotNode >= 0 && !hotOut)
                connected = Connect(st.tree, st.linkParent, u32(hotNode));
            const bool changed = Snapshot(st.tree) != st.beforeDrag;
            if (changed) {
                st.undo.PushBack(st.beforeDrag);
                st.redo.Clear();
                Changed(st);
            }
            if (!connected && hotNode < 0 && Length(mouse - st.dragStart) > 6.0f * s) {
                // Dropped on empty canvas: pick a node to add as a child.
                st.addParent = i32(st.linkParent);
                st.addPosition = c.ToGraph(mouse) - Vec2(NodeWidth * 0.5f, 0.0f);
                st.addSearch.Clear();
                st.addFocus = true;
                ui::OpenPopup("##bt_add_node");
            }
            st.beforeDrag.Clear();
            st.drag = BTEditorState::Drag::None;
        }
        break;
    case BTEditorState::Drag::Pan:
        if (ed.input.IsMouseDown(MouseButton::Right) || ed.input.IsMouseDown(MouseButton::Middle)) {
            if (Length(mouse - st.rightPress) > 4.0f * s)
                st.dragMoved = true;
            st.pan += delta / c.scale;
            c.pan = st.pan;
        } else {
            if (!st.dragMoved && hovered) {
                if (hotNode >= 0) {
                    if (!st.selection.Contains(u32(hotNode))) {
                        st.selection.Clear();
                        st.selection.PushBack(u32(hotNode));
                    }
                    ui::OpenPopup("##bt_node_menu");
                } else {
                    st.addParent = -1;
                    st.addPosition = c.ToGraph(mouse);
                    st.addSearch.Clear();
                    st.addFocus = true;
                    ui::OpenPopup("##bt_add_node");
                }
            }
            st.drag = BTEditorState::Drag::None;
        }
        break;
    case BTEditorState::Drag::None: break;
    }

    // --- Wires (parent bottom -> child top) ---
    for (u32 i = 0; i < st.tree.nodes.Size(); ++i)
        for (u32 ch : st.tree.nodes[i].children) {
            if (ch >= st.tree.nodes.Size())
                continue;
            const bool running = !states.Empty() && states[ch].status == BTStatus::Running && states[i].status == BTStatus::Running;
            const Vec2 from = OutPin(c, st.tree.nodes[i]), to = InPin(c, st.tree.nodes[ch]);
            if (running)
                DrawWire(dl, from, to, Color(0xFF, 0xC8, 0x3C, 160), c.scale, 6.0f);
            DrawWire(dl, from, to, Color(0xB8, 0xBC, 0xC8, 230), c.scale);
        }
    if (st.drag == BTEditorState::Drag::Link && st.linkParent < st.tree.nodes.Size())
        DrawWire(dl, OutPin(c, st.tree.nodes[st.linkParent]), mouse, th.accentHovered, c.scale);

    // --- Nodes ---
    for (u32 i = 0; i < st.tree.nodes.Size(); ++i)
        DrawNode(dl, c, st.tree, i, st.selection.Contains(i), states.Empty() ? BTStatus::Idle : states[i].status, !states.Empty());
    if (hotNode >= 0 && (hotIn || hotOut)) {
        const BTNode& n = st.tree.nodes[usize(hotNode)];
        dl.AddCircleOutline(hotOut ? OutPin(c, n) : InPin(c, n), (PinRadius + 4.0f) * c.scale, th.accentHovered, 1.5f);
    }
    dl.AddTextInRect(ctx.Fonts().FindFont("Inter"), 11.0f * s,
                     Rect(Vec2(canvasRect.min.x + 10.0f * s, canvasRect.max.y - 24.0f * s), canvasRect.max - Vec2(10.0f * s, 4.0f * s)), th.textDisabled,
                     "Right click: add node   Drag from a node's bottom pin: add a child (children run left to right)   Alt+click a pin: "
                     "disconnect   Wheel: zoom   RMB drag: pan   F: frame");
    dl.PopClipRect();

    // --- Popups ---
    AddNodePopup(st);
    if (ui::BeginPopup("##bt_node_menu")) {
        const bool one = st.selection.Size() == 1;
        if (ui::MenuItem(ZX_ICON_CROWN "  Make Root", {}, false, one && st.selection[0] != 0)) {
            PushUndo(st);
            const u32 node = st.selection[0];
            SetRoot(st.tree, node);
            st.selection.Clear();
            st.selection.PushBack(0);
            Changed(st);
        }
        if (ui::MenuItem(ZX_ICON_UNLINK "  Disconnect")) {
            PushUndo(st);
            for (u32 id : st.selection)
                Disconnect(st.tree, id);
            Changed(st);
        }
        if (ui::MenuItem(ZX_ICON_TRASH_2 "  Delete", "Del")) {
            PushUndo(st);
            RemoveNodes(st.tree, st.selection.View());
            st.selection.Clear();
            Changed(st);
        }
        ui::EndPopup();
    }

    // --- Keyboard ---
    st.capturesKeys = hovered && !ctx.WantsKeyboard();
    if (st.capturesKeys) {
        if ((ui::IsKeyPressed(Key::Delete, false) || ui::IsKeyPressed(Key::Backspace, false)) && !st.selection.Empty()) {
            PushUndo(st);
            RemoveNodes(st.tree, st.selection.View());
            st.selection.Clear();
            Changed(st);
        }
        if (ui::IsShortcutPressed(KeyModifiers::Control, Key::A)) {
            st.selection.Clear();
            for (u32 i = 0; i < st.tree.nodes.Size(); ++i)
                st.selection.PushBack(i);
        }
        if (ui::IsKeyPressed(Key::F, false))
            st.frameRequested = true;
    }
    if (ui::IsWindowHovered() || st.capturesKeys) {
        if (ui::IsShortcutPressed(KeyModifiers::Control, Key::S))
            Save(ed, st);
        const bool undo = ui::IsShortcutPressed(KeyModifiers::Control, Key::Z) && !ctx.WantsKeyboard();
        const bool redo = (ui::IsShortcutPressed(KeyModifiers::Control, Key::Y) || ui::IsShortcutPressed(KeyModifiers::Control | KeyModifiers::Shift, Key::Z)) &&
                          !ctx.WantsKeyboard();
        if ((undo && !st.undo.Empty()) || (redo && !st.redo.Empty())) {
            Array<String>& from = undo ? st.undo : st.redo;
            Array<String>& to = undo ? st.redo : st.undo;
            to.PushBack(Snapshot(st.tree));
            ParseBehaviorTree(from.Back().View(), st.tree, nullptr, false);
            from.PopBack();
            st.selection.Clear();
            Changed(st);
        }
        st.capturesKeys = true;
    }

    // --- Side panels ---
    ui::SetCursorPos(leftRect.min);
    if (ui::BeginChild("##bt_left", leftRect.Size() / s, false))
        BlackboardPanel(ed, st);
    ui::EndChild();
    ui::SetCursorPos(rightRect.min);
    if (ui::BeginChild("##bt_right", rightRect.Size() / s, false))
        DetailsPanel(st);
    ui::EndChild();
    ui::End();
}

} // namespace zx::editor
