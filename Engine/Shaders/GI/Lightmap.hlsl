// Lightmap baking, step 1 (GlobalIllumination bake): every lightmapped instance is drawn in its UV1 rectangle of the
// atlas (conservative rasterization: thin triangles still cover their texels), writing the world position and normal
// of each texel. Step 2 (DDGI.hlsl LightmapTraceCS) shoots rays from there.

#include "Common/Bindless.hlsli"
#include "Common/Mesh.hlsli"

// Keep in sync with render::LightmapRasterParams (GlobalIllumination.cpp).
struct RasterParams
{
    float4 worldRows[3];
    float4 scaleBias;     // UV1 * xy + zw = atlas uv
    uint vertexBuffer;
    uint3 pad;
};
ZX_PUSH_CONSTANTS(RasterParams, g_Raster);

struct RasterVSOut
{
    float4 position : SV_Position;
    float3 world : TEXCOORD0;
    float3 normal : NORMAL;
};

RasterVSOut RasterVS(uint vertexId : SV_VertexID)
{
    const MeshVertex v = LoadMeshVertex(GetBuffer(g_Raster.vertexBuffer), vertexId);
    RasterVSOut o;
    const float2 uv = v.uv1 * g_Raster.scaleBias.xy + g_Raster.scaleBias.zw;
    o.position = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.5, 1.0);
    const float4 p = float4(v.position, 1.0);
    o.world = float3(dot(g_Raster.worldRows[0], p), dot(g_Raster.worldRows[1], p), dot(g_Raster.worldRows[2], p));
    o.normal = float3(dot(g_Raster.worldRows[0].xyz, v.normal), dot(g_Raster.worldRows[1].xyz, v.normal), dot(g_Raster.worldRows[2].xyz, v.normal));
    return o;
}

struct RasterPSOut
{
    float4 position : SV_Target0; // world position, w = 1 covered
    float4 normal : SV_Target1;   // world normal, w = 1
};

RasterPSOut RasterPS(RasterVSOut i)
{
    RasterPSOut o;
    o.position = float4(i.world, 1.0);
    o.normal = float4(normalize(i.normal), 1.0);
    return o;
}
