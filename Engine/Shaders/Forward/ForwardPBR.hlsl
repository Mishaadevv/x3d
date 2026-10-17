// Forward PBR for static meshes (M4): one directional light + hemispherical ambient, metallic-roughness materials
// from the GPU material table, normal/occlusion/emissive maps. Output: linear HDR.
// Variants: ALPHA_TEST (masked materials), DEPTH_ONLY (depth pre-pass; with ALPHA_TEST it still samples alpha).
// ZX_GRAPH_MATERIAL: the source is prefixed with generated material graph code (EvaluateGraphMaterial) that replaces
// the fixed metallic-roughness sampling; its parameters live in the material parameter buffer.

#include "Common/BRDF.hlsli"
#include "Common/Bindless.hlsli"
#include "Common/Lights.hlsli"
#include "Common/Mesh.hlsli"

struct ViewData
{
    float4x4 viewProj;
    float3 cameraPosition;
    float time;
    float3 lightDirection; // direction the light travels
    float lightIntensity;
    float3 lightColor;
    uint instanceBuffer;
    float3 skyColor;
    uint materialBuffer;
    float3 groundColor;
    float ambientIntensity;
    uint debugMode; // 0 lit, 1 base color, 2 normal, 3 roughness/metallic, 4 LOD
    uint materialSampler; // bindless sampler (anisotropy level chosen by the engine profile)
    uint materialParams;  // material graph parameter buffer (ByteAddressBuffer)
    uint pad;
    // Cascaded sun shadows (SceneRenderer::SetupCascades).
    float4x4 shadowMatrices[4]; // world -> shadow uv + depth
    float4 cascadeSplits;       // view-space far distance per cascade
    float4 cascadeTexelWorld;   // world size of a shadow texel per cascade
    uint4 shadowMaps;           // bindless SRV per cascade
    float3 cameraForward;
    float shadowTexel;          // 1 / resolution
    float normalBias;           // in texels
    uint cascadeCount;          // 0 = shadows off
    uint shadowFilter;          // 0 hard, 1 four taps, 2 nine taps
    float cascadeBlend;
    // Clustered point/spot lights (pre-exposed).
    uint lightBuffer;
    uint clusterBuffer;
    uint lightCount;
    float clusterDepthScale;
    float clusterDepthBias;
    float2 clusterTileScale;
    float exposure;
    float3 skyIrradianceScale;
    uint skyIrradianceSH; // != invalid: g_SkySH is bound; otherwise hemispherical ambient
    uint aoTexture;       // GTAO (R8), or invalid
    float2 invRenderSize;
    uint pad3;
    uint localShadowAtlas; // point/spot shadow atlas, or invalid
    uint localShadowFaces; // per face: 4 matrix rows + uv rect (80 bytes)
    float localAtlasTexel;
    float localNormalBias;
    // Reflections (keep in sync with SceneRenderer::ViewConstants).
    uint skyCube;            // prefiltered sky cube (TextureCube SRV) or invalid: SH sky along the reflection
    float skyCubeMips;       // mip count - 1 (roughness 1)
    uint probeBuffer;        // GpuProbe x probeCount (sorted: applied first wins)
    uint probeCount;
    float reflectionExposure; // current exposure / the cubes' reference exposure
    uint ssrTexture;         // last frame's screen-space reflections (rgb, a = confidence) or invalid
    float ssrMaxRoughness;
    float reflectionPad;
    float4x4 ssrViewProj;    // view-projection the SSR texture was traced with
    uint decalBuffer;        // GpuDecal x decalCount (sorted, later ones on top)
    uint decalCount;
    float2 decalPad;
    uint cloudShadow;        // fraction of sunlight the clouds let through (top-down map) or invalid
    float cloudShadowInvExtent;
    float2 cloudShadowOrigin;
    // GPU-driven draws (GpuCull.hlsl): per-slot data and the visible instance lists of every view region.
    uint drawSlots;
    uint visibleLists;
    uint slotCount;
    uint listCapacity;
};
ZX_VIEW_CONSTANTS(ViewData, g_View);

// Sky irradiance SH9 written by the sky compute pass, bound as a root constant buffer (uniform, cheap to read).
struct SkySHData
{
    float4 coefficients[9];
};
ZX_PASS_CONSTANTS(SkySHData, g_SkySH);

struct DrawData
{
    uint instanceIndex;
    uint materialIndex;
    uint vertexBuffer;
    uint lod;
    uint drawId; // != invalid: an indirect draw (rhi::IndirectDrawIdConstant) - the fields above come from its slot
};
ZX_PUSH_CONSTANTS(DrawData, g_Draw);

#define MATERIAL_STRIDE 96
#define MATERIAL_UNLIT 1u
#define MATERIAL_ALPHA_TEST 2u
#define MATERIAL_DOUBLE_SIDED 4u
#define MATERIAL_GRAPH 8u
#define INSTANCE_STRIDE 192 // world, normal + burn rows, burn sphere, previous world (Velocity.hlsl)

