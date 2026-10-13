#ifndef ZX_MESH_HLSLI
#define ZX_MESH_HLSLI

// Packed mesh vertex (36 bytes), see asset::PackedVertex. Vertices are pulled from a ByteAddressBuffer.
//   float3 position | uint normal (oct snorm16x2) | uint tangent (oct snorm16x2, sign in bit 0)
//   float2 uv0 | uint uv1 (half2) | uint color (RGBA8)

#include "Common/Bindless.hlsli"

#define ZX_VERTEX_STRIDE 36

struct MeshVertex
{
    float3 position;
    float3 normal;
    float4 tangent; // w = bitangent sign
    float2 uv0;
    float2 uv1;
    float4 color;
};

float2 UnpackSnorm16x2(uint p)
{
    const int2 v = int2(int(p << 16) >> 16, int(p) >> 16);
    return max(float2(v) / 32767.0, -1.0);
}

float3 OctDecode(float2 e)
{
    float3 n = float3(e.x, e.y, 1.0 - abs(e.x) - abs(e.y));
    const float t = saturate(-n.z);
    n.x += n.x >= 0.0 ? -t : t;
    n.y += n.y >= 0.0 ? -t : t;
    return normalize(n);
}

float4 UnpackUnorm4x8(uint p)
{
    return float4(p & 0xFF, (p >> 8) & 0xFF, (p >> 16) & 0xFF, p >> 24) / 255.0;
}

MeshVertex LoadMeshVertex(ByteAddressBuffer vb, uint index)
{
    const uint base = index * ZX_VERTEX_STRIDE;
    const uint4 a = vb.Load4(base);      // position.xyz, normal
    const uint4 b = vb.Load4(base + 16); // tangent, uv0.xy, uv1
    const uint c = vb.Load(base + 32);   // color
    MeshVertex v;
    v.position = asfloat(a.xyz);
    v.normal = OctDecode(UnpackSnorm16x2(a.w));
    v.tangent = float4(OctDecode(UnpackSnorm16x2(b.x & ~1u)), (b.x & 1u) ? -1.0 : 1.0);
    v.uv0 = asfloat(b.yz);
    v.uv1 = f16tof32(uint2(b.w & 0xFFFF, b.w >> 16));
    v.color = UnpackUnorm4x8(c);
    return v;
}

#endif // ZX_MESH_HLSLI
