#pragma once

// ZAudio: own float32 48 kHz stereo mixer on a high-priority thread, WASAPI output (shared mode, event-driven).
//
//   AudioEngine audio;
//   audio.Initialize({});                                   // or {.backend = AudioBackend::Null} for tests/offline
//   VoiceId v = audio.Play(clip, VoiceParams{.spatial = true, .position = p});
//   audio.SetListener(cameraPos, forward, up, velocity);
//   audio.Update(dt);                                        // game thread, once per frame: 3D, virtualization, ducking
//
// Voices are mixed into buses (Music, SFX, UI, Voice -> Master) with volume, mute and meters; a send feeds a shared
// reverb. The game thread computes gains/pan/pitch/filters from 3D data; the mixer ramps every parameter across a
// block so changes never click. Above `maxAudibleVoices` the quietest voices become virtual (they keep their place
// in the sound but are not mixed).

#include "Audio/AudioClip.h"
#include "Audio/Dsp.h"
#include "Core/Containers/Function.h"
#include "Core/Math/Vector.h"
#include "Core/Threading/Mutex.h"
#include "Core/Threading/Thread.h"

#include <atomic>

namespace zx::audio {

inline constexpr u32 MixRate = 48000;
inline constexpr u32 MixBlock = 256; // frames per mixer block

enum class Bus : u8
{
    Master,
    Music,
    SFX,
    UI,
    Voice,
    Count
};
const char* BusName(Bus bus);

enum class Rolloff : u8
{
    Inverse,     // natural: gain = min / distance
    Linear,      // straight line from min to max
    Logarithmic, // inverse squared, steeper
};

struct VoiceParams
{
    f32 volume = 1.0f;
    f32 pitch = 1.0f;
    bool loop = false;
    Bus bus = Bus::SFX;
    i32 priority = 0;  // higher stays audible longer when voices are limited
    f32 pan = 0.0f;    // 2D voices: -1 left .. 1 right
    // 3D
    bool spatial = false;
    Vec3 position;
    Vec3 velocity;
    Vec3 direction = Vec3(0, 0, 1); // cone axis
    f32 minDistance = 1.0f;          // full volume inside
    f32 maxDistance = 50.0f;         // silent (Linear) / no further attenuation beyond
    Rolloff rolloff = Rolloff::Inverse;
    f32 coneInner = 360.0f; // degrees
    f32 coneOuter = 360.0f;
    f32 coneOuterGain = 0.0f;
    f32 dopplerScale = 1.0f;
    f32 spread = 0.0f;      // 0 = point source, 1 = fills both speakers
    // Effects
    f32 occlusion = 0.0f;   // 0..1: low-pass + attenuation (set from raycasts)
    f32 reverbSend = 0.0f;  // 0..1 extra send on top of the listener's reverb zone
    f32 fadeIn = 0.0f;      // seconds
};

using VoiceId = u32; // 0 = invalid

struct AudioSettings
{
    enum class Backend : u8
    {
        Default, // WASAPI, falls back to Null without a device
        Null,    // no output; use RenderOffline (tests, servers)
    };
    Backend backend = Backend::Default;
    u32 maxVoices = 256;
    u32 maxAudibleVoices = 64;
    f32 bufferMs = 20.0f; // WASAPI buffer
};

/// Bus volumes etc. that can be blended to (e.g. "Paused": music down, SFX muted).
struct MixSnapshot
{
    f32 busVolume[u32(Bus::Count)] = {1, 1, 1, 1, 1};
    f32 reverbWet = 0.0f;
};

struct ReverbSettings
{
    f32 wet = 0.0f;      // listener's zone mix (0 = dry)
    f32 roomSize = 0.6f; // 0..1
    f32 damping = 0.5f;  // 0..1
};

struct AudioStats
{
    u32 voices = 0;      // playing
    u32 audible = 0;     // mixed this block
    u32 virtualVoices = 0;
    u32 streams = 0;
    u64 underruns = 0;   // device asked for audio the mixer could not deliver in time
    f32 mixCpuMs = 0.0f; // average per block
    bool deviceOpen = false;
    String deviceName;
};

class AudioEngine
{
public:
    AudioEngine();
    ~AudioEngine();
    ZX_NON_COPYABLE(AudioEngine)

    bool Initialize(const AudioSettings& settings = {});
    void Shutdown();
    bool IsInitialized() const { return m_initialized; }

