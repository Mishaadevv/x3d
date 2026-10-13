#include "Engine/Materials.h"
#include "Core/FileSystem/FileSystem.h"
#include "Core/HAL/PlatformFile.h"
#include "Core/Log/Log.h"
#include "Core/String/Path.h"

ZX_DEFINE_LOG_CATEGORY_STATIC(LogMaterials, Info);

namespace zx {

using namespace render;

namespace {

void FindFiles(const String& dir, StringView extension, Array<String>& out)
{
    Array<platform::DirectoryEntry> entries;
    platform::ListDirectory(dir, entries);
    for (const platform::DirectoryEntry& e : entries) {
        if (e.name.View().StartsWith("."))
            continue;
        const String path = Path::Join(dir, e.name);
        if (e.isDirectory)
            FindFiles(path, extension, out);
        else if (Path::HasExtension(e.name, extension))
            out.PushBack(path);
    }
}

MaterialBlend ToRenderBlend(MaterialBlendMode m)
{
    return m == MaterialBlendMode::Masked ? MaterialBlend::Masked : m == MaterialBlendMode::Translucent ? MaterialBlend::Translucent
                                                                                                        : MaterialBlend::Opaque;
}

} // namespace

MaterialLibrary::MaterialLibrary(asset::AssetManager& assets, RenderAssets& renderAssets, StringView contentRoot)
    : m_assets(assets), m_renderAssets(renderAssets), m_contentRoot(Path::Normalize(contentRoot))
{
}

void MaterialLibrary::ScanContent()
{
    Array<String> files;
    FindFiles(m_contentRoot, MaterialExtension, files);
    FindFiles(m_contentRoot, ".zmati", files); // parents are loaded on demand
    for (const String& f : files)
        LoadFile(f);
    if (!files.Empty())
        ZX_LOG_INFO(LogMaterials, "Loaded {} material graph(s)", files.Size());
}

Guid MaterialLibrary::LoadFile(StringView physicalPath)
{
    const String path = Path::Normalize(physicalPath);
    String text;
    JsonValue doc;
    JsonParseError error;
    if (!fs::ReadTextFile(path, text) || !ParseJson(text.View(), doc, &error)) {
        ZX_LOG_ERROR(LogMaterials, "Can't read material {}{}", path, error.message.Empty() ? String() : String::Format(": {}", error.message));
        return {};
    }
    if (doc.Get("format").AsString() == "zmati") {
        // Instance: parent graph + parameter overrides.
        const StringView parentRel = doc.Get("parent").AsString();
        const String parentPath = Path::IsAbsolute(parentRel) ? String(parentRel) : Path::Join(m_contentRoot, parentRel);
        Material* parent = FindByPath(parentPath);
        const Guid parentGuid = parent ? parent->guid : LoadFile(parentPath);
        parent = Find(parentGuid);
        if (!parent || parent->IsInstance()) {
            ZX_LOG_ERROR(LogMaterials, "{}: parent material '{}' not found", Path::FileName(path), parentRel);
            return {};
        }
        Guid guid;
        if (const Optional<Guid> g = Guid::Parse(doc.Get("guid").AsString()))
            guid = *g;
        if (!guid.IsValid())
            guid = Guid::New();
        Material* m = Find(guid);
        if (!m) {
            m_materials.PushBack(MakeUnique<Material>());
            m = m_materials.Back().Get();
        }
        m->guid = guid;
        m->path = path;
        m->parent = parentGuid;
        m->overrides = doc.Get("overrides").IsObject() ? doc.Get("overrides") : JsonValue::MakeObject();
        m->dirty = false;
        RebuildInstance(guid);
        return guid;
    }
    MaterialGraph graph;
    Array<String> warnings;
    if (!graph.FromJson(doc, &warnings)) {
        ZX_LOG_ERROR(LogMaterials, "{} is not a material graph", path);
        return {};
    }
    for (const String& w : warnings)
        ZX_LOG_WARN(LogMaterials, "{}: {}", Path::FileName(path), w);
    if (!graph.guid.IsValid()) {
        graph.guid = Guid::New();
        ZX_LOG_WARN(LogMaterials, "{} had no guid; assigned one (save it to keep it)", Path::FileName(path));
    }
    Material* m = Find(graph.guid);
    if (!m) {
        m_materials.PushBack(MakeUnique<Material>());
        m = m_materials.Back().Get();
    }
    m->guid = graph.guid;
    m->path = path;
    m->graph = move(graph);
    m->dirty = false;
    Apply(m->guid);
    return m->guid;
}

Guid MaterialLibrary::Create(StringView physicalPath)
{
    Material m;
    m.graph = MaterialGraph::CreateDefault();
    platform::CreateDirectoryTree(Path::Parent(physicalPath));
    if (!fs::WriteTextFile(physicalPath, ToJsonString(m.graph.ToJson()))) {
        ZX_LOG_ERROR(LogMaterials, "Can't write {}", physicalPath);
        return {};
    }
    return LoadFile(physicalPath);
}

Guid MaterialLibrary::CreateInstance(const Guid& parentGuid, StringView physicalPath)
{
    const Material* parent = Find(parentGuid);
    if (!parent || parent->IsInstance())
        return {};
    JsonValue doc = JsonValue::MakeObject();
    doc.Set("format", "zmati");
    doc.Set("version", i64(1));
    doc.Set("guid", String::Format("{}", Guid::New()));
    const String rel = Path::MakeRelative(parent->path, m_contentRoot);
    doc.Set("parent", rel.Empty() ? parent->path : rel);
    doc.Set("overrides", JsonValue::MakeObject());
    platform::CreateDirectoryTree(Path::Parent(physicalPath));
    if (!fs::WriteTextFile(physicalPath, ToJsonString(doc))) {
        ZX_LOG_ERROR(LogMaterials, "Can't write {}", physicalPath);
        return {};
    }
    return LoadFile(physicalPath);
}

void MaterialLibrary::RebuildInstance(const Guid& guid)
{
    Material* m = Find(guid);
    const Material* parent = m ? Find(m->parent) : nullptr;
    if (!m || !parent)
        return;
    m->graph = parent->graph;
    m->graph.guid = guid;
    for (MaterialGraphNode& n : m->graph.nodes) {
        if (n.name.Empty())
            continue;
        const JsonValue* o = m->overrides.Find(n.name);
        if (!o)
            continue;
        if (const JsonValue* v = o->Find("value"); v && v->IsArray())
            n.value = Vec4(v->Size() > 0 ? (*v)[0].AsFloat() : n.value.x, v->Size() > 1 ? (*v)[1].AsFloat() : n.value.y,
                           v->Size() > 2 ? (*v)[2].AsFloat() : n.value.z, v->Size() > 3 ? (*v)[3].AsFloat() : n.value.w);
        if (const JsonValue* t = o->Find("texture"))
            n.texture = t->AsString();
    }
    Apply(guid);
}

bool MaterialLibrary::Save(const Guid& guid)
{
    Material* m = Find(guid);
    if (!m)
        return false;
    if (m->IsInstance()) {
        const Material* parent = Find(m->parent);
        JsonValue doc = JsonValue::MakeObject();
        doc.Set("format", "zmati");
        doc.Set("version", i64(1));
        doc.Set("guid", String::Format("{}", m->guid));
        const String rel = parent ? Path::MakeRelative(parent->path, m_contentRoot) : String();
        doc.Set("parent", rel.Empty() && parent ? parent->path : rel);
        doc.Set("overrides", m->overrides);
        if (!fs::WriteTextFile(m->path, ToJsonString(doc))) {
            ZX_LOG_ERROR(LogMaterials, "Can't write {}", m->path);
            return false;
        }
        m->dirty = false;
        return true;
    }
    if (!fs::WriteTextFile(m->path, ToJsonString(m->graph.ToJson()))) {
        ZX_LOG_ERROR(LogMaterials, "Can't write {}", m->path);
        return false;
    }
    m->dirty = false;
    return true;
}

MaterialLibrary::Material* MaterialLibrary::Find(const Guid& guid)
{
    for (UniquePtr<Material>& m : m_materials)
        if (m->guid == guid)
            return m.Get();
    return nullptr;
}

MaterialLibrary::Material* MaterialLibrary::FindByPath(StringView physicalPath)
{
    const String path = Path::Normalize(physicalPath);
    for (UniquePtr<Material>& m : m_materials)
        if (m->path.View().EqualsIgnoreCase(path))
            return m.Get();
    return nullptr;
}

asset::AssetId MaterialLibrary::TextureAsset(StringView relativePath, bool normalMap)
{
    const String key = String::Format("{}|{}", relativePath, normalMap);
    if (const asset::AssetId* id = m_textures.Find(key))
        return *id;
    const String physical = Path::IsAbsolute(relativePath) ? String(relativePath) : Path::Join(m_contentRoot, relativePath);
    if (normalMap) {
        // Normal maps must be imported linear (BC5): record that in the .zmeta before the texture is first loaded.
        const String metaPath = physical + ".zmeta";
        String text;
        JsonValue meta;
        if (!fs::ReadTextFile(metaPath, text) || !ParseJson(text.View(), meta) || !meta.IsObject()) {
            meta = JsonValue::MakeObject();
            meta.Set("guid", String::Format("{}", Guid::New()));
            meta.Set("importer", "Texture");
        }
        JsonValue& settings = meta["settings"];
        if (!settings.IsObject())
            settings = JsonValue::MakeObject();
        if (settings.Get("normalMap").AsBool() == false || settings.Get("srgb").AsBool(true)) {
            settings.Set("normalMap", true);
            settings.Set("srgb", false);
            fs::WriteTextFile(metaPath, ToJsonString(meta));
        }
    }
    const asset::AssetId id = m_assets.LoadPath(physical);
    m_textures.InsertOrAssign(key, id);
    return id;
}

void MaterialLibrary::Apply(const Guid& guid)
{
    Material* m = Find(guid);
    if (!m)
        return;
    m->compiled = CompileMaterialGraph(m->graph);
    for (const String& e : m->compiled.errors)
        ZX_LOG_WARN(LogMaterials, "{}: {}", Path::FileName(m->path), e);
    GraphMaterialDesc desc;
    desc.blend = ToRenderBlend(m->graph.blend);
    desc.doubleSided = m->graph.twoSided;
    desc.unlit = m->graph.unlit;
    desc.alphaCutoff = m->graph.alphaCutoff;
    if (m->compiled.Ok()) {
        desc.shaderHash = m->compiled.hash;
        desc.shaderCode = m->compiled.code;
    }
    WriteMaterialParams(m->graph, m->compiled, desc.params);
    for (const MaterialParamSlot& slot : m->compiled.params) {
        if (slot.kind != MaterialParamSlot::Kind::Texture)
            continue;
        const MaterialGraphNode* n = m->graph.FindNode(slot.node);
        if (n && !n->texture.Empty())
            desc.textures.PushBack(Pair<u32, asset::AssetId>{slot.offset, TextureAsset(n->texture, n->normalMap)});
    }
    m_renderAssets.SetGraphMaterial(guid, desc);
    m->shader = m_renderAssets.GetMaterialInfo(m_renderAssets.GetMaterialIndex(guid)).shader;
    // Instances follow their parent's structure and defaults.
    if (!m->IsInstance()) {
        Array<Guid> instances;
        for (const UniquePtr<Material>& i : m_materials)
            if (i->parent == guid)
                instances.PushBack(i->guid);
        for (const Guid& i : instances)
            RebuildInstance(i);
    }
}

MaterialShaderCache::State MaterialLibrary::ShaderState(const Guid& guid) const
{
    for (const UniquePtr<Material>& m : m_materials)
        if (m->guid == guid)
            return m->shader ? m_renderAssets.Shaders().GetState(m->shader) : MaterialShaderCache::State::Failed;
    return MaterialShaderCache::State::Failed;
}

} // namespace zx
