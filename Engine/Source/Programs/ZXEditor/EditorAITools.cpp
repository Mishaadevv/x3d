// The editor operations an AI may perform (built-in agent and MCP clients). Calls arrive on worker threads and run on
// the main thread between frames (EditorToolHost::Pump); every scene edit is one undo step labelled "AI: ...".

#include "EditorAI.h"
#include "Editor.h"
#include "Core/Console/Console.h"
#include "Core/Containers/Algorithm.h"
#include "Core/FileSystem/FileSystem.h"
#include "Core/HAL/PlatformFile.h"
#include "Core/Image/Png.h"
#include "Core/Reflection/ReflectionSerialization.h"
#include "Core/String/Base64.h"
#include "Core/String/Path.h"
#include "Core/String/Utf.h"
#include "Core/Version.h"
#include "Engine/ComponentRegistry.h"
#include "Engine/PhysicsSystem.h"
#include "Engine/SceneSerialization.h"

ZX_DEFINE_LOG_CATEGORY_STATIC(LogAITools, Info);

namespace zx::editor {

using ai::ToolResult;

struct EditorToolHost::Job
{
    String name;
    JsonValue args;
    ToolResult result;
    Event done{true};
    std::atomic<u32> refs{2}; // the caller and the main thread
    void Release()
    {
        if (refs.fetch_sub(1) == 1)
            delete this;
    }
};

namespace {

// --- Helpers --------------------------------------------------------------------------------------------------------

JsonValue Schema(StringView json)
{
    JsonValue v;
    const bool ok = ParseJson(json, v);
    ZX_ASSERT(ok);
    (void)ok;
    return v;
}

JsonValue Vec3Json(Vec3 v)
{
    JsonValue a = JsonValue::MakeArray();
    a.PushBack(JsonValue(f64(Round(v.x * 1000.0f) / 1000.0f)));
    a.PushBack(JsonValue(f64(Round(v.y * 1000.0f) / 1000.0f)));
    a.PushBack(JsonValue(f64(Round(v.z * 1000.0f) / 1000.0f)));
    return a;
}

bool ReadVec3(const JsonValue& v, Vec3& out)
{
    if (v.IsArray() && v.Size() == 3 && v[0].IsNumber() && v[1].IsNumber() && v[2].IsNumber()) {
        out = Vec3(v[0].AsFloat(), v[1].AsFloat(), v[2].AsFloat());
        return true;
    }
    if (v.IsNumber()) { // uniform scale
        out = Vec3(v.AsFloat());
        return true;
    }
    if (v.IsObject() && v.Contains("x")) {
        out = Vec3(v.Get("x").AsFloat(), v.Get("y").AsFloat(), v.Get("z").AsFloat());
        return true;
    }
    return false;
}

String Json(const JsonValue& v) { return ToJsonString(v, JsonWriteOptions{.pretty = true, .indent = 1}); }

String NameOf(Editor& ed, ecs::Entity e)
{
    const scene::EntityName* n = ed.world.Get<scene::EntityName>(e);
    return n ? n->value : String();
}

/// An entity by guid or by exact name (case-insensitive when unique).
ecs::Entity ResolveEntity(Editor& ed, const JsonValue& ref, String& error)
{
    const StringView text = ref.AsString().Trim();
    if (text.Empty()) {
        error = "No entity given (use a guid or a name from list_entities).";
        return {};
    }
    if (const Optional<Guid> g = Guid::Parse(text))
        if (const ecs::Entity e = FindEntityByGuid(ed.world, *g))
            return e;
    ecs::Entity found, foundNoCase;
    u32 matches = 0, noCase = 0;
    ed.world.Query<const scene::EntityName>().Each([&](ecs::Entity e, const scene::EntityName& n) {
        if (n.value == text) {
            found = e;
            ++matches;
        } else if (n.value.EqualsIgnoreCase(text)) {
            foundNoCase = e;
            ++noCase;
        }
    });
    if (matches == 1)
        return found;
    if (matches == 0 && noCase == 1)
        return foundNoCase;
    error = matches + noCase > 1 ? String::Format("{} entities are named '{}': use the guid (list_entities).", matches + noCase, text)
                                 : String::Format("No entity '{}' (list_entities shows them).", text);
    return {};
}

/// Component JSON key from a type name or display name ("PointLight", "Point Light", "pointlight").
const ComponentType* FindComponent(StringView name)
{
    if (const ComponentType* c = FindComponentType(name))
        return c;
    String squeezed(name);
    squeezed.ReplaceAll(" ", "");
    for (const ComponentType& c : GetComponentTypes()) {
        String display(c.displayName);
        display.ReplaceAll(" ", "");
        if (c.type->name.View().EqualsIgnoreCase(squeezed) || display.EqualsIgnoreCase(squeezed))
            return &c;
    }
    return nullptr;
}

String ComponentList()
{
    String s;
    for (const ComponentType& c : GetComponentTypes())
        if (c.userAddable)
            s.Appendf("{}{}", s.Empty() ? "" : ", ", c.type->name.View());
    return s;
}

String TypeLabel(const TypeInfo& t)
{
    switch (t.kind) {
    case TypeKind::Bool: return "bool";
    case TypeKind::Float:
    case TypeKind::Double: return "number";
    case TypeKind::String:
    case TypeKind::Name: return "string";
    case TypeKind::Guid: return "guid";
    case TypeKind::Enum: {
        String s = "enum (";
        for (usize i = 0; i < t.enumEntries.Size(); ++i)
            s.Appendf("{}{}", i ? "|" : "", t.enumEntries[i].name.View());
        return s + ")";
    }
    case TypeKind::Array: return String::Format("array of {}", t.element ? TypeLabel(*t.element) : String("?"));
    case TypeKind::Struct:
        if (t.name == "Vec3" || t.name == "Vec2" || t.name == "Vec4" || t.name == "Quat")
            return String::Format("{} [numbers]", t.name.View());
        if (t.name == "Guid" || t.name.View().Contains("AssetId"))
            return "asset guid";
        return String::Format("object {}", t.name.View());
    default: return IsIntegerKind(t.kind) ? String("integer") : String(t.name.View());
    }
}

String DescribeComponent(const ComponentType& c)
{
    String s = String::Format("## {} (\"{}\")\n", c.displayName, c.type->name.View());
    if (const StringView tip = c.type->meta.GetText(Meta::KeyTooltip()); !tip.Empty())
        s.Appendf("{}\n", tip);
    const void* defaults = Reflection::GetDefaultObject(*c.type);
    c.type->ForEachField([&](const FieldInfo& f) {
        if (f.IsTransient() || HasAnyFlags(f.flags, FieldFlags::Hidden))
            return;
        String def = ToJsonString(Reflection::ToJson(f.Ptr(defaults), *f.type), JsonWriteOptions{.pretty = false});
        if (def.Size() > 80)
            def = def.SubStr(0, 77) + "...";
        s.Appendf("- {}: {} = {}", f.name.View(), TypeLabel(*f.type), def);
        if (const MetaValue* units = f.meta.Find(Meta::KeyUnits()))
            s.Appendf(" ({})", units->text);
        if (const StringView tip = f.meta.GetText(Meta::KeyTooltip()); !tip.Empty())
            s.Appendf(" - {}", tip);
        s += "\n";
    });
    return s;
}

/// Project root for file tools: the open project, else the engine samples' Content.
String FileRoot(Editor& ed) { return ed.project.descriptorPath.Empty() ? ed.contentRoot : ed.project.root; }

bool ResolvePath(Editor& ed, StringView rel, String& physical, String& error)
{
    String p(rel.Trim());
    p.ReplaceChar('\\', '/');
    if (p.Empty() || p == "." || p == "/")
        p.Clear();
    if (Path::IsAbsolute(p) || p.Contains(':')) {
        error = "Use a path relative to the project folder (e.g. Content/Scripts/Player.zs).";
        return false;
    }
    const String root = Path::Normalize(FileRoot(ed));
    physical = Path::Normalize(p.Empty() ? root : Path::Join(root, p));
    if (physical != root && !Path::IsUnder(physical, root)) {
        error = "The path leaves the project folder.";
        return false;
    }
    return true;
}

bool IsTextFile(StringView path)
{
    static constexpr StringView Ext[] = {".zs", ".zscene", ".zmat", ".zmati", ".zprefab", ".zgraph", ".zbt", ".zseq", ".zloc",
                                         ".zproj", ".zui", ".json", ".md", ".txt", ".ini", ".csv", ".h", ".hpp", ".cpp", ".c",
                                         ".hlsl", ".hlsli", ".cmake", ".xml", ".yaml", ".yml", ".zanim", ".zcurve"};
    for (StringView e : Ext)
        if (Path::HasExtension(path, e))
            return true;
    return Path::FileName(path) == "CMakeLists.txt";
}

bool IsGeneratedDir(StringView rel)
{
    for (StringView d : {StringView("Saved"), StringView("Intermediate"), StringView("Binaries"), StringView("Builds"), StringView(".git")})
        if (rel == d || (rel.StartsWith(d) && rel.Size() > d.Size() && rel[d.Size()] == '/'))
            return true;
    return false;
}

String LogSince(Editor& ed, usize start, bool includeInfo = true)
{
    String out;
    usize i = 0;
    ed.log->ForEach([&](const EditorLogSink::Entry& e) {
        if (i++ < start)
            return;
        if (!includeInfo && e.level < LogLevel::Warning)
            return;
        const char* level = e.level >= LogLevel::Error ? "error" : e.level == LogLevel::Warning ? "warning" : "info";
        out.Appendf("[{}] {}: {}\n", level, e.category, e.text);
    });
    if (out.Size() > 12000)
        out = "...\n" + out.SubStr(out.Size() - 12000);
    return out;
}

/// Downscales RGBA rows to at most maxWidth (box filter) and encodes an RGB PNG as base64.
String EncodeScreenshot(const u8* pixels, usize pitch, u32 x0, u32 y0, u32 w, u32 h, u32 maxWidth)
{
    const u32 factor = Max(1u, (w + maxWidth - 1) / maxWidth);
    const u32 ow = Max(1u, w / factor), oh = Max(1u, h / factor);
    Array<u8> rgb;
    rgb.Resize(usize(ow) * oh * 3);
    for (u32 y = 0; y < oh; ++y)
        for (u32 x = 0; x < ow; ++x) {
            u32 sum[3] = {};
            for (u32 dy = 0; dy < factor; ++dy) {
                const u8* row = pixels + usize(y0 + y * factor + dy) * pitch + usize(x0 + x * factor) * 4;
                for (u32 dx = 0; dx < factor; ++dx)
                    for (u32 c = 0; c < 3; ++c)
                        sum[c] += row[dx * 4 + c];
            }
            for (u32 c = 0; c < 3; ++c)
                rgb[(usize(y) * ow + x) * 3 + c] = u8(sum[c] / (factor * factor));
        }
    const Array<u8> png = png::Encode(rgb.Data(), ow, oh, 3);
    return Base64Encode(png.View());
}

// --- Tools ----------------------------------------------------------------------------------------------------------

ToolResult EditorState(Editor& ed, const JsonValue&)
{
    JsonValue s = JsonValue::MakeObject();
    s.Set("engine", String::Format("ZeqouX3D {}", EngineVersionString));
    s.Set("profile", ed.lite ? "Lite" : "Full");
    if (ed.project.descriptorPath.Empty()) {
        s.Set("project", JsonValue());
        s.Set("note", "No project is open (the editor shows the Project Browser or the engine samples). The user opens or creates a project.");
    } else {
        s["project"].Set("name", ed.project.descriptor.name);
        s["project"].Set("folder", ed.project.root);
        s["project"].Set("language", ed.project.descriptor.language);
        s["project"].Set("startupScene", ed.project.descriptor.startupScene);
    }
    s["scene"].Set("name", ed.sceneName);
    s["scene"].Set("file", ed.scenePath.Empty() ? String("(unsaved)") : Path::MakeRelative(ed.scenePath, FileRoot(ed)));
    s["scene"].Set("unsavedChanges", ed.undo.IsDirty());
    s["scene"].Set("entities", u64(ed.world.EntityCount()));
    s.Set("playing", ed.IsPlaying());
    s.Set("paused", ed.playState == PlayState::Paused);
    JsonValue sel = JsonValue::MakeArray();
    for (ecs::Entity e : ed.selection)
        sel.PushBack(String::Format("{} ({})", NameOf(ed, e), GetEntityGuid(ed.world, e)));
    s.Set("selection", move(sel));
    s["camera"].Set("position", Vec3Json(ed.camera.position));
    s["camera"].Set("forward", Vec3Json(ed.camera.Forward()));
    s["camera"].Set("spawnPoint", Vec3Json(ed.camera.position + ed.camera.Forward() * 5.0f));
    s.Set("errorsInLog", u64(ed.log->Counts(LogLevel::Error) + ed.log->Counts(LogLevel::Fatal)));
    return ToolResult::Ok(Json(s));
}

ToolResult GetDocs(Editor& ed, const JsonValue& args)
{
    const StringView topic = args.Get("topic").AsString("overview");
    if (topic == "overview")
        return ToolResult::Ok(McpInstructions());
    if (topic == "components") {
        const StringView name = args.Get("component").AsString();
        if (!name.Empty()) {
            const ComponentType* c = FindComponent(name);
            return c ? ToolResult::Ok(DescribeComponent(*c)) : ToolResult::Error(String::Format("Unknown component '{}'. Components: {}", name, ComponentList()));
        }
        String s = "# Components\nPass `component` for the fields of one. JSON key (display name): description\n";
        for (const ComponentType& c : GetComponentTypes()) {
            if (!c.userAddable && c.type->name != "Transform")
                continue;
            s.Appendf("- {} ({})", c.type->name.View(), c.displayName);
            if (const StringView tip = c.type->meta.GetText(Meta::KeyTooltip()); !tip.Empty())
                s.Appendf(": {}", tip);
            s += "\n";
        }
        return ToolResult::Ok(s);
    }
    if (topic == "console") {
        const StringView filter = args.Get("filter").AsString();
        String s = "# Console commands (run_console_command)\n";
        Array<const ConsoleObject*> objects;
        for (const ConsoleObject* o = Console::GetObjects(); o; o = o->NextObject())
            if (o->IsCommand() && (filter.Empty() || StringView(o->GetName()).Contains(filter)))
                objects.PushBack(o);
        Sort(objects, [](const ConsoleObject* a, const ConsoleObject* b) { return StringView(a->GetName()) < StringView(b->GetName()); });
        for (const ConsoleObject* o : objects)
            s.Appendf("- {}: {}\n", o->GetName(), o->GetHelp());
        return ToolResult::Ok(s);
    }
    static constexpr StringView Docs[][2] = {{"project", "agents"}, {"zeqouscript", "zeqouscript"}, {"cpp", "cpp"}, {"getting_started", "getting_started"}};
    for (const auto& d : Docs)
        if (topic == d[0]) {
            ProjectInfo p = ed.project;
            if (p.descriptor.name.Empty())
                p.descriptor.name = "MyGame";
            return ToolResult::Ok(ProjectDocText(p, d[1]));
        }
    return ToolResult::Error("Unknown topic. Topics: overview, components, console, zeqouscript, cpp, project, getting_started");
}

ToolResult ListEntities(Editor& ed, const JsonValue& args)
{
    const StringView filter = args.Get("filter").AsString();
    const usize limit = usize(Clamp<i64>(args.Get("limit").AsInt(300), 1, 2000));
    String out;
    usize count = 0, total = 0;
    // Depth-first from the roots so the hierarchy reads naturally.
    Array<ecs::Entity> roots;
    CollectRoots(ed.world, roots);
    Array<ecs::Entity> all;
    for (ecs::Entity r : roots)
        CollectHierarchy(ed.world, r, all);
    for (ecs::Entity e : all) {
        const String name = NameOf(ed, e);
        String components;
        for (const ComponentType& c : GetComponentTypes())
            if (c.type->name != "EntityName" && c.type->name != "Transform" && ed.world.GetComponent(e, c.id, false))
                components.Appendf("{}{}", components.Empty() ? "" : ", ", c.type->name.View());
        if (!filter.Empty() && !String(name).ToLower().Contains(String(filter).ToLower()) && !components.ToLower().Contains(String(filter).ToLower()))
            continue;
        ++total;
        if (count >= limit)
            continue;
        ++count;
        u32 depth = 0;
        for (ecs::Entity p = scene::GetParent(ed.world, e); p; p = scene::GetParent(ed.world, p))
            ++depth;
        const Vec3 pos = scene::ComputeWorldMatrix(ed.world, e).GetTranslation();
        out.Appendf("{}- {} [{}] at ({:.2f}, {:.2f}, {:.2f}){}{}\n", String(depth * 2, ' '), name.Empty() ? String("(unnamed)") : name,
                    EnsureEntityGuid(ed.world, e), pos.x, pos.y, pos.z, components.Empty() ? "" : ": ", components);
    }
    if (total > count)
        out.Appendf("... {} more (use filter / limit)\n", total - count);
    if (out.Empty())
        out = filter.Empty() ? "The scene is empty." : "No entity matches.";
    return ToolResult::Ok(out);
}

ToolResult GetEntity(Editor& ed, const JsonValue& args)
{
    String error;
    const ecs::Entity e = ResolveEntity(ed, args.Get("entity"), error);
    if (!e)
        return ToolResult::Error(error);
    JsonValue s = SerializeEntity(ed.world, e);
    s.Set("worldPosition", Vec3Json(scene::ComputeWorldMatrix(ed.world, e).GetTranslation()));
    JsonValue children = JsonValue::MakeArray();
    for (ecs::Entity c : scene::GetChildren(ed.world, e))
        children.PushBack(String::Format("{} ({})", NameOf(ed, c), EnsureEntityGuid(ed.world, c)));
    s.Set("children", move(children));
    if (const Transform* t = ed.world.Get<Transform>(e))
        s.Set("rotationEulerDegrees", Vec3Json(t->rotation.ToEulerDegrees()));
    return ToolResult::Ok(Json(s));
}

/// Applies name / transform shortcuts and component changes to a snapshot. Returns false with an error.
bool EditSnapshot(JsonValue& snapshot, const JsonValue& args, Array<String>& notes, String& error)
{
    JsonValue& comps = snapshot["components"];
    if (const JsonValue* name = args.Find("name"); name && name->IsString())
        comps["EntityName"].Set("value", *name);
    JsonValue& tr = comps["Transform"];
    if (tr.IsNull())
        tr = JsonValue::MakeObject();
    Vec3 v;
    if (const JsonValue* p = args.Find("position")) {
        if (!ReadVec3(*p, v)) {
            error = "position must be [x, y, z] (meters)";
            return false;
        }
        tr.Set("position", Vec3Json(v));
    }
    if (const JsonValue* r = args.Find("rotation")) {
        if (!ReadVec3(*r, v)) {
            error = "rotation must be [pitch, yaw, roll] in degrees";
            return false;
        }
        const Quat q = Quat::FromEulerDegrees(v);
        JsonValue a = JsonValue::MakeArray();
        a.PushBack(JsonValue(q.x));
        a.PushBack(JsonValue(q.y));
        a.PushBack(JsonValue(q.z));
        a.PushBack(JsonValue(q.w));
        tr.Set("rotation", move(a));
    }
    if (const JsonValue* s = args.Find("scale")) {
        if (!ReadVec3(*s, v)) {
            error = "scale must be [x, y, z] or one number";
            return false;
        }
        tr.Set("scale", Vec3Json(v));
    }
    if (const JsonValue* components = args.Find("components")) {
        if (!components->IsObject()) {
            error = "components must be an object: { \"PointLight\": { \"lumens\": 1200 } }";
            return false;
        }
        for (const JsonMember& m : components->Members()) {
            const ComponentType* c = FindComponent(m.first);
            if (!c) {
                error = String::Format("Unknown component '{}'. Components: {}", m.first, ComponentList());
                return false;
            }
            const StringView key = c->type->name.View();
            if (m.second.IsNull()) {
                if (!c->removable) {
                    error = String::Format("{} cannot be removed", key);
                    return false;
                }
                comps.Remove(key);
                continue;
            }
            if (!m.second.IsObject()) {
                error = String::Format("{} must be an object of fields (or null to remove it)", key);
                return false;
            }
            JsonValue& target = comps[key];
            if (!target.IsObject())
                target = JsonValue::MakeObject();
            for (const JsonMember& f : m.second.Members()) {
                if (!c->type->FindField(Name(f.first)))
                    notes.PushBack(String::Format("{} has no field '{}' (ignored; get_docs components {} lists them)", key, f.first, key));
                target.Set(f.first, f.second);
            }
        }
    }
    return true;
}

/// Built-in shape: a mesh renderer with the shape plus the matching collider.
bool AddShape(JsonValue& comps, StringView shape, String& error)
{
    BuiltinShape s;
    if (shape.EqualsIgnoreCase("cube") || shape.EqualsIgnoreCase("box"))
        s = BuiltinShape::Cube;
    else if (shape.EqualsIgnoreCase("sphere"))
        s = BuiltinShape::Sphere;
    else if (shape.EqualsIgnoreCase("cylinder"))
        s = BuiltinShape::Cylinder;
    else if (shape.EqualsIgnoreCase("plane"))
        s = BuiltinShape::Plane;
    else {
        error = String::Format("Unknown shape '{}' (cube, sphere, cylinder, plane)", shape);
        return false;
    }
    MeshRenderer mr;
    mr.mesh = builtin::Mesh(s);
    JsonValue m = Reflection::ToJson(mr, true);
    if (JsonValue* existing = comps.Find("MeshRenderer"))
        for (const JsonMember& f : existing->Members())
            m.Set(f.first, f.second);
    comps.Set("MeshRenderer", move(m));
    switch (s) {
    case BuiltinShape::Sphere: comps["SphereCollider"] = JsonValue::MakeObject(); break;
    case BuiltinShape::Cylinder: comps["CylinderCollider"] = JsonValue::MakeObject(); break;
    case BuiltinShape::Plane: comps.Set("BoxCollider", Reflection::ToJson(BoxCollider{Vec3(0.0f, -0.005f, 0.0f), Vec3(1.0f, 0.01f, 1.0f)}, true)); break;
    default: comps["BoxCollider"] = JsonValue::MakeObject(); break;
    }
    return true;
}

ToolResult CreateEntities(Editor& ed, const JsonValue& args)
{
    if (ed.IsPlaying())
        return ToolResult::Error("The editor is playing: stop first (play_control stop) - edits made while playing are discarded.");
    const JsonValue& list = args.Get("entities");
    if (!list.IsArray() || list.Size() == 0)
        return ToolResult::Error("entities must be a non-empty array");
    Array<EntitySnapshot> snapshots;
    Array<String> notes;
    HashMap<String, Guid> batchNames; // parents created in this call
    const Vec3 spawn = ed.camera.position + ed.camera.Forward() * 5.0f;
    for (const JsonValue& item : list.Elements()) {
        if (!item.IsObject())
            return ToolResult::Error("each entity must be an object");
        JsonValue snap = JsonValue::MakeObject();
        const Guid guid = Guid::New();
        snap.Set("guid", String::Format("{}", guid));
        JsonValue& comps = snap["components"];
        comps = JsonValue::MakeObject();
        comps["EntityName"].Set("value", item.Get("name").AsString("Entity"));
        comps["Transform"].Set("position", Vec3Json(spawn));
        String error;
        if (const StringView shape = item.Get("shape").AsString(); !shape.Empty() && !AddShape(comps, shape, error))
            return ToolResult::Error(error);
        if (!EditSnapshot(snap, item, notes, error))
            return ToolResult::Error(String::Format("{}: {}", item.Get("name").AsString("entity"), error));
        if (const JsonValue* parent = item.Find("parent"); parent && !parent->AsString().Empty()) {
            if (const Guid* g = batchNames.Find(String(parent->AsString()))) {
                snap.Set("parent", String::Format("{}", *g));
            } else {
                const ecs::Entity p = ResolveEntity(ed, *parent, error);
                if (!p)
                    return ToolResult::Error(String::Format("parent: {}", error));
                snap.Set("parent", String::Format("{}", EnsureEntityGuid(ed.world, p)));
            }
        }
        batchNames.InsertOrAssign(String(item.Get("name").AsString("Entity")), guid);
        snapshots.PushBack(EntitySnapshot{guid, move(snap)});
    }
    Array<String> warnings;
    ed.undo.Begin(snapshots.Size() == 1 ? String("AI: Create") : String::Format("AI: Create {} objects", snapshots.Size()));
    const Array<ecs::Entity> created = ApplyEntitySnapshots(ed.world, snapshots.View(), &warnings);
    for (ecs::Entity e : created)
        if (e)
            ed.undo.RecordCreated(e);
    ed.undo.End();
    ed.selection.Clear();
    String out = "Created:\n";
    for (ecs::Entity e : created)
        if (e) {
            ed.selection.PushBack(e);
            out.Appendf("- {} [{}]\n", NameOf(ed, e), GetEntityGuid(ed.world, e));
        }
    ed.selected = ed.selection.Empty() ? ecs::NullEntity : ed.selection.Back();
    for (const String& w : warnings)
        out.Appendf("warning: {}\n", w);
    for (const String& n : notes)
        out.Appendf("note: {}\n", n);
    return ToolResult::Ok(out);
}

ToolResult ModifyEntity(Editor& ed, const JsonValue& args)
{
    if (ed.IsPlaying())
        return ToolResult::Error("The editor is playing: stop first (play_control stop) - edits made while playing are discarded.");
    String error;
    const ecs::Entity e = ResolveEntity(ed, args.Get("entity"), error);
    if (!e)
        return ToolResult::Error(error);
    const Guid guid = EnsureEntityGuid(ed.world, e);
    JsonValue snap = SerializeEntity(ed.world, e);
    Array<String> notes;
    if (const StringView shape = args.Get("shape").AsString(); !shape.Empty() && !AddShape(snap["components"], shape, error))
        return ToolResult::Error(error);
    if (!EditSnapshot(snap, args, notes, error))
        return ToolResult::Error(error);
    if (const JsonValue* parent = args.Find("parent")) {
        if (parent->IsNull() || parent->AsString().Empty()) {
            snap.Remove("parent");
        } else {
            const ecs::Entity p = ResolveEntity(ed, *parent, error);
            if (!p)
                return ToolResult::Error(String::Format("parent: {}", error));
            if (p == e || scene::IsAncestorOf(ed.world, e, p))
                return ToolResult::Error("That parent would create a cycle.");
            snap.Set("parent", String::Format("{}", EnsureEntityGuid(ed.world, p)));
        }
    }
    Array<String> warnings;
    ed.undo.Begin(String::Format("AI: Edit {}", NameOf(ed, e)));
    ed.undo.Record(e);
    const EntitySnapshot s{guid, move(snap)};
    ApplyEntitySnapshots(ed.world, Span<const EntitySnapshot>(&s, 1), &warnings);
    ed.undo.End();
    const ecs::Entity after = FindEntityByGuid(ed.world, guid);
    String out = String::Format("Updated {} [{}].\n", NameOf(ed, after), guid);
    for (const String& w : warnings)
        out.Appendf("warning: {}\n", w);
    for (const String& n : notes)
        out.Appendf("note: {}\n", n);
    return ToolResult::Ok(out);
}

ToolResult DeleteEntities(Editor& ed, const JsonValue& args)
{
    if (ed.IsPlaying())
        return ToolResult::Error("The editor is playing: stop first.");
    Array<ecs::Entity> targets;
    for (const JsonValue& ref : args.Get("entities").Elements()) {
        String error;
        const ecs::Entity e = ResolveEntity(ed, ref, error);
        if (!e)
            return ToolResult::Error(error);
        targets.PushBack(e);
    }
    if (targets.Empty())
        return ToolResult::Error("entities must list guids or names");
    ed.undo.Begin(String::Format("AI: Delete {} object(s)", targets.Size()));
    for (ecs::Entity e : targets)
        ed.undo.RecordHierarchy(e);
    for (ecs::Entity e : targets)
        if (ed.world.IsAlive(e))
            scene::DestroyHierarchy(ed.world, e);
    ed.undo.End();
    ed.selection.Clear();
    ed.selected = ecs::NullEntity;
    return ToolResult::Ok(String::Format("Deleted {} object(s) with their children.", targets.Size()));
}

ToolResult SelectEntities(Editor& ed, const JsonValue& args)
{
    ed.selection.Clear();
    for (const JsonValue& ref : args.Get("entities").Elements()) {
        String error;
        const ecs::Entity e = ResolveEntity(ed, ref, error);
        if (!e)
            return ToolResult::Error(error);
        ed.selection.PushBack(e);
    }
    ed.selected = ed.selection.Empty() ? ecs::NullEntity : ed.selection.Back();
    if (args.Get("focus").AsBool(true) && ed.selected)
        ed.FocusSelected();
    return ToolResult::Ok(String::Format("Selected {} object(s).", ed.selection.Size()));
}

ToolResult RunCommand(Editor& ed, const JsonValue& args)
{
    const StringView line = args.Get("command").AsString().Trim();
    if (line.Empty())
        return ToolResult::Error("command is empty (get_docs console lists the commands)");
    for (StringView statement : {line}) {
        const Array<StringView> tokens = Console::Tokenize(statement);
        if (!tokens.Empty() && (tokens[0].EqualsIgnoreCase("quit") || tokens[0].EqualsIgnoreCase("profile.switch") || tokens[0].EqualsIgnoreCase("exit")))
            return ToolResult::Error("This command closes or restarts the editor; ask the user to do it.");
    }
    const usize before = ed.log->Count();
    const bool ok = Console::Execute(line);
    String out = LogSince(ed, before);
    if (out.Empty())
        out = ok ? "Done (no output)." : "Failed.";
    return ok ? ToolResult::Ok(out) : ToolResult::Error(out);
}

ToolResult GetLog(Editor& ed, const JsonValue& args)
{
    const usize count = usize(Clamp<i64>(args.Get("count").AsInt(60), 1, 500));
    const bool errorsOnly = args.Get("errors_only").AsBool(false);
    const usize total = ed.log->Count();
    usize start = total > count * (errorsOnly ? 20 : 1) ? total - count * (errorsOnly ? 20 : 1) : 0;
    String out = LogSince(ed, start, !errorsOnly);
    return ToolResult::Ok(out.Empty() ? String(errorsOnly ? "No warnings or errors." : "The log is empty.") : out);
}

ToolResult ListFiles(Editor& ed, const JsonValue& args)
{
    String dir, error;
    if (!ResolvePath(ed, args.Get("path").AsString(), dir, error))
        return ToolResult::Error(error);
    if (!platform::DirectoryExists(dir))
        return ToolResult::Error("No such folder.");
    fs::ListOptions options;
    options.recursive = args.Get("recursive").AsBool(false);
    Array<platform::DirectoryEntry> entries;
    fs::List(dir, entries, options);
    const String root = Path::Normalize(FileRoot(ed));
    const String base = Path::MakeRelative(dir, root);
    String out;
    usize shown = 0;
    for (const platform::DirectoryEntry& e : entries) {
        const String rel = base.Empty() || base == "." ? e.name : Path::Join(base, e.name);
        if (IsGeneratedDir(rel) || shown >= 800)
            continue;
        ++shown;
        if (e.isDirectory)
            out.Appendf("{}/\n", rel);
        else
            out.Appendf("{}  ({} bytes)\n", rel, e.size);
    }
    if (shown == 800)
        out += "... (more; list a subfolder)\n";
    return ToolResult::Ok(out.Empty() ? String("(empty)") : out);
}

ToolResult ReadFileTool(Editor& ed, const JsonValue& args)
{
    String path, error;
    if (!ResolvePath(ed, args.Get("path").AsString(), path, error))
        return ToolResult::Error(error);
    if (!IsTextFile(path))
        return ToolResult::Error("Only text files can be read (scripts, scenes, materials, JSON, Markdown, C++).");
    String text;
    if (!fs::ReadTextFile(path, text))
        return ToolResult::Error("Cannot read the file (does it exist?).");
    if (text.Size() > 200000)
        return ToolResult::Error(String::Format("The file is {} KB; too large to read whole.", text.Size() / 1024));
    return ToolResult::Ok(text);
}

ToolResult WriteFileTool(Editor& ed, const JsonValue& args)
{
    String path, error;
    if (!ResolvePath(ed, args.Get("path").AsString(), path, error))
        return ToolResult::Error(error);
    const String rel = Path::MakeRelative(path, Path::Normalize(FileRoot(ed)));
    if (!IsTextFile(path))
        return ToolResult::Error("Only text files can be written (.zs, .zscene, .zmat, .zprefab, .zgraph, .json, .md, C++ ...).");
    if (IsGeneratedDir(rel))
        return ToolResult::Error("Saved/, Intermediate/, Binaries/ and Builds/ are generated by the engine.");
    if (!args.Get("content").IsString())
        return ToolResult::Error("content must be a string");
    const bool existed = platform::FileExists(path);
    platform::CreateDirectoryTree(Path::Parent(path));
    if (!fs::WriteTextFile(path, args.Get("content").AsString()))
        return ToolResult::Error("Cannot write the file.");
    ZX_LOG_INFO(LogAITools, "AI {} {}", existed ? "updated" : "created", rel);
    String note;
    if (Path::HasExtension(path, ".zs"))
        note = " Scripts reload automatically; check get_log for compile errors.";
    return ToolResult::Ok(String::Format("{} {} ({} bytes).{}", existed ? "Updated" : "Created", rel, args.Get("content").AsString().Size(), note));
}

ToolResult PlayControl(Editor& ed, const JsonValue& args)
{
    const StringView action = args.Get("action").AsString();
    if (action == "play") {
        if (!ed.IsPlaying())
            ed.BeginPlay();
    } else if (action == "stop") {
        if (ed.IsPlaying())
            ed.EndPlay();
    } else if (action == "pause") {
        if (ed.IsPlaying())
            ed.TogglePause();
    } else {
        return ToolResult::Error("action must be play, stop or pause");
    }
    return ToolResult::Ok(ed.IsPlaying() ? (ed.playState == PlayState::Paused ? "Paused." : "Playing. Take a screenshot to look, get_log for errors.")
                                         : "Stopped (the scene is restored to its state before Play).");
}

ToolResult SaveScene(Editor& ed, const JsonValue&)
{
    if (ed.IsPlaying())
        return ToolResult::Error("Stop playing first.");
    if (ed.scenePath.Empty())
        return ToolResult::Error("The scene has never been saved: ask the user to save it (File > Save Scene As) or run_console_command \"scene.save\".");
    ed.SaveScene(false);
    return ToolResult::Ok(String::Format("Saved {}.", ed.scenePath));
}

ToolResult UndoTool(Editor& ed, const JsonValue& args)
{
    const i64 steps = Clamp<i64>(args.Get("steps").AsInt(1), 1, 50);
    String out;
    for (i64 i = 0; i < steps && ed.undo.CanUndo(); ++i) {
        out.Appendf("Undid: {}\n", ed.undo.UndoLabel());
        ed.Undo();
    }
    return ToolResult::Ok(out.Empty() ? String("Nothing to undo.") : out);
}

// --- Registry -------------------------------------------------------------------------------------------------------

using ToolFn = ToolResult (*)(Editor&, const JsonValue&);

struct ToolEntry
{
    const char* name;
    const char* title;
    bool readOnly;
    ToolFn fn;
    const char* description;
    const char* schema;
};

const ToolEntry ToolEntries[] = {
    {"get_editor_state", "Editor state", true, EditorState,
     "Project, open scene, entity count, selection, play state, editor camera (spawnPoint = 5 m in front of it, where new "
     "objects appear by default) and the log's error count. Call this first.",
     R"j({"type":"object","properties":{}})j"},
    {"get_docs", "Engine documentation", true, GetDocs,
     "Engine documentation. topic: overview (how to build games here), components (all components; with `component`: "
     "its fields, types, defaults, units), console (editor commands; `filter` narrows), zeqouscript (the gameplay "
     "scripting language - read before writing .zs files), cpp (C++ game code), project (project layout and conventions).",
     R"j({"type":"object","properties":{"topic":{"type":"string","enum":["overview","components","console","zeqouscript","cpp","project","getting_started"]},
        "component":{"type":"string","description":"With topic=components: one component, e.g. PointLight"},
        "filter":{"type":"string","description":"With topic=console: substring of command names"}},"required":["topic"]})j"},
    {"list_entities", "List entities", true, ListEntities,
     "The scene hierarchy: name [guid] at world position: components. Indentation shows children.",
     R"j({"type":"object","properties":{"filter":{"type":"string","description":"Substring of the name or a component name"},
        "limit":{"type":"integer","description":"Max entities (default 300)"}}})j"},
    {"get_entity", "Get entity", true, GetEntity,
     "Every component of one entity with its values (fields equal to the default are omitted), its children and world position.",
     R"j({"type":"object","properties":{"entity":{"type":"string","description":"Guid or exact name"}},"required":["entity"]})j"},
    {"create_entities", "Create entities", false, CreateEntities,
     "Creates entities in one undo step and selects them. Units: meters, degrees; Y is up. Each entity: name, optional "
     "shape (cube|sphere|cylinder|plane: a 1 m mesh with a matching collider; scale it), position (default: the camera's "
     "spawn point), rotation [pitch, yaw, roll] degrees, scale, parent (guid or name, also of an entity earlier in the "
     "same list), components {ComponentName: {field: value}} (see get_docs components). Example: "
     "{\"entities\":[{\"name\":\"Floor\",\"shape\":\"cube\",\"position\":[0,-0.5,0],\"scale\":[20,1,20]},"
     "{\"name\":\"Lamp\",\"position\":[0,3,0],\"components\":{\"PointLight\":{\"lumens\":1500,\"color\":[1,0.8,0.6]}}}]}",
     R"j({"type":"object","properties":{"entities":{"type":"array","items":{"type":"object","properties":{
        "name":{"type":"string"},"shape":{"type":"string","enum":["cube","sphere","cylinder","plane"]},
        "position":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},
        "rotation":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3,"description":"pitch, yaw, roll in degrees"},
        "scale":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},
        "parent":{"type":"string"},"components":{"type":"object"}},"required":["name"]}}},"required":["entities"]})j"},
    {"modify_entity", "Modify entity", false, ModifyEntity,
     "Changes one entity (one undo step): name, position / rotation (degrees) / scale (local), parent (guid, name or "
     "\"\" for the scene root), shape, components {Name: {fields to change}} merged into the current values; "
     "{Name: null} removes a component; a component the entity lacks is added.",
     R"j({"type":"object","properties":{"entity":{"type":"string","description":"Guid or exact name"},"name":{"type":"string"},
        "position":{"type":"array","items":{"type":"number"}},"rotation":{"type":"array","items":{"type":"number"}},
        "scale":{"type":"array","items":{"type":"number"}},"parent":{"type":"string"},
        "shape":{"type":"string","enum":["cube","sphere","cylinder","plane"]},"components":{"type":"object"}},"required":["entity"]})j"},
    {"delete_entities", "Delete entities", false, DeleteEntities, "Deletes entities with their children (one undo step).",
     R"j({"type":"object","properties":{"entities":{"type":"array","items":{"type":"string"}}},"required":["entities"]})j"},
    {"select_entities", "Select entities", false, SelectEntities,
     "Selects entities in the editor (and moves the camera to them unless focus is false) - to show the user something.",
     R"j({"type":"object","properties":{"entities":{"type":"array","items":{"type":"string"}},"focus":{"type":"boolean"}},"required":["entities"]})j"},
    {"run_console_command", "Run console command", false, RunCommand,
     "Runs an editor console command and returns its log output. Examples: \"create sun\", \"create clouds\", \"time golden\", "
     "\"terrain.op flatten 0\", \"material.create Brick\", \"script.attach Scripts/Player.zs\" (on the selection), "
     "\"prefab.place Prefabs/Tree.zprefab\", \"nav.build\", \"camera 0 5 -10\". get_docs console lists all.",
     R"j({"type":"object","properties":{"command":{"type":"string"}},"required":["command"]})j"},
    {"get_log", "Output log", true, GetLog, "The latest Output Log lines (script compile errors, warnings, gameplay logs).",
     R"j({"type":"object","properties":{"count":{"type":"integer"},"errors_only":{"type":"boolean"}}})j"},
    {"list_files", "List project files", true, ListFiles,
     "Files of the project (paths relative to the project folder; Content/ holds scenes, scripts, materials, models).",
     R"j({"type":"object","properties":{"path":{"type":"string","description":"Folder, e.g. Content/Scripts (default: project root)"},
        "recursive":{"type":"boolean"}}})j"},
    {"read_file", "Read file", true, ReadFileTool, "Reads a text file of the project (scripts, scenes, materials, JSON, C++).",
     R"j({"type":"object","properties":{"path":{"type":"string"}},"required":["path"]})j"},
    {"write_file", "Write file", false, WriteFileTool,
     "Creates or overwrites a text file in the project (e.g. Content/Scripts/Player.zs). Scripts reload automatically; "
     "attach one with run_console_command \"script.attach <path under Content>\" after selecting the entity, or add a "
     "Script component {\"file\": \"Scripts/Player.zs\"}.",
     R"j({"type":"object","properties":{"path":{"type":"string"},"content":{"type":"string"}},"required":["path","content"]})j"},
    {"screenshot", "Screenshot", true, nullptr,
     "A picture of the 3D viewport (or the whole editor) as it is now - use it to check your work.",
     R"j({"type":"object","properties":{"area":{"type":"string","enum":["viewport","editor"]},
        "max_width":{"type":"integer","description":"Default 1024"}}})j"},
    {"play_control", "Play / Stop", false, PlayControl,
     "Starts, pauses or stops Play-in-Editor. Edits made while playing are discarded on stop.",
     R"j({"type":"object","properties":{"action":{"type":"string","enum":["play","stop","pause"]}},"required":["action"]})j"},
    {"save_scene", "Save scene", false, SaveScene, "Saves the open scene to its file.", R"j({"type":"object","properties":{}})j"},
    {"undo", "Undo", false, UndoTool, "Undoes the last scene edits (yours or the user's).",
     R"j({"type":"object","properties":{"steps":{"type":"integer"}}})j"},
};

} // namespace

