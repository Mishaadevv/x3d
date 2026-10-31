#include "SequencerEditor.h"
#include "Editor.h"
#include "Core/FileSystem/FileSystem.h"
#include "Core/String/Path.h"
#include "Scene/Hierarchy.h"

ZX_DEFINE_LOG_CATEGORY_STATIC(LogSeqEditor, Info);

namespace zx::editor {

namespace {

constexpr f32 RowHeight = 28.0f;
constexpr f32 RulerHeight = 26.0f;
constexpr f32 HeaderWidth = 230.0f;
constexpr f32 FrameRate = 30.0f;
constexpr usize MaxUndo = 100;

struct TrackStyle
{
    const char* icon;
    Color color;
};

TrackStyle StyleOf(SeqTrackType t)
{
    switch (t) {
    case SeqTrackType::Transform: return {ZX_ICON_MOVE_3D, Color(0x5A, 0xA0, 0xF5)};
    case SeqTrackType::Animation: return {ZX_ICON_BONE, Color(0x6C, 0xC8, 0x6A)};
    case SeqTrackType::Event: return {ZX_ICON_ZAP, Color(0xF0, 0xB8, 0x4A)};
    case SeqTrackType::Camera: return {ZX_ICON_VIDEO, Color(0xE0, 0x78, 0xC8)};
    case SeqTrackType::Audio: return {ZX_ICON_VOLUME_2, Color(0xB0, 0x7C, 0xF0)};
    default: return {ZX_ICON_KEY, Color(0x9A, 0x9A, 0x9A)};
    }
}

const char* TrackTooltip(SeqTrackType t)
{
    switch (t) {
    case SeqTrackType::Transform: return "Moves, turns and scales an entity between keys";
    case SeqTrackType::Animation: return "Plays a clip of the entity's Animator from each key";
    case SeqTrackType::Event: return "Calls fn onSequenceEvent(name) in the scripts of the entity";
    case SeqTrackType::Camera: return "Cuts to a Camera entity (an empty key gives the view back to the game camera)";
    case SeqTrackType::Audio: return "Plays a sound file at each key";
    default: return "";
    }
}

// --- Document helpers ------------------------------------------------------------------------------------------------

String Snapshot(const SequenceAsset& s) { return WriteSequence(s); }

bool ValidTrack(const SeqEditorState& st) { return st.selTrack >= 0 && usize(st.selTrack) < st.seq.tracks.Size(); }

bool ValidKey(const SeqEditorState& st)
{
    return ValidTrack(st) && st.selKey >= 0 && usize(st.selKey) < st.seq.tracks[usize(st.selTrack)].keys.Size();
}

f32 Snap(const SeqEditorState& st, f32 t) { return st.snap ? Round(t * FrameRate) / FrameRate : t; }

String EntityNameOf(Editor& ed, ecs::Entity e)
{
    const scene::EntityName* n = ed.world.IsAlive(e) ? ed.world.Get<const scene::EntityName>(e) : nullptr;
    return n && !n->value.Empty() ? String(n->value) : String("Entity");
}

/// How a track names an entity: "@self" for the Sequence Player's entity, "@self/Child/Path" below it, else its name.
String TargetNameFor(Editor& ed, ecs::Entity owner, ecs::Entity e)
{
    if (!e || !ed.world.IsAlive(e))
        return String();
    if (e == owner)
        return String("@self");
    String path = EntityNameOf(ed, e);
    if (owner)
        for (ecs::Entity x = scene::GetParent(ed.world, e); x; x = scene::GetParent(ed.world, x)) {
            if (x == owner)
                return String::Format("@self/{}", path);
            path = String::Format("{}/{}", EntityNameOf(ed, x), path);
        }
    return EntityNameOf(ed, e);
}

/// The first Animator on `e` or below it.
ecs::Entity FindAnimatorEntity(Editor& ed, ecs::Entity e)
{
    Array<ecs::Entity> queue;
    if (e)
        queue.PushBack(e);
    for (usize q = 0; q < queue.Size() && q < 256; ++q) {
        if (ed.world.Has<Animator>(queue[q]))
            return queue[q];
        queue.Append(scene::GetChildren(ed.world, queue[q]));
    }
    return {};
}

String RelativePath(Editor& ed, const SeqEditorState& st) { return Path::Normalize(Path::MakeRelative(st.path, ed.contentRoot)); }

/// The Sequence Player that plays this file: the selected entity if it does, else the one already used, else the first.
ecs::Entity FindOwner(Editor& ed, const SeqEditorState& st)
{
    const String rel = RelativePath(ed, st);
    auto uses = [&](ecs::Entity e) {
        const SequencePlayer* p = e && ed.world.IsAlive(e) ? ed.world.Get<const SequencePlayer>(e) : nullptr;
        return p && Path::Normalize(p->sequence) == rel;
    };
    if (uses(ed.selected))
        return ed.selected;
    if (uses(st.owner))
        return st.owner;
    ecs::Entity found;
    ed.world.Query<const SequencePlayer>().Each([&](ecs::Entity e, const SequencePlayer& p) {
        if (!found && Path::Normalize(p.sequence) == rel)
            found = e;
    });
    return found;
}

void RestoreViewportCamera(Editor& ed, SeqEditorState& st)
{
    if (!st.hasSavedCamera)
        return;
    ed.camera.position = st.savedCameraPosition;
    ed.camera.yaw = st.savedYaw;
    ed.camera.pitch = st.savedPitch;
    ed.camera.fovY = st.savedFov;
    st.hasSavedCamera = false;
}

void ResetPreview(Editor& ed, SeqEditorState& st)
{
    ed.sequencer.audio = ed.audio.Get(); // so that running sounds stop
    ed.sequencer.Restore(ed.world, st.session);
    st.session = SequenceSession();
    st.applyNeeded = true;
    RestoreViewportCamera(ed, st);
}

/// The camera entity the current cut shows (evaluated from the Camera tracks, so it also works without the preview).
ecs::Entity CurrentCutCamera(Editor& ed, SeqEditorState& st)
{
    ecs::Entity cam;
    for (const SeqTrack& t : st.seq.tracks) {
        if (t.type != SeqTrackType::Camera || t.muted)
            continue;
        if (const SeqKey* k = ActiveSeqKey(t, st.time))
            cam = k->text.Empty() ? ecs::Entity() : ed.sequencer.Resolve(ed.world, st.session, k->text.View(), st.owner);
    }
    return cam;
}

/// "Look through the cut": the viewport camera sits where the cut's camera is (the editor's own view comes back when
/// it is switched off, the cut ends, or the window closes).
void UpdateCameraView(Editor& ed, SeqEditorState& st, bool playingGame)
{
    const ecs::Entity cam = st.cameraView && !playingGame ? CurrentCutCamera(ed, st) : ecs::Entity();
    if (!cam || !ed.world.IsAlive(cam)) {
        RestoreViewportCamera(ed, st);
        return;
    }
    if (!st.hasSavedCamera) {
        st.savedCameraPosition = ed.camera.position;
        st.savedYaw = ed.camera.yaw;
        st.savedPitch = ed.camera.pitch;
        st.savedFov = ed.camera.fovY;
        st.hasSavedCamera = true;
    }
    const Mat4 m = scene::ComputeWorldMatrix(ed.world, cam);
    const Vec3 f = SafeNormalize(m.GetForward(), Vec3(0, 0, 1));
    ed.camera.position = m.GetTranslation();
    ed.camera.yaw = Atan2(f.x, f.z);
    ed.camera.pitch = -Asin(f.y); // positive pitch looks down
    if (const Camera* c = ed.world.Get<const Camera>(cam))
        ed.camera.fovY = DegToRad(Clamp(c->fieldOfView, 5.0f, 170.0f));
}

void Recheck(Editor& ed, SeqEditorState& st)
{
    st.problems.Clear();
    CheckSequence(st.seq, st.problems);
    st.missing.Clear();
    SequenceSession probe; // fresh: no cached misses
    for (const SeqTrack& t : st.seq.tracks) {
        if (t.muted)
            continue;
        const String label = t.name.Empty() ? String::Format("{} {}", SeqTrackTypeName(t.type), t.target.Empty() ? String("(self)") : t.target) : t.name;
        if (t.type != SeqTrackType::Camera) {
            if (!ed.sequencer.Resolve(ed.world, probe, t.target.View(), st.owner))
                st.missing.PushBack(t.target.Empty() || t.target == "@self"
                                        ? String::Format("Track '{}': no Sequence Player uses this file, so \"@self\" is nothing", label)
                                        : String::Format("Track '{}': no entity \"{}\" in the scene", label, t.target));
        } else {
            for (const SeqKey& k : t.keys)
                if (!k.text.Empty() && !ed.sequencer.Resolve(ed.world, probe, k.text.View(), st.owner)) {
                    st.missing.PushBack(String::Format("Track '{}': no camera entity \"{}\" in the scene", label, k.text));
                    break;
                }
        }
    }
}

void PushUndo(SeqEditorState& st)
{
    st.undo.PushBack(Snapshot(st.seq));
    if (st.undo.Size() > MaxUndo)
        st.undo.RemoveAt(0);
    st.redo.Clear();
}

/// Something in the sequence changed: the preview poses the scene again from the originals.
void Changed(Editor& ed, SeqEditorState& st)
{
    st.dirty = true;
    Recheck(ed, st);
    ed.sequencer.audio = ed.audio.Get();
    ed.sequencer.Restore(ed.world, st.session);
    st.session.names.Clear();
    st.session.missUntil.Clear();
    st.applyNeeded = true;
}

void BeginEdit(SeqEditorState& st, StringView field)
{
    if (st.editField == field)
        return;
    st.editField = String(field);
    PushUndo(st);
}

void Save(Editor& ed, SeqEditorState& st)
{
    if (st.path.Empty())
        return;
    for (SeqTrack& t : st.seq.tracks)
        SortSeqKeys(t);
    if (fs::WriteTextFile(st.path, WriteSequence(st.seq))) {
        st.dirty = false;
        ed.sequencer.Reload(); // players pick it up the next time they start
        ZX_LOG_INFO(LogSeqEditor, "Saved {}", Path::FileName(st.path));
    } else {
        ZX_LOG_ERROR(LogSeqEditor, "Can't write {}", st.path);
    }
}

/// Moves a key in time, keeping the keys sorted (returns the key's new index).
void SetKeyTime(SeqTrack& tr, i32& idx, f32 t)
{
    tr.keys[usize(idx)].time = t;
    while (idx > 0 && tr.keys[usize(idx) - 1].time > tr.keys[usize(idx)].time) {
        swap(tr.keys[usize(idx) - 1], tr.keys[usize(idx)]);
        --idx;
    }
    while (usize(idx) + 1 < tr.keys.Size() && tr.keys[usize(idx) + 1].time < tr.keys[usize(idx)].time) {
        swap(tr.keys[usize(idx) + 1], tr.keys[usize(idx)]);
        ++idx;
    }
}

/// Reads the target's current local transform into a Transform key.
bool Capture(Editor& ed, SeqEditorState& st, const SeqTrack& tr, SeqKey& k)
{
    const ecs::Entity e = ed.sequencer.Resolve(ed.world, st.session, tr.target.View(), st.owner);
    const Transform* t = e ? ed.world.Get<const Transform>(e) : nullptr;
    if (!t)
        return false;
    k.position = t->position;
    k.rotation = t->rotation.ToEulerDegrees();
    k.scale = t->scale;
    return true;
}

Array<String> ClipNamesOf(Editor& ed, SeqEditorState& st, const SeqTrack& tr)
{
    Array<String> names;
    const ecs::Entity target = ed.sequencer.Resolve(ed.world, st.session, tr.target.View(), st.owner);
    const ecs::Entity animator = FindAnimatorEntity(ed, target);
    if (const Animator* a = animator ? ed.world.Get<const Animator>(animator) : nullptr)
        for (const asset::AssetId& id : a->clips) {
            const String n = ed.assets->GetName(id);
            if (!n.Empty())
                names.PushBack(n);
        }
    return names;
}

void AddKey(Editor& ed, SeqEditorState& st, i32 trackIndex, f32 time)
{
    if (trackIndex < 0 || usize(trackIndex) >= st.seq.tracks.Size())
        return;
    PushUndo(st);
    SeqTrack& tr = st.seq.tracks[usize(trackIndex)];
    time = Clamp(Snap(st, time), 0.0f, st.seq.duration);
    st.session.missUntil.Clear();
    SeqKey k;
    k.time = time;
    switch (tr.type) {
    case SeqTrackType::Transform: Capture(ed, st, tr, k); break;
    case SeqTrackType::Animation: {
        const Array<String> clips = ClipNamesOf(ed, st, tr);
        if (!clips.Empty())
            k.text = clips[0];
        if (const SeqKey* prev = ActiveSeqKey(tr, time))
            k.text = prev->text.Empty() ? k.text : prev->text;
        break;
    }
    case SeqTrackType::Event: k.text = "Event"; break;
    case SeqTrackType::Camera:
        if (const SeqKey* prev = ActiveSeqKey(tr, time); prev && !prev->text.Empty())
            k.text = prev->text;
        break;
    case SeqTrackType::Audio:
    case SeqTrackType::Count: break;
    }
    // One key per instant on tracks that hold a state (a second one would be ignored): replace it.
    i32 index = -1;
    if (tr.type == SeqTrackType::Transform || tr.type == SeqTrackType::Animation || tr.type == SeqTrackType::Camera)
        for (usize i = 0; i < tr.keys.Size(); ++i)
            if (Abs(tr.keys[i].time - time) < 1e-4f)
                index = i32(i);
    if (index >= 0) {
        if (tr.type == SeqTrackType::Transform) {
            SeqKey& old = tr.keys[usize(index)];
            old.position = k.position;
            old.rotation = k.rotation;
            old.scale = k.scale;
        }
    } else {
        tr.keys.PushBack(k);
        SortSeqKeys(tr);
        for (usize i = 0; i < tr.keys.Size(); ++i)
            if (Abs(tr.keys[i].time - time) < 1e-6f)
                index = i32(i);
    }
    st.selTrack = trackIndex;
    st.selKey = index;
    Changed(ed, st);
}

void AddTrack(Editor& ed, SeqEditorState& st, SeqTrackType type)
{
    PushUndo(st);
    SeqTrack t;
    t.type = type;
    const ecs::Entity sel = ed.world.IsAlive(ed.selected) ? ed.selected : ecs::Entity();
    st.session.missUntil.Clear();
    if (type == SeqTrackType::Camera) {
        ecs::Entity cam = sel && ed.world.Has<Camera>(sel) ? sel : ecs::Entity();
        if (!cam)
            ed.world.Query<const Camera>().Each([&](ecs::Entity e, const Camera&) {
                if (!cam)
                    cam = e;
            });
        SeqKey k;
        k.time = Clamp(Snap(st, st.time), 0.0f, st.seq.duration);
        k.text = TargetNameFor(ed, st.owner, cam);
        t.keys.PushBack(k);
    } else {
        t.target = TargetNameFor(ed, st.owner, sel);
        if (t.target.Empty())
            t.target = "@self";
        if (type == SeqTrackType::Transform) {
            SeqKey k;
            k.time = Clamp(Snap(st, st.time), 0.0f, st.seq.duration);
            Capture(ed, st, t, k);
            t.keys.PushBack(k);
        }
    }
    st.seq.tracks.PushBack(move(t));
    st.selTrack = i32(st.seq.tracks.Size()) - 1;
    st.selKey = st.seq.tracks.Back().keys.Empty() ? -1 : 0;
    Changed(ed, st);
}

void RemoveTrack(Editor& ed, SeqEditorState& st, i32 index)
{
    if (index < 0 || usize(index) >= st.seq.tracks.Size())
        return;
    PushUndo(st);
    st.seq.tracks.RemoveAt(usize(index));
    st.selTrack = -1;
    st.selKey = -1;
    Changed(ed, st);
}

void RemoveKey(Editor& ed, SeqEditorState& st)
{
    if (!ValidKey(st))
        return;
    PushUndo(st);
    st.seq.tracks[usize(st.selTrack)].keys.RemoveAt(usize(st.selKey));
    st.selKey = -1;
    Changed(ed, st);
}

/// Playhead moved by the user: no sounds fire for what was jumped over.
void SetTime(SeqEditorState& st, f32 t)
{
    st.time = Clamp(t, 0.0f, st.seq.duration);
    st.prev = st.time - 1e-4f;
    st.applyNeeded = true;
}

void FrameAll(SeqEditorState& st, const Rect& rows, f32 s)
{
    const f32 width = Max(rows.Width() / s - 40.0f, 100.0f);
    st.pps = Clamp(width / Max(st.seq.duration, 0.1f), 10.0f, 1200.0f);
    st.scrollT = -10.0f / st.pps;
}

// --- Drawing ---------------------------------------------------------------------------------------------------------

void DrawDiamond(ui::DrawList& dl, Vec2 c, f32 r, Color fill, Color outline)
{
    const Vec2 pts[4] = {c + Vec2(0.0f, -r), c + Vec2(r, 0.0f), c + Vec2(0.0f, r), c + Vec2(-r, 0.0f)};
    dl.AddConvexPolygon(Span<const Vec2>(pts, 4), outline);
    const f32 q = r - 1.5f;
    const Vec2 in[4] = {c + Vec2(0.0f, -q), c + Vec2(q, 0.0f), c + Vec2(0.0f, q), c + Vec2(-q, 0.0f)};
    dl.AddConvexPolygon(Span<const Vec2>(in, 4), fill);
}

f32 RulerStep(f32 pxPerSecond)
{
    static const f32 steps[] = {1.0f / 30.0f, 0.1f, 0.25f, 0.5f, 1.0f, 2.0f, 5.0f, 10.0f, 30.0f, 60.0f, 300.0f};
    for (f32 s : steps)
        if (s * pxPerSecond >= 70.0f)
            return s;
    return 600.0f;
}

/// Where a track's block (Animation clip / Camera cut) ends.
f32 BlockEnd(const SeqTrack& tr, usize i, f32 duration)
{
    const SeqKey& k = tr.keys[i];
    const f32 next = i + 1 < tr.keys.Size() ? tr.keys[i + 1].time : duration;
    if (tr.type == SeqTrackType::Animation && k.length > 0.0f)
        return Min(k.time + k.length, next);
    return next;
}

// --- Side panel ------------------------------------------------------------------------------------------------------

void Field(StringView text)
{
    ui::AlignTextToFramePadding();
    ui::Text(text);
    ui::SameLine(84.0f);
    ui::SetNextItemWidth(-1.0f);
}

void DetailsPanel(Editor& ed, SeqEditorState& st)
{
    const ui::Theme& th = ui::GetCurrentContext()->GetStyle().colors;
    ui::SeparatorText("Track");
    if (!ValidTrack(st)) {
        ui::TextDisabled("Select a track to edit it");
    } else {
        SeqTrack& tr = st.seq.tracks[usize(st.selTrack)];
        const TrackStyle style = StyleOf(tr.type);
        ui::TextColored(style.color, String::Format("{}  {}", style.icon, SeqTrackTypeName(tr.type)));
        ui::TextWrapped(TrackTooltip(tr.type));
        ui::Spacing();
        Field("Name");
        if (ui::InputText("##trname", tr.name, ui::InputTextFlags::None, "(shown in the list)")) {
            BeginEdit(st, "trname");
            st.dirty = true;
        }
        if (tr.type != SeqTrackType::Camera) {
            Field("Target");
            if (ui::InputText("##trtarget", tr.target, ui::InputTextFlags::None, "entity name or @self")) {
                BeginEdit(st, "trtarget");
                Changed(ed, st);
            }
            ui::ItemTooltip("An entity name, \"@self\" (the Sequence Player's entity) or \"@self/Child/Path\"");
            if (ui::Button(ZX_ICON_CROSSHAIR "  Use Selected Entity", Vec2(-1.0f, 0.0f))) {
                if (ed.world.IsAlive(ed.selected)) {
                    PushUndo(st);
                    tr.target = TargetNameFor(ed, st.owner, ed.selected);
                    Changed(ed, st);
                }
            }
        }
        bool muted = tr.muted;
        if (ui::Checkbox("Muted", &muted)) {
            PushUndo(st);
            tr.muted = muted;
            Changed(ed, st);
        }
    }

    ui::SeparatorText("Key");
    if (!ValidKey(st)) {
        ui::TextDisabled(ValidTrack(st) ? "Select a key, or press K to add one at the playhead" : "No key selected");
    } else {
        SeqTrack& tr = st.seq.tracks[usize(st.selTrack)];
        SeqKey& k = tr.keys[usize(st.selKey)];
        Field("Time");
        f32 t = k.time;
        if (ui::DragFloat("##ktime", &t, 0.01f, 0.0f, 3600.0f, "{:.2f} s")) {
            BeginEdit(st, "ktime");
            i32 idx = st.selKey;
            SetKeyTime(tr, idx, Max(t, 0.0f));
            st.selKey = idx;
            Changed(ed, st);
        }
        switch (tr.type) {
        case SeqTrackType::Transform: {
            Field("Position");
            f32 p[3] = {k.position.x, k.position.y, k.position.z};
            if (ui::DragFloat3("##kpos", p, 0.05f)) {
                BeginEdit(st, "kpos");
                k.position = Vec3(p[0], p[1], p[2]);
                Changed(ed, st);
            }
            Field("Rotation");
            f32 r[3] = {k.rotation.x, k.rotation.y, k.rotation.z};
            if (ui::DragFloat3("##krot", r, 0.5f)) {
                BeginEdit(st, "krot");
                k.rotation = Vec3(r[0], r[1], r[2]);
                Changed(ed, st);
            }
            ui::ItemTooltip("Pitch, yaw, roll in degrees");
            Field("Scale");
            f32 sc[3] = {k.scale.x, k.scale.y, k.scale.z};
            if (ui::DragFloat3("##kscale", sc, 0.01f)) {
                BeginEdit(st, "kscale");
                k.scale = Vec3(sc[0], sc[1], sc[2]);
                Changed(ed, st);
            }
            static const StringView names[] = {"Linear", "Smooth", "Step", "Spline"};
            i32 interp = i32(k.interp);
            Field("Blend");
            if (ui::Combo("##kinterp", &interp, Span<const StringView>(names, 4))) {
                PushUndo(st);
                k.interp = SeqInterp(interp);
                Changed(ed, st);
            }
            ui::ItemTooltip("How the segment after this key is crossed: Smooth eases in and out, Step holds, Spline curves through the positions");
            if (ui::Button(ZX_ICON_CROSSHAIR "  Capture from Entity", Vec2(-1.0f, 0.0f))) {
                PushUndo(st);
                if (!Capture(ed, st, tr, k))
                    ZX_LOG_WARN(LogSeqEditor, "The target is not in the scene");
                Changed(ed, st);
            }
            ui::ItemTooltip("Reads the entity's transform as it is now into this key (move it in the viewport first)");
            break;
        }
        case SeqTrackType::Animation: {
            Field("Clip");
            if (ui::InputText("##kclip", k.text, ui::InputTextFlags::None, "clip name")) {
                BeginEdit(st, "kclip");
                Changed(ed, st);
            }
            const Array<String> clips = ClipNamesOf(ed, st, tr);
            if (!clips.Empty()) {
                Array<StringView> items;
                items.PushBack("(pick a clip)");
                for (const String& c : clips)
                    items.PushBack(c.View());
                i32 pick = 0;
                Field("");
                if (ui::Combo("##kclips", &pick, items.View()) && pick > 0) {
                    PushUndo(st);
                    k.text = clips[usize(pick) - 1];
                    Changed(ed, st);
                }
            } else {
                ui::TextDisabled("The target has no Animator (or its model is not loaded)");
            }
            Field("Speed");
            if (ui::DragFloat("##kspeed", &k.speed, 0.01f, 0.0f, 8.0f, "{:.2f}x")) {
                BeginEdit(st, "kspeed");
                Changed(ed, st);
            }
            Field("Length");
            if (ui::DragFloat("##klength", &k.length, 0.02f, 0.0f, 3600.0f, "{:.2f} s")) {
                BeginEdit(st, "klength");
                Changed(ed, st);
            }
            ui::ItemTooltip("How long the clip block lasts (0 = until the next key)");
            if (ui::Checkbox("Loop", &k.loop)) {
                PushUndo(st);
                Changed(ed, st);
            }
            break;
        }
        case SeqTrackType::Event:
            Field("Event");
            if (ui::InputText("##kevent", k.text, ui::InputTextFlags::None, "name")) {
                BeginEdit(st, "kevent");
                Changed(ed, st);
            }
            ui::ItemTooltip("Scripts on the target (and its parents) get fn onSequenceEvent(name) while the game plays");
            break;
        case SeqTrackType::Camera: {
            Field("Camera");
            if (ui::InputText("##kcamera", k.text, ui::InputTextFlags::None, "(empty = release)")) {
                BeginEdit(st, "kcamera");
                Changed(ed, st);
            }
            Array<String> cameras;
            ed.world.Query<const Camera>().Each([&](ecs::Entity e, const Camera&) { cameras.PushBack(TargetNameFor(ed, st.owner, e)); });
            Array<StringView> items;
            items.PushBack("(pick a camera)");
            items.PushBack("(release the camera)");
            for (const String& c : cameras)
                items.PushBack(c.View());
            i32 pick = 0;
            Field("");
            if (ui::Combo("##kcameras", &pick, items.View()) && pick > 0) {
                PushUndo(st);
                k.text = pick == 1 ? String() : cameras[usize(pick) - 2];
                Changed(ed, st);
            }
            Field("FOV");
            if (ui::DragFloat("##kfov", &k.fov, 0.2f, 0.0f, 170.0f, "{:.1f} deg")) {
                BeginEdit(st, "kfov");
                Changed(ed, st);
            }
            ui::ItemTooltip("Vertical field of view during this cut (0 = the camera's own)");
            break;
        }
        case SeqTrackType::Audio: {
            Field("Sound");
            if (ui::InputText("##ksound", k.text, ui::InputTextFlags::None, "Content-relative .wav / .ogg")) {
                BeginEdit(st, "ksound");
                Changed(ed, st);
            }
            const String& picked = ed.panels.browserSelected;
            if ((Path::HasExtension(picked, ".wav") || Path::HasExtension(picked, ".ogg")) &&
                ui::Button(ZX_ICON_FILE_AUDIO "  Use Selected Sound", Vec2(-1.0f, 0.0f))) {
                PushUndo(st);
                k.text = Path::Normalize(Path::MakeRelative(picked, ed.contentRoot));
                Changed(ed, st);
            }
            Field("Volume");
            if (ui::DragFloat("##kvolume", &k.volume, 0.01f, 0.0f, 4.0f, "{:.2f}")) {
                BeginEdit(st, "kvolume");
                Changed(ed, st);
            }
            if (ui::Checkbox("3D at the target", &k.spatial)) {
                PushUndo(st);
                Changed(ed, st);
            }
            break;
        }
        case SeqTrackType::Count: break;
        }
        if (ui::Button(ZX_ICON_TRASH_2 "  Delete Key", Vec2(-1.0f, 0.0f)))
            RemoveKey(ed, st);
    }

    if (!st.problems.Empty() || !st.missing.Empty()) {
        ui::SeparatorText("Problems");
        ui::PushTextColor(th.warning);
        for (const String& p : st.problems)
            ui::TextWrapped(p);
        for (const String& p : st.missing)
            ui::TextWrapped(p);
        ui::PopTextColor();
    }
}

} // namespace

bool CreateSequenceFile(StringView physicalPath)
{
    SequenceAsset s;
    s.name = String(Path::Stem(physicalPath));
    return fs::WriteTextFile(physicalPath, WriteSequence(s));
}

void CreateCinematic(Editor& ed)
{
    const String dir = ed.panels.browserCurrent.Empty() ? ed.contentRoot : ed.panels.browserCurrent;
    String path = Path::Join(dir, "Cinematic.zseq");
    for (u32 n = 2; platform::FileExists(path); ++n)
        path = Path::Join(dir, String::Format("Cinematic{}.zseq", n));
    const Vec3 from = ed.camera.position;
    const Vec3 to = from + ed.camera.Forward() * 10.0f;
    const f32 yaw = RadToDeg(ed.camera.yaw), pitch = RadToDeg(ed.camera.pitch);
    SequenceAsset seq = MakeCameraFlyover("@self/Cinematic Camera", from, to, yaw, pitch, 6.0f);
    seq.name = String(Path::Stem(path));
    if (!fs::WriteTextFile(path, WriteSequence(seq))) {
        ZX_LOG_ERROR(LogSeqEditor, "Can't write {}", path);
        return;
    }
    const ecs::Entity owner = ed.CreateEntity("Cinematic", Transform());
    SequencePlayer player;
    player.sequence = Path::Normalize(Path::MakeRelative(path, ed.contentRoot));
    player.playOnStart = false;
    player.restoreWhenDone = true;
    ed.world.Add(owner, player);
    // Inactive until the sequence cuts to it (an active camera would be the game camera from the first frame).
    const ecs::Entity camera = ed.CreateEntity("Cinematic Camera", Transform(from, Quat::FromEuler(DegToRad(pitch), DegToRad(yaw), 0.0f)), owner);
    Camera cam;
    cam.active = false;
    ed.world.Add(camera, cam);
    ed.CommitCreated(owner, "Create Cinematic");
    OpenSequencerEditor(ed, path);
}

void PauseSequencerPreview(Editor& ed)
{
    SeqEditorState& st = ed.seqEditor;
    if (st.open)
        ResetPreview(ed, st);
}

void OpenSequencerEditor(Editor& ed, StringView physicalPath)
{
    SeqEditorState& st = ed.seqEditor;
    if (st.open && st.dirty && st.path != physicalPath)
        Save(ed, st);
    String text, error;
    SequenceAsset seq;
    if (!fs::ReadTextFile(physicalPath, text) || !ParseSequence(text.View(), seq, &error)) {
        ZX_LOG_ERROR(LogSeqEditor, "Can't open {}: {}", physicalPath, error);
        return;
    }
    if (st.open)
        ResetPreview(ed, st);
    const bool camera = st.cameraView;
    st = SeqEditorState();
    st.cameraView = camera;
    st.open = true;
    st.path = String(physicalPath);
    st.seq = move(seq);
    st.owner = FindOwner(ed, st);
    Recheck(ed, st);
}

void DrawSequencerPanel(Editor& ed)
{
    SeqEditorState& st = ed.seqEditor;
    st.capturesKeys = false;
    if (!st.open)
        return;
    ui::Context& ctx = *ui::GetCurrentContext();
    const f32 s = ctx.GetScale();
    const ui::Theme& th = ctx.GetStyle().colors;
    {
        const Vec2 display = Vec2(f32(ed.swapChain->GetSize().x), f32(ed.swapChain->GetSize().y)) / s;
        const Vec2 size = display * Vec2(0.94f, 0.46f);
        ui::SetNextWindowSize(size, ui::Cond::FirstUse);
        ui::SetNextWindowPos(Vec2((display.x - size.x) * 0.5f, display.y - size.y - 40.0f), ui::Cond::FirstUse);
    }
    bool open = true;
    if (!ui::Begin(SequencerTitle, &open, ui::WindowFlags::NoScrollbar)) {
        ui::End();
        if (!open) {
            if (st.dirty)
                Save(ed, st);
            ResetPreview(ed, st);
            st.open = false;
            st.playing = false;
        }
        return;
    }
    if (!open) {
        if (st.dirty)
            Save(ed, st);
        ResetPreview(ed, st);
        st.open = false;
        st.playing = false;
        ui::End();
        return;
    }
    if (st.path.Empty()) {
        ui::TextDisabled("Open a .zseq from the Content Browser, or Create > Cinematic");
        ui::End();
        return;
    }

    const bool playingGame = ed.IsPlaying();
    const f32 duration = Max(st.seq.duration, 0.1f);
    if (playingGame)
        st.playing = false;
    // The owner may change (selection); its "@self" targets follow.
    if (const ecs::Entity owner = FindOwner(ed, st); owner != st.owner) {
        ResetPreview(ed, st);
        st.owner = owner;
        Recheck(ed, st);
    }
    if (ed.frameIndex % 30 == 0)
        Recheck(ed, st);

    // --- Playback of the timeline (not while the game plays: its Sequence Players run themselves) ---
    if (playingGame) {
        if (ed.sequencer.IsPlaying(st.owner))
            st.time = Min(ed.sequencer.Time(st.owner), duration);
    } else if (st.playing) {
        ed.sequencer.audio = ed.audio.Get();
        ed.sequencer.animation = ed.animation.Get();
        const f32 before = st.prev;
        f32 next = st.time + ed.dt;
        if (next >= duration) {
            if (st.loop) {
                ed.sequencer.Fire(ed.world, st.seq, st.owner, before, duration, st.session);
                next = Fmod(next, duration);
                st.prev = -1e-4f;
            } else {
                next = duration;
                st.playing = false;
            }
        }
        ed.sequencer.Fire(ed.world, st.seq, st.owner, st.prev, next, st.session);
        ed.sequencer.ClearEvents();
        st.time = next;
        st.prev = next;
        st.applyNeeded = true;
    }
    if (st.applyNeeded && st.preview && !playingGame) {
        ed.sequencer.audio = ed.audio.Get();
        ed.sequencer.animation = ed.animation.Get();
        ed.sequencer.Apply(ed.world, st.seq, st.owner, st.time, st.session);
        st.applyNeeded = false;
    }
    if (!st.preview && st.session.camerasTaken) // preview switched off: the scene is as it was
        ResetPreview(ed, st);
    UpdateCameraView(ed, st, playingGame);

    // --- Toolbar ---
    if (ui::Button(ZX_ICON_SAVE "  Save", Vec2::Zero(), st.dirty ? ui::ButtonVariant::Primary : ui::ButtonVariant::Default))
        Save(ed, st);
    ui::SameLine();
    ui::AlignTextToFramePadding();
    ui::Text(String::Format("{}{}", Path::FileName(st.path), st.dirty ? "*" : ""));
    ui::SameLine(0.0f, 14.0f);
    if (ui::IconButton(ZX_ICON_SKIP_BACK, "To start (Home)"))
        SetTime(st, 0.0f);
    ui::SameLine();
    if (ui::IconButton(st.playing ? ZX_ICON_PAUSE : ZX_ICON_PLAY, st.playing ? "Pause (Space)" : "Play the timeline (Space)", st.playing) && !playingGame) {
        if (!st.playing && st.time >= duration - 1e-4f)
            SetTime(st, 0.0f);
        st.playing = !st.playing;
        st.prev = st.time - 1e-4f;
    }
    ui::SameLine();
    if (ui::IconButton(ZX_ICON_SKIP_FORWARD, "To end (End)"))
        SetTime(st, duration);
    ui::SameLine();
    if (ui::IconButton(ZX_ICON_REPEAT, "Loop the preview", st.loop))
        st.loop = !st.loop;
    ui::SameLine();
    if (ui::IconButton(ZX_ICON_MAGNET, "Snap keys and the playhead to frames (30 per second)", st.snap))
        st.snap = !st.snap;
    ui::SameLine();
    if (ui::IconButton(ZX_ICON_EYE, "Preview: pose the scene at the playhead (off = the scene as it is)", st.preview)) {
        st.preview = !st.preview;
        st.applyNeeded = true;
        if (!st.preview)
            ResetPreview(ed, st);
    }
    ui::SameLine();
    if (ui::IconButton(ZX_ICON_VIDEO, "Look through the camera of the current cut in the viewport", st.cameraView))
        st.cameraView = !st.cameraView;
    ui::SameLine(0.0f, 14.0f);
    ui::AlignTextToFramePadding();
    ui::Text(String::Format("{:.2f} / {:.2f} s   frame {}", st.time, st.seq.duration, i32(Round(st.time * FrameRate))));
    ui::SameLine(0.0f, 14.0f);
    ui::SetNextItemWidth(110.0f);
    if (ui::DragFloat("##seq_duration", &st.seq.duration, 0.05f, 0.1f, 3600.0f, "{:.2f} s")) {
        BeginEdit(st, "duration");
        st.seq.duration = Max(st.seq.duration, 0.1f);
        st.time = Min(st.time, st.seq.duration);
        Changed(ed, st);
    }
    ui::ItemTooltip("Length of the sequence");
    ui::SameLine();
    if (ui::Button(ZX_ICON_PLUS "  Track"))
        ui::OpenPopup("##seq_add_track");
    ui::SameLine(0.0f, 14.0f);
    if (!st.problems.Empty() || !st.missing.Empty()) {
        ui::TextColored(th.warning, String::Format(ZX_ICON_TRIANGLE_ALERT "  {} problem{}", st.problems.Size() + st.missing.Size(),
                                                   st.problems.Size() + st.missing.Size() == 1 ? "" : "s"));
        ui::ItemTooltip(!st.problems.Empty() ? st.problems[0].View() : st.missing[0].View());
    } else if (playingGame) {
        ui::TextColored(th.success, ZX_ICON_PLAY "  Playing");
    } else if (!st.owner) {
        ui::TextDisabled("No Sequence Player uses this file");
    } else {
        ui::TextColored(th.success, String::Format(ZX_ICON_CHECK "  {}", EntityNameOf(ed, st.owner)));
    }

    // --- Layout: tracks + timeline | details ---
    const Rect content = ui::GetWindowContentRect();
    const f32 rightW = Round(260.0f * s), gap = Round(6.0f * s);
    const f32 top = ui::GetCursorPos().y + Round(4.0f * s);
    const Rect left(Vec2(content.min.x, top), Vec2(content.max.x - rightW - gap, content.max.y));
    const Rect rightRect(Vec2(left.max.x + gap, top), content.max);
    const f32 headerW = Round(HeaderWidth * s), rulerH = Round(RulerHeight * s), rowH = Round(RowHeight * s);
    const Rect headers(Vec2(left.min.x, left.min.y + rulerH), Vec2(left.min.x + headerW, left.max.y));
    const Rect ruler(Vec2(left.min.x + headerW, left.min.y), Vec2(left.max.x, left.min.y + rulerH));
    const Rect rows(Vec2(left.min.x + headerW, left.min.y + rulerH), left.max);
    if (st.frameRequested && rows.Width() > 80.0f) {
        FrameAll(st, rows, s);
        st.frameRequested = false;
    }
    if (!ctx.IsAnyItemActive())
        st.editField.Clear();
    const f32 pxPerSecond = st.pps * s;
    auto X = [&](f32 t) { return rows.min.x + (t - st.scrollT) * pxPerSecond; };
    auto T = [&](f32 x) { return st.scrollT + (x - rows.min.x) / pxPerSecond; };
    const f32 contentH = f32(st.seq.tracks.Size()) * rowH;
    st.scrollY = Clamp(st.scrollY, 0.0f, Max(0.0f, contentH - rows.Height()));
    auto rowTop = [&](usize i) { return rows.min.y + f32(i) * rowH - st.scrollY; };

    ui::DrawList& dl = ui::GetWindowDrawList();
    ui::SetCursorPos(left.min);
    ui::InvisibleButton("##seq_canvas", left.Size() / s);
    const bool hovered = ui::IsItemHovered() || ui::IsItemActive();
    const Vec2 mouse = ui::GetMousePos();
    const bool inRuler = ruler.Contains(mouse);
    const bool inRows = rows.Contains(mouse);
    const bool inHeaders = headers.Contains(mouse);

    // Hit testing: key diamonds first, then the row under the mouse.
    i32 hotTrack = -1, hotKey = -1;
    if ((inRows || inHeaders) && hovered) {
        const i32 row = i32(Floor((mouse.y - rows.min.y + st.scrollY) / rowH));
        if (row >= 0 && usize(row) < st.seq.tracks.Size())
            hotTrack = row;
    }
    if (inRows && hovered)
        for (usize ti = 0; ti < st.seq.tracks.Size(); ++ti) {
            const SeqTrack& tr = st.seq.tracks[ti];
            const f32 cy = rowTop(ti) + rowH * 0.5f;
            if (Abs(mouse.y - cy) > rowH * 0.5f)
                continue;
            for (usize ki = tr.keys.Size(); ki > 0; --ki)
                if (Abs(X(tr.keys[ki - 1].time) - mouse.x) < 8.0f * s) {
                    hotTrack = i32(ti);
                    hotKey = i32(ki - 1);
                    break;
                }
        }

    // --- Mouse ---
    const bool ctrl = HasAnyFlags(ed.input.Modifiers(), KeyModifiers::Control);
    const bool shift = HasAnyFlags(ed.input.Modifiers(), KeyModifiers::Shift);
    auto muteRect = [&](usize i) { return Rect(Vec2(headers.max.x - 50.0f * s, rowTop(i)), Vec2(headers.max.x - 26.0f * s, rowTop(i) + rowH)); };
    auto deleteRect = [&](usize i) { return Rect(Vec2(headers.max.x - 26.0f * s, rowTop(i)), Vec2(headers.max.x - 2.0f * s, rowTop(i) + rowH)); };
    if (hovered && ed.input.WasMousePressed(MouseButton::Left)) {
        st.dragStart = mouse;
        st.dragMoved = false;
        if (inRuler) {
            st.drag = SeqEditorState::Drag::Playhead;
        } else if (inHeaders && hotTrack >= 0) {
            if (muteRect(usize(hotTrack)).Contains(mouse)) {
                PushUndo(st);
                st.seq.tracks[usize(hotTrack)].muted = !st.seq.tracks[usize(hotTrack)].muted;
                Changed(ed, st);
            } else if (deleteRect(usize(hotTrack)).Contains(mouse)) {
                RemoveTrack(ed, st, hotTrack);
            } else {
                st.selTrack = hotTrack;
                st.selKey = -1;
            }
        } else if (inRows) {
            if (hotKey >= 0) {
                st.selTrack = hotTrack;
                st.selKey = hotKey;
                st.beforeDrag = Snapshot(st.seq);
                st.dragOffset = st.seq.tracks[usize(hotTrack)].keys[usize(hotKey)].time - T(mouse.x);
                st.drag = SeqEditorState::Drag::Key;
            } else {
                if (hotTrack >= 0) {
                    st.selTrack = hotTrack;
                    st.selKey = -1;
                }
                if (ui::IsMouseDoubleClicked(MouseButton::Left) && hotTrack >= 0)
                    AddKey(ed, st, hotTrack, T(mouse.x));
                else
                    st.drag = SeqEditorState::Drag::Playhead;
            }
        }
        if (st.drag == SeqEditorState::Drag::Playhead)
            SetTime(st, Snap(st, T(mouse.x)));
    }
    if (hovered && (ed.input.WasMousePressed(MouseButton::Right) || ed.input.WasMousePressed(MouseButton::Middle)) && st.drag == SeqEditorState::Drag::None) {
        st.rightPress = mouse;
        st.dragMoved = false;
        st.drag = SeqEditorState::Drag::Pan;
    }
    if (hovered && ed.input.MouseWheel() != 0.0f) {
        if (shift) {
            st.scrollY = Clamp(st.scrollY - ed.input.MouseWheel() * rowH, 0.0f, Max(0.0f, contentH - rows.Height()));
        } else {
            const f32 under = T(mouse.x);
            st.pps = Clamp(st.pps * Pow(1.15f, ed.input.MouseWheel()), 10.0f, 1200.0f);
            st.scrollT = under - (mouse.x - rows.min.x) / (st.pps * s);
        }
    }
    const Vec2 delta = ed.input.MouseDelta();
    switch (st.drag) {
    case SeqEditorState::Drag::Playhead:
        if (ed.input.IsMouseDown(MouseButton::Left))
            SetTime(st, Snap(st, T(mouse.x)));
        else
            st.drag = SeqEditorState::Drag::None;
        break;
    case SeqEditorState::Drag::Key:
        if (ed.input.IsMouseDown(MouseButton::Left)) {
            if (Length(mouse - st.dragStart) > 3.0f * s)
                st.dragMoved = true;
            if (st.dragMoved && ValidKey(st)) {
                SeqTrack& tr = st.seq.tracks[usize(st.selTrack)];
                i32 idx = st.selKey;
                const f32 t = Clamp(Snap(st, T(mouse.x) + st.dragOffset), 0.0f, st.seq.duration);
                if (Abs(tr.keys[usize(idx)].time - t) > 1e-6f) {
                    SetKeyTime(tr, idx, t);
                    st.selKey = idx;
                    st.dirty = true;
                    st.session.missUntil.Clear();
                    st.applyNeeded = true;
                }
            }
        } else {
            if (st.dragMoved) {
                st.undo.PushBack(st.beforeDrag);
                st.redo.Clear();
                Changed(ed, st);
            }
            st.beforeDrag.Clear();
            st.drag = SeqEditorState::Drag::None;
        }
        break;
    case SeqEditorState::Drag::Pan:
        if (ed.input.IsMouseDown(MouseButton::Right) || ed.input.IsMouseDown(MouseButton::Middle)) {
            if (Length(mouse - st.rightPress) > 4.0f * s)
                st.dragMoved = true;
            st.scrollT = Max(st.scrollT - delta.x / pxPerSecond, -2.0f);
            st.scrollY = Clamp(st.scrollY - delta.y, 0.0f, Max(0.0f, contentH - rows.Height()));
        } else {
            if (!st.dragMoved && hovered) {
                if (hotKey >= 0) {
                    st.selTrack = hotTrack;
                    st.selKey = hotKey;
                } else if (hotTrack >= 0) {
                    st.selTrack = hotTrack;
                    st.selKey = -1;
                }
                st.popupTrack = hotTrack;
                st.popupTime = Clamp(Snap(st, T(mouse.x)), 0.0f, st.seq.duration);
                ui::OpenPopup("##seq_ctx");
            }
            st.drag = SeqEditorState::Drag::None;
        }
        break;
    case SeqEditorState::Drag::None: break;
    }

    // --- Drawing ---
    const ui::FontHandle font = ctx.Fonts().FindFont("Inter");
    const ui::FontHandle semi = ctx.Fonts().FindFont("Inter SemiBold");
    dl.AddRect(left, Color(0x1A, 0x1B, 0x1F));
    // Rows.
    dl.PushClipRect(rows);
    for (usize ti = 0; ti < st.seq.tracks.Size(); ++ti) {
        const f32 y0 = rowTop(ti);
        if (y0 + rowH < rows.min.y || y0 > rows.max.y)
            continue;
        const bool selectedRow = i32(ti) == st.selTrack;
        dl.AddRect(Rect(Vec2(rows.min.x, y0), Vec2(rows.max.x, y0 + rowH)),
                   selectedRow ? WithAlpha(th.accent, 0.14f) : (ti % 2 == 0 ? Color(0x1F, 0x20, 0x25) : Color(0x1B, 0x1C, 0x21)));
        dl.AddLine(Vec2(rows.min.x, y0 + rowH), Vec2(rows.max.x, y0 + rowH), Color(0x2A, 0x2B, 0x31), 1.0f);
    }
    // Time grid.
    {
        const f32 step = RulerStep(pxPerSecond);
        for (f32 t = Floor(st.scrollT / step) * step; X(t) < rows.max.x; t += step)
            if (X(t) >= rows.min.x)
                dl.AddLine(Vec2(X(t), rows.min.y), Vec2(X(t), rows.max.y), Color(0x2A, 0x2B, 0x31), 1.0f);
    }
    // After the end.
    if (X(duration) < rows.max.x)
        dl.AddRect(Rect(Vec2(Max(X(duration), rows.min.x), rows.min.y), rows.max), Color(0, 0, 0, 90));
    for (usize ti = 0; ti < st.seq.tracks.Size(); ++ti) {
        const SeqTrack& tr = st.seq.tracks[ti];
        const f32 y0 = rowTop(ti);
        if (y0 + rowH < rows.min.y || y0 > rows.max.y)
            continue;
        const f32 cy = y0 + rowH * 0.5f;
        const TrackStyle style = StyleOf(tr.type);
        const f32 alpha = tr.muted ? 0.35f : 1.0f;
        const Color base = WithAlpha(style.color, alpha);
        // Blocks and connections.
        for (usize ki = 0; ki < tr.keys.Size(); ++ki) {
            const SeqKey& k = tr.keys[ki];
            if (tr.type == SeqTrackType::Animation || tr.type == SeqTrackType::Camera) {
                const f32 x0 = X(k.time), x1 = X(BlockEnd(tr, ki, duration));
                if (x1 < rows.min.x || x0 > rows.max.x || x1 - x0 < 1.0f)
                    continue;
                const bool release = tr.type == SeqTrackType::Camera && k.text.Empty();
                const Rect block(Vec2(x0, y0 + 4.0f * s), Vec2(x1, y0 + rowH - 4.0f * s));
                dl.AddRect(block, WithAlpha(style.color, release ? 0.10f * alpha : 0.30f * alpha), ui::CornerRadii(4.0f * s), 1.0f, WithAlpha(style.color, 0.6f * alpha));
                String label = release ? String("(game camera)") : k.text;
                if (tr.type == SeqTrackType::Animation && k.loop)
                    label += "  " ZX_ICON_REPEAT;
                dl.AddTextInRect(font, 11.0f * s, Rect(block.min + Vec2(12.0f * s, 0.0f), block.max - Vec2(4.0f * s, 0.0f)), WithAlpha(Color(0xE8, 0xE8, 0xE8), alpha), label);
            } else if (tr.type == SeqTrackType::Transform && ki + 1 < tr.keys.Size()) {
                const SeqKey& n = tr.keys[ki + 1];
                const f32 x0 = X(k.time), x1 = X(n.time);
                if (x1 < rows.min.x || x0 > rows.max.x)
                    continue;
                if (k.interp == SeqInterp::Step) {
                    dl.AddLine(Vec2(x0, cy + 5.0f * s), Vec2(x1, cy + 5.0f * s), WithAlpha(style.color, 0.5f * alpha), 2.0f);
                    dl.AddLine(Vec2(x1, cy + 5.0f * s), Vec2(x1, cy), WithAlpha(style.color, 0.5f * alpha), 2.0f);
                } else {
                    dl.AddLine(Vec2(x0, cy), Vec2(x1, cy), WithAlpha(style.color, (k.interp == SeqInterp::Linear ? 0.35f : 0.6f) * alpha), 2.0f);
                    if (k.interp == SeqInterp::Spline)
                        dl.AddLine(Vec2(x0, cy + 4.0f * s), Vec2(x1, cy + 4.0f * s), WithAlpha(style.color, 0.3f * alpha), 1.0f);
                }
            }
        }
        // Keys.
        for (usize ki = 0; ki < tr.keys.Size(); ++ki) {
            const SeqKey& k = tr.keys[ki];
            const f32 x = X(k.time);
            if (x < rows.min.x - 10.0f || x > rows.max.x + 10.0f)
                continue;
            const bool sel = i32(ti) == st.selTrack && i32(ki) == st.selKey;
            const bool hot = i32(ti) == hotTrack && i32(ki) == hotKey;
            const Color fill = sel ? Color(0xFF, 0xFF, 0xFF) : hot ? WithAlpha(style.color, 1.0f) : base;
            DrawDiamond(dl, Vec2(x, cy), (sel || hot ? 7.5f : 6.5f) * s, fill, sel ? th.accentHovered : Color(0x10, 0x10, 0x12));
            if (tr.type == SeqTrackType::Event || tr.type == SeqTrackType::Audio)
                dl.AddTextInRect(font, 11.0f * s, Rect(Vec2(x + 10.0f * s, y0), Vec2(x + 160.0f * s, y0 + rowH)), WithAlpha(th.textDim, alpha), k.text);
        }
    }
    // End of the sequence + playhead.
    if (X(duration) >= rows.min.x && X(duration) <= rows.max.x)
        dl.AddLine(Vec2(X(duration), rows.min.y), Vec2(X(duration), rows.max.y), Color(0xE8, 0x4C, 0x4C, 160), 1.5f);
    if (st.seq.tracks.Empty())
        dl.AddTextInRect(font, 12.0f * s, rows, th.textDisabled, "Add a track with  + Track,  then press K (or double-click a row) to add keys", ui::TextAlign::Center);
    dl.PopClipRect();

    // Ruler.
    dl.PushClipRect(ruler);
    dl.AddRect(ruler, Color(0x24, 0x25, 0x2A));
    {
        const f32 step = RulerStep(pxPerSecond);
        for (f32 t = Floor(st.scrollT / step) * step; X(t) < ruler.max.x; t += step) {
            const f32 x = X(t);
            if (x < ruler.min.x - 40.0f * s)
                continue;
            dl.AddLine(Vec2(x, ruler.max.y - 10.0f * s), Vec2(x, ruler.max.y), th.textDim, 1.0f);
            for (u32 sub = 1; sub < 5 && step * pxPerSecond >= 100.0f; ++sub) {
                const f32 xs = X(t + step * f32(sub) / 5.0f);
                dl.AddLine(Vec2(xs, ruler.max.y - 5.0f * s), Vec2(xs, ruler.max.y), th.textDisabled, 1.0f);
            }
            dl.AddText(font, 10.5f * s, Vec2(x + 3.0f * s, ruler.min.y + 3.0f * s), th.textDim,
                       step < 0.25f ? String::Format("{:.2f}", Max(t, 0.0f)) : step < 1.0f ? String::Format("{:.1f}", Max(t, 0.0f)) : String::Format("{:.0f}", Max(t, 0.0f)));
        }
    }
    dl.PopClipRect();

    // Track headers.
    dl.PushClipRect(headers);
    dl.AddRect(headers, Color(0x20, 0x21, 0x26));
    for (usize ti = 0; ti < st.seq.tracks.Size(); ++ti) {
        const SeqTrack& tr = st.seq.tracks[ti];
        const f32 y0 = rowTop(ti);
        if (y0 + rowH < headers.min.y || y0 > headers.max.y)
            continue;
        const TrackStyle style = StyleOf(tr.type);
        const f32 alpha = tr.muted ? 0.4f : 1.0f;
        const Rect cell(Vec2(headers.min.x, y0), Vec2(headers.max.x, y0 + rowH));
        if (i32(ti) == st.selTrack)
            dl.AddRect(cell, WithAlpha(th.accent, 0.22f));
        else if (i32(ti) == hotTrack && inHeaders)
            dl.AddRect(cell, Color(0xFF, 0xFF, 0xFF, 10));
        dl.AddRect(Rect(Vec2(cell.min.x, cell.min.y), Vec2(cell.min.x + 3.0f * s, cell.max.y)), WithAlpha(style.color, alpha));
        dl.AddLine(Vec2(cell.min.x, cell.max.y), Vec2(cell.max.x, cell.max.y), Color(0x2A, 0x2B, 0x31), 1.0f);
        const String label = !tr.name.Empty() ? tr.name
                             : tr.type == SeqTrackType::Camera ? String("Camera Cuts")
                             : tr.target.Empty() || tr.target == "@self" ? String("(self)")
                                                                        : tr.target;
        dl.AddTextInRect(semi, 12.0f * s, Rect(cell.min + Vec2(10.0f * s, 0.0f), cell.max - Vec2(56.0f * s, 0.0f)), WithAlpha(Color(0xE8, 0xE8, 0xE8), alpha),
                         String::Format("{}  {}", style.icon, label));
        const Rect mr = muteRect(ti), dr = deleteRect(ti);
        dl.AddTextInRect(font, 12.0f * s, mr, tr.muted ? th.textDisabled : th.textDim, tr.muted ? ZX_ICON_EYE_OFF : ZX_ICON_EYE, ui::TextAlign::Center);
        dl.AddTextInRect(font, 12.0f * s, dr, mr.Contains(mouse) ? th.textDim : th.textDisabled, ZX_ICON_TRASH_2, ui::TextAlign::Center);
        if (inHeaders && hovered && mr.Contains(mouse))
            ui::SetTooltip(tr.muted ? "Unmute" : "Mute");
        else if (inHeaders && hovered && dr.Contains(mouse))
            ui::SetTooltip("Delete the track");
    }
    dl.PopClipRect();
    dl.AddLine(Vec2(headers.max.x, left.min.y), Vec2(headers.max.x, left.max.y), Color(0x10, 0x10, 0x12), 1.0f);

    // Playhead over everything.
    {
        const f32 x = X(st.time);
        if (x >= rows.min.x - 1.0f && x <= rows.max.x + 1.0f) {
            const Color ph(0xF0, 0x5A, 0x5A);
            dl.PushClipRect(Rect(Vec2(rows.min.x - 8.0f * s, left.min.y), left.max));
            dl.AddLine(Vec2(x, ruler.min.y), Vec2(x, rows.max.y), ph, 1.5f);
            dl.AddTriangle(Vec2(x - 7.0f * s, ruler.min.y + 2.0f * s), Vec2(x + 7.0f * s, ruler.min.y + 2.0f * s), Vec2(x, ruler.max.y - 1.0f * s), ph);
            dl.PopClipRect();
        }
    }
    dl.AddRectOutline(left, Color(0x10, 0x10, 0x12), 1.0f);

    // --- Popups ---
    if (ui::BeginPopup("##seq_add_track")) {
        ui::TextDisabled("Add Track");
        for (u32 i = 0; i < u32(SeqTrackType::Count); ++i) {
            const SeqTrackType type = SeqTrackType(i);
            const TrackStyle style = StyleOf(type);
            const bool clicked = ui::MenuItem(String::Format("   {}  {}##seqt{}", style.icon, SeqTrackTypeName(type), i));
            if (ui::IsItemHovered())
                ui::SetTooltip(TrackTooltip(type));
            if (clicked) {
                AddTrack(ed, st, type);
                ui::CloseCurrentPopup();
            }
        }
        ui::EndPopup();
    }
    if (ui::BeginPopup("##seq_ctx")) {
        const bool onTrack = st.popupTrack >= 0 && usize(st.popupTrack) < st.seq.tracks.Size();
        if (ui::MenuItem(ZX_ICON_KEY "  Add Key Here", {}, false, onTrack))
            AddKey(ed, st, st.popupTrack, st.popupTime);
        if (ui::MenuItem(ZX_ICON_TRASH_2 "  Delete Key", "Del", false, ValidKey(st)))
            RemoveKey(ed, st);
        if (ValidKey(st) && st.seq.tracks[usize(st.selTrack)].type == SeqTrackType::Transform && ui::BeginMenu("Blend After This Key")) {
            SeqKey& k = st.seq.tracks[usize(st.selTrack)].keys[usize(st.selKey)];
            for (SeqInterp i : {SeqInterp::Linear, SeqInterp::Smooth, SeqInterp::Step, SeqInterp::Spline})
                if (ui::MenuItem(String::Format("{}##seqi{}", SeqInterpName(i), u32(i)), {}, k.interp == i)) {
                    PushUndo(st);
                    k.interp = i;
                    Changed(ed, st);
                }
            ui::EndMenu();
        }
        ui::Separator();
        if (ui::MenuItem(ZX_ICON_TRASH_2 "  Delete Track", {}, false, onTrack))
            RemoveTrack(ed, st, st.popupTrack);
        if (ui::MenuItem(ZX_ICON_SKIP_FORWARD "  Set Length Here", {}, false, st.popupTime >= 0.1f)) {
            PushUndo(st);
            st.seq.duration = st.popupTime;
            st.time = Min(st.time, st.seq.duration);
            Changed(ed, st);
        }
        ui::EndPopup();
    }

    // --- Keyboard ---
    st.capturesKeys = hovered && !ctx.WantsKeyboard();
    if (st.capturesKeys) {
        if (ui::IsKeyPressed(Key::Space, false) && !playingGame) {
            if (!st.playing && st.time >= duration - 1e-4f)
                SetTime(st, 0.0f);
            st.playing = !st.playing;
            st.prev = st.time - 1e-4f;
        }
        if (ui::IsKeyPressed(Key::K, false))
            AddKey(ed, st, ValidTrack(st) ? st.selTrack : -1, st.time);
        if (ui::IsKeyPressed(Key::Delete, false) || ui::IsKeyPressed(Key::Backspace, false))
            RemoveKey(ed, st);
        if (ui::IsKeyPressed(Key::Left))
            SetTime(st, (ctrl ? Round(st.time * FrameRate) - 10.0f : Round(st.time * FrameRate) - 1.0f) / FrameRate);
        if (ui::IsKeyPressed(Key::Right))
            SetTime(st, (ctrl ? Round(st.time * FrameRate) + 10.0f : Round(st.time * FrameRate) + 1.0f) / FrameRate);
        if (ui::IsKeyPressed(Key::Home, false))
            SetTime(st, 0.0f);
        if (ui::IsKeyPressed(Key::End, false))
            SetTime(st, duration);
        if (ui::IsKeyPressed(Key::F, false))
            st.frameRequested = true;
    }
    if (ui::IsWindowHovered() || st.capturesKeys) {
        if (ui::IsShortcutPressed(KeyModifiers::Control, Key::S))
            Save(ed, st);
        const bool undo = ui::IsShortcutPressed(KeyModifiers::Control, Key::Z) && !ctx.WantsKeyboard();
        const bool redo = (ui::IsShortcutPressed(KeyModifiers::Control, Key::Y) || ui::IsShortcutPressed(KeyModifiers::Control | KeyModifiers::Shift, Key::Z)) &&
                          !ctx.WantsKeyboard();
        if ((undo && !st.undo.Empty()) || (redo && !st.redo.Empty())) {
            Array<String>& from = undo ? st.undo : st.redo;
            Array<String>& to = undo ? st.redo : st.undo;
            to.PushBack(Snapshot(st.seq));
            ParseSequence(from.Back().View(), st.seq, nullptr);
            from.PopBack();
            st.selKey = -1;
            if (!ValidTrack(st))
                st.selTrack = -1;
            Changed(ed, st);
        }
        st.capturesKeys = true;
    }

    // --- Details ---
    ui::SetCursorPos(rightRect.min);
    if (ui::BeginChild("##seq_right", rightRect.Size() / s, false))
        DetailsPanel(ed, st);
    ui::EndChild();
    ui::End();
}

} // namespace zx::editor