struct Material
{
    float4 baseColor;
    float3 emissive;
    float metallic;
    float roughness;
    float normalScale;
    float occlusionStrength;
    float alphaCutoff;
    uint baseColorTexture;
    uint metallicRoughnessTexture;
    uint normalTexture;
    uint occlusionTexture;
    uint emissiveTexture;
    uint flags;
    float uvRotation;
    uint paramOffset; // material graphs: byte offset of the parameter block
    float2 uvScale;
    float2 uvOffset;
};

Material LoadMaterial(uint index)
{
    ByteAddressBuffer b = GetBuffer(g_View.materialBuffer);
    const uint o = index * MATERIAL_STRIDE;
    const uint4 r0 = b.Load4(o), r1 = b.Load4(o + 16), r2 = b.Load4(o + 32), r3 = b.Load4(o + 48), r4 = b.Load4(o + 64),
                r5 = b.Load4(o + 80);
    Material m;
    m.baseColor = asfloat(r0);
    m.emissive = asfloat(r1.xyz);
    m.metallic = asfloat(r1.w);
    m.roughness = asfloat(r2.x);
    m.normalScale = asfloat(r2.y);
    m.occlusionStrength = asfloat(r2.z);
    m.alphaCutoff = asfloat(r2.w);
    m.baseColorTexture = r3.x;
    m.metallicRoughnessTexture = r3.y;
    m.normalTexture = r3.z;
    m.occlusionTexture = r3.w;
    m.emissiveTexture = r4.x;
    m.flags = r4.y;
    m.uvRotation = asfloat(r4.z);
    m.paramOffset = r4.w;
    m.uvScale = asfloat(r5.xy);
    m.uvOffset = asfloat(r5.zw);
    return m;
}

struct Instance
{
    float4x4 world;
    float3x3 normalMatrix;
};

Instance LoadInstance(uint index)
{
    ByteAddressBuffer b = GetBuffer(g_View.instanceBuffer);
    const uint o = index * INSTANCE_STRIDE;
    Instance inst;
    // Stored as rows (C++ writes the transposed column-major matrix).
    inst.world = float4x4(asfloat(b.Load4(o)), asfloat(b.Load4(o + 16)), asfloat(b.Load4(o + 32)), asfloat(b.Load4(o + 48)));
    inst.normalMatrix = float3x3(asfloat(b.Load3(o + 64)), asfloat(b.Load3(o + 80)), asfloat(b.Load3(o + 96)));
    return inst;
}

struct VSOutput
{
    float4 position : SV_Position;
#if !defined(DEPTH_ONLY) || defined(ALPHA_TEST)
    float2 uv : TEXCOORD0;
    nointerpolation uint3 drawInfo : DRAWINFO; // instance, material, lod
#endif
#ifndef DEPTH_ONLY
    float3 worldPos : TEXCOORD1;
    float3 normal : NORMAL;
    float4 tangent : TANGENT;
    float4 color : COLOR;
#endif
};

VSOutput VSMain(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID)
{
    uint instanceIndex = g_Draw.instanceIndex, material = g_Draw.materialIndex, vertexBuffer = g_Draw.vertexBuffer, lod = g_Draw.lod;
    if (g_Draw.drawId != ZX_INVALID_INDEX) {
        // Indirect: the draw's slot (one LOD of a mesh section + material) and this instance's entry in its list.
        const uint slot = g_Draw.drawId % g_View.slotCount, view = g_Draw.drawId / g_View.slotCount;
        const uint4 s = GetBuffer(g_View.drawSlots).Load4(slot * 32); // listOffset, material, vertexBuffer, lod
        instanceIndex = GetBuffer(g_View.visibleLists).Load((view * g_View.listCapacity + s.x + instanceId) * 4);
        material = s.y;
        vertexBuffer = s.z;
        lod = s.w;
    }
    const MeshVertex v = LoadMeshVertex(GetBuffer(vertexBuffer), vertexId);
    const Instance inst = LoadInstance(instanceIndex);
    const float4 world = mul(inst.world, float4(v.position, 1.0));
    VSOutput o;
    o.position = mul(g_View.viewProj, world);
#if !defined(DEPTH_ONLY) || defined(ALPHA_TEST)
    o.uv = v.uv0;
    o.drawInfo = uint3(instanceIndex, material, lod);
#endif
#ifndef DEPTH_ONLY
    o.worldPos = world.xyz;
    o.normal = mul(inst.normalMatrix, v.normal);
    o.tangent = float4(mul((float3x3)inst.world, v.tangent.xyz), v.tangent.w);
    o.color = v.color;
#endif
    return o;
}

float2 TransformUV(Material m, float2 uv)
{
    float s, c;
    sincos(m.uvRotation, s, c);
    uv *= m.uvScale;
    return float2(c * uv.x + s * uv.y, -s * uv.x + c * uv.y) + m.uvOffset;
}

float4 SampleOr(uint texture, float2 uv, float4 fallback)
{
    return texture != ZX_INVALID_INDEX ? GetTexture2D(texture).Sample(GetSampler(g_View.materialSampler), uv) : fallback;
}