String McpInstructions()
{
    return String::Format(
        "You are connected to the ZeqouX3D editor ({}), a 3D game engine. You can build games here: create and edit "
        "objects in the open scene, write gameplay scripts, run editor commands, play the game and look at it.\n"
        "Workflow: get_editor_state first; get_docs zeqouscript before writing scripts and get_docs components for "
        "component fields; build with create_entities / modify_entity (each call is one undo step the user can undo); "
        "write scripts with write_file under Content/Scripts and attach them; check with screenshot and get_log; "
        "play_control play to test, then stop; save_scene when done.\n"
        "Conventions: meters, seconds, degrees; Y up; +Z forward. Objects have a Transform; a 1 m cube = shape cube. A "
        "scene needs light to be visible (\"create sun\", \"create skylight\"). Keep the user's work: don't delete or "
        "overwrite what you did not create unless asked. Names in English.",
        EngineVersionString);
}

EditorToolHost::EditorToolHost(Editor& ed) : m_ed(ed)
{
    for (const ToolEntry& t : ToolEntries) {
        ai::ToolDef d;
        d.name = t.name;
        d.title = t.title;
        d.description = t.description;
        d.inputSchema = Schema(t.schema);
        d.readOnly = t.readOnly;
        m_tools.PushBack(move(d));
    }
}

