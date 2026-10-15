"""Builds Engine/Content/StarterContent/Sounds from downloaded CC0 recordings (see CREDITS.txt written next to them).

    python Tools/import_starter_sounds.py <download dir>

The download dir holds the unpacked Kenney packs (x_kenney_impact-sounds, x_kenney_rpg-audio, x_kenney_sci-fi-sounds,
x_kenney_interface-sounds) and the OpenGameArt files (fire.wav, fire-1.ogg, x_rain/1.ogg, wind.ogg, VistulaShort.mp3,
birds_wind_ambient.ogg, x_sfx_loops/ from rubberduck's "30 CC0 SFX loops"). Ambient recordings are cut
to seamless loops (equal-power crossfade of the end into the start) and written as Ogg Vorbis; one-shots are copied.
Needs the soundfile package (libsndfile >= 1.1 for MP3).
"""

import os
import shutil
import sys

import numpy as np
import soundfile as sf

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'Engine', 'Content', 'StarterContent', 'Sounds')


def find(src, name):
    for base, _, files in os.walk(src):
        if name in files:
            return os.path.join(base, name)
    raise FileNotFoundError(name)


def loop(src_path, dst, start, length, fade, mono=False, rate=None, gain=1.0):
    """Cuts [start, start + length + fade) seconds and folds the tail over the head: a seamless loop of `length`."""
    data, sr = sf.read(src_path, dtype='float32', always_2d=True)
    if mono:
        data = data.mean(axis=1, keepdims=True)
    if rate and rate != sr:
        # Linear resampling is enough for noisy ambiences.
        n = int(len(data) * rate / sr)
        x = np.linspace(0, len(data) - 1, n)
        data = np.stack([np.interp(x, np.arange(len(data)), data[:, c]) for c in range(data.shape[1])], axis=1).astype(np.float32)
        sr = rate
    a, n, f = int(start * sr), int(length * sr), int(fade * sr)
    seg = data[a:a + n + f].copy()
    assert len(seg) == n + f, (src_path, len(data) / sr)
    t = np.linspace(0.0, 1.0, f, dtype=np.float32)[:, None]
    head = seg[:f] * np.sin(t * np.pi / 2) + seg[n:n + f] * np.cos(t * np.pi / 2)
    out = np.concatenate([head, seg[f:n]])
    peak = np.abs(out).max()
    out = out * (0.7 * gain / peak if peak > 0 else 1.0)
    # Written in blocks: libsndfile's Vorbis encoder can crash on one large write.
    out = np.ascontiguousarray(np.clip(out, -1.0, 1.0), dtype=np.float32)
    with sf.SoundFile(os.path.join(OUT, dst), 'w', sr, out.shape[1], format='OGG', subtype='VORBIS') as f:
        for i in range(0, len(out), 4096):
            f.write(out[i:i + 4096])


def highpass(src_path, dst, cutoff, gain=1.0):
    """A loop file filtered towards hiss (steam from noise), normalized."""
    data, sr = sf.read(src_path, dtype='float32', always_2d=True)
    rc = 1.0 / (2 * np.pi * cutoff)
    a = rc / (rc + 1.0 / sr)
    out = np.zeros_like(data)
    for c in range(data.shape[1]):
        x = data[:, c]
        y = np.zeros_like(x)
        for i in range(1, len(x)):
            y[i] = a * (y[i - 1] + x[i] - x[i - 1])
        out[:, c] = y
    out = out * (0.7 * gain / max(np.abs(out).max(), 1e-6))
    sf.write(os.path.join(OUT, dst), np.clip(out, -1, 1), sr, format='OGG', subtype='VORBIS')


def normalized(src_path, dst, gain=1.0):
    """A loop that is already seamless, written as Vorbis at a common level."""
    data, sr = sf.read(src_path, dtype='float32', always_2d=True)
    data = data * (0.7 * gain / max(np.abs(data).max(), 1e-6))
    os.makedirs(os.path.dirname(os.path.join(OUT, dst)), exist_ok=True)
    sf.write(os.path.join(OUT, dst), np.clip(data, -1, 1), sr, format='OGG', subtype='VORBIS')


