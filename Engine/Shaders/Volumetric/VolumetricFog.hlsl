// Volumetric fog (see Render/VolumetricFog.h). Cells (froxels): x, y = screen tiles, z = slices with view depth
// d = distance * t^2, t = (z + 0.5 + jitter) / slices. Volumes are RGBA16F 3D textures:
//   scatter    rgb = light scattered towards the camera per meter (sigma_s * L + emission), a = extinction (1/m)
//   lighting   rgb = L (light per unit of scattering, phase included) - lights translucent particles
//   integrated rgb = in-scatter from the camera to the far side of the cell, a = transmittance

#include "Common/Bindless.hlsli"
#include "Common/Lights.hlsli"

struct VolumeParams
{
    float4x4 invViewProj;   // clip (no jitter) -> world
    float4x4 prevViewProj;  // previous frame world -> clip
    float4x4 shadowMatrices[4];
    float4 cascadeSplits;
    uint4 shadowMaps;
    float3 cameraPosition;
    float distance;
    float3 cameraForward;
    uint cascadeCount;
    uint3 grid;
    uint volumeCount;
    float3 sunColor;        // pre-exposed illuminance
    float anisotropy;
    float3 dirToSun;
    float jitter;           // slice offset this frame, -0.5 .. 0.5
    float3 ambient;         // pre-exposed sky luminance
    float time;
    float3 albedo;
    float fogDensity;       // height fog extinction at fogBase (already scaled)
    float fogFalloff;
    float fogBase;
    float localLights;
    float history;          // 0 = no usable history
    uint lightBuffer;
    uint clusterBuffer;
    uint lightCount;
    float clusterDepthScale;
    float clusterDepthBias;
    float2 clusterTileScale; // clusters per render pixel
    uint volumes;
    float2 renderSize;
    uint scatterOut;
    uint lightingOut;
    uint historyIn;
    uint scatterIn;
    uint integratedOut;
    uint integratedIn;
    float3 prevCameraPosition;
    uint depthTexture;
    float3 prevCameraForward;
    float nearPlane;
    float skyScale; // < 0: no sky SH bound, use ambient
    float3 pad;
};
ZX_PASS_CONSTANTS(VolumeParams, g_V);

struct SkySH { float4 c[9]; };
ConstantBuffer<SkySH> g_SkySH : register(b2, space0);

float3 EvalSH(float3 n)
{
    const float y[9] = {0.282095, 0.488603 * n.y, 0.488603 * n.z, 0.488603 * n.x, 1.092548 * n.x * n.y, 1.092548 * n.y * n.z,
                        0.315392 * (3.0 * n.z * n.z - 1.0), 1.092548 * n.x * n.z, 0.546274 * (n.x * n.x - n.y * n.y)};
    float3 e = 0;
    [unroll] for (uint k = 0; k < 9; ++k)
        e += g_SkySH.c[k].rgb * y[k];
    return max(e, 0.0);
}

#define PI 3.14159265

float HenyeyGreenstein(float cosTheta, float g)
{
    const float g2 = g * g;
    return (1.0 - g2) / (4.0 * PI * pow(max(1.0 + g2 - 2.0 * g * cosTheta, 1e-4), 1.5));
}

float SliceDepth(float t) { return g_V.distance * t * t; }

/// World direction of the view ray through screen uv (0..1, y down).
float3 RayDirection(float2 uv)
{
    const float4 p = mul(g_V.invViewProj, float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.5, 1.0));
    return normalize(p.xyz / p.w - g_V.cameraPosition);
}

float Hash31(float3 p)
{
    p = frac(p * 0.1031);
    p += dot(p, p.zyx + 31.32);
    return frac((p.x + p.y) * p.z);
}

float ValueNoise(float3 p)
{
    const float3 i = floor(p), f = frac(p), u = f * f * (3.0 - 2.0 * f);
    return lerp(lerp(lerp(Hash31(i), Hash31(i + float3(1, 0, 0)), u.x), lerp(Hash31(i + float3(0, 1, 0)), Hash31(i + float3(1, 1, 0)), u.x), u.y),
                lerp(lerp(Hash31(i + float3(0, 0, 1)), Hash31(i + float3(1, 0, 1)), u.x), lerp(Hash31(i + float3(0, 1, 1)), Hash31(i + float3(1, 1, 1)), u.x), u.y),
                u.z);
}

float Fbm(float3 p)
{
    return ValueNoise(p) * 0.55 + ValueNoise(p * 2.03 + 17.1) * 0.3 + ValueNoise(p * 4.1 - 9.7) * 0.15;
}

