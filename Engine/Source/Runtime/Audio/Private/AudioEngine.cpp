// Mixer: voices -> buses -> master. Game thread: 3D, virtualization, ducking, snapshots (Update). Mixer thread:
// commands, sample-accurate ramps, streaming decode, reverb, limiter (Mix).

#include "Audio/AudioEngine.h"
#include "Core/Containers/Algorithm.h"
#include "Core/Log/Log.h"
#include "Core/Time/Time.h"

#include <cmath>

namespace zx::audio {

ZX_DEFINE_LOG_CATEGORY_STATIC(LogAudio, Info);

const char* BusName(Bus bus)
{
    switch (bus) {
    case Bus::Master: return "Master";
    case Bus::Music: return "Music";
    case Bus::SFX: return "SFX";
    case Bus::UI: return "UI";
    case Bus::Voice: return "Voice";
    default: return "?";
    }
}

// --- Internal state -----------------------------------------------------------------------------------------------------

struct AudioEngine::Voice
{
    VoiceId id = 0;
    RefPtr<AudioClip> clip;
    VoiceParams params;
    bool paused = false;
    bool stopping = false;
    f64 time = 0.0;
    f32 audibility = 0.0f;
    // Last computed mix parameters.
    f32 gainL = 0.0f, gainR = 0.0f, pitch = 1.0f, lowpass = 1.0f, send = 0.0f;
    bool audible = true;
};

struct AudioEngine::Command
{
    enum class Type : u8
    {
        Start,
        Update,
        Stop,
        StopAll,
        Buses,
        Reverb,
    } type;
    VoiceId id = 0;
    RefPtr<AudioClip> clip;
    Bus bus = Bus::SFX;
    bool loop = false;
    bool paused = false;
    bool audible = true;
    f32 gainL = 0.0f, gainR = 0.0f, pitch = 1.0f, lowpass = 1.0f, send = 0.0f;
    f32 fadeFrames = 0.0f;
    f32 busGains[u32(Bus::Count)] = {};
    ReverbSettings reverb;
};

struct AudioEngine::MixVoice
{
    VoiceId id = 0;
    RefPtr<AudioClip> clip;
    UniquePtr<StreamDecoder> stream;
    Array<f32> streamBuffer; // decoded frames [streamBase, streamBase + streamFrames)
    f64 streamBase = 0.0;
    u32 streamFrames = 0;
    bool streamEnded = false;
    u32 channels = 1;
    f64 position = 0.0; // in source frames (keeps growing across loops for streams)
    f64 rate = 1.0;     // source rate / mix rate
    bool loop = false;
    Bus bus = Bus::SFX;
    bool paused = false;
    bool audible = true;
    f32 gainL = 0.0f, gainR = 0.0f, targetL = 0.0f, targetR = 0.0f;
    f32 pitch = 1.0f;
    f32 lpCoef = 1.0f, lpTarget = 1.0f;
    f32 lpState[2] = {0.0f, 0.0f};
    f32 send = 0.0f, sendTarget = 0.0f;
    f32 fade = 1.0f, fadeStep = 0.0f; // envelope (fade-in / stop)
    bool finished = false;
};

namespace {

/// 4-point Hermite interpolation.
inline f32 Hermite(f32 xm1, f32 x0, f32 x1, f32 x2, f32 t)
{
    const f32 c = (x1 - xm1) * 0.5f;
    const f32 v = x0 - x1;
    const f32 w = c + v;
    const f32 a = w + v + (x2 - x0) * 0.5f;
    const f32 bNeg = w + a;
    return ((((a * t) - bNeg) * t + c) * t + x0);
}

constexpr f32 SpeedOfSound = 343.0f;

} // namespace

// --- Lifetime -----------------------------------------------------------------------------------------------------------

AudioEngine::AudioEngine() = default;

AudioEngine::~AudioEngine() { Shutdown(); }

bool AudioEngine::Initialize(const AudioSettings& settings)
{
    if (m_initialized)
        return true;
    m_settings = settings;
    m_reverb.Init(f32(MixRate));
    m_reverb.SetParams(0.6f, 0.5f);
    m_limiter.Setup(f32(MixRate), -0.3f, 80.0f);
    for (Array<f32>& b : m_busBuffers)
        b.Resize(MixBlock * 2);
    m_reverbInput.Resize(MixBlock * 2);
    m_scratch.Resize(MixBlock * 2);
    m_mixVoices.Reserve(settings.maxVoices);
    m_voices.Reserve(settings.maxVoices);
    m_initialized = true;
    if (settings.backend == AudioSettings::Backend::Default) {
        m_quit = false;
        m_thread.Start("Audio Mixer", [this] { MixerThread(); }, ThreadPriority::TimeCritical);
    }
    ZX_LOG_INFO(LogAudio, "Audio: {} kHz stereo mixer, {} voices ({} audible), {} backend", MixRate / 1000, settings.maxVoices,
                settings.maxAudibleVoices, settings.backend == AudioSettings::Backend::Null ? "null" : "WASAPI");
    return true;
}

void AudioEngine::Shutdown()
{
    if (!m_initialized)
        return;
    m_quit = true;
    if (m_thread.IsJoinable())
        m_thread.Join();
    m_mixVoices.Clear();
    m_voices.Clear();
    {
        ScopedLock lock(m_commandLock);
        m_commands.Clear();
        m_mixerCommands.Clear();
    }
    m_initialized = false;
}

// --- Game thread API ----------------------------------------------------------------------------------------------------

VoiceId AudioEngine::Play(const RefPtr<AudioClip>& clip, const VoiceParams& params)
{
    if (!m_initialized || !clip || (clip->format == ClipFormat::Pcm && clip->frameCount == 0))
        return 0;
    if (m_voices.Size() >= m_settings.maxVoices) {
        // Steal the least important voice (lowest priority, quietest).
        usize victim = U32Max;
        for (usize i = 0; i < m_voices.Size(); ++i) {
            const Voice& v = m_voices[i];
            if (v.params.priority > params.priority)
                continue;
            if (victim == U32Max || v.params.priority < m_voices[victim].params.priority ||
                (v.params.priority == m_voices[victim].params.priority && v.audibility < m_voices[victim].audibility))
                victim = i;
        }
        if (victim == U32Max)
            return 0;
        Stop(m_voices[victim].id, 0.005f);
        m_voices.RemoveAt(victim);
    }
    Voice v;
    v.id = m_nextId++;
    if (m_nextId == 0)
        m_nextId = 1;
    v.clip = clip;
    v.params = params;
    m_voices.PushBack(v);
    Command c{Command::Type::Start};
    c.id = v.id;
    c.clip = clip;
    c.bus = params.bus;
    c.loop = params.loop;
    c.fadeFrames = params.fadeIn * f32(MixRate);
    {
        ScopedLock lock(m_commandLock);
        m_commands.PushBack(move(c));
    }
    Update(0.0f); // gains for the first block
    return v.id;
}

void AudioEngine::Stop(VoiceId voice, f32 fadeSeconds)
{
    for (Voice& v : m_voices)
        if (v.id == voice)
            v.stopping = true;
    Command c{Command::Type::Stop};
    c.id = voice;
    c.fadeFrames = Max(fadeSeconds * f32(MixRate), 32.0f); // never cut a wave mid-cycle
    ScopedLock lock(m_commandLock);
    m_commands.PushBack(move(c));
}

void AudioEngine::StopAll(f32 fadeSeconds)
{
    for (Voice& v : m_voices)
        v.stopping = true;
    Command c{Command::Type::StopAll};
    c.fadeFrames = Max(fadeSeconds * f32(MixRate), 32.0f);
    ScopedLock lock(m_commandLock);
    m_commands.PushBack(move(c));
}

void AudioEngine::SetPaused(VoiceId voice, bool paused)
{
    for (Voice& v : m_voices)
        if (v.id == voice)
            v.paused = paused;
}

bool AudioEngine::IsPlaying(VoiceId voice) const
{
    for (const Voice& v : m_voices)
        if (v.id == voice)
            return !v.stopping;
    return false;
}

void AudioEngine::SetParams(VoiceId voice, const VoiceParams& params)
{
    for (Voice& v : m_voices)
        if (v.id == voice) {
            const bool loop = v.params.loop;
            const Bus bus = v.params.bus;
            v.params = params;
            v.params.loop = loop; // fixed at start
            v.params.bus = bus;
        }
}

const VoiceParams* AudioEngine::GetParams(VoiceId voice) const
{
    for (const Voice& v : m_voices)
        if (v.id == voice)
            return &v.params;
    return nullptr;
}

f64 AudioEngine::GetTime(VoiceId voice) const
{
    for (const Voice& v : m_voices)
        if (v.id == voice)
            return v.time;
    return 0.0;
}

void AudioEngine::SetListener(Vec3 position, Vec3 forward, Vec3 up, Vec3 velocity)
{
    m_listenerPos = position;
    m_listenerForward = SafeNormalize(forward, Vec3(0, 0, 1));
    m_listenerUp = SafeNormalize(up, Vec3(0, 1, 0));
    m_listenerVelocity = velocity;
}

void AudioEngine::SetBusVolume(Bus bus, f32 volume)
{
    m_busVolume[u32(bus)] = Max(volume, 0.0f);
    m_snapDuration = 0.0f; // a manual change ends a snapshot blend
}

void AudioEngine::SetBusMuted(Bus bus, bool muted) { m_busMuted[u32(bus)] = muted; }

void AudioEngine::ApplySnapshot(const MixSnapshot& snapshot, f32 seconds)
{
    for (u32 b = 0; b < u32(Bus::Count); ++b)
        m_snapFrom.busVolume[b] = m_busVolume[b];
    m_snapFrom.reverbWet = m_reverbTarget.wet;
    m_snapTo = snapshot;
    m_snapTime = 0.0f;
    m_snapDuration = Max(seconds, 1e-4f);
}

void AudioEngine::Update(f32 dt)
{
    if (!m_initialized)
        return;
    // Ended voices and play positions reported by the mixer.
    {
        ScopedLock lock(m_reportLock);
        for (u64 id : m_finished)
            m_voices.RemoveIf([id](const Voice& v) { return v.id == VoiceId(id); });
        m_finished.Clear();
        for (const Pair<u32, f64>& t : m_times)
            for (Voice& v : m_voices)
                if (v.id == t.first)
                    v.time = t.second;
        m_times.Clear();
    }
    // Snapshot blend.
    if (m_snapDuration > 0.0f && dt > 0.0f) {
        m_snapTime = Min(m_snapTime + dt, m_snapDuration);
        const f32 t = m_snapTime / m_snapDuration;
        for (u32 b = 0; b < u32(Bus::Count); ++b)
            m_busVolume[b] = Lerp(m_snapFrom.busVolume[b], m_snapTo.busVolume[b], t);
        m_reverbTarget.wet = Lerp(m_snapFrom.reverbWet, m_snapTo.reverbWet, t);
        if (m_snapTime >= m_snapDuration)
            m_snapDuration = 0.0f;
    }
    // Ducking: music goes down while the Voice bus is active (fast attack, slow release).
    const f32 voiceLevel = m_voiceBusRms.load(std::memory_order_relaxed);
    const f32 duckTarget = m_duckEnabled && voiceLevel > 0.005f ? DbToGain(m_duckDb) : 1.0f;
    if (dt > 0.0f) {
        const f32 tc = duckTarget < m_duckGain ? 0.05f : 0.4f;
        m_duckGain += (duckTarget - m_duckGain) * (1.0f - std::exp(-dt / tc));
        const f32 rk = 1.0f - std::exp(-dt / 0.3f);
        m_reverbCurrent.wet += (m_reverbTarget.wet - m_reverbCurrent.wet) * rk;
        m_reverbCurrent.roomSize += (m_reverbTarget.roomSize - m_reverbCurrent.roomSize) * rk;
        m_reverbCurrent.damping += (m_reverbTarget.damping - m_reverbCurrent.damping) * rk;
    } else if (m_voices.Size() <= 1) {
        m_reverbCurrent = m_reverbTarget;
    }

    // Per voice: gains, pitch, filter, send.
    const Vec3 right = SafeNormalize(Cross(m_listenerUp, m_listenerForward), Vec3(1, 0, 0));
    for (Voice& v : m_voices) {
        const VoiceParams& p = v.params;
        f32 gain = Max(p.volume, 0.0f);
        f32 pan = Clamp(p.pan, -1.0f, 1.0f);
        f32 pitch = Clamp(p.pitch, 0.05f, 8.0f);
        f32 send = p.reverbSend;
        if (p.spatial) {
            const Vec3 rel = p.position - m_listenerPos;
            const f32 dist = Length(rel);
            const f32 minD = Max(p.minDistance, 0.01f), maxD = Max(p.maxDistance, minD + 0.01f);
            const f32 d = Clamp(dist, minD, maxD);
            switch (p.rolloff) {
            case Rolloff::Linear: gain *= 1.0f - (d - minD) / (maxD - minD); break;
            case Rolloff::Logarithmic: gain *= (minD / d) * (minD / d); break;
            default: gain *= minD / d; break;
            }
            // Directional cone (angle between the source's axis and the direction to the listener).
            if (p.coneOuter < 360.0f && dist > 1e-4f) {
                const f32 angle = RadToDeg(Acos(Clamp(Dot(SafeNormalize(p.direction, Vec3(0, 0, 1)), -rel / dist), -1.0f, 1.0f))) * 2.0f;
                const f32 inner = Min(p.coneInner, p.coneOuter);
                if (angle > inner) {
                    const f32 t = Clamp((angle - inner) / Max(p.coneOuter - inner, 1e-3f), 0.0f, 1.0f);
                    gain *= Lerp(1.0f, p.coneOuterGain, t);
                }
            }
            // Panning from the listener's right axis; closer than min distance blends to center.
            if (dist > 1e-4f) {
                pan = Dot(rel / dist, right);
                pan *= Clamp(dist / minD, 0.0f, 1.0f) * (1.0f - Clamp(p.spread, 0.0f, 1.0f));
            } else {
                pan = 0.0f;
            }
            // Doppler.
            if (p.dopplerScale > 0.0f && dist > 1e-4f) {
                const Vec3 dir = rel / dist; // listener -> source
                const f32 vl = Dot(m_listenerVelocity, dir);
                const f32 vs = Dot(p.velocity, dir);
                const f32 doppler = Clamp((SpeedOfSound + vl) / Max(SpeedOfSound + vs, 1.0f), 0.5f, 2.0f);
                pitch *= 1.0f + (doppler - 1.0f) * p.dopplerScale;
            }
            send += 0.15f + 0.6f * Clamp(dist / maxD, 0.0f, 1.0f); // distant sounds are more reverberant
        }
        const f32 occlusion = Clamp(p.occlusion, 0.0f, 1.0f);
        gain *= 1.0f - 0.6f * occlusion;
        const f32 cutoff = Lerp(20000.0f, 700.0f, occlusion);
        // Constant-power pan.
        const f32 angle = (pan + 1.0f) * (Pi * 0.25f);
        v.gainL = gain * Cos(angle) * 1.41421356f;
        v.gainR = gain * Sin(angle) * 1.41421356f;
        if (pan == 0.0f)
            v.gainL = v.gainR = gain;
        v.pitch = pitch;
        v.lowpass = occlusion > 0.0f ? OnePoleLowPass::Coefficient(f32(MixRate), cutoff) : 1.0f;
        v.send = Clamp(send, 0.0f, 1.0f);
        const f32 busGain = m_busMuted[u32(p.bus)] ? 0.0f : m_busVolume[u32(p.bus)];
        v.audibility = Max(v.gainL, v.gainR) * busGain;
    }
    // Virtualization: the most important / loudest voices are mixed, the rest only advance.
    Array<Voice*> order;
    order.Reserve(m_voices.Size());
    for (Voice& v : m_voices)
        order.PushBack(&v);
    Sort(order, [](const Voice* a, const Voice* b) {
        if (a->params.priority != b->params.priority)
            return a->params.priority > b->params.priority;
        return a->audibility > b->audibility;
    });
    u32 audible = 0, virtualCount = 0;
    for (Voice* v : order) {
        v->audible = audible < m_settings.maxAudibleVoices && v->audibility > 1e-5f;
        if (v->audible)
            ++audible;
        else
            ++virtualCount;
    }
    m_audible = audible;
    m_virtual = virtualCount;

    // Push everything to the mixer.
    ScopedLock lock(m_commandLock);
    for (const Voice& v : m_voices) {
        if (v.stopping)
            continue;
        Command c{Command::Type::Update};
        c.id = v.id;
        c.gainL = v.gainL;
        c.gainR = v.gainR;
        c.pitch = v.pitch;
        c.lowpass = v.lowpass;
        c.send = v.send;
        c.audible = v.audible;
        c.paused = v.paused;
        m_commands.PushBack(move(c));
    }
    Command buses{Command::Type::Buses};
    const f32 master = m_busMuted[0] ? 0.0f : m_busVolume[0];
    for (u32 b = 0; b < u32(Bus::Count); ++b)
        buses.busGains[b] = b == 0 ? master : (m_busMuted[b] ? 0.0f : m_busVolume[b]);
    buses.busGains[u32(Bus::Music)] *= m_duckGain;
    m_commands.PushBack(move(buses));
    Command reverb{Command::Type::Reverb};
    reverb.reverb = m_reverbCurrent;
    m_commands.PushBack(move(reverb));
}

AudioStats AudioEngine::GetStats() const
{
    AudioStats s;
    s.voices = u32(m_voices.Size());
    s.audible = m_audible.load();
    s.virtualVoices = m_virtual.load();
    s.streams = m_streams.load();
    s.underruns = m_underruns.load();
    s.mixCpuMs = m_mixMs.load();
    s.deviceOpen = m_deviceOpen.load();
    s.deviceName = m_deviceName;
    return s;
}

// --- Mixer thread -------------------------------------------------------------------------------------------------------

void AudioEngine::ApplyCommands()
{
    {
        ScopedLock lock(m_commandLock);
        Array<Command> pending = move(m_commands);
        m_commands = move(m_mixerCommands);
        m_mixerCommands = move(pending);
    }
    auto find = [this](VoiceId id) -> MixVoice* {
        for (MixVoice& v : m_mixVoices)
            if (v.id == id)
                return &v;
        return nullptr;
    };
    for (Command& c : m_mixerCommands) {
        switch (c.type) {
        case Command::Type::Start: {
            MixVoice v;
            v.id = c.id;
            v.clip = c.clip;
            v.bus = c.bus;
            v.loop = c.loop;
            v.channels = Max(c.clip->channels, 1u);
            v.rate = f64(c.clip->sampleRate) / f64(MixRate);
            if (c.clip->IsStreamed()) {
                v.stream = c.clip->CreateDecoder();
                if (!v.stream)
                    break;
                v.channels = v.stream->Channels();
            }
            if (c.fadeFrames > 0.0f) {
                v.fade = 0.0f;
                v.fadeStep = 1.0f / c.fadeFrames;
            }
            m_mixVoices.PushBack(move(v));
            break;
        }
        case Command::Type::Update:
            if (MixVoice* v = find(c.id)) {
                const bool first = v->targetL == 0.0f && v->targetR == 0.0f && v->gainL == 0.0f && v->position == 0.0;
                v->targetL = c.gainL;
                v->targetR = c.gainR;
                v->pitch = c.pitch;
                v->lpTarget = c.lowpass;
                v->sendTarget = c.send;
                v->audible = c.audible;
                v->paused = c.paused;
                if (first) {
                    // Gains ramp up from zero over the first block (no click even if the clip starts mid-wave);
                    // the filter and the send start where they should.
                    v->lpCoef = c.lowpass;
                    v->send = c.send;
                }
            }
            break;
        case Command::Type::Stop:
            if (MixVoice* v = find(c.id))
                v->fadeStep = -1.0f / Max(c.fadeFrames, 1.0f);
            break;
        case Command::Type::StopAll:
            for (MixVoice& v : m_mixVoices)
                v.fadeStep = -1.0f / Max(c.fadeFrames, 1.0f);
            break;
        case Command::Type::Buses:
            for (u32 b = 0; b < u32(Bus::Count); ++b)
                m_mixBusTarget[b] = c.busGains[b];
            break;
        case Command::Type::Reverb:
            if (Abs(c.reverb.roomSize - m_mixReverb.roomSize) > 1e-3f || Abs(c.reverb.damping - m_mixReverb.damping) > 1e-3f)
                m_reverb.SetParams(c.reverb.roomSize, c.reverb.damping);
            m_mixReverb = c.reverb;
            break;
        }
    }
    m_mixerCommands.Clear();
}

void AudioEngine::Mix(f32* out, u32 frames)
{
    Stopwatch sw;
    ApplyCommands();
    u32 done = 0;
    while (done < frames) {
        const u32 n = Min(frames - done, MixBlock);
        MixOneBlock(out + usize(done) * 2, n);
        done += n;
    }
    // Report ended voices and positions to the game thread.
    {
        ScopedLock lock(m_reportLock);
        for (const MixVoice& v : m_mixVoices) {
            if (v.finished)
                m_finished.PushBack(v.id);
            const f64 total = v.clip ? f64(v.clip->frameCount) : 0.0;
            f64 pos = v.position;
            if (v.loop && total > 0.0)
                pos = std::fmod(pos, total);
            m_times.PushBack(Pair<u32, f64>{v.id, pos / f64(Max(v.clip ? v.clip->sampleRate : MixRate, 1u))});
        }
    }
    m_mixVoices.RemoveIf([](const MixVoice& v) { return v.finished; });
    u32 streams = 0;
    for (const MixVoice& v : m_mixVoices)
        streams += v.stream ? 1u : 0u;
    m_streams = streams;
    const f32 ms = f32(sw.ElapsedMilliseconds()) * f32(MixBlock) / f32(Max(frames, 1u));
    m_mixMs = m_mixMs.load() * 0.95f + ms * 0.05f;
}

void AudioEngine::MixOneBlock(f32* out, u32 frames)
{
    for (u32 b = 1; b < u32(Bus::Count); ++b)
        MemZero(m_busBuffers[b].Data(), usize(frames) * 2 * sizeof(f32));
    MemZero(m_reverbInput.Data(), usize(frames) * 2 * sizeof(f32));
    const f32 invFrames = 1.0f / f32(frames);

    for (MixVoice& v : m_mixVoices) {
        if (v.finished || v.paused)
            continue;
        const AudioClip& clip = *v.clip;
        const f64 step = v.rate * f64(v.pitch);
        const u32 ch = v.channels;
        // Virtual PCM voices only advance (stream voices are always decoded to keep their decoder in step).
        if (!v.audible && !v.stream) {
            v.position += step * frames;
            if (v.fadeStep < 0.0f) {
                v.fade += v.fadeStep * f32(frames);
                v.finished = v.fade <= 0.0f;
            }
            if (!v.loop && v.position >= f64(clip.frameCount))
                v.finished = true;
            else if (v.loop && clip.frameCount)
                v.position = std::fmod(v.position, f64(clip.frameCount));
            v.gainL = v.targetL;
            v.gainR = v.targetR;
            continue;
        }
        // Streams: keep enough decoded frames ahead for this block (+ interpolation taps).
        if (v.stream) {
            const f64 needEnd = v.position + step * frames + 3.0;
            while (!v.streamEnded && v.streamBase + v.streamFrames < needEnd) {
                // Drop frames that are behind the read position (keep 2 for interpolation).
                const u32 keepFrom = u32(Max(0.0, std::floor(v.position) - 2.0 - v.streamBase));
                if (keepFrom > 0 && keepFrom <= v.streamFrames) {
                    MemMove(v.streamBuffer.Data(), v.streamBuffer.Data() + usize(keepFrom) * ch,
                            usize(v.streamFrames - keepFrom) * ch * sizeof(f32));
                    v.streamFrames -= keepFrom;
                    v.streamBase += keepFrom;
                }
                const u32 chunk = 2048;
                v.streamBuffer.Resize(usize(v.streamFrames + chunk) * ch);
                u32 got = v.stream->Read(v.streamBuffer.Data() + usize(v.streamFrames) * ch, chunk);
                if (got == 0 && v.loop && v.stream->Rewind())
                    got = v.stream->Read(v.streamBuffer.Data() + usize(v.streamFrames) * ch, chunk);
                if (got == 0)
                    v.streamEnded = true;
                v.streamFrames += got;
            }
        }
        const f32* data = v.stream ? v.streamBuffer.Data() : clip.samples.Data();
        const i64 total = v.stream ? i64(v.streamFrames) : i64(clip.frameCount);
        const f64 base = v.stream ? v.streamBase : 0.0;
        auto sample = [&](i64 frame, u32 c) -> f32 {
            frame -= i64(base);
            if (frame < 0 || frame >= total) {
                if (!v.loop || v.stream || total == 0)
                    return 0.0f;
                frame = ((frame % total) + total) % total;
            }
            return data[usize(frame) * ch + Min(c, ch - 1)];
        };
        f32* busOut = m_busBuffers[u32(v.bus)].Data();
        const f32 dL = (v.targetL - v.gainL) * invFrames, dR = (v.targetR - v.gainR) * invFrames;
        const f32 dLp = (v.lpTarget - v.lpCoef) * invFrames, dSend = (v.sendTarget - v.send) * invFrames;
        const bool filter = v.lpCoef < 1.0f || v.lpTarget < 1.0f;
        for (u32 f = 0; f < frames; ++f) {
            const i64 i = i64(std::floor(v.position));
            const f32 t = f32(v.position - f64(i));
            f32 s[2];
            for (u32 c = 0; c < 2; ++c) {
                if (c == 1 && ch == 1) {
                    s[1] = s[0];
                    break;
                }
                s[c] = Hermite(sample(i - 1, c), sample(i, c), sample(i + 1, c), sample(i + 2, c), t);
            }
            v.gainL += dL;
            v.gainR += dR;
            v.send += dSend;
            if (filter) {
                v.lpCoef += dLp;
                v.lpState[0] += (s[0] - v.lpState[0]) * v.lpCoef;
                v.lpState[1] += (s[1] - v.lpState[1]) * v.lpCoef;
                s[0] = v.lpState[0];
                s[1] = v.lpState[1];
            }
            if (v.fadeStep != 0.0f) {
                v.fade = Clamp(v.fade + v.fadeStep, 0.0f, 1.0f);
                if (v.fade >= 1.0f && v.fadeStep > 0.0f)
                    v.fadeStep = 0.0f;
            }
            const f32 l = s[0] * v.gainL * v.fade, r = s[1] * v.gainR * v.fade;
            busOut[f * 2] += l;
            busOut[f * 2 + 1] += r;
            m_reverbInput[f * 2] += l * v.send;
            m_reverbInput[f * 2 + 1] += r * v.send;
            v.position += step;
        }
        v.gainL = v.targetL;
        v.gainR = v.targetR;
        v.lpCoef = v.lpTarget;
        v.send = v.sendTarget;
        if (v.fadeStep < 0.0f && v.fade <= 0.0f)
            v.finished = true;
        if (v.stream) {
            if (v.streamEnded && v.position >= v.streamBase + v.streamFrames)
                v.finished = true;
        } else if (v.position >= f64(clip.frameCount)) {
            if (v.loop)
                v.position = std::fmod(v.position, f64(clip.frameCount));
            else
                v.finished = true;
        }
    }

    // Buses -> master (ramped gains), meters, ducking detector, reverb, limiter.
    MemZero(out, usize(frames) * 2 * sizeof(f32));
    for (u32 b = 1; b < u32(Bus::Count); ++b) {
        const f32* in = m_busBuffers[b].Data();
        const f32 g0 = m_mixBusGain[b], dg = (m_mixBusTarget[b] - g0) * invFrames;
        f32 peak = 0.0f, energy = 0.0f;
        for (u32 f = 0; f < frames; ++f) {
            const f32 g = g0 + dg * f32(f + 1);
            const f32 l = in[f * 2] * g, r = in[f * 2 + 1] * g;
            out[f * 2] += l;
            out[f * 2 + 1] += r;
            peak = Max(peak, Max(Abs(l), Abs(r)));
            energy += in[f * 2] * in[f * 2] + in[f * 2 + 1] * in[f * 2 + 1];
        }
        m_mixBusGain[b] = m_mixBusTarget[b];
        const f32 prev = m_busPeak[b].load(std::memory_order_relaxed);
        m_busPeak[b].store(Max(peak, prev * 0.93f), std::memory_order_relaxed);
        if (b == u32(Bus::Voice))
            m_voiceBusRms.store(Sqrt(energy / f32(frames * 2)), std::memory_order_relaxed);
    }
    m_reverb.Process(m_reverbInput.Data(), out, frames, m_mixReverb.wet);
    const f32 m0 = m_mixBusGain[0], dm = (m_mixBusTarget[0] - m0) * invFrames;
    f32 masterPeak = 0.0f;
    for (u32 f = 0; f < frames; ++f) {
        const f32 g = m0 + dm * f32(f + 1);
        out[f * 2] *= g;
        out[f * 2 + 1] *= g;
    }
    m_mixBusGain[0] = m_mixBusTarget[0];
    m_limiter.Process(out, frames);
    for (u32 f = 0; f < frames * 2; ++f)
        masterPeak = Max(masterPeak, Abs(out[f]));
    const f32 prev = m_busPeak[0].load(std::memory_order_relaxed);
    m_busPeak[0].store(Max(masterPeak, prev * 0.93f), std::memory_order_relaxed);
}

void AudioEngine::RenderOffline(f32* out, u32 frames) { Mix(out, frames); }

} // namespace zx::audio
