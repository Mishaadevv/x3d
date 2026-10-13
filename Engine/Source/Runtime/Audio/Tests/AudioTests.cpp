#include "Audio/AudioEngine.h"
#include "Core/FileSystem/FileSystem.h"
#include "Core/CoreInit.h"
#include "Core/Math/Math.h"
#include "Core/Testing/Test.h"

#include <cmath>

using namespace zx;
using namespace zx::audio;

namespace {

PcmData Sine(f32 frequency, f32 seconds, u32 rate = 48000, u32 channels = 1, f32 amplitude = 0.5f)
{
    PcmData p;
    p.sampleRate = rate;
    p.channels = channels;
    const u32 frames = u32(seconds * f32(rate));
    p.samples.Resize(usize(frames) * channels);
    for (u32 f = 0; f < frames; ++f)
        for (u32 c = 0; c < channels; ++c)
            p.samples[usize(f) * channels + c] = amplitude * std::sin(TwoPi * frequency * f32(f) / f32(rate));
    return p;
}

RefPtr<AudioClip> ClipFrom(const PcmData& p)
{
    RefPtr<AudioClip> c(New<AudioClip>());
    c->sampleRate = p.sampleRate;
    c->channels = p.channels;
    c->samples = p.samples;
    c->frameCount = p.samples.Size() / p.channels;
    return c;
}

f32 Rms(const Array<f32>& stereo, u32 channel, usize from = 0, usize to = ~usize(0))
{
    f64 acc = 0.0;
    usize n = 0;
    for (usize f = from; f < stereo.Size() / 2 && f < to; ++f, ++n)
        acc += f64(stereo[f * 2 + channel]) * stereo[f * 2 + channel];
    return n ? f32(std::sqrt(acc / f64(n))) : 0.0f;
}

/// Largest jump between neighbouring samples (a click shows up as a jump far above what the signal itself does).
f32 MaxStep(const Array<f32>& stereo)
{
    f32 m = 0.0f;
    for (usize i = 2; i < stereo.Size(); ++i)
        m = Max(m, Abs(stereo[i] - stereo[i - 2]));
    return m;
}

u32 ZeroCrossings(const Array<f32>& stereo, u32 channel, usize from, usize to)
{
    u32 n = 0;
    for (usize f = from + 1; f < to; ++f)
        n += (stereo[(f - 1) * 2 + channel] < 0.0f) != (stereo[f * 2 + channel] < 0.0f) ? 1u : 0u;
    return n;
}

struct OfflineMixer
{
    AudioEngine engine;
    OfflineMixer(u32 maxAudible = 64)
    {
        AudioSettings s;
        s.backend = AudioSettings::Backend::Null;
        s.maxAudibleVoices = maxAudible;
        engine.Initialize(s);
    }
    Array<f32> Render(f32 seconds, bool update = true)
    {
        Array<f32> out;
        const u32 frames = u32(seconds * f32(MixRate));
        out.Resize(usize(frames) * 2);
        for (u32 done = 0; done < frames; done += 480) { // 10 ms "frames" of the game
            if (update)
                engine.Update(0.01f);
            engine.RenderOffline(out.Data() + usize(done) * 2, Min(480u, frames - done));
        }
        return out;
    }
};

/// Minimal IMA ADPCM encoder (mono) for round-trip tests.
Array<u8> EncodeImaWav(const PcmData& pcm, u32 blockAlign = 256)
{
    static const i32 Steps[89] = {7,    8,    9,    10,   11,   12,   13,   14,   16,   17,   19,   21,   23,   25,   28,   31,   34,   37,
                                  41,   45,   50,   55,   60,   66,   73,   80,   88,   97,   107,  118,  130,  143,  157,  173,  190,  209,
                                  230,  253,  279,  307,  337,  371,  408,  449,  494,  544,  598,  658,  724,  796,  876,  963,  1060, 1166,
                                  1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484,
                                  7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
    static const i32 Index[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};
    const u32 framesPerBlock = (blockAlign - 4) * 2 + 1;
    Array<u8> data;
    i32 predictor = 0, index = 0;
    usize f = 0;
    const usize total = pcm.samples.Size();
    auto toI16 = [&](usize i) { return i < total ? i32(Clamp(pcm.samples[i] * 32767.0f, -32768.0f, 32767.0f)) : 0; };
    while (f < total) {
        const usize start = data.Size();
        data.Resize(start + blockAlign);
        u8* b = data.Data() + start;
        predictor = toI16(f);
        const i16 p16 = i16(predictor);
        MemCopy(b, &p16, 2);
        b[2] = u8(index);
        b[3] = 0;
        ++f;
        for (u32 i = 0; i < framesPerBlock - 1; ++i, ++f) {
            const i32 diff0 = toI16(f) - predictor;
            const i32 step = Steps[index];
            u8 nibble = diff0 < 0 ? 8 : 0;
            i32 diff = Abs(diff0), vpdiff = step >> 3;
            if (diff >= step) { nibble |= 4; diff -= step; vpdiff += step; }
            if (diff >= step >> 1) { nibble |= 2; diff -= step >> 1; vpdiff += step >> 1; }
            if (diff >= step >> 2) { nibble |= 1; vpdiff += step >> 2; }
            predictor = Clamp(predictor + ((nibble & 8) ? -vpdiff : vpdiff), -32768, 32767);
            index = Clamp(index + Index[nibble], 0, 88);
            u8& byte = b[4 + i / 2];
            byte = (i & 1) ? u8(byte | (nibble << 4)) : nibble;
        }
    }
    // RIFF header (fmt with IMA tag 0x11).
    Array<u8> out;
    auto put = [&](const void* p, usize n) { out.Append(Span<const u8>(static_cast<const u8*>(p), n)); };
    auto put32 = [&](u32 v) { put(&v, 4); };
    auto put16 = [&](u16 v) { put(&v, 2); };
    put("RIFF", 4);
    put32(u32(4 + 8 + 20 + 8 + data.Size()));
    put("WAVEfmt ", 8);
    put32(20);
    put16(0x11);
    put16(1);
    put32(pcm.sampleRate);
    put32(pcm.sampleRate * blockAlign / framesPerBlock);
    put16(u16(blockAlign));
    put16(4);
    put16(2);
    put16(u16(framesPerBlock));
    put("data", 4);
    put32(u32(data.Size()));
    put(data.Data(), data.Size());
    return out;
}

} // namespace

// --- Decoders -------------------------------------------------------------------------------------------------------------

ZX_TEST(Audio, WavRoundTrip)
{
    const PcmData src = Sine(440.0f, 0.1f, 44100, 2);
    for (bool f32Format : {false, true}) {
        const Array<u8> bytes = EncodeWav(src, f32Format);
        Result<PcmData> back = DecodeWav(bytes.View());
        ZX_REQUIRE(back.IsOk());
        ZX_EXPECT_EQ(back->sampleRate, 44100u);
        ZX_EXPECT_EQ(back->channels, 2u);
        ZX_REQUIRE(back->samples.Size() == src.samples.Size());
        f32 err = 0.0f;
        for (usize i = 0; i < src.samples.Size(); ++i)
            err = Max(err, Abs(back->samples[i] - src.samples[i]));
        ZX_EXPECT_LT(err, f32Format ? 1e-7f : 1.0f / 30000.0f);
    }
    // Garbage and unsupported files fail cleanly.
    const u8 junk[16] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E'};
    ZX_EXPECT(!DecodeWav(Span<const u8>(junk, 16)).IsOk());
    ZX_EXPECT(!DecodeWav(Span<const u8>(junk, 4)).IsOk());
}

ZX_TEST(Audio, WavImaAdpcm)
{
    const PcmData src = Sine(300.0f, 0.25f, 22050, 1, 0.6f);
    const Array<u8> bytes = EncodeImaWav(src);
    Result<PcmData> back = DecodeWav(bytes.View());
    ZX_REQUIRE(back.IsOk());
    ZX_EXPECT_EQ(back->channels, 1u);
    ZX_REQUIRE(back->samples.Size() >= src.samples.Size());
    // ADPCM is lossy: check the signal-to-noise ratio.
    f64 signal = 0.0, noise = 0.0;
    for (usize i = 0; i < src.samples.Size(); ++i) {
        signal += f64(src.samples[i]) * src.samples[i];
        noise += f64(back->samples[i] - src.samples[i]) * (back->samples[i] - src.samples[i]);
    }
    ZX_EXPECT_GT(10.0 * std::log10(signal / Max(noise, 1e-20)), 25.0);
}

// --- DSP ------------------------------------------------------------------------------------------------------------------

ZX_TEST(Audio, DspFiltersAndDynamics)
{
    Biquad lp;
    lp.Setup(Biquad::Type::LowPass, 48000.0f, 1000.0f);
    ZX_EXPECT_NEAR(lp.Magnitude(48000.0f, 1000.0f), 0.7071f, 0.01f);
    ZX_EXPECT_GT(lp.Magnitude(48000.0f, 100.0f), 0.99f);
    ZX_EXPECT_LT(lp.Magnitude(48000.0f, 10000.0f), 0.02f);
    Biquad hp;
    hp.Setup(Biquad::Type::HighPass, 48000.0f, 1000.0f);
    ZX_EXPECT_LT(hp.Magnitude(48000.0f, 100.0f), 0.02f);

    // Limiter: a +6 dB sine never exceeds the ceiling.
    Limiter lim;
    lim.Setup(48000.0f, -1.0f, 50.0f);
    Array<f32> loud;
    loud.Resize(48000 * 2);
    for (u32 f = 0; f < 48000; ++f)
        loud[f * 2] = loud[f * 2 + 1] = 2.0f * std::sin(TwoPi * 200.0f * f32(f) / 48000.0f);
    lim.Process(loud.Data(), 48000);
    f32 peak = 0.0f;
    for (f32 s : loud)
        peak = Max(peak, Abs(s));
    ZX_EXPECT_LE(peak, DbToGain(-1.0f) + 1e-5f);

    // Reverb: a click leaves a tail.
    Reverb rv;
    rv.Init(48000.0f);
    rv.SetParams(0.8f, 0.3f);
    Array<f32> in, out;
    in.Resize(48000 * 2);
    out.Resize(48000 * 2);
    in[0] = in[1] = 1.0f;
    rv.Process(in.Data(), out.Data(), 48000, 1.0f);
    ZX_EXPECT_GT(Rms(out, 0, 12000, 24000), 1e-4f);

    // Compressor: a loud steady tone ends up quieter than it went in.
    Compressor comp;
    comp.Setup(48000.0f, -20.0f, 4.0f, 5.0f, 100.0f);
    Array<f32> tone;
    tone.Resize(48000 * 2);
    for (u32 f = 0; f < 48000; ++f)
        tone[f * 2] = tone[f * 2 + 1] = 0.9f * std::sin(TwoPi * 300.0f * f32(f) / 48000.0f);
    const f32 before = Rms(tone, 0, 24000);
    comp.Process(tone.Data(), 48000);
    ZX_EXPECT_LT(Rms(tone, 0, 24000), before * 0.5f);
    ZX_EXPECT_GT(comp.GainReductionDb(), 6.0f);
}

// --- Mixer ----------------------------------------------------------------------------------------------------------------

ZX_TEST(Audio, MixerLevelsPanAndPitch)
{
    OfflineMixer m;
    const RefPtr<AudioClip> tone = ClipFrom(Sine(1000.0f, 2.0f, 48000, 1, 0.5f));
    VoiceParams p;
    p.volume = 0.5f;
    p.loop = true;
    const VoiceId v = m.engine.Play(tone, p);
    ZX_REQUIRE(v != 0);
    Array<f32> out = m.Render(0.5f);
    // Center: equal channels, amplitude 0.25 -> RMS 0.177.
    ZX_EXPECT_NEAR(Rms(out, 0, 4800), 0.25f / Sqrt(2.0f), 0.01f);
    ZX_EXPECT_NEAR(Rms(out, 1, 4800), Rms(out, 0, 4800), 1e-4f);
    // 1 kHz -> ~2000 zero crossings per second.
    ZX_EXPECT_NEAR(f32(ZeroCrossings(out, 0, 4800, 24000)) / 0.4f, 2000.0f, 20.0f);

    // Hard right, double pitch.
    p.pan = 1.0f;
    p.pitch = 2.0f;
    m.engine.SetParams(v, p);
    out = m.Render(0.5f);
    ZX_EXPECT_LT(Rms(out, 0, 4800), 0.002f);
    ZX_EXPECT_GT(Rms(out, 1, 4800), 0.2f);
    ZX_EXPECT_NEAR(f32(ZeroCrossings(out, 1, 4800, 24000)) / 0.4f, 4000.0f, 40.0f);
}

ZX_TEST(Audio, MixerEndsStopsAndNeverClicks)
{
    OfflineMixer m;
    // A short one-shot ends by itself and leaves the voice list.
    const VoiceId shot = m.engine.Play(ClipFrom(Sine(500.0f, 0.05f)));
    m.Render(0.2f);
    ZX_EXPECT(!m.engine.IsPlaying(shot));
    ZX_EXPECT_EQ(m.engine.GetStats().voices, 0u);

    // Stopping a loud looping tone fades out instead of cutting: no jump larger than the tone's own slope.
    const RefPtr<AudioClip> tone = ClipFrom(Sine(200.0f, 1.0f, 48000, 1, 0.9f));
    VoiceParams lp;
    lp.loop = true;
    const VoiceId v = m.engine.Play(tone, lp);
    Array<f32> out = m.Render(0.25f);
    m.engine.Stop(v, 0.01f);
    Array<f32> tail = m.Render(0.1f);
    out.Append(tail.View());
    const f32 toneSlope = 0.9f * TwoPi * 200.0f / 48000.0f; // max per-sample change of the tone itself
    ZX_EXPECT_LT(MaxStep(out), toneSlope * 1.5f);
    ZX_EXPECT_LT(Rms(tail, 0, 2400), 1e-6f);
}

ZX_TEST(Audio, Mixer3DAndDoppler)
{
    OfflineMixer m;
    m.engine.SetListener(Vec3(0.0f), Vec3(0, 0, 1), Vec3(0, 1, 0));
    const RefPtr<AudioClip> tone = ClipFrom(Sine(1000.0f, 1.0f, 48000, 1, 0.5f));
    VoiceParams p;
    p.spatial = true;
    p.loop = true;
    p.position = Vec3(10, 0, 0); // to the right
    p.minDistance = 1.0f;
    p.maxDistance = 100.0f;
    p.dopplerScale = 0.0f;
    const VoiceId v = m.engine.Play(tone, p);
    Array<f32> out = m.Render(0.3f);
    // Inverse rolloff: gain 0.1 at 10 m; fully right -> right channel carries sqrt(2) * 0.1 of the constant-power pan.
    ZX_EXPECT_LT(Rms(out, 0, 4800), 0.003f);
    ZX_EXPECT_NEAR(Rms(out, 1, 4800), 0.5f / Sqrt(2.0f) * 0.1f * Sqrt(2.0f), 0.01f);

    // Approaching at 34.3 m/s: pitch up by ~10 %.
    p.position = Vec3(0, 0, 20);
    p.velocity = Vec3(0, 0, -34.3f);
    p.dopplerScale = 1.0f;
    m.engine.SetParams(v, p);
    out = m.Render(0.5f);
    const f32 freq = f32(ZeroCrossings(out, 0, 4800, 24000)) / 0.4f / 2.0f;
    ZX_EXPECT_NEAR(freq, 1000.0f * 343.0f / (343.0f - 34.3f), 15.0f);
}

ZX_TEST(Audio, VirtualizationAnd64SourcesWithoutClicks)
{
    // Acceptance (M9): 64 moving 3D sources play without clicks; beyond the limit voices go virtual.
    OfflineMixer m(64);
    m.engine.SetListener(Vec3(0.0f), Vec3(0, 0, 1), Vec3(0, 1, 0));
    Array<RefPtr<AudioClip>> clips;
    for (u32 i = 0; i < 4; ++i)
        clips.PushBack(ClipFrom(Sine(150.0f + 70.0f * f32(i), 1.0f, 44100, 1, 0.3f)));
    Array<VoiceId> voices;
    for (u32 i = 0; i < 80; ++i) {
        VoiceParams p;
        p.spatial = true;
        p.loop = true;
        p.volume = 0.2f;
        p.position = Vec3(Cos(f32(i)) * (2.0f + f32(i % 10)), 0.0f, Sin(f32(i)) * (2.0f + f32(i % 10)));
        voices.PushBack(m.engine.Play(clips[i % 4], p));
    }
    Array<f32> out;
    f32 t = 0.0f;
    for (u32 frame = 0; frame < 100; ++frame) { // 1 s at 100 updates/s, sources circling the listener
        t += 0.01f;
        for (u32 i = 0; i < voices.Size(); ++i) {
            VoiceParams p = *m.engine.GetParams(voices[i]);
            const f32 a = f32(i) + t * (1.0f + f32(i % 7));
            const f32 r = 2.0f + f32(i % 10);
            p.position = Vec3(Cos(a) * r, 0.0f, Sin(a) * r);
            m.engine.SetParams(voices[i], p);
        }
        m.engine.Update(0.01f);
        Array<f32> chunk;
        chunk.Resize(480 * 2);
        m.engine.RenderOffline(chunk.Data(), 480);
        out.Append(chunk.View());
    }
    const AudioStats stats = m.engine.GetStats();
    ZX_EXPECT_EQ(stats.voices, 80u);
    ZX_EXPECT_EQ(stats.audible, 64u);
    ZX_EXPECT_EQ(stats.virtualVoices, 16u);
    // No sample jumps beyond what the summed tones themselves can do in one sample (with the limiter engaged).
    f32 peak = 0.0f;
    for (f32 s : out)
        peak = Max(peak, Abs(s));
    ZX_EXPECT_LE(peak, 0.97f);
    ZX_EXPECT_LT(MaxStep(out), 0.12f);
}

ZX_TEST(Audio, BusesSnapshotsAndDucking)
{
    OfflineMixer m;
    const RefPtr<AudioClip> tone = ClipFrom(Sine(400.0f, 1.0f, 48000, 1, 0.4f));
    VoiceParams music;
    music.bus = Bus::Music;
    music.loop = true;
    m.engine.Play(tone, music);
    m.engine.SetDucking(false);
    const f32 full = Rms(m.Render(0.3f), 0, 4800);
    ZX_EXPECT_GT(full, 0.2f);
    m.engine.SetBusMuted(Bus::Music, true);
    ZX_EXPECT_LT(Rms(m.Render(0.2f), 0, 4800), 1e-5f);
    m.engine.SetBusMuted(Bus::Music, false);

    // Snapshot to half volume over 0.2 s.
    MixSnapshot quiet;
    quiet.busVolume[u32(Bus::Music)] = 0.5f;
    m.engine.ApplySnapshot(quiet, 0.2f);
    const f32 half = Rms(m.Render(0.5f), 0, 12000); // after the 0.2 s blend
    ZX_EXPECT_NEAR(half / full, 0.5f, 0.03f);
    ZX_EXPECT_NEAR(m.engine.GetBusVolume(Bus::Music), 0.5f, 1e-4f);

    // Ducking: dialogue on the Voice bus lowers the music.
    m.engine.SetBusVolume(Bus::Music, 1.0f);
    m.engine.SetDucking(true, -12.0f);
    VoiceParams speech;
    speech.bus = Bus::Voice;
    speech.loop = true;
    speech.volume = 0.0001f; // quiet so the measurement is the music
    const VoiceId talk = m.engine.Play(ClipFrom(Sine(800.0f, 1.0f, 48000, 1, 0.9f)), speech);
    VoiceParams sp = speech;
    sp.volume = 0.3f;
    m.engine.SetParams(talk, sp);
    m.Render(0.6f);
    m.engine.SetParams(talk, speech);
    // Music is reduced while the voice bus is active (meter-based detection lags one update).
    ZX_EXPECT_LT(m.engine.GetBusPeak(Bus::Music), 0.4f * 0.6f);
}

// --- Ogg Vorbis -----------------------------------------------------------------------------------------------------------

namespace {

Array<u8> ReadTestFile(StringView name)
{
    Array<u8> bytes;
    fs::ReadFile(FindEngineRootDirectory() + "/Tests/Data/Audio/" + name, bytes);
    return bytes;
}

} // namespace

ZX_TEST(Audio, VorbisMatchesLibvorbis)
{
    // References: Tests/Data/Audio/*.ref.wav, decoded by libvorbis (Tools/gen_audio_testdata.py), 16-bit.
    for (StringView name : {StringView("mono_44k_q4"), StringView("stereo_48k_q8"), StringView("stereo_22k_q0")}) {
        const Array<u8> ogg = ReadTestFile(String::Format("{}.ogg", name));
        const Array<u8> refWav = ReadTestFile(String::Format("{}.ref.wav", name));
        ZX_REQUIRE(!ogg.Empty() && !refWav.Empty());
        Result<PcmData> ours = DecodeVorbis(ogg.View());
        if (!ours) {
            test::ReportFailure(__FILE__, __LINE__, String::Format("{}: {}", name, ours.GetError().message));
            continue;
        }
        Result<PcmData> ref = DecodeWav(refWav.View());
        ZX_REQUIRE(ref.IsOk());
        ZX_EXPECT_EQ(ours->channels, ref->channels);
        ZX_EXPECT_EQ(ours->sampleRate, ref->sampleRate);
        ZX_EXPECT_EQ(ours->samples.Size(), ref->samples.Size());
        const usize n = Min(ours->samples.Size(), ref->samples.Size());
        f32 maxErr = 0.0f;
        f64 dot = 0.0, energy = 0.0;
        for (usize i = 0; i < n; ++i) {
            maxErr = Max(maxErr, Abs(Clamp(ours->samples[i], -1.0f, 1.0f) - ref->samples[i])); // the 16-bit reference clips
            dot += f64(ours->samples[i]) * ref->samples[i];
            energy += f64(ref->samples[i]) * ref->samples[i];
        }
        if (maxErr > 2.0f / 32768.0f)
            test::ReportFailure(__FILE__, __LINE__,
                                String::Format("{}: max error {} (gain ratio {:.4f}, {} vs {} samples)", name, maxErr, energy > 0 ? dot / energy : 0.0,
                                               ours->samples.Size(), ref->samples.Size()));
    }
}

ZX_TEST(Audio, VorbisStreamingAndClips)
{
    const Array<u8> ogg = ReadTestFile("stereo_48k_q8.ogg");
    ZX_REQUIRE(!ogg.Empty());
    // Streaming in odd-sized chunks gives the same samples as decoding at once; Rewind starts over.
    Result<PcmData> whole = DecodeVorbis(ogg.View());
    ZX_REQUIRE(whole.IsOk());
    Result<UniquePtr<StreamDecoder>> dec = CreateVorbisDecoder(ogg.View());
    ZX_REQUIRE(dec.IsOk());
    Array<f32> streamed;
    f32 buf[333 * 2];
    for (u32 n; (n = (*dec)->Read(buf, 333)) > 0;)
        streamed.Append(Span<const f32>(buf, usize(n) * 2));
    ZX_EXPECT(streamed.Size() == whole->samples.Size());
    ZX_EXPECT((*dec)->Rewind());
    const u32 again = (*dec)->Read(buf, 333);
    ZX_EXPECT_EQ(again, 333u);
    ZX_EXPECT_EQ(buf[100], whole->samples[100]);

    // Clips: short files are decoded on load, long ones stream.
    Result<RefPtr<AudioClip>> shortClip = LoadAudioClip(ogg.View(), "short.ogg");
    ZX_REQUIRE(shortClip.IsOk());
    ZX_EXPECT((*shortClip)->format == ClipFormat::Pcm);
    ZX_EXPECT_EQ((*shortClip)->frameCount, u64(24000));
    ClipLoadOptions streamAll;
    streamAll.streamLongerThan = 0.1;
    Result<RefPtr<AudioClip>> music = LoadAudioClip(ogg.View(), "music.ogg", streamAll);
    ZX_REQUIRE(music.IsOk());
    ZX_EXPECT((*music)->IsStreamed());

    // Streaming music through the mixer: plays, loops seamlessly, reports one stream.
    OfflineMixer m;
    VoiceParams p;
    p.bus = Bus::Music;
    p.loop = true;
    m.engine.Play(*music, p);
    Array<f32> out = m.Render(1.2f); // the file is 0.5 s: two loops
    ZX_EXPECT_EQ(m.engine.GetStats().streams, 1u);
    ZX_EXPECT_GT(Rms(out, 0, 4800, 48000), 0.05f);
    ZX_EXPECT_GT(Rms(out, 0, 48000), 0.05f); // still playing after the first loop
}

ZX_TEST(ManualAudio, DumpVorbis)
{
    // Writes our decodes next to the references for inspection: Saved/AudioDump/<name>.wav
    for (StringView name : {StringView("mono_44k_q4"), StringView("stereo_48k_q8"), StringView("stereo_22k_q0")}) {
        Result<PcmData> ours = DecodeVorbis(ReadTestFile(String::Format("{}.ogg", name)).View());
        if (ours)
            fs::WriteFile(String::Format("{}/Saved/AudioDump/{}.wav", FindEngineRootDirectory(), name), EncodeWav(*ours, true).View());
    }
}
