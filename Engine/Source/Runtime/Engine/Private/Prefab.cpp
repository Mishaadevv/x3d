#include "Engine/Prefab.h"
#include "Core/FileSystem/FileSystem.h"
#include "Core/Log/Log.h"
#include "Core/Memory/Memory.h"
#include "Core/Reflection/ReflectionSerialization.h"
#include "Core/String/Path.h"
#include "Engine/ComponentRegistry.h"
#include "Engine/SceneRendering.h"

ZX_DEFINE_LOG_CATEGORY_STATIC(LogPrefab, Info);

namespace zx {

ZX_REFLECT_STRUCT_BEGIN(PrefabInstance)
    ZX_FIELD(prefab, Meta::ReadOnly())
ZX_REFLECT_STRUCT_END()

ZX_REFLECT_STRUCT_BEGIN(PrefabLink)
    ZX_FIELD(instance, Meta::ReadOnly())
    ZX_FIELD(source, Meta::ReadOnly())
ZX_REFLECT_STRUCT_END()

namespace {

constexpr StringView InstanceKey = "PrefabInstance";
constexpr StringView LinkKey = "PrefabLink";
constexpr u32 MaxExpansions = 100000; // guards against prefabs that (indirectly) contain themselves

String GuidText(const Guid& g) { return String::Format("{}", g); }

Guid ParseGuidText(const JsonValue& v)
{
    const Optional<Guid> g = Guid::Parse(v.AsString());
    return g ? *g : Guid();
}

/// base <- delta for JSON objects (recursive); other values are replaced.
JsonValue MergeDelta(const JsonValue& base, const JsonValue& delta)
{
    if (!base.IsObject() || !delta.IsObject())
        return delta;
    JsonValue out = base;
    for (const JsonMember& m : delta.Members()) {
        if (JsonValue* b = out.Find(m.first); b && b->IsObject() && m.second.IsObject())
            *b = MergeDelta(*b, m.second);
        else
            out.Set(m.first, m.second);
    }
    return out;
}

/// Full (non-delta) JSON of a registered component, for field-by-field comparisons.
JsonValue CanonicalComponent(StringView name, const JsonValue& json)
{
    const ComponentType* c = FindComponentType(name);
    if (!c || !c->type->construct)
        return json;
    const TypeInfo& t = *c->type;
    void* tmp = Malloc(Max<usize>(t.size, 1), Max<usize>(t.alignment, DefaultAlignment));
    t.construct(tmp);
    Reflection::FromJson(json, tmp, t);
    JsonValue full = Reflection::ToJson(tmp, t, nullptr);
    t.destruct(tmp);
    Free(tmp);
    return full;
}

/// Fields of `cur` that differ from `base` (both canonical objects); nested objects recursively.
JsonValue DiffObjects(const JsonValue& cur, const JsonValue& base)
{
    JsonValue out = JsonValue::MakeObject();
    for (const JsonMember& m : cur.Members()) {
        const JsonValue* b = base.Find(m.first);
        if (!b) {
            out.Set(m.first, m.second);
        } else if (m.second.IsObject() && b->IsObject()) {
            JsonValue d = DiffObjects(m.second, *b);
            if (d.Size() > 0)
                out.Set(m.first, move(d));
        } else if (!(m.second == *b)) {
            out.Set(m.first, m.second);
        }
    }
    return out;
}

/// Components of `cur` as overrides against `base`. Link/instance components are always kept.
JsonValue DiffComponents(const JsonValue& cur, const JsonValue& base, JsonValue& removed)
{
    JsonValue out = JsonValue::MakeObject();
    removed = JsonValue::MakeArray();
    for (const JsonMember& m : cur.Members()) {
        const JsonValue* b = base.Find(m.first);
        if (m.first == InstanceKey || m.first == LinkKey || !b) {
            out.Set(m.first, m.second);
            continue;
        }
        JsonValue d = DiffObjects(CanonicalComponent(m.first, m.second), CanonicalComponent(m.first, *b));
        if (d.Size() > 0)
            out.Set(m.first, move(d));
    }
    for (const JsonMember& m : base.Members())
        if (m.first != InstanceKey && m.first != LinkKey && !cur.Contains(m.first))
            removed.PushBack(JsonValue(m.first));
    return out;
}

/// Applies `delta` (components + removedComponents) on top of `base` (an entity state).
JsonValue MergeEntity(const JsonValue& base, const JsonValue& delta)
{
    JsonValue out = base;
    JsonValue comps = base.Get("components").IsObject() ? base.Get("components") : JsonValue::MakeObject();
    for (const JsonMember& m : delta.Get("components").Members()) {
        if (JsonValue* b = comps.Find(m.first); b && b->IsObject() && m.second.IsObject())
            *b = MergeDelta(*b, m.second);
        else
            comps.Set(m.first, m.second);
    }
    JsonValue removed = base.Get("removedComponents").IsArray() ? base.Get("removedComponents") : JsonValue::MakeArray();
    for (const JsonValue& r : delta.Get("removedComponents").Elements()) {
        comps.Remove(r.AsString());
        removed.PushBack(r);
    }
    out.Set("components", move(comps));
    if (removed.Size() > 0)
        out.Set("removedComponents", move(removed));
    for (StringView key : {StringView("guid"), StringView("parent"), StringView("index")})
        if (const JsonValue* v = delta.Find(key))
            out.Set(key, *v);
    // Deleted members accumulate across nesting levels.
    if (const JsonValue* r = delta.Find("removedFromPrefab")) {
        JsonValue list = base.Get("removedFromPrefab").IsArray() ? base.Get("removedFromPrefab") : JsonValue::MakeArray();
        for (const JsonValue& g : r->Elements())
            list.PushBack(g);
        out.Set("removedFromPrefab", move(list));
    }
    return out;
}

/// A prefab file entity as a member of instance `instance` (guids, parents and nested links remapped).
JsonValue InstanceEntity(const JsonValue& src, const Guid& instance, const Guid& prefabRoot, bool isRoot)
{
    auto remap = [&](const Guid& g) { return g == prefabRoot ? instance : PrefabMemberGuid(instance, g); };
    JsonValue out = src;
    const Guid source = ParseGuidText(src.Get("guid"));
    out.Remove("index");
    if (isRoot)
        out.Remove("parent");
    else if (const JsonValue* p = src.Find("parent"))
        out.Set("parent", GuidText(remap(ParseGuidText(*p))));
    // ("removedFromPrefab" holds source ids of the entity's own instance: valid at any nesting level as is.)
    out.Set("guid", GuidText(isRoot ? instance : PrefabMemberGuid(instance, source)));
    JsonValue& comps = out["components"];
    if (!comps.IsObject())
        comps = JsonValue::MakeObject();
    if (JsonValue* link = comps.Find(LinkKey)) {
        // Member of a nested instance: its identity derives from that instance's (remapped) root.
        const Guid inner = remap(ParseGuidText(link->Get("instance")));
        const Guid innerSource = ParseGuidText(link->Get("source"));
        link->Set("instance", GuidText(inner));
        out.Set("guid", GuidText(PrefabMemberGuid(inner, innerSource)));
    } else if (!isRoot) {
        JsonValue l = JsonValue::MakeObject();
        l.Set("instance", GuidText(instance));
        l.Set("source", GuidText(source));
        comps.Set(LinkKey, move(l));
    }
    return out;
}

Guid LinkKeyOf(const JsonValue& state)
{
    const JsonValue* link = state.Get("components").Find(LinkKey);
    return link ? PrefabMemberGuid(ParseGuidText(link->Get("instance")), ParseGuidText(link->Get("source"))) : Guid();
}

/// "index" = position among the siblings that precede it in the array (roots keep theirs).
void AssignSiblingIndices(Array<EntitySnapshot>& entries, usize first)
{
    HashMap<Guid, i64> counts;
    for (usize i = 0; i < entries.Size(); ++i) {
        const JsonValue* p = entries[i].state.Find("parent");
        if (!p)
            continue;
        i64& n = *counts.Insert(ParseGuidText(*p), 0).first;
        if (i >= first)
            entries[i].state.Set("index", n);
        ++n;
    }
}

void CollectSources(ecs::World& world, Span<const ecs::Entity> entities, asset::AssetManager* assets, StringView contentRoot, JsonValue& out)
{
    out = JsonValue::MakeArray();
    if (!assets)
        return;
    Array<String> paths;
    auto add = [&](const asset::AssetId& id) {
        String path = assets->GetSourcePath(id);
        if (path.Empty())
            return;
        if (String rel = Path::MakeRelative(path, contentRoot); !rel.Empty())
            path = rel;
        if (!paths.Contains(path))
            paths.PushBack(move(path));
    };
    for (ecs::Entity e : entities)
        if (const MeshRenderer* mr = world.Get<const MeshRenderer>(e)) {
            add(mr->mesh);
            for (const asset::AssetId& m : mr->materials)
                add(m);
        }
    for (String& p : paths)
        out.PushBack(JsonValue(p));
}

/// Serialized subtree with prefab-local ids: `localOf` maps world guids to prefab ids. Links to instances outside the
/// subtree are dropped; the root loses its parent and position.
Array<JsonValue> BuildPrefabEntities(ecs::World& world, Span<const ecs::Entity> all, const HashMap<Guid, Guid>& localOf)
{
    Array<JsonValue> list;
    for (usize i = 0; i < all.Size(); ++i) {
        JsonValue j = SerializeEntity(world, all[i]);
        j.Remove("index");
        j.Set("guid", GuidText(*localOf.Find(GetEntityGuid(world, all[i]))));
        if (i == 0) {
            j.Remove("parent");
            if (JsonValue* t = j["components"].Find("Transform"))
                t->Remove("position");
        } else if (const JsonValue* p = j.Find("parent")) {
            j.Set("parent", GuidText(*localOf.Find(ParseGuidText(*p))));
        }
        JsonValue& comps = j["components"];
        if (i == 0)
            comps.Remove(InstanceKey);
        if (JsonValue* link = comps.Find(LinkKey)) {
            if (const Guid* local = localOf.Find(ParseGuidText(link->Get("instance"))); local && i > 0)
                link->Set("instance", GuidText(*local));
            else
                comps.Remove(LinkKey);
        }
        list.PushBack(move(j));
    }
    return list;
}

bool WritePrefabFile(StringView physicalPath, Array<JsonValue>& entities, JsonValue assetsList, PrefabLibrary& library)
{
    CompressPrefabInstances(entities, library);
    JsonValue doc = JsonValue::MakeObject();
    doc.Set("format", "zprefab");
    doc.Set("version", PrefabFormatVersion);
    doc.Set("assets", move(assetsList));
    JsonValue list = JsonValue::MakeArray();
    for (JsonValue& e : entities)
        list.PushBack(move(e));
    doc.Set("entities", move(list));
    platform::CreateDirectoryTree(Path::Parent(physicalPath));
    if (!fs::WriteTextFile(physicalPath, ToJsonString(doc))) {
        ZX_LOG_ERROR(LogPrefab, "Can't write prefab {}", physicalPath);
        return false;
    }
    return true;
}

/// The instance subtree in file form (members as overrides against the prefab currently in the library).
Array<EntitySnapshot> CaptureInstance(ecs::World& world, ecs::Entity root, PrefabLibrary& library)
{
    Array<ecs::Entity> all;
    CollectHierarchy(world, root, all);
    Array<JsonValue> list;
    for (ecs::Entity e : all)
        list.PushBack(SerializeEntity(world, e));
    for (usize i = 1; i < list.Size(); ++i)
        list[i].Remove("index");
    CompressPrefabInstances(list, library);
    Array<EntitySnapshot> entries;
    for (JsonValue& j : list)
        entries.PushBack(EntitySnapshot{ParseGuidText(j.Get("guid")), move(j)});
    return entries;
}

/// Destroys `root`'s subtree and recreates it from `entries` (expanded). Returns the new root.
ecs::Entity Rebuild(ecs::World& world, ecs::Entity root, Array<EntitySnapshot>& entries, PrefabLibrary& library)
{
    const Guid rootGuid = entries[0].guid;
    scene::DestroyHierarchy(world, root);
    ExpandPrefabInstances(entries, library);
    AssignSiblingIndices(entries, 1);
    ApplyEntitySnapshots(world, entries.View());
    return FindEntityByGuid(world, rootGuid);
}

} // namespace

// --- Library ------------------------------------------------------------------------------------------------------------

String PrefabLibrary::PhysicalPath(StringView relativePath) const
{
    return Path::IsAbsolute(relativePath) ? Path::Normalize(relativePath) : Path::Join(m_contentRoot, relativePath);
}

String PrefabLibrary::RelativePath(StringView physicalPath) const
{
    const String rel = Path::MakeRelative(physicalPath, m_contentRoot);
    return rel.Empty() ? Path::Normalize(physicalPath) : rel;
}

const PrefabLibrary::Prefab* PrefabLibrary::Get(StringView relativePath)
{
    const String key = Path::Normalize(relativePath);
    if (const UniquePtr<Prefab>* p = m_prefabs.Find(key))
        return p->Get();
    if (m_failed.Contains(key))
        return nullptr;
    String text;
    JsonValue doc;
    JsonParseError error;
    const String physical = PhysicalPath(key);
    if (!fs::ReadTextFile(physical, text) || !ParseJson(text.View(), doc, &error) || doc.Get("format").AsString() != "zprefab" ||
        doc.Get("entities").Size() == 0) {
        ZX_LOG_ERROR(LogPrefab, "Can't load prefab '{}'{}", key, error.message.Empty() ? String() : String::Format(": {}", error.message));
        m_failed.InsertOrAssign(key, true);
        return nullptr;
    }
    UniquePtr<Prefab> prefab = MakeUnique<Prefab>();
    prefab->path = key;
    for (const JsonValue& e : doc.Get("entities").Elements())
        prefab->entities.PushBack(e);
    prefab->root = ParseGuidText(prefab->entities[0].Get("guid"));
    if (m_assets)
        for (const JsonValue& a : doc.Get("assets").Elements())
            if (!a.AsString().Empty())
                m_assets->LoadPath(PhysicalPath(a.AsString()));
    const Prefab* result = prefab.Get();
    m_prefabs.InsertOrAssign(key, move(prefab));
    return result;
}

void PrefabLibrary::Invalidate(StringView relativePath)
{
    const String key = Path::Normalize(relativePath);
    m_prefabs.Remove(key);
    m_failed.Remove(key);
}

// --- Expansion / compression ---------------------------------------------------------------------------------------------

Guid PrefabMemberGuid(const Guid& instance, const Guid& source) { return Guid::FromName(instance, GuidText(source)); }

void ExpandPrefabInstances(Array<EntitySnapshot>& entries, PrefabLibrary& library, Array<String>* warnings)
{
    u32 expansions = 0;
    for (usize i = 0; i < entries.Size(); ++i) {
        if (entries[i].state.IsNull() || entries[i].state.Contains("_expanded"))
            continue;
        const JsonValue* inst = entries[i].state.Get("components").Find(InstanceKey);
        if (!inst)
            continue;
        const String path(inst->Get("prefab").AsString()); // copy: the state is modified below
        entries[i].state.Set("_expanded", true);
        if (++expansions > MaxExpansions) {
            if (warnings)
                warnings->PushBack("Prefab expansion limit reached (does a prefab contain itself?)");
            break;
        }
        const PrefabLibrary::Prefab* prefab = library.Get(path);
        if (!prefab) {
            if (warnings)
                warnings->PushBack(String::Format("Missing prefab '{}'", path));
            continue;
        }
        const Guid instance = entries[i].guid;

        // Members already present (scene overrides, or members written by an outer prefab), by link.
        HashMap<Guid, usize> linked;
        for (usize j = 0; j < entries.Size(); ++j)
            if (const Guid key = LinkKeyOf(entries[j].state); key.IsValid())
                linked.InsertOrAssign(key, j);
        // Members deleted in this instance (prefab-local source ids, recorded on the instance root).
        HashMap<Guid, bool> removed;
        for (const JsonValue& r : entries[i].state.Get("removedFromPrefab").Elements()) {
            const Guid source = ParseGuidText(r);
            removed.InsertOrAssign(source, true);
            if (const usize* j = linked.Find(PrefabMemberGuid(instance, source)))
                entries[*j].state = JsonValue(); // written by an outer level, deleted at this one
        }

        for (usize k = 0; k < prefab->entities.Size(); ++k) {
            const bool isRoot = k == 0;
            JsonValue base = InstanceEntity(prefab->entities[k], instance, prefab->root, isRoot);
            if (isRoot) {
                JsonValue merged = MergeEntity(base, entries[i].state);
                merged.Set("_expanded", true);
                entries[i].state = move(merged);
                continue;
            }
            const Guid guid = ParseGuidText(base.Get("guid"));
            const JsonValue* baseLink = base.Get("components").Find(LinkKey);
            if (baseLink && ParseGuidText(baseLink->Get("instance")) == instance &&
                removed.Contains(ParseGuidText(baseLink->Get("source"))))
                continue;
            if (const usize* j = linked.Find(LinkKeyOf(base))) {
                entries[*j].state = MergeEntity(base, entries[*j].state);
                entries[*j].guid = ParseGuidText(entries[*j].state.Get("guid"));
            } else {
                entries.PushBack(EntitySnapshot{guid, move(base)});
            }
        }
    }
    for (EntitySnapshot& e : entries)
        e.state.Remove("_expanded");
}

void CompressPrefabInstances(Array<JsonValue>& entities, PrefabLibrary& library)
{
    HashMap<Guid, bool> handled; // members already written as overrides of an outer instance
    for (usize idx = 0; idx < entities.Size(); ++idx) {
        const JsonValue* inst = entities[idx].Get("components").Find(InstanceKey);
        const Guid rootGuid = ParseGuidText(entities[idx].Get("guid"));
        if (!inst || handled.Contains(rootGuid))
            continue;
        if (!library.Get(inst->Get("prefab").AsString()))
            continue;

        // What the instance would be without overrides (nested prefabs included).
        Array<EntitySnapshot> fresh;
        JsonValue rootEntry = JsonValue::MakeObject();
        rootEntry.Set("guid", GuidText(rootGuid));
        JsonValue rootComps = JsonValue::MakeObject();
        rootComps.Set(InstanceKey, *inst);
        rootEntry.Set("components", move(rootComps));
        fresh.PushBack(EntitySnapshot{rootGuid, move(rootEntry)});
        ExpandPrefabInstances(fresh, library);
        HashMap<Guid, usize> baseOf;
        for (usize f = 0; f < fresh.Size(); ++f)
            baseOf.InsertOrAssign(fresh[f].guid, f);

        // The instance's subtree (entities are in depth-first order: parents first).
        HashMap<Guid, bool> inSubtree;
        inSubtree.InsertOrAssign(rootGuid, true);
        for (usize j = idx; j < entities.Size(); ++j) {
            const Guid g = ParseGuidText(entities[j].Get("guid"));
            if (j > idx) {
                const JsonValue* p = entities[j].Find("parent");
                if (!p || !inSubtree.Contains(ParseGuidText(*p)))
                    continue;
                inSubtree.InsertOrAssign(g, true);
            }
            const usize* b = baseOf.Find(g);
            if (!b)
                continue; // added in the instance: stays complete
            handled.InsertOrAssign(g, true);
            JsonValue removedComps;
            JsonValue comps = DiffComponents(entities[j].Get("components"), fresh[*b].state.Get("components"), removedComps);
            entities[j].Set("components", move(comps));
            if (removedComps.Size() > 0)
                entities[j].Set("removedComponents", move(removedComps));
        }
        // Deleted members are recorded (as source ids) on the root of the instance they belong to; if that root is
        // gone too, the outer level records the root instead.
        HashMap<Guid, usize> indexOf;
        for (usize j = idx; j < entities.Size(); ++j)
            indexOf.InsertOrAssign(ParseGuidText(entities[j].Get("guid")), j);
        for (usize f = 1; f < fresh.Size(); ++f) {
            if (fresh[f].state.IsNull() || inSubtree.Contains(fresh[f].guid))
                continue;
            const JsonValue* link = fresh[f].state.Get("components").Find(LinkKey);
            if (!link)
                continue;
            const Guid owner = ParseGuidText(link->Get("instance"));
            const usize* ownerIndex = indexOf.Find(owner);
            if (!ownerIndex || !inSubtree.Contains(owner))
                continue;
            JsonValue& list = entities[*ownerIndex]["removedFromPrefab"];
            if (!list.IsArray())
                list = JsonValue::MakeArray();
            list.PushBack(link->Get("source"));
        }
    }
}

// --- Editing operations --------------------------------------------------------------------------------------------------

bool CreatePrefabFromEntity(ecs::World& world, ecs::Entity root, StringView relativePath, PrefabLibrary& library, asset::AssetManager* assets)
{
    if (!world.IsAlive(root))
        return false;
    if (world.Has<PrefabInstance>(root)) {
        ZX_LOG_ERROR(LogPrefab, "'{}' is already a prefab instance (prefab variants are not supported yet)", relativePath);
        return false;
    }
    Array<ecs::Entity> all;
    CollectHierarchy(world, root, all);
    // Prefab-local ids: new ids for our own entities; members of nested instances derive from their instance root.
    HashMap<Guid, Guid> localOf;
    for (ecs::Entity e : all) {
        const Guid g = EnsureEntityGuid(world, e);
        const PrefabLink* link = world.Get<const PrefabLink>(e);
        if (!link || e == root)
            localOf.InsertOrAssign(g, Guid::New());
    }
    for (ecs::Entity e : all)
        if (const PrefabLink* link = world.Get<const PrefabLink>(e); link && e != root) {
            const Guid* instanceLocal = localOf.Find(link->instance);
            localOf.InsertOrAssign(GetEntityGuid(world, e), instanceLocal ? PrefabMemberGuid(*instanceLocal, link->source) : Guid::New());
        }
    Array<JsonValue> list = BuildPrefabEntities(world, all.View(), localOf);
    JsonValue sources;
    CollectSources(world, all.View(), assets, library.ContentRoot(), sources);
    library.Invalidate(relativePath);
    if (!WritePrefabFile(library.PhysicalPath(relativePath), list, move(sources), library))
        return false;
    library.Invalidate(relativePath);

    // The subtree becomes an instance of the new prefab (same root guid, members with derived guids).
    const JsonValue rootState = SerializeEntity(world, root);
    JsonValue state = JsonValue::MakeObject();
    state.Set("guid", rootState.Get("guid"));
    for (StringView key : {StringView("parent"), StringView("index")})
        if (const JsonValue* v = rootState.Find(key))
            state.Set(key, *v);
    JsonValue comps = JsonValue::MakeObject();
    JsonValue pi = JsonValue::MakeObject();
    pi.Set("prefab", Path::Normalize(relativePath));
    comps.Set(InstanceKey, move(pi));
    if (const Transform* t = world.Get<const Transform>(root))
        comps.Set("Transform", Reflection::ToJson(*t));
    state.Set("components", move(comps));
    Array<EntitySnapshot> entries;
    entries.PushBack(EntitySnapshot{ParseGuidText(state.Get("guid")), move(state)});
    Rebuild(world, root, entries, library);
    ZX_LOG_INFO(LogPrefab, "Created prefab '{}' ({} entities)", relativePath, all.Size());
    return true;
}

ecs::Entity InstantiatePrefabFile(ecs::World& world, PrefabLibrary& library, StringView relativePath, const Transform& transform,
                                  ecs::Entity parent)
{
    if (!library.Get(relativePath))
        return {};
    const Guid guid = Guid::New();
    JsonValue state = JsonValue::MakeObject();
    state.Set("guid", GuidText(guid));
    if (world.IsAlive(parent)) {
        state.Set("parent", GuidText(EnsureEntityGuid(world, parent)));
        state.Set("index", u64(scene::GetChildren(world, parent).Size()));
    }
    JsonValue comps = JsonValue::MakeObject();
    JsonValue pi = JsonValue::MakeObject();
    pi.Set("prefab", Path::Normalize(relativePath));
    comps.Set(InstanceKey, move(pi));
    comps.Set("Transform", Reflection::ToJson(transform));
    state.Set("components", move(comps));
    Array<EntitySnapshot> entries;
    entries.PushBack(EntitySnapshot{guid, move(state)});
    ExpandPrefabInstances(entries, library);
    AssignSiblingIndices(entries, 1);
    const Array<ecs::Entity> created = ApplyEntitySnapshots(world, entries.View());
    return created.Empty() ? ecs::Entity() : created[0];
}

ecs::Entity RefreshPrefabInstance(ecs::World& world, ecs::Entity root, PrefabLibrary& library, bool revert)
{
    const PrefabInstance* pi = world.Get<const PrefabInstance>(root);
    if (!pi)
        return root;
    Array<ecs::Entity> all;
    CollectHierarchy(world, root, all);
    Array<JsonValue> list;
    for (ecs::Entity e : all)
        list.PushBack(SerializeEntity(world, e));
    Array<EntitySnapshot> entries;
    if (revert) {
        // Only the placement of the root survives a revert.
        JsonValue state = JsonValue::MakeObject();
        for (StringView key : {StringView("guid"), StringView("parent"), StringView("index")})
            if (const JsonValue* v = list[0].Find(key))
                state.Set(key, *v);
        JsonValue comps = JsonValue::MakeObject();
        comps.Set(InstanceKey, list[0].Get("components").Get(InstanceKey));
        if (const Transform* t = world.Get<const Transform>(root))
            comps.Set("Transform", Reflection::ToJson(*t));
        state.Set("components", move(comps));
        entries.PushBack(EntitySnapshot{ParseGuidText(state.Get("guid")), move(state)});
    } else {
        entries = CaptureInstance(world, root, library);
    }
    return Rebuild(world, root, entries, library);
}

bool ApplyPrefabInstance(ecs::World& world, ecs::Entity root, PrefabLibrary& library, asset::AssetManager* assets)
{
    const PrefabInstance* pi = world.Get<const PrefabInstance>(root);
    if (!pi)
        return false;
    const String path = pi->prefab;
    const PrefabLibrary::Prefab* prefab = library.Get(path);
    if (!prefab)
        return false;
    const Guid instance = GetEntityGuid(world, root);
    Array<ecs::Entity> all;
    CollectHierarchy(world, root, all);
    // Prefab ids: members keep their source id, new entities get new ids, nested members derive from their root.
    HashMap<Guid, Guid> localOf;
    localOf.InsertOrAssign(instance, prefab->root);
    for (ecs::Entity e : all) {
        if (e == root)
            continue;
        const PrefabLink* link = world.Get<const PrefabLink>(e);
        if (link && link->instance == instance)
            localOf.InsertOrAssign(GetEntityGuid(world, e), link->source);
        else if (!link)
            localOf.InsertOrAssign(EnsureEntityGuid(world, e), Guid::New());
    }
    for (ecs::Entity e : all)
        if (const PrefabLink* link = world.Get<const PrefabLink>(e); link && e != root && link->instance != instance) {
            const Guid* instanceLocal = localOf.Find(link->instance);
            localOf.InsertOrAssign(GetEntityGuid(world, e), instanceLocal ? PrefabMemberGuid(*instanceLocal, link->source) : Guid::New());
        }
    Array<JsonValue> list = BuildPrefabEntities(world, all.View(), localOf);
    // Our own members carry the link to this instance: in the prefab file they are plain entities.
    for (usize i = 1; i < list.Size(); ++i)
        if (JsonValue* link = list[i]["components"].Find(LinkKey); link && ParseGuidText(link->Get("instance")) == prefab->root)
            list[i]["components"].Remove(LinkKey);
    JsonValue sources;
    CollectSources(world, all.View(), assets, library.ContentRoot(), sources);

    // Every instance's overrides are taken against the old prefab, then rebuilt on the new one. Nested instances are
    // reached through their own roots (outermost first, so inner roots are captured before their outer one rebuilds).
    Array<Guid> roots;
    world.Query<const PrefabInstance>().Each([&](ecs::Entity e, const PrefabInstance& p) {
        if (p.prefab == path)
            roots.PushBack(GetEntityGuid(world, e));
    });
    Array<Array<EntitySnapshot>> captured;
    for (const Guid& g : roots)
        captured.PushBack(CaptureInstance(world, FindEntityByGuid(world, g), library));

    if (!WritePrefabFile(library.PhysicalPath(path), list, move(sources), library))
        return false;
    library.Invalidate(path);
    for (usize i = 0; i < roots.Size(); ++i)
        if (const ecs::Entity r = FindEntityByGuid(world, roots[i]))
            Rebuild(world, r, captured[i], library);
    ZX_LOG_INFO(LogPrefab, "Applied changes to '{}' ({} instance(s) updated)", path, roots.Size());
    return true;
}

ecs::Entity FindPrefabInstanceRoot(ecs::World& world, ecs::Entity e)
{
    if (!world.IsAlive(e))
        return {};
    if (world.Has<PrefabInstance>(e))
        return e;
    if (const PrefabLink* link = world.Get<const PrefabLink>(e))
        return FindEntityByGuid(world, link->instance);
    return {};
}

void GetPrefabOverrides(ecs::World& world, ecs::Entity e, PrefabLibrary& library, Array<String>& out)
{
    out.Clear();
    const ecs::Entity root = FindPrefabInstanceRoot(world, e);
    if (!root)
        return;
    const PrefabInstance* pi = world.Get<const PrefabInstance>(root);
    if (!library.Get(pi->prefab))
        return;
    Array<EntitySnapshot> fresh;
    JsonValue rootEntry = JsonValue::MakeObject();
    const Guid rootGuid = GetEntityGuid(world, root);
    rootEntry.Set("guid", GuidText(rootGuid));
    JsonValue comps = JsonValue::MakeObject();
    JsonValue inst = JsonValue::MakeObject();
    inst.Set("prefab", pi->prefab);
    comps.Set(InstanceKey, move(inst));
    rootEntry.Set("components", move(comps));
    fresh.PushBack(EntitySnapshot{rootGuid, move(rootEntry)});
    ExpandPrefabInstances(fresh, library);
    const Guid guid = GetEntityGuid(world, e);
    const JsonValue* base = nullptr;
    for (const EntitySnapshot& f : fresh)
        if (f.guid == guid)
            base = &f.state;
    if (!base)
        return;
    const JsonValue cur = SerializeEntity(world, e);
    for (const JsonMember& m : cur.Get("components").Members()) {
        if (m.first == InstanceKey || m.first == LinkKey)
            continue;
        const JsonValue* b = base->Get("components").Find(m.first);
        if (!b) {
            out.PushBack(m.first);
            continue;
        }
        const JsonValue d = DiffObjects(CanonicalComponent(m.first, m.second), CanonicalComponent(m.first, *b));
        for (const JsonMember& f : d.Members())
            out.PushBack(String::Format("{}.{}", m.first, f.first));
    }
}

} // namespace zx