    /// Starts a clip. Returns 0 when it could not start (no voice left).
    VoiceId Play(const RefPtr<AudioClip>& clip, const VoiceParams& params = {});
    void Stop(VoiceId voice, f32 fadeSeconds = 0.02f);
    void StopAll(f32 fadeSeconds = 0.05f);
    void SetPaused(VoiceId voice, bool paused);
    bool IsPlaying(VoiceId voice) const;
    /// Changes position / volume / ... of a playing voice (applied at the next Update).
    void SetParams(VoiceId voice, const VoiceParams& params);
    const VoiceParams* GetParams(VoiceId voice) const;
    /// Seconds played (loops wrap), as last reported by the mixer.
    f64 GetTime(VoiceId voice) const;

    void SetListener(Vec3 position, Vec3 forward, Vec3 up, Vec3 velocity = Vec3(0.0f));
    void SetBusVolume(Bus bus, f32 volume);
    f32 GetBusVolume(Bus bus) const { return m_busVolume[u32(bus)]; }
    void SetBusMuted(Bus bus, bool muted);
    bool IsBusMuted(Bus bus) const { return m_busMuted[u32(bus)]; }
    /// Peak level of a bus over the last blocks (0..1+), for meters.
    f32 GetBusPeak(Bus bus) const { return m_busPeak[u32(bus)].load(std::memory_order_relaxed); }
    /// Music ducks under the Voice bus by `amountDb` while dialogue plays.
    void SetDucking(bool enabled, f32 amountDb = -9.0f) { m_duckEnabled = enabled, m_duckDb = amountDb; }
    void SetReverb(const ReverbSettings& reverb) { m_reverbTarget = reverb; }
    /// Blends bus volumes (and reverb) to a snapshot over `seconds`.
    void ApplySnapshot(const MixSnapshot& snapshot, f32 seconds);
    void SetMasterVolume(f32 volume) { SetBusVolume(Bus::Master, volume); }

    /// Game thread, once per frame: 3D attenuation / panning / doppler, virtualization, ducking, snapshots.
    void Update(f32 dt);
    /// Null backend: mixes `frames` stereo frames into `out` (also usable to bounce audio).
    void RenderOffline(f32* out, u32 frames);

    AudioStats GetStats() const;

private:
    struct Voice;
    struct MixVoice;
    struct Command;
    void MixerThread();
    void Mix(f32* out, u32 frames);
    void MixOneBlock(f32* out, u32 frames);
    void ApplyCommands();
    bool OpenDevice();
    void CloseDevice();

    AudioSettings m_settings;
    bool m_initialized = false;
    // Game-thread state.
    Array<Voice> m_voices;
    u32 m_nextId = 1;
    Vec3 m_listenerPos, m_listenerForward = Vec3(0, 0, 1), m_listenerUp = Vec3(0, 1, 0), m_listenerVelocity;
    f32 m_busVolume[u32(Bus::Count)] = {1, 1, 1, 1, 1};
    bool m_busMuted[u32(Bus::Count)] = {};
    MixSnapshot m_snapFrom, m_snapTo;
    f32 m_snapTime = 0.0f, m_snapDuration = 0.0f;
    bool m_duckEnabled = true;
    f32 m_duckDb = -9.0f;
    f32 m_duckGain = 1.0f;
    ReverbSettings m_reverbTarget, m_reverbCurrent;
    // Shared with the mixer.
    Mutex m_commandLock;
    Array<Command> m_commands;      // game -> mixer
    Array<Command> m_mixerCommands; // swapped in on the mixer thread
    std::atomic<f32> m_busPeak[u32(Bus::Count)] = {};
    std::atomic<f32> m_voiceBusRms{0.0f};
    std::atomic<u32> m_audible{0}, m_virtual{0}, m_streams{0};
    std::atomic<u64> m_underruns{0};
    std::atomic<f32> m_mixMs{0.0f};
    Mutex m_reportLock;
    Array<u64> m_finished;          // mixer -> game: ids that ended
    Array<Pair<u32, f64>> m_times;  // mixer -> game: voice time
    // Mixer-thread state.
    Array<MixVoice> m_mixVoices;
    f32 m_mixBusGain[u32(Bus::Count)] = {1, 1, 1, 1, 1};
    f32 m_mixBusTarget[u32(Bus::Count)] = {1, 1, 1, 1, 1};
    ReverbSettings m_mixReverb;
    Reverb m_reverb;
    Limiter m_limiter;
    Array<f32> m_busBuffers[u32(Bus::Count)];
    Array<f32> m_reverbInput, m_scratch, m_decodeBuffer;
    // Device / thread.
    Thread m_thread;
    std::atomic<bool> m_quit{false};
    void* m_device = nullptr; // backend state (Wasapi.cpp)
    String m_deviceName;
    std::atomic<bool> m_deviceOpen{false};
};

} // namespace zx::audio
