// Water surface: a mesh generated in the vertex shader (concentric rings around the camera for large water, a regular
// grid for small), displaced by the FFT ocean cascades (Ocean.hlsl), shaded with screen-space reflections over the
// prefiltered sky, the sun (GGX, shadowed by the sun cascades), light scattered through the wave crests, depth
// absorption and in-scattering, refraction, caustics on what lies below, whitecaps and shore foam.
// Pre-exposed HDR output, depth written (reversed-Z).

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

// --- Pattern textures (baked once): seamless caustic web and foam bubbles ---------------------------------------------

struct BakeParams
{
    uint caustics; // UAV R16F
    uint foam;     // UAV R16F
    uint size;
    uint period;   // cells per tile
};
ZX_PUSH_CONSTANTS(BakeParams, g_Bake);

float2 PeriodicHash2(float2 cell, float period)
{
    const float2 p = fmod(fmod(cell, period) + period, period);
    return frac(sin(float2(dot(p, float2(127.1, 311.7)), dot(p, float2(269.5, 183.3)))) * 43758.5453);
}

/// Worley F1 and F2 on a torus of `period` cells.
float2 PeriodicCells(float2 p, float period)
{
    const float2 i = floor(p);
    const float2 f = frac(p);
    float d1 = 8.0, d2 = 8.0;
    [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x) {
            const float2 o = float2(x, y);
            const float2 r = o + 0.15 + 0.7 * PeriodicHash2(i + o, period) - f;
            const float d = dot(r, r);
            if (d < d1) {
                d2 = d1;
                d1 = d;
            } else if (d < d2) {
                d2 = d;
            }
        }
    return float2(sqrt(d1), sqrt(d2));
}

[numthreads(8, 8, 1)]
void BakePatternsCS(uint2 id : SV_DispatchThreadID)
{
    if (any(id >= g_Bake.size))
        return;
    const float period = float(g_Bake.period);
    const float2 p = (float2(id) + 0.5) / float(g_Bake.size) * period;
    // Caustics: thin bright lines along the cell edges, a softer second octave.
    const float2 a = PeriodicCells(p, period);
    const float2 b = PeriodicCells(p * 2.0 + 0.37, period * 2.0);
    const float caustic = (1.0 - smoothstep(0.0, 0.1, a.y - a.x)) + (1.0 - smoothstep(0.0, 0.12, b.y - b.x)) * 0.45;
    GetRWTexture2D(g_Bake.caustics)[id] = caustic;
    // Foam: dense round bubbles with thin gaps, two sizes.
    const float2 c = PeriodicCells(p * 1.5 + 0.11, period * 1.5);
    const float2 d = PeriodicCells(p * 4.0 + 0.73, period * 4.0);
    const float foam = saturate(1.15 - c.x * 1.1) * saturate(1.25 - d.x * 1.0);
    GetRWTexture2D(g_Bake.foam)[id] = foam;
}

// --- Surface ---------------------------------------------------------------------------------------------------------

struct WaterParams
{
    float4x4 viewProj;
    float4x4 invViewProj;
    float4x4 world;      // surface -> world (rotation about Y + translation)
    float4x4 worldToLocal;
    float4x4 shadowMatrices[4];
    float4 cascadeSplits;
    uint4 shadowMaps;
    float3 cameraPosition;
    float time;
    float2 size;          // meters
    uint ringMode;        // 1: rings around the camera, 0: regular grid
    uint grid;            // grid: quads per side | rings: ring count
    float2 ringCenter;    // local xz of the rings' center (camera above the surface, snapped)
    float ringInner;      // radius of the first ring
    float ringGrowth;     // radius ratio between rings
    uint ringSegments;
    uint cascadeCount;
    float nearPlane;
    float amplitude;      // displacement multiplier
    float4 cascadeSizes;  // patch size per cascade (m)
    uint4 displacement;   // SRV per cascade
    uint4 derivatives;    // SRV per cascade
    float3 shallowColor;
    float clarity;
    float3 deepColor;
    float refraction;
    float3 scatterColor;  // light through the crests
    float foam;
    float3 sunRadiance;   // pre-exposed sun color x illuminance
    float shoreFoam;
    float3 dirToSun;
    float roughness;
    float3 ambient;       // pre-exposed sky light (flat)
    float caustics;
    float3 skyScale;      // sky-view LUT value -> pre-exposed radiance
    uint skyView;
    uint skyCube;         // prefiltered sky cube or invalid
    float skyCubeMips;
    float reflectionExposure;
    uint ssr;             // 1: screen-space reflections
    uint sceneColor;
    uint sceneDepth;
    float2 invRenderSize;
    uint cascadesShadow;  // sun shadow cascades available
    float edgeFade;       // meters over which the waves calm down towards the edges (0: none)
    uint ssrSteps;
    uint causticTexture;
    uint foamTexture;
    float3 pad;
};
ZX_PASS_CONSTANTS(WaterParams, g_Water);

