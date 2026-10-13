#pragma once

#include "Core/Math/Quat.h"

namespace zx {

/// PCG32 random generator (O'Neill). Small, fast, statistically good. Not cryptographic.
class Random
{
public:
    explicit Random(u64 seed = 0x853c49e6748fea9bull, u64 stream = 0xda3e39cb94b95bdbull) { Seed(seed, stream); }

    void Seed(u64 seed, u64 stream = 0xda3e39cb94b95bdbull)
    {
        m_state = 0;
        m_inc = (stream << 1u) | 1u;
        NextU32();
        m_state += seed;
        NextU32();
    }

    u32 NextU32()
    {
        const u64 old = m_state;
        m_state = old * 6364136223846793005ull + m_inc;
        const u32 xorshifted = u32(((old >> 18u) ^ old) >> 27u);
        const u32 rot = u32(old >> 59u);
        return (xorshifted >> rot) | (xorshifted << ((~rot + 1u) & 31));
    }
    u64 NextU64() { return (u64(NextU32()) << 32) | NextU32(); }
    /// Uniform in [0, bound) without modulo bias.
    u32 NextBounded(u32 bound)
    {
        if (bound == 0)
            return 0;
        const u32 threshold = (~bound + 1u) % bound;
        for (;;) {
            const u32 r = NextU32();
            if (r >= threshold)
                return r % bound;
        }
    }
    /// Uniform float in [0, 1).
    f32 NextFloat() { return f32(NextU32() >> 8) * (1.0f / 16777216.0f); }
    /// Uniform in [min, max] (integers inclusive).
    i32 Range(i32 min, i32 max) { return min + i32(NextBounded(u32(max - min + 1))); }
    f32 Range(f32 min, f32 max) { return min + (max - min) * NextFloat(); }
    bool NextBool() { return (NextU32() & 1) != 0; }

    Vec2 InsideUnitCircle()
    {
        const f32 r = Sqrt(NextFloat());
        const f32 a = NextFloat() * TwoPi;
        return {Cos(a) * r, Sin(a) * r};
    }
    Vec3 OnUnitSphere()
    {
        const f32 z = NextFloat() * 2.0f - 1.0f;
        const f32 a = NextFloat() * TwoPi;
        const f32 r = Sqrt(1.0f - z * z);
        return {r * Cos(a), r * Sin(a), z};
    }
    Vec3 InsideUnitSphere() { return OnUnitSphere() * Pow(NextFloat(), 1.0f / 3.0f); }
    /// Uniformly distributed random rotation (Shoemake).
    Quat NextRotation()
    {
        const f32 u1 = NextFloat(), u2 = NextFloat() * TwoPi, u3 = NextFloat() * TwoPi;
        const f32 a = Sqrt(1.0f - u1), b = Sqrt(u1);
        return {a * Sin(u2), a * Cos(u2), b * Sin(u3), b * Cos(u3)};
    }

private:
    u64 m_state;
    u64 m_inc;
};

/// Thread-local generator seeded from the clock/thread id.
Random& ThreadRandom();

/// Stateless integer hash -> float in [0,1) (for procedural placement, noise).
constexpr f32 HashToFloat(u32 x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return f32(x >> 8) * (1.0f / 16777216.0f);
}

} // namespace zx