def copy(src, name, dst):
    os.makedirs(os.path.dirname(os.path.join(OUT, dst)), exist_ok=True)
    shutil.copyfile(find(src, name), os.path.join(OUT, dst))


def main():
    src = sys.argv[1]
    if os.path.isdir(OUT):
        shutil.rmtree(OUT)
    os.makedirs(OUT)
    # Ambient loops.
    loop(os.path.join(src, 'fire.wav'), 'Fire_Loop.ogg', 2.0, 16.0, 1.5, mono=True)
    loop(os.path.join(src, 'x_rain', '1.ogg'), 'Rain_Loop.ogg', 1.0, 20.0, 2.0)
    loop(os.path.join(src, 'wind.ogg'), 'Wind_Loop.ogg', 0.0, 5.0, 0.9)
    loop(os.path.join(src, 'VistulaShort.mp3'), 'Waves_Loop.ogg', 30.0, 24.0, 3.0, rate=32000)
    shutil.copyfile(os.path.join(src, 'fire-1.ogg'), os.path.join(OUT, 'Fire_Crackle.ogg'))
    loop(os.path.join(src, 'birds_wind_ambient.ogg'), 'Birds_Loop.ogg', 4.0, 40.0, 3.0)
    loops = os.path.join(src, 'x_sfx_loops')
    normalized(os.path.join(loops, 'water_flowing.ogg'), 'Water_Stream_Loop.ogg', 0.8)
    normalized(os.path.join(loops, 'water_boiling.ogg'), 'Water_Boiling_Loop.ogg', 0.8)
    normalized(os.path.join(loops, 'rolling.ogg'), 'Props/Rolling_Loop.ogg')
    normalized(os.path.join(loops, 'machine_08.ogg'), 'Props/Machine_Hum_Loop.ogg', 0.8)
    highpass(os.path.join(loops, 'noise_01.ogg'), 'Steam_Loop.ogg', 1800.0, 0.8)
    # Footsteps and impacts (Kenney Impact Sounds).
    for surface in ['concrete', 'grass', 'wood', 'carpet', 'snow']:
        for i in range(5):
            copy(src, f'footstep_{surface}_00{i}.ogg', f'Footsteps/Footstep_{surface.capitalize()}_{i}.ogg')
    impacts = {'Wood_Light': 'impactWood_light', 'Wood_Medium': 'impactWood_medium', 'Wood_Heavy': 'impactWood_heavy',
               'Plank': 'impactPlank_medium', 'Metal_Light': 'impactMetal_light', 'Metal_Medium': 'impactMetal_medium',
               'Metal_Heavy': 'impactMetal_heavy', 'Soft': 'impactSoft_medium', 'Soft_Heavy': 'impactSoft_heavy',
               'Stone': 'impactMining', 'Glass': 'impactGlass_medium', 'Generic': 'impactGeneric_light', 'Punch': 'impactPunch_medium'}
    for name, prefix in impacts.items():
        for i in range(5):
            copy(src, f'{prefix}_00{i}.ogg', f'Impacts/Impact_{name}_{i}.ogg')
    # Props (Kenney RPG Audio, Sci-Fi Sounds).
    props = {'Door_Open_0': 'doorOpen_1.ogg', 'Door_Open_1': 'doorOpen_2.ogg', 'Door_Close_0': 'doorClose_1.ogg',
             'Door_Close_1': 'doorClose_2.ogg', 'Door_Close_2': 'doorClose_3.ogg', 'Coins_0': 'handleCoins.ogg',
             'Coins_1': 'handleCoins2.ogg', 'Creak_0': 'creak1.ogg', 'Creak_1': 'creak2.ogg', 'Creak_2': 'creak3.ogg',
             'Latch': 'metalLatch.ogg', 'Click': 'metalClick.ogg', 'Chop': 'chop.ogg', 'Cloth': 'cloth1.ogg',
             'Explosion_0': 'explosionCrunch_000.ogg', 'Explosion_1': 'explosionCrunch_001.ogg', 'Explosion_2': 'explosionCrunch_002.ogg',
             'Explosion_Low': 'lowFrequency_explosion_000.ogg', 'Force_Field': 'forceField_000.ogg', 'Thruster': 'thrusterFire_000.ogg',
             'Laser': 'laserLarge_000.ogg', 'Jump': 'cloth2.ogg', 'Pickup': 'beltHandle1.ogg', 'Computer': 'computerNoise_000.ogg'}
    for i in range(5):
        props[f'Laser_Small_{i}'] = f'laserSmall_00{i}.ogg'
    for name, file in props.items():
        copy(src, file, f'Props/{name}.ogg')
    # Interface (Kenney Interface Sounds): editor and game UI.
    ui = {'Click': 'click_00{}.ogg', 'Confirm': 'confirmation_00{}.ogg', 'Error': 'error_00{}.ogg', 'Toggle': 'toggle_00{}.ogg',
          'Open': 'open_00{}.ogg', 'Close': 'close_00{}.ogg', 'Select': 'select_00{}.ogg', 'Back': 'back_00{}.ogg', 'Drop': 'drop_00{}.ogg'}
    for name, pattern in ui.items():
        for i in range(4):
            copy(src, pattern.format(i + 1), f'UI/{name}_{i}.ogg')
    # Engines fade in and out in the pack: the steady middle becomes a loop.
    loop(find(src, 'spaceEngineLow_000.ogg'), 'Props/Engine_Loop.ogg', 0.8, 3.0, 0.8)
    loop(find(src, 'engineCircular_000.ogg'), 'Props/Motor_Loop.ogg', 0.8, 3.0, 0.8)
    with open(os.path.join(OUT, 'CREDITS.txt'), 'w', encoding='utf-8', newline='\n') as f:
        f.write("""Starter Content sounds - all CC0 (public domain), free for any use; credit is appreciated, not required.

Footsteps/, Impacts/, Props/, UI/  Kenney (www.kenney.nl): Impact Sounds, RPG Audio, Sci-Fi Sounds, Interface Sounds
Fire_Loop.ogg                 "fireplace sound loop" by PagDev (opengameart.org/content/fireplace-sound-loop)
Fire_Crackle.ogg              "Fire Crackling" by AntumDeluge (opengameart.org/content/fire-crackling)
Rain_Loop.ogg                 "Rain (loopable)" by Ylmir (opengameart.org/content/rain-loopable)
Wind_Loop.ogg                 "wind whoosh loop" by SketchMan3 (opengameart.org/content/wind-whoosh-loop)
Waves_Loop.ogg                "Sea and river wave sounds" by RandomMind (opengameart.org/content/sea-and-river-wave-sounds)
Birds_Loop.ogg                "Birds and Wind - Ambient" by Spring Spring (opengameart.org/content/birds-and-wind-ambient-birds-wind-and-synth)
Water_Stream_Loop.ogg, Water_Boiling_Loop.ogg, Steam_Loop.ogg (filtered noise), Props/Rolling_Loop.ogg, Props/Machine_Hum_Loop.ogg
                              "30 CC0 SFX loops" by rubberduck (opengameart.org/content/30-cc0-sfx-loops)

Loops were cut and crossfaded by Tools/import_starter_sounds.py.
""")
    total = sum(os.path.getsize(os.path.join(b, n)) for b, _, fs in os.walk(OUT) for n in fs)
    count = sum(len(fs) for _, _, fs in os.walk(OUT))
    print(f'{count} files, {total // 1024} KB -> {OUT}')


if __name__ == '__main__':
    main()
