// WAV (RIFF/WAVE) decoding: PCM 8/16/24/32-bit, IEEE float 32/64, IMA ADPCM, Microsoft ADPCM; clip loading.

#include "Audio/AudioClip.h"
#include "Core/FileSystem/FileSystem.h"
#include "Core/Memory/Memory.h"

namespace zx::audio {

namespace {

constexpr u16 FormatPcm = 1;
constexpr u16 FormatMsAdpcm = 2;
constexpr u16 FormatFloat = 3;
constexpr u16 FormatImaAdpcm = 0x11;
constexpr u16 FormatExtensible = 0xFFFE;

u16 Read16(const u8* p) { return u16(p[0] | (p[1] << 8)); }
u32 Read32(const u8* p) { return u32(p[0]) | (u32(p[1]) << 8) | (u32(p[2]) << 16) | (u32(p[3]) << 24); }
i16 ReadI16(const u8* p) { return i16(Read16(p)); }

// --- IMA ADPCM ---------------------------------------------------------------------------------------------------------

const i32 ImaStepTable[89] = {7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,    25,    28,
                              31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
                              130,   143,   157,   173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,   494,
                              544,   598,   658,   724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
                              2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,
                              9493,  10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
const i32 ImaIndexTable[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

struct ImaState
{
    i32 predictor = 0;
    i32 index = 0;
    i16 Decode(u8 nibble)
    {
        const i32 step = ImaStepTable[index];
        i32 diff = step >> 3;
        if (nibble & 1)
            diff += step >> 2;
        if (nibble & 2)
            diff += step >> 1;
        if (nibble & 4)
            diff += step;
        predictor += (nibble & 8) ? -diff : diff;
        predictor = Clamp(predictor, -32768, 32767);
        index = Clamp(index + ImaIndexTable[nibble], 0, 88);
        return i16(predictor);
    }
};

bool DecodeImaAdpcm(const u8* data, usize size, u32 channels, u32 blockAlign, Array<f32>& out)
{
    if (blockAlign < 4 * channels)
        return false;
    // Per block: a 4-byte header per channel, then 4-byte groups per channel of 8 nibbles each.
    const u32 framesPerBlock = (blockAlign - 4 * channels) * 2 / channels + 1;
    for (usize offset = 0; offset + blockAlign <= size; offset += blockAlign) {
        const u8* b = data + offset;
        ImaState st[2];
        const usize base = out.Size();
        out.Resize(base + usize(framesPerBlock) * channels);
        for (u32 c = 0; c < channels; ++c) {
            st[c].predictor = ReadI16(b + c * 4);
            st[c].index = Clamp(i32(b[c * 4 + 2]), 0, 88);
            out[base + c] = f32(st[c].predictor) / 32768.0f;
        }
        const u8* p = b + 4 * channels;
        u32 frame = 1;
        while (frame < framesPerBlock && p + 4 * channels <= b + blockAlign) {
            for (u32 c = 0; c < channels; ++c) {
                for (u32 i = 0; i < 8; ++i) {
                    const u8 byte = p[c * 4 + i / 2];
                    const u8 nibble = (i & 1) ? (byte >> 4) : (byte & 0x0F);
                    if (frame + i < framesPerBlock)
                        out[base + usize(frame + i) * channels + c] = f32(st[c].Decode(nibble)) / 32768.0f;
                }
            }
            p += 4 * channels;
            frame += 8;
        }
    }
    return true;
}

// --- Microsoft ADPCM ----------------------------------------------------------------------------------------------------

const i32 MsAdaptTable[16] = {230, 230, 230, 230, 307, 409, 512, 614, 768, 614, 512, 409, 307, 230, 230, 230};

bool DecodeMsAdpcm(const u8* data, usize size, u32 channels, u32 blockAlign, const i32 (*coefs)[2], u32 numCoefs, Array<f32>& out)
{
    if (blockAlign < 7 * channels)
        return false;
    const u32 framesPerBlock = (blockAlign - 7 * channels) * 2 / channels + 2;
    for (usize offset = 0; offset + blockAlign <= size; offset += blockAlign) {
        const u8* b = data + offset;
        i32 predictor[2], delta[2], s1[2], s2[2];
        for (u32 c = 0; c < channels; ++c)
            predictor[c] = Min(u32(b[c]), numCoefs - 1);
        const u8* p = b + channels;
        for (u32 c = 0; c < channels; ++c)
            delta[c] = ReadI16(p + c * 2);
        p += 2 * channels;
        for (u32 c = 0; c < channels; ++c)
            s1[c] = ReadI16(p + c * 2);
        p += 2 * channels;
        for (u32 c = 0; c < channels; ++c)
            s2[c] = ReadI16(p + c * 2);
        p += 2 * channels;
        const usize base = out.Size();
        out.Resize(base + usize(framesPerBlock) * channels);
        for (u32 c = 0; c < channels; ++c) {
            out[base + c] = f32(s2[c]) / 32768.0f; // the older sample comes first
            out[base + channels + c] = f32(s1[c]) / 32768.0f;
        }
        u32 nibbleIndex = 0;
        for (u32 frame = 2; frame < framesPerBlock; ++frame) {
            for (u32 c = 0; c < channels; ++c) {
                const usize byteIndex = nibbleIndex / 2;
                if (p + byteIndex >= b + blockAlign)
                    break;
                const u8 byte = p[byteIndex];
                i32 nibble = (nibbleIndex & 1) ? (byte & 0x0F) : (byte >> 4);
                ++nibbleIndex;
                const i32 signedNibble = nibble >= 8 ? nibble - 16 : nibble;
                const i32 c1 = coefs[predictor[c]][0], c2 = coefs[predictor[c]][1];
                i32 sample = ((s1[c] * c1) + (s2[c] * c2)) / 256 + signedNibble * delta[c];
                sample = Clamp(sample, -32768, 32767);
                s2[c] = s1[c];
                s1[c] = sample;
                delta[c] = Max((MsAdaptTable[nibble] * delta[c]) / 256, 16);
                out[base + usize(frame) * channels + c] = f32(sample) / 32768.0f;
            }
        }
    }
    return true;
}

/// More than two channels: average into stereo (front pairs dominate); mono and stereo pass through.
void DownmixToStereo(PcmData& pcm)
{
    if (pcm.channels <= 2)
        return;
    const usize frames = pcm.samples.Size() / pcm.channels;
    Array<f32> out;
    out.Resize(frames * 2);
    for (usize f = 0; f < frames; ++f) {
        const f32* in = pcm.samples.Data() + f * pcm.channels;
        f32 l = 0.0f, r = 0.0f;
        for (u32 c = 0; c < pcm.channels; ++c) {
            if (c % 2 == 0)
                l += in[c];
            else
                r += in[c];
        }
        const f32 norm = 2.0f / f32(pcm.channels);
        out[f * 2] = l * norm;
        out[f * 2 + 1] = r * norm;
    }
    pcm.samples = move(out);
    pcm.channels = 2;
}

} // namespace

Result<PcmData> DecodeWav(Span<const u8> data)
{
    if (data.Size() < 12 || MemCompare(data.Data(), "RIFF", 4) != 0 || MemCompare(data.Data() + 8, "WAVE", 4) != 0)
        return Error::Make(ErrorCode::InvalidData, "Not a RIFF/WAVE file");
    u16 format = 0, channels = 0, bits = 0, blockAlign = 0;
    u32 sampleRate = 0;
    i32 msCoefs[256][2];
    u32 numCoefs = 0;
    const u8* pcmData = nullptr;
    usize pcmSize = 0;
    usize pos = 12;
    while (pos + 8 <= data.Size()) {
        const u8* chunk = data.Data() + pos;
        const u32 size = Read32(chunk + 4);
        const usize avail = Min<usize>(size, data.Size() - pos - 8);
        const u8* body = chunk + 8;
        if (MemCompare(chunk, "fmt ", 4) == 0 && avail >= 16) {
            format = Read16(body);
            channels = Read16(body + 2);
            sampleRate = Read32(body + 4);
            blockAlign = Read16(body + 12);
            bits = Read16(body + 14);
            if (format == FormatExtensible && avail >= 40)
                format = Read16(body + 24); // sub-format GUID starts with the format tag
            if (format == FormatMsAdpcm && avail >= 22) {
                numCoefs = Min<u32>(Read16(body + 20), 256);
                for (u32 i = 0; i < numCoefs && 22 + i * 4 + 4 <= avail; ++i) {
                    msCoefs[i][0] = ReadI16(body + 22 + i * 4);
                    msCoefs[i][1] = ReadI16(body + 24 + i * 4);
                }
            }
        } else if (MemCompare(chunk, "data", 4) == 0) {
            pcmData = body;
            pcmSize = avail;
        }
        pos += 8 + size + (size & 1);
    }
    if (!pcmData || channels == 0 || sampleRate == 0)
        return Error::Make(ErrorCode::InvalidData, "WAV: missing fmt or data chunk");
    if (channels > 8)
        return Error::Make(ErrorCode::Unsupported, "WAV: {} channels", channels);

    PcmData pcm;
    pcm.sampleRate = sampleRate;
    pcm.channels = channels;
    switch (format) {
    case FormatPcm: {
        const u32 bytes = bits / 8;
        if (bytes < 1 || bytes > 4)
            return Error::Make(ErrorCode::Unsupported, "WAV: {}-bit PCM", bits);
        const usize count = pcmSize / bytes;
        pcm.samples.Resize(count);
        for (usize i = 0; i < count; ++i) {
            const u8* s = pcmData + i * bytes;
            f32 v;
            switch (bytes) {
            case 1: v = (f32(s[0]) - 128.0f) / 128.0f; break;
            case 2: v = f32(ReadI16(s)) / 32768.0f; break;
            case 3: v = f32(i32((u32(s[0]) << 8) | (u32(s[1]) << 16) | (u32(s[2]) << 24)) >> 8) / 8388608.0f; break;
            default: v = f32(f64(i32(Read32(s))) / 2147483648.0); break;
            }
            pcm.samples[i] = v;
        }
        break;
    }
    case FormatFloat: {
        if (bits == 32) {
            pcm.samples.Resize(pcmSize / 4);
            MemCopy(pcm.samples.Data(), pcmData, pcm.samples.Size() * 4);
        } else if (bits == 64) {
            pcm.samples.Resize(pcmSize / 8);
            for (usize i = 0; i < pcm.samples.Size(); ++i) {
                f64 d;
                MemCopy(&d, pcmData + i * 8, 8);
                pcm.samples[i] = f32(d);
            }
        } else {
            return Error::Make(ErrorCode::Unsupported, "WAV: {}-bit float", bits);
        }
        break;
    }
    case FormatImaAdpcm:
        if (channels > 2 || !DecodeImaAdpcm(pcmData, pcmSize, channels, blockAlign, pcm.samples))
            return Error::Make(ErrorCode::InvalidData, "WAV: bad IMA ADPCM data");
        break;
    case FormatMsAdpcm:
        if (channels > 2 || numCoefs == 0 || !DecodeMsAdpcm(pcmData, pcmSize, channels, blockAlign, msCoefs, numCoefs, pcm.samples))
            return Error::Make(ErrorCode::InvalidData, "WAV: bad Microsoft ADPCM data");
        break;
    default: return Error::Make(ErrorCode::Unsupported, "WAV: format tag 0x{:X} is not supported", format);
    }
    pcm.samples.Resize(pcm.samples.Size() - pcm.samples.Size() % pcm.channels);
    DownmixToStereo(pcm);
    return pcm;
}

Array<u8> EncodeWav(const PcmData& pcm, bool float32)
{
    const u32 bytesPerSample = float32 ? 4 : 2;
    const u32 dataSize = u32(pcm.samples.Size()) * bytesPerSample;
    Array<u8> out;
    out.Resize(44 + dataSize);
    u8* p = out.Data();
    auto w32 = [&](u32 v) { MemCopy(p, &v, 4), p += 4; };
    auto w16 = [&](u16 v) { MemCopy(p, &v, 2), p += 2; };
    MemCopy(p, "RIFF", 4), p += 4;
    w32(36 + dataSize);
    MemCopy(p, "WAVEfmt ", 8), p += 8;
    w32(16);
    w16(float32 ? FormatFloat : FormatPcm);
    w16(u16(pcm.channels));
    w32(pcm.sampleRate);
    w32(pcm.sampleRate * pcm.channels * bytesPerSample);
    w16(u16(pcm.channels * bytesPerSample));
    w16(u16(bytesPerSample * 8));
    MemCopy(p, "data", 4), p += 4;
    w32(dataSize);
    for (f32 s : pcm.samples) {
        if (float32) {
            MemCopy(p, &s, 4), p += 4;
        } else {
            const i16 v = i16(Clamp(s * 32767.0f + (s >= 0 ? 0.5f : -0.5f), -32768.0f, 32767.0f));
            MemCopy(p, &v, 2), p += 2;
        }
    }
    return out;
}

// --- Clips ------------------------------------------------------------------------------------------------------------

UniquePtr<StreamDecoder> AudioClip::CreateDecoder() const
{
    if (format != ClipFormat::Vorbis)
        return {};
    Result<UniquePtr<StreamDecoder>> d = CreateVorbisDecoder(encoded.View());
    return d ? move(d.Value()) : UniquePtr<StreamDecoder>();
}

Result<RefPtr<AudioClip>> LoadAudioClip(Span<const u8> data, StringView name, const ClipLoadOptions& options)
{
    RefPtr<AudioClip> clip(New<AudioClip>());
    clip->name = name;
    if (data.Size() >= 4 && MemCompare(data.Data(), "OggS", 4) == 0) {
        Result<UniquePtr<StreamDecoder>> dec = CreateVorbisDecoder(data);
        if (!dec)
            return Error::Make(dec.GetError().code, "{}: {}", name, dec.GetError().message);
        clip->sampleRate = (*dec)->SampleRate();
        clip->channels = (*dec)->Channels();
        // Count frames by decoding once (fast); short clips keep the PCM.
        Array<f32> pcm;
        f32 buffer[4096 * 2];
        for (;;) {
            const u32 n = (*dec)->Read(buffer, 4096);
            if (n == 0)
                break;
            pcm.Append(Span<const f32>(buffer, usize(n) * clip->channels));
        }
        clip->frameCount = pcm.Size() / clip->channels;
        if (clip->Duration() > options.streamLongerThan) {
            clip->format = ClipFormat::Vorbis;
            clip->encoded.Append(data);
        } else {
            clip->format = ClipFormat::Pcm;
            clip->samples = move(pcm);
        }
        return clip;
    }
    Result<PcmData> wav = DecodeWav(data);
    if (!wav)
        return Error::Make(wav.GetError().code, "{}: {}", name, wav.GetError().message);
    clip->format = ClipFormat::Pcm;
    clip->sampleRate = wav->sampleRate;
    clip->channels = wav->channels;
    clip->samples = move(wav->samples);
    clip->frameCount = clip->samples.Size() / clip->channels;
    return clip;
}

Result<RefPtr<AudioClip>> LoadAudioClipFile(StringView path, const ClipLoadOptions& options)
{
    Array<u8> bytes;
    if (!fs::ReadFile(path, bytes))
        return Error::Make(ErrorCode::NotFound, "Audio file not found: {}", path);
    return LoadAudioClip(bytes.View(), path, options);
}

} // namespace zx::audio
