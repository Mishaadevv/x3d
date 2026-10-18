#include "Render/OceanWaves.h"
#include <cmath>
#include "Render/Water.h"

namespace zx::render {

namespace {

constexpr f32 Gravity = 9.81f;

/// JONSWAP frequency spectrum S(omega) for wind `u` (m/s) over `fetch` (m).
f32 Jonswap(f32 omega, f32 u, f32 fetch, f32& peak)
{
    const f32 g = Gravity;
    peak = 22.0f * std::pow(g * g / (u * fetch), 1.0f / 3.0f);
    const f32 alpha = 0.076f * std::pow(u * u / (fetch * g), 0.22f);
    const f32 sigma = omega <= peak ? 0.07f : 0.09f;
    const f32 r = std::exp(-(omega - peak) * (omega - peak) / (2.0f * sigma * sigma * peak * peak));
    return alpha * g * g / std::pow(omega, 5.0f) * std::exp(-1.25f * std::pow(peak / omega, 4.0f)) * std::pow(3.3f, r);
}

/// Peak wavelength of the sea state (m).
f32 PeakWavelength(f32 u, f32 fetch)
{
    f32 peak = 1.0f;
    Jonswap(1.0f, u, fetch, peak);
    return 2.0f * Pi * Gravity / (peak * peak);
}

/// Directional spreading around the wind (normalized over -pi..pi): mostly downwind, a little across.
f32 Spreading(f32 theta)
{
    const f32 c = std::cos(theta);
    return c > 0.0f ? (8.0f / (3.0f * Pi)) * c * c * c * c * 0.92f + 0.08f / (2.0f * Pi) : 0.08f / (2.0f * Pi);
}

/// Standard normal sample from a hash of (a, b, c) (Box-Muller).
Vec2 Gaussian(u32 a, u32 b, u32 c)
{
    auto hash = [](u32 x) {
        x ^= x >> 16;
        x *= 0x7FEB352Du;
        x ^= x >> 15;
        x *= 0x846CA68Bu;
        x ^= x >> 16;
        return x;
    };
    const u32 h1 = hash(a * 73856093u ^ hash(b * 19349663u ^ hash(c * 83492791u + 1u)));
    const u32 h2 = hash(h1 + 0x9E3779B9u);
    const f32 u1 = Max((f32(h1 >> 8) + 0.5f) / 16777216.0f, 1e-7f);
    const f32 u2 = (f32(h2 >> 8) + 0.5f) / 16777216.0f;
    const f32 r = std::sqrt(-2.0f * std::log(u1));
    return Vec2(r * std::cos(2.0f * Pi * u2), r * std::sin(2.0f * Pi * u2));
}

Vec2 CMul(Vec2 a, Vec2 b) { return Vec2(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x); }

/// In-place inverse FFT (e^{+i}) of `count` complex values spaced `stride` apart; count is a power of two.
void InverseFft(Vec2* data, u32 count, u32 stride)
{
    for (u32 i = 1, j = 0; i < count; ++i) { // bit reversal
        u32 bit = count >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j) {
            const Vec2 t = data[usize(i) * stride];
            data[usize(i) * stride] = data[usize(j) * stride];
            data[usize(j) * stride] = t;
        }
    }
    for (u32 len = 2; len <= count; len <<= 1) {
        const f32 angle = 2.0f * Pi / f32(len);
        const Vec2 wl(std::cos(angle), std::sin(angle));
        for (u32 i = 0; i < count; i += len) {
            Vec2 w(1.0f, 0.0f);
            for (u32 k = 0; k < len / 2; ++k) {
                Vec2& a = data[usize(i + k) * stride];
                Vec2& b = data[usize(i + k + len / 2) * stride];
                const Vec2 t = CMul(w, b);
                b = a - t;
                a = a + t;
                w = CMul(w, wl);
            }
        }
    }
}

void InverseFft2D(Array<Vec2>& data, u32 n)
{
    for (u32 y = 0; y < n; ++y)
        InverseFft(data.Data() + usize(y) * n, n, 1);
    for (u32 x = 0; x < n; ++x)
        InverseFft(data.Data() + x, n, n);
}

} // namespace

