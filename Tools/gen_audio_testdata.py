"""Generates Ogg Vorbis test files and their reference decodes for the engine's Vorbis decoder tests.

Writes Tests/Data/Audio/<name>.ogg (encoded by libvorbis through libsndfile) and <name>.ref.wav (the same file
decoded by libvorbis, 16-bit). The engine decodes the .ogg with its own decoder and must match the reference.
Tool-only dependency: `pip install soundfile numpy`.
"""
import os
import numpy as np
import soundfile as sf

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'Tests', 'Data', 'Audio')
os.makedirs(OUT, exist_ok=True)
rng = np.random.default_rng(1234)


def signal(seconds, rate, channels):
    t = np.arange(int(seconds * rate)) / rate
    chans = []
    for c in range(channels):
        # Tones + a chirp + a little noise: exercises floors, residues and both block sizes (transients).
        s = 0.3 * np.sin(2 * np.pi * (220 + 110 * c) * t)
        s += 0.15 * np.sin(2 * np.pi * (300 + 2500 * t) * t)
        s += 0.05 * rng.standard_normal(len(t))
        s[int(len(t) * 0.5):int(len(t) * 0.5) + 200] += 0.35  # a click -> short blocks (peaks stay below 1.0)
        chans.append(s)
    return np.stack(chans, axis=1).astype(np.float32)


CASES = [
    ('mono_44k_q4', 0.6, 44100, 1, 0.4),
    ('stereo_48k_q8', 0.5, 48000, 2, 0.8),
    ('stereo_22k_q0', 0.7, 22050, 2, 0.0),
]

for name, seconds, rate, channels, quality in CASES:
    path = os.path.join(OUT, name + '.ogg')
    sf.write(path, signal(seconds, rate, channels), rate, format='OGG', subtype='VORBIS', compression_level=1.0 - quality)
    ref, ref_rate = sf.read(path, dtype='float32', always_2d=True)
    sf.write(os.path.join(OUT, name + '.ref.wav'), ref, ref_rate, subtype='PCM_16')
    print(f'{name}: {os.path.getsize(path)} bytes, {ref.shape[0]} frames, {ref.shape[1]} ch')
