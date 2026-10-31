#include "Engine/Sequencer.h"
#include "Core/Containers/Algorithm.h"
#include "Core/FileSystem/FileSystem.h"
#include "Core/Log/Log.h"
#include "Core/Serialization/Json.h"
#include "Core/String/Path.h"
#include "Engine/Animation.h"
#include "Engine/Gameplay.h"
#include "Scene/Hierarchy.h"

#include <cmath>

namespace zx {

ZX_DEFINE_LOG_CATEGORY_STATIC(LogSequencer, Info);

ZX_REFLECT_STRUCT_BEGIN(SequencePlayer)
    ZX_FIELD(sequence, Meta::Tooltip("Sequence (.zseq, Content-relative): the timeline this entity plays"))
    ZX_FIELD(playOnStart, Meta::Tooltip("Starts when the game starts (otherwise scripts: entity.sequencePlay())"))
    ZX_FIELD(loop)
    ZX_FIELD(speed, Meta::Range(0.0f, 8.0f))
    ZX_FIELD(restoreWhenDone, Meta::Tooltip("When it ends, put back everything it changed (transforms, the gameplay camera)"))
ZX_REFLECT_STRUCT_END()

namespace {

struct TrackName
{
    SeqTrackType type;
    const char* name;
};
constexpr TrackName TrackNames[] = {
    {SeqTrackType::Transform, "Transform"}, {SeqTrackType::Animation, "Animation"}, {SeqTrackType::Event, "Event"},
    {SeqTrackType::Camera, "Camera"},       {SeqTrackType::Audio, "Audio"},
};

struct InterpName
{
    SeqInterp interp;
    const char* name;
};
constexpr InterpName InterpNames[] = {
    {SeqInterp::Linear, "Linear"}, {SeqInterp::Smooth, "Smooth"}, {SeqInterp::Step, "Step"}, {SeqInterp::Spline, "Spline"},
};

constexpr f32 TimeEpsilon = 1e-5f;

JsonValue VecJson(Vec3 v)
{
    JsonValue a = JsonValue::MakeArray();
    a.PushBack(JsonValue(v.x));
    a.PushBack(JsonValue(v.y));
    a.PushBack(JsonValue(v.z));
    return a;
}

void ReadVec(const JsonValue& j, Vec3& out)
{
    const Span<const JsonValue> e = j.Elements();
    if (e.Size() == 3)
        out = Vec3(e[0].AsFloat(), e[1].AsFloat(), e[2].AsFloat());
}

f32 Smooth01(f32 x) { return x * x * (3.0f - 2.0f * x); }

Transform KeyTransform(const SeqKey& k) { return Transform(k.position, Quat::FromEulerDegrees(k.rotation), k.scale); }

/// Catmull-Rom through p1 -> p2 (p0 / p3 are the neighbours).
Vec3 CatmullRom(Vec3 p0, Vec3 p1, Vec3 p2, Vec3 p3, f32 u)
{
    const f32 u2 = u * u, u3 = u2 * u;
    return (p1 * 2.0f + (p2 - p0) * u + (p0 * 2.0f - p1 * 5.0f + p2 * 4.0f - p3) * u2 + (p1 * 3.0f - p0 - p2 * 3.0f + p3) * u3) * 0.5f;
}

void StopVoices(AudioSystem* audio, SequenceSession& session)
{
    if (audio)
        for (audio::VoiceId v : session.voices)
            if (audio->Engine().IsPlaying(v))
                audio->Engine().Stop(v, 0.05f);
    session.voices.Clear();
}

/// The first Animator on `e` or below it (a model is usually a child of the entity that is named).
ecs::Entity FindAnimator(ecs::World& world, ecs::Entity e)
{
    Array<ecs::Entity> queue;
    queue.PushBack(e);
    for (usize q = 0; q < queue.Size() && q < 256; ++q) {
        if (world.Has<Animator>(queue[q]))
            return queue[q];
        queue.Append(scene::GetChildren(world, queue[q]));
    }
    return {};
}

} // namespace

const char* SeqTrackTypeName(SeqTrackType t)
{
    for (const TrackName& n : TrackNames)
        if (n.type == t)
            return n.name;
    return "Transform";
}

bool SeqTrackTypeFromName(StringView name, SeqTrackType& out)
{
    for (const TrackName& n : TrackNames)
        if (name.EqualsIgnoreCase(n.name)) {
            out = n.type;
            return true;
        }
    return false;
}

const char* SeqInterpName(SeqInterp i)
{
    for (const InterpName& n : InterpNames)
        if (n.interp == i)
            return n.name;
    return "Smooth";
}

bool SeqInterpFromName(StringView name, SeqInterp& out)
{
    for (const InterpName& n : InterpNames)
        if (name.EqualsIgnoreCase(n.name)) {
            out = n.interp;
            return true;
        }
    return false;
}

void SortSeqKeys(SeqTrack& track)
{
    StableSort(track.keys, [](const SeqKey& a, const SeqKey& b) { return a.time < b.time; });
}

bool ParseSequence(StringView json, SequenceAsset& out, String* error)
{
    JsonValue root;
    JsonParseError pe;
    if (!ParseJson(json, root, &pe)) {
        if (error)
            *error = String::Format("line {}: {}", pe.line, pe.message);
        return false;
    }
    if (root.Get("format").AsString() != "zseq") {
        if (error)
            *error = "not a .zseq file";
        return false;
    }
    out = SequenceAsset();
    out.name = String(root.Get("name").AsString("Sequence"));
    out.duration = Max(root.Get("duration").AsFloat(5.0f), 0.01f);
    for (const JsonValue& j : root.Get("tracks").Elements()) {
        SeqTrack t;
        if (!SeqTrackTypeFromName(j.Get("type").AsString(), t.type)) {
            if (error)
                *error = String::Format("track {}: unknown type '{}'", out.tracks.Size(), j.Get("type").AsString());
            return false;
        }
        t.name = String(j.Get("name").AsString());
        t.target = String(j.Get("target").AsString());
        t.muted = j.Get("muted").AsBool(false);
        for (const JsonValue& kj : j.Get("keys").Elements()) {
            SeqKey k;
            k.time = Max(kj.Get("time").AsFloat(0.0f), 0.0f);
            ReadVec(kj.Get("pos"), k.position);
            ReadVec(kj.Get("rot"), k.rotation);
            ReadVec(kj.Get("scale"), k.scale);
            if (const JsonValue* v = kj.Find("interp"))
                SeqInterpFromName(v->AsString(), k.interp);
            k.text = String(kj.Get("text").AsString());
            k.speed = kj.Get("speed").AsFloat(1.0f);
            k.length = Max(kj.Get("length").AsFloat(0.0f), 0.0f);
            k.loop = kj.Get("loop").AsBool(false);
            k.volume = kj.Get("volume").AsFloat(1.0f);
            k.spatial = kj.Get("spatial").AsBool(false);
            k.fov = kj.Get("fov").AsFloat(0.0f);
            t.keys.PushBack(move(k));
        }
        SortSeqKeys(t);
        out.tracks.PushBack(move(t));
    }
    return true;
}

String WriteSequence(const SequenceAsset& seq)
{
    JsonValue root = JsonValue::MakeObject();
    root.Set("format", JsonValue("zseq"));
    root.Set("name", JsonValue(seq.name.View()));
    root.Set("duration", JsonValue(seq.duration));
    JsonValue tracks = JsonValue::MakeArray();
    for (const SeqTrack& t : seq.tracks) {
        JsonValue j = JsonValue::MakeObject();
        j.Set("type", JsonValue(SeqTrackTypeName(t.type)));
        if (!t.name.Empty())
            j.Set("name", JsonValue(t.name.View()));
        if (!t.target.Empty())
            j.Set("target", JsonValue(t.target.View()));
        if (t.muted)
            j.Set("muted", JsonValue(true));
        JsonValue keys = JsonValue::MakeArray();
        for (const SeqKey& k : t.keys) {
            JsonValue kj = JsonValue::MakeObject();
            kj.Set("time", JsonValue(k.time));
            switch (t.type) {
            case SeqTrackType::Transform:
                kj.Set("pos", VecJson(k.position));
                kj.Set("rot", VecJson(k.rotation));
                kj.Set("scale", VecJson(k.scale));
                kj.Set("interp", JsonValue(SeqInterpName(k.interp)));
                break;
            case SeqTrackType::Animation:
                kj.Set("text", JsonValue(k.text.View()));
                kj.Set("speed", JsonValue(k.speed));
                kj.Set("length", JsonValue(k.length));
                kj.Set("loop", JsonValue(k.loop));
                break;
            case SeqTrackType::Event: kj.Set("text", JsonValue(k.text.View())); break;
            case SeqTrackType::Camera:
                kj.Set("text", JsonValue(k.text.View()));
                if (k.fov > 0.0f)
                    kj.Set("fov", JsonValue(k.fov));
                break;
            case SeqTrackType::Audio:
                kj.Set("text", JsonValue(k.text.View()));
                kj.Set("volume", JsonValue(k.volume));
                kj.Set("spatial", JsonValue(k.spatial));
                break;
            case SeqTrackType::Count: break;
            }
            keys.PushBack(move(kj));
        }
        j.Set("keys", move(keys));
        tracks.PushBack(move(j));
    }
    root.Set("tracks", move(tracks));
    return ToJsonString(root);
}

void CheckSequence(const SequenceAsset& seq, Array<String>& problems)
{
    for (usize ti = 0; ti < seq.tracks.Size(); ++ti) {
        const SeqTrack& t = seq.tracks[ti];
        const String label = t.name.Empty() ? String::Format("{} {}", SeqTrackTypeName(t.type), t.target.Empty() ? String("(self)") : t.target)
                                            : t.name;
        if (t.keys.Empty()) {
            problems.PushBack(String::Format("Track '{}' has no keys", label));
            continue;
        }
        for (usize i = 0; i < t.keys.Size(); ++i) {
            const SeqKey& k = t.keys[i];
            if (k.time > seq.duration + TimeEpsilon)
                problems.PushBack(String::Format("Track '{}': the key at {:.2f} s is after the end ({:.2f} s)", label, k.time, seq.duration));
            if (i > 0 && Abs(k.time - t.keys[i - 1].time) < TimeEpsilon && t.type != SeqTrackType::Event && t.type != SeqTrackType::Audio)
                problems.PushBack(String::Format("Track '{}': two keys at {:.2f} s (the later one wins)", label, k.time));
            if ((t.type == SeqTrackType::Animation || t.type == SeqTrackType::Event || t.type == SeqTrackType::Audio) && k.text.Empty())
                problems.PushBack(String::Format("Track '{}': the key at {:.2f} s has no {}", label, k.time,
                                                 t.type == SeqTrackType::Animation ? "clip" : t.type == SeqTrackType::Event ? "event name" : "sound"));
        }
        if (t.type == SeqTrackType::Camera && t.keys[0].text.Empty())
            problems.PushBack(String::Format("Track '{}': the first key releases the camera - nothing is cut to before it", label));
    }
}

Transform SampleTransformTrack(const SeqTrack& track, f32 time)
{
    const Array<SeqKey>& k = track.keys;
    if (k.Empty())
        return Transform();
    if (time <= k[0].time)
        return KeyTransform(k[0]);
    usize i = 0;
    while (i + 1 < k.Size() && k[i + 1].time <= time)
        ++i;
    if (i + 1 >= k.Size())
        return KeyTransform(k[i]);
    const SeqKey& a = k[i];
    const SeqKey& b = k[i + 1];
    if (a.interp == SeqInterp::Step)
        return KeyTransform(a);
    const f32 span = b.time - a.time;
    const f32 u = span > 1e-6f ? Saturate((time - a.time) / span) : 1.0f;
    const f32 e = a.interp == SeqInterp::Smooth ? Smooth01(u) : u;
    Transform r;
    if (a.interp == SeqInterp::Spline) {
        const Vec3 p0 = i > 0 ? k[i - 1].position : a.position * 2.0f - b.position;
        const Vec3 p3 = i + 2 < k.Size() ? k[i + 2].position : b.position * 2.0f - a.position;
        r.position = CatmullRom(p0, a.position, b.position, p3, u);
    } else {
        r.position = Lerp(a.position, b.position, e);
    }
    r.rotation = Slerp(Quat::FromEulerDegrees(a.rotation), Quat::FromEulerDegrees(b.rotation), e);
    r.scale = Lerp(a.scale, b.scale, e);
    return r;
}

const SeqKey* ActiveSeqKey(const SeqTrack& track, f32 time)
{
    const SeqKey* found = nullptr;
    for (const SeqKey& k : track.keys) {
        if (k.time > time + TimeEpsilon)
            break;
        found = &k;
    }
    return found;
}

SequenceAsset MakeCameraFlyover(StringView camera, Vec3 from, Vec3 to, f32 yawDeg, f32 pitchDeg, f32 duration, bool releaseAtEnd)
{
    SequenceAsset s;
    s.name = "Camera Flyover";
    s.duration = Max(duration, 0.1f);
    SeqTrack cut;
    cut.type = SeqTrackType::Camera;
    cut.name = "Camera Cuts";
    SeqKey on;
    on.text = String(camera);
    cut.keys.PushBack(on);
    if (releaseAtEnd) {
        SeqKey off;
        off.time = s.duration;
        cut.keys.PushBack(off);
    }
    s.tracks.PushBack(move(cut));
    SeqTrack move_;
    move_.type = SeqTrackType::Transform;
    move_.target = String(camera);
    SeqKey a;
    a.time = 0.0f;
    a.position = from;
    a.rotation = Vec3(pitchDeg, yawDeg, 0.0f);
    a.interp = SeqInterp::Smooth;
    SeqKey b = a;
    b.time = s.duration;
    b.position = to;
    move_.keys.PushBack(a);
    move_.keys.PushBack(b);
    s.tracks.PushBack(move(move_));
    return s;
}

// ---------------------------------------------------------------------------------------------------------------------

const SequenceAsset* SequenceSystem::Load(StringView path)
{
    if (File* f = m_files.Find(String(path)))
        return f->ok ? &f->seq : nullptr;
    File& f = m_files.InsertOrAssign(String(path), File{});
    String text, error;
    const String full = contentRoot.Empty() ? String(path) : Path::Join(contentRoot, path);
    if (!fs::ReadTextFile(full, text))
        ZX_LOG_WARN(LogSequencer, "Sequence not found: {}", full);
    else if (!ParseSequence(text.View(), f.seq, &error))
        ZX_LOG_ERROR(LogSequencer, "{}: {}", path, error);
    else
        f.ok = true;
    return f.ok ? &f.seq : nullptr;
}

void SequenceSystem::Reload(StringView path)
{
    if (path.Empty())
        m_files.Clear();
    else
        m_files.Remove(String(path));
}

ecs::Entity SequenceSystem::Resolve(ecs::World& world, SequenceSession& session, StringView name, ecs::Entity self)
{
    if (name.Empty() || name == "@self")
        return world.IsAlive(self) ? self : ecs::Entity();
    const String key(name);
    if (const ecs::Entity* cached = session.names.Find(key); cached && world.IsAlive(*cached))
        return *cached;
    if (const u32* until = session.missUntil.Find(key); until && session.frame < *until)
        return {};
    ecs::Entity current;
    StringView rest = name;
    if (rest.StartsWith("@self/")) {
        current = self;
        rest = rest.DropLeft(6);
    }
    bool first = !current;
    while (!rest.Empty()) {
        StringView segment = rest, tail;
        if (!rest.Split('/', segment, tail))
            tail = StringView();
        rest = tail;
        if (segment.Empty())
            continue;
        ecs::Entity next;
        if (first) {
            world.Query<const scene::EntityName>().Each([&](ecs::Entity e, const scene::EntityName& n) {
                if (!next && n.value.View() == segment)
                    next = e;
            });
        } else {
            for (ecs::Entity child : scene::GetChildren(world, current))
                if (const scene::EntityName* n = world.Get<const scene::EntityName>(child); n && n->value.View() == segment) {
                    next = child;
                    break;
                }
        }
        first = false;
        current = next;
        if (!current)
            break;
    }
    if (current) {
        session.names.InsertOrAssign(key, current);
        session.missUntil.Remove(key);
    } else {
        session.missUntil.InsertOrAssign(key, session.frame + 15u);
    }
    return current;
}

void SequenceSystem::Apply(ecs::World& world, const SequenceAsset& seq, ecs::Entity self, f32 time, SequenceSession& session)
{
    ++session.frame;
    ecs::Entity wantedCamera;
    const SeqKey* cameraKey = nullptr;
    f32 cameraKeyTime = -1.0f;
    bool cameraTrackSeen = false;
    for (const SeqTrack& track : seq.tracks) {
        if (track.muted || track.keys.Empty())
            continue;
        switch (track.type) {
        case SeqTrackType::Transform: {
            const ecs::Entity target = Resolve(world, session, track.target.View(), self);
            Transform* t = target ? world.Get<Transform>(target) : nullptr;
            if (!t)
                break;
            bool saved = false;
            for (const SequenceSession::SavedTransform& s : session.transforms)
                saved |= s.entity == target;
            if (!saved)
                session.transforms.PushBack({target, *t});
            *t = SampleTransformTrack(track, time);
            break;
        }
        case SeqTrackType::Animation: {
            const SeqKey* key = ActiveSeqKey(track, time);
            if (!key || key->text.Empty() || !animation)
                break;
            const ecs::Entity target = Resolve(world, session, track.target.View(), self);
            const ecs::Entity animator = target ? FindAnimator(world, target) : ecs::Entity();
            if (!animator)
                break;
            f32 local = Max(time - key->time, 0.0f);
            if (key->length > 0.0f)
                local = Min(local, key->length);
            bool saved = false;
            for (const SequenceSession::SavedAnimator& s : session.animators)
                saved |= s.entity == animator;
            if (!saved)
                if (const Animator* a = world.Get<const Animator>(animator))
                    session.animators.PushBack({animator, a->clip, a->playing, a->loop, a->previewInEditor});
            animation->Scrub(world, animator, key->text.View(), local * key->speed, key->loop);
            break;
        }
        case SeqTrackType::Camera: {
            cameraTrackSeen = true;
            const SeqKey* key = ActiveSeqKey(track, time);
            if (key && key->time >= cameraKeyTime) { // the latest cut among the camera tracks wins
                cameraKeyTime = key->time;
                cameraKey = key;
            }
            break;
        }
        default: break; // Event / Audio keys fire in Fire()
        }
    }
    if (!cameraTrackSeen)
        return;
    if (cameraKey && !cameraKey->text.Empty()) {
        const ecs::Entity cam = Resolve(world, session, cameraKey->text.View(), self);
        if (cam && world.Has<Camera>(cam))
            wantedCamera = cam;
    }
    if (wantedCamera) {
        if (!session.camerasTaken) {
            session.cameras.Clear();
            world.Query<const Camera>().Each([&](ecs::Entity e, const Camera& c) { session.cameras.PushBack({e, c.active, c.fieldOfView}); });
            session.camerasTaken = true;
        }
        for (const SequenceSession::SavedCamera& s : session.cameras) {
            Camera* c = world.IsAlive(s.entity) ? world.Get<Camera>(s.entity) : nullptr;
            if (!c)
                continue;
            const bool on = s.entity == wantedCamera;
            c->active = on;
            c->fieldOfView = on && cameraKey->fov > 0.0f ? cameraKey->fov : s.fov;
        }
    } else if (session.camerasTaken) {
        for (const SequenceSession::SavedCamera& s : session.cameras)
            if (Camera* c = world.IsAlive(s.entity) ? world.Get<Camera>(s.entity) : nullptr) {
                c->active = s.active;
                c->fieldOfView = s.fov;
            }
        session.camerasTaken = false;
    }
}

void SequenceSystem::Fire(ecs::World& world, const SequenceAsset& seq, ecs::Entity self, f32 from, f32 to, SequenceSession& session)
{
    if (to < from)
        return;
    for (const SeqTrack& track : seq.tracks) {
        if (track.muted || (track.type != SeqTrackType::Event && track.type != SeqTrackType::Audio))
            continue;
        for (const SeqKey& k : track.keys) {
            if (!(k.time > from && k.time <= to) || k.text.Empty())
                continue;
            const ecs::Entity target = Resolve(world, session, track.target.View(), self);
            if (track.type == SeqTrackType::Event) {
                if (target)
                    m_events.PushBack(Event{target, k.text});
            } else if (audio) {
                Vec3 at;
                if (k.spatial)
                    if (const scene::WorldTransform* wt = target ? world.Get<const scene::WorldTransform>(target) : nullptr)
                        at = wt->GetPosition();
                const audio::VoiceId voice = audio->PlayOneShot(k.text.View(), k.volume, k.spatial, at);
                if (voice) {
                    session.voices.RemoveIf([&](audio::VoiceId v) { return !audio->Engine().IsPlaying(v); });
                    session.voices.PushBack(voice);
                }
            }
        }
    }
}

void SequenceSystem::Restore(ecs::World& world, SequenceSession& session)
{
    for (const SequenceSession::SavedTransform& s : session.transforms)
        if (Transform* t = world.IsAlive(s.entity) ? world.Get<Transform>(s.entity) : nullptr)
            *t = s.transform;
    session.transforms.Clear();
    for (const SequenceSession::SavedAnimator& s : session.animators)
        if (Animator* a = world.IsAlive(s.entity) ? world.Get<Animator>(s.entity) : nullptr) {
            a->clip = s.clip;
            a->playing = s.playing;
            a->loop = s.loop;
            a->previewInEditor = s.previewInEditor;
        }
    session.animators.Clear();
    for (const SequenceSession::SavedCamera& s : session.cameras)
        if (Camera* c = world.IsAlive(s.entity) ? world.Get<Camera>(s.entity) : nullptr) {
            c->active = s.active;
            c->fieldOfView = s.fov;
        }
    session.cameras.Clear();
    session.camerasTaken = false;
    StopVoices(audio, session);
    session.names.Clear();
    session.missUntil.Clear();
}

// --- Players ---------------------------------------------------------------------------------------------------------

void SequenceSystem::Begin(ecs::World& world)
{
    Clear();
    Array<ecs::Entity> starters;
    world.Query<const SequencePlayer>().Each([&](ecs::Entity e, const SequencePlayer& p) {
        if (p.playOnStart && !p.sequence.Empty())
            starters.PushBack(e);
    });
    for (ecs::Entity e : starters)
        Play(world, e, 0.0f);
}

void SequenceSystem::Clear()
{
    for (auto& kv : m_runs)
        StopVoices(audio, kv.second.session);
    m_runs.Clear();
    m_events.Clear();
}

void SequenceSystem::End(ecs::World& world)
{
    for (auto& kv : m_runs)
        Restore(world, kv.second.session);
    m_runs.Clear();
    m_events.Clear();
}

bool SequenceSystem::Play(ecs::World& world, ecs::Entity e, f32 from)
{
    const SequencePlayer* p = world.IsAlive(e) ? world.Get<const SequencePlayer>(e) : nullptr;
    if (!p || p->sequence.Empty())
        return false;
    const SequenceAsset* seq = Load(p->sequence.View());
    if (!seq)
        return false;
    if (Run* old = m_runs.Find(e.Pack()))
        Restore(world, old->session);
    Run& r = m_runs.InsertOrAssign(e.Pack(), Run{});
    r.time = Clamp(from, 0.0f, seq->duration);
    r.prev = r.time - 1e-4f;
    r.playing = true;
    return true;
}

void SequenceSystem::Stop(ecs::World& world, ecs::Entity e)
{
    if (Run* r = m_runs.Find(e.Pack())) {
        Restore(world, r->session);
        m_runs.Remove(e.Pack());
    }
}

void SequenceSystem::SetPaused(ecs::Entity e, bool paused)
{
    if (Run* r = m_runs.Find(e.Pack()))
        r->paused = paused;
}

f32 SequenceSystem::Time(ecs::Entity e) const
{
    const Run* r = m_runs.Find(e.Pack());
    return r ? r->time : 0.0f;
}

bool SequenceSystem::IsPlaying(ecs::Entity e) const
{
    const Run* r = m_runs.Find(e.Pack());
    return r && r->playing;
}

void SequenceSystem::Tick(ecs::World& world, f32 dt)
{
    m_events.Clear();
    Array<u64> keys;
    for (auto& kv : m_runs)
        keys.PushBack(kv.first);
    for (u64 key : keys) {
        Run* run = m_runs.Find(key);
        if (!run)
            continue;
        const ecs::Entity e = ecs::Entity::Unpack(key);
        const SequencePlayer* p = world.IsAlive(e) ? world.Get<const SequencePlayer>(e) : nullptr;
        if (!p) { // the entity is gone (or lost its component): nothing left to pose
            if (world.IsAlive(e))
                Restore(world, run->session);
            else
                StopVoices(audio, run->session);
            m_runs.Remove(key);
            continue;
        }
        if (!run->playing || run->paused)
            continue;
        const SequenceAsset* seq = Load(p->sequence.View());
        if (!seq) {
            run->playing = false;
            continue;
        }
        const f32 duration = Max(seq->duration, 1e-3f);
        f32 next = run->time + dt * Max(p->speed, 0.0f);
        bool finished = false;
        if (next >= duration) {
            if (p->loop) {
                Fire(world, *seq, e, run->prev, duration, run->session);
                next = Fmod(next, duration);
                run->prev = -1e-4f;
            } else {
                next = duration;
                finished = true;
            }
        }
        Fire(world, *seq, e, run->prev, next, run->session);
        run->time = next;
        run->prev = next;
        Apply(world, *seq, e, next, run->session);
        if (finished) {
            run->playing = false;
            if (p->restoreWhenDone)
                Restore(world, run->session);
        }
    }
}

} // namespace zx
