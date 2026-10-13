#ifndef ZX_LIGHTS_HLSLI
#define ZX_LIGHTS_HLSLI

// Punctual lights (point/spot) for clustered forward shading. Keep in sync with render::GpuLight (SceneRenderer.cpp).
// Units: color * intensity in candela (luminous intensity); the renderer pre-multiplies by the view exposure.

#define ZX_LIGHT_POINT 0u
#define ZX_LIGHT_SPOT 1u
#define ZX_LIGHT_STRIDE 64u
#define ZX_CLUSTER_GRID_X 16u
#define ZX_CLUSTER_GRID_Y 9u
#define ZX_CLUSTER_GRID_Z 24u
#define ZX_MAX_LIGHTS_PER_CLUSTER 63u // + 1 count = 256 bytes per cluster

struct PunctualLight
{
    float3 position;
    float range;
    float3 color; // linear, * intensity (cd) * exposure
    uint type;
    float3 direction; // spot: direction the light shines
    float spotScale;  // 1 / (cos(inner) - cos(outer))
    float spotOffset; // -cos(outer) * spotScale
    uint shadowFace;  // first face in the local shadow face buffer, or invalid
    float shadowTexel; // world texel size per meter of distance
};

PunctualLight LoadLight(ByteAddressBuffer b, uint index)
{
    const uint o = index * ZX_LIGHT_STRIDE;
    const uint4 r0 = b.Load4(o), r1 = b.Load4(o + 16), r2 = b.Load4(o + 32), r3 = b.Load4(o + 48);
    PunctualLight l;
    l.position = asfloat(r0.xyz);
    l.range = asfloat(r0.w);
    l.color = asfloat(r1.xyz);
    l.type = r1.w;
    l.direction = asfloat(r2.xyz);
    l.spotScale = asfloat(r2.w);
    l.spotOffset = asfloat(r3.x);
    l.shadowFace = r3.y;
    l.shadowTexel = asfloat(r3.z);
    return l;
}

/// Inverse-square falloff windowed to reach zero at the range (Karis 2013).
float DistanceAttenuation(float distanceSq, float range)
{
    const float r = distanceSq / (range * range);
    const float window = saturate(1.0 - r * r);
    return window * window / max(distanceSq, 1e-4);
}

float SpotAttenuation(PunctualLight l, float3 toLight)
{
    if (l.type != ZX_LIGHT_SPOT)
        return 1.0;
    const float a = saturate(dot(-toLight, l.direction) * l.spotScale + l.spotOffset);
    return a * a;
}

uint ClusterIndex(uint3 c)
{
    return (c.z * ZX_CLUSTER_GRID_Y + c.y) * ZX_CLUSTER_GRID_X + c.x;
}

#endif // ZX_LIGHTS_HLSLI
