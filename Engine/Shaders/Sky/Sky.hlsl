// Physical sky: LUT generation (compute) and sky/sun rendering behind the scene (fullscreen pass at far depth).

#include "Common/Bindless.hlsli"
#include "Sky/Atmosphere.hlsli"
#include "Sky/Hdri.hlsli"

struct SkyParams
{
    float4x4 invViewProj;   // clip -> world (sky pass)
    float3 cameraPosition;  // world meters
    float exposure;
    float3 sunDirection;    // direction to the sun (normalized)
    float sunIlluminance;   // lux at the top of the atmosphere (x sun color)
    float3 sunColor;
    float sunAngularRadius; // radians
    uint transmittanceLut;  // SRV
    uint multiScatterLut;   // SRV
    uint skyViewLut;        // SRV
    uint outputUav;         // compute target
    float cameraAltitude;   // km above ground
    uint hdri;              // SRV of the HDRI panorama (ZX_INVALID_INDEX = physical sky)
    float hdriRotation;     // radians around +Y
    float hdriLod;          // background mip
    float3 hdriClampDir;    // sun cone clamped in the sky lighting
    float hdriClampCos;
    float hdriClampLum;
    float hdriLutLod;       // mip resampled into the sky-view LUT
    float skyBrightness;    // visible sky (and distant fog colour): artistic multiplier
    float starLuminance;    // night sky (absolute cd/m^2, added on top): brightest stars
    float3 moonDirection;   // direction to the moon
    float moonIlluminance;  // lux from the moon disk (0 = none)
    float nightLuminance;   // the dark sky itself
    float3 pad;
};
ZX_PASS_CONSTANTS(SkyParams, g_Sky);

// --- Transmittance LUT -------------------------------------------------------------------------------------------

[numthreads(8, 8, 1)]
void TransmittanceCS(uint2 id : SV_DispatchThreadID)
{
    if (id.x >= TRANSMITTANCE_LUT_W || id.y >= TRANSMITTANCE_LUT_H)
        return;
    const float2 uv = (float2(id) + 0.5) / float2(TRANSMITTANCE_LUT_W, TRANSMITTANCE_LUT_H);
    float r, mu;
    TransmittanceRMu(uv, r, mu);
    const float3 origin = float3(0, r, 0);
    const float3 dir = float3(sqrt(max(1.0 - mu * mu, 0.0)), mu, 0);
    const float tMax = RaySphere(origin, dir, ATM_TOP);
    const uint steps = 40;
    const float dt = tMax / steps;
    float3 opticalDepth = 0;
    for (uint i = 0; i < steps; ++i) {
        const float3 p = origin + dir * ((i + 0.5) * dt);
        opticalDepth += SampleMedium(length(p) - ATM_BOTTOM).extinction * dt;
    }
    GetRWTexture2D(g_Sky.outputUav)[id] = float4(exp(-opticalDepth), 1);
}

// --- Multiple scattering LUT -------------------------------------------------------------------------------------

[numthreads(8, 8, 1)]
void MultiScatterCS(uint2 id : SV_DispatchThreadID)
{
    if (id.x >= MULTISCATTER_LUT_SIZE || id.y >= MULTISCATTER_LUT_SIZE)
        return;
    const float2 uv = (float2(id) + 0.5) / MULTISCATTER_LUT_SIZE;
    const float sunCos = uv.x * 2.0 - 1.0;
    const float r = ATM_BOTTOM + uv.y * (ATM_TOP - ATM_BOTTOM) + 1e-3;
    const float3 origin = float3(0, r, 0);
    const float3 sunDir = float3(sqrt(max(1.0 - sunCos * sunCos, 0.0)), sunCos, 0);
    Texture2D<float4> transmittance = GetTexture2D(g_Sky.transmittanceLut);

    // Integrate over the sphere of directions (8 x 8 stratified), isotropic phase.
    float3 secondOrder = 0, transfer = 0;
    const uint dirs = 8;
    for (uint a = 0; a < dirs; ++a)
        for (uint b = 0; b < dirs; ++b) {
            const float cosT = 1.0 - 2.0 * ((a + 0.5) / dirs);
            const float phi = 2.0 * ATM_PI * ((b + 0.5) / dirs);
            const float sinT = sqrt(max(1.0 - cosT * cosT, 0.0));
            const float3 dir = float3(sinT * cos(phi), cosT, sinT * sin(phi));
            const float tGround = RaySphere(origin, dir, ATM_BOTTOM);
            const float tTop = RaySphere(origin, dir, ATM_TOP);
            const float tMax = tGround > 0.0 ? tGround : tTop;
            const uint steps = 20;
            const float dt = tMax / steps;
            float3 throughput = 1, L = 0, f = 0;
            for (uint i = 0; i < steps; ++i) {
                const float3 p = origin + dir * ((i + 0.5) * dt);
                const float h = length(p);
                const MediumSample m = SampleMedium(h - ATM_BOTTOM);
                const float3 sunT = SampleTransmittance(transmittance, g_LinearClamp, h, dot(p / h, sunDir));
                const float3 stepT = exp(-m.extinction * dt);
                const float3 integ = (1.0 - stepT) / max(m.extinction, 1e-7); // analytic integral over the step
                L += throughput * m.scattering * sunT * (1.0 / (4.0 * ATM_PI)) * integ;
                f += throughput * m.scattering * integ;
                throughput *= stepT;
            }
            if (tGround > 0.0) {
                const float3 p = origin + dir * tGround;
                const float3 n = normalize(p);
                const float3 sunT = SampleTransmittance(transmittance, g_LinearClamp, ATM_BOTTOM, dot(n, sunDir));
                L += throughput * sunT * saturate(dot(n, sunDir)) * ATM_GROUND_ALBEDO / ATM_PI;
            }
            secondOrder += L / (dirs * dirs);
            transfer += f / (dirs * dirs);
        }
    // Infinite series of isotropic bounces: psi = L2 / (1 - f).
    const float3 psi = secondOrder / max(1.0 - transfer, 1e-3);
    GetRWTexture2D(g_Sky.outputUav)[id] = float4(psi, 1);
}

