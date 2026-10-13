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

float SafeNormalize(float x) { return x == 0.0 ? 0.0 : sign(x); }
float2 SafeNormalize(float2 v) { const float l = dot(v, v); return l > 1e-12 ? v * rsqrt(l) : 0.0; }
float3 SafeNormalize(float3 v) { const float l = dot(v, v); return l > 1e-12 ? v * rsqrt(l) : 0.0; }
float4 SafeNormalize(float4 v) { const float l = dot(v, v); return l > 1e-12 ? v * rsqrt(l) : 0.0; }

#endif // ZX_MATERIAL_GRAPH_HLSLI
