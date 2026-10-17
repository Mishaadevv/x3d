// FFT ocean (Tessendorf 2001, packing as in "Ocean simulation and rendering in War Thunder" / Jump Trajectory): per
// cascade a wave spectrum h0(k) (CPU, JONSWAP + spreading) evolves in time, eight real fields are packed into four
// complex signals, inverse-FFT'd (rows, then columns) and assembled into displacement + slope textures with
// accumulated whitecap foam.
//
//   EvolveCS    h0 -> 4 complex fields (spectrum A, B: RGBA32F, two complex each)
//   FftCS       one row (or column) per group: bit reversal + log2(N) radix-2 stages in groupshared (in place)
//   AssembleCS  sign fix, displacement (xyz + foam), derivatives, foam from the Jacobian
//   MipCS       2x2 box down one level (displacement and derivatives)

#include "Common/Bindless.hlsli"

#ifndef OCEAN_N
#define OCEAN_N 128
#endif
#define LOG2_N (OCEAN_N == 128 ? 7 : (OCEAN_N == 64 ? 6 : 8))
static const float PI = 3.14159265359;

struct OceanParams
{
    uint h0;          // SRV buffer: float4 per texel (h0(k).xy, conj(h0(-k)).xy)
    uint specA;       // UAV RGBA32F
    uint specB;       // UAV RGBA32F
    uint displacement; // UAV RGBA16F (mip 0, or the destination mip for MipCS)
    uint derivatives; // UAV RGBA16F
    uint srcDisplacement; // MipCS: source mip UAVs
    uint srcDerivatives;
    uint columns;     // FftCS: 0 = rows, 1 = columns
    float patchSize;  // meters
    float time;
    float choppiness;
    float dt;
    float foamDecay;  // 1/s
    float foamThreshold;
    float foamBias;
    uint size;        // MipCS: destination size
};
ZX_PUSH_CONSTANTS(OceanParams, g_O);

float2 CMul(float2 a, float2 b) { return float2(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x); }

[numthreads(8, 8, 1)]
void EvolveCS(uint2 id : SV_DispatchThreadID)
{
    if (any(id >= OCEAN_N))
        return;
    const float2 k = (float2(id) - OCEAN_N * 0.5) * (2.0 * PI / g_O.patchSize);
    const float kLen = max(length(k), 1e-4);
    const float omega = sqrt(9.81 * kLen); // deep-water dispersion
    const float4 h0 = asfloat(GetBuffer(g_O.h0).Load4((id.y * OCEAN_N + id.x) * 16));
    float s, c;
    sincos(omega * g_O.time, s, c);
    const float2 h = CMul(h0.xy, float2(c, s)) + CMul(h0.zw, float2(c, -s));
    const float2 ih = float2(-h.y, h.x);    // i h
    const float2 mih = float2(h.y, -h.x);   // -i h
    const float lambda = g_O.choppiness;
    const float2 kn = k / kLen;
    // Displacement and its derivatives in the frequency domain.
    const float2 dx = mih * kn.x * lambda, dz = mih * kn.y * lambda;
    const float2 dydx = ih * k.x, dydz = ih * k.y;
    const float2 dxdx = h * (k.x * kn.x) * lambda, dzdz = h * (k.y * kn.y) * lambda, dxdz = h * (k.x * kn.y) * lambda;
    // Pairs of real fields: X + iY (each field is the transform of a real signal).
    const float2 a = dx + float2(-h.y, h.x);          // Dx + i Dy
    const float2 b = dz + float2(-dxdz.y, dxdz.x);    // Dz + i Dxdz
    const float2 cc = dydx + float2(-dydz.y, dydz.x); // Dydx + i Dydz
    const float2 d = dxdx + float2(-dzdz.y, dzdz.x);  // Dxdx + i Dzdz
    GetRWTexture2D(g_O.specA)[id] = float4(a, b);
    GetRWTexture2D(g_O.specB)[id] = float4(cc, d);
}

groupshared float4 s_a[2][OCEAN_N];
groupshared float4 s_b[2][OCEAN_N];

