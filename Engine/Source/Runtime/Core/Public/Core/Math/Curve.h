#pragma once

#include "Core/Containers/Array.h"
#include "Core/Math/Vector.h"

namespace zx {

template <typename T>
constexpr T CatmullRom(const T& p0, const T& p1, const T& p2, const T& p3, f32 t)
{
    const f32 t2 = t * t, t3 = t2 * t;
    return (p1 * 2.0f + (p2 - p0) * t + (p0 * 2.0f - p1 * 5.0f + p2 * 4.0f - p3) * t2 +
            (p1 * 3.0f - p0 - p2 * 3.0f + p3) * t3) *
           0.5f;
}

template <typename T>
constexpr T CubicBezier(const T& p0, const T& p1, const T& p2, const T& p3, f32 t)
{
    const f32 u = 1.0f - t;
    return p0 * (u * u * u) + p1 * (3.0f * u * u * t) + p2 * (3.0f * u * t * t) + p3 * (t * t * t);
}

/// Cubic Hermite with tangents m0, m1 (already scaled by the segment length).
template <typename T>
constexpr T Hermite(const T& p0, const T& m0, const T& p1, const T& m1, f32 t)
{
    const f32 t2 = t * t, t3 = t2 * t;
    return p0 * (2.0f * t3 - 3.0f * t2 + 1.0f) + m0 * (t3 - 2.0f * t2 + t) + p1 * (-2.0f * t3 + 3.0f * t2) +
           m1 * (t3 - t2);
}

enum class CurveInterp : u8
{
    Constant,
    Linear,
    Cubic
};

enum class CurveWrap : u8
{
    Clamp,
    Loop,
    PingPong
};

/// Scalar keyframe curve (animation curves, particle "over lifetime" curves, falloffs).
class Curve
{
public:
    struct Key
    {
        f32 time = 0.0f;
        f32 value = 0.0f;
        f32 inTangent = 0.0f;  // slope (value per second) arriving at the key
        f32 outTangent = 0.0f; // slope leaving the key
        CurveInterp interp = CurveInterp::Cubic; // interpolation to the next key
    };

    Curve() = default;
    static Curve Constant(f32 value);
    static Curve Linear(f32 t0, f32 v0, f32 t1, f32 v1);
    static Curve EaseInOut(f32 t0, f32 v0, f32 t1, f32 v1);

    /// Inserts a key keeping keys sorted by time. Returns its index.
    usize AddKey(const Key& key);
    usize AddKey(f32 time, f32 value, CurveInterp interp = CurveInterp::Cubic);
    void RemoveKey(usize index) { m_keys.RemoveAt(index); }
    void Clear() { m_keys.Clear(); }
    /// Sets tangents automatically (Catmull-Rom style) for all cubic keys.
    void AutoTangents();

    f32 Evaluate(f32 time) const;

    Span<const Key> Keys() const { return m_keys.View(); }
    Span<Key> Keys() { return m_keys.View(); }
    usize KeyCount() const { return m_keys.Size(); }
    f32 StartTime() const { return m_keys.Empty() ? 0.0f : m_keys[0].time; }
    f32 EndTime() const { return m_keys.Empty() ? 0.0f : m_keys.Back().time; }

    CurveWrap preWrap = CurveWrap::Clamp;
    CurveWrap postWrap = CurveWrap::Clamp;

private:
    Array<Key> m_keys;
};

} // namespace zx