float SunShadow(float3 worldPos, float viewDepth)
{
    for (uint c = 0; c < g_V.cascadeCount; ++c) {
        if (viewDepth > g_V.cascadeSplits[c])
            continue;
        const float4 s = mul(g_V.shadowMatrices[c], float4(worldPos, 1.0));
        const float3 uvz = s.xyz / s.w;
        if (any(uvz.xy < 0.0) || any(uvz.xy > 1.0))
            continue;
        return GetTexture2D(g_V.shadowMaps[c]).SampleCmpLevelZero(g_ShadowSampler, uvz.xy, uvz.z);
    }
    return 1.0;
}

// Fog volume (112 bytes): world -> volume rows (3 x float4), albedo + density, emissive + shape, noise + edge, wind.
void AddVolumes(float3 worldPos, inout float extinction, inout float3 albedoSum, inout float3 emission)
{
    ByteAddressBuffer b = GetBuffer(g_V.volumes);
    for (uint k = 0; k < g_V.volumeCount; ++k) {
        const uint o = k * 112;
        const float4 r0 = asfloat(b.Load4(o)), r1 = asfloat(b.Load4(o + 16)), r2 = asfloat(b.Load4(o + 32));
        const float3 local = float3(dot(r0.xyz, worldPos) + r0.w, dot(r1.xyz, worldPos) + r1.w, dot(r2.xyz, worldPos) + r2.w);
        const float4 albedoDensity = asfloat(b.Load4(o + 48));
        const uint4 e = b.Load4(o + 64);
        const float4 noiseEdge = asfloat(b.Load4(o + 80)); // noiseScale, noiseAmount, edgeFade, -
        const float3 wind = asfloat(b.Load3(o + 96));
        const float d = e.w == 1u ? length(local) * 2.0 : max(abs(local.x), max(abs(local.y), abs(local.z))) * 2.0; // 1 at the surface
        if (d >= 1.0)
            continue;
        float density = albedoDensity.w * saturate((1.0 - d) / max(noiseEdge.z, 1e-3));
        if (noiseEdge.y > 0.0) {
            const float n = Fbm((worldPos - wind * g_V.time) * noiseEdge.x);
            density *= lerp(1.0, saturate(n * 2.2 - 0.45), noiseEdge.y);
        }
        extinction += density;
        albedoSum += albedoDensity.rgb * density;
        emission += asfloat(e.xyz) * density;
    }
}

[numthreads(4, 4, 4)]
void InjectCS(uint3 id : SV_DispatchThreadID)
{
    if (any(id >= g_V.grid))
        return;
    const float2 uv = (float2(id.xy) + 0.5) / float2(g_V.grid.xy);
    const float3 dir = RayDirection(uv);
    const float t = (float(id.z) + 0.5 + g_V.jitter) / float(g_V.grid.z);
    const float viewDepth = SliceDepth(t);
    const float3 worldPos = g_V.cameraPosition + dir * (viewDepth / max(dot(dir, g_V.cameraForward), 1e-3));

    // Density: exponential height fog + local volumes.
    float extinction = g_V.fogDensity * exp(-g_V.fogFalloff * (worldPos.y - g_V.fogBase));
    float3 albedoSum = g_V.albedo * extinction;
    float3 emission = 0.0;
    AddVolumes(worldPos, extinction, albedoSum, emission);
    const float3 albedo = extinction > 1e-6 ? albedoSum / extinction : g_V.albedo;

    // Light arriving and scattered towards the camera (phase included).
    // Sky light (as the analytic height fog: the sky SH along the view ray).
    float3 L = g_V.skyScale >= 0.0 ? EvalSH(dir) * g_V.skyScale : g_V.ambient * 0.5;
    L += g_V.sunColor * HenyeyGreenstein(dot(dir, g_V.dirToSun), g_V.anisotropy) * SunShadow(worldPos, viewDepth);
    if (g_V.lightCount > 0 && g_V.localLights > 0.0) {
        const float2 pixel = uv * g_V.renderSize;
        const uint2 tile = min(uint2(pixel * g_V.clusterTileScale), uint2(ZX_CLUSTER_GRID_X - 1, ZX_CLUSTER_GRID_Y - 1));
        const uint slice = (uint)clamp(log(max(viewDepth, 1e-3)) * g_V.clusterDepthScale + g_V.clusterDepthBias, 0.0, ZX_CLUSTER_GRID_Z - 1.0);
        const uint base = ClusterIndex(uint3(tile, slice)) * (ZX_MAX_LIGHTS_PER_CLUSTER + 1) * 4;
        ByteAddressBuffer clusters = GetBuffer(g_V.clusterBuffer);
        ByteAddressBuffer lights = GetBuffer(g_V.lightBuffer);
        const uint n = clusters.Load(base);
        for (uint k = 0; k < n; ++k) {
            const PunctualLight l = LoadLight(lights, clusters.Load(base + 4 + k * 4));
            const float3 toL = l.position - worldPos;
            const float d2 = dot(toL, toL);
            if (d2 >= l.range * l.range)
                continue;
            const float3 toLn = toL * rsqrt(max(d2, 1e-6));
            // Isotropic-leaning phase: point lights read as glowing halos from every side.
            const float phase = lerp(1.0 / (4.0 * PI), HenyeyGreenstein(dot(dir, toLn), g_V.anisotropy), 0.5);
            L += l.color * (DistanceAttenuation(d2, l.range) * SpotAttenuation(l, toLn) * phase * g_V.localLights);
        }
    }

    float4 scatter = float4(albedo * L * extinction + emission, extinction);
    // Temporal: blend with last frame's cell at the same world position.
    if (g_V.history > 0.0) {
        const float4 prevClip = mul(g_V.prevViewProj, float4(worldPos, 1.0));
        const float2 prevUv = float2(prevClip.x / prevClip.w * 0.5 + 0.5, 0.5 - prevClip.y / prevClip.w * 0.5);
        const float prevDepth = dot(worldPos - g_V.prevCameraPosition, g_V.prevCameraForward);
        const float prevT = sqrt(max(prevDepth, 0.0) / g_V.distance);
        if (prevClip.w > 0.0 && all(prevUv >= 0.0) && all(prevUv <= 1.0) && prevT < 1.0) {
            const float4 h = GetTexture3D(g_V.historyIn).SampleLevel(g_LinearClamp, float3(prevUv, prevT), 0);
            scatter = lerp(scatter, h, g_V.history);
        }
    }
    GetRWTexture3D(g_V.scatterOut)[id] = scatter;
    GetRWTexture3D(g_V.lightingOut)[id] = float4(L, 1.0);
}