// --- Sky-view LUT ------------------------------------------------------------------------------------------------

float3 IntegrateSky(float3 origin, float3 dir, float3 sunDir, uint steps, out float3 throughput)
{
    Texture2D<float4> transmittance = GetTexture2D(g_Sky.transmittanceLut);
    Texture2D<float4> multiScatter = GetTexture2D(g_Sky.multiScatterLut);
    const float tGround = RaySphere(origin, dir, ATM_BOTTOM);
    const float tTop = RaySphere(origin, dir, ATM_TOP);
    const float tMax = tGround > 0.0 ? tGround : tTop;
    throughput = 1;
    if (tMax <= 0.0)
        return 0;
    const float cosTheta = dot(dir, sunDir);
    const float phaseR = RayleighPhase(cosTheta), phaseM = MiePhase(cosTheta);
    float3 L = 0;
    float tPrev = 0;
    for (uint i = 0; i < steps; ++i) {
        // Quadratic step distribution: denser near the camera.
        const float t1 = tMax * pow((i + 1.0) / steps, 2.0);
        const float dt = t1 - tPrev;
        const float3 p = origin + dir * (tPrev + dt * 0.5);
        tPrev = t1;
        const float h = length(p);
        const MediumSample m = SampleMedium(h - ATM_BOTTOM);
        const float sunCos = dot(p / h, sunDir);
        const float3 sunT = SampleTransmittance(transmittance, g_LinearClamp, h, sunCos);
        // Planet shadow on single scattering.
        const float planet = RaySphere(p, sunDir, ATM_BOTTOM) > 0.0 ? 0.0 : 1.0;
        const float3 psi = multiScatter.SampleLevel(g_LinearClamp, MultiScatterUV(h, sunCos), 0).rgb;
        const float3 S = planet * sunT * (m.rayleigh * phaseR + m.mie * phaseM) + psi * m.scattering;
        const float3 stepT = exp(-m.extinction * dt);
        L += throughput * S * (1.0 - stepT) / max(m.extinction, 1e-7);
        throughput *= stepT;
    }
    return L;
}

[numthreads(8, 8, 1)]
void SkyViewCS(uint2 id : SV_DispatchThreadID)
{
    if (id.x >= SKYVIEW_LUT_W || id.y >= SKYVIEW_LUT_H)
        return;
    const float2 uv = (float2(id) + 0.5) / float2(SKYVIEW_LUT_W, SKYVIEW_LUT_H);
    const float3 dir = SkyViewDir(uv);
    const float3 origin = float3(0, ATM_BOTTOM + g_Sky.cameraAltitude, 0);
    float3 throughput;
    const float3 L = IntegrateSky(origin, dir, g_Sky.sunDirection, 30, throughput);
    GetRWTexture2D(g_Sky.outputUav)[id] = float4(L, 1);
}

