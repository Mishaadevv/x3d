#include "Audio/Dsp.h"

#include <cmath>

namespace zx::audio {

// --- Biquad -------------------------------------------------------------------------------------------------------------

void Biquad::Setup(Type type, f32 sampleRate, f32 frequency, f32 q, f32 gainDb)
{
    const f64 w0 = 2.0 * 3.14159265358979323846 * Clamp(f64(frequency), 1.0, f64(sampleRate) * 0.49) / sampleRate;
    const f64 cw = std::cos(w0), sw = std::sin(w0);
    const f64 alpha = sw / (2.0 * Max(f64(q), 1e-3));
    const f64 A = std::pow(10.0, gainDb / 40.0);
    f64 b0, b1, b2, a0, a1, a2;
    switch (type) {
    case Type::LowPass:
        b0 = (1 - cw) / 2, b1 = 1 - cw, b2 = (1 - cw) / 2, a0 = 1 + alpha, a1 = -2 * cw, a2 = 1 - alpha;
        break;
    case Type::HighPass:
        b0 = (1 + cw) / 2, b1 = -(1 + cw), b2 = (1 + cw) / 2, a0 = 1 + alpha, a1 = -2 * cw, a2 = 1 - alpha;
        break;
    case Type::BandPass:
        b0 = alpha, b1 = 0, b2 = -alpha, a0 = 1 + alpha, a1 = -2 * cw, a2 = 1 - alpha;
        break;
    default: // Peak
        b0 = 1 + alpha * A, b1 = -2 * cw, b2 = 1 - alpha * A, a0 = 1 + alpha / A, a1 = -2 * cw, a2 = 1 - alpha / A;
        break;
    }
    m_b0 = f32(b0 / a0);
    m_b1 = f32(b1 / a0);
    m_b2 = f32(b2 / a0);
    m_a1 = f32(a1 / a0);
    m_a2 = f32(a2 / a0);
}

f32 Biquad::Magnitude(f32 sampleRate, f32 frequency) const
{
    // |H(e^jw)| with z = e^jw.
    const f64 w = 2.0 * 3.14159265358979323846 * frequency / sampleRate;
    const f64 c1 = std::cos(w), s1 = std::sin(w), c2 = std::cos(2 * w), s2 = std::sin(2 * w);
    const f64 nr = m_b0 + m_b1 * c1 + m_b2 * c2, ni = -(m_b1 * s1 + m_b2 * s2);
    const f64 dr = 1 + m_a1 * c1 + m_a2 * c2, di = -(m_a1 * s1 + m_a2 * s2);
    return f32(std::sqrt((nr * nr + ni * ni) / (dr * dr + di * di)));
}

// --- Freeverb -----------------------------------------------------------------------------------------------------------

namespace {
const u32 CombTuning[8] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617}; // at 44.1 kHz
const u32 AllPassTuning[4] = {556, 441, 341, 225};
constexpr u32 StereoSpread = 23;
constexpr f32 FixedGain = 0.015f;
} // namespace

void Reverb::Init(f32 sampleRate)
{
    const f32 scale = sampleRate / 44100.0f;
    for (u32 ch = 0; ch < 2; ++ch) {
        for (u32 i = 0; i < 8; ++i) {
            m_combs[ch][i].buffer.Resize(Max<u32>(1, u32(f32(CombTuning[i] + ch * StereoSpread) * scale)));
            MemZero(m_combs[ch][i].buffer.Data(), m_combs[ch][i].buffer.SizeBytes());
        }
        for (u32 i = 0; i < 4; ++i) {
            m_allPasses[ch][i].buffer.Resize(Max<u32>(1, u32(f32(AllPassTuning[i] + ch * StereoSpread) * scale)));
            MemZero(m_allPasses[ch][i].buffer.Data(), m_allPasses[ch][i].buffer.SizeBytes());
        }
    }
}

void Reverb::SetParams(f32 roomSize, f32 damping, f32 width)
{
    m_feedback = 0.7f + Clamp(roomSize, 0.0f, 1.0f) * 0.28f;
    m_damp = Clamp(damping, 0.0f, 1.0f) * 0.4f;
    m_width = Clamp(width, 0.0f, 1.0f);
}

void Reverb::Reset()
{
    for (u32 ch = 0; ch < 2; ++ch) {
        for (Comb& c : m_combs[ch]) {
            MemZero(c.buffer.Data(), c.buffer.SizeBytes());
            c.store = 0.0f;
        }
        for (AllPass& a : m_allPasses[ch])
            MemZero(a.buffer.Data(), a.buffer.SizeBytes());
    }
}

