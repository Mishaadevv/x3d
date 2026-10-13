#include "Core/Math/Curve.h"

namespace zx {

Curve Curve::Constant(f32 value)
{
    Curve c;
    c.AddKey(0.0f, value, CurveInterp::Constant);
    return c;
}

Curve Curve::Linear(f32 t0, f32 v0, f32 t1, f32 v1)
{
    Curve c;
    c.AddKey(t0, v0, CurveInterp::Linear);
    c.AddKey(t1, v1, CurveInterp::Linear);
    return c;
}

Curve Curve::EaseInOut(f32 t0, f32 v0, f32 t1, f32 v1)
{
    Curve c;
    c.AddKey(Key{t0, v0, 0.0f, 0.0f, CurveInterp::Cubic});
    c.AddKey(Key{t1, v1, 0.0f, 0.0f, CurveInterp::Cubic});
    return c;
}

usize Curve::AddKey(const Key& key)
{
    usize i = 0;
    while (i < m_keys.Size() && m_keys[i].time <= key.time)
        ++i;
    m_keys.Insert(i, key);
    return i;
}

usize Curve::AddKey(f32 time, f32 value, CurveInterp interp)
{
    const usize i = AddKey(Key{time, value, 0.0f, 0.0f, interp});
    AutoTangents();
    return i;
}

void Curve::AutoTangents()
{
    const usize n = m_keys.Size();
    for (usize i = 0; i < n; ++i) {
        f32 slope = 0.0f;
        if (n >= 2) {
            const Key& prev = m_keys[i > 0 ? i - 1 : i];
            const Key& next = m_keys[i + 1 < n ? i + 1 : i];
            const f32 dt = next.time - prev.time;
            slope = dt > 0.0f ? (next.value - prev.value) / dt : 0.0f;
        }
        m_keys[i].inTangent = slope;
        m_keys[i].outTangent = slope;
    }
}

f32 Curve::Evaluate(f32 time) const
{
    const usize n = m_keys.Size();
    if (n == 0)
        return 0.0f;
    if (n == 1)
        return m_keys[0].value;

    const f32 start = m_keys[0].time;
    const f32 end = m_keys[n - 1].time;
    const f32 length = end - start;
    auto wrap = [&](CurveWrap mode, f32 t) {
        if (length <= 0.0f || mode == CurveWrap::Clamp)
            return Clamp(t, start, end);
        f32 local = Fmod(t - start, mode == CurveWrap::PingPong ? 2.0f * length : length);
        if (local < 0.0f)
            local += mode == CurveWrap::PingPong ? 2.0f * length : length;
        if (mode == CurveWrap::PingPong && local > length)
            local = 2.0f * length - local;
        return start + local;
    };
    if (time < start)
        time = wrap(preWrap, time);
    else if (time > end)
        time = wrap(postWrap, time);

    // Binary search for the segment [i, i+1].
    usize lo = 0, hi = n - 1;
    while (hi - lo > 1) {
        const usize mid = (lo + hi) / 2;
        if (m_keys[mid].time <= time)
            lo = mid;
        else
            hi = mid;
    }
    const Key& a = m_keys[lo];
    const Key& b = m_keys[hi];
    const f32 dt = b.time - a.time;
    if (dt <= 0.0f)
        return b.value;
    const f32 t = Saturate((time - a.time) / dt);
    switch (a.interp) {
    case CurveInterp::Constant:
        return t < 1.0f ? a.value : b.value;
    case CurveInterp::Linear:
        return Lerp(a.value, b.value, t);
    case CurveInterp::Cubic:
        return Hermite(a.value, a.outTangent * dt, b.value, b.inTangent * dt, t);
    }
    return a.value;
}

} // namespace zx