EditorToolHost::~EditorToolHost() { Shutdown(); }

ToolResult EditorToolHost::Call(StringView name, const JsonValue& arguments)
{
    if (m_closed.load())
        return ToolResult::Error("The editor is closing.");
    Job* job = new Job;
    job->name = name;
    job->args = arguments;
    {
        ScopedLock lock(m_mutex);
        m_queue.PushBack(job);
    }
    // The main thread may be busy (loading, a modal dialog): wait, but not forever.
    const bool done = job->done.Wait(180000);
    ToolResult r = done ? job->result : ToolResult::Error("The editor did not respond in time (busy?).");
    job->Release();
    return r;
}

void EditorToolHost::Pump()
{
    Array<Job*> jobs;
    {
        ScopedLock lock(m_mutex);
        jobs = move(m_queue);
        m_queue.Clear();
    }
    for (Job* job : jobs)
        Execute(*job);
}

void EditorToolHost::Shutdown()
{
    m_closed = true;
    Array<Job*> jobs;
    {
        ScopedLock lock(m_mutex);
        jobs = move(m_queue);
        m_queue.Clear();
    }
    for (Job* job : jobs) {
        job->result = ToolResult::Error("The editor is closing.");
        job->done.Signal();
        job->Release();
    }
    // Captures still waiting for a frame complete with an error.
    for (Editor::CaptureRequest& r : m_ed.captureRequests)
        r.done(nullptr, 0, 0, 0, Rect());
    m_ed.captureRequests.Clear();
}

