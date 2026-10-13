#include "Engine/SceneUndo.h"

namespace zx {

void SceneUndoStack::Begin(StringView label)
{
    if (m_depth++ == 0) {
        m_open = Entry();
        m_open.label = label;
    }
}

void SceneUndoStack::RecordState(const Guid& guid, JsonValue before)
{
    if (m_depth == 0 || m_suspended || !guid.IsValid())
        return;
    for (const EntitySnapshot& s : m_open.before)
        if (s.guid == guid)
            return;
    m_open.before.PushBack(EntitySnapshot{guid, move(before)});
}

void SceneUndoStack::Record(ecs::Entity e)
{
    if (m_depth == 0 || m_suspended || !m_world.IsAlive(e))
        return;
    const Guid guid = EnsureEntityGuid(m_world, e);
    for (const EntitySnapshot& s : m_open.before)
        if (s.guid == guid)
            return;
    // The parent's guid is assigned by SerializeEntity; the parent itself is unchanged by that.
    m_open.before.PushBack(EntitySnapshot{guid, SerializeEntity(m_world, e)});
}

void SceneUndoStack::RecordHierarchy(ecs::Entity root)
{
    Array<ecs::Entity> all;
    CollectHierarchy(m_world, root, all);
    for (ecs::Entity e : all)
        Record(e);
}

void SceneUndoStack::RecordCreated(ecs::Entity e)
{
    if (m_depth > 0 && m_world.IsAlive(e))
        RecordState(EnsureEntityGuid(m_world, e), JsonValue());
}

void SceneUndoStack::RecordCreatedHierarchy(ecs::Entity root)
{
    Array<ecs::Entity> all;
    CollectHierarchy(m_world, root, all);
    for (ecs::Entity e : all)
        RecordCreated(e);
}

bool SceneUndoStack::End()
{
    ZX_ASSERT(m_depth > 0, "SceneUndoStack::End without Begin");
    if (--m_depth > 0)
        return false;
    if (m_suspended) {
        m_open = Entry();
        return false;
    }
    HashMap<Guid, ecs::Entity> map;
    BuildGuidMap(m_world, map);
    Entry entry = move(m_open);
    m_open = Entry();
    Array<EntitySnapshot> before;
    for (EntitySnapshot& s : entry.before) {
        const ecs::Entity* e = map.Find(s.guid);
        JsonValue after = e && m_world.IsAlive(*e) ? SerializeEntity(m_world, *e) : JsonValue();
        if (after == s.state)
            continue;
        entry.after.PushBack(EntitySnapshot{s.guid, move(after)});
        before.PushBack(move(s));
    }
    if (before.Empty())
        return false;
    entry.before = move(before);

    if (m_savedIndex > i64(m_index))
        m_savedIndex = -1; // the saved state was in the discarded redo branch
    m_entries.Resize(m_index);
    m_entries.PushBack(move(entry));
    if (m_entries.Size() > m_maxEntries) {
        m_entries.RemoveAt(0);
        --m_savedIndex; // -1 stays unreachable (no position is negative)
        if (m_savedIndex < 0)
            m_savedIndex = -1;
    }
    m_index = m_entries.Size();
    ++m_version;
    return true;
}

void SceneUndoStack::Discard()
{
    ZX_ASSERT(m_depth > 0, "SceneUndoStack::Discard without Begin");
    if (--m_depth == 0)
        m_open = Entry();
}

bool SceneUndoStack::Apply(bool undo, Array<ecs::Entity>* affected)
{
    if (m_depth > 0 || m_suspended || (undo ? !CanUndo() : !CanRedo()))
        return false;
    const Entry& entry = m_entries[undo ? m_index - 1 : m_index];
    const Array<ecs::Entity> result = ApplyEntitySnapshots(m_world, (undo ? entry.before : entry.after).View());
    if (affected) {
        affected->Clear();
        for (ecs::Entity e : result)
            if (e && m_world.IsAlive(e))
                affected->PushBack(e);
    }
    m_index = undo ? m_index - 1 : m_index + 1;
    ++m_version;
    return true;
}

bool SceneUndoStack::Undo(Array<ecs::Entity>* affected) { return Apply(true, affected); }
bool SceneUndoStack::Redo(Array<ecs::Entity>* affected) { return Apply(false, affected); }

void SceneUndoStack::Clear()
{
    m_entries.Clear();
    m_index = 0;
    m_savedIndex = 0;
    m_depth = 0;
    m_open = Entry();
    ++m_version;
}

} // namespace zx
