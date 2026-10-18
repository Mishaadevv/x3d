#pragma once

// Ocean wave spectrum shared by the GPU simulation (Render/Water.h) and a CPU copy of the long waves for gameplay
// (buoyancy, splashes, "am I under water"):
//
//   BuildOceanSpectrum(surface, n, spectrum);   // h0(k) per cascade, JONSWAP + directional spreading
//   OceanWaves waves; waves.Build(surface);     // CPU: the two longest cascades on a 64x64 grid
//   waves.Update(time);                         // evolve + inverse FFT (lazy: only when the time changed)
//   waves.Sample(xz, displacement, &velocity);  // at an undisplaced world position, before the wave amount
//
// The random phases are seeded by the signed wave-number index, so the spectrum of a 64x64 grid is exactly the middle
// of the 128x128 one: Lite / Full and the CPU copy see the same big waves.

#include "Core/Containers/Array.h"
#include "Core/Math/Math.h"

namespace zx::render {

struct WaterSurface;

inline constexpr u32 OceanCascades = 3;

struct OceanSpectrum
{
    u32 n = 0;
    f32 patchSize[OceanCascades] = {}; // meters: the swell, waves, ripples
    f32 peakWavelength = 0.0f;
    f32 choppiness = 1.0f;
    Array<Vec4> h0[OceanCascades]; // per texel: h0(k).xy, conj(h0(-k)).xy; k = (index - n/2) * 2pi / patchSize
};

/// The initial spectrum of `surface` (wind speed, fetch, direction, choppiness) on an n x n grid per cascade.
void BuildOceanSpectrum(const WaterSurface& surface, u32 n, OceanSpectrum& out);

/// CPU evaluation of the long waves (the ripples don't move boats).
class OceanWaves
{
public:
    static constexpr u32 Size = 64;
    static constexpr u32 Cascades = 2;

    void Build(const WaterSurface& surface);
    bool IsBuilt() const { return m_built; }
    u64 Key() const { return m_key; }
    void Update(f32 time);
    f32 Time() const { return m_time; }
    /// Displacement of the surface point that rests at world `xz` (x, height, z) and its velocity (m/s), before the
    /// surface's wave amount and edge calming.
    void Sample(Vec2 xz, Vec3& displacement, Vec3* velocity = nullptr) const;
    f32 PatchSize(u32 cascade) const { return m_spectrum.patchSize[cascade]; }
    f32 PeakWavelength() const { return m_spectrum.peakWavelength; }

private:
    OceanSpectrum m_spectrum;
    Array<Vec3> m_displacement[Cascades];
    Array<Vec3> m_velocity[Cascades];
    f32 m_time = -1.0f;
    u64 m_key = 0;
    bool m_built = false;
};

} // namespace zx::render
