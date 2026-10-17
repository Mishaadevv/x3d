#include "Asset/AssetManager.h"
#include "Core/Containers/Algorithm.h"
#include "Core/FileSystem/FileSystem.h"
#include "Core/Hash/Hash.h"
#include "Core/Jobs/JobSystem.h"
#include "Core/Log/Log.h"
#include "Core/Serialization/Binary.h"
#include "Core/Serialization/Json.h"
#include "Core/String/Path.h"
#include "Core/Threading/Thread.h"

ZX_DEFINE_LOG_CATEGORY_STATIC(LogAssets, Info);

namespace zx::asset {

namespace {

// Namespace for path-derived GUIDs (sources without a .zmeta when writeMetaFiles is off).
constexpr Guid PathNamespace(0x5A58334441535345ull, 0x5450415448000001ull);

enum class SourceKind : u8
{
    Scene,
    Image,
};

bool IsSceneExtension(StringView path)
{
    return Path::HasExtension(path, "gltf") || Path::HasExtension(path, "glb") || Path::HasExtension(path, "obj");
}

bool IsImageExtension(StringView path)
{
    for (StringView ext : {"png", "jpg", "jpeg", "tga", "bmp", "hdr", "exr", "dds"})
        if (Path::HasExtension(path, ext))
            return true;
    return false;
}

enum class EntryOrigin : u8
{
    None,
    DdcFile,     // cooked file in the DDC
    TextureFile, // image file cooked on demand (keyed by its content)
    Memory,      // registered in memory (built-in / procedural); never released
};

struct TextureSettings
{
    bool srgb = true;
    bool normalMap = false;
    bool alphaCoverage = false;
    f32 alphaCutoff = 0.5f;
    TextureCompression compression = TextureCompression::Auto;
    TextureWrap wrapU = TextureWrap::Repeat;
    TextureWrap wrapV = TextureWrap::Repeat;
};

void RunJob(Function<void(), 48> fn)
{
    if (jobs::IsInitialized())
        jobs::Schedule(move(fn), {}, JobPriority::Low);
    else
        fn();
}

} // namespace

struct AssetManager::Entry
{
    AssetId id;
    AssetId source;
    AssetType type = AssetType::Unknown;
    std::atomic<AssetState> state{AssetState::Unknown};
    EntryOrigin origin = EntryOrigin::None;
    String file; // DDC file or image path
    String name;
    TextureSettings texture;
    String error;
    UniquePtr<CookedMesh> mesh;
    UniquePtr<CookedTexture> tex;
    UniquePtr<MaterialAsset> material;
    UniquePtr<PrefabAsset> prefab;
    UniquePtr<Skeleton> skeleton;
    UniquePtr<AnimationClip> animation;
};

struct AssetManager::Source
{
    AssetId id;
    String path;
    SourceKind kind = SourceKind::Scene;
    f32 importScale = 1.0f; // scenes: .zmeta settings.scale (models authored in other units)
    std::atomic<u8> state{0}; // 0 = not cooked, 1 = cooking, 2 = cooked, 3 = failed
};

AssetManager::AssetManager(const AssetManagerSettings& settings) : m_settings(settings)
{
    if (!m_settings.ddcDirectory.Empty())
        fs::CreateDirectories(m_settings.ddcDirectory);
}

AssetManager::~AssetManager() { WaitIdle(); }

void AssetManager::WaitIdle()
{
    while (m_inFlight.load(std::memory_order_acquire) != 0)
        Thread::SleepMs(1);
}

void AssetManager::GetAssetIds(Array<AssetId>& out) const
{
    ScopedLock lock(m_mutex);
    for (const auto& kv : m_entries)
        if (kv.second->origin != EntryOrigin::Memory)
            out.PushBack(kv.first);
}

String AssetManager::DdcPath(u64 key, StringView suffix) const
{
    char hex[17];
    for (u32 i = 0; i < 16; ++i)
        hex[i] = "0123456789abcdef"[(key >> ((15 - i) * 4)) & 15];
    hex[16] = 0;
    return Path::Join(m_settings.ddcDirectory, String::Format("{}{}", StringView(hex, 16), suffix));
}

AssetManager::Entry* AssetManager::FindEntry(AssetId id) const
{
    const UniquePtr<Entry>* e = m_entries.Find(id);
    return e ? e->Get() : nullptr;
}

AssetManager::Entry& AssetManager::GetOrAddEntry(AssetId id)
{
    if (Entry* e = FindEntry(id))
        return *e;
    UniquePtr<Entry> e = MakeUnique<Entry>();
    e->id = id;
    Entry* raw = e.Get();
    m_entries.InsertOrAssign(id, move(e));
    return *raw;
}

Result<AssetId> AssetManager::RegisterSource(StringView pathIn)
{
    const String path = Path::Normalize(pathIn);
    const bool scene = IsSceneExtension(path);
    if (!scene && !IsImageExtension(path))
        return Error::Make(ErrorCode::Unsupported, "Unsupported asset source '{}'", Path::FileName(path));
    if (!fs::Exists(path))
        return Error::Make(ErrorCode::NotFound, "Asset source not found: {}", path);

    // GUID from the .zmeta (created on first import).
    AssetId id;
    TextureSettings texSettings;
    f32 importScale = 1.0f;
    const String metaPath = path + ".zmeta";
    String metaText;
    JsonValue meta;
    if (fs::ReadTextFile(metaPath, metaText) && ParseJson(metaText.View(), meta)) {
        if (Optional<Guid> g = Guid::Parse(meta.Get("guid").AsString()))
            id = *g;
        const JsonValue& s = meta.Get("settings");
        texSettings.srgb = s.Get("srgb").AsBool(true);
        texSettings.normalMap = s.Get("normalMap").AsBool(false);
        texSettings.compression = ParseTextureCompression(s.Get("compression").AsString());
        importScale = Clamp(s.Get("scale").AsFloat(1.0f), 1e-4f, 1e4f);
    }
    if (!id.IsValid()) {
        if (m_settings.writeMetaFiles) {
            id = Guid::New();
            JsonValue m = JsonValue::MakeObject();
            m.Set("guid", JsonValue(String::Format("{}", id)));
            m.Set("importer", JsonValue(scene ? "Scene" : "Texture"));
            JsonValue settings = JsonValue::MakeObject();
            if (!scene) {
                settings.Set("srgb", JsonValue(texSettings.srgb));
                settings.Set("normalMap", JsonValue(texSettings.normalMap));
            }
            m.Set("settings", move(settings));
            if (!fs::WriteTextFile(metaPath, ToJsonString(m)))
                ZX_LOG_WARN(LogAssets, "Can't write {}", metaPath);
        } else {
            id = Guid::FromName(PathNamespace, path);
        }
    }

    ScopedLock lock(m_mutex);
    if (!m_sources.Contains(id)) {
        UniquePtr<Source> s = MakeUnique<Source>();
        s->id = id;
        s->path = path;
        s->kind = scene ? SourceKind::Scene : SourceKind::Image;
        s->importScale = importScale;
        m_sources.InsertOrAssign(id, move(s));
        Entry& e = GetOrAddEntry(id);
        e.source = id;
        e.name = String(Path::Stem(path));
        if (scene) {
            e.type = AssetType::Prefab;
        } else {
            e.type = AssetType::Texture;
            e.origin = EntryOrigin::TextureFile;
            e.file = path;
            e.texture = texSettings;
        }
    }
    return id;
}

AssetId AssetManager::LoadPath(StringView path)
{
    Result<AssetId> id = RegisterSource(path);
    if (!id) {
        ZX_LOG_ERROR(LogAssets, "{}", id.GetError().message);
        return {};
    }
    Load(*id);
    return *id;
}

void AssetManager::Load(AssetId id)
{
    if (!id.IsValid())
        return;
    // Decide under the lock, launch after it (jobs may run inline when the job system is off).
    Entry* load = nullptr;
    Source* cook = nullptr;
    {
        ScopedLock lock(m_mutex);
        Entry& e = GetOrAddEntry(id);
        if (e.state.load(std::memory_order_acquire) != AssetState::Unknown)
            return;
        if (e.origin != EntryOrigin::None) {
            e.state.store(AssetState::Loading, std::memory_order_release);
            load = &e;
        } else {
            e.state.store(AssetState::Queued, std::memory_order_release);
            if (UniquePtr<Source>* s = m_sources.Find(id))
                cook = s->Get();
        }
    }
    if (load)
        StartEntryLoad(*load);
    if (cook)
        StartSourceCook(*cook);
}

void AssetManager::StartEntryLoad(Entry& e)
{
    m_inFlight.fetch_add(1, std::memory_order_acq_rel);
    m_jobsStarted.fetch_add(1, std::memory_order_relaxed);
    RunJob([this, &e] {
        SetWork(e.origin == EntryOrigin::TextureFile ? StringView(Path::FileName(e.file)) : e.name.View());
        LoadEntry(e);
        m_jobsFinished.fetch_add(1, std::memory_order_relaxed);
        m_inFlight.fetch_sub(1, std::memory_order_acq_rel);
    });
}

void AssetManager::StartSourceCook(Source& s)
{
    u8 expected = 0;
    if (!s.state.compare_exchange_strong(expected, 1))
        return;
    m_inFlight.fetch_add(1, std::memory_order_acq_rel);
    m_jobsStarted.fetch_add(1, std::memory_order_relaxed);
    RunJob([this, &s] {
        SetWork(Path::FileName(s.path));
        CookSource(s);
        m_jobsFinished.fetch_add(1, std::memory_order_relaxed);
        m_inFlight.fetch_sub(1, std::memory_order_acq_rel);
    });
}

void AssetManager::SetWork(StringView work)
{
    ScopedLock lock(m_workMutex);
    m_currentWork = work;
}

String AssetManager::CurrentWork() const
{
    if (IsIdle())
        return String();
    ScopedLock lock(m_workMutex);
    return m_currentWork;
}

void AssetManager::Fail(Entry& e, StringView message)
{
    {
        ScopedLock lock(m_mutex);
        e.error = String(message);
    }
    ZX_LOG_ERROR(LogAssets, "{} ({}): {}", e.name, AssetTypeName(e.type), message);
    e.state.store(AssetState::Failed, std::memory_order_release);
}

// --- Source cooking ---------------------------------------------------------------------------------------------------

namespace {

struct ManifestItem
{
    AssetId id;
    AssetType type = AssetType::Unknown;
    EntryOrigin origin = EntryOrigin::None;
    String file;
    String name;
    TextureSettings texture;
};

void WriteManifest(const Array<ManifestItem>& items, Array<u8>& out)
{
    BinaryWriter w(out);
    w.Write<u32>(0x4E414D5A); // "ZMAN"
    w.Write<u32>(CookerVersion);
    w.WriteVarU64(items.Size());
    for (const ManifestItem& it : items) {
        w.Write<u64>(it.id.high);
        w.Write<u64>(it.id.low);
        w.Write<u8>(u8(it.type));
        w.Write<u8>(u8(it.origin));
        w.WriteString(it.file);
        w.WriteString(it.name);
        w.WriteBool(it.texture.srgb);
        w.WriteBool(it.texture.normalMap);
        w.WriteBool(it.texture.alphaCoverage);
        w.Write<f32>(it.texture.alphaCutoff);
        w.Write<u8>(u8(it.texture.wrapU));
        w.Write<u8>(u8(it.texture.wrapV));
    }
}

bool ReadManifest(ByteSpan data, Array<ManifestItem>& items)
{
    BinaryReader r(data);
    if (r.Read<u32>() != 0x4E414D5A || r.Read<u32>() != CookerVersion)
        return false;
    const u64 n = r.ReadVarU64();
    if (n > (1u << 20))
        return false;
    for (u64 i = 0; i < n && !r.HasError(); ++i) {
        ManifestItem& it = items.AddDefaulted();
        it.id.high = r.Read<u64>();
        it.id.low = r.Read<u64>();
        it.type = AssetType(r.Read<u8>());
        it.origin = EntryOrigin(r.Read<u8>());
        it.file = r.ReadString();
        it.name = r.ReadString();
        it.texture.srgb = r.ReadBool();
        it.texture.normalMap = r.ReadBool();
        it.texture.alphaCoverage = r.ReadBool();
        it.texture.alphaCutoff = r.Read<f32>();
        it.texture.wrapU = TextureWrap(r.Read<u8>());
        it.texture.wrapV = TextureWrap(r.Read<u8>());
    }
    return !r.HasError();
}

AssetId SubAsset(AssetId source, StringView kind, usize index)
{
    return Guid::FromName(source, String::Format("{}/{}", kind, index));
}

} // namespace

void AssetManager::CookSource(Source& s)
{
    // Content key: the source file plus its companion files (.bin buffers / .mtl libraries).
    XXHash64Stream h(CookerVersion);
    Array<u8> bytes;
    if (!fs::ReadFile(s.path, bytes)) {
        s.state.store(3);
        ZX_LOG_ERROR(LogAssets, "Can't read {}", s.path);
        return;
    }
    h.Update(bytes.Data(), bytes.Size());
    {
        Array<platform::DirectoryEntry> siblings;
        fs::List(Path::Parent(s.path), siblings, fs::ListOptions{.includeDirectories = false});
        Sort(siblings, [](const platform::DirectoryEntry& a, const platform::DirectoryEntry& b) { return a.name < b.name; });
        for (const platform::DirectoryEntry& f : siblings)
            if (Path::HasExtension(f.name, "bin") || Path::HasExtension(f.name, "mtl")) {
                Array<u8> companion;
                if (fs::ReadFile(Path::Join(Path::Parent(s.path), f.name), companion))
                    h.Update(companion.Data(), companion.Size());
            }
    }
    h.UpdateValue(m_settings.generateMeshLods);
    h.UpdateValue(m_settings.textureMaxSize);
    h.UpdateValue(s.importScale);
    // The manifest holds sub-asset ids derived from the source GUID and absolute paths of external textures, so a
    // copy of the same file (another project, a duplicate) must not share it.
    h.UpdateValue(s.id.high);
    h.UpdateValue(s.id.low);
    h.Update(s.path.Data(), s.path.Size());
    const u64 key = h.Digest();
    const String manifestPath = DdcPath(key, ".zmanifest");

    Array<ManifestItem> items;
    Array<u8> manifestBytes;
    bool hit = fs::ReadFile(manifestPath, manifestBytes) && ReadManifest(manifestBytes.View(), items);
    if (hit) {
        for (const ManifestItem& it : items)
            if (it.origin == EntryOrigin::DdcFile && !fs::Exists(it.file))
                hit = false;
    }
    if (hit) {
        m_ddcHits.fetch_add(1, std::memory_order_relaxed);
    } else {
        items.Clear();
        ImportOptions importOptions;
        importOptions.scale = s.importScale;
        Result<ImportedScene> imported = ImportScene(s.path, importOptions);
        if (!imported) {
            s.state.store(3);
            ScopedLock lock(m_mutex);
            if (Entry* e = FindEntry(s.id)) {
                e->error = imported.GetError().message;
                e->state.store(AssetState::Failed, std::memory_order_release);
            }
            ZX_LOG_ERROR(LogAssets, "Import of {} failed: {}", s.path, imported.GetError().message);
            return;
        }
        ImportedScene& scene = *imported;
        m_cooked.fetch_add(1, std::memory_order_relaxed);

        // Textures: external images are cooked lazily (keyed by their own content), embedded ones now.
        Array<TextureSettings> texSettings;
        texSettings.Resize(scene.textures.Size());
        for (usize t = 0; t < scene.textures.Size(); ++t) {
            texSettings[t].srgb = scene.textures[t].srgb;
            texSettings[t].normalMap = scene.textures[t].normalMap;
            texSettings[t].wrapU = scene.textures[t].wrapU;
            texSettings[t].wrapV = scene.textures[t].wrapV;
        }
        for (const ImportedMaterial& m : scene.materials)
            if (m.alphaMode == AlphaMode::Mask && m.baseColorTexture.IsValid()) {
                texSettings[usize(m.baseColorTexture.texture)].alphaCoverage = true;
                texSettings[usize(m.baseColorTexture.texture)].alphaCutoff = m.alphaCutoff;
            }
        Array<ManifestItem> textureItems;
        textureItems.Resize(scene.textures.Size());
        SetWork(String::Format("{}: textures", Path::FileName(s.path)));
        jobs::ParallelFor(u32(scene.textures.Size()), 1, [&](u32 begin, u32 end) {
            for (u32 t = begin; t < end; ++t) {
                const ImportedTexture& src = scene.textures[t];
                ManifestItem& it = textureItems[t];
                it.id = SubAsset(s.id, "texture", t);
                it.type = AssetType::Texture;
                it.name = src.name.Empty() ? String::Format("texture{}", t) : src.name;
                it.texture = texSettings[t];
                if (!src.path.Empty()) {
                    it.origin = EntryOrigin::TextureFile;
                    it.file = src.path;
                    continue;
                }
                Result<Image> img = DecodeImage(src.data.View(), src.mimeType == "image/jpeg" ? "x.jpg" : "x.png");
                if (!img)
                    continue; // left without origin -> fails on load
                TextureCookOptions o;
                o.srgb = it.texture.srgb;
                o.normalMap = it.texture.normalMap;
                o.preserveAlphaCoverage = it.texture.alphaCoverage;
                o.alphaCutoff = it.texture.alphaCutoff;
                o.maxSize = m_settings.textureMaxSize;
                Result<CookedTexture> cooked = CookTexture(*img, o);
                if (!cooked)
                    continue;
                cooked->name = it.name;
                cooked->wrapU = it.texture.wrapU;
                cooked->wrapV = it.texture.wrapV;
                Array<u8> out;
                Serialize(*cooked, out);
                it.origin = EntryOrigin::DdcFile;
                it.file = DdcPath(key, String::Format("_tex{}.zasset", t));
                fs::WriteFile(it.file, out.View());
            }
        });
        items.Append(textureItems.View());

        // Materials.
        for (usize m = 0; m < scene.materials.Size(); ++m) {
            const ImportedMaterial& im = scene.materials[m];
            MaterialAsset ma;
            ma.name = im.name;
            ma.shadingModel = im.unlit ? ShadingModel::Unlit : ShadingModel::DefaultLit;
            ma.alphaMode = im.alphaMode;
            ma.alphaCutoff = im.alphaCutoff;
            ma.doubleSided = im.doubleSided;
            ma.baseColor = im.baseColor;
            ma.metallic = im.metallic;
            ma.roughness = im.roughness;
            ma.emissive = im.emissive;
            ma.normalScale = im.normalTexture.strength;
            ma.occlusionStrength = im.occlusionTexture.strength;
            ma.uvScale = im.baseColorTexture.scale;
            ma.uvOffset = im.baseColorTexture.offset;
            ma.uvRotation = im.baseColorTexture.rotation;
            auto tex = [&](const TextureRef& r) { return r.IsValid() ? SubAsset(s.id, "texture", usize(r.texture)) : AssetId(); };
            ma.baseColorTexture = tex(im.baseColorTexture);
            ma.metallicRoughnessTexture = tex(im.metallicRoughnessTexture);
            ma.normalTexture = tex(im.normalTexture);
            ma.occlusionTexture = tex(im.occlusionTexture);
            ma.emissiveTexture = tex(im.emissiveTexture);
            Array<u8> out;
            Serialize(ma, out);
            ManifestItem& it = items.AddDefaulted();
            it.id = SubAsset(s.id, "material", m);
            it.type = AssetType::Material;
            it.origin = EntryOrigin::DdcFile;
            it.name = im.name;
            it.file = DdcPath(key, String::Format("_mat{}.zasset", m));
            fs::WriteFile(it.file, out.View());
        }

        // Meshes (LODs + optimization dominate; parallel per mesh).
        Array<Array<u32>> slotMaterials;
        slotMaterials.Resize(scene.meshes.Size());
        Array<ManifestItem> meshItems;
        meshItems.Resize(scene.meshes.Size());
        SetWork(String::Format("{}: optimizing meshes, building LODs", Path::FileName(s.path)));
        jobs::ParallelFor(u32(scene.meshes.Size()), 1, [&](u32 begin, u32 end) {
            for (u32 m = begin; m < end; ++m) {
                CookedMesh cooked = CookMesh(scene.meshes[m], m_settings.generateMeshLods);
                if (cooked.name.Empty())
                    cooked.name = String::Format("{}_{}", Path::Stem(s.path), m);
                slotMaterials[m] = cooked.slotSourceMaterial;
                Array<u8> out;
                Serialize(cooked, out);
                ManifestItem& it = meshItems[m];
                it.id = SubAsset(s.id, "mesh", m);
                it.type = AssetType::Mesh;
                it.origin = EntryOrigin::DdcFile;
                it.name = cooked.name;
                it.file = DdcPath(key, String::Format("_mesh{}.zasset", m));
                fs::WriteFile(it.file, out.View());
            }
        });
        items.Append(meshItems.View());

        // Skeletons and animation clips.
        auto writeItem = [&](AssetId id, AssetType type, const String& name, StringView suffix, const Array<u8>& out) {
            ManifestItem& it = items.AddDefaulted();
            it.id = id;
            it.type = type;
            it.origin = EntryOrigin::DdcFile;
            it.name = name;
            it.file = DdcPath(key, suffix);
            fs::WriteFile(it.file, out.View());
        };
        for (usize k = 0; k < scene.skeletons.Size(); ++k) {
            Array<u8> out;
            Serialize(scene.skeletons[k], out);
            const String name = scene.skeletons[k].name.Empty() ? String::Format("{}_Skeleton{}", Path::Stem(s.path), k) : scene.skeletons[k].name;
            writeItem(SubAsset(s.id, "skeleton", k), AssetType::Skeleton, name, String::Format("_skel{}.zasset", k), out);
        }
        for (usize k = 0; k < scene.animations.Size(); ++k) {
            Array<u8> out;
            Serialize(scene.animations[k], out);
            writeItem(SubAsset(s.id, "animation", k), AssetType::Animation, scene.animations[k].name, String::Format("_anim{}.zasset", k), out);
        }

        // Prefab: breadth-first from the roots so parents precede children.
        PrefabAsset prefab;
        prefab.name = String(Path::Stem(s.path));
        Array<i32> queue = Array<i32>(scene.rootNodes.View());
        Array<i32> prefabIndex;
        prefabIndex.Resize(scene.nodes.Size(), -1);
        for (usize q = 0; q < queue.Size(); ++q) {
            const i32 ni = queue[q];
            if (ni < 0 || usize(ni) >= scene.nodes.Size() || prefabIndex[usize(ni)] >= 0)
                continue;
            const ImportedNode& node = scene.nodes[usize(ni)];
            if (node.isBone && node.mesh < 0)
                continue; // bones live in the skeleton asset, not as entities (their subtree too)
            prefabIndex[usize(ni)] = i32(prefab.nodes.Size());
            PrefabNode& pn = prefab.nodes.AddDefaulted();
            pn.name = node.name;
            pn.local = node.local;
            pn.parent = node.parent >= 0 ? prefabIndex[usize(node.parent)] : -1;
            if (node.mesh >= 0) {
                pn.mesh = SubAsset(s.id, "mesh", usize(node.mesh));
                for (u32 material : slotMaterials[usize(node.mesh)])
                    pn.materials.PushBack(SubAsset(s.id, "material", material));
                if (node.skin >= 0) {
                    pn.skeleton = SubAsset(s.id, "skeleton", usize(node.skin));
                    pn.skinToNode = node.skinToNode;
                    for (usize a = 0; a < scene.animations.Size(); ++a)
                        if (scene.animationSkeleton[a] == u32(node.skin))
                            pn.animations.PushBack(SubAsset(s.id, "animation", a));
                }
            }
            for (i32 c : node.children)
                queue.PushBack(c);
        }
        Array<u8> out;
        Serialize(prefab, out);
        ManifestItem& pi = items.AddDefaulted();
        pi.id = s.id;
        pi.type = AssetType::Prefab;
        pi.origin = EntryOrigin::DdcFile;
        pi.name = prefab.name;
        pi.file = DdcPath(key, "_prefab.zasset");
        fs::WriteFile(pi.file, out.View());

        Array<u8> manifest;
        WriteManifest(items, manifest);
        fs::WriteFile(manifestPath, manifest.View());
        ZX_LOG_INFO(LogAssets, "Cooked {}: {} meshes, {} materials, {} textures, {} skeletons, {} animations", Path::FileName(s.path),
                    scene.meshes.Size(), scene.materials.Size(), scene.textures.Size(), scene.skeletons.Size(), scene.animations.Size());
    }

    // Register sub-assets and start the loads that were waiting for this source.
    Array<Entry*> launch;
    {
        ScopedLock lock(m_mutex);
        for (const ManifestItem& it : items) {
            Entry& e = GetOrAddEntry(it.id);
            e.source = s.id;
            e.type = it.type;
            e.origin = it.origin;
            e.file = it.file;
            e.name = it.name;
            e.texture = it.texture;
            if (e.state.load(std::memory_order_acquire) == AssetState::Queued) {
                if (e.origin == EntryOrigin::None) {
                    e.state.store(AssetState::Failed, std::memory_order_release);
                } else {
                    e.state.store(AssetState::Loading, std::memory_order_release);
                    launch.PushBack(&e);
                }
            }
        }
        s.state.store(2);
    }
    for (Entry* e : launch)
        StartEntryLoad(*e);
}

// --- Entry loading ----------------------------------------------------------------------------------------------------

void AssetManager::LoadEntry(Entry& e)
{
    Array<u8> bytes;
    if (e.origin == EntryOrigin::TextureFile) {
        if (!fs::ReadFile(e.file, bytes))
            return Fail(e, String::Format("can't read {}", e.file));
        XXHash64Stream h(CookerVersion);
        h.Update(bytes.Data(), bytes.Size());
        h.UpdateValue(e.texture.srgb);
        h.UpdateValue(e.texture.normalMap);
        h.UpdateValue(e.texture.alphaCoverage);
        h.UpdateValue(e.texture.alphaCutoff);
        h.UpdateValue(e.texture.compression);
        h.UpdateValue(m_settings.textureMaxSize);
        const String cookedPath = DdcPath(h.Digest(), ".ztex");
        Array<u8> cookedBytes;
        UniquePtr<CookedTexture> tex = MakeUnique<CookedTexture>();
        if (fs::ReadFile(cookedPath, cookedBytes) && Deserialize(cookedBytes.View(), *tex)) {
            m_ddcHits.fetch_add(1, std::memory_order_relaxed);
        } else {
            Result<Image> img = DecodeImage(bytes.View(), e.file);
            if (!img)
                return Fail(e, img.GetError().message);
            TextureCookOptions o;
            o.srgb = e.texture.srgb;
            o.normalMap = e.texture.normalMap;
            o.preserveAlphaCoverage = e.texture.alphaCoverage;
            o.alphaCutoff = e.texture.alphaCutoff;
            o.compression = e.texture.compression;
            o.maxSize = m_settings.textureMaxSize;
            Result<CookedTexture> cooked = CookTexture(*img, o);
            if (!cooked)
                return Fail(e, cooked.GetError().message);
            *tex = move(*cooked);
            tex->name = e.name;
            Array<u8> out;
            Serialize(*tex, out);
            fs::WriteFile(cookedPath, out.View());
            m_cooked.fetch_add(1, std::memory_order_relaxed);
        }
        tex->wrapU = e.texture.wrapU;
        tex->wrapV = e.texture.wrapV;
        e.tex = move(tex);
        e.state.store(AssetState::Loaded, std::memory_order_release);
        return;
    }
    if (!fs::ReadFile(e.file, bytes))
        return Fail(e, String::Format("missing cooked file {}", e.file));
    bool ok = false;
    switch (PeekAssetType(bytes.View())) {
    case AssetType::Mesh:
        e.mesh = MakeUnique<CookedMesh>();
        ok = Deserialize(bytes.View(), *e.mesh);
        break;
    case AssetType::Texture:
        e.tex = MakeUnique<CookedTexture>();
        ok = Deserialize(bytes.View(), *e.tex);
        break;
    case AssetType::Material:
        e.material = MakeUnique<MaterialAsset>();
        ok = Deserialize(bytes.View(), *e.material);
        break;
    case AssetType::Prefab:
        e.prefab = MakeUnique<PrefabAsset>();
        ok = Deserialize(bytes.View(), *e.prefab);
        break;
    case AssetType::Skeleton:
        e.skeleton = MakeUnique<Skeleton>();
        ok = Deserialize(bytes.View(), *e.skeleton);
        break;
    case AssetType::Animation:
        e.animation = MakeUnique<AnimationClip>();
        ok = Deserialize(bytes.View(), *e.animation);
        break;
    default: break;
    }
    if (!ok)
        return Fail(e, String::Format("corrupt cooked file {}", e.file));
    e.state.store(AssetState::Loaded, std::memory_order_release);
}

// --- Queries ----------------------------------------------------------------------------------------------------------

AssetState AssetManager::GetState(AssetId id) const
{
    ScopedLock lock(m_mutex);
    const Entry* e = FindEntry(id);
    return e ? e->state.load(std::memory_order_acquire) : AssetState::Unknown;
}

AssetType AssetManager::GetType(AssetId id) const
{
    ScopedLock lock(m_mutex);
    const Entry* e = FindEntry(id);
    return e ? e->type : AssetType::Unknown;
}

String AssetManager::GetSourcePath(AssetId id) const
{
    ScopedLock lock(m_mutex);
    AssetId source = id;
    if (const Entry* e = FindEntry(id); e && e->source.IsValid())
        source = e->source;
    const UniquePtr<Source>* s = m_sources.Find(source);
    return s ? (*s)->path : String();
}

String AssetManager::GetErrorMessage(AssetId id) const
{
    ScopedLock lock(m_mutex);
    const Entry* e = FindEntry(id);
    return e ? e->error : String();
}

#define ZX_ASSET_GETTER(Fn, T, member)                                                                                 \
    const T* AssetManager::Fn(AssetId id) const                                                                        \
    {                                                                                                                  \
        ScopedLock lock(m_mutex);                                                                                      \
        const Entry* e = FindEntry(id);                                                                                \
        return e && e->state.load(std::memory_order_acquire) == AssetState::Loaded ? e->member.Get() : nullptr;        \
    }
ZX_ASSET_GETTER(GetMesh, CookedMesh, mesh)
ZX_ASSET_GETTER(GetTexture, CookedTexture, tex)
ZX_ASSET_GETTER(GetMaterial, MaterialAsset, material)
ZX_ASSET_GETTER(GetPrefab, PrefabAsset, prefab)
ZX_ASSET_GETTER(GetSkeleton, Skeleton, skeleton)
ZX_ASSET_GETTER(GetAnimation, AnimationClip, animation)
#undef ZX_ASSET_GETTER

String AssetManager::GetName(AssetId id) const
{
    ScopedLock lock(m_mutex);
    const Entry* e = FindEntry(id);
    return e ? e->name : String();
}

void AssetManager::AddMemoryMesh(AssetId id, CookedMesh mesh)
{
    ScopedLock lock(m_mutex);
    Entry& e = GetOrAddEntry(id);
    e.source = id;
    e.type = AssetType::Mesh;
    e.origin = EntryOrigin::Memory;
    e.name = mesh.name;
    e.mesh = MakeUnique<CookedMesh>(move(mesh));
    e.state.store(AssetState::Loaded, std::memory_order_release);
}

void AssetManager::ReleaseCpuData(AssetId id)
{
    ScopedLock lock(m_mutex);
    if (Entry* e = FindEntry(id); e && e->origin != EntryOrigin::Memory && e->state.load(std::memory_order_acquire) == AssetState::Loaded) {
        e->mesh.Reset();
        e->tex.Reset();
    }
}

AssetManager::Stats AssetManager::GetStats() const
{
    ScopedLock lock(m_mutex);
    Stats s;
    for (const auto& kv : m_entries) {
        ++s.assets;
        const AssetState st = kv.second->state.load(std::memory_order_acquire);
        s.loaded += st == AssetState::Loaded ? 1 : 0;
        s.failed += st == AssetState::Failed ? 1 : 0;
    }
    s.inFlight = m_inFlight.load(std::memory_order_acquire);
    s.jobsStarted = m_jobsStarted.load(std::memory_order_relaxed);
    s.jobsFinished = m_jobsFinished.load(std::memory_order_relaxed);
    s.cookedThisSession = m_cooked.load(std::memory_order_relaxed);
    s.ddcHits = m_ddcHits.load(std::memory_order_relaxed);
    return s;
}

} // namespace zx::asset