struct VSOut
{
    float4 position : SV_Position;
    float3 worldPos : TEXCOORD0;
    float2 baseXZ : TEXCOORD1;  // undisplaced world xz (where the cascades are sampled)
    float fade : TEXCOORD2;     // 0 at the edges (calm), 1 inside
};

float3 SampleDisplacement(float2 xz, float lod)
{
    float3 d = 0;
    [unroll] for (uint c = 0; c < 4; ++c)
        if (c < g_Water.cascadeCount)
            d += GetTexture2D(g_Water.displacement[c]).SampleLevel(g_LinearWrap, xz / g_Water.cascadeSizes[c], lod).xyz;
    return d * g_Water.amplitude;
}

VSOut WaterVS(uint vertexId : SV_VertexID)
{
    static const uint2 Corners[6] = {uint2(0, 0), uint2(0, 1), uint2(1, 1), uint2(0, 0), uint2(1, 1), uint2(1, 0)};
    const uint quad = vertexId / 6;
    const uint2 corner = Corners[vertexId % 6];
    const float2 half = g_Water.size * 0.5;
    float2 local;
    float cell;
    if (g_Water.ringMode) {
        // Rings: segment around, ring outwards; radii grow geometrically (dense near the camera).
        const uint seg = quad % g_Water.ringSegments + corner.x;
        const uint ring = quad / g_Water.ringSegments + corner.y;
        const float radius = ring == 0 ? 0.0 : g_Water.ringInner * pow(g_Water.ringGrowth, float(ring - 1));
        float s, c;
        sincos(float(seg) / float(g_Water.ringSegments) * 6.2831853, s, c);
        local = g_Water.ringCenter + float2(c, s) * radius;
        cell = max(radius * 6.2831853 / float(g_Water.ringSegments), 0.05);
    } else {
        const uint2 cellId = uint2(quad % g_Water.grid, quad / g_Water.grid) + corner;
        local = (float2(cellId) / g_Water.grid - 0.5) * g_Water.size;
        cell = max(g_Water.size.x, g_Water.size.y) / g_Water.grid;
    }
    local = clamp(local, -half, half); // the outline stays exactly the rectangle
    float3 world = mul(g_Water.world, float4(local.x, 0, local.y, 1)).xyz;
    // Calm towards the edges (the outline stays put, pools meet their walls).
    const float2 toEdge = half - abs(local);
    const float fade = g_Water.edgeFade > 0.0 ? saturate(min(toEdge.x, toEdge.y) / g_Water.edgeFade) : 1.0;
    // Waves smaller than the mesh cells are left to the pixel shader (no aliasing in the geometry).
    const float lod = max(log2(cell / max(g_Water.cascadeSizes[0] / 128.0, 1e-3)), 0.0);
    const float3 d = SampleDisplacement(world.xz, lod * 0.5) * fade;
    VSOut o;
    o.baseXZ = world.xz;
    world += d;
    o.position = mul(g_Water.viewProj, float4(world, 1));
    o.worldPos = world;
    o.fade = fade;
    return o;
}

// --- Helpers ---------------------------------------------------------------------------------------------------------

float Hash(float2 p) { return frac(sin(dot(p, float2(127.1, 311.7))) * 43758.5453); }
float2 Hash2(float2 p) { return frac(sin(float2(dot(p, float2(127.1, 311.7)), dot(p, float2(269.5, 183.3)))) * 43758.5453); }

float Noise(float2 p)
{
    const float2 i = floor(p);
    const float2 f = frac(p);
    const float2 u = f * f * (3.0 - 2.0 * f);
    return lerp(lerp(Hash(i), Hash(i + float2(1, 0)), u.x), lerp(Hash(i + float2(0, 1)), Hash(i + float2(1, 1)), u.x), u.y);
}

