// Water: copy of the scene (refraction / thickness), then a grid displaced by Gerstner waves shaded with Fresnel sky
// reflection, sun glints, refraction, depth absorption and foam. Pre-exposed HDR output, depth written (reversed-Z).

#include "Common/Bindless.hlsli"
#include "Sky/Atmosphere.hlsli"

// --- Scene copy ------------------------------------------------------------------------------------------------------

struct CopyParams
{
    uint srcColor;
    uint srcDepth;
    uint dstColor; // UAV RGBA16F
    uint dstDepth; // UAV R32F
    uint width;
    uint height;
};
ZX_PUSH_CONSTANTS(CopyParams, g_Copy);

[numthreads(8, 8, 1)]
void CopySceneCS(uint2 id : SV_DispatchThreadID)
{
    if (id.x >= g_Copy.width || id.y >= g_Copy.height)
        return;
    GetRWTexture2D(g_Copy.dstColor)[id] = GetTexture2D(g_Copy.srcColor).Load(int3(id, 0));
    GetRWTexture2D(g_Copy.dstDepth)[id] = GetTexture2D(g_Copy.srcDepth).Load(int3(id, 0)).rrrr;
}

// --- Surface ---------------------------------------------------------------------------------------------------------

#define WAVE_COUNT 4

struct WaterParams
{
    float4x4 viewProj;
    float4x4 world;
    float3 cameraPosition;
    float time;
    float2 size;
    uint grid;
    float nearPlane;
    float4 waves[WAVE_COUNT];     // xy = direction, z = wavenumber k, w = amplitude
    float4 waveMotion[WAVE_COUNT]; // x = angular frequency, y = steepness Q
    float3 shallowColor;
    float clarity;
    float3 deepColor;
    float refraction;
    float3 sunRadiance; // pre-exposed sun color x illuminance
    float foam;
    float3 dirToSun;
    float shoreFoam;
    float3 ambient; // pre-exposed sky light (flat)
    float roughness;
    float3 skyScale; // sky-view LUT value -> pre-exposed radiance
    uint skyView;    // SRV or invalid (flat ambient reflections)
    uint sceneColor;
    uint sceneDepth;
    float2 invRenderSize;
};
ZX_PASS_CONSTANTS(WaterParams, g_Water);

struct VSOut
{
    float4 position : SV_Position;
    float3 worldPos : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float crest : TEXCOORD2; // 0..1: how folded the surface is (foam on wave tops)
};

/// Sum of Gerstner waves at a world xz (GPU Gems 1, ch. 1).
void Gerstner(float2 xz, out float3 offset, out float3 normal, out float crest)
{
    offset = 0;
    float3 n = float3(0, 1, 0);
    float fold = 0;
    [unroll] for (uint i = 0; i < WAVE_COUNT; ++i) {
        const float2 d = g_Water.waves[i].xy;
        const float k = g_Water.waves[i].z;
        const float a = g_Water.waves[i].w;
        const float q = g_Water.waveMotion[i].y;
        const float f = k * dot(d, xz) - g_Water.waveMotion[i].x * g_Water.time;
        float s, c;
        sincos(f, s, c);
        offset += float3(q * a * d.x * c, a * s, q * a * d.y * c);
        const float wa = k * a;
        n.x -= d.x * wa * c;
        n.z -= d.y * wa * c;
        n.y -= q * wa * s;
        fold += q * wa * s;
    }
    normal = normalize(n);
    crest = saturate(fold);
}

VSOut WaterVS(uint vertexId : SV_VertexID)
{
    // Grid of `grid` x `grid` quads, two triangles each, generated from the vertex id.
    const uint quad = vertexId / 6;
    const uint corner = vertexId % 6;
    static const uint2 Corners[6] = {uint2(0, 0), uint2(0, 1), uint2(1, 1), uint2(0, 0), uint2(1, 1), uint2(1, 0)};
    const uint2 cell = uint2(quad % g_Water.grid, quad / g_Water.grid) + Corners[corner];
    const float2 local = (float2(cell) / g_Water.grid - 0.5) * g_Water.size;
    float3 world = mul(g_Water.world, float4(local.x, 0, local.y, 1)).xyz;
    // Waves fade out within a few meters of the edges so the outline stays put.
    const float2 edge = min(cell, g_Water.grid - cell) / float(g_Water.grid) * g_Water.size;
    const float fade = saturate(min(edge.x, edge.y) / 3.0);
    float3 offset, normal;
    float crest;
    Gerstner(world.xz, offset, normal, crest);
    world += offset * fade;
    VSOut o;
    o.position = mul(g_Water.viewProj, float4(world, 1));
    o.worldPos = world;
    o.normal = normalize(lerp(float3(0, 1, 0), normal, fade));
    o.crest = crest * fade;
    return o;
}

// Small procedural detail (no textures): value noise and its gradient.
float Hash(float2 p) { return frac(sin(dot(p, float2(127.1, 311.7))) * 43758.5453); }

float Noise(float2 p)
{
    const float2 i = floor(p);
    const float2 f = frac(p);
    const float2 u = f * f * (3.0 - 2.0 * f);
    return lerp(lerp(Hash(i), Hash(i + float2(1, 0)), u.x), lerp(Hash(i + float2(0, 1)), Hash(i + float2(1, 1)), u.x), u.y);
}