/// Shadow atlas tile clear: a fullscreen triangle at depth 0 (= far in reversed-Z), drawn into the tile viewport.
float4 ClearTileVS(uint vertexId : SV_VertexID) : SV_Position
{
    const float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0.0, 1.0);
}

#ifdef ZX_GRAPH_MATERIAL
MaterialInputs GraphInputs(Material m, float2 uv)
{
    MaterialInputs mi = (MaterialInputs)0;
    mi.uv0 = uv;
    mi.uv1 = uv; // second UV set is not interpolated yet
    mi.vertexColor = 1.0;
    mi.normal = float3(0.0, 1.0, 0.0);
    mi.viewDir = float3(0.0, 1.0, 0.0);
    mi.time = g_View.time;
    mi.paramBuffer = g_View.materialParams;
    mi.paramOffset = m.paramOffset;
    mi.sampler = g_View.materialSampler;
    return mi;
}
#endif

#ifdef DEPTH_ONLY

void PSDepth(VSOutput i)
{
#ifdef ALPHA_TEST
    const Material m = LoadMaterial(i.drawInfo.y);
#ifdef ZX_GRAPH_MATERIAL
    clip(EvaluateGraphMaterial(GraphInputs(m, i.uv)).opacityMask - m.alphaCutoff);
#else
    const float alpha = m.baseColor.a * SampleOr(m.baseColorTexture, TransformUV(m, i.uv), 1.0).a;
    clip(alpha - m.alphaCutoff);
#endif
#endif
}

#else

float SampleShadowTap(Texture2D<float4> map, float2 uv, float depth)
{
    return map.SampleCmpLevelZero(g_ShadowSampler, uv, depth);
}

/// PCF in one cascade. Each hardware compare tap already filters 2x2 texels.
float SampleCascade(uint cascade, float3 worldPos, float3 N)
{
    const float3 offsetPos = worldPos + N * (g_View.normalBias * g_View.cascadeTexelWorld[cascade]);
    const float4 p = mul(g_View.shadowMatrices[cascade], float4(offsetPos, 1.0));
    const float3 uvz = p.xyz / p.w;
    Texture2D<float4> map = GetTexture2D(g_View.shadowMaps[cascade]);
    const float2 t = g_View.shadowTexel;
    if (g_View.shadowFilter == 0)
        return SampleShadowTap(map, uvz.xy, uvz.z);
    if (g_View.shadowFilter == 1) {
        return 0.25 * (SampleShadowTap(map, uvz.xy + float2(-0.5, -0.5) * t, uvz.z) +
                       SampleShadowTap(map, uvz.xy + float2(0.5, -0.5) * t, uvz.z) +
                       SampleShadowTap(map, uvz.xy + float2(-0.5, 0.5) * t, uvz.z) +
                       SampleShadowTap(map, uvz.xy + float2(0.5, 0.5) * t, uvz.z));
    }
    float sum = 0.0;
    [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x)
            sum += SampleShadowTap(map, uvz.xy + float2(x, y) * t, uvz.z);
    return sum / 9.0;
}

/// Sun visibility (1 = lit). Picks the cascade by view depth and cross-fades near the cascade's far end.
float SunShadow(float3 worldPos, float3 N, out uint cascade)
{
    cascade = 4;
    if (g_View.cascadeCount == 0)
        return 1.0;
    const float viewDepth = dot(worldPos - g_View.cameraPosition, g_View.cameraForward);
    [loop] for (uint c = 0; c < g_View.cascadeCount; ++c)
        if (viewDepth < g_View.cascadeSplits[c]) {
            cascade = c;
            break;
        }
    if (cascade >= g_View.cascadeCount)
        return 1.0;
    float shadow = SampleCascade(cascade, worldPos, N);
    const float start = cascade == 0 ? 0.0 : g_View.cascadeSplits[cascade - 1];
    const float fade = (g_View.cascadeSplits[cascade] - viewDepth) / max((g_View.cascadeSplits[cascade] - start) * g_View.cascadeBlend, 1e-4);
    if (fade < 1.0) {
        // Blend into the next cascade, or fade out shadows at the end of the shadow distance.
        const float next = cascade + 1 < g_View.cascadeCount ? SampleCascade(cascade + 1, worldPos, N) : 1.0;
        shadow = lerp(next, shadow, saturate(fade));
    }
    return shadow;
}

