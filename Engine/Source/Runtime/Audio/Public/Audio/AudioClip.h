#pragma once

// Audio clips: decoded PCM (short sounds) or encoded data decoded while playing (music). Own decoders only:
// WAV (PCM 8/16/24/32-bit, IEEE float, IMA ADPCM, Microsoft ADPCM) and Ogg Vorbis.
//
//   Result<RefPtr<AudioClip>> clip = LoadAudioClip(bytes, "Sounds/Step.wav");   // or LoadAudioClipFile(path)
//   engine.Play(*clip, params);

#include "Core/Containers/Array.h"
#include "Core/Containers/Span.h"
#include "Core/Memory/Pointers.h"
#include "Core/Result.h"
#include "Core/String/String.h"

namespace zx::audio {

enum class ClipFormat : u8
{
    Pcm,    // decoded float samples in `samples`
    Vorbis, // Ogg Vorbis bytes in `encoded`, decoded while playing
};

/// Reads frames from an encoded clip; one per playing voice. Runs on the mixer thread.
class StreamDecoder
{
public:
    virtual ~StreamDecoder() = default;
    /// Decodes up to `frames` interleaved frames into `out` (channels per frame = Channels()). Returns frames written;
    /// 0 = end of stream.
    virtual u32 Read(f32* out, u32 frames) = 0;
    /// Restarts at the first frame (looping).
    virtual bool Rewind() = 0;
    virtual u32 Channels() const = 0;
    virtual u32 SampleRate() const = 0;
};

class AudioClip : public RefCounted
{
public:
    String name;
    ClipFormat format = ClipFormat::Pcm;
    u32 sampleRate = 48000;
    u32 channels = 1;            // 1 or 2 (more are mixed down to stereo on load)
    Array<f32> samples;          // Pcm: interleaved frames
    Array<u8> encoded;           // Vorbis: the whole file
    u64 frameCount = 0;          // total frames (known for Pcm and Vorbis)

    f64 Duration() const { return sampleRate ? f64(frameCount) / f64(sampleRate) : 0.0; }
    bool IsStreamed() const { return format != ClipFormat::Pcm; }
    /// New decoder for a streamed clip (null for Pcm).
    UniquePtr<StreamDecoder> CreateDecoder() const;
    u64 MemorySize() const { return samples.SizeBytes() + encoded.Size(); }
};

struct ClipLoadOptions
{
    /// Vorbis files longer than this stay encoded and are decoded while playing; shorter ones are decoded on load.
    f64 streamLongerThan = 8.0; // seconds
};

/// Detects the format from the data (RIFF/WAVE or OggS).
Result<RefPtr<AudioClip>> LoadAudioClip(Span<const u8> data, StringView name, const ClipLoadOptions& options = {});
Result<RefPtr<AudioClip>> LoadAudioClipFile(StringView path, const ClipLoadOptions& options = {});

// --- Individual decoders (also used by tests) ----------------------------------------------------------------------------

struct PcmData
{
    u32 sampleRate = 0;
    u32 channels = 0;
    Array<f32> samples; // interleaved, [-1, 1]
};

Result<PcmData> DecodeWav(Span<const u8> data);
/// Whole Ogg Vorbis file to PCM.
Result<PcmData> DecodeVorbis(Span<const u8> data);
/// Streaming Vorbis decoder over `data` (must outlive the decoder).
Result<UniquePtr<StreamDecoder>> CreateVorbisDecoder(Span<const u8> data);

/// Writes a 16-bit PCM or 32-bit float WAV (tools, tests).
Array<u8> EncodeWav(const PcmData& pcm, bool float32 = false);

} // namespace zx::audio
