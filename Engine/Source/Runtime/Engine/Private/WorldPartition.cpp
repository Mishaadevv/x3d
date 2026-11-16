#include "Engine/WorldPartition.h"
#include "Core/FileSystem/FileSystem.h"
#include "Core/HAL/PlatformFile.h"
#include "Core/Log/Log.h"
#include "Core/String/Path.h"
#include "Core/Containers/Algorithm.h"
#include "Core/String/Parse.h"
#include "Core/Time/Time.h"
#include "Scene/Hierarchy.h"
#include "Asset/AssetManager.h"
#include "Core/Reflection/ReflectionSerialization.h"
#include "Core/Serialization/Binary.h"
#include "Engine/Foliage.h"
#include "Engine/Impostor.h"
#include "Engine/SceneRendering.h"
#include "Core/Hash/Hash.h"
#include "Engine/Terrain.h"

#include <cmath>

namespace zx {

ZX_DEFINE_LOG_CATEGORY_STATIC(LogWorldPartition, Info);

namespace {
Guid GuidOf(const JsonValue& v)
{
    const Optional<Guid> g = Guid::Parse(v.AsString());
    return g ? *g : Guid();
}
} // namespace

ZX_REFLECT_STRUCT_BEGIN(SpatiallyLoaded)
    ZX_FIELD(enabled, Meta::Tooltip("Streams in and out with its World Partition cell (off: always loaded)"))
ZX_REFLECT_STRUCT_END()

JsonValue WorldPartitionToJson(const WorldPartitionSettings& s)
{
    JsonValue v = JsonValue::MakeObject();
    v.Set("enabled", s.enabled);
    v.Set("cellSize", f64(s.cellSize));
    v.Set("loadRange", f64(s.loadRange));
    v.Set("hlodRange", f64(s.hlodRange));
    return v;
}

WorldPartitionSettings WorldPartitionFromJson(const JsonValue& v)
{
    WorldPartitionSettings s;
    s.enabled = v.Get("enabled").AsBool(false);
    s.cellSize = Clamp(v.Get("cellSize").AsFloat(256.0f), 16.0f, 4096.0f);
    s.loadRange = Clamp(v.Get("loadRange").AsFloat(640.0f), 16.0f, 20000.0f);
    s.hlodRange = Clamp(v.Get("hlodRange").AsFloat(8000.0f), 0.0f, 100000.0f);
    return s;
}

IVec2 WorldCellOf(Vec3 position, f32 cellSize)
{
    return IVec2(i32(std::floor(position.x / cellSize)), i32(std::floor(position.z / cellSize)));
}

String WorldCellFileName(IVec2 cell) { return String::Format("{}_{}.zcell", cell.x, cell.y); }

bool IsSpatialRoot(ecs::World& world, ecs::Entity e)
{
    const SpatiallyLoaded* s = world.Get<const SpatiallyLoaded>(e);
    return s && s->enabled && !world.Has<scene::Parent>(e);
}

WorldStreamer::~WorldStreamer() { Reset(); }

void WorldStreamer::WaitAll()
{
    for (auto& kv : m_cells)
        if (kv.second.state == CellState::Parsing)
            jobs::Wait(kv.second.job);
}

void WorldStreamer::Reset()
{
    WaitAll();
    for (auto& kv : m_cells)
        delete kv.second.doc;
    m_cells.Clear();
    m_groups.Clear();
    m_hlodMeshes.Clear();
    m_impostors.Clear();
    m_directory.Clear();
    m_settings = WorldPartitionSettings{};
    m_stats = Stats{};
}

String WorldStreamer::CellPath(IVec2 cell) const { return Path::Join(m_directory, WorldCellFileName(cell)); }

void WorldStreamer::ScanDirectory()
{
    Array<platform::DirectoryEntry> names;
    fs::ListOptions lo;
    lo.includeDirectories = false;
    lo.extension = ".zcell";
    fs::List(m_directory, names, lo);
    for (const platform::DirectoryEntry& n : names) {
        const StringView stem = Path::Stem(n.name);
        const usize us = stem.Find('_');
        if (us == StringView::NPos)
            continue;
        const Optional<i64> x = ParseInt(stem.Left(us)), z = ParseInt(stem.SubStr(us + 1));
        if (!x || !z)
            continue;
        const IVec2 c = IVec2(i32(*x), i32(*z));
        if (!m_cells.Find(Key(c))) {
            Cell cell;
            cell.coord = c;
            m_cells.InsertOrAssign(Key(c), move(cell));
        }
    }
    Array<platform::DirectoryEntry> hlods;
    lo.extension = ".zhlod";
    fs::List(m_directory, hlods, lo);
    for (const platform::DirectoryEntry& n : hlods) {
        StringView stem = Path::Stem(n.name);
        if (!stem.StartsWith("hlod_"))
            continue;
        stem = stem.SubStr(5);
        const usize us = stem.Find('_');
        if (us == StringView::NPos)
            continue;
        const Optional<i64> x = ParseInt(stem.Left(us)), z = ParseInt(stem.SubStr(us + 1));
        if (!x || !z)
            continue;
        const IVec2 g = IVec2(i32(*x), i32(*z));
        Group group;
        group.coord = g;
        group.onDisk = true;
        m_groups.InsertOrAssign(Key(g), move(group));
    }
    m_stats.cellsOnDisk = u32(m_cells.Size());
}

IVec2 WorldStreamer::GroupOf(IVec2 cell)
{
    auto div = [](i32 v) { return v >= 0 ? v / i32(HlodGroupCells) : -((-v + i32(HlodGroupCells) - 1) / i32(HlodGroupCells)); };
    return IVec2(div(cell.x), div(cell.y));
}

String WorldStreamer::HlodPath(IVec2 group) const { return Path::Join(m_directory, String::Format("hlod_{}_{}.zhlod", group.x, group.y)); }

namespace {
constexpr u32 HlodMagic = 0x444F4C48; // "HLOD"
constexpr f32 HlodSink = 3.0f;        // meters the stand-ins sit below the real ground (loaded tiles cover them)

/// Appends `src` (moved by `offset`) to `dst` (32-bit indices; sections and their LODs carried over).
void AppendMesh(asset::CookedMesh& dst, Array<u32>& indices, const asset::CookedMesh& src, Vec3 offset)
{
    const u32 base = u32(dst.vertices.Size());
    for (asset::PackedVertex v : src.vertices) {
        v.position += offset;
        dst.vertices.PushBack(v);
        dst.bounds.Expand(v.position);
    }
    const u32 indexBase = u32(indices.Size());
    for (u32 i = 0; i < src.IndexCount(); ++i)
        indices.PushBack(base + src.Index(i));
    const u32 lodBase = u32(dst.sectionLods.Size());
    for (asset::CookedSectionLod l : src.sectionLods) {
        l.indexOffset += indexBase;
        dst.sectionLods.PushBack(l);
    }
    for (asset::CookedMeshSection sec : src.sections) {
        sec.firstLod += lodBase;
        sec.bounds.min += offset;
        sec.bounds.max += offset;
        dst.sections.PushBack(sec);
    }
    dst.lodCount = Max(dst.lodCount, src.lodCount);
}
} // namespace

String WorldStreamer::EnsureImpostor(StringView model, UniquePtr<asset::AssetManager>& cooker)
{
    // Keyed by the model and its file's size and time: an edited model bakes again.
    const String source = Path::Join(m_io.assetRoot, model);
    platform::FileInfo info;
    if (!platform::GetFileInfo(source, info))
        return {};
    const u64 key = HashFnv1a64(String::Format("{}|{}|{}|v1", model, info.size, info.modifiedTime).View());
    const String file = String::Format("impostors/{}_{:016x}.zimp", Path::Stem(model), key);
    const String full = Path::Join(m_directory, file);
    if (platform::FileExists(full))
        return file;
    const f64 start = Clock::NowSeconds();
    ImpostorAsset imp;
    if (!BakeImpostorFromFile(source, imp, cooker))
        return {};
    Array<u8> bytes;
    SerializeImpostor(imp, bytes);
    platform::CreateDirectoryTree(Path::Join(m_directory, "impostors"));
    if (!fs::WriteFile(full, bytes.View()))
        return {};
    ZX_LOG_INFO(LogWorldPartition, "HLOD: impostor of {} baked ({} x {} views, {:.0f} ms)", Path::Stem(model), imp.frames, imp.frames,
                (Clock::NowSeconds() - start) * 1000.0);
    return file;
}

u32 WorldStreamer::BuildHlods(Span<const IVec2> only)
{
    UniquePtr<asset::AssetManager> cooker; // impostor bakes (only when a model has none yet)
    HashMap<String, String> impostorFiles;  // model -> file ("" = none)
    if (!IsActive())
        return 0;
    // The groups to (re)build.
    HashMap<u64, IVec2> groups;
    for (auto& kv : m_cells) {
        const IVec2 g = GroupOf(kv.second.coord);
        bool wanted = only.Empty();
        for (const IVec2& c : only)
            wanted |= GroupOf(c) == g;
        if (wanted)
            groups.InsertOrAssign(Key(g), g);
    }
    u32 written = 0;
    const f32 groupSize = m_settings.cellSize * f32(HlodGroupCells);
    for (auto& gk : groups) {
        const IVec2 g = gk.second;
        const Vec3 origin(f32(g.x) * groupSize, 0.0f, f32(g.y) * groupSize);
        asset::CookedMesh merged;
        merged.name = "HLOD";
        merged.materialSlotCount = 1;
        merged.slotSourceMaterial.PushBack(0);
        Array<u32> indices;
        Guid material;
        // Impostor plants: per model, per cell of the group.
        Array<String> plantModels, plantFiles;
        Array<Array<Array<f32>>> plantCells;
        for (i32 cz = 0; cz < i32(HlodGroupCells); ++cz)
            for (i32 cx = 0; cx < i32(HlodGroupCells); ++cx) {
                const IVec2 c(g.x * i32(HlodGroupCells) + cx, g.y * i32(HlodGroupCells) + cz);
                if (!m_cells.Find(Key(c)))
                    continue;
                String text;
                JsonValue doc;
                if (!fs::ReadTextFile(CellPath(c), text) || !ParseJson(text.View(), doc))
                    continue;
                // Coarse copies of the cell's terrains (the rest of a cell is small at HLOD distances).
                for (const JsonValue& o : doc.Get("entities").Elements()) {
                    const JsonValue& comps = o.Get("components");
                    const JsonValue& tj = comps.Get("Terrain");
                    if (!tj.IsObject() || !o.Get("parent").IsNull())
                        continue;
                    Terrain t;
                    Reflection::FromJson(tj, t);
                    Transform tr;
                    Reflection::FromJson(comps.Get("Transform"), tr);
                    const u32 samples = Clamp(u32(t.size / 16.0f) + 1u, 9u, 65u); // about 16 m between samples
                    AppendMesh(merged, indices, CookTerrainProxy(t, samples), tr.position - origin - Vec3(0.0f, HlodSink, 0.0f));
                    if (!material.IsValid())
                        material = t.material;
                }
                // Trees and rocks of the cell (roots with Foliage): their plants in group space.
                for (const JsonValue& o : doc.Get("entities").Elements()) {
                    const JsonValue& comps = o.Get("components");
                    const JsonValue& fj = comps.Get("Foliage");
                    if (!fj.IsObject() || !o.Get("parent").IsNull() || !m_io.assets)
                        continue;
                    Foliage f;
                    Reflection::FromJson(fj, f);
                    if (f.model.Empty() || !WantsImpostor(f))
                        continue;
                    String* file = impostorFiles.Find(f.model);
                    if (!file) {
                        impostorFiles.InsertOrAssign(f.model, EnsureImpostor(f.model, cooker));
                        file = impostorFiles.Find(f.model);
                    }
                    if (file->Empty())
                        continue;
                    DecodeFoliage(f);
                    Transform tr;
                    Reflection::FromJson(comps.Get("Transform"), tr);
                    const Mat4 em = tr.ToMatrix();
                    const Vec3 fwd = tr.rotation * Vec3(0.0f, 0.0f, 1.0f);
                    const f32 entityYaw = std::atan2(fwd.x, fwd.z);
                    usize set = 0;
                    while (set < plantModels.Size() && plantModels[set] != f.model)
                        ++set;
                    if (set == plantModels.Size()) {
                        plantModels.PushBack(f.model);
                        plantFiles.PushBack(*file);
                        Array<Array<f32>> cells;
                        cells.Resize(HlodGroupCells * HlodGroupCells);
                        plantCells.PushBack(move(cells));
                    }
                    Array<f32>& list = plantCells[set][usize(cz) * HlodGroupCells + usize(cx)];
                    for (const FoliageInstance& in : f.instances) {
                        const Vec3 p = em.TransformPointAffine(in.position) - origin;
                        for (f32 v : {p.x, p.y, p.z, in.yaw + entityYaw, in.scale * Abs(tr.scale.x)})
                            list.PushBack(v);
                    }
                }
            }
        if (merged.vertices.Empty() && plantModels.Empty()) {
            fs::Delete(HlodPath(g));
            m_groups.Remove(Key(g));
            continue;
        }
        merged.index16 = false;
        merged.indexData.ResizeUninitialized(indices.Size() * 4);
        MemCopy(merged.indexData.Data(), indices.Data(), indices.Size() * 4);
        Array<u8> meshBytes;
        asset::Serialize(merged, meshBytes);
        Array<u8> bytes;
        BinaryWriter w(bytes);
        w.Write<u32>(HlodMagic);
        w.WriteBytes(&origin, sizeof(Vec3));
        w.WriteBytes(&material, sizeof(Guid));
        w.WriteArray(Span<const u8>(meshBytes.Data(), meshBytes.Size()));
        w.Write<u32>(u32(plantModels.Size()));
        for (usize s = 0; s < plantModels.Size(); ++s) {
            w.WriteString(plantModels[s]);
            w.WriteString(plantFiles[s]);
            for (const Array<f32>& cell : plantCells[s])
                w.WriteArray(Span<const f32>(cell.Data(), cell.Size()));
        }
        if (fs::WriteFile(HlodPath(g), bytes.View())) {
            ++written;
            Group* existing = m_groups.Find(Key(g));
            if (!existing) {
                Group group;
                group.coord = g;
                m_groups.InsertOrAssign(Key(g), move(group));
                existing = m_groups.Find(Key(g));
            }
            existing->onDisk = true;
            existing->stale = true; // a shown copy is replaced
        }
    }
    return written;
}

void WorldStreamer::ShowHlod(ecs::World& world, Group& group)
{
    if (!group.entities.Empty() || !group.onDisk || !m_io.assets)
        return;
    Array<u8> bytes;
    if (!fs::ReadFile(HlodPath(group.coord), bytes))
        return;
    BinaryReader r(bytes.View());
    if (r.Read<u32>() != HlodMagic)
        return;
    Vec3 origin;
    Guid material;
    r.ReadBytes(&origin, sizeof(Vec3));
    r.ReadBytes(&material, sizeof(Guid));
    Array<u8> meshBytes;
    r.ReadArray(meshBytes);
    // Impostor plants (older files end here).
    group.plants.Clear();
    group.loadedMask = ~0u; // UpdateHlodPlants fills them in
    const u32 sets = r.Remaining() >= 4 ? r.Read<u32>() : 0u;
    for (u32 s = 0; s < sets && !r.HasError(); ++s) {
        const String model = r.ReadString();
        const String file = r.ReadString();
        Array<Array<f32>> cells;
        cells.Resize(HlodGroupCells * HlodGroupCells);
        for (Array<f32>& c : cells)
            r.ReadArray(c);
        LoadedImpostor* imp = m_impostors.Find(file);
        if (!imp) {
            ImpostorAsset asset;
            Array<u8> impBytes;
            LoadedImpostor li;
            if (fs::ReadFile(Path::Join(m_directory, file), impBytes) && DeserializeImpostor(impBytes.View(), asset)) {
                const ImpostorIds ids = RegisterImpostor(*m_io.assets, asset, String::Format("{}|{}", m_directory, file).View());
                li.ids = {ids.mesh, ids.material, ids.albedo, ids.normal};
                li.frames = asset.frames;
                li.centerY = asset.centerY;
            } else {
                ZX_LOG_WARN(LogWorldPartition, "HLOD: impostor {} is missing (rebuild the HLODs)", file);
            }
            m_impostors.InsertOrAssign(file, move(li));
            imp = m_impostors.Find(file);
        }
        if (imp->ids.Empty())
            continue;
        Foliage f;
        f.model = model;
        ImpostorIds ids{imp->ids[0], imp->ids[1], imp->ids[2], imp->ids[3]};
        ImpostorAsset meta;
        meta.frames = imp->frames;
        meta.centerY = imp->centerY;
        MakeImpostorFoliage(ids, meta, f);
        f.cullDistance = m_settings.hlodRange + m_settings.cellSize * f32(HlodGroupCells) * 1.5f;
        Transform ptr;
        ptr.position = origin;
        HlodPlants plants;
        plants.entity = scene::CreateNode(world, String::Format("HLOD {}_{} {}", group.coord.x, group.coord.y, Path::Stem(model)), ptr);
        world.Add(plants.entity, move(f));
        world.Add(plants.entity, HlodProxy{group.coord});
        plants.cells = move(cells);
        group.entities.PushBack(plants.entity);
        group.plants.PushBack(move(plants));
    }
    const Guid id = Guid::FromName(Guid(0x484C4F4450617274ull, 0x5A58334457507478ull),
                                   String::Format("{}|{}_{}|{}", m_directory, group.coord.x, group.coord.y, group.version).View());
    if (!m_hlodMeshes.Contains(id)) {
        asset::CookedMesh mesh;
        if (!asset::Deserialize(meshBytes.View(), mesh) || mesh.vertices.Empty())
            return; // plants only
        m_io.assets->AddMemoryMesh(id, move(mesh));
        m_hlodMeshes.Insert(id);
    }
    Transform tr;
    tr.position = origin;
    const ecs::Entity e = scene::CreateNode(world, String::Format("HLOD {}_{}", group.coord.x, group.coord.y), tr);
    MeshRenderer mr;
    mr.mesh = id;
    if (material.IsValid())
        mr.materials.PushBack(material);
    mr.collision = false;
    world.Add(e, move(mr));
    world.Add(e, HlodProxy{group.coord});
    group.entities.PushBack(e);
}

void WorldStreamer::UpdateHlodPlants(ecs::World& world, Group& group, u32 loadedMask)
{
    if (group.loadedMask == loadedMask)
        return;
    group.loadedMask = loadedMask;
    for (HlodPlants& p : group.plants) {
        Foliage* f = world.IsAlive(p.entity) ? world.Get<Foliage>(p.entity) : nullptr;
        if (!f)
            continue;
        Array<FoliageInstance> instances;
        for (usize c = 0; c < p.cells.Size(); ++c) {
            if (loadedMask & (1u << c))
                continue; // the cell's real plants stand there
            const Array<f32>& v = p.cells[c];
            for (usize i = 0; i + 4 < v.Size(); i += 5)
                instances.PushBack(FoliageInstance{Vec3(v[i], v[i + 1], v[i + 2]), v[i + 3], v[i + 4]});
        }
        EncodeFoliage(*f, instances.View());
    }
}

void WorldStreamer::HideHlod(ecs::World& world, Group& group)
{
    for (ecs::Entity e : group.entities)
        if (world.IsAlive(e))
            world.Destroy(e);
    group.entities.Clear();
    group.plants.Clear();
}

void WorldStreamer::Configure(StringView cellDirectory, const WorldPartitionSettings& settings, const SceneIOOptions& io)
{
    Reset();
    m_settings = settings;
    if (!settings.enabled)
        return;
    m_directory = cellDirectory;
    m_io = io;
    ScanDirectory();
    ZX_LOG_INFO(LogWorldPartition, "World Partition: {} cells of {:.0f} m, load range {:.0f} m", m_stats.cellsOnDisk, settings.cellSize,
                settings.loadRange);
}

void WorldStreamer::SetDirectory(StringView cellDirectory, const WorldPartitionSettings& settings, const SceneIOOptions& io)
{
    if (!IsActive()) {
        m_settings = settings;
        m_directory = cellDirectory;
        m_io = io;
        return;
    }
    m_directory = cellDirectory;
    m_settings = settings;
    m_io = io;
}

void WorldStreamer::LoadAll(ecs::World& world)
{
    if (!IsActive())
        return;
    for (auto& kv : m_cells)
        if (kv.second.state == CellState::Unloaded)
            StartLoad(kv.second);
    WaitAll();
    for (auto& kv : m_cells) {
        Cell& cell = kv.second;
        if (cell.state == CellState::Parsing)
            cell.state = cell.doc && cell.doc->IsObject() ? CellState::Creating : CellState::Loaded;
        while (cell.state == CellState::Creating && CreateNext(world, cell)) {
        }
        cell.state = CellState::Loaded;
        delete cell.doc;
        cell.doc = nullptr;
        cell.rootGroups.Clear();
        cell.nextGroup = 0;
    }
}

bool WorldStreamer::IsCellLoaded(IVec2 c) const
{
    const Cell* cell = m_cells.Find(Key(c));
    return cell && cell->state == CellState::Loaded;
}

void WorldStreamer::StartLoad(Cell& cell)
{
    cell.state = CellState::Parsing;
    cell.doc = new JsonValue();
    JsonValue* doc = cell.doc;
    String path = CellPath(cell.coord);
    auto work = [doc, path = move(path)]() {
        String text;
        if (!fs::ReadTextFile(path, text) || !ParseJson(text.View(), *doc))
            *doc = JsonValue();
    };
    if (jobs::IsInitialized())
        cell.job = jobs::Schedule(move(work));
    else
        work();
}

bool WorldStreamer::CreateNext(ecs::World& world, Cell& cell)
{
    // One root subtree (the root and the entities after it until the next root) per call.
    if (cell.rootGroups.Empty() && cell.nextGroup == 0) {
        const JsonValue& entities = cell.doc->Get("entities");
        u32 index = 0;
        for (const JsonValue& o : entities.Elements()) {
            if (o.Get("parent").IsNull() || cell.rootGroups.Empty())
                cell.rootGroups.EmplaceBack();
            cell.rootGroups.Back().PushBack(index++);
        }
    }
    if (cell.nextGroup >= cell.rootGroups.Size())
        return false;
    const Array<u32>& group = cell.rootGroups[cell.nextGroup++];
    const Span<const JsonValue> all = static_cast<const JsonValue*>(cell.doc)->Get("entities").Elements();
    JsonValue sub = JsonValue::MakeObject();
    sub.Set("format", "zscene");
    sub.Set("version", SceneFormatVersion);
    if (cell.nextGroup == 1)
        sub.Set("assets", cell.doc->Get("assets")); // the cell's sources, once
    JsonValue list = JsonValue::MakeArray();
    for (u32 i : group)
        list.PushBack(all[i]);
    sub.Set("entities", move(list));
    LoadScene(sub, world, m_io);
    if (!group.Empty())
        cell.roots.PushBack(GuidOf(all[group[0]].Get("guid")));
    return cell.nextGroup < cell.rootGroups.Size();
}

void WorldStreamer::Unload(ecs::World& world, Cell& cell)
{
    if (cell.state == CellState::Parsing)
        jobs::Wait(cell.job);
    if (!cell.roots.Empty()) {
        HashMap<Guid, ecs::Entity> map;
        BuildGuidMap(world, map);
        for (const Guid& g : cell.roots)
            if (const ecs::Entity* e = map.Find(g)) {
                Array<ecs::Entity> tree;
                CollectHierarchy(world, *e, tree);
                for (usize i = tree.Size(); i-- > 0;)
                    world.Destroy(tree[i]);
            }
    }
    delete cell.doc;
    cell.doc = nullptr;
    cell.roots.Clear();
    cell.rootGroups.Clear();
    cell.nextGroup = 0;
    cell.state = CellState::Unloaded;
}

void WorldStreamer::Update(ecs::World& world, Span<const Vec3> sources, bool allowUnload, f64 budgetMs)
{
    if (!IsActive())
        return;
    const f64 start = Clock::NowSeconds();
    const f32 cs = m_settings.cellSize;
    auto distanceTo = [&](IVec2 c) {
        // Distance from the closest source to the cell's rectangle (in xz).
        f32 best = Infinity;
        for (const Vec3& s : sources) {
            const f32 x0 = f32(c.x) * cs, z0 = f32(c.y) * cs;
            const f32 dx = Max(Max(x0 - s.x, s.x - (x0 + cs)), 0.0f), dz = Max(Max(z0 - s.z, s.z - (z0 + cs)), 0.0f);
            best = Min(best, Sqrt(dx * dx + dz * dz));
        }
        return best;
    };
    // Decide: load the nearest wanted cells first.
    Array<Pair<f32, u64>> wanted;
    u32 loaded = 0, loading = 0;
    for (auto& kv : m_cells) {
        Cell& cell = kv.second;
        const f32 d = distanceTo(cell.coord);
        if (cell.state == CellState::Unloaded && d <= m_settings.loadRange)
            wanted.PushBack(Pair<f32, u64>{d, kv.first});
        else if (cell.state != CellState::Unloaded && allowUnload && d > m_settings.loadRange * 1.25f)
            Unload(world, cell);
    }
    Sort(wanted, [](const Pair<f32, u64>& a, const Pair<f32, u64>& b) { return a.first < b.first; });
    for (const Pair<f32, u64>& w : wanted)
        if (Cell* cell = m_cells.Find(w.second))
            StartLoad(*cell);
    // Advance: parsed docs become entities within the budget.
    for (auto& kv : m_cells) {
        Cell& cell = kv.second;
        if (cell.state == CellState::Parsing && (!jobs::IsInitialized() || jobs::IsCompleted(cell.job)))
            cell.state = cell.doc && cell.doc->IsObject() ? CellState::Creating : CellState::Loaded;
    }
    for (auto& kv : m_cells) {
        Cell& cell = kv.second;
        while (cell.state == CellState::Creating && (Clock::NowSeconds() - start) * 1000.0 < budgetMs) {
            if (!CreateNext(world, cell)) {
                cell.state = CellState::Loaded;
                cell.loadedTime = Clock::NowSeconds();
                delete cell.doc;
                cell.doc = nullptr;
                cell.rootGroups.Clear();
            }
        }
    }
    // HLODs: a group of cells shows its stand-in while any of its cells is not (yet) loaded and settled (a loaded cell
    // keeps it a moment longer: its terrain is still being built). It sits a little lower than the real ground, which
    // covers it where cells are loaded.
    u32 hlods = 0;
    const f64 now = Clock::NowSeconds();
    HashMap<u64, bool> groupComplete; // every cell of the group loaded and settled
    HashMap<u64, u32> groupSettled;   // bit per settled cell (their real plants replace the impostors)
    for (auto& kv : m_cells) {
        const Cell& cell = kv.second;
        const bool settled = cell.state == CellState::Loaded && now - cell.loadedTime > 1.5;
        const u64 gk = Key(GroupOf(cell.coord));
        if (settled) {
            const IVec2 g = GroupOf(cell.coord);
            const u32 bit = 1u << u32((cell.coord.y - g.y * i32(HlodGroupCells)) * i32(HlodGroupCells) + (cell.coord.x - g.x * i32(HlodGroupCells)));
            if (u32* m = groupSettled.Find(gk))
                *m |= bit;
            else
                groupSettled.InsertOrAssign(gk, bit);
        }
        bool* complete = groupComplete.Find(gk);
        if (!complete)
            groupComplete.InsertOrAssign(gk, settled);
        else
            *complete = *complete && settled;
    }
    const f32 groupSize = cs * f32(HlodGroupCells);
    for (auto& kv : m_groups) {
        Group& group = kv.second;
        const f32 x0 = f32(group.coord.x) * groupSize, z0 = f32(group.coord.y) * groupSize;
        f32 d = Infinity;
        for (const Vec3& src : sources) {
            const f32 dx = Max(Max(x0 - src.x, src.x - (x0 + groupSize)), 0.0f), dz = Max(Max(z0 - src.z, src.z - (z0 + groupSize)), 0.0f);
            d = Min(d, Sqrt(dx * dx + dz * dz));
        }
        const bool* complete = groupComplete.Find(kv.first);
        const bool want = m_settings.hlodRange > 0.0f && d <= m_settings.hlodRange && !(complete && *complete);
        if (group.stale && !group.entities.Empty()) {
            HideHlod(world, group);
            ++group.version;
        }
        group.stale = false;
        if (want && group.entities.Empty())
            ShowHlod(world, group);
        else if (!want && !group.entities.Empty())
            HideHlod(world, group);
        if (!group.plants.Empty()) {
            const u32* settledMask = groupSettled.Find(kv.first);
            UpdateHlodPlants(world, group, settledMask ? *settledMask : 0u);
        }
        hlods += group.entities.Empty() ? 0u : 1u;
    }
    m_stats.hlods = hlods;
    for (auto& kv : m_cells) {
        loaded += kv.second.state == CellState::Loaded ? 1u : 0u;
        loading += kv.second.state == CellState::Parsing || kv.second.state == CellState::Creating ? 1u : 0u;
    }
    m_stats.loaded = loaded;
    m_stats.loading = loading;
    m_stats.lastWorkMs = (Clock::NowSeconds() - start) * 1000.0;
    m_stats.maxWorkMs = Max(m_stats.maxWorkMs, m_stats.lastWorkMs);
}

void WorldStreamer::LoadAllInRange(ecs::World& world, Span<const Vec3> sources)
{
    for (u32 guard = 0; guard < 100000; ++guard) {
        Update(world, sources, false, 1e9);
        if (m_stats.loading == 0)
            break;
        WaitAll();
    }
}

bool WorldStreamer::SaveCells(ecs::World& world)
{
    if (!IsActive())
        return true;
    // Cells half way through loading: finish them (their entities are partly in the world).
    for (auto& kv : m_cells)
        if (kv.second.state == CellState::Parsing || kv.second.state == CellState::Creating) {
            if (kv.second.state == CellState::Parsing && jobs::IsInitialized())
                jobs::Wait(kv.second.job);
            kv.second.state = kv.second.doc && kv.second.doc->IsObject() ? CellState::Creating : CellState::Loaded;
            while (kv.second.state == CellState::Creating && CreateNext(world, kv.second)) {
            }
            kv.second.state = CellState::Loaded;
            delete kv.second.doc;
            kv.second.doc = nullptr;
            kv.second.rootGroups.Clear();
            kv.second.nextGroup = 0;
        }
    auto cellOf = [&](ecs::Entity r) {
        const scene::WorldTransform* wt = world.Get<const scene::WorldTransform>(r);
        return WorldCellOf(wt ? wt->matrix.GetTranslation() : Vec3(), m_settings.cellSize);
    };
    // Cells on disk that are not loaded but now hold entities of the world: load them fully first, so writing them
    // keeps what they had.
    Array<ecs::Entity> roots;
    CollectRoots(world, roots);
    for (ecs::Entity r : roots) {
        if (!IsSpatialRoot(world, r))
            continue;
        Cell* cell = m_cells.Find(Key(cellOf(r)));
        if (cell && cell->state == CellState::Unloaded) {
            StartLoad(*cell);
            if (jobs::IsInitialized())
                jobs::Wait(cell->job);
            cell->state = cell->doc && cell->doc->IsObject() ? CellState::Creating : CellState::Loaded;
            while (cell->state == CellState::Creating && CreateNext(world, *cell)) {
            }
            cell->state = CellState::Loaded;
            delete cell->doc;
            cell->doc = nullptr;
            cell->rootGroups.Clear();
            cell->nextGroup = 0;
        }
    }
    // Group the spatial roots by cell.
    roots.Clear();
    CollectRoots(world, roots);
    HashMap<u64, Array<ecs::Entity>> byCell;
    for (ecs::Entity r : roots) {
        if (!IsSpatialRoot(world, r))
            continue;
        const u64 key = Key(cellOf(r));
        if (!byCell.Find(key))
            byCell.InsertOrAssign(key, Array<ecs::Entity>{});
        byCell.Find(key)->PushBack(r);
    }
    platform::CreateDirectoryTree(fs::ToPhysicalPath(m_directory));
    bool ok = true;
    for (auto& kv : byCell) {
        const IVec2 c(i32(u32(kv.first >> 32)), i32(u32(kv.first)));
        HashSet<u64> members;
        for (ecs::Entity e : kv.second)
            members.Insert(e.Pack());
        SceneIOOptions io = m_io;
        io.rootFilter = [&members](ecs::Entity e) { return members.Contains(e.Pack()); };
        const JsonValue doc = SaveScene(world, io);
        ok &= fs::WriteTextFile(CellPath(c), ToJsonString(doc));
        Cell* cell = m_cells.Find(kv.first);
        if (!cell) {
            Cell fresh;
            fresh.coord = c;
            m_cells.InsertOrAssign(kv.first, move(fresh));
            cell = m_cells.Find(kv.first);
        }
        cell->state = CellState::Loaded; // its entities are in the world
        cell->roots.Clear();
        for (ecs::Entity e : kv.second)
            cell->roots.PushBack(GetEntityGuid(world, e));
    }
    // Loaded cells whose entities are all gone (deleted or moved out): remove their files.
    for (auto& kv : m_cells)
        if (kv.second.state == CellState::Loaded && !byCell.Find(kv.first)) {
            fs::Delete(CellPath(kv.second.coord));
            kv.second.roots.Clear();
        }
    m_stats.cellsOnDisk = 0;
    for (auto& kv : m_cells)
        m_stats.cellsOnDisk += fs::Exists(CellPath(kv.second.coord)) ? 1u : 0u;
    return ok;
}

} // namespace zx
