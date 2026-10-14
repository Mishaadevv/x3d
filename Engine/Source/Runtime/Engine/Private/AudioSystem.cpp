#include "Engine/AudioSystem.h"
#include "Core/HAL/PlatformFile.h"
#include "Core/Log/Log.h"
#include "Core/String/Path.h"
#include "Engine/PhysicsSystem.h"
#include "Scene/Hierarchy.h"

namespace zx {

ZX_DEFINE_LOG_CATEGORY_STATIC(LogAudioSystem, Info);

namespace audio {
ZX_REFLECT_ENUM_BEGIN(Bus)
    ZX_ENUM_VALUE(Master)
    ZX_ENUM_VALUE(Music)
    ZX_ENUM_VALUE(SFX)
    ZX_ENUM_VALUE(UI)
    ZX_ENUM_VALUE(Voice)
ZX_REFLECT_ENUM_END()

ZX_REFLECT_ENUM_BEGIN(Rolloff)
    ZX_ENUM_VALUE(Inverse)
    ZX_ENUM_VALUE(Linear)
    ZX_ENUM_VALUE(Logarithmic)
ZX_REFLECT_ENUM_END()
} // namespace audio

ZX_REFLECT_STRUCT_BEGIN(AudioSource)
    ZX_FIELD(clip, Meta::Hidden()) // edited by the Details panel's clip slot
    ZX_FIELD(volume, Meta::Range(0, 4))
    ZX_FIELD(pitch, Meta::Range(0.1, 4))
    ZX_FIELD(loop)
    ZX_FIELD(playOnStart)
    ZX_FIELD(bus)
    ZX_FIELD(spatial, Meta::Tooltip("3D: attenuated and panned from the entity's position"))
    ZX_FIELD(minDistance, Meta::Range(0.01, 1000), Meta::Units("m"))
    ZX_FIELD(maxDistance, Meta::Range(0.1, 5000), Meta::Units("m"))
    ZX_FIELD(rolloff)
    ZX_FIELD(priority, Meta::Range(-100, 100))
    ZX_FIELD(dopplerScale, Meta::Range(0, 4))
    ZX_FIELD(spread, Meta::Range(0, 1))
    ZX_FIELD(coneInner, Meta::Range(0, 360), Meta::Units("deg"))
    ZX_FIELD(coneOuter, Meta::Range(0, 360), Meta::Units("deg"))
    ZX_FIELD(coneOuterGain, Meta::Range(0, 1))
    ZX_FIELD(occlusion, Meta::Tooltip("Muffle the sound when geometry blocks it (raycast)"))
    ZX_FIELD(reverbSend, Meta::Range(0, 1))
ZX_REFLECT_STRUCT_END()

ZX_REFLECT_STRUCT_BEGIN(AudioListener)
    ZX_FIELD(active)
ZX_REFLECT_STRUCT_END()

ZX_REFLECT_STRUCT_BEGIN(ReverbZone)
    ZX_FIELD(radius, Meta::Range(0.1, 1000), Meta::Units("m"))
    ZX_FIELD(fade, Meta::Range(0, 100), Meta::Units("m"))
    ZX_FIELD(wet, Meta::Range(0, 1))
    ZX_FIELD(roomSize, Meta::Range(0, 1))
    ZX_FIELD(damping, Meta::Range(0, 1))
ZX_REFLECT_STRUCT_END()

ZX_REFLECT_STRUCT_BEGIN(ImpactSound)
    ZX_FIELD(sound, Meta::Tooltip("A file, or a set of variants: Impacts/Impact_Wood plays Impact_Wood_0 .. _9 at random"))
    ZX_FIELD(volume, Meta::Range(0, 4))
    ZX_FIELD(minSpeed, Meta::Range(0, 50), Meta::Units("m/s"), Meta::Tooltip("Softer hits are silent"))
    ZX_FIELD(maxSpeed, Meta::Range(0.1, 100), Meta::Units("m/s"), Meta::Tooltip("Hits this fast play at full volume"))
    ZX_FIELD(pitchVariation, Meta::Range(0, 0.5))
    ZX_FIELD(maxDistance, Meta::Range(1, 500), Meta::Units("m"))
ZX_REFLECT_STRUCT_END()

AudioSystem::AudioSystem(audio::AudioEngine& engine) : m_engine(engine) {}

String AudioSystem::PickVariant(StringView base)
{
    Array<String>* files = m_variants.Find(String(base));
    if (!files) {
        Array<String> found;
        auto exists = [&](StringView rel) { return platform::FileExists(Path::IsAbsolute(rel) ? String(rel) : Path::Join(m_contentRoot, rel)); };
        if (!Path::Extension(base).Empty() && exists(base)) {
            found.PushBack(String(base));
        } else {
            for (u32 i = 0; i < 10; ++i)
                for (StringView ext : {StringView(".ogg"), StringView(".wav")}) {
                    const String f = String::Format("{}_{}{}", base, i, ext);
                    if (exists(f)) {
                        found.PushBack(f);
                        break;
                    }
                }
            for (StringView ext : {StringView(".ogg"), StringView(".wav")})
                if (found.Empty() && exists(String::Format("{}{}", base, ext)))
                    found.PushBack(String::Format("{}{}", base, ext));
        }
        m_variants.InsertOrAssign(String(base), move(found));
        files = m_variants.Find(String(base));
    }
    return files->Empty() ? String() : (*files)[m_rng.NextBounded(u32(files->Size()))];
}

void AudioSystem::PlayImpacts(ecs::World& world, Span<const PhysicsEvent> events)
{
    for (const PhysicsEvent& ev : events) {
        if (ev.type != physics::ContactEventType::TouchBegin)
            continue;
        for (ecs::Entity e : {ev.a, ev.b}) {
            const ImpactSound* s = world.IsAlive(e) ? world.Get<const ImpactSound>(e) : nullptr;
            if (!s || s->sound.Empty() || ev.speed < s->minSpeed)
                continue;
            // One sound per body per 80 ms (a box landing touches with several corners at once).
            f64* last = m_lastImpact.Find(e.Pack());
            if (last && m_time - *last < 0.08)
                continue;
            m_lastImpact.InsertOrAssign(e.Pack(), m_time);
            const String file = PickVariant(s->sound);
            RefPtr<audio::AudioClip> clip = file.Empty() ? nullptr : LoadClip(file);
            if (!clip)
                continue;
            const f32 t = Clamp((ev.speed - s->minSpeed) / Max(s->maxSpeed - s->minSpeed, 0.01f), 0.0f, 1.0f);
            audio::VoiceParams p;
            p.volume = s->volume * (0.15f + 0.85f * t * t);
            p.pitch = 1.0f + s->pitchVariation * m_rng.Range(-1.0f, 1.0f);
            p.spatial = true;
            p.position = ev.point;
            p.maxDistance = s->maxDistance;
            m_engine.Play(clip, p);
        }
    }
}

AudioSystem::~AudioSystem()
{
    End();
    m_engine.Stop(m_preview, 0.01f);
}

void AudioSystem::SetContentRoot(StringView physicalDir)
{
    if (m_contentRoot == physicalDir)
        return;
    End();
    m_contentRoot = physicalDir;
    m_clips.Clear();
}

void AudioSystem::ClearCache()
{
    m_clips.Clear();
    m_variants.Clear();
}

RefPtr<audio::AudioClip> AudioSystem::LoadClip(StringView file, String* error)
{
    if (file.Empty()) {
        if (error)
            *error = "No sound file";
        return {};
    }
    const String path = Path::IsAbsolute(file) ? String(file) : Path::Join(m_contentRoot, file);
    platform::FileInfo info;
    const i64 mtime = platform::GetFileInfo(path, info) ? info.modifiedTime : 0;
    CachedClip* cached = m_clips.Find(file);
    if (!cached || cached->modifiedTime != mtime) {
        CachedClip entry;
        entry.modifiedTime = mtime;
        Result<RefPtr<audio::AudioClip>> loaded = audio::LoadAudioClipFile(path);
        if (loaded)
            entry.clip = loaded.Value();
        else
            entry.error = loaded.GetError().message;
        cached = &m_clips.InsertOrAssign(String(file), move(entry));
    }
    if (error)
        *error = cached->error;
    return cached->clip;
}

audio::VoiceParams AudioSystem::ParamsFor(const AudioSource& s, const Mat4& world) const
{
    audio::VoiceParams p;
    p.volume = s.volume;
    p.pitch = s.pitch;
    p.loop = s.loop;
    p.bus = s.bus;
    p.priority = s.priority;
    p.spatial = s.spatial;
    p.position = world.GetTranslation();
    p.direction = SafeNormalize(world.TransformVector(Vec3(0, 0, 1)), Vec3(0, 0, 1));
    p.minDistance = s.minDistance;
    p.maxDistance = s.maxDistance;
    p.rolloff = s.rolloff;
    p.coneInner = s.coneInner;
    p.coneOuter = s.coneOuter;
    p.coneOuterGain = s.coneOuterGain;
    p.dopplerScale = s.dopplerScale;
    p.spread = s.spread;
    p.reverbSend = s.reverbSend;
    return p;
}

bool AudioSystem::PlayEntity(ecs::World& world, ecs::Entity e)
{
    const AudioSource* s = world.IsAlive(e) ? world.Get<const AudioSource>(e) : nullptr;
    if (!s)
        return false;
    StopEntity(e);
    String error;
    RefPtr<audio::AudioClip> clip = LoadClip(s->clip, &error);
    if (!clip) {
        ZX_LOG_WARN(LogAudioSystem, "AudioSource: {}", error);
        return false;
    }
    const audio::VoiceId v = m_engine.Play(clip, ParamsFor(*s, scene::ComputeWorldMatrix(world, e)));
    if (!v)
        return false;
    Running r;
    r.entity = e;
    r.voice = v;
    r.occlusionPhase = u32(m_sources.Size());
    m_sources.PushBack(r);
    return true;
}

void AudioSystem::StopEntity(ecs::Entity e)
{
    m_sources.RemoveIf([&](const Running& r) {
        if (r.entity != e)
            return false;
        m_engine.Stop(r.voice, 0.05f);
        return true;
    });
}

audio::VoiceId AudioSystem::PlayOneShot(StringView file, f32 volume, bool spatial, Vec3 position, audio::Bus bus)
{
    String error;
    String variant;
    if (Path::Extension(file).Empty()) {
        const bool known = m_variants.Contains(String(file));
        variant = PickVariant(file);
        if (variant.Empty()) {
            if (!known) // once per sound set (footsteps would repeat it every step)
                ZX_LOG_WARN(LogAudioSystem, "No sound '{}' or variants {}_0 .. _9", file, file);
            return 0;
        }
    }
    RefPtr<audio::AudioClip> clip = LoadClip(variant.Empty() ? file : variant.View(), &error);
    if (!clip) {
        ZX_LOG_WARN(LogAudioSystem, "{}", error);
        return 0;
    }
    audio::VoiceParams p;
    p.volume = volume;
    p.spatial = spatial;
    p.position = position;
    p.bus = bus;
    p.maxDistance = 60.0f;
    return m_engine.Play(clip, p);
}

bool AudioSystem::TogglePreview(StringView file)
{
    const bool same = m_engine.IsPlaying(m_preview) && m_previewFile == file;
    m_engine.Stop(m_preview, 0.02f);
    m_preview = 0;
    if (same)
        return false;
    String error;
    RefPtr<audio::AudioClip> clip = LoadClip(file, &error);
    if (!clip) {
        ZX_LOG_WARN(LogAudioSystem, "Preview: {}", error);
        return false;
    }
    audio::VoiceParams p;
    p.bus = audio::Bus::UI;
    m_preview = m_engine.Play(clip, p);
    m_previewFile = file;
    return m_preview != 0;
}

void AudioSystem::Begin(ecs::World& world)
{
    End();
    m_running = true;
    m_frame = 0;
    ZX_UNUSED(world);
}

void AudioSystem::End()
{
    for (const Running& r : m_sources)
        m_engine.Stop(r.voice, 0.05f);
    m_sources.Clear();
    m_started.Clear();
    m_running = false;
    m_engine.SetReverb({});
}

void AudioSystem::Tick(ecs::World& world, f32 dt, const ListenerPose& fallback)
{
    if (!m_running)
        Begin(world);
    ++m_frame;
    m_time += f64(dt);
    // New sources (scene start, spawned while playing).
    Array<ecs::Entity> fresh;
    world.Query<const AudioSource>().Each([&](ecs::Entity e, const AudioSource& s) {
        if (!s.playOnStart || s.clip.Empty())
            return;
        for (ecs::Entity done : m_started)
            if (done == e)
                return;
        fresh.PushBack(e);
    });
    for (ecs::Entity e : fresh) {
        m_started.PushBack(e);
        PlayEntity(world, e);
    }

    // Listener: an active AudioListener, else the camera given by the host.
    ListenerPose listener = fallback;
    world.Query<const AudioListener, const scene::WorldTransform>().Each(
        [&](const AudioListener& l, const scene::WorldTransform& wt) {
            if (!l.active)
                return;
            listener.position = wt.GetPosition();
            listener.forward = SafeNormalize(wt.matrix.TransformVector(Vec3(0, 0, 1)), Vec3(0, 0, 1));
            listener.up = SafeNormalize(wt.matrix.TransformVector(Vec3(0, 1, 0)), Vec3(0, 1, 0));
        });
    m_engine.SetListener(listener.position, listener.forward, listener.up);

    // Reverb: the zone the listener is most inside of.
    audio::ReverbSettings reverb;
    f32 best = 0.0f;
    world.Query<const ReverbZone, const scene::WorldTransform>().Each([&](const ReverbZone& z, const scene::WorldTransform& wt) {
        const f32 d = Distance(wt.GetPosition(), listener.position);
        const f32 weight = Clamp((z.radius - d) / Max(z.fade, 1e-3f), 0.0f, 1.0f);
        if (weight > best) {
            best = weight;
            reverb.wet = z.wet * weight;
            reverb.roomSize = z.roomSize;
            reverb.damping = z.damping;
        }
    });
    m_engine.SetReverb(reverb);

    // Playing sources follow their entities.
    m_sources.RemoveIf([&](Running& r) {
        const AudioSource* s = world.IsAlive(r.entity) ? world.Get<const AudioSource>(r.entity) : nullptr;
        if (!s || !m_engine.IsPlaying(r.voice)) {
            if (s == nullptr)
                m_engine.Stop(r.voice, 0.05f);
            return true;
        }
        const scene::WorldTransform* wt = world.Get<const scene::WorldTransform>(r.entity);
        const Mat4 m = wt ? wt->matrix : scene::ComputeWorldMatrix(world, r.entity);
        audio::VoiceParams p = ParamsFor(*s, m);
        if (r.hasLast && dt > 0.0f)
            p.velocity = (p.position - r.lastPosition) / dt;
        r.lastPosition = p.position;
        r.hasLast = true;
        // Occlusion: a few raycasts per frame, spread over the sources, smoothed.
        if (s->occlusion && s->spatial && occlusion && (m_frame + r.occlusionPhase) % 4 == 0) {
            const f32 target = Clamp(occlusion(listener.position, p.position), 0.0f, 1.0f);
            r.occlusion += (target - r.occlusion) * 0.5f;
        } else if (!s->occlusion) {
            r.occlusion = 0.0f;
        }
        p.occlusion = r.occlusion;
        m_engine.SetParams(r.voice, p);
        return false;
    });
    m_engine.Update(dt);
}

} // namespace zx