// HDRI sky: the panorama (sun clamped) resampled into the sky-view LUT; sunIlluminance carries the intensity.
[numthreads(8, 8, 1)]
void SkyViewFromHdriCS(uint2 id : SV_DispatchThreadID)
{
    if (id.x >= SKYVIEW_LUT_W || id.y >= SKYVIEW_LUT_H)
        return;
    const float2 uv = (float2(id) + 0.5) / float2(SKYVIEW_LUT_W, SKYVIEW_LUT_H);
    const float3 dir = SkyViewDir(uv);
    float3 L = SampleHdri(g_Sky.hdri, dir, g_Sky.hdriRotation, g_Sky.hdriLutLod);
    L = ClampHdriSun(L, dir, g_Sky.hdriClampDir, g_Sky.hdriClampCos, g_Sky.hdriClampLum);
    GetRWTexture2D(g_Sky.outputUav)[id] = float4(max(L, 0.0), 1);
}

// --- Sky pass ----------------------------------------------------------------------------------------------------

struct VSOutput
{
    float4 position : SV_Position;
    float2 ndc : TEXCOORD0;
};

VSOutput SkyVS(uint vertexId : SV_VertexID)
{
    const float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    VSOutput o;
    o.ndc = uv * float2(2, -2) + float2(-1, 1);
    o.position = float4(o.ndc, 0.0, 1.0); // reversed-Z far plane: only background pixels pass GreaterEqual
    return o;
}

float Hash31(float3 p)
{
    p = frac(p * 0.1031);
    p += dot(p, p.zyx + 31.32);
    return frac((p.x + p.y) * p.z);
}

/// Stars, the moon and the dark sky glow in absolute luminance (cd/m^2), seen through the atmosphere.
float3 NightSky(float3 dir)
{
    if (g_Sky.starLuminance <= 0.0 && g_Sky.moonIlluminance <= 0.0 && g_Sky.nightLuminance <= 0.0)
        return 0.0;
    if (dir.y < -0.02)
        return 0.0;
    const float r = ATM_BOTTOM + g_Sky.cameraAltitude;
    const float3 T = SampleTransmittance(GetTexture2D(g_Sky.transmittanceLut), g_LinearClamp, r, max(dir.y, 0.0));
    float3 night = g_Sky.nightLuminance * float3(0.35, 0.45, 0.8) * (0.7 + 0.3 * saturate(1.0 - dir.y));
    // Stars: one candidate per cell of a fine grid on the sphere; few cells hold one, brightness falls off steeply.
    if (g_Sky.starLuminance > 0.0) {
        const float3 p = dir * 260.0;
        const float3 cell = floor(p);
        const float h = Hash31(cell);
        if (h > 0.982) {
            const float3 center = cell + 0.5 + (float3(Hash31(cell + 17.1), Hash31(cell + 41.7), Hash31(cell + 73.3)) - 0.5) * 0.5;
            const float d = length(p - center);
            const float strength = pow((h - 0.982) / 0.018, 4.0);
            const float t = Hash31(cell + 5.3);
            const float3 tint = lerp(float3(0.75, 0.85, 1.0), float3(1.0, 0.85, 0.65), t);
            night += g_Sky.starLuminance * strength * exp(-d * d * 30.0) * tint;
        }
    }
    // The moon: a disk (twice its real size, like the sun) with darker seas.
    if (g_Sky.moonIlluminance > 0.0) {
        const float cosMoon = dot(dir, g_Sky.moonDirection);
        const float radius = 0.0093;
        const float cosRadius = cos(radius);
        if (cosMoon > cosRadius) {
            const float solidAngle = 2.0 * ATM_PI * (1.0 - cosRadius);
            const float3 local = (dir - g_Sky.moonDirection * cosMoon) / radius;
            const float seas = Hash31(floor(local * 3.0 + 7.0)) * 0.25 + Hash31(floor(local * 7.0 + 3.0)) * 0.15;
            const float edge = saturate((cosMoon - cosRadius) / (1.0 - cosRadius) * 8.0);
            night += g_Sky.moonIlluminance / solidAngle * (1.0 - seas) * edge * float3(0.95, 0.95, 1.0);
        }
    }
    return night * T;
}