/// Point/spot light visibility from the local shadow atlas (point lights pick the cube face by the major axis).
float LocalShadow(PunctualLight l, float3 worldPos, float3 N)
{
    if (l.shadowFace == ZX_INVALID_INDEX || g_View.localShadowAtlas == ZX_INVALID_INDEX)
        return 1.0;
    const float3 fromLight = worldPos - l.position;
    uint face = l.shadowFace;
    if (l.type == ZX_LIGHT_POINT) {
        const float3 a = abs(fromLight);
        face += a.x >= a.y && a.x >= a.z ? (fromLight.x > 0 ? 0 : 1) : a.y >= a.z ? (fromLight.y > 0 ? 2 : 3) : (fromLight.z > 0 ? 4 : 5);
    }
    ByteAddressBuffer faces = GetBuffer(g_View.localShadowFaces);
    const uint o = face * 80;
    const float4x4 m = float4x4(asfloat(faces.Load4(o)), asfloat(faces.Load4(o + 16)), asfloat(faces.Load4(o + 32)), asfloat(faces.Load4(o + 48)));
    const float4 rect = asfloat(faces.Load4(o + 64));
    const float3 p = worldPos + N * (g_View.localNormalBias * l.shadowTexel * length(fromLight));
    const float4 clip = mul(m, float4(p, 1.0));
    const float3 ndc = clip.xyz / clip.w;
    const float2 uv = (ndc.xy * float2(0.5, -0.5) + 0.5) * rect.xy + rect.zw;
    Texture2D<float4> atlas = GetTexture2D(g_View.localShadowAtlas);
    const float t = g_View.localAtlasTexel;
    return 0.25 * (atlas.SampleCmpLevelZero(g_ShadowSampler, uv + float2(-0.5, -0.5) * t, ndc.z) +
                   atlas.SampleCmpLevelZero(g_ShadowSampler, uv + float2(0.5, -0.5) * t, ndc.z) +
                   atlas.SampleCmpLevelZero(g_ShadowSampler, uv + float2(-0.5, 0.5) * t, ndc.z) +
                   atlas.SampleCmpLevelZero(g_ShadowSampler, uv + float2(0.5, 0.5) * t, ndc.z));
}

float3 EvaluateClusteredLights(SurfaceData s, float3 worldPos, float3 V, float4 svPosition, float viewDepth, out uint lightCount)
{
    lightCount = 0;
    if (g_View.lightCount == 0)
        return 0.0;
    const uint slice = (uint)clamp(log(max(viewDepth, 1e-3)) * g_View.clusterDepthScale + g_View.clusterDepthBias, 0.0, ZX_CLUSTER_GRID_Z - 1.0);
    const uint2 tile = min(uint2(svPosition.xy * g_View.clusterTileScale), uint2(ZX_CLUSTER_GRID_X - 1, ZX_CLUSTER_GRID_Y - 1));
    ByteAddressBuffer clusters = GetBuffer(g_View.clusterBuffer);
    ByteAddressBuffer lights = GetBuffer(g_View.lightBuffer);
    const uint base = ClusterIndex(uint3(tile, slice)) * (ZX_MAX_LIGHTS_PER_CLUSTER + 1) * 4;
    lightCount = clusters.Load(base);
    float3 color = 0.0;
    for (uint i = 0; i < lightCount; ++i) {
        const PunctualLight l = LoadLight(lights, clusters.Load(base + 4 + i * 4));
        const float3 toLight = l.position - worldPos;
        const float distSq = dot(toLight, toLight);
        if (distSq >= l.range * l.range)
            continue;
        const float3 L = toLight * rsqrt(distSq);
        float attenuation = DistanceAttenuation(distSq, l.range) * SpotAttenuation(l, L);
        if (attenuation > 0.0)
            attenuation *= LocalShadow(l, worldPos, s.normal);
        color += EvaluateDirectLight(s, V, L, l.color * attenuation);
    }
    return color;
}

float3 SkyIrradiance(float3 n); // below

// Reflection probe (64 bytes, ReflectionRenderer::GpuProbe).
struct GpuProbe
{
    float3 boxMin;
    uint cube;
    float3 boxMax;
    float mips;
    float3 position; // capture point
    float blend;     // meters inside the box over which the probe fades in
    float intensity;
    uint boxProjection;
    float2 pad;
};

GpuProbe LoadProbe(uint index)
{
    ByteAddressBuffer b = GetBuffer(g_View.probeBuffer);
    const uint o = index * 64;
    GpuProbe p;
    p.boxMin = asfloat(b.Load3(o));
    p.cube = b.Load(o + 12);
    p.boxMax = asfloat(b.Load3(o + 16));
    p.mips = asfloat(b.Load(o + 28));
    p.position = asfloat(b.Load3(o + 32));
    p.blend = asfloat(b.Load(o + 44));
    p.intensity = asfloat(b.Load(o + 48));
    p.boxProjection = b.Load(o + 52);
    p.pad = 0;
    return p;
}

