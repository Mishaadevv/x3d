#include "Engine/SceneSerialization.h"
#include "Core/Containers/Algorithm.h"
#include "Core/FileSystem/FileSystem.h"
#include "Core/Log/Log.h"
#include "Core/Reflection/ReflectionSerialization.h"
#include "Core/String/Path.h"
#include "Engine/ComponentRegistry.h"
#include "Engine/Prefab.h"
#include "Engine/SceneRendering.h"

ZX_DEFINE_LOG_CATEGORY_STATIC(LogScene, Info);

namespace zx {

ZX_REFLECT_STRUCT_BEGIN(EntityGuid)
    ZX_FIELD(value)
ZX_REFLECT_STRUCT_END()

namespace {

String GuidString(const Guid& g) { return String::Format("{}", g); }

Guid ParseGuid(const JsonValue& v)
{
    const Optional<Guid> g = Guid::Parse(v.AsString());
    return g ? *g : Guid();
}

void ApplyComponents(ecs::World& world, ecs::Entity e, const JsonValue& components, Array<String>* warnings)
{
    for (const ComponentType& c : GetComponentTypes()) {
        const JsonValue* j = components.Find(c.type->name.View());
        if (!j) {
            if (world.HasComponent(e, c.id))
                world.RemoveComponent(e, c.id);
            continue;
        }
        void* p = world.HasComponent(e, c.id) ? world.GetComponent(e, c.id, true) : world.AddComponent(e, c.id);
        if (!p)
            continue; // tag
        Reflection::CopyValue(p, Reflection::GetDefaultObject(*c.type), *c.type);
        Reflection::FromJson(*j, p, *c.type, warnings);
    }
    if (warnings)
        for (const JsonMember& m : components.Members())
            if (!FindComponentType(m.first))
                warnings->PushBack(String::Format("Unknown component '{}' skipped", m.first));
    if (world.Has<Transform>(e) && !world.Has<scene::WorldTransform>(e))
        world.Add(e, scene::WorldTransform{});
}

} // namespace

// --- Guids ----------------------------------------------------------------------------------------------------------------

Guid EnsureEntityGuid(ecs::World& world, ecs::Entity e)
{
    if (!world.IsAlive(e))
        return {};
    if (const EntityGuid* g = world.Get<const EntityGuid>(e); g && g->value.IsValid())
        return g->value;
    const Guid g = Guid::New();
    world.Add(e, EntityGuid{g});
    return g;
}

Guid GetEntityGuid(const ecs::World& world, ecs::Entity e)
{
    const EntityGuid* g = world.IsAlive(e) ? world.Get<EntityGuid>(e) : nullptr;
    return g ? g->value : Guid();
}

ecs::Entity FindEntityByGuid(ecs::World& world, const Guid& guid)
{
    ecs::Entity found;
    if (guid.IsValid())
        world.Query<const EntityGuid>().Each([&](ecs::Entity e, const EntityGuid& g) {
            if (g.value == guid)
                found = e;
        });
    return found;
}

void BuildGuidMap(ecs::World& world, HashMap<Guid, ecs::Entity>& out)
{
    out.Clear();
    world.Query<const EntityGuid>().Each([&](ecs::Entity e, const EntityGuid& g) { out.InsertOrAssign(g.value, e); });
}

// --- Hierarchy helpers -------------------------------------------------------------------------------------------------

void CollectHierarchy(const ecs::World& world, ecs::Entity root, Array<ecs::Entity>& out)
{
    if (!world.IsAlive(root))
        return;
    out.PushBack(root);
    for (ecs::Entity c : scene::GetChildren(world, root))
        CollectHierarchy(world, c, out);
}

void CollectRoots(ecs::World& world, Array<ecs::Entity>& out)
{
    world.Query<const Transform>().Without<scene::Parent>().Each([&](ecs::Entity e, const Transform&) { out.PushBack(e); });
}

// --- Snapshots ----------------------------------------------------------------------------------------------------------

JsonValue SerializeEntity(ecs::World& world, ecs::Entity e)
{
    if (!world.IsAlive(e))
        return {};
    JsonValue o = JsonValue::MakeObject();
    o.Set("guid", GuidString(EnsureEntityGuid(world, e)));
    if (const ecs::Entity parent = scene::GetParent(world, e)) {
        o.Set("parent", GuidString(EnsureEntityGuid(world, parent)));
        const usize index = scene::GetChildren(world, parent).Find(e);
        o.Set("index", u64(index == NPos ? 0 : index));
    }
    JsonValue components = JsonValue::MakeObject();
    for (const ComponentType& c : GetComponentTypes()) {
        if (!world.HasComponent(e, c.id))
            continue;
        const void* p = world.GetComponent(e, c.id, false);
        components.Set(c.type->name.View(), p ? Reflection::ToJson(p, *c.type, Reflection::GetDefaultObject(*c.type)) : JsonValue::MakeObject());
    }
    o.Set("components", move(components));
    return o;
}

Array<ecs::Entity> ApplyEntitySnapshots(ecs::World& world, Span<const EntitySnapshot> snapshots, Array<String>* warnings)
{
    HashMap<Guid, ecs::Entity> map;
    BuildGuidMap(world, map);
    Array<ecs::Entity> result;
    result.Resize(snapshots.Size());

    // 1. Create / update.
    for (usize i = 0; i < snapshots.Size(); ++i) {
        const EntitySnapshot& s = snapshots[i];
        if (s.state.IsNull() || !s.guid.IsValid())
            continue;
        ecs::Entity e;
        if (const ecs::Entity* found = map.Find(s.guid); found && world.IsAlive(*found)) {
            e = *found;
        } else {
            e = world.Create();
            world.Add(e, EntityGuid{s.guid});
            map.InsertOrAssign(s.guid, e);
        }
        ApplyComponents(world, e, s.state.Get("components"), warnings);
        result[i] = e;
    }

    // 2. Parents, in ascending sibling index so every child lands at its recorded position.
    Array<usize> order;
    for (usize i = 0; i < snapshots.Size(); ++i)
        if (result[i])
            order.PushBack(i);
    StableSort(order, [&](usize a, usize b) { return snapshots[a].state.Get("index").AsInt() < snapshots[b].state.Get("index").AsInt(); });
    for (usize i : order) {
        const ecs::Entity e = result[i];
        const JsonValue& state = snapshots[i].state;
        ecs::Entity parent;
        if (const JsonValue* p = state.Find("parent")) {
            if (const ecs::Entity* found = map.Find(ParseGuid(*p)); found && world.IsAlive(*found))
                parent = *found;
            else if (warnings)
                warnings->PushBack(String::Format("Parent {} of entity {} not found", p->AsString(), snapshots[i].guid));
        }
        if (scene::GetParent(world, e) != parent && !scene::SetParent(world, e, parent))
            continue;
        if (scene::Children* c = parent ? world.Get<scene::Children>(parent) : nullptr) {
            const usize current = c->entities.Find(e);
            const usize target = Min(usize(Max<i64>(0, state.Get("index").AsInt())), c->entities.Size() - 1);
            if (current != NPos && current != target) {
                c->entities.RemoveAt(current);
                c->entities.Insert(target, e);
            }
        }
    }

    // 3. Destroy (with descendants that are still attached).
    for (const EntitySnapshot& s : snapshots)
        if (s.state.IsNull())
            if (const ecs::Entity* found = map.Find(s.guid); found && world.IsAlive(*found))
                scene::DestroyHierarchy(world, *found);
    return result;
}

// --- Scenes -------------------------------------------------------------------------------------------------------------

JsonValue SaveScene(ecs::World& world, const SceneIOOptions& options, const JsonValue& settings)
{
    Array<ecs::Entity> roots, entities;
    CollectRoots(world, roots);
    for (ecs::Entity r : roots)
        if (!options.rootFilter || options.rootFilter(r))
            CollectHierarchy(world, r, entities);

    JsonValue doc = JsonValue::MakeObject();
    doc.Set("format", "zscene");
    doc.Set("version", SceneFormatVersion);

    // Asset sources referenced by mesh renderers (meshes and materials are sub-assets of a model source).
    JsonValue assetList = JsonValue::MakeArray();
    if (options.assets) {
        Array<String> paths;
        auto addSource = [&](const asset::AssetId& id) {
            if (!id.IsValid())
                return;
            String path = options.assets->GetSourcePath(id);
            if (path.Empty())
                return;
            const String rel = options.assetRoot.Empty() ? String() : Path::MakeRelative(path, options.assetRoot);
            if (!rel.Empty())
                path = rel;
            if (!paths.Contains(path))
                paths.PushBack(move(path));
        };
        for (ecs::Entity e : entities)
            if (const MeshRenderer* mr = world.Get<const MeshRenderer>(e)) {
                addSource(mr->mesh);
                for (const asset::AssetId& m : mr->materials)
                    addSource(m);
            }
        for (String& p : paths)
            assetList.PushBack(JsonValue(p));
    }
    doc.Set("assets", move(assetList));
    if (!settings.IsNull())
        doc.Set("settings", settings);

    Array<JsonValue> serialized;
    for (ecs::Entity e : entities) {
        JsonValue o = SerializeEntity(world, e);
        o.Remove("index"); // file order already encodes it
        serialized.PushBack(move(o));
    }
    if (options.prefabs)
        CompressPrefabInstances(serialized, *options.prefabs); // instances keep only their overrides
    JsonValue list = JsonValue::MakeArray();
    for (JsonValue& o : serialized)
        list.PushBack(move(o));
    doc.Set("entities", move(list));
    return doc;
}

bool LoadScene(const JsonValue& doc, ecs::World& world, const SceneIOOptions& options, JsonValue* settingsOut, Array<String>* warnings)
{
    if (!doc.IsObject() || doc.Get("format").AsString() != "zscene") {
        ZX_LOG_ERROR(LogScene, "Not a .zscene document");
        return false;
    }
    const i64 version = doc.Get("version").AsInt();
    if (version < 1 || version > SceneFormatVersion) {
        ZX_LOG_ERROR(LogScene, "Unsupported scene version {} (this build reads up to {})", version, SceneFormatVersion);
        return false;
    }
    if (options.assets)
        for (const JsonValue& a : doc.Get("assets").Elements()) {
            const StringView path = a.AsString();
            if (!path.Empty())
                options.assets->LoadPath(Path::IsAbsolute(path) || options.assetRoot.Empty() ? String(path) : Path::Join(options.assetRoot, path));
        }

    Array<EntitySnapshot> snapshots;
    for (const JsonValue& o : doc.Get("entities").Elements()) {
        EntitySnapshot s;
        s.guid = ParseGuid(o.Get("guid"));
        if (!s.guid.IsValid())
            s.guid = Guid::New();
        s.state = o;
        snapshots.PushBack(move(s));
    }
    if (options.prefabs)
        ExpandPrefabInstances(snapshots, *options.prefabs, warnings);
    // File order is depth-first: give each child its position among the siblings listed before it.
    HashMap<Guid, i64> childCounts;
    for (EntitySnapshot& s : snapshots)
        if (const JsonValue* p = s.state.Find("parent")) {
            i64& count = *childCounts.Insert(ParseGuid(*p), 0).first;
            s.state.Set("index", count++);
        }
    ApplyEntitySnapshots(world, snapshots.View(), warnings);
    if (settingsOut)
        *settingsOut = doc.Get("settings");
    return true;
}

bool SaveSceneFile(StringView path, ecs::World& world, const SceneIOOptions& options, const JsonValue& settings)
{
    const String text = ToJsonString(SaveScene(world, options, settings));
    if (!fs::WriteTextFile(path, text)) {
        ZX_LOG_ERROR(LogScene, "Can't write scene {}", path);
        return false;
    }
    return true;
}

bool LoadSceneFile(StringView path, ecs::World& world, const SceneIOOptions& options, JsonValue* settingsOut)
{
    String text;
    if (!fs::ReadTextFile(path, text)) {
        ZX_LOG_ERROR(LogScene, "Can't read scene {}", path);
        return false;
    }
    JsonValue doc;
    JsonParseError error;
    if (!ParseJson(text.View(), doc, &error)) {
        ZX_LOG_ERROR(LogScene, "{}({}:{}): {}", path, error.line, error.column, error.message);
        return false;
    }
    Array<String> warnings;
    const bool ok = LoadScene(doc, world, options, settingsOut, &warnings);
    for (const String& w : warnings)
        ZX_LOG_WARN(LogScene, "{}: {}", Path::FileName(path), w);
    return ok;
}

void ClearScene(ecs::World& world)
{
    Array<ecs::Entity> roots;
    CollectRoots(world, roots);
    for (ecs::Entity r : roots)
        scene::DestroyHierarchy(world, r);
}

} // namespace zx