float2 NoiseGradient(float2 p)
{
    const float e = 0.05;
    return float2(Noise(p + float2(e, 0)) - Noise(p - float2(e, 0)), Noise(p + float2(0, e)) - Noise(p - float2(0, e))) / (2.0 * e);
}

float3 SkyRadiance(float3 dir)
{
    if (g_Water.skyView == ZX_INVALID_INDEX)
        return g_Water.ambient;
    dir.y = max(dir.y, 0.01); // reflections never see below the horizon
    return GetTexture2D(g_Water.skyView).SampleLevel(g_LinearClamp, SkyViewUV(normalize(dir)), 0).rgb * g_Water.skyScale;
}

/// View-space distance from a reversed-Z infinite projection depth.
float LinearDepth(float d) { return g_Water.nearPlane / max(d, 1e-7); }

float4 WaterPS(VSOut i, bool front : SV_IsFrontFace) : SV_Target0
{
    const float3 toCamera = g_Water.cameraPosition - i.worldPos;
    const float3 v = normalize(toCamera);
    // Detail ripples on top of the waves (two scrolling noise layers).
    const float2 p = i.worldPos.xz;
    const float t = g_Water.time;
    const float2 ripples = NoiseGradient(p * 0.9 + float2(t * 0.35, t * 0.2)) * 0.6 + NoiseGradient(p * 2.3 - float2(t * 0.25, -t * 0.4)) * 0.3;
    const float detail = saturate(1.0 - length(toCamera) / 120.0); // calmer far away (less shimmering)
    float3 n = normalize(i.normal + float3(-ripples.x, 0, -ripples.y) * 0.12 * detail);
    if (!front)
        n = -n;

    // Scene behind the water: thickness along the view ray and refraction.
    const float2 uv = i.position.xy * g_Water.invRenderSize;
    Texture2D<float4> sceneDepth = GetTexture2D(g_Water.sceneDepth);
    Texture2D<float4> sceneColor = GetTexture2D(g_Water.sceneColor);
    const float waterZ = LinearDepth(i.position.z);
    float sceneZ = LinearDepth(sceneDepth.SampleLevel(g_PointClamp, uv, 0).r);
    float thickness = max(sceneZ - waterZ, 0.0);
    float2 refractUv = uv + n.xz * g_Water.refraction * saturate(thickness * 0.5);
    const float refractedZ = LinearDepth(sceneDepth.SampleLevel(g_PointClamp, refractUv, 0).r);
    if (refractedZ < waterZ) // the distorted sample is in front of the water: don't refract
        refractUv = uv;
    else
        thickness = max(refractedZ - waterZ, 0.0);
    const float3 behind = sceneColor.SampleLevel(g_LinearClamp, refractUv, 0).rgb;

    // Absorption (red first) and in-scattered body color.
    const float depthFactor = thickness / max(g_Water.clarity, 0.05);
    const float3 transmittance = exp(-depthFactor * float3(2.2, 0.75, 0.45));
    const float3 bodyColor = lerp(g_Water.deepColor, g_Water.shallowColor, exp(-depthFactor * 0.8));
    const float sunNdotL = saturate(g_Water.dirToSun.y);
    const float3 bodyLight = g_Water.ambient + g_Water.sunRadiance * sunNdotL / ATM_PI;
    const float3 under = behind * transmittance + bodyColor * bodyLight * (1.0 - transmittance);

    // Reflection (Schlick Fresnel, F0 = 0.02 for water) and the sun glint (GGX).
    const float nDotV = saturate(dot(n, v));
    const float fresnel = 0.02 + 0.98 * pow(1.0 - nDotV, 5.0);
    const float3 reflected = SkyRadiance(reflect(-v, n));
    const float3 h = normalize(v + g_Water.dirToSun);
    const float a2 = g_Water.roughness * g_Water.roughness * g_Water.roughness * g_Water.roughness;
    const float nDotH = saturate(dot(n, h));
    const float dTerm = a2 / (ATM_PI * pow(nDotH * nDotH * (a2 - 1.0) + 1.0, 2.0));
    const float nDotL = saturate(dot(n, g_Water.dirToSun));
    const float3 glint = g_Water.sunRadiance * min(dTerm * fresnel * 0.25 / max(nDotV, 0.05), 50.0) * nDotL;
    float3 color = lerp(under, reflected, fresnel) + glint;

    // Foam along shores and on folded crests, broken up by noise.
    const float shore = 1.0 - saturate(thickness / max(g_Water.shoreFoam, 0.01));
    const float crest = saturate((i.crest - 0.5) * 4.0) * 0.8;
    const float pattern = Noise(p * 3.0 + t * 0.3) * Noise(p * 7.0 - t * 0.2);
    const float foam = saturate((max(shore, crest) - (1.0 - pattern * 1.8)) * 3.0) * g_Water.foam;
    const float3 foamColor = 0.85 * (g_Water.ambient + g_Water.sunRadiance * saturate(dot(n, g_Water.dirToSun)) / ATM_PI);
    color = lerp(color, foamColor, foam);
    return float4(min(color, 60000.0), 1.0);
}