/// Foam bubbles (baked texture, two scales drifting apart).
float FoamPattern(float2 p, float t)
{
    Texture2D<float4> tex = GetTexture2D(g_Water.foamTexture);
    const float a = tex.Sample(g_LinearWrap, p * 0.11 + float2(t * 0.007, t * 0.004)).r;
    const float b = tex.Sample(g_LinearWrap, p * 0.27 - float2(t * 0.005, -t * 0.009)).r;
    return saturate(a * 0.65 + b * 0.55);
}

/// Caustics: two layers of the baked light web moving across each other (where they cross, the light focuses).
float Caustics(float2 p, float t)
{
    Texture2D<float4> tex = GetTexture2D(g_Water.causticTexture);
    const float2 warp = float2(sin(p.y * 0.9 + t * 1.1), sin(p.x * 0.8 - t * 0.9)) * 0.03;
    const float a = tex.Sample(g_LinearWrap, p * 0.13 + warp + float2(t * 0.021, t * 0.013)).r;
    const float b = tex.Sample(g_LinearWrap, p * 0.17 - warp - float2(t * 0.017, -t * 0.019)).r;
    return min(a, b) * 2.2 + (a + b) * 0.25;
}

float3 SkyRadiance(float3 dir, float roughness)
{
    if (g_Water.skyCube != ZX_INVALID_INDEX)
        return GetTextureCube(g_Water.skyCube).SampleLevel(g_LinearClamp, dir, roughness * g_Water.skyCubeMips).rgb * g_Water.reflectionExposure;
    if (g_Water.skyView == ZX_INVALID_INDEX)
        return g_Water.ambient;
    dir.y = max(dir.y, 0.01);
    return GetTexture2D(g_Water.skyView).SampleLevel(g_LinearClamp, SkyViewUV(normalize(dir)), 0).rgb * g_Water.skyScale;
}

float LinearDepth(float d) { return g_Water.nearPlane / max(d, 1e-7); }

float SunShadow(float3 worldPos, float viewDepth)
{
    if (!g_Water.cascadesShadow)
        return 1.0;
    [unroll] for (uint c = 0; c < 4; ++c) {
        if (viewDepth > g_Water.cascadeSplits[c])
            continue;
        const float4 s = mul(g_Water.shadowMatrices[c], float4(worldPos, 1.0));
        const float3 uvz = s.xyz / s.w;
        if (any(uvz.xy < 0.0) || any(uvz.xy > 1.0) || g_Water.shadowMaps[c] == ZX_INVALID_INDEX)
            continue;
        return GetTexture2D(g_Water.shadowMaps[c]).SampleCmpLevelZero(g_ShadowSampler, uvz.xy, uvz.z);
    }
    return 1.0;
}

float2 ToScreen(float3 world, out float clipW, out float ndcZ)
{
    const float4 c = mul(g_Water.viewProj, float4(world, 1.0));
    clipW = c.w;
    ndcZ = c.z / c.w;
    return float2(c.x / c.w * 0.5 + 0.5, 0.5 - c.y / c.w * 0.5);
}

/// Screen-space reflection from `origin` along `dir`: marches the scene copy's depth. a = confidence.
float4 TraceReflection(float3 origin, float3 dir, float distance)
{
    Texture2D<float4> depth = GetTexture2D(g_Water.sceneDepth);
    float stepLen = 0.15 + distance * 0.02;
    float3 p = origin + dir * stepLen;
    [loop] for (uint i = 0; i < g_Water.ssrSteps; ++i) {
        float w, z;
        const float2 uv = ToScreen(p, w, z);
        if (w <= 0.0 || any(uv < 0.0) || any(uv > 1.0))
            break;
        const float sceneZ = LinearDepth(depth.SampleLevel(g_PointClamp, uv, 0).r);
        const float rayZ = w;
        if (rayZ > sceneZ && rayZ - sceneZ < stepLen * 2.5 + 0.3) {
            // Refine between the last two steps.
            float3 a = p - dir * stepLen, b = p;
            [unroll] for (uint r = 0; r < 4; ++r) {
                const float3 m = (a + b) * 0.5;
                float mw, mz;
                const float2 muv = ToScreen(m, mw, mz);
                if (mw > LinearDepth(depth.SampleLevel(g_PointClamp, muv, 0).r))
                    b = m;
                else
                    a = m;
            }
            float bw, bz;
            const float2 huv = ToScreen(b, bw, bz);
            const float2 edge = saturate(min(huv, 1.0 - huv) * 8.0);
            const float conf = edge.x * edge.y * saturate(1.0 - float(i) / float(g_Water.ssrSteps));
            return float4(GetTexture2D(g_Water.sceneColor).SampleLevel(g_LinearClamp, huv, 0).rgb, conf);
        }
        p += dir * stepLen;
        stepLen *= 1.18;
    }
    return 0;
}