[numthreads(8, 8, 1)]
void IntegrateCS(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= g_V.grid.xy))
        return;
    const float2 uv = (float2(id.xy) + 0.5) / float2(g_V.grid.xy);
    const float rayScale = 1.0 / max(dot(RayDirection(uv), g_V.cameraForward), 1e-3); // view depth -> ray length
    Texture3D<float4> scatter = GetTexture3D(g_V.scatterIn);
    RWTexture3D<float4> outVolume = GetRWTexture3D(g_V.integratedOut);
    float3 inscatter = 0.0;
    float transmittance = 1.0;
    float previous = 0.0;
    for (uint z = 0; z < g_V.grid.z; ++z) {
        const float far = SliceDepth(float(z + 1) / float(g_V.grid.z));
        const float len = (far - previous) * rayScale;
        previous = far;
        const float4 s = scatter[uint3(id.xy, z)];
        const float sigma = max(s.a, 1e-6);
        const float tSlice = exp(-sigma * len);
        // Energy-conserving integration of the scattering over the cell (Hillaire 2015).
        inscatter += transmittance * s.rgb * (1.0 - tSlice) / sigma;
        transmittance *= tSlice;
        outVolume[uint3(id.xy, z)] = float4(inscatter, transmittance);
    }
}

/// The integrated fog at a view depth: (in-scatter, transmittance) from the camera to that depth.
float4 SampleIntegrated(float2 uv, float viewDepth)
{
    const float t = sqrt(saturate(viewDepth / g_V.distance));
    const float w = t - 0.5 / float(g_V.grid.z);
    float4 v = GetTexture3D(g_V.integratedIn).SampleLevel(g_LinearClamp, float3(uv, max(w, 0.5 / float(g_V.grid.z))), 0);
    if (w < 0.5 / float(g_V.grid.z)) // inside the first cell: fade in from no fog at the camera
        v = lerp(float4(0, 0, 0, 1), v, saturate(t * float(g_V.grid.z)));
    return v;
}

struct ApplyOut
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

ApplyOut ApplyVS(uint vertexId : SV_VertexID)
{
    ApplyOut o;
    o.uv = float2((vertexId << 1) & 2, vertexId & 2);
    o.position = float4(o.uv * float2(2, -2) + float2(-1, 1), 0.0, 1.0);
    return o;
}

float4 ApplyPS(ApplyOut i) : SV_Target0
{
    const float raw = GetTexture2D(g_V.depthTexture).Load(int3(i.position.xy, 0)).r;
    const float viewDepth = raw > 0.0 ? g_V.nearPlane / raw : g_V.distance;
    const float4 v = SampleIntegrated(i.uv, viewDepth);
    // Premultiplied: scene * transmittance + in-scatter (blend: src One, dst SrcAlpha).
    return float4(min(v.rgb, 60000.0), v.a);
}
