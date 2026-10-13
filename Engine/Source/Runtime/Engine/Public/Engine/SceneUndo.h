#pragma once

// Undo/redo for scene edits, based on entity snapshots (SceneSerialization). A transaction records the state of every
// touched entity before the edit; when it ends, the states after are captured and unchanged entities are dropped.
// Undo applies the "before" snapshots, redo the "after" ones - so any edit expressible as component/hierarchy changes
// (move, property edit, create, delete, reparent, add/remove component) is undoable without per-operation code.
//
//   undo.Begin("Delete");  undo.RecordHierarchy(e);  DestroyHierarchy(world, e);  undo.End();
//   undo.Begin("Create");  e = CreateNode(...);  undo.RecordCreated(e);  undo.End();
//
// Entities are identified by EntityGuid, so handles may change across undo/redo (recreated entities get new ones).

#include "Engine/SceneSerialization.h"

namespace zx {

class SceneUndoStack
{
public:
    explicit SceneUndoStack(ecs::World& world, usize maxEntries = 256) : m_world(world), m_maxEntries(maxEntries) {}
    ZX_NON_COPYABLE(SceneUndoStack)

    /// Transactions nest; only the outermost Begin/End pair creates an undo step (labelled by the outermost Begin).
    void Begin(StringView label);
    /// Ends the transaction. Returns true if an undo step was pushed (something actually changed).
    bool End();
    /// Ends the transaction without pushing a step (the edits stay applied).
    void Discard();
    bool InTransaction() const { return m_depth > 0; }
    /// While suspended (Play-in-Editor), transactions record nothing and Undo/Redo do nothing; the history stays valid
    /// as long as the world is restored to its pre-suspension state (same guids) before resuming.
    void SetSuspended(bool suspended) { m_suspended = suspended; }
    bool IsSuspended() const { return m_suspended; }

    /// Snapshots the entity's current state as its "before" state (first call per entity and transaction wins).
    void Record(ecs::Entity e);
    void RecordHierarchy(ecs::Entity root);
    /// Marks an entity created in this transaction (its "before" state is "does not exist").
    void RecordCreated(ecs::Entity e);
    void RecordCreatedHierarchy(ecs::Entity root);
    /// Records an explicitly captured "before" state (e.g. taken before an in-place inspector edit).
    void RecordState(const Guid& guid, JsonValue before);

    bool CanUndo() const { return m_index > 0; }
    bool CanRedo() const { return m_index < m_entries.Size(); }
    StringView UndoLabel() const { return CanUndo() ? m_entries[m_index - 1].label.View() : StringView(); }
    StringView RedoLabel() const { return CanRedo() ? m_entries[m_index].label.View() : StringView(); }
    /// Apply one step. `affected` receives the step's entities that exist afterwards.
    bool Undo(Array<ecs::Entity>* affected = nullptr);
    bool Redo(Array<ecs::Entity>* affected = nullptr);

    /// History for UI: labels of all steps; steps [0, CurrentIndex()) are applied.
    usize Count() const { return m_entries.Size(); }
    StringView Label(usize i) const { return m_entries[i].label.View(); }
    usize CurrentIndex() const { return m_index; }

    void Clear();
    /// Remembers the current position as "saved"; IsDirty compares against it.
    void MarkSaved() { m_savedIndex = i64(m_index); }
    bool IsDirty() const { return m_savedIndex != i64(m_index); }
    /// Incremented by every push/undo/redo/clear (UI caches can watch it).
    u32 Version() const { return m_version; }

private:
    struct Entry
    {
        String label;
        Array<EntitySnapshot> before;
        Array<EntitySnapshot> after;
    };
    bool Apply(bool undo, Array<ecs::Entity>* affected);

    ecs::World& m_world;
    usize m_maxEntries;
    Array<Entry> m_entries;
    usize m_index = 0;
    i64 m_savedIndex = 0;
    u32 m_version = 0;
    u32 m_depth = 0;
    bool m_suspended = false;
    Entry m_open;
};

} // namespace zx
