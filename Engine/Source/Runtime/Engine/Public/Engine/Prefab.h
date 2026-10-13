#pragma once

// Prefabs (.zprefab): a reusable entity hierarchy stored as an asset. Scenes (and other prefabs) contain instances that
// keep only their overrides; edits to the prefab propagate to every instance.
//
// File: same entity objects as .zscene ({ "format": "zprefab", "version": 1, "assets": [...], "entities": [...] }); the
// first entity is the prefab root, guids are prefab-local ids.
//
// Instances in a world:
//   - the instance root carries PrefabInstance { prefab path }, its guid G identifies the instance;
//   - every other entity that comes from the prefab carries PrefabLink { instance G, source <prefab-local id> } and
//     the deterministic guid Guid::FromName(G, source), so reloading reproduces the same identities.
// Nested prefabs: an instance root inside a prefab is itself a member of the outer prefab; its own members link to it.
//
// Scene files store instances compressed: member components are deltas against the (recursively expanded) prefab,
// "removedComponents" lists components the instance dropped, and the root's "removedFromPrefab" lists prefab entities
// deleted in the instance. ExpandPrefabInstances turns that back into full entity states.

#include "Engine/SceneSerialization.h"

namespace zx {

struct PrefabInstance
{
    String prefab; // path relative to the content root ('/' separators)
};

struct PrefabLink
{
    Guid instance; // guid of the instance root
    Guid source;   // prefab-local id of the entity
};

ZX_DECLARE_TYPE(PrefabInstance);
ZX_DECLARE_TYPE(PrefabLink);

inline constexpr i64 PrefabFormatVersion = 1;
inline constexpr StringView PrefabExtension = ".zprefab";

/// Loaded prefab files, by content-relative path. Loading a prefab also requests its asset sources.
class PrefabLibrary
{
public:
    struct Prefab
    {
        String path;
        Array<JsonValue> entities; // file order (root first)
        Guid root;
    };

    PrefabLibrary(asset::AssetManager* assets, StringView contentRoot) : m_assets(assets), m_contentRoot(contentRoot) {}

    /// Null if the file is missing or invalid (logged once per path until invalidated).
    const Prefab* Get(StringView relativePath);
    void Invalidate(StringView relativePath);
    void Clear() { m_prefabs.Clear(); }
    StringView ContentRoot() const { return m_contentRoot; }
    String PhysicalPath(StringView relativePath) const;
    String RelativePath(StringView physicalPath) const;

private:
    asset::AssetManager* m_assets;
    String m_contentRoot;
    HashMap<String, UniquePtr<Prefab>> m_prefabs;
    HashMap<String, bool> m_failed;
};

/// Guid of a prefab member in an instance.
Guid PrefabMemberGuid(const Guid& instance, const Guid& source);

/// Replaces every compressed instance in `entries` by full entity states (recursively for nested prefabs). Entries keep
/// their order; generated members are appended. Unknown prefabs are reported and left as plain entities.
void ExpandPrefabInstances(Array<EntitySnapshot>& entries, PrefabLibrary& library, Array<String>* warnings = nullptr);

/// Inverse of ExpandPrefabInstances for serialized world entities (SerializeEntity output, parents before children):
/// member components become deltas against the prefab, missing members are listed on the root.
void CompressPrefabInstances(Array<JsonValue>& entities, PrefabLibrary& library);

/// Writes the subtree of `root` as a prefab file and turns the subtree into an instance of it (same root guid).
/// Returns false on I/O errors. `relativePath` is relative to the library's content root.
bool CreatePrefabFromEntity(ecs::World& world, ecs::Entity root, StringView relativePath, PrefabLibrary& library,
                            asset::AssetManager* assets);

/// Creates a new instance of a prefab. Returns its root (NullEntity if the prefab can't be loaded).
ecs::Entity InstantiatePrefabFile(ecs::World& world, PrefabLibrary& library, StringView relativePath, const Transform& transform,
                                  ecs::Entity parent = {});

/// Rebuilds the instance rooted at `root` from its prefab, keeping its overrides (or dropping them if `revert`).
/// Entity guids are preserved. Returns the new root.
ecs::Entity RefreshPrefabInstance(ecs::World& world, ecs::Entity root, PrefabLibrary& library, bool revert = false);

/// Writes the instance's current state into its prefab file (members keep their ids, new entities join the prefab)
/// and refreshes every instance of that prefab in `world`. Returns false if `root` is not an instance root.
bool ApplyPrefabInstance(ecs::World& world, ecs::Entity root, PrefabLibrary& library, asset::AssetManager* assets);

/// Instance root an entity belongs to (itself if it is a root), or NullEntity.
ecs::Entity FindPrefabInstanceRoot(ecs::World& world, ecs::Entity e);
/// Names of top-level fields of each component that differ from the prefab ("Transform.position", ...).
void GetPrefabOverrides(ecs::World& world, ecs::Entity e, PrefabLibrary& library, Array<String>& out);

} // namespace zx
