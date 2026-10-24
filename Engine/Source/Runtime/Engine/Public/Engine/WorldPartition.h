#pragma once

// World Partition (M18): big worlds split into a grid of cells that stream in and out around the camera / player.
//
// A scene with World Partition enabled (its settings "worldPartition") keeps its Spatially Loaded root entities (and
// their children) out of the .zscene: they live in cell files next to it, <scene>_Cells/<x>_<z>.zcell (the scene
// format, one file per cell, by the root's world position). Everything else (sun, sky, time of day, game logic) is
// always loaded.
//
//   WorldStreamer streamer;
//   streamer.Configure(cellDirectory, settings, sceneIO);   // after loading the scene
//   streamer.Update(world, sources, allowUnload, budgetMs);  // every frame
//   streamer.SaveCells(world);                               // when the scene is saved
//   streamer.BuildHlods();                                   // far-away stand-ins of every cell
//
// HLOD: each group of 4 x 4 cells gets a hlod_<x>_<z>.zhlod with coarse copies of its terrains (one mesh, a little below
// the real ground); groups within hlodRange that are not completely loaded show it, so the horizon is never empty.
//
// Loading never stalls a frame for long: files are read and parsed by jobs, and the entities of a cell are created
// one root at a time within a per-frame time budget. Cells unload past loadRange x 1.25 (hysteresis).

#include "Core/Containers/Array.h"
#include "Core/Containers/HashMap.h"
#include "Core/Jobs/JobSystem.h"
#include "Core/Reflection/Reflection.h"
#include "Core/Serialization/Json.h"
#include "Engine/SceneSerialization.h"
#include "Scene/ECS/World.h"

namespace zx {

/// Marker: this root entity (with its children) streams with the World Partition cell it stands in.
struct SpatiallyLoaded
{
    bool enabled = true;
};
ZX_DECLARE_TYPE(SpatiallyLoaded);

struct WorldPartitionSettings
{
    bool enabled = false;
    f32 cellSize = 256.0f;   // meters
    f32 loadRange = 640.0f;  // meters around each streaming source
    f32 hlodRange = 8000.0f; // meters: unloaded cells closer than this show their HLOD
};

/// Marks the stand-in entities World Partition creates for HLODs (never saved).
struct HlodProxy
{
    IVec2 cell;
};
JsonValue WorldPartitionToJson(const WorldPartitionSettings& s);
WorldPartitionSettings WorldPartitionFromJson(const JsonValue& v);

/// Cell coordinates of a world position.
IVec2 WorldCellOf(Vec3 position, f32 cellSize);
/// "<x>_<z>.zcell"
String WorldCellFileName(IVec2 cell);
/// True for root entities that stream with their cell (Spatially Loaded, enabled).
bool IsSpatialRoot(ecs::World& world, ecs::Entity e);

class WorldStreamer
{
public:
    ~WorldStreamer();

    /// Starts streaming the cells in `cellDirectory` (physical or virtual) with `settings`; disabled settings stop it.
    void Configure(StringView cellDirectory, const WorldPartitionSettings& settings, const SceneIOOptions& io);
    /// Forgets every cell (scene closed). Does not touch the world.
    void Reset();
    /// Points an active streamer at another directory (Save As) keeping what is loaded; starts it if inactive.
    void SetDirectory(StringView cellDirectory, const WorldPartitionSettings& settings, const SceneIOOptions& io);
    /// Loads every cell (synchronously).
    void LoadAll(ecs::World& world);
    bool IsActive() const { return m_settings.enabled && !m_directory.Empty(); }
    const WorldPartitionSettings& Settings() const { return m_settings; }

    /// Loads the cells within loadRange of any source, unloads far ones (when allowed), creating entities for at most
    /// budgetMs per frame.
    void Update(ecs::World& world, Span<const Vec3> sources, bool allowUnload, f64 budgetMs);
    /// Loads every cell within range right away (startup, tests).
    void LoadAllInRange(ecs::World& world, Span<const Vec3> sources);
    /// Writes the spatial roots of the world into their cell files. Cells that are not loaded keep what is on disk (new
    /// entities in them are merged in); loaded cells left empty are deleted.
    bool SaveCells(ecs::World& world);
    /// Writes the HLOD of every cell on disk (or only `cells`) from its file.
    u32 BuildHlods(Span<const IVec2> cells = {});

    struct Stats
    {
        u32 cellsOnDisk = 0;
        u32 loaded = 0;
        u32 loading = 0;      // reading / parsing / creating entities
        u32 hlods = 0;        // groups shown as HLOD
        f64 lastWorkMs = 0.0; // main-thread time spent this frame
        f64 maxWorkMs = 0.0;  // since Configure
    };
    const Stats& GetStats() const { return m_stats; }
    bool IsCellLoaded(IVec2 cell) const;

private:
    enum class CellState : u8
    {
        Unloaded,
        Parsing,
        Creating,
        Loaded,
    };
    struct Cell
    {
        IVec2 coord;
        CellState state = CellState::Unloaded;
        JobHandle job;
        JsonValue* doc = nullptr; // parsed by the job
        Array<Array<u32>> rootGroups; // entity indices of each root subtree in doc order
        usize nextGroup = 0;
        Array<Guid> roots; // created roots (to unload them)
        f64 loadedTime = 0.0;  // when it finished loading (the HLOD stays a moment longer)
    };
    /// HLODs cover groups of HlodGroupCells x HlodGroupCells cells: one mesh per group (few draws for a whole horizon).
    static constexpr u32 HlodGroupCells = 4;
    struct Group
    {
        IVec2 coord;
        bool onDisk = false;
        bool stale = false; // rebuilt: a shown copy is replaced
        u32 version = 0;
        Array<ecs::Entity> entities;
    };
    static IVec2 GroupOf(IVec2 cell);
    static u64 Key(IVec2 c) { return (u64(u32(c.x)) << 32) | u32(c.y); }
    String CellPath(IVec2 cell) const;
    void ScanDirectory();
    void StartLoad(Cell& cell);
    void Unload(ecs::World& world, Cell& cell);
    bool CreateNext(ecs::World& world, Cell& cell);
    void WaitAll();
    void ShowHlod(ecs::World& world, Group& group);
    void HideHlod(ecs::World& world, Group& group);
    String HlodPath(IVec2 group) const;

    WorldPartitionSettings m_settings;
    String m_directory;
    SceneIOOptions m_io;
    HashMap<u64, Cell> m_cells;
    HashMap<u64, Group> m_groups;
    HashSet<Guid> m_hlodMeshes; // registered with the asset manager
    Stats m_stats;
};

} // namespace zx
