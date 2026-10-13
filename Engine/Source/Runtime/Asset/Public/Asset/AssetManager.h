#pragma once

// Asset manager v1: GUIDs in .zmeta files, cooking into a derived data cache, asynchronous loading on the job system.
//
//   AssetManager assets({.ddcDirectory = root + "/Intermediate/DDC"});
//   AssetId sponza = assets.LoadPath("C:/.../Sponza.gltf");     // returns at once, cooks/loads in the background
//   ...each frame...
//   if (const PrefabAsset* p = assets.GetPrefab(sponza)) { ...instantiate... }
//   assets.Load(meshId); const CookedMesh* m = assets.GetMesh(meshId);
//
// Sources: .gltf/.glb/.obj (main asset = Prefab; sub-assets: meshes, materials, textures), .png/.jpg/.tga/.bmp/.hdr/.exr
// (main asset = Texture). A source's .zmeta holds its GUID; sub-asset ids are derived: Guid::FromName(source, "mesh/3").
// DDC keys hash the source bytes, the cook settings and CookerVersion, so edits and cooker changes re-cook.

#include "Asset/CookedAssets.h"
#include "Core/Containers/HashMap.h"
#include "Core/Memory/Pointers.h"
#include "Core/Threading/Mutex.h"

#include <atomic>

namespace zx::asset {

enum class AssetState : u8
{
    Unknown, // never requested / not registered
    Queued,  // requested, waiting for its source to be cooked
    Loading,
    Loaded,
    Failed,
};

struct AssetManagerSettings
{
    /// Physical directory for cooked data (created on demand).
    String ddcDirectory;
    /// Texture dimension cap (0 = none). The Lite engine profile uses 1024.
    u32 textureMaxSize = 0;
    bool generateMeshLods = true;
    /// Write .zmeta files next to sources that don't have one (off = derive GUIDs from the path).
    bool writeMetaFiles = true;
};

class AssetManager
{
public:
    explicit AssetManager(const AssetManagerSettings& settings);
    ~AssetManager();
    ZX_NON_COPYABLE(AssetManager)

    /// Reads (or creates) the source's .zmeta and returns its main asset id. Cheap; does not load.
    Result<AssetId> RegisterSource(StringView path);
    /// Requests an asynchronous load. Sub-asset ids may be requested before their source finished cooking.
    void Load(AssetId id);
    /// RegisterSource + Load. Returns an invalid id if the source is unknown/unsupported (logged).
    AssetId LoadPath(StringView path);

    AssetState GetState(AssetId id) const;
    AssetType GetType(AssetId id) const;
    String GetErrorMessage(AssetId id) const;
    /// Physical path of the source file that produced the asset (main or known sub-asset); empty if unknown.
    String GetSourcePath(AssetId id) const;
    /// Loaded data or nullptr. Pointers stay valid until ReleaseCpuData/Unload.
    const CookedMesh* GetMesh(AssetId id) const;
    const CookedTexture* GetTexture(AssetId id) const;
    const MaterialAsset* GetMaterial(AssetId id) const;
    const PrefabAsset* GetPrefab(AssetId id) const;
    /// Registers a ready mesh (built-in shapes, procedural geometry) as a Loaded asset. Kept for the manager's lifetime.
    void AddMemoryMesh(AssetId id, CookedMesh mesh);
    /// Frees the CPU copy (e.g. after GPU upload); the state stays Loaded.
    void ReleaseCpuData(AssetId id);

    /// Blocks until no load or cook is in flight.
    void WaitIdle();
    bool IsIdle() const { return m_inFlight.load(std::memory_order_acquire) == 0; }

    struct Stats
    {
        u32 assets = 0;
        u32 loaded = 0;
        u32 failed = 0;
        u32 inFlight = 0;
        u32 cookedThisSession = 0; // DDC misses
        u32 ddcHits = 0;
    };
    Stats GetStats() const;

private:
    struct Entry;
    struct Source;

    Entry* FindEntry(AssetId id) const;
    Entry& GetOrAddEntry(AssetId id);
    void StartEntryLoad(Entry& e);
    void StartSourceCook(Source& s);
    void CookSource(Source& s);
    void LoadEntry(Entry& e);
    void Fail(Entry& e, StringView message);
    String DdcPath(u64 key, StringView suffix) const;

    AssetManagerSettings m_settings;
    mutable Mutex m_mutex;
    HashMap<AssetId, UniquePtr<Entry>> m_entries;
    HashMap<AssetId, UniquePtr<Source>> m_sources;
    std::atomic<u32> m_inFlight{0};
    std::atomic<u32> m_cooked{0};
    std::atomic<u32> m_ddcHits{0};
};

} // namespace zx::asset