/// Specular environment light along R (pre-exposed): reflection probes around the point (box-projected, blended
/// by their fade margin, in priority order), the rest from the sky cube; screen-space reflections from the last
/// frame on top where they found a hit.
float3 SpecularEnvironment(float3 worldPos, float3 R, float roughness)
{
    float3 acc = 0;
    float weight = 0;
    for (uint k = 0; k < min(g_View.probeCount, 32u) && weight < 0.999; ++k) {
        const GpuProbe p = LoadProbe(k);
        const float3 inside = min(worldPos - p.boxMin, p.boxMax - worldPos);
        const float edge = min(inside.x, min(inside.y, inside.z));
        if (edge <= 0.0)
            continue;
        const float w = saturate(edge / max(p.blend, 1e-3)) * (1.0 - weight);
        float3 dir = R;
        if (p.boxProjection) {
            // Parallax correction: intersect the reflection ray with the box, look up from the capture point.
            const float3 t1 = (p.boxMax - worldPos) / R, t2 = (p.boxMin - worldPos) / R;
            const float3 tf = max(t1, t2);
            const float t = min(tf.x, min(tf.y, tf.z));
            dir = worldPos + R * t - p.position;
        }
        acc += GetTextureCube(p.cube).SampleLevel(g_LinearClamp, dir, roughness * p.mips).rgb * (p.intensity * g_View.reflectionExposure) * w;
        weight += w;
    }
    if (weight < 0.999) {
        float3 sky;
        if (g_View.skyCube != ZX_INVALID_INDEX)
            sky = GetTextureCube(g_View.skyCube).SampleLevel(g_LinearClamp, R, roughness * g_View.skyCubeMips).rgb * g_View.reflectionExposure;
        else
            sky = SkyIrradiance(R); // no sky cube: the SH sky (widened towards the normal by the caller)
        acc += sky * (1.0 - weight);
    }
    if (g_View.ssrTexture != ZX_INVALID_INDEX && roughness < g_View.ssrMaxRoughness) {
        const float4 prev = mul(g_View.ssrViewProj, float4(worldPos, 1.0));
        const float2 uv = float2(prev.x / prev.w * 0.5 + 0.5, 0.5 - prev.y / prev.w * 0.5);
        if (prev.w > 0.0 && all(uv > 0.0) && all(uv < 1.0)) {
            const float4 ssr = GetTexture2D(g_View.ssrTexture).SampleLevel(g_LinearClamp, uv, 0);
            acc = lerp(acc, ssr.rgb, saturate(ssr.a));
        }
    }
    return acc;
}

float BurnHash(float3 p)
{
    p = frac(p * 0.3183099 + 0.1) * 17.0;
    return frac(p.x * p.y * p.z * (p.x + p.y + p.z));
}

float BurnNoise(float3 p)
{
    const float3 i = floor(p), f = frac(p), u = f * f * (3.0 - 2.0 * f);
    return lerp(lerp(lerp(BurnHash(i), BurnHash(i + float3(1, 0, 0)), u.x), lerp(BurnHash(i + float3(0, 1, 0)), BurnHash(i + float3(1, 1, 0)), u.x), u.y),
                lerp(lerp(BurnHash(i + float3(0, 0, 1)), BurnHash(i + float3(1, 0, 1)), u.x), lerp(BurnHash(i + float3(0, 1, 1)), BurnHash(i + float3(1, 1, 1)), u.x), u.y),
                u.z);
}

/// Fire (Flammable objects): charred inside the burn sphere with a ragged edge and soot ahead of it, the edge glowing
/// where the flames are, embers glowing through cracks in the char. Instance row 7 = burn sphere, normal rows' w =
/// charring, glow, embers.
void ApplyBurn(inout SurfaceData s, float3 worldPos, uint instanceIndex)
{
    ByteAddressBuffer b = GetBuffer(g_View.instanceBuffer);
    const uint o = instanceIndex * INSTANCE_STRIDE;
    const float4 burn = asfloat(b.Load4(o + 112));
    if (burn.w <= 0.0)
        return;
    const float charring = asfloat(b.Load(o + 76));
    const float glow = asfloat(b.Load(o + 92));
    const float embers = asfloat(b.Load(o + 108));
    const float n = BurnNoise(worldPos * 3.1) * 0.65 + BurnNoise(worldPos * 9.7) * 0.35;
    const float d = distance(worldPos, burn.xyz) + (n - 0.5) * min(0.35, burn.w * 0.6);
    const float edge = 0.05 + min(burn.w, 2.0) * 0.03;
    const float charred = saturate((burn.w - d) / edge);
    const float soot = saturate((burn.w + edge * 3.0 - d) / (edge * 3.0)) * 0.45;
    const float dark = max(charred, soot) * charring;
    s.baseColor = lerp(s.baseColor, s.baseColor * 0.04 + 0.012, dark);
    s.roughness = lerp(s.roughness, 0.92, dark);
    s.metallic *= 1.0 - dark;
    const float flicker = 0.7 + 0.3 * BurnNoise(worldPos * 2.3 + g_View.time * float3(0.0, 1.9, 0.7));
    const float x = (d - burn.w) / edge;
    const float rim = exp(-x * x) * glow;
    const float crack = saturate(1.0 - abs(BurnNoise(worldPos * 6.1) * 0.7 + BurnNoise(worldPos * 17.3) * 0.3 - 0.5) * 14.0);
    const float cracks = crack * crack * charred * embers;
    s.emissive += float3(1.0, 0.3, 0.05) * ((rim * 1.2 + cracks * 0.7) * flicker * 3000.0 * g_View.exposure);
}

// Decal (144 bytes, SceneRenderer GpuDecal): projected along its local -Y through a -1..1 box.
#define DECAL_STRIDE 144

