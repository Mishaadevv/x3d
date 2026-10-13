#pragma once

// Small DSP building blocks used by the mixer: filters, reverb, delay, dynamics. All process 48 kHz float audio.

#include "Core/Containers/Array.h"
#include "Core/Math/MathCommon.h"

namespace zx::audio {

inline f32 DbToGain(f32 db) { return Pow(10.0f, db / 20.0f); }
inline f32 GainToDb(f32 gain) { return gain > 1e-9f ? 20.0f * std::log10(gain) : -180.0f; }

/// RBJ cookbook biquad (direct form I), one channel.
class Biquad
{
public:
    enum class Type : u8
    {
        LowPass,
        HighPass,
        BandPass,
        Peak,
    };
    void Setup(Type type, f32 sampleRate, f32 frequency, f32 q = 0.7071f, f32 gainDb = 0.0f);
    f32 Process(f32 x)
    {
        const f32 y = m_b0 * x + m_b1 * m_x1 + m_b2 * m_x2 - m_a1 * m_y1 - m_a2 * m_y2;
        m_x2 = m_x1;
        m_x1 = x;
        m_y2 = m_y1;
        m_y1 = y;
        return y;
    }
    void Reset() { m_x1 = m_x2 = m_y1 = m_y2 = 0.0f; }
    /// Magnitude response at `frequency` (tests).
    f32 Magnitude(f32 sampleRate, f32 frequency) const;

private:
    f32 m_b0 = 1.0f, m_b1 = 0.0f, m_b2 = 0.0f, m_a1 = 0.0f, m_a2 = 0.0f;
    f32 m_x1 = 0.0f, m_x2 = 0.0f, m_y1 = 0.0f, m_y2 = 0.0f;
};

/// One-pole low-pass with a cheap coefficient - smooth when the cutoff changes every block (occlusion).
struct OnePoleLowPass
{
    f32 state[2] = {0.0f, 0.0f};
    static f32 Coefficient(f32 sampleRate, f32 cutoff) { return 1.0f - std::exp(-TwoPi * Min(cutoff, sampleRate * 0.49f) / sampleRate); }
};

/// Freeverb (Jezar): 8 parallel combs + 4 series all-passes per channel, stereo spread.
class Reverb
{
public:
    void Init(f32 sampleRate);
    void SetParams(f32 roomSize, f32 damping, f32 width = 1.0f); // 0..1
    /// Adds the reverberated input (mono-summed `in`) to `out` (stereo interleaved) scaled by `wet`.
    void Process(const f32* inStereo, f32* outStereo, u32 frames, f32 wet);
    void Reset();

private:
    struct Comb
    {
        Array<f32> buffer;
        u32 index = 0;
        f32 store = 0.0f;
    };
    struct AllPass
    {
        Array<f32> buffer;
        u32 index = 0;
    };
    Comb m_combs[2][8];
    AllPass m_allPasses[2][4];
    f32 m_feedback = 0.84f, m_damp = 0.2f, m_width = 1.0f;
};

/// Feedback delay (echo), stereo.
class Delay
{
public:
    void Init(f32 sampleRate, f32 maxSeconds = 2.0f);
    void SetParams(f32 seconds, f32 feedback, f32 mix);
    void Process(f32* stereo, u32 frames);

private:
    Array<f32> m_buffer; // interleaved
    u32 m_index = 0, m_delayFrames = 1;
    f32 m_feedback = 0.3f, m_mix = 0.3f, m_sampleRate = 48000.0f;
};

/// Feed-forward compressor with peak detection, soft knee and make-up gain; a limiter is ratio = inf.
class Compressor
{
public:
    void Setup(f32 sampleRate, f32 thresholdDb, f32 ratio, f32 attackMs, f32 releaseMs, f32 makeupDb = 0.0f, f32 kneeDb = 6.0f);
    void Process(f32* stereo, u32 frames);
    f32 GainReductionDb() const { return m_reductionDb; }

private:
    f32 m_threshold = -12.0f, m_ratio = 4.0f, m_knee = 6.0f, m_makeup = 0.0f;
    f32 m_attack = 0.0f, m_release = 0.0f, m_envelope = 0.0f, m_reductionDb = 0.0f;
};

/// Brick-wall peak limiter for the master bus (instant attack, smooth release, hard ceiling).
class Limiter
{
public:
    void Setup(f32 sampleRate, f32 ceilingDb = -0.3f, f32 releaseMs = 80.0f);
    void Process(f32* stereo, u32 frames);
    f32 GainReductionDb() const { return GainToDb(m_gain); }

private:
    f32 m_ceiling = 0.966f, m_release = 0.0f, m_gain = 1.0f;
};

} // namespace zx::audio