void EditorToolHost::Execute(Job& job)
{
    const ToolEntry* entry = nullptr;
    for (const ToolEntry& t : ToolEntries)
        if (job.name == t.name)
            entry = &t;
    if (!entry) {
        job.result = ToolResult::Error(String::Format("Unknown tool {}", job.name));
    } else if (!entry->fn) {
        // Screenshot: finishes when the next frame has been rendered.
        const bool viewport = job.args.Get("area").AsString("viewport") != "editor";
        const u32 maxWidth = u32(Clamp<i64>(job.args.Get("max_width").AsInt(1024), 256, 1920));
        Job* pending = &job;
        m_ed.captureRequests.PushBack(Editor::CaptureRequest{[pending, viewport, maxWidth](const u8* pixels, u32 w, u32 h, usize pitch, Rect vp) {
            if (!pixels || !w || !h) {
                pending->result = ToolResult::Error("No frame was rendered.");
            } else {
                u32 x0 = 0, y0 = 0, cw = w, ch = h;
                if (viewport && vp.Width() > 16.0f && vp.Height() > 16.0f) {
                    x0 = Min(u32(Max(vp.min.x, 0.0f)), w - 1);
                    y0 = Min(u32(Max(vp.min.y, 0.0f)), h - 1);
                    cw = Min(u32(vp.Width()), w - x0);
                    ch = Min(u32(vp.Height()), h - y0);
                }
                pending->result = ToolResult::Ok(String::Format("{} ({} x {} px)", viewport ? "Viewport" : "Editor window", cw, ch));
                pending->result.pngBase64 = EncodeScreenshot(pixels, pitch, x0, y0, cw, ch, maxWidth);
            }
            pending->done.Signal();
            pending->Release();
        }, viewport});
        return;
    } else {
        job.result = entry->fn(m_ed, job.args);
        ZX_LOG_INFO(LogAITools, "AI tool {}{}", job.name, job.result.isError ? " (error)" : "");
    }
    job.done.Signal();
    job.Release();
}

} // namespace zx::editor
