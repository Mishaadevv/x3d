// Details panel (component inspector) and the reflection-driven property editor.

#include "Editor.h"
#include "Core/HAL/PlatformFile.h"
#include "Core/String/Parse.h"
#include "Core/String/Path.h"
#include "Engine/ComponentRegistry.h"

namespace zx::editor {

namespace {

constexpr f32 LabelRatio = 0.38f;

Editor* g_editor = nullptr; // set while the Details panel draws (asset names, material editing)

/// Display name of an asset reference: material file, built-in shape or source file.
String AssetName(const Guid& g, StringView assetType, StringView& icon)
{
    icon = ZX_ICON_PACKAGE;
    if (!g.IsValid())
        return "None";
    if (g_editor && g_editor->materials)
        if (const MaterialLibrary::Material* m = g_editor->materials->Find(g)) {
            icon = ZX_ICON_PALETTE;
            return String(Path::Stem(m->path));
        }
    for (u32 i = 0; i < u32(BuiltinShape::Count); ++i)
        if (builtin::Mesh(BuiltinShape(i)) == g) {
            icon = ZX_ICON_SHAPES;
            return String::Format("{} (built-in)", builtin::ShapeName(BuiltinShape(i)));
        }
    if (g_editor && g_editor->assets) {
        const String source = g_editor->assets->GetSourcePath(g);
        if (!source.Empty())
            return String::Format("{} ({})", assetType, Path::FileName(source));
    }
    return String::Format("{} {}", assetType, String::Format("{}", g).View().Left(8));
}

// Prefab overrides of the entity being inspected ("Component.field"), set while drawing its components.
const Array<String>* g_overrides = nullptr;
StringView g_component;
u32 g_depth = 0;

bool IsOverridden(StringView field)
{
    if (!g_overrides || g_depth != 1)
        return false;
    for (const String& o : *g_overrides)
        if (o.Size() == g_component.Size() + 1 + field.Size() && o.View().StartsWith(g_component) && o.View().EndsWith(field) &&
            o[g_component.Size()] == '.')
            return true;
    return false;
}

/// Starts a "label | value" row; the value widget fills the rest of the line. Overridden prefab values are bold with
/// an accent marker (as in UE/Unity). `condition` (EditCondition) adds an override checkbox in front of the label;
/// `conditionChanged` is set when it was clicked. Long labels are cut with an ellipsis at the value column.
void PropertyLabel(StringView label, StringView tooltip, bool overridden = false, bool* condition = nullptr, bool* conditionChanged = nullptr)
{
    ui::Context& ctx = *ui::GetCurrentContext();
    const f32 s = ctx.GetScale();
    const f32 avail = ui::GetContentRegionAvail().x / s;
    const f32 lineStart = ui::GetCursorPos().x;
    const f32 valueX = lineStart + Round(avail * LabelRatio * s);
    ui::AlignTextToFramePadding();
    if (condition) {
        const bool clicked = ui::Checkbox("##override", condition);
        if (conditionChanged)
            *conditionChanged = clicked;
        ui::ItemTooltip(*condition ? StringView("Overridden: this value is used") : StringView("Not overridden: tick to use this value"));
        ui::SameLine(0.0f, 4.0f);
    }
    const Vec2 p = ui::GetCursorPos();
    if (overridden)
        ui::GetWindowDrawList().AddRect(Rect(p + Vec2(-Round(5.0f * s), Round(3.0f * s)), p + Vec2(-Round(3.0f * s), ui::GetFrameHeight() - Round(3.0f * s))),
                                        ctx.GetStyle().colors.accentHovered);
    const f32 width = Max(valueX - p.x - Round(6.0f * s), 1.0f);
    const ui::FontHandle font = ctx.Fonts().FindFont(overridden ? "Inter SemiBold" : "Inter");
    ui::GetWindowDrawList().AddTextInRect(font, ctx.GetStyle().fontSize * s, Rect(p, p + Vec2(width, ui::GetFrameHeight())),
                                          overridden ? ctx.GetStyle().colors.text : ctx.GetStyle().colors.textDim, label);
    ui::Dummy(Vec2(width / s, ui::GetFrameHeight() / s));
    if (!tooltip.Empty())
        ui::ItemTooltip(tooltip);
    else if (ctx.Fonts().MeasureWidth(font, ctx.GetStyle().fontSize * s, label) > width)
        ui::ItemTooltip(label); // cut: show it in full on hover
    ui::SameLine(Round(avail * LabelRatio));
    ui::SetNextItemWidth(-1.0f);
}

bool EditFloat(const FieldInfo& f, f32* v, const String& id)
{
    const MetaValue* range = f.meta.Find(Meta::KeyRange());
    const StringView units = f.meta.GetText(Meta::KeyUnits());
    if (f.meta.Has(Meta::KeyAngle())) {
        f32 deg = RadToDeg(*v);
        const f32 mn = range ? f32(RadToDeg(f32(range->number))) : 0.0f, mx = range ? f32(RadToDeg(f32(range->number2))) : 0.0f;
        if (ui::DragFloat(id, &deg, 0.25f, mn, mx, "{:.1f}°")) {
            *v = DegToRad(deg);
            return true;
        }
        return false;
    }
    const String format = units.Empty() ? String("{:.3f}") : String::Format("{{:.2f}} {}", units);
    if (range && range->number2 - range->number <= 1000.0)
        return ui::SliderFloat(id, v, f32(range->number), f32(range->number2), format);
    const f32 speed = range ? f32((range->number2 - range->number) / 2000.0) : 0.02f;
    return ui::DragFloat(id, v, Max(speed, 0.001f), range ? f32(range->number) : 0.0f, range ? f32(range->number2) : 0.0f, format);
}

bool EditStruct(const FieldInfo* field, const TypeInfo& type, void* p, const String& id, StringView label);

bool EditValue(const FieldInfo* field, const TypeInfo& type, void* p, const String& id, StringView label)
{
    const bool isColor = (field && field->meta.Has(Meta::KeyColor())) || type.meta.Has(Meta::KeyColor());
    switch (type.kind) {
    case TypeKind::Bool:
        return ui::Checkbox(id, static_cast<bool*>(p));
    case TypeKind::Int32: {
        i32* v = static_cast<i32*>(p);
        const MetaValue* range = field ? field->meta.Find(Meta::KeyRange()) : nullptr;
        return ui::DragInt(id, v, 0.1f, range ? i32(range->number) : 0, range ? i32(range->number2) : 0);
    }
    case TypeKind::UInt32:
    case TypeKind::UInt16:
    case TypeKind::UInt8:
    case TypeKind::Int8:
    case TypeKind::Int16:
    case TypeKind::Int64:
    case TypeKind::UInt64: {
        // Edit through an i32 view (values out of range are clamped).
        i64 value = 0;
        MemCopy(&value, p, type.size);
        i32 v32 = i32(Clamp<i64>(value, I32Min, I32Max));
        if (ui::DragInt(id, &v32, 0.1f, IsSignedKind(type.kind) ? 0 : 0, 0)) {
            value = IsSignedKind(type.kind) ? i64(v32) : i64(Max(v32, 0));
            MemCopy(p, &value, type.size);
            return true;
        }
        return false;
    }
    case TypeKind::Float:
        return field ? EditFloat(*field, static_cast<f32*>(p), id) : ui::DragFloat(id, static_cast<f32*>(p));
    case TypeKind::Double: {
        f32 v = f32(*static_cast<f64*>(p));
        if (ui::DragFloat(id, &v)) {
            *static_cast<f64*>(p) = v;
            return true;
        }
        return false;
    }
    case TypeKind::String: {
        String* s = static_cast<String*>(p);
        if (field && field->meta.Has(Meta::KeyMultiline()))
            return ui::InputTextMultiline(id, *s, Vec2(0.0f, 70.0f));
        return ui::InputText(id, *s);
    }
    case TypeKind::Name:
        ui::Text(static_cast<const zx::Name*>(p)->View());
        return false;
    case TypeKind::Guid: {
        Guid& g = *static_cast<Guid*>(p);
        const StringView assetType = field ? field->meta.GetText(Meta::KeyAssetType(), "Asset") : StringView("Asset");
        StringView icon;
        const String name = AssetName(g, assetType, icon);
        const bool isMaterial = assetType == "Material";
        const bool editable = isMaterial && g_editor && g_editor->materials && g_editor->materials->Find(g);
        // Read-only asset slot; materials accept a .zmat dropped from the Content Browser.
        const f32 fh = ui::GetFrameHeight() / ui::GetCurrentContext()->GetScale();
        ui::SetNextItemWidth(editable ? -(fh + 4.0f) : -1.0f);
        String copy = String::Format("{}  {}", icon, name);
        ui::InputText(id, copy, ui::InputTextFlags::ReadOnly);
        bool changed = false;
        if (isMaterial && g_editor && g_editor->materials) {
            ui::ItemTooltip("Drop a material (.zmat) from the Content Browser here");
            if (const void* payload = ui::AcceptDragDropPayload("asset")) {
                const StringView path(static_cast<const char*>(payload));
                if (Path::HasExtension(path, ".zmat") || Path::HasExtension(path, ".zmati")) {
                    const MaterialLibrary::Material* m = g_editor->materials->FindByPath(path);
                    const Guid dropped = m ? m->guid : g_editor->materials->LoadFile(path);
                    if (dropped.IsValid() && dropped != g) {
                        g = dropped;
                        changed = true;
                    }
                }
            }
        }
        if (editable) {
            ui::SameLine(0.0f, 4.0f);
            if (ui::IconButton(ZX_ICON_PENCIL, "Edit in the Material Editor"))
                OpenMaterialEditor(*g_editor, g);
        }
        return changed;
    }
    case TypeKind::Enum: {
        Array<StringView> names;
        i32 current = -1;
        i64 value = 0;
        MemCopy(&value, p, type.size);
        for (usize i = 0; i < type.enumEntries.Size(); ++i) {
            names.PushBack(type.enumEntries[i].displayName.View());
            if (type.enumEntries[i].value == value)
                current = i32(i);
        }
        if (ui::Combo(id, &current, names.View()) && current >= 0) {
            value = type.enumEntries[usize(current)].value;
            MemCopy(p, &value, type.size);
            return true;
        }
        return false;
    }
    case TypeKind::Struct: {
        const StringView n = type.name.View();
        if (isColor && n == "Vec3") {
            Vec3* v = static_cast<Vec3*>(p);
            LinearColor c(v->x, v->y, v->z, 1.0f);
            if (ui::ColorEdit(id, &c, false)) {
                *v = Vec3(c.r, c.g, c.b);
                return true;
            }
            return false;
        }
        if (n == "LinearColor")
            return ui::ColorEdit(id, static_cast<LinearColor*>(p), true);
        if (n == "Vec3")
            return ui::DragFloat3(id, &static_cast<Vec3*>(p)->x, 0.01f);
        if (n == "Vec2")
            return ui::DragFloat2(id, &static_cast<Vec2*>(p)->x, 0.01f);
        if (n == "Quat") {
            Quat* q = static_cast<Quat*>(p);
            // Keep the Euler angles being edited stable (avoids flipping near the poles while dragging).
            static Quat s_lastQ;
            static Vec3 s_lastEuler;
            Vec3 euler = (*q == s_lastQ) ? s_lastEuler : q->ToEulerDegrees();
            if (ui::DragFloat3(id, &euler.x, 0.25f)) {
                *q = Quat::FromEulerDegrees(euler);
                s_lastQ = *q;
                s_lastEuler = euler;
                return true;
            }
            return false;
        }
        return EditStruct(field, type, p, id, label);
    }
    case TypeKind::Array: {
        const usize count = type.arrayOps->size(p);
        bool changed = false;
        if (ui::TreeNode(String::Format("{} elements{}", count, id))) {
            for (usize i = 0; i < count; ++i) {
                ui::PushID(i32(i));
                PropertyLabel(String::Format("[{}]", i), {});
                changed |= EditValue(field, *type.element, type.arrayOps->at(p, i), String("##elem"), {});
                ui::PopID();
            }
            ui::TreePop();
        }
        return changed;
    }
    default:
        ui::TextDisabled("(unsupported)");
        return false;
    }
}

bool EditStruct(const FieldInfo*, const TypeInfo& type, void* p, const String& id, StringView label)
{
    bool changed = false;
    if (ui::TreeNode(String::Format("{}{}", label.Empty() ? type.displayName.View() : label, id), ui::TreeNodeFlags::DefaultOpen)) {
        changed = PropertyEditor(type, p);
        ui::TreePop();
    }
    return changed;
}

/// "moveSpeed" -> "Move Speed".
String PrettyName(StringView name)
{
    String out;
    for (usize i = 0; i < name.Size(); ++i) {
        char c = name[i];
        if (c == '_') {
            out += ' ';
            continue;
        }
        if (i == 0 && c >= 'a' && c <= 'z')
            c = char(c - 'a' + 'A');
        else if (c >= 'A' && c <= 'Z' && i > 0 && name[i - 1] >= 'a' && name[i - 1] <= 'z')
            out += ' ';
        out += c;
    }
    return out;
}

/// Collects the .zs files under the content root (relative paths).
void ListScripts(const String& root, const String& dir, Array<String>& out)
{
    Array<platform::DirectoryEntry> entries;
    platform::ListDirectory(dir, entries);
    for (const platform::DirectoryEntry& e : entries) {
        if (e.name.View().StartsWith("."))
            continue;
        const String path = Path::Join(dir, e.name);
        if (e.isDirectory)
            ListScripts(root, path, out);
        else if (Path::HasExtension(e.name, ".zs"))
            out.PushBack(Path::MakeRelative(path, root));
    }
}

/// Script component: file slot (drop a .zs, pick from the project, create one), compile status and the script's props.
bool EditScript(Editor& ed, Script& s)
{
    bool changed = false;
    const f32 fh = ui::GetFrameHeight() / ui::GetCurrentContext()->GetScale();
    PropertyLabel("Script", "The ZeqouScript file (.zs) this entity runs. Drop one from the Content Browser.");
    ui::SetNextItemWidth(-(fh * 2.0f + 8.0f));
    String shown = s.file.Empty() ? String("None") : String::Format(ZX_ICON_FILE_CODE "  {}", Path::FileName(s.file));
    ui::InputText("##script_file", shown, ui::InputTextFlags::ReadOnly);
    ui::ItemTooltip(s.file.Empty() ? StringView("Drop a script (.zs) from the Content Browser here") : s.file.View());
    if (const void* payload = ui::AcceptDragDropPayload("asset")) {
        const StringView path(static_cast<const char*>(payload));
        if (Path::HasExtension(path, ".zs")) {
            s.file = Path::MakeRelative(path, ed.contentRoot);
            s.properties.Clear();
            changed = true;
        }
    }
    ui::SameLine(0.0f, 4.0f);
    static Array<String> s_scripts;
    if (ui::IconButton(ZX_ICON_FOLDER_OPEN, "Pick a script of this project")) {
        s_scripts.Clear();
        ListScripts(ed.contentRoot, ed.contentRoot, s_scripts);
        ui::OpenPopup("##pick_script");
    }
    if (ui::BeginPopup("##pick_script")) {
        if (ui::MenuItem(ZX_ICON_FILE_PLUS "  New Script...")) {
            const String dir = ed.contentRoot + "/Scripts";
            platform::CreateDirectoryTree(dir);
            String path = dir + "/NewScript.zs";
            for (u32 n = 2; platform::FileExists(path); ++n)
                path = String::Format("{}/NewScript{}.zs", dir, n);
            if (CreateScriptFile(path)) {
                s.file = Path::MakeRelative(path, ed.contentRoot);
                s.properties.Clear();
                changed = true;
                OpenScriptEditor(ed, path);
                ed.panels.browserListedDir.Clear();
            }
        }
        if (!s_scripts.Empty())
            ui::Separator();
        for (const String& f : s_scripts)
            if (ui::MenuItem(String::Format(ZX_ICON_FILE_CODE "  {}", f), {}, f == s.file) && f != s.file) {
                s.file = f;
                s.properties.Clear();
                changed = true;
            }
        if (s_scripts.Empty())
            ui::TextDisabled("No scripts in this project yet");
        ui::EndPopup();
    }
    ui::SameLine(0.0f, 4.0f);
    ui::BeginDisabled(s.file.Empty());
    if (ui::IconButton(ZX_ICON_PENCIL, "Edit in the Script Editor"))
        OpenScriptEditor(ed, Path::Join(ed.contentRoot, s.file));
    ui::EndDisabled();
    if (s.file.Empty() || !ed.scripts)
        return changed;

    String error;
    const script::Program* program = ed.scripts->GetProgram(s.file, &error);
    if (!program) {
        ui::PushTextColor(Color(0xF0, 0x60, 0x60));
        ui::TextWrapped(String::Format(ZX_ICON_CIRCLE_X "  {}", error));
        ui::PopTextColor();
        if (ui::IsItemClicked()) {
            // "file:line:col: message" -> open at the line.
            u32 line = 0;
            const usize a = error.Find(':');
            if (a != StringView::NPos)
                if (Optional<i64> l = ParseInt(error.View().SubStr(a + 1, error.Find(':', a + 1) - a - 1)))
                    line = u32(*l);
            OpenScriptEditor(ed, Path::Join(ed.contentRoot, s.file), line);
        }
        return changed;
    }
    if (program->properties.Empty()) {
        ui::TextDisabled("No props - declare 'prop speed = 1.0' to edit values here");
        return changed;
    }
    // Props: the script's defaults, overridden per entity (bold).
    JsonValue values = ParseScriptProperties(s);
    for (const script::Property& prop : program->properties) {
        ui::PushID(prop.name);
        const JsonValue* stored = values.Find(prop.name);
        script::Value v = stored ? JsonToScriptValue(*stored, prop) : prop.defaultValue;
        const String label = PrettyName(prop.name);
        PropertyLabel(label, prop.tooltip, stored != nullptr);
        bool edited = false;
        switch (prop.kind) {
        case script::PropertyKind::Number: {
            f32 f = v.AsFloat();
            if (prop.rangeMax > prop.rangeMin)
                edited = ui::SliderFloat("##v", &f, f32(prop.rangeMin), f32(prop.rangeMax), "{:.2f}");
            else
                edited = ui::DragFloat("##v", &f, 0.05f);
            if (edited)
                v = script::Value(f);
            break;
        }
        case script::PropertyKind::Bool: {
            bool b = v.AsBool();
            edited = ui::Checkbox("##v", &b);
            if (edited)
                v = script::Value(b);
            break;
        }
        case script::PropertyKind::String: {
            String str(v.AsString());
            edited = ui::InputText("##v", str);
            if (edited)
                v = script::Value(str.View());
            break;
        }
        case script::PropertyKind::Vec3: {
            Vec3 x = v.AsVec3();
            edited = ui::DragFloat3("##v", &x.x, 0.01f);
            if (edited)
                v = script::Value(x);
            break;
        }
        case script::PropertyKind::Color: {
            const Vec3 x = v.AsVec3();
            LinearColor c(x.x, x.y, x.z, 1.0f);
            edited = ui::ColorEdit("##v", &c, false);
            if (edited)
                v = script::Value(Vec3(c.r, c.g, c.b));
            break;
        }
        }
        if (ui::BeginPopupContextItem("##prop_menu")) {
            if (ui::MenuItem(ZX_ICON_ROTATE_CCW "  Reset to Default", {}, false, stored != nullptr)) {
                v = prop.defaultValue;
                edited = true;
            }
            ui::EndPopup();
        }
        if (edited) {
            SetScriptProperty(s, prop, v);
            changed = true;
        }
        ui::PopID();
    }
    return changed;
}

/// C++ Script component: the game module's component class and its props (ZX_GAME_PROP), like the Script component.
bool EditNativeScript(Editor& ed, NativeScript& s)
{
    bool changed = false;
    GameModuleSystem* game = ed.game.Get();
    const bool hasSource = !ed.project.descriptorPath.Empty() && HasGameModuleSource(ed.project.root, GameModuleName(ed.project));
    PropertyLabel("Class", "The C++ component (a ScriptComponent in the project's game module) this entity runs.");
    if (!game || !game->IsLoaded()) {
        String shown = s.className.Empty() ? String("None") : s.className;
        ui::SetNextItemWidth(-1.0f);
        ui::InputText("##native_class", shown, ui::InputTextFlags::ReadOnly);
        ui::PushTextColor(Color(0xE8, 0xB8, 0x4A));
        if (!hasSource)
            ui::TextWrapped(ZX_ICON_TRIANGLE_ALERT "  This project has no C++ game module yet.");
        else if (ed.gameBuildState == GameBuildState::Building)
            ui::TextWrapped(ZX_ICON_LOADER_CIRCLE "  Compiling game code...");
        else
            ui::TextWrapped(ZX_ICON_TRIANGLE_ALERT "  The game module is not loaded - compile it (F7).");
        ui::PopTextColor();
        if (!hasSource && ui::Button(ZX_ICON_FOLDER_CODE "  Create C++ Game Module"))
            CreateGameModule(ed);
        else if (hasSource && ed.gameBuildState != GameBuildState::Building && ui::Button(ZX_ICON_HAMMER "  Compile"))
            CompileGameCode(ed);
        return false;
    }
    const Array<const game::ComponentDesc*> classes = game->Components();
    Array<StringView> names;
    names.PushBack("None");
    i32 current = 0;
    for (usize i = 0; i < classes.Size(); ++i) {
        names.PushBack(classes[i]->name);
        if (s.className == classes[i]->name)
            current = i32(i + 1);
    }
    if (current == 0 && !s.className.Empty()) {
        names.PushBack(s.className); // missing from the module: keep showing it
        current = i32(names.Size() - 1);
    }
    ui::SetNextItemWidth(-(ui::GetFrameHeight() / ui::GetCurrentContext()->GetScale() + 4.0f));
    if (ui::Combo("##native_class", &current, names.View())) {
        const String picked = current == 0 ? String() : String(names[usize(current)]);
        if (picked != s.className) {
            s.className = picked;
            s.properties.Clear();
            changed = true;
        }
    }
    ui::SameLine(0.0f, 4.0f);
    ui::BeginDisabled(ed.gameBuildState == GameBuildState::Building);
    if (ui::IconButton(ed.gameBuildState == GameBuildState::Building ? ZX_ICON_LOADER_CIRCLE : ZX_ICON_HAMMER, "Compile game code (F7)"))
        CompileGameCode(ed);
    ui::EndDisabled();
    if (s.className.Empty())
        return changed;
    const game::ComponentDesc* desc = game->FindComponent(s.className);
    if (!desc) {
        ui::PushTextColor(Color(0xF0, 0x60, 0x60));
        ui::TextWrapped(String::Format(ZX_ICON_CIRCLE_X "  The game module has no component '{}'", s.className));
        ui::PopTextColor();
        return changed;
    }
    if (desc->description && *desc->description)
        ui::TextDisabled(desc->description);
    if (!desc->propCount) {
        ui::TextDisabled("No props - add ZX_GAME_PROP(member, min, max, \"tooltip\") to edit values here");
        return changed;
    }
    // Props: the component's defaults, overridden per entity (bold).
    const JsonValue defaults = GameModuleSystem::DefaultProps(*desc);
    const JsonValue stored = ParseNativeScriptProperties(s);
    for (u32 i = 0; i < desc->propCount; ++i) {
        const game::PropDesc& prop = desc->props[i];
        ui::PushID(prop.name);
        const JsonValue* over = stored.Find(prop.name);
        JsonValue v = over ? *over : defaults.Get(prop.name);
        PropertyLabel(PrettyName(prop.name), prop.tooltip ? StringView(prop.tooltip) : StringView(), over != nullptr);
        bool edited = false;
        switch (prop.type) {
        case game::PropType::Float: {
            f32 f = v.AsFloat();
            edited = prop.max > prop.min ? ui::SliderFloat("##v", &f, prop.min, prop.max, "{:.2f}") : ui::DragFloat("##v", &f, 0.05f);
            if (edited)
                v = JsonValue(f);
            break;
        }
        case game::PropType::Int: {
            i32 n = i32(v.AsInt());
            edited = ui::DragInt("##v", &n, 0.1f, i32(prop.min), i32(prop.max));
            if (edited)
                v = JsonValue(n);
            break;
        }
        case game::PropType::Bool: {
            bool b = v.AsBool();
            edited = ui::Checkbox("##v", &b);
            if (edited)
                v = JsonValue(b);
            break;
        }
        case game::PropType::Vec3: {
            f32 x[3] = {v.Size() == 3 ? v[0].AsFloat() : 0.0f, v.Size() == 3 ? v[1].AsFloat() : 0.0f, v.Size() == 3 ? v[2].AsFloat() : 0.0f};
            edited = ui::DragFloat3("##v", x, 0.01f);
            if (edited) {
                v = JsonValue::MakeArray();
                for (f32 c : x)
                    v.PushBack(c);
            }
            break;
        }
        }
        if (ui::BeginPopupContextItem("##prop_menu")) {
            if (ui::MenuItem(ZX_ICON_ROTATE_CCW "  Reset to Default", {}, false, over != nullptr)) {
                v = defaults.Get(prop.name);
                edited = true;
            }
            ui::EndPopup();
        }
        if (edited) {
            SetNativeScriptProperty(s, *desc, prop.name, v);
            changed = true;
        }
        ui::PopID();
    }
    return changed;
}

bool IsInlineStruct(const TypeInfo& t)
{
    if (t.kind != TypeKind::Struct)
        return true;
    const StringView n = t.name.View();
    return n == "Vec3" || n == "Vec2" || n == "Quat" || n == "LinearColor";
}

} // namespace

bool PropertyEditor(const TypeInfo& type, void* object, StringView idPrefix)
{
    struct DepthScope
    {
        DepthScope() { ++g_depth; }
        ~DepthScope() { --g_depth; }
    } depth;
    bool changed = false;
    StringView category;
    for (const TypeInfo* t = &type; t; t = t->base) {
        for (const FieldInfo& f : t->fields) {
            if (HasAnyFlags(f.flags, FieldFlags::Hidden) || f.meta.Has(Meta::KeyHidden()))
                continue;
            // Category: a section header when it changes.
            const StringView cat = f.meta.GetText(Meta::KeyCategory());
            if (!cat.Empty() && cat != category)
                ui::SeparatorText(cat);
            category = cat;
            ui::PushID(f.name.View());
            // EditCondition: the named bool field is drawn as a checkbox in front; the value is editable when it's set.
            bool* condition = nullptr;
            if (const StringView condName = f.meta.GetText(Meta::KeyEditCondition()); !condName.Empty())
                for (const TypeInfo* ct = &type; ct && !condition; ct = ct->base)
                    for (const FieldInfo& cf : ct->fields)
                        if (cf.name.View() == condName && cf.type->kind == TypeKind::Bool)
                            condition = static_cast<bool*>(cf.Ptr(object));
            const bool readOnly = HasAnyFlags(f.flags, FieldFlags::ReadOnly) || f.meta.Has(Meta::KeyReadOnly()) || (condition && !*condition);
            const String id = String::Format("##{}{}", idPrefix, f.name.View());
            void* p = f.Ptr(object);
            if (IsInlineStruct(*f.type) || f.type->kind == TypeKind::Array) {
                bool conditionChanged = false;
                PropertyLabel(f.displayName, f.meta.GetText(Meta::KeyTooltip()), IsOverridden(f.name.View()) || (condition && *condition), condition,
                              &conditionChanged);
                changed |= conditionChanged;
                if (readOnly && f.type->kind != TypeKind::Array && f.type->kind != TypeKind::Guid)
                    ui::BeginDisabled(true);
                changed |= EditValue(&f, *f.type, p, id, f.displayName);
                if (readOnly && f.type->kind != TypeKind::Array && f.type->kind != TypeKind::Guid)
                    ui::EndDisabled();
            } else {
                changed |= EditStruct(&f, *f.type, p, id, f.displayName);
            }
            ui::PopID();
        }
    }
    return changed;
}

void DrawDetailsPanel(Editor& ed)
{
    if (!ui::Begin(DetailsTitle, &ed.showDetails)) {
        ui::End();
        return;
    }
    const ui::Theme& c = ui::GetCurrentContext()->GetStyle().colors;
    if (!ed.world.IsAlive(ed.selected)) {
        const Rect r = ui::GetWindowContentRect();
        ui::GetWindowDrawList().AddTextInRect(ui::GetCurrentContext()->Fonts().FindFont("Inter"),
                                              12.5f * ui::GetCurrentContext()->GetScale(), r, c.textDisabled,
                                              "Select an entity to see its details", ui::TextAlign::Center, false);
        ui::End();
        return;
    }
    const ecs::Entity e = ed.selected;
    g_editor = &ed;
    // Header: name + add component.
    scene::EntityName* name = ed.world.Get<scene::EntityName>(e);
    if (!name)
        name = ed.world.Add(e, scene::EntityName{String::Format("Entity {}", e.index)});
    // State before this frame's in-place edits (an edit opens an undo step that lasts until the widget is released).
    const JsonValue before = SerializeEntity(ed.world, e);
    name = ed.world.Get<scene::EntityName>(e);
    const f32 addW = 132.0f;
    ui::SetNextItemWidth(-(addW + 6.0f));
    if (ui::InputText("##entity_name", name->value))
        ed.BeginContinuousEdit("Rename", e, &before);
    ui::SameLine(0.0f, 6.0f);
    if (ui::Button(ZX_ICON_PLUS "  Add Component", Vec2(addW, 0.0f), ui::ButtonVariant::Primary))
        ui::OpenPopup("##add_component");
    if (ui::BeginPopup("##add_component")) {
        ui::TextDisabled("Components");
        ui::Separator();
        bool any = false;
        for (const ComponentType& ct : GetComponentTypes()) {
            if (!ct.userAddable || ed.world.HasComponent(e, ct.id))
                continue;
            any = true;
            if (ui::MenuItem(String::Format("{}  {}", ct.icon, ct.displayName))) {
                ed.undo.Begin(String::Format("Add {}", ct.displayName));
                ed.undo.Record(e);
                ed.world.AddComponent(e, ct.id);
                ed.undo.End();
            }
        }
        if (!any)
            ui::TextDisabled("(all components added)");
        ui::EndPopup();
    }
    ui::TextDisabled(String::Format("Entity #{}  (generation {})", e.index, e.generation));
    ui::Spacing();

    // Prefab bar: source asset, override count, Select / Revert / Apply.
    Array<String> overrides;
    if (const ecs::Entity instance = FindPrefabInstanceRoot(ed.world, e); instance && ed.prefabs) {
        GetPrefabOverrides(ed.world, e, *ed.prefabs, overrides);
        const PrefabInstance* pi = ed.world.Get<const PrefabInstance>(instance);
        ui::Context& ctx = *ui::GetCurrentContext();
        const f32 s = ctx.GetScale();
        const Vec2 p = ui::GetCursorPos();
        const f32 h = Round(ui::GetFrameHeight() + 10.0f * s);
        const Rect bar(p, Vec2(p.x + ui::GetContentRegionAvail().x, p.y + h));
        ui::GetWindowDrawList().AddRect(bar, WithAlpha(Color(0x3B, 0x8C, 0xF5), 0.12f), ui::CornerRadii(4.0f * s), 1.0f,
                                        WithAlpha(Color(0x3B, 0x8C, 0xF5), 0.45f));
        ui::SetCursorPos(p + Vec2(Round(8.0f * s), Round(5.0f * s)));
        ui::AlignTextToFramePadding();
        ui::PushTextColor(Color(0x6C, 0xB4, 0xF5));
        ui::Text(String::Format(ZX_ICON_PACKAGE "  {}", pi ? Path::Stem(pi->prefab) : StringView("Prefab")));
        ui::PopTextColor();
        ui::ItemTooltip(pi ? pi->prefab.View() : StringView());
        ui::SameLine();
        ui::TextDisabled(overrides.Empty() ? String("no overrides") : String::Format("{} override{}", overrides.Size(), overrides.Size() == 1 ? "" : "s"));
        const f32 bw = 64.0f;
        ui::SameLine(Max(0.0f, (bar.Width() / s) - bw * 2.0f - 22.0f));
        if (ui::Button("Revert##prefab", Vec2(bw, 0.0f)))
            ed.RevertPrefab(e);
        ui::ItemTooltip("Discard this instance's overrides");
        ui::SameLine(0.0f, 4.0f);
        if (ui::Button("Apply##prefab", Vec2(bw, 0.0f), ui::ButtonVariant::Primary))
            ed.ApplyPrefab(e);
        ui::ItemTooltip("Write this instance's changes into the prefab (updates every instance)");
        ui::SetCursorPos(Vec2(p.x, bar.max.y));
        ui::Spacing();
        if (!ed.world.IsAlive(e)) { // rebuilt by revert/apply: the selection moved to the new entity
            ui::End();
            return;
        }
    }
    g_overrides = overrides.Empty() ? nullptr : &overrides;

    // Components in registry order.
    ecs::ComponentId toRemove = ecs::InvalidComponentId;
    for (const ComponentType& ct : GetComponentTypes()) {
        if (ct.hiddenInDetails || !ed.world.HasComponent(e, ct.id))
            continue;
        void* data = ed.world.GetComponent(e, ct.id, false);
        if (!data)
            continue;
        ui::PushID(ct.displayName);
        const bool open = ui::CollapsingHeader(String::Format("{}  {}", ct.icon, ct.displayName), ui::TreeNodeFlags::DefaultOpen);
        if (ui::BeginPopupContextItem()) {
            if (ui::MenuItem(ZX_ICON_ROTATE_CCW "  Reset")) {
                ed.undo.Begin(String::Format("Reset {}", ct.displayName));
                ed.undo.Record(e);
                ct.type->destruct(data);
                ct.type->construct(data);
                ed.undo.End();
            }
            if (ui::MenuItem(ZX_ICON_TRASH_2 "  Remove Component", {}, false, ct.removable))
                toRemove = ct.id;
            ui::EndPopup();
        }
        if (open) {
            ui::Indent(4.0f);
            g_component = ct.type->name.View();
            bool edited = false;
            if (ct.type == &TypeOf<Script>())
                edited = EditScript(ed, *static_cast<Script*>(data));
            else if (ct.type == &TypeOf<NativeScript>())
                edited = EditNativeScript(ed, *static_cast<NativeScript*>(data));
            else if (ct.type == &TypeOf<AudioSource>())
                edited = EditAudioSourceClip(ed, *static_cast<AudioSource*>(data)) | PropertyEditor(*ct.type, data);
            else
                edited = PropertyEditor(*ct.type, data);
            if (edited) {
                ed.world.GetComponent(e, ct.id, true); // mark changed for systems
                ed.BeginContinuousEdit(String::Format("Edit {}", ct.displayName), e, &before);
            }
            ui::Unindent(4.0f);
            ui::Spacing();
        }
        ui::PopID();
    }
    g_overrides = nullptr;
    if (toRemove != ecs::InvalidComponentId) {
        ed.undo.Begin(String::Format("Remove {}", FindComponentType(toRemove)->displayName));
        ed.undo.Record(e);
        ed.world.RemoveComponent(e, toRemove);
        ed.undo.End();
    }
    ui::End();
}

} // namespace zx::editor