void Reverb::Process(const f32* inStereo, f32* outStereo, u32 frames, f32 wet)
{
    if (wet <= 0.0f || m_combs[0][0].buffer.Empty())
        return;
    const f32 wet1 = wet * (m_width * 0.5f + 0.5f), wet2 = wet * ((1.0f - m_width) * 0.5f);
    for (u32 f = 0; f < frames; ++f) {
        const f32 input = (inStereo[f * 2] + inStereo[f * 2 + 1]) * FixedGain;
        f32 out[2];
        for (u32 ch = 0; ch < 2; ++ch) {
            f32 acc = 0.0f;
            for (Comb& c : m_combs[ch]) {
                const f32 y = c.buffer[c.index];
                c.store = y * (1.0f - m_damp) + c.store * m_damp;
                c.buffer[c.index] = input + c.store * m_feedback;
                if (++c.index >= c.buffer.Size())
                    c.index = 0;
                acc += y;
            }
            for (AllPass& a : m_allPasses[ch]) {
                const f32 buf = a.buffer[a.index];
                const f32 y = -acc + buf;
                a.buffer[a.index] = acc + buf * 0.5f;
                if (++a.index >= a.buffer.Size())
                    a.index = 0;
                acc = y;
            }
            out[ch] = acc;
        }
        outStereo[f * 2] += out[0] * wet1 + out[1] * wet2;
        outStereo[f * 2 + 1] += out[1] * wet1 + out[0] * wet2;
    }
}

// --- Delay --------------------------------------------------------------------------------------------------------------

void Delay::Init(f32 sampleRate, f32 maxSeconds)
{
    m_sampleRate = sampleRate;
    m_buffer.Resize(usize(Max(1.0f, sampleRate * maxSeconds)) * 2);
    MemZero(m_buffer.Data(), m_buffer.SizeBytes());
    m_index = 0;
}

void Delay::SetParams(f32 seconds, f32 feedback, f32 mix)
{
    m_delayFrames = Clamp(u32(seconds * m_sampleRate), 1u, u32(m_buffer.Size() / 2));
    m_feedback = Clamp(feedback, 0.0f, 0.95f);
    m_mix = Clamp(mix, 0.0f, 1.0f);
}

void Delay::Process(f32* stereo, u32 frames)
{
    const u32 length = u32(m_buffer.Size() / 2);
    if (length == 0)
        return;
    for (u32 f = 0; f < frames; ++f) {
        const u32 read = (m_index + length - m_delayFrames) % length;
        for (u32 ch = 0; ch < 2; ++ch) {
            const f32 delayed = m_buffer[read * 2 + ch];
            const f32 dry = stereo[f * 2 + ch];
            m_buffer[m_index * 2 + ch] = dry + delayed * m_feedback;
            stereo[f * 2 + ch] = dry + delayed * m_mix;
        }
        m_index = (m_index + 1) % length;
    }
}

// --- Dynamics -----------------------------------------------------------------------------------------------------------

void Compressor::Setup(f32 sampleRate, f32 thresholdDb, f32 ratio, f32 attackMs, f32 releaseMs, f32 makeupDb, f32 kneeDb)
{
    m_threshold = thresholdDb;
    m_ratio = Max(ratio, 1.0f);
    m_knee = Max(kneeDb, 0.0f);
    m_makeup = makeupDb;
    m_attack = std::exp(-1.0f / (Max(attackMs, 0.01f) * 0.001f * sampleRate));
    m_release = std::exp(-1.0f / (Max(releaseMs, 0.01f) * 0.001f * sampleRate));
}

void Compressor::Process(f32* stereo, u32 frames)
{
    for (u32 f = 0; f < frames; ++f) {
        const f32 peak = Max(Abs(stereo[f * 2]), Abs(stereo[f * 2 + 1]));
        const f32 level = GainToDb(peak);
        // Soft-knee gain computer.
        const f32 over = level - m_threshold;
        f32 reduction = 0.0f;
        if (2.0f * over >= m_knee)
            reduction = over * (1.0f - 1.0f / m_ratio);
        else if (2.0f * over > -m_knee && m_knee > 0.0f)
            reduction = (1.0f - 1.0f / m_ratio) * (over + m_knee * 0.5f) * (over + m_knee * 0.5f) / (2.0f * m_knee);
        const f32 coef = reduction > m_envelope ? m_attack : m_release;
        m_envelope = reduction + (m_envelope - reduction) * coef;
        const f32 gain = DbToGain(m_makeup - m_envelope);
        stereo[f * 2] *= gain;
        stereo[f * 2 + 1] *= gain;
    }
    m_reductionDb = m_envelope;
}

void Limiter::Setup(f32 sampleRate, f32 ceilingDb, f32 releaseMs)
{
    m_ceiling = DbToGain(ceilingDb);
    m_release = 1.0f - std::exp(-1.0f / (Max(releaseMs, 1.0f) * 0.001f * sampleRate));
    m_gain = 1.0f;
}

void Limiter::Process(f32* stereo, u32 frames)
{
    for (u32 f = 0; f < frames; ++f) {
        const f32 peak = Max(Abs(stereo[f * 2]), Abs(stereo[f * 2 + 1]));
        const f32 needed = peak * m_gain > m_ceiling ? m_ceiling / peak : 1.0f;
        if (needed < m_gain)
            m_gain = needed;                       // instant attack: never above the ceiling
        else
            m_gain += (Min(needed, 1.0f) - m_gain) * m_release; // smooth recovery
        stereo[f * 2] = Clamp(stereo[f * 2] * m_gain, -m_ceiling, m_ceiling);
        stereo[f * 2 + 1] = Clamp(stereo[f * 2 + 1] * m_gain, -m_ceiling, m_ceiling);
    }
}

} // namespace zx::audio