float4 SkyPS(VSOutput i) : SV_Target0
{
    const float4 p = mul(g_Sky.invViewProj, float4(i.ndc, 1.0, 1.0));
    const float3 dir = normalize(p.xyz / p.w - g_Sky.cameraPosition);
    if (g_Sky.hdri != ZX_INVALID_INDEX) {
        const float3 H = SampleHdri(g_Sky.hdri, dir, g_Sky.hdriRotation, g_Sky.hdriLod);
        return float4(min(max(H, 0.0) * (g_Sky.sunIlluminance * g_Sky.exposure), 60000.0), 1.0);
    }
    float3 L = GetTexture2D(g_Sky.skyViewLut).SampleLevel(g_LinearClamp, SkyViewUV(dir), 0).rgb;
    // Sun disk with limb darkening, attenuated by the atmosphere.
    const float cosSun = dot(dir, g_Sky.sunDirection);
    const float cosRadius = cos(g_Sky.sunAngularRadius);
    if (cosSun > cosRadius && dir.y > -0.01) {
        const float r = ATM_BOTTOM + g_Sky.cameraAltitude;
        const float3 T = SampleTransmittance(GetTexture2D(g_Sky.transmittanceLut), g_LinearClamp, r, dir.y);
        const float solidAngle = 2.0 * ATM_PI * (1.0 - cosRadius);
        const float x = saturate((1.0 - cosSun) / (1.0 - cosRadius));
        const float limb = 1.0 - 0.6 * (1.0 - sqrt(1.0 - x));
        L += T * limb / solidAngle;
    }
    float3 color = L * g_Sky.sunColor * (g_Sky.sunIlluminance * g_Sky.skyBrightness);
    color += NightSky(dir);
    // Pre-exposed; clamp to keep the 11/10-bit float target finite.
    return float4(min(color * g_Sky.exposure, 60000.0), 1.0);
}

// --- Sky irradiance (SH9) ----------------------------------------------------------------------------------------
// Projects the sky-view LUT onto 3rd-order spherical harmonics and convolves with the clamped cosine lobe. Output:
// 9 x float4 (rgb) such that irradiance(n) / pi = sum(c_i * Y_i(n)), i.e. Lambert diffuse = albedo * SHEval(n).

void SHBasis(float3 n, out float y[9])
{
    y[0] = 0.282095;
    y[1] = 0.488603 * n.y;
    y[2] = 0.488603 * n.z;
    y[3] = 0.488603 * n.x;
    y[4] = 1.092548 * n.x * n.y;
    y[5] = 1.092548 * n.y * n.z;
    y[6] = 0.315392 * (3.0 * n.z * n.z - 1.0);
    y[7] = 1.092548 * n.x * n.z;
    y[8] = 0.546274 * (n.x * n.x - n.y * n.y);
}

groupshared float3 gs_sh[256][9];

[numthreads(256, 1, 1)]
void SkyIrradianceCS(uint tid : SV_GroupIndex)
{
    Texture2D<float4> lut = GetTexture2D(g_Sky.skyViewLut);
    float3 acc[9];
    [unroll] for (uint k = 0; k < 9; ++k)
        acc[k] = 0;
    const uint total = SKYVIEW_LUT_W * SKYVIEW_LUT_H;
    const float dLon = 2.0 * ATM_PI / SKYVIEW_LUT_W;
    for (uint i = tid; i < total; i += 256) {
        const uint2 p = uint2(i % SKYVIEW_LUT_W, i / SKYVIEW_LUT_W);
        const float2 uv = (float2(p) + 0.5) / float2(SKYVIEW_LUT_W, SKYVIEW_LUT_H);
        const float3 dir = SkyViewDir(uv);
        // Solid angle of the texel: cos(lat) * dlat * dlon (latitude is non-linear in v).
        const float latA = asin(clamp(SkyViewDir(float2(uv.x, uv.y - 0.5 / SKYVIEW_LUT_H)).y, -1.0, 1.0));
        const float latB = asin(clamp(SkyViewDir(float2(uv.x, uv.y + 0.5 / SKYVIEW_LUT_H)).y, -1.0, 1.0));
        float dOmega = sqrt(max(1.0 - dir.y * dir.y, 0.0)) * abs(latA - latB) * dLon;
        float3 L = lut.Load(int3(p, 0)).rgb;
        if (dir.y < 0.0 && g_Sky.hdri == ZX_INVALID_INDEX) // below the horizon: ground bounce from the horizon radiance
            L = ATM_GROUND_ALBEDO * lut.Load(int3(p.x, SKYVIEW_LUT_H / 2 - 1, 0)).rgb;
        float y[9];
        SHBasis(dir, y);
        [unroll] for (uint k = 0; k < 9; ++k)
            acc[k] += L * y[k] * dOmega;
    }
    [unroll] for (uint k = 0; k < 9; ++k)
        gs_sh[tid][k] = acc[k];
    GroupMemoryBarrierWithGroupSync();
    for (uint s = 128; s > 0; s >>= 1) {
        if (tid < s)
            [unroll] for (uint k = 0; k < 9; ++k)
                gs_sh[tid][k] += gs_sh[tid + s][k];
        GroupMemoryBarrierWithGroupSync();
    }
    if (tid == 0) {
        // Cosine-lobe convolution (A0 = pi, A1 = 2pi/3, A2 = pi/4), then / pi.
        const float band[9] = {1.0, 2.0 / 3.0, 2.0 / 3.0, 2.0 / 3.0, 0.25, 0.25, 0.25, 0.25, 0.25};
        RWByteAddressBuffer outSH = GetRWBuffer(g_Sky.outputUav);
        [unroll] for (uint k = 0; k < 9; ++k)
            outSH.Store4(k * 16, asuint(float4(gs_sh[0][k] * band[k], 0.0)));
    }
}