uint ReverseBits(uint v) { return reversebits(v) >> (32 - LOG2_N); }

[numthreads(OCEAN_N, 1, 1)]
void FftCS(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID)
{
    const uint t = tid.x;
    const uint row = gid.x;
    const uint2 load = g_O.columns ? uint2(row, ReverseBits(t)) : uint2(ReverseBits(t), row);
    RWTexture2D<float4> A = GetRWTexture2D(g_O.specA);
    RWTexture2D<float4> B = GetRWTexture2D(g_O.specB);
    s_a[0][t] = A[load];
    s_b[0][t] = B[load];
    GroupMemoryBarrierWithGroupSync();
    uint ping = 0;
    [unroll] for (uint stage = 1; stage <= LOG2_N; ++stage) {
        const uint m = 1u << stage, half = m >> 1;
        const uint kk = t & (m - 1);
        const uint base = t - kk;
        const uint j = kk & (half - 1);
        float s, c;
        sincos(2.0 * PI * float(j) / float(m), s, c); // inverse transform: e^{+i...}
        const float2 w = float2(c, s);
        const float sign = kk < half ? 1.0 : -1.0;
        const float4 ea = s_a[ping][base + j], oa = s_a[ping][base + j + half];
        const float4 eb = s_b[ping][base + j], ob = s_b[ping][base + j + half];
        s_a[ping ^ 1][t] = ea + sign * float4(CMul(w, oa.xy), CMul(w, oa.zw));
        s_b[ping ^ 1][t] = eb + sign * float4(CMul(w, ob.xy), CMul(w, ob.zw));
        ping ^= 1;
        GroupMemoryBarrierWithGroupSync();
    }
    const uint2 store = g_O.columns ? uint2(row, t) : uint2(t, row);
    A[store] = s_a[ping][t];
    B[store] = s_b[ping][t];
}

[numthreads(8, 8, 1)]
void AssembleCS(uint2 id : SV_DispatchThreadID)
{
    if (any(id >= OCEAN_N))
        return;
    const float sign = ((id.x + id.y) & 1) ? -1.0 : 1.0; // the spectrum was centered at N/2
    const float4 a = GetRWTexture2D(g_O.specA)[id] * sign;
    const float4 b = GetRWTexture2D(g_O.specB)[id] * sign;
    const float dx = a.x, dy = a.y, dz = a.z, dxdz = a.w;
    const float dydx = b.x, dydz = b.y, dxdx = b.z, dzdz = b.w;
    // Whitecaps: the surface folds where the Jacobian of the horizontal displacement drops; foam builds up there
    // and fades over seconds.
    const float jacobian = (1.0 + dxdx) * (1.0 + dzdz) - dxdz * dxdz;
    RWTexture2D<float4> disp = GetRWTexture2D(g_O.displacement);
    const float previous = disp[id].w;
    const float fresh = saturate((g_O.foamThreshold - jacobian) * g_O.foamBias);
    const float foam = max(previous * exp(-g_O.foamDecay * g_O.dt), fresh);
    disp[id] = float4(dx, dy, dz, foam);
    GetRWTexture2D(g_O.derivatives)[id] = float4(dydx, dydz, dxdx, dzdz);
}

[numthreads(8, 8, 1)]
void MipCS(uint2 id : SV_DispatchThreadID)
{
    if (any(id >= g_O.size))
        return;
    const uint2 s = id * 2;
    RWTexture2D<float4> sd = GetRWTexture2D(g_O.srcDisplacement);
    RWTexture2D<float4> sr = GetRWTexture2D(g_O.srcDerivatives);
    GetRWTexture2D(g_O.displacement)[id] = (sd[s] + sd[s + uint2(1, 0)] + sd[s + uint2(0, 1)] + sd[s + uint2(1, 1)]) * 0.25;
    GetRWTexture2D(g_O.derivatives)[id] = (sr[s] + sr[s + uint2(1, 0)] + sr[s + uint2(0, 1)] + sr[s + uint2(1, 1)]) * 0.25;
}
