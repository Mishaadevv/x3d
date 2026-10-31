#pragma once

// Sequencer (M18): cinematics and scripted scene changes as a timeline.
//
//   .zseq file (JSON, Content-relative, hot-reloaded): a duration and tracks; every track has a target entity (by name,
//   "Parent/Child" paths work, empty or "@self" = the entity with the Sequence Player) and keys sorted by time.
//     Transform  keys: a local position / rotation (pitch, yaw, roll degrees) / scale; the segment after a key is
//                interpolated Linear, Smooth (ease in-out), Step (hold) or Spline (Catmull-Rom through the positions).
//     Animation  keys: start a clip (by name, from the Animator on the target or its children) at `time`, `speed`, `loop`,
//                `length` (0 = until the next key). The pose is the clip at that moment of the block, so scrubbing works.
//     Event      keys: `text` is passed to fn onSequenceEvent(name) in the scripts of the target (and its ancestors).
//     Camera     keys: cut to the Camera entity named `text` (optionally setting its `fov`); an empty `text` gives the
//                view back to the camera that was active before the sequence took over.
//     Audio      keys: play the sound `text` (Content-relative) at `volume`; `spatial` plays it at the target's position.
//
//   Sequence Player component: runs a .zseq in the game (Play On Start, loop, speed). Scripts: entity.sequencePlay([from]),
//   sequenceStop(), sequencePause(paused), sequenceTime(), sequencePlaying().
//
//   SequenceSystem::Apply poses the scene at a time (the editor's timeline uses it to scrub and preview); everything it
//   changes is remembered in a SequenceSession so Restore puts the scene back as it was.

#include "Core/Containers/Array.h"
#include "Core/Containers/HashMap.h"
#include "Core/Math/Math.h"
#include "Core/Math/Quat.h"
#include "Core/Reflection/Reflection.h"
#include "Core/String/String.h"
#include "Engine/Animation.h"
#include "Engine/AudioSystem.h"
#include "Scene/ECS/World.h"

namespace zx {

enum class SeqTrackType : u8
{
    Transform,
    Animation,
    Event,
    Camera,
    Audio,
    Count
};

enum class SeqInterp : u8
{
    Linear,
    Smooth,
    Step,
    Spline
};

const char* SeqTrackTypeName(SeqTrackType t);
bool SeqTrackTypeFromName(StringView name, SeqTrackType& out);
const char* SeqInterpName(SeqInterp i);
bool SeqInterpFromName(StringView name, SeqInterp& out);

/// One key. Which fields matter depends on the track type (see the header comment).
struct SeqKey
{
    f32 time = 0.0f;            // seconds
    // Transform
    Vec3 position;
    Vec3 rotation;              // pitch, yaw, roll in degrees
    Vec3 scale = Vec3(1.0f);
    SeqInterp interp = SeqInterp::Smooth;
    // Animation: clip name; Event: name; Camera: camera entity name (empty = release); Audio: Content-relative file
    String text;
    f32 speed = 1.0f;           // Animation: playback rate
    f32 length = 0.0f;          // Animation: seconds this clip block lasts (0 = until the next key)
    bool loop = false;          // Animation
    f32 volume = 1.0f;          // Audio
    bool spatial = false;       // Audio: at the target's position
    f32 fov = 0.0f;             // Camera: vertical field of view in degrees (0 = keep the camera's own)
};

struct SeqTrack
{
    SeqTrackType type = SeqTrackType::Transform;
    String name;                // shown in the editor (empty = the target)
    String target;              // entity name / path; empty or "@self" = the Sequence Player's entity
    bool muted = false;
    Array<SeqKey> keys;         // sorted by time
};

struct SequenceAsset
{
    String name;
    f32 duration = 5.0f;        // seconds
    Array<SeqTrack> tracks;
};

/// Keeps the keys of a track ordered by time (stable).
void SortSeqKeys(SeqTrack& track);
bool ParseSequence(StringView json, SequenceAsset& out, String* error = nullptr);
String WriteSequence(const SequenceAsset& seq);
/// Problems that make a track do nothing (no keys, empty clip / event / sound, two keys at one time), one line each.
void CheckSequence(const SequenceAsset& seq, Array<String>& problems);
/// The local transform a Transform track gives at `time` (before the first key / after the last: that key; an empty
/// track: identity).
Transform SampleTransformTrack(const SeqTrack& track, f32 time);
/// The last key at or before `time` (Animation / Camera blocks), null before the first one.
const SeqKey* ActiveSeqKey(const SeqTrack& track, f32 time);
/// A new sequence of `duration` seconds: a camera cut to `camera`, a Transform track flying it from `from` to `to`
/// looking along yaw / pitch (degrees), and (releaseAtEnd) a final cut back to the gameplay camera.
SequenceAsset MakeCameraFlyover(StringView camera, Vec3 from, Vec3 to, f32 yawDeg, f32 pitchDeg, f32 duration, bool releaseAtEnd = true);

/// Runs a .zseq on an entity of the scene (the entity itself is "@self").
struct SequencePlayer
{
    String sequence;            // .zseq, Content-relative
    bool playOnStart = true;
    bool loop = false;
    f32 speed = 1.0f;
    bool restoreWhenDone = false; // put everything the sequence changed back when it ends (cameras: the gameplay view)
};
ZX_DECLARE_TYPE(SequencePlayer);

/// What a run changed in the scene, so that it can be undone (and the targets it found).
struct SequenceSession
{
    struct SavedTransform
    {
        ecs::Entity entity;
        Transform transform;
    };
    struct SavedCamera
    {
        ecs::Entity entity;
        bool active = false;
        f32 fov = 70.0f;
    };
    struct SavedAnimator
    {
        ecs::Entity entity;
        asset::AssetId clip;
        bool playing = true;
        bool loop = true;
        bool previewInEditor = false;
    };
    Array<SavedTransform> transforms;
    Array<SavedAnimator> animators;
    Array<SavedCamera> cameras;
    bool camerasTaken = false;               // a camera cut is in effect (the other cameras are switched off)
    Array<audio::VoiceId> voices;
    HashMap<String, ecs::Entity> names;      // targets found by name
    HashMap<String, u32> missUntil;          // names not found: look again after this frame
    u32 frame = 0;
};

class SequenceSystem
{
public:
    String contentRoot;                      // SequencePlayer::sequence paths are relative to it
    AudioSystem* audio = nullptr;            // Audio keys (null: silent)
    AnimationSystem* animation = nullptr;    // Animation keys (null: no clips)