// --- Exponential height fog --------------------------------------------------------------------------------------
// density(y) = density * exp(-falloff * (y - baseHeight)); the optical depth along the view ray is integrated
// analytically. In-scattering: sky irradiance (SH9, bound as b2) + sun with a Henyey-Greenstein lobe.

struct FogParams
{
    float density;   // 1/m at baseHeight
    float falloff;   // 1/m
    float baseHeight;
    float maxDistance;
    float3 albedo;
    float sunG;      // HG anisotropy
    float3 sunColor; // pre-exposed sun illuminance x color reaching the ground
    float skyScale;  // pre-exposed sky irradiance scale
    uint depth;      // SRV of the scene depth
    float startDistance; // volumetric fog covers the ray up to here
    uint2 pad;
};
ZX_PUSH_CONSTANTS(FogParams, g_Fog);

struct SkySH { float4 c[9]; };
ConstantBuffer<SkySH> g_FogSH : register(b2, space0);

float3 EvalSH(float3 n)
{
    const float y[9] = {0.282095, 0.488603 * n.y, 0.488603 * n.z, 0.488603 * n.x, 1.092548 * n.x * n.y, 1.092548 * n.y * n.z,
                        0.315392 * (3.0 * n.z * n.z - 1.0), 1.092548 * n.x * n.z, 0.546274 * (n.x * n.x - n.y * n.y)};
    float3 e = 0;
    [unroll] for (uint k = 0; k < 9; ++k)
        e += g_FogSH.c[k].rgb * y[k];
    return max(e, 0.0);
}

float4 FogPS(VSOutput i) : SV_Target0
{
    const float d = GetTexture2D(g_Fog.depth).Load(int3(i.position.xy, 0)).r;
    float4 p = mul(g_Sky.invViewProj, float4(i.ndc, max(d, 1e-7), 1.0));
    float3 toPoint = p.xyz / p.w - g_Sky.cameraPosition;
    float dist = length(toPoint);
    const float3 dir = toPoint / max(dist, 1e-4);
    dist = d <= 0.0 ? g_Fog.maxDistance : min(dist, g_Fog.maxDistance);
    // Optical depth: integral of density along the ray.
    const float camY = g_Sky.cameraPosition.y - g_Fog.baseHeight;
    // Integral of the density from the camera to x along the ray.
    const float k = dir.y * g_Fog.falloff;
    const float scale = g_Fog.density * exp(-g_Fog.falloff * camY);
    const float start = min(g_Fog.startDistance, dist);
    const float opticalDepth = abs(k) > 1e-6 ? scale * (exp(-k * start) - exp(-k * dist)) / k : scale * (dist - start);
    if (opticalDepth <= 1e-6)
        discard;
    const float transmittance = exp(-max(opticalDepth, 0.0));
    const float cosTheta = dot(dir, g_Sky.sunDirection);
    const float g = g_Fog.sunG;
    const float hg = (1.0 - g * g) / (4.0 * ATM_PI * pow(max(1.0 + g * g - 2.0 * g * cosTheta, 1e-4), 1.5));
    float3 inscatter = g_Fog.albedo * (EvalSH(dir) * g_Fog.skyScale + g_Fog.sunColor * hg);
    if (g_Sky.hdri == ZX_INVALID_INDEX) {
        // Distant fog takes the color of the sky just above the horizon in the same direction, so the far scene
        // melts into the sky instead of ending in a grey band.
        float3 horizonDir = dir;
        horizonDir.y = max(horizonDir.y, 0.03);
        const float3 sky = GetTexture2D(g_Sky.skyViewLut).SampleLevel(g_LinearClamp, SkyViewUV(normalize(horizonDir)), 0).rgb *
                           g_Sky.sunColor * (g_Sky.sunIlluminance * g_Sky.exposure * g_Sky.skyBrightness);
        inscatter = lerp(inscatter, sky + g_Fog.albedo * g_Fog.sunColor * hg * 0.5, saturate(dist / 1500.0) * 0.8);
    }
    // Premultiplied: result = inscatter * (1 - T) + scene * T (blend: src One, dst SrcAlpha).
    return float4(min(inscatter * (1.0 - transmittance), 60000.0), transmittance);
}
