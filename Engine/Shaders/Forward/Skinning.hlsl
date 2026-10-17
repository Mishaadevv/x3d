// GPU skinning: one thread per vertex blends up to four bone matrices (linear blend skinning) and writes the posed
// vertex in the regular packed format (Common/Mesh.hlsli) into the instance's own vertex buffer, which every pass
// then draws like a static mesh. Last frame's buffer is kept for the velocity pass.

#include "Common/Mesh.hlsli"

struct SkinParams
{
    uint vertices;     // SRV: source PackedVertex (bind pose)
    uint skin;         // SRV: per vertex packed bone indices + packed weights (u8 x 4 each)
    uint palette;      // SRV: 3 float4 rows (row-major 3x4) per bone
    uint paletteBase;  // first bone of this instance in the palette
    uint output;       // UAV: posed PackedVertex
    uint vertexCount;
    uint boneCount;
    uint pad;
};
ZX_PUSH_CONSTANTS(SkinParams, g_Skin);

uint PackSnorm16x2(float2 v)
{
    const int2 i = int2(round(clamp(v, -1.0, 1.0) * 32767.0));
    return (uint(i.x) & 0xFFFFu) | (uint(i.y) << 16);
}

float2 OctEncode(float3 n)
{
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    float2 e = n.xy;
    if (n.z < 0.0)
        e = (1.0 - abs(n.yx)) * float2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return e;
}

[numthreads(64, 1, 1)]
void SkinCS(uint id : SV_DispatchThreadID)
{
    if (id >= g_Skin.vertexCount)
        return;
    ByteAddressBuffer src = GetBuffer(g_Skin.vertices);
    const uint base = id * ZX_VERTEX_STRIDE;
    const uint4 a = src.Load4(base);
    const uint4 b = src.Load4(base + 16);
    const uint c = src.Load(base + 32);
    const uint2 skin = GetBuffer(g_Skin.skin).Load2(id * 8);
    ByteAddressBuffer palette = GetBuffer(g_Skin.palette);
    float3x4 m = 0.0;
    [unroll] for (uint k = 0; k < 4; ++k) {
        const float w = float((skin.y >> (k * 8)) & 0xFF) / 255.0;
        if (w <= 0.0)
            continue;
        const uint bone = min((skin.x >> (k * 8)) & 0xFF, g_Skin.boneCount - 1);
        const uint o = (g_Skin.paletteBase + bone) * 48;
        m += w * float3x4(asfloat(palette.Load4(o)), asfloat(palette.Load4(o + 16)), asfloat(palette.Load4(o + 32)));
    }
    const float3 position = mul(m, float4(asfloat(a.xyz), 1.0));
    // Normals / tangents with the 3x3 part (bones carry no shear; non-uniform scale is rare enough to ignore).
    const float3x3 r = (float3x3)m;
    const float3 normal = normalize(mul(r, OctDecode(UnpackSnorm16x2(a.w))));
    const float3 tangent = normalize(mul(r, OctDecode(UnpackSnorm16x2(b.x & ~1u))));
    RWByteAddressBuffer dst = GetRWBuffer(g_Skin.output);
    dst.Store4(base, uint4(asuint(position), PackSnorm16x2(OctEncode(normal))));
    dst.Store4(base + 16, uint4((PackSnorm16x2(OctEncode(tangent)) & ~1u) | (b.x & 1u), b.yzw));
    dst.Store(base + 32, c);
}
