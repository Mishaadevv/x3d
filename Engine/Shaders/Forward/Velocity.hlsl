// Velocity of moving objects: screen-space motion (current uv - previous uv) from each instance's world matrix this
// frame and last frame. Only instances that moved are drawn (depth-tested against the scene, no depth writes); the
// rest of the screen keeps 0 and its motion comes from the camera (depth reprojection) where it is needed.

#include "Common/Bindless.hlsli"
#include "Common/Mesh.hlsli"

struct VelocityParams
{
    float4x4 viewProj;         // this frame, jittered (rasterizes exactly like the scene)
    float4x4 viewProjNoJitter;
    float4x4 prevViewProj;     // last frame, no jitter
    uint instanceBuffer;
    uint instanceStride;
    uint2 pad;
};
ZX_PASS_CONSTANTS(VelocityParams, g_Vel);

struct DrawData
{
    uint instanceIndex;
    uint materialIndex;
    uint vertexBuffer;
    uint lod;
    uint drawId;
    uint prevVertexBuffer; // skinned meshes: last frame's posed vertices
};
ZX_PUSH_CONSTANTS(DrawData, g_Draw);

struct VSOut
{
    float4 position : SV_Position;
    float4 current : TEXCOORD0;
    float4 previous : TEXCOORD1;
};

float4x4 LoadRows(ByteAddressBuffer b, uint o)
{
    return float4x4(asfloat(b.Load4(o)), asfloat(b.Load4(o + 16)), asfloat(b.Load4(o + 32)), asfloat(b.Load4(o + 48)));
}

VSOut VelocityVS(uint vertexId : SV_VertexID)
{
    const MeshVertex v = LoadMeshVertex(GetBuffer(g_Draw.vertexBuffer), vertexId);
    ByteAddressBuffer instances = GetBuffer(g_Vel.instanceBuffer);
    const uint o = g_Draw.instanceIndex * g_Vel.instanceStride;
    const float4 world = mul(LoadRows(instances, o), float4(v.position, 1.0));
    const float3 prevPosition = g_Draw.prevVertexBuffer != ZX_INVALID_INDEX ? LoadMeshVertex(GetBuffer(g_Draw.prevVertexBuffer), vertexId).position : v.position;
    const float4 prevWorld = mul(LoadRows(instances, o + 128), float4(prevPosition, 1.0)); // previous world rows
    VSOut r;
    r.position = mul(g_Vel.viewProj, world);
    r.current = mul(g_Vel.viewProjNoJitter, world);
    r.previous = mul(g_Vel.prevViewProj, prevWorld);
    return r;
}

float4 VelocityPS(VSOut i) : SV_Target0
{
    const float2 cur = i.current.xy / i.current.w;
    const float2 prev = i.previous.xy / max(i.previous.w, 1e-5);
    // NDC -> uv (y down): the motion in uv units since last frame.
    const float2 motion = (cur - prev) * float2(0.5, -0.5);
    return float4(motion, 0.0, 1.0);
}