// --- Shading ---------------------------------------------------------------------------------------------------------

float4 WaterPS(VSOut i) : SV_Target0
{
    const float3 toCamera = g_Water.cameraPosition - i.worldPos;
    // Seen from above or from below the surface (the generated meshes don't keep one winding).
    const bool front = toCamera.y > 0.0;
    const float distance = length(toCamera);
    const float3 v = toCamera / max(distance, 1e-4);
    const float t = g_Water.time;

    // Slopes and foam from every cascade; the finest fade out with distance (filtered by the mips anyway).
    float2 slope = 0;
    float whitecaps = 0;
    float crestHeight = 0;
    [unroll] for (uint c = 0; c < 4; ++c) {
        if (c >= g_Water.cascadeCount)
            continue;
        const float2 uv = i.baseXZ / g_Water.cascadeSizes[c];
        const float weight = saturate(1.0 - distance / (g_Water.cascadeSizes[c] * 12.0)) * 0.85 + 0.15 * (c == 0 ? 1.0 : 0.0);
        [branch] if (weight <= 0.0)
            continue; // too small to see from here
        // Trilinear (anisotropic filtering of six fetches costs too much on integrated GPUs; the mips keep it calm).
        const float4 der = GetTexture2D(g_Water.derivatives[c]).Sample(g_LinearWrap, uv);
        const float4 disp = GetTexture2D(g_Water.displacement[c]).Sample(g_LinearWrap, uv);
        slope += float2(der.x / (1.0 + der.z), der.y / (1.0 + der.w)) * weight;
        whitecaps += c < 2 ? disp.w * weight : 0.0; // ripples don't break
        crestHeight += disp.y;
    }
    slope *= g_Water.amplitude * i.fade;
    crestHeight *= g_Water.amplitude * i.fade;
    float3 n = normalize(float3(-slope.x, 1.0, -slope.y));
    n = normalize(lerp(n, float3(0, 1, 0), saturate(distance / 1500.0) * 0.6)); // calmer towards the horizon
    if (!front)
        n = -n;
    const float nDotV = saturate(dot(n, v));

    // Scene behind: refraction and the thickness of water along the view ray.
    const float2 uv = i.position.xy * g_Water.invRenderSize;
    Texture2D<float4> sceneDepth = GetTexture2D(g_Water.sceneDepth);
    Texture2D<float4> sceneColor = GetTexture2D(g_Water.sceneColor);
    const float waterZ = LinearDepth(i.position.z);
    const float sceneZ = LinearDepth(sceneDepth.SampleLevel(g_PointClamp, uv, 0).r);
    float thickness = max(sceneZ - waterZ, 0.0);
    float2 refractUv = uv + n.xz * g_Water.refraction * saturate(thickness * 0.5) / max(waterZ * 0.05, 1.0);
    float refractedRaw = sceneDepth.SampleLevel(g_PointClamp, refractUv, 0).r;
    if (LinearDepth(refractedRaw) < waterZ) { // the distorted sample is in front of the water: don't refract
        refractUv = uv;
        refractedRaw = sceneDepth.SampleLevel(g_PointClamp, uv, 0).r;
    }
    thickness = max(LinearDepth(refractedRaw) - waterZ, 0.0);
    float3 behind = sceneColor.SampleLevel(g_LinearClamp, refractUv, 0).rgb;

    // What lies below, in the world: caustics dance on it, shadowed by the depth of water above.
    const float4 bed = mul(g_Water.invViewProj, float4(refractUv.x * 2.0 - 1.0, 1.0 - refractUv.y * 2.0, refractedRaw, 1.0));
    const float3 bedPos = bed.xyz / max(bed.w, 1e-6);
    const float bedDepth = max(i.worldPos.y - bedPos.y, 0.0);
    const float sunUp = saturate(g_Water.dirToSun.y);
    const float shadow = SunShadow(i.worldPos, waterZ);
    const float causticFade = saturate(bedDepth * 2.0) * exp(-bedDepth / max(g_Water.clarity * 1.5, 0.1)) *
                              exp(-thickness / max(g_Water.clarity, 0.05)) * g_Water.caustics * sunUp * shadow;
    [branch] if (causticFade > 0.01 && refractedRaw > 0.0) { // only where the bottom shows
        const float2 cp = bedPos.xz + g_Water.dirToSun.xz / max(g_Water.dirToSun.y, 0.2) * bedDepth; // projected along the sun
        behind *= 1.0 + Caustics(cp, t) * causticFade;
    }

    // Absorption (red first) and the water's own color scattered back.
    const float depthFactor = thickness / max(g_Water.clarity, 0.05);
    const float3 transmittance = exp(-depthFactor * float3(2.4, 0.8, 0.45));
    const float3 bodyColor = lerp(g_Water.deepColor, g_Water.shallowColor, exp(-depthFactor * 0.7));
    const float3 bodyLight = g_Water.ambient + g_Water.sunRadiance * sunUp * shadow / ATM_PI;
    float3 under = behind * transmittance + bodyColor * bodyLight * (1.0 - transmittance);

    // Light through the crests (subsurface scattering, Atlas / Sea of Thieves style): looking towards the sun
    // through a raised, thin wave.
    const float3 l = g_Water.dirToSun;
    const float towardsSun = pow(saturate(dot(-v, l) * 0.5 + 0.5), 4.0);
    const float thin = pow(saturate(0.5 - 0.5 * dot(l, n)), 3.0);
    const float sss = (saturate(crestHeight * 0.6) * towardsSun * thin * 2.2 + pow(nDotV, 2.0) * 0.12) * (0.3 + 0.7 * shadow);
    under += g_Water.scatterColor * (g_Water.sunRadiance / ATM_PI * sss + g_Water.ambient * 0.08 * saturate(crestHeight));

    // Reflection: the scene where the screen has it, the prefiltered sky elsewhere.
    const float fresnel = 0.02 + 0.98 * pow(1.0 - nDotV, 5.0);
    float3 r = reflect(-v, n);
    r.y = abs(r.y);
    const float roughness = saturate(g_Water.roughness + saturate(distance / 800.0) * 0.15);
    float3 reflected = SkyRadiance(r, roughness);
    [branch] if (g_Water.ssr && front && fresnel > 0.04) { // looking down the reflection hardly shows
        const float4 hit = TraceReflection(i.worldPos + n * 0.05, r, distance);
        reflected = lerp(reflected, hit.rgb, hit.a);
    }
    // The sun (GGX), shadowed.
    const float3 h = normalize(v + l);
    const float a2 = roughness * roughness * roughness * roughness + 1e-5;
    const float nDotH = saturate(dot(n, h));
    const float dTerm = a2 / (ATM_PI * pow(nDotH * nDotH * (a2 - 1.0) + 1.0, 2.0));
    const float nDotL = saturate(dot(n, l));
    const float3 glint = g_Water.sunRadiance * min(dTerm * fresnel * 0.25 / max(nDotV, 0.05), 80.0) * nDotL * shadow;
    float3 color = lerp(under, reflected, fresnel) + glint;

    // Foam: whitecaps on breaking crests (from the simulation) and along shores / around objects.
    const float2 p = i.baseXZ;
    const float shore = (1.0 - saturate(thickness / max(g_Water.shoreFoam, 0.01))) * saturate(1.0 - distance / 300.0);
    const float bands = 0.6 + 0.4 * sin(thickness * 14.0 - t * 2.2 + Noise(p * 0.8) * 4.0);
    const float caps = saturate(whitecaps * 1.5) * g_Water.foam;
    float foamAmount = 0.0;
    [branch] if (caps > 0.01 || shore > 0.01) {
        const float pattern = FoamPattern(p * 0.9, t);
        foamAmount = saturate(max(caps * (0.35 + pattern), shore * bands * (0.45 + pattern)) * 1.4) * saturate(g_Water.foam + shore);
    }
    const float3 foamLight = 0.9 * (g_Water.ambient * 1.1 + g_Water.sunRadiance * saturate(dot(n, l) * 0.6 + 0.4) * shadow / ATM_PI);
    color = lerp(color, foamLight, foamAmount);
    return float4(min(color, 60000.0), 1.0);
}
