#ifndef ZX_ATMOSPHERE_HLSLI
#define ZX_ATMOSPHERE_HLSLI

// Physically based atmosphere (Hillaire 2020, "A Scalable and Production Ready Sky and Atmosphere Rendering
// Technique"), own implementation. Distances in kilometers; planet center at the origin; luminance is relative to a
// sun illuminance of 1 (multiplied by the sun's illuminance at the top of the atmosphere when used).
// Keep the constants in sync with render::Atmosphere (Sky.cpp).

static const float ATM_PI = 3.14159265359;
static const float ATM_BOTTOM = 6360.0;
static const float ATM_TOP = 6460.0;
static const float3 ATM_RAYLEIGH_SCATTERING = float3(5.802, 13.558, 33.1) * 1e-3;
static const float ATM_RAYLEIGH_SCALE = 8.0;
static const float ATM_MIE_SCATTERING = 3.996e-3;
static const float ATM_MIE_EXTINCTION = 4.440e-3;
static const float ATM_MIE_SCALE = 1.2;
static const float ATM_MIE_G = 0.8;
static const float3 ATM_OZONE_ABSORPTION = float3(0.650, 1.881, 0.085) * 1e-3;
static const float3 ATM_GROUND_ALBEDO = float3(0.3, 0.3, 0.3);

#define TRANSMITTANCE_LUT_W 256
#define TRANSMITTANCE_LUT_H 64
#define MULTISCATTER_LUT_SIZE 32
#define SKYVIEW_LUT_W 192
#define SKYVIEW_LUT_H 108

struct MediumSample
{
    float3 scattering;
    float3 extinction;
    float3 rayleigh;
    float mie;
};

MediumSample SampleMedium(float height) // km above ground
{
    const float rayleighDensity = exp(-height / ATM_RAYLEIGH_SCALE);
    const float mieDensity = exp(-height / ATM_MIE_SCALE);
    const float ozoneDensity = max(0.0, 1.0 - abs(height - 25.0) / 15.0);
    MediumSample m;
    m.rayleigh = ATM_RAYLEIGH_SCATTERING * rayleighDensity;
    m.mie = ATM_MIE_SCATTERING * mieDensity;
    m.scattering = m.rayleigh + m.mie;
    m.extinction = m.rayleigh + ATM_MIE_EXTINCTION * mieDensity + ATM_OZONE_ABSORPTION * ozoneDensity;
    return m;
}

/// Distance along the ray to the sphere (nearest positive hit), or -1.
float RaySphere(float3 origin, float3 dir, float radius)
{
    const float b = dot(origin, dir);
    const float c = dot(origin, origin) - radius * radius;
    const float disc = b * b - c;
    if (disc < 0.0)
        return -1.0;
    const float s = sqrt(disc);
    const float t0 = -b - s, t1 = -b + s;
    return t0 >= 0.0 ? t0 : (t1 >= 0.0 ? t1 : -1.0);
}

float RayleighPhase(float cosTheta) { return 3.0 / (16.0 * ATM_PI) * (1.0 + cosTheta * cosTheta); }

float MiePhase(float cosTheta) // Cornette-Shanks
{
    const float g = ATM_MIE_G, g2 = g * g;
    const float k = 3.0 / (8.0 * ATM_PI) * (1.0 - g2) / (2.0 + g2);
    return k * (1.0 + cosTheta * cosTheta) / pow(max(1.0 + g2 - 2.0 * g * cosTheta, 1e-4), 1.5);
}

// Transmittance LUT parameterization (Bruneton): uv <-> (r, mu).
float2 TransmittanceUV(float r, float mu)
{
    const float H = sqrt(ATM_TOP * ATM_TOP - ATM_BOTTOM * ATM_BOTTOM);
    const float rho = sqrt(max(r * r - ATM_BOTTOM * ATM_BOTTOM, 0.0));
    const float disc = r * r * (mu * mu - 1.0) + ATM_TOP * ATM_TOP;
    const float d = max(0.0, -r * mu + sqrt(max(disc, 0.0)));
    const float dMin = ATM_TOP - r, dMax = rho + H;
    return float2((d - dMin) / (dMax - dMin), rho / H);
}

void TransmittanceRMu(float2 uv, out float r, out float mu)
{
    const float H = sqrt(ATM_TOP * ATM_TOP - ATM_BOTTOM * ATM_BOTTOM);
    const float rho = H * uv.y;
    r = sqrt(rho * rho + ATM_BOTTOM * ATM_BOTTOM);
    const float dMin = ATM_TOP - r, dMax = rho + H;
    const float d = dMin + uv.x * (dMax - dMin);
    mu = d == 0.0 ? 1.0 : (H * H - rho * rho - d * d) / (2.0 * r * d);
    mu = clamp(mu, -1.0, 1.0);
}

float3 SampleTransmittance(Texture2D<float4> lut, SamplerState s, float r, float mu)
{
    return lut.SampleLevel(s, TransmittanceUV(r, mu), 0).rgb;
}

float2 MultiScatterUV(float r, float sunCosZenith)
{
    return saturate(float2(sunCosZenith * 0.5 + 0.5, (r - ATM_BOTTOM) / (ATM_TOP - ATM_BOTTOM)));
}

// Sky-view LUT: longitude around the up axis, latitude remapped non-linearly to keep detail at the horizon.
float2 SkyViewUV(float3 dir)
{
    const float lon = atan2(dir.z, dir.x);
    const float lat = asin(clamp(dir.y, -1.0, 1.0));
    const float v = 0.5 + 0.5 * sign(lat) * sqrt(abs(lat) / (ATM_PI * 0.5));
    return float2(lon / (2.0 * ATM_PI) + 0.5, 1.0 - v);
}

float3 SkyViewDir(float2 uv)
{
    const float lon = (uv.x - 0.5) * 2.0 * ATM_PI;
    const float t = (1.0 - uv.y - 0.5) * 2.0;
    const float lat = sign(t) * t * t * (ATM_PI * 0.5);
    return float3(cos(lat) * cos(lon), sin(lat), cos(lat) * sin(lon));
}

#endif // ZX_ATMOSPHERE_HLSLI