/// Projects the decals touching worldPos onto the surface before lighting: base color (+ coverage in alpha), normal
/// map, roughness, emissive. Fades where the surface turns away from the projection direction.
void ApplyDecals(inout SurfaceData s, inout float3 N, float3 worldPos, float3 geometricNormal)
{
    if (g_View.decalCount == 0)
        return;
    // Screen-space derivatives outside the loop (sampling inside divergent control flow needs explicit gradients).
    const float3 dPdx = ddx(worldPos), dPdy = ddy(worldPos);
    ByteAddressBuffer b = GetBuffer(g_View.decalBuffer);
    for (uint k = 0; k < min(g_View.decalCount, 256u); ++k) {
        const uint o = k * DECAL_STRIDE;
        float4x4 m;
        m[0] = asfloat(b.Load4(o));
        m[1] = asfloat(b.Load4(o + 16));
        m[2] = asfloat(b.Load4(o + 32));
        m[3] = asfloat(b.Load4(o + 48));
        const float3 p = mul(m, float4(worldPos, 1.0)).xyz; // rows: world -> decal box
        if (any(abs(p) > 1.0))
            continue;
        const float3 axisY = asfloat(b.Load3(o + 64)); // decal up (projects along -Y)
        const uint albedo = b.Load(o + 76);
        const float3 axisX = asfloat(b.Load3(o + 80));
        const uint normalMap = b.Load(o + 92);
        const float4 colorOpacity = asfloat(b.Load4(o + 96));
        const float4 params = asfloat(b.Load4(o + 112)); // roughness, affectRoughness, normalStrength, angleFade
        const float emissive = asfloat(b.Load(o + 128));
        // Angle fade: full on surfaces facing the projector, gone at 90 degrees.
        const float facing = dot(geometricNormal, axisY);
        const float fade = saturate((facing - (1.0 - params.w) * 0.5) / max(params.w * 0.5 + 0.05, 1e-3));
        // Edge fade along the projection depth avoids hard cuts on thick geometry.
        const float depthFade = saturate((1.0 - abs(p.y)) * 8.0);
        const float2 uv = float2(p.x * 0.5 + 0.5, 0.5 - p.z * 0.5);
        const float3x3 m3 = (float3x3)m;
        const float2 gx = float2(mul(m3, dPdx).x * 0.5, -mul(m3, dPdx).z * 0.5);
        const float2 gy = float2(mul(m3, dPdy).x * 0.5, -mul(m3, dPdy).z * 0.5);
        float4 tex = float4(1, 1, 1, 1);
        if (albedo != ZX_INVALID_INDEX)
            tex = GetTexture2D(albedo).SampleGrad(g_LinearClamp, uv, gx, gy);
        const float a = tex.a * colorOpacity.a * fade * depthFade;
        if (a <= 0.001)
            continue;
        s.baseColor = lerp(s.baseColor, tex.rgb * colorOpacity.rgb, a);
        s.metallic = lerp(s.metallic, 0.0, a);
        s.emissive += tex.rgb * colorOpacity.rgb * emissive * a;
        if (params.y > 0.5)
            s.roughness = lerp(s.roughness, params.x, a);
        if (normalMap != ZX_INVALID_INDEX) {
            float2 xy = GetTexture2D(normalMap).SampleGrad(g_LinearClamp, uv, gx, gy).rg * 2.0 - 1.0;
            const float3 tn = float3(xy, sqrt(saturate(1.0 - dot(xy, xy))));
            const float3 T = normalize(axisX - N * dot(N, axisX));
            const float3 B = cross(N, T);
            const float3 dn = normalize(tn.x * T - tn.y * B + tn.z * N);
            N = normalize(lerp(N, dn, a * params.z));
        }
    }
}

float2 EncodeOct(float3 n)
{
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    float2 e = n.xy;
    if (n.z < 0.0)
        e = (1.0 - abs(n.yx)) * float2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return e * 0.5 + 0.5;
}

/// Diffuse sky light / pi in direction n (pre-exposed), from SH9 or the fallback hemisphere.
float3 SkyIrradiance(float3 n)
{
    if (g_View.skyIrradianceSH == ZX_INVALID_INDEX)
        return lerp(g_View.groundColor, g_View.skyColor, n.y * 0.5 + 0.5) * g_View.ambientIntensity;
    const float y[9] = {0.282095, 0.488603 * n.y, 0.488603 * n.z, 0.488603 * n.x, 1.092548 * n.x * n.y, 1.092548 * n.y * n.z,
                        0.315392 * (3.0 * n.z * n.z - 1.0), 1.092548 * n.x * n.z, 0.546274 * (n.x * n.x - n.y * n.y)};
    float3 e = 0;
    [unroll] for (uint k = 0; k < 9; ++k)
        e += g_SkySH.coefficients[k].rgb * y[k];
    return max(e, 0.0) * g_View.skyIrradianceScale;
}