    /// A sequence file (cached; null if missing or broken).
    const SequenceAsset* Load(StringView path);
    /// Drops cached files (a sequence was saved); they reload on the next use. Empty path = all.
    void Reload(StringView path = {});

    // --- Players (Sequence Player components) ---
    /// Play starts fresh: every run is forgotten (the scene is not touched).
    void Begin(ecs::World& world);
    /// Advances every playing Sequence Player by `dt` and poses the scene.
    void Tick(ecs::World& world, f32 dt);
    /// Puts back everything the players changed and forgets them.
    void End(ecs::World& world);
    /// Forgets the runs without touching the scene (the host restores it, e.g. the editor reloads its snapshot).
    void Clear();
    /// Starts the player of `e` at `from` seconds. False without a Sequence Player / sequence.
    bool Play(ecs::World& world, ecs::Entity e, f32 from = 0.0f);
    /// Stops it and puts back what it changed.
    void Stop(ecs::World& world, ecs::Entity e);
    void SetPaused(ecs::Entity e, bool paused);
    f32 Time(ecs::Entity e) const;
    bool IsPlaying(ecs::Entity e) const;

    /// Events fired this tick (receiver, name); the host passes them to the scripts.
    struct Event
    {
        ecs::Entity entity;
        String name;
    };
    Span<const Event> Events() const { return m_events.View(); }
    void ClearEvents() { m_events.Clear(); }

    // --- Evaluation (players and the editor's timeline) ---
    /// Poses the scene at `time` (transforms, animation clips, camera cuts) and remembers the originals in `session`.
    void Apply(ecs::World& world, const SequenceAsset& seq, ecs::Entity self, f32 time, SequenceSession& session);
    /// Fires the Event and Audio keys with from < time <= to (use a `from` just below 0 to include a key at 0).
    void Fire(ecs::World& world, const SequenceAsset& seq, ecs::Entity self, f32 from, f32 to, SequenceSession& session);
    /// Undoes what `session` changed: transforms, cameras, running sounds.
    void Restore(ecs::World& world, SequenceSession& session);
    /// The entity a track target names (see the header comment); null if there is none.
    ecs::Entity Resolve(ecs::World& world, SequenceSession& session, StringView name, ecs::Entity self);

private:
    struct File
    {
        SequenceAsset seq;
        bool ok = false;
    };
    struct Run
    {
        f32 time = 0.0f;
        f32 prev = 0.0f;            // the time of the last tick (events fire for keys after it)
        bool started = false;
        bool playing = false;
        bool paused = false;
        SequenceSession session;
    };
    HashMap<String, File> m_files;
    HashMap<u64, Run> m_runs;
    Array<Event> m_events;
};

} // namespace zx
