#pragma once

// Scene files (.zscene) and entity snapshots, both built on reflection and the ComponentRegistry.
//
//   JsonValue doc = SaveScene(world, {.assets = &assets, .assetRoot = contentDir});
//   LoadScene(doc, world, {.assets = &assets, .assetRoot = contentDir});   // appends to the world
//
// Format (version 1):
//   { "format": "zscene", "version": 1,
//     "assets":   ["Sponza/Sponza.gltf", ...],                // sources to load (relative to assetRoot or absolute)
//     "settings": { ... },                                     // free-form, owned by the caller (environment etc.)
//     "entities": [ { "guid": "...", "parent": "...", "components": { "Transform": {...}, "PointLight": {...} } } ] }
// Entities are written depth-first (parents before children, children in order), so loading in file order rebuilds
// the hierarchy exactly. Components are registered types only (GetComponentTypes), written as deltas from defaults.
//
// Snapshots (undo/redo, copy/paste) use the same entity object plus "index" (position among the parent's children).
// ApplyEntitySnapshots makes the world match a set of snapshots: missing entities are created (with their guid),
// existing ones get their components replaced, and a null snapshot destroys the entity.

#include "Asset/AssetManager.h"
#include "Core/Containers/Function.h"
#include "Core/Hash/Guid.h"
#include "Core/Serialization/Json.h"
#include "Scene/Hierarchy.h"

namespace zx {

/// Persistent identity of an entity (saved in scenes; survives undo/redo and reloads).
struct EntityGuid
{
    Guid value;
};
ZX_DECLARE_TYPE(EntityGuid);

inline constexpr i64 SceneFormatVersion = 1;

/// Returns the entity's guid, adding a new random one if it has none.
Guid EnsureEntityGuid(ecs::World& world, ecs::Entity e);
Guid GetEntityGuid(const ecs::World& world, ecs::Entity e);
/// Linear scan; use BuildGuidMap for many lookups.
ecs::Entity FindEntityByGuid(ecs::World& world, const Guid& guid);
void BuildGuidMap(ecs::World& world, HashMap<Guid, ecs::Entity>& out);

/// Serializes the entity's registered components (deltas from defaults), guid, parent guid and sibling index.
/// Assigns guids to the entity and its parent if they have none.
JsonValue SerializeEntity(ecs::World& world, ecs::Entity e);

struct EntitySnapshot
{
    Guid guid;
    JsonValue state; // SerializeEntity output, or null = the entity does not exist
};

/// Makes the world match the snapshots (see the header comment). Returns the entities that exist afterwards, in
/// snapshot order (NullEntity for destroyed ones). Warnings (unknown components/fields) are appended if requested.
Array<ecs::Entity> ApplyEntitySnapshots(ecs::World& world, Span<const EntitySnapshot> snapshots, Array<String>* warnings = nullptr);

/// The entity and all its descendants, depth-first (parents first).
void CollectHierarchy(const ecs::World& world, ecs::Entity root, Array<ecs::Entity>& out);
/// Scene root entities (have a Transform, no Parent), in a stable order (creation order of the archetype rows).
void CollectRoots(ecs::World& world, Array<ecs::Entity>& out);

class PrefabLibrary;

struct SceneIOOptions
{
    asset::AssetManager* assets = nullptr; // save: resolves source paths; load: loads them
    String assetRoot;                      // physical directory asset paths are relative to
    PrefabLibrary* prefabs = nullptr;      // save prefab instances as overrides / expand them on load (Prefab.h)
    /// Save: only these root entities (with their children); null = all.
    Function<bool(ecs::Entity), 32> rootFilter;
};

/// Serializes every scene entity (anything with a Transform). `settings` is stored as-is.
JsonValue SaveScene(ecs::World& world, const SceneIOOptions& options, const JsonValue& settings = {});
/// Appends the scene's entities to the world (new guids are not generated; existing equal guids are reused and
/// overwritten). Returns false on a format error. `settingsOut` receives the "settings" object.
bool LoadScene(const JsonValue& doc, ecs::World& world, const SceneIOOptions& options, JsonValue* settingsOut = nullptr,
               Array<String>* warnings = nullptr);

/// File helpers (virtual or physical paths, see FileSystem).
bool SaveSceneFile(StringView path, ecs::World& world, const SceneIOOptions& options, const JsonValue& settings = {});
bool LoadSceneFile(StringView path, ecs::World& world, const SceneIOOptions& options, JsonValue* settingsOut = nullptr);

/// Destroys every entity that has a Transform (singletons and other non-scene entities stay).
void ClearScene(ecs::World& world);

} // namespace zx
