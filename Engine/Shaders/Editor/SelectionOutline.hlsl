// Selection outline (editor): selected meshes are rasterized into a mask (R = visible, G = covered at all), then a
// fullscreen pass draws a soft outline around the mask on top of the final image. Hidden parts get a dimmer outline.

#include "Common/Bindless.hlsli"
#include "Common/Mesh.hlsli"

// --- Mask --------------------------------------------------------------------------------------------------------------

struct MaskPush
{
    float4x4 worldViewProj;
    uint vertexBuffer;
    uint sceneDepth;       // reversed-Z depth at render resolution
    float2 outputToDepth;  // output pixel -> depth texel scale
    float nearPlane;
    float3 pad;
};

struct MaskVSOutput
{
    float4 position : SV_Position;
};

#ifdef ZX_MASK
ZX_PUSH_CONSTANTS(MaskPush, g_Mask);

MaskVSOutput MaskVS(uint vertexId : SV_VertexID)
{
    const MeshVertex v = LoadMeshVertex(GetBuffer(g_Mask.vertexBuffer), vertexId);
    MaskVSOutput o;
    o.position = mul(g_Mask.worldViewProj, float4(v.position, 1.0));
    return o;
}

float2 MaskPS(MaskVSOutput i) : SV_Target0
{
    const int2 texel = int2(i.position.xy * g_Mask.outputToDepth);
    const float scene = GetTexture2D(g_Mask.sceneDepth).Load(int3(texel, 0)).r;
    // Compare linear view depths (reversed infinite Z: z = near / viewZ); the bias absorbs TAA jitter and LOD changes.
    const float fragZ = g_Mask.nearPlane / max(i.position.z, 1e-7);
    const float sceneZ = g_Mask.nearPlane / max(scene, 1e-7);
    const float visible = fragZ <= sceneZ * 1.01 + 0.02 ? 1.0 : 0.0;
    return float2(visible, 1.0);
}
#endif

// --- Outline -----------------------------------------------------------------------------------------------------------

struct OutlinePush
{
    uint mask;
    float radius;      // pixels
    float hiddenAlpha; // opacity of the outline around hidden parts
    float pad;
    float4 color;      // sRGB-encoded (the output is an sRGB-encoded UNORM target)
};

struct OutlineVSOutput
{
    float4 position : SV_Position;
};

#ifdef ZX_OUTLINE
ZX_PUSH_CONSTANTS(OutlinePush, g_Outline);

OutlineVSOutput OutlineVS(uint vertexId : SV_VertexID)
{
    const float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    OutlineVSOutput o;
    o.position = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

float4 OutlinePS(OutlineVSOutput i) : SV_Target0
{
    Texture2D<float4> mask = GetTexture2D(g_Outline.mask);
    const int2 p = int2(i.position.xy);
    const float2 center = mask.Load(int3(p, 0)).rg;
    if (center.g > 0.5)
        discard; // inside the selection: no fill

    // Coverage of the neighbourhood on two rings (soft, roughly anti-aliased edge).
    float covered = 0.0, visible = 0.0;
    const float r = g_Outline.radius;
    [unroll] for (int k = 0; k < 16; ++k) {
        const float a = k * (6.2831853 / 16.0);
        const float2 dir = float2(cos(a), sin(a));
        [unroll] for (int ring = 0; ring < 2; ++ring) {
            const float2 m = mask.Load(int3(p + int2(round(dir * r * (ring == 0 ? 1.0 : 0.5))), 0)).rg;
            covered += m.g;
            visible += m.r;
        }
    }
    if (covered <= 0.0)
        discard;
    const float strength = saturate(covered / 6.0);
    const float alpha = strength * lerp(g_Outline.hiddenAlpha, 1.0, saturate(visible / max(covered, 1.0) * 2.0));
    return float4(g_Outline.color.rgb, g_Outline.color.a * alpha);
}
#endif