void BuildOceanSpectrum(const WaterSurface& s, u32 n, OceanSpectrum& out)
{
    out.n = n;
    out.choppiness = Clamp(s.choppiness, 0.0f, 2.0f);
    const f32 u = Clamp(s.windSpeed, 0.5f, 40.0f);
    const f32 fetch = Clamp(s.fetch, 0.005f, 2000.0f) * 1000.0f;
    // Cascade sizes from the peak wavelength: the swell, waves ~6x shorter, ripples ~6x shorter still.
    out.peakWavelength = PeakWavelength(u, fetch);
    out.patchSize[0] = Clamp(out.peakWavelength * 3.5f, 4.0f, 2500.0f);
    out.patchSize[1] = out.patchSize[0] / 6.3f;
    out.patchSize[2] = out.patchSize[1] / 6.1f;
    const u64 key = WaterSpectrumKey(s);
    const Vec2 wind(std::sin(s.windDirection), std::cos(s.windDirection));
    const i32 half = i32(n) / 2;
    for (u32 c = 0; c < OceanCascades; ++c) {
        const f32 dk = 2.0f * Pi / out.patchSize[c];
        // Each cascade keeps its own band of wavelengths (no wave counted twice).
        const f32 kMin = c == 0 ? 0.0f : 2.0f * Pi / out.patchSize[c] * 6.0f;
        const f32 kMax = c + 1 < OceanCascades ? 2.0f * Pi / out.patchSize[c + 1] * 6.0f : 1e9f;
        // (ix, iy): signed wave-number index; the seed doesn't depend on the grid size.
        auto h0 = [&](i32 ix, i32 iy) {
            const Vec2 k = Vec2(f32(ix), f32(iy)) * dk;
            const f32 kLen = Length(k);
            if (kLen < 1e-5f || kLen < kMin || kLen >= kMax)
                return Vec2(0.0f);
            const f32 omega = std::sqrt(Gravity * kLen);
            f32 p;
            const f32 sOmega = Jonswap(omega, u, fetch, p);
            const f32 theta = std::atan2(k.y * wind.x - k.x * wind.y, Dot(k, wind));
            const f32 dOmegaDk = Gravity / (2.0f * omega);
            // Damp the tiniest waves (they only alias) and convert S(omega, theta) -> S(k).
            const f32 tiny = std::exp(-kLen * kLen * 0.0004f);
            const f32 sK = sOmega * Spreading(theta) * dOmegaDk / kLen * tiny;
            const Vec2 xi = Gaussian(u32(ix) & 0xFFFF, u32(iy) & 0xFFFF, c + u32(key & 0xFF) * 7u);
            return xi * std::sqrt(Max(sK, 0.0f) * dk * dk * 0.5f);
        };
        Array<Vec4>& data = out.h0[c];
        data.Resize(usize(n) * n);
        for (i32 y = 0; y < i32(n); ++y)
            for (i32 x = 0; x < i32(n); ++x) {
                const Vec2 a = h0(x - half, y - half);
                const Vec2 b = h0(half - x, half - y); // h0(-k)
                data[usize(y) * n + usize(x)] = Vec4(a.x, a.y, b.x, -b.y); // conj
            }
    }
}

void OceanWaves::Build(const WaterSurface& s)
{
    BuildOceanSpectrum(s, Size, m_spectrum);
    // The Nyquist row / column has no partner on this small grid: drop it (it would only alias).
    for (u32 c = 0; c < Cascades; ++c)
        for (u32 i = 0; i < Size; ++i) {
            m_spectrum.h0[c][i] = Vec4(0.0f);
            m_spectrum.h0[c][usize(i) * Size] = Vec4(0.0f);
        }
    m_key = WaterSpectrumKey(s);
    m_time = -1.0f;
    m_built = true;
    for (u32 c = 0; c < Cascades; ++c) {
        m_displacement[c].Resize(usize(Size) * Size);
        m_velocity[c].Resize(usize(Size) * Size);
    }
}