float3 CascadeColor(uint c)
{
    const float3 colors[5] = {float3(1, 0.3, 0.3), float3(0.3, 1, 0.3), float3(0.3, 0.5, 1), float3(1, 1, 0.3), float3(1, 1, 1)};
    return colors[min(c, 4u)];
}

float3 LodColor(uint lod)
{
    const float3 colors[5] = {float3(1, 1, 1), float3(0.2, 0.9, 0.2), float3(0.2, 0.5, 1), float3(1, 0.8, 0.1), float3(1, 0.2, 0.2)};
    return colors[min(lod, 4u)];
}

// Second target (bound when screen-space reflections are on): octahedral normal, roughness, a = 1 (opaque surface).
float4 ShadePixel(VSOutput i, bool frontFace, inout float4 normalRoughness)
{
    const Material m = LoadMaterial(i.drawInfo.y);
    SurfaceData s;
    s.subsurface = 0.0;
    float3 N = normalize(i.normal);
    float3 T = i.tangent.xyz;
    if (!frontFace) {
        N = -N;
        T = -T;
    }
    const float3 geometricNormal = N;
#ifdef ZX_GRAPH_MATERIAL
    MaterialInputs mi = GraphInputs(m, i.uv);
    mi.vertexColor = i.color;
    mi.worldPos = i.worldPos;
    mi.normal = N;
    mi.tangent = float4(T, i.tangent.w);
    mi.viewDir = normalize(g_View.cameraPosition - i.worldPos);
    const MaterialOutputs mo = EvaluateGraphMaterial(mi);
    s.subsurface = max(mo.subsurface, 0.0);
    s.baseColor = max(mo.baseColor, 0.0);
    s.alpha = saturate(mo.opacity);
#ifdef ALPHA_TEST
    clip(mo.opacityMask - m.alphaCutoff);
#endif
    s.roughness = saturate(mo.roughness);
    s.metallic = saturate(mo.metallic);
    s.occlusion = saturate(mo.occlusion);
    s.emissive = max(mo.emissive, 0.0);
    if (any(mo.normal != float3(0.0, 0.0, 1.0))) {
        const float3 tn = normalize(mo.normal);
        T = normalize(T - N * dot(N, T));
        const float3 B = cross(N, T) * i.tangent.w;
        N = normalize(tn.x * T + tn.y * B + tn.z * N);
    }
#else
    const float2 uv = TransformUV(m, i.uv);
    const float4 baseSample = SampleOr(m.baseColorTexture, uv, 1.0);
    s.baseColor = m.baseColor.rgb * baseSample.rgb * i.color.rgb;
    s.alpha = m.baseColor.a * baseSample.a * i.color.a;
#ifdef ALPHA_TEST
    clip(s.alpha - m.alphaCutoff);
#endif
    const float4 mr = SampleOr(m.metallicRoughnessTexture, uv, 1.0);
    s.roughness = saturate(m.roughness * mr.g);
    s.metallic = saturate(m.metallic * mr.b);
    s.occlusion = lerp(1.0, SampleOr(m.occlusionTexture, uv, 1.0).r, m.occlusionStrength);
    s.emissive = m.emissive * SampleOr(m.emissiveTexture, uv, 1.0).rgb;
    if (m.normalTexture != ZX_INVALID_INDEX) {
        // BC5: XY in RG, Z reconstructed. OpenGL convention (green = +bitangent).
        float2 xy = GetTexture2D(m.normalTexture).Sample(GetSampler(g_View.materialSampler), uv).rg * 2.0 - 1.0;
        xy *= m.normalScale;
        const float3 tn = float3(xy, sqrt(saturate(1.0 - dot(xy, xy))));
        T = normalize(T - N * dot(N, T));
        const float3 B = cross(N, T) * i.tangent.w;
        N = normalize(tn.x * T + tn.y * B + tn.z * N);
    }
#endif
    ApplyBurn(s, i.worldPos, i.drawInfo.x);
    ApplyDecals(s, N, i.worldPos, geometricNormal);
    s.normal = N;
    normalRoughness = float4(EncodeOct(N), s.roughness, 1.0);

    uint cascade;
    float shadow = SunShadow(i.worldPos, N, cascade);
    if (g_View.cloudShadow != ZX_INVALID_INDEX) // clouds passing over the sun
        shadow *= GetTexture2D(g_View.cloudShadow).SampleLevel(g_LinearClamp, (i.worldPos.xz - g_View.cloudShadowOrigin) * g_View.cloudShadowInvExtent, 0).r;
    switch (g_View.debugMode) {
    case 1: return float4(s.baseColor, 1);
    case 2: return float4(N * 0.5 + 0.5, 1);
    case 3: return float4(s.roughness, s.metallic, 0, 1);
    case 4: return float4(LodColor(i.drawInfo.z) * (0.3 + 0.7 * saturate(dot(N, -g_View.lightDirection) * 0.5 + 0.5)), 1);
    case 5: return float4(CascadeColor(cascade) * (0.25 + 0.75 * shadow), 1);
    case 8: { // fraction of the local lights' energy that passes their shadow maps
        const float viewDepth = dot(i.worldPos - g_View.cameraPosition, g_View.cameraForward);
        const uint slice = (uint)clamp(log(max(viewDepth, 1e-3)) * g_View.clusterDepthScale + g_View.clusterDepthBias, 0.0, ZX_CLUSTER_GRID_Z - 1.0);
        const uint2 tile = min(uint2(i.position.xy * g_View.clusterTileScale), uint2(ZX_CLUSTER_GRID_X - 1, ZX_CLUSTER_GRID_Y - 1));
        const uint base = ClusterIndex(uint3(tile, slice)) * (ZX_MAX_LIGHTS_PER_CLUSTER + 1) * 4;
        ByteAddressBuffer clusters = GetBuffer(g_View.clusterBuffer);
        float lit = 0, total = 1e-6;
        const uint n = g_View.lightCount ? clusters.Load(base) : 0;
        for (uint k = 0; k < n; ++k) {
            const PunctualLight l = LoadLight(GetBuffer(g_View.lightBuffer), clusters.Load(base + 4 + k * 4));
            const float3 toL = l.position - i.worldPos;
            const float a = DistanceAttenuation(dot(toL, toL), l.range) * saturate(dot(N, normalize(toL)));
            total += a;
            lit += a * LocalShadow(l, i.worldPos, N);
        }
        return float4((lit / total).xxx, 1);
    }
    case 7: {
        const float a = g_View.aoTexture != ZX_INVALID_INDEX
                            ? GetTexture2D(g_View.aoTexture).SampleLevel(g_LinearClamp, i.position.xy * g_View.invRenderSize, 0).r
                            : 1.0;
        return float4(a.xxx, 1);
    }
    case 6: {
        uint count;
        const float viewDepth = dot(i.worldPos - g_View.cameraPosition, g_View.cameraForward);
        EvaluateClusteredLights(s, i.worldPos, normalize(g_View.cameraPosition - i.worldPos), i.position, viewDepth, count);
        const float h = saturate(count / 16.0);
        return float4(h, 1.0 - abs(h * 2.0 - 1.0), 1.0 - h, 1) * (count > 0 ? 1.0 : 0.15);
    }
    default: break;
    }

    if (m.flags & MATERIAL_UNLIT)
        return float4(s.baseColor + s.emissive, s.alpha);

    const float3 V = normalize(g_View.cameraPosition - i.worldPos);
    float3 color = EvaluateDirectLight(s, V, -g_View.lightDirection, g_View.lightColor * (g_View.lightIntensity * shadow));
    // Ambient: sky irradiance (SH9) + specular image-based lighting.
    const float3 irradiance = SkyIrradiance(N);
    const float NoV = max(dot(N, V), 1e-4);
    const float3 f0 = lerp(0.04, s.baseColor, s.metallic);
    // Specular ambient: probes / sky cube / SSR along the reflection (bent towards the normal with roughness).
    const float3 R = normalize(lerp(reflect(-V, N), N, s.roughness * s.roughness));
    const float3 specAmbient = EnvBRDFApprox(f0, s.roughness, NoV) * SpecularEnvironment(i.worldPos, R, s.roughness);
    // Ambient occlusion: material AO combined with GTAO; multi-bounce for diffuse (Jimenez), specular occlusion (Lagarde).
    float ao = s.occlusion;
    if (g_View.aoTexture != ZX_INVALID_INDEX)
        ao = min(ao, GetTexture2D(g_View.aoTexture).SampleLevel(g_LinearClamp, i.position.xy * g_View.invRenderSize, 0).r);
    const float3 a = 2.0404 * s.baseColor - 0.3324, b = -4.7951 * s.baseColor + 0.6417, c = 2.7552 * s.baseColor + 0.6903;
    const float3 diffuseAO = max(ao.xxx, ((ao * a + b) * ao + c) * ao);
    const float specAO = saturate(pow(NoV + ao, exp2(-16.0 * s.roughness - 1.0)) - 1.0 + ao);
    color += irradiance * s.baseColor * (1.0 - s.metallic) * diffuseAO + specAmbient * specAO;
    const float viewDepth = dot(i.worldPos - g_View.cameraPosition, g_View.cameraForward);
    uint clusterLights;
    color += EvaluateClusteredLights(s, i.worldPos, V, i.position, viewDepth, clusterLights);
    color += s.emissive;
    return float4(color, s.alpha);
}

// ZX_NORMAL_OUTPUT: the opaque pass with screen-space reflections also writes normal + roughness (second target).
struct PSOutput
{
    float4 color : SV_Target0;
#ifdef ZX_NORMAL_OUTPUT
    float4 normalRoughness : SV_Target1;
#endif
};

PSOutput PSMain(VSOutput i, bool frontFace : SV_IsFrontFace)
{
    PSOutput o;
    float4 normalRoughness = 0;
    o.color = ShadePixel(i, frontFace, normalRoughness);
#ifdef ZX_NORMAL_OUTPUT
    o.normalRoughness = normalRoughness;
#endif
    return o;
}

#endif
