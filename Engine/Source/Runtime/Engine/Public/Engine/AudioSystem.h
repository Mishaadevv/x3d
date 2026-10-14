#pragma once

// Sound in scenes: AudioSource / AudioListener / ReverbZone components and the system that plays them while the game
// runs (on top of audio::AudioEngine).
//
//   AudioSystem audio(engine);
//   audio.SetContentRoot(projectContentDir);      // AudioSource::clip is relative to it
//   audio.Begin(world);                           // Play: sources with playOnStart begin
//   audio.Tick(world, dt, fallbackListener);      // every frame: positions, velocities, occlusion, reverb zones
//   audio.End();                                  // Stop

#include "Audio/AudioEngine.h"
#include "Core/Containers/Function.h"
#include "Core/Containers/HashMap.h"
#include "Core/Math/Math.h"
#include "Core/Math/Random.h"
#include "Core/Reflection/Reflection.h"
#include "Scene/ECS/World.h"

namespace zx {

namespace audio {
ZX_DECLARE_TYPE(Bus);
ZX_DECLARE_TYPE(Rolloff);
} // namespace audio

/// Plays a sound file (WAV / Ogg Vorbis, path relative to the project's Content folder) from the entity.
struct AudioSource
{
    String clip;
    f32 volume = 1.0f;
    f32 pitch = 1.0f;
    bool loop = true;
    bool playOnStart = true;
    audio::Bus bus = audio::Bus::SFX;
    bool spatial = true;       // 3D: attenuated and panned from the entity's position
    f32 minDistance = 1.0f;    // meters at full volume
    f32 maxDistance = 40.0f;
    audio::Rolloff rolloff = audio::Rolloff::Inverse;
    i32 priority = 0;
    f32 dopplerScale = 1.0f;
    f32 spread = 0.0f;
    f32 coneInner = 360.0f;    // degrees around the entity's forward axis
    f32 coneOuter = 360.0f;
    f32 coneOuterGain = 0.0f;
    bool occlusion = false;    // muffled when geometry is between the source and the listener
    f32 reverbSend = 0.0f;
};

/// The ears of the game (the first active one); without one the camera is used.
struct AudioListener
{
    bool active = true;
};

/// Reverberation around a point: the listener inside `radius` hears the zone's reverb (blended over `fade` meters).
struct ReverbZone
{
    f32 radius = 10.0f;
    f32 fade = 3.0f;
    f32 wet = 0.35f;
    f32 roomSize = 0.7f;
    f32 damping = 0.5f;
};

/// The sound of a Rigid Body hitting things, louder the harder the hit. `sound` is a file or a set of variants picked
/// at random: "Impacts/Impact_Wood" plays Impact_Wood_0 .. Impact_Wood_9 (.ogg / .wav).
struct ImpactSound
{
    String sound;
    f32 volume = 1.0f;
    f32 minSpeed = 0.8f;        // m/s: softer hits are silent
    f32 maxSpeed = 8.0f;        // m/s: full volume
    f32 pitchVariation = 0.08f; // random pitch change, fraction
    f32 maxDistance = 30.0f;    // meters
};

struct PhysicsEvent;

ZX_DECLARE_TYPE(AudioSource);
ZX_DECLARE_TYPE(ImpactSound);
ZX_DECLARE_TYPE(AudioListener);
ZX_DECLARE_TYPE(ReverbZone);

struct ListenerPose
{
    Vec3 position;
    Vec3 forward = Vec3(0, 0, 1);
    Vec3 up = Vec3(0, 1, 0);
};

class AudioSystem
{
public:
    explicit AudioSystem(audio::AudioEngine& engine);
    ~AudioSystem();
    ZX_NON_COPYABLE(AudioSystem)

    void SetContentRoot(StringView physicalDir);
    /// A clip by content-relative path (cached; reloaded when the file changes). Null + error on failure.
    RefPtr<audio::AudioClip> LoadClip(StringView file, String* error = nullptr);
    void ClearCache();

    void Begin(ecs::World& world);
    void Tick(ecs::World& world, f32 dt, const ListenerPose& fallback);
    void End();
    bool IsRunning() const { return m_running; }
    /// Outside of Play (editor previews): just advances the engine.
    void TickIdle(f32 dt) { m_engine.Update(dt); }

    /// One-shot sounds (scripts, UI). 2D when `spatial` is false. A path without an extension is a set of variants
    /// (path_0 .. path_9): one is picked at random.
    audio::VoiceId PlayOneShot(StringView file, f32 volume = 1.0f, bool spatial = false, Vec3 position = Vec3(0.0f),
                               audio::Bus bus = audio::Bus::SFX);
    /// A random existing variant of a sound set: `base` itself if it names a file, else base_0 .. base_9 with .ogg /
    /// .wav (cached). Empty when there is none.
    String PickVariant(StringView base);
    /// Impact Sounds of the bodies in this frame's collisions (begin events), louder the faster they hit.
    void PlayImpacts(ecs::World& world, Span<const PhysicsEvent> events);
    /// Starts / stops the AudioSource of an entity while playing.
    bool PlayEntity(ecs::World& world, ecs::Entity e);
    void StopEntity(ecs::Entity e);

    /// Editor: plays a file once on the UI bus (calling again with the same file stops it).
    bool TogglePreview(StringView file);
    StringView PreviewFile() const { return m_engine.IsPlaying(m_preview) ? m_previewFile.View() : StringView(); }

    /// Returns 0..1 how blocked the path between two points is (set by the host, e.g. a mesh raycast).
    Function<f32(Vec3 from, Vec3 to), 32> occlusion;

    audio::AudioEngine& Engine() { return m_engine; }
    u32 PlayingSources() const { return u32(m_sources.Size()); }

private:
    struct CachedClip
    {
        RefPtr<audio::AudioClip> clip;
        i64 modifiedTime = 0;
        String error;
    };
    struct Running
    {
        ecs::Entity entity;
        audio::VoiceId voice = 0;
        Vec3 lastPosition;
        bool hasLast = false;
        f32 occlusion = 0.0f;
        u32 occlusionPhase = 0;
    };
    audio::VoiceParams ParamsFor(const AudioSource& s, const Mat4& world) const;

    audio::AudioEngine& m_engine;
    HashMap<String, Array<String>> m_variants;  // sound set -> existing files
    HashMap<u64, f64> m_lastImpact;             // entity -> time of its last impact sound
    f64 m_time = 0.0;
    Random m_rng;
    String m_contentRoot;
    HashMap<String, CachedClip> m_clips;
    Array<Running> m_sources;
    Array<ecs::Entity> m_started; // entities whose playOnStart already ran
    bool m_running = false;
    u32 m_frame = 0;
    audio::VoiceId m_preview = 0;
    String m_previewFile;
};

} // namespace zx
