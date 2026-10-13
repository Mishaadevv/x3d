#pragma once

#include "Core/Math/Vector.h"

namespace zx::noise {

/// Improved Perlin gradient noise, output approximately in [-1, 1].
f32 Perlin(Vec2 p, u32 seed = 0);
f32 Perlin(Vec3 p, u32 seed = 0);

/// Simplex noise, output approximately in [-1, 1].
f32 Simplex(Vec2 p, u32 seed = 0);
f32 Simplex(Vec3 p, u32 seed = 0);

/// Worley (cellular) noise: distance to the nearest feature point (F1), roughly in [0, 1.5].
f32 Worley(Vec2 p, u32 seed = 0);
f32 Worley(Vec3 p, u32 seed = 0);

/// Fractal Brownian motion over Simplex noise. Output roughly in [-1, 1].
f32 Fbm(Vec2 p, u32 octaves = 5, f32 lacunarity = 2.0f, f32 gain = 0.5f, u32 seed = 0);
f32 Fbm(Vec3 p, u32 octaves = 5, f32 lacunarity = 2.0f, f32 gain = 0.5f, u32 seed = 0);
/// Ridged multifractal (mountain-like), output in [0, 1].
f32 Ridged(Vec2 p, u32 octaves = 5, f32 lacunarity = 2.0f, f32 gain = 0.5f, u32 seed = 0);

} // namespace zx::noise