void OceanWaves::Update(f32 time)
{
    if (!m_built || time == m_time)
        return;
    m_time = time;
    const u32 n = Size;
    const f32 lambda = m_spectrum.choppiness;
    Array<Vec2> a, b, v; // Dx + i Dy, Dz + i Vy, Vx + i Vz
    a.Resize(usize(n) * n);
    b.Resize(usize(n) * n);
    v.Resize(usize(n) * n);
    for (u32 c = 0; c < Cascades; ++c) {
        const f32 dk = 2.0f * Pi / m_spectrum.patchSize[c];
        for (u32 y = 0; y < n; ++y)
            for (u32 x = 0; x < n; ++x) {
                const usize i = usize(y) * n + x;
                const Vec4 h0 = m_spectrum.h0[c][i];
                const Vec2 k = Vec2(f32(i32(x) - i32(n / 2)), f32(i32(y) - i32(n / 2))) * dk;
                const f32 kLen = Max(Length(k), 1e-4f);
                const f32 omega = std::sqrt(Gravity * kLen);
                const Vec2 e(std::cos(omega * time), std::sin(omega * time));
                const Vec2 p = CMul(Vec2(h0.x, h0.y), e), m = CMul(Vec2(h0.z, h0.w), Vec2(e.x, -e.y));
                const Vec2 h = p + m;
                const Vec2 dh = Vec2(-(p.y - m.y), p.x - m.x) * omega; // i omega (p - m)
                const Vec2 kn = k / kLen;
                auto mi = [](Vec2 z) { return Vec2(z.y, -z.x); }; // -i z
                auto ip = [](Vec2 z) { return Vec2(-z.y, z.x); }; // i z
                const Vec2 dx = mi(h) * (kn.x * lambda), dz = mi(h) * (kn.y * lambda);
                const Vec2 vx = mi(dh) * (kn.x * lambda), vz = mi(dh) * (kn.y * lambda);
                a[i] = dx + ip(h);
                b[i] = dz + ip(dh);
                v[i] = vx + ip(vz);
            }
        InverseFft2D(a, n);
        InverseFft2D(b, n);
        InverseFft2D(v, n);
        for (u32 y = 0; y < n; ++y)
            for (u32 x = 0; x < n; ++x) {
                const usize i = usize(y) * n + x;
                const f32 sign = ((x + y) & 1) ? -1.0f : 1.0f; // the spectrum was centered at n/2
                m_displacement[c][i] = Vec3(a[i].x, a[i].y, b[i].x) * sign;
                m_velocity[c][i] = Vec3(v[i].x, b[i].y, v[i].y) * sign;
            }
    }
}

void OceanWaves::Sample(Vec2 xz, Vec3& displacement, Vec3* velocity) const
{
    displacement = Vec3(0.0f);
    if (velocity)
        *velocity = Vec3(0.0f);
    if (!m_built || m_time < 0.0f)
        return;
    const i32 n = i32(Size);
    for (u32 c = 0; c < Cascades; ++c) {
        // Grid node j sits at x = j * patch / n (the GPU samples its textures the same way).
        const Vec2 g = xz / m_spectrum.patchSize[c] * f32(n);
        const f32 fx = std::floor(g.x), fy = std::floor(g.y);
        const f32 tx = g.x - fx, ty = g.y - fy;
        auto wrap = [n](f32 v) { return i32(((i64(v) % n) + n) % n); };
        const i32 x0 = wrap(fx), y0 = wrap(fy), x1 = (x0 + 1) % n, y1 = (y0 + 1) % n;
        auto lerp2 = [&](const Array<Vec3>& f) {
            const Vec3 a = Lerp(f[usize(y0) * n + x0], f[usize(y0) * n + x1], tx);
            const Vec3 b = Lerp(f[usize(y1) * n + x0], f[usize(y1) * n + x1], tx);
            return Lerp(a, b, ty);
        };
        displacement += lerp2(m_displacement[c]);
        if (velocity)
            *velocity += lerp2(m_velocity[c]);
    }
}

} // namespace zx::render
