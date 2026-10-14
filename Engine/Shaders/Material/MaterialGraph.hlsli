#ifndef ZX_MATERIAL_GRAPH_HLSLI
#define ZX_MATERIAL_GRAPH_HLSLI

// Interface between generated material graph code (Render/MaterialGraph.cpp) and the passes that evaluate it
// (Forward/ForwardPBR.hlsl with ZX_GRAPH_MATERIAL). Parameters (float4 values, texture indices) are read from the
// material's block in the parameter buffer.

#include "/Shaders/Common/Bindless.hlsli"

struct MaterialInputs
{
    float2 uv0;
    float2 uv1;
    float4 vertexColor;
    float3 worldPos;
    float3 normal;   // interpolated world-space normal (front-facing)
    float3 viewDir;  // pixel -> camera, normalized
    float time;
    uint paramBuffer;
    uint paramOffset;
    uint sampler;
};

struct MaterialOutputs
{
    float3 baseColor;
    float metallic;
    float roughness;
    float3 normal; // tangent space
    float3 emissive;
    float occlusion;
    float opacity;
    float opacityMask;
};

MaterialOutputs DefaultMaterialOutputs()
{
    MaterialOutputs o;
    o.baseColor = float3(0.8, 0.8, 0.8);
    o.metallic = 0.0;
    o.roughness = 0.5;
    o.normal = float3(0.0, 0.0, 1.0);
    o.emissive = 0.0;
    o.occlusion = 1.0;
    o.opacity = 1.0;
    o.opacityMask = 1.0;
    return o;
}

float4 ParamFloat4(MaterialInputs mi, uint offset)
{
    return asfloat(GetBuffer(mi.paramBuffer).Load4(mi.paramOffset + offset));
}

uint ParamUint(MaterialInputs mi, uint offset)
{
    return GetBuffer(mi.paramBuffer).Load(mi.paramOffset + offset);
}

float4 SampleParamTexture(MaterialInputs mi, uint offset, float2 uv)
{
    const uint texture = ParamUint(mi, offset);
    return texture != ZX_INVALID_INDEX ? GetTexture2D(texture).Sample(GetSampler(mi.sampler), uv) : float4(1.0, 1.0, 1.0, 1.0);
}

/// BC5 normal maps: XY in RG (OpenGL convention), Z reconstructed. Missing texture = flat normal.
float4 SampleParamNormalMap(MaterialInputs mi, uint offset, float2 uv)
{
    const uint texture = ParamUint(mi, offset);
    if (texture == ZX_INVALID_INDEX)
        return float4(0.0, 0.0, 1.0, 1.0);
    const float2 xy = GetTexture2D(texture).Sample(GetSampler(mi.sampler), uv).rg * 2.0 - 1.0;
    return float4(xy, sqrt(saturate(1.0 - dot(xy, xy))), 1.0);
}

float Fresnel(MaterialInputs mi, float exponent, float baseReflect)
{
    const float f = pow(1.0 - saturate(dot(normalize(mi.normal), mi.viewDir)), max(exponent, 0.0));
    return baseReflect + (1.0 - baseReflect) * f;
}

float3 Desaturate(float3 color, float fraction)
{
    const float grey = dot(color, float3(0.2126, 0.7152, 0.0722));
    return lerp(color, grey.xxx, fraction);
}

/// World-aligned grid, projected along the dominant axis of the normal (works on any scaled box without UVs).
/// x = line mask (1 on a line, antialiased, fading to the average far away), y = checker (0 / 1).
float2 WorldGrid(MaterialInputs mi, float cellSize, float lineWidth)
{
    const float3 n = abs(mi.normal);
    const float3 p = mi.worldPos / max(cellSize, 1e-3);
    const float2 uv = (n.y >= n.x && n.y >= n.z) ? p.xz : (n.x >= n.z ? p.zy : p.xy);
    const float2 fw = max(fwidth(uv), 1e-5);
    const float2 d = abs(frac(uv - 0.5) - 0.5);
    const float halfWidth = saturate(lineWidth) * 0.5;
    const float2 l = 1.0 - smoothstep(halfWidth - fw, halfWidth + fw, d);
    const float lines = lerp(max(l.x, l.y), saturate(lineWidth * 2.0), saturate(max(fw.x, fw.y) * 2.0 - 0.5));
    const float checker = fmod(floor(uv.x) + floor(uv.y) + 4096.0, 2.0);
    return float2(lines, checker);
}

float NoiseHash(float3 p)
{
    p = frac(p * 0.3183099 + 0.1);
    p *= 17.0;
    return frac(p.x * p.y * p.z * (p.x + p.y + p.z));
}

float ValueNoise(float3 p)
{
    const float3 i = floor(p);
    float3 f = frac(p);
    f = f * f * (3.0 - 2.0 * f);
    return lerp(lerp(lerp(NoiseHash(i), NoiseHash(i + float3(1, 0, 0)), f.x), lerp(NoiseHash(i + float3(0, 1, 0)), NoiseHash(i + float3(1, 1, 0)), f.x), f.y),
                lerp(lerp(NoiseHash(i + float3(0, 0, 1)), NoiseHash(i + float3(1, 0, 1)), f.x), lerp(NoiseHash(i + float3(0, 1, 1)), NoiseHash(i + 1.0), f.x), f.y), f.z);
}

/// Fractal value noise 0..1 (4 octaves).
float FractalNoise(float3 p, float scale)
{
    p *= max(scale, 1e-4);
    float sum = 0.0, amplitude = 0.5;
    [unroll] for (int o = 0; o < 4; ++o) {
        sum += ValueNoise(p) * amplitude;
        p = p * 2.03 + 17.1;
        amplitude *= 0.5;
    }
    return saturate(sum / 0.9375);
}

float SafeNormalize(float x) { return x == 0.0 ? 0.0 : sign(x); }
float2 SafeNormalize(float2 v) { const float l = dot(v, v); return l > 1e-12 ? v * rsqrt(l) : 0.0; }
float3 SafeNormalize(float3 v) { const float l = dot(v, v); return l > 1e-12 ? v * rsqrt(l) : 0.0; }
float4 SafeNormalize(float4 v) { const float l = dot(v, v); return l > 1e-12 ? v * rsqrt(l) : 0.0; }

#endif // ZX_MATERIAL_GRAPH_HLSLI
