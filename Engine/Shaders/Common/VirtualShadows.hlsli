// Virtual shadow maps of the sun (SceneRendererVsm.cpp): a clipmap of levels around the camera, each 128 x 128 pages of
// 128 x 128 texels; level k's texels are texel0 * 2^k meters. A pixel uses the level whose texels match its footprint
// (distance * pixelScale); the pages it touches are marked (VirtualShadows.hlsl MarkCS), the CPU allocates them in the
// physical atlas a frame or two later and renders the ones that are new or that something moved through. Missing pages
// fall back to coarser levels, so there are never holes - just softer shadows for a moment.
// Table (ByteAddressBuffer): per level 16 bytes (window origin page x, y; page size in meters; -) for the shown pages,
// then from byte 256 the same for the generation being built (the sun turned), from byte 512 the page entries of the
// shown generation (level * 16384 + y * 128 + x): physical page + 1, 0 = not resident.

#ifndef ZX_VIRTUAL_SHADOWS_HLSLI
#define ZX_VIRTUAL_SHADOWS_HLSLI

static const uint VsmWindowPages = 128, VsmPageTexels = 128, VsmTableEntries = 512, VsmBuildHeader = 256;

uint VsmLevel(float distance, float pixelScale, float texel0, uint levels)
{
    const float footprint = distance * pixelScale;
    const float level = ceil(log2(max(footprint / texel0, 1.0)));
    return min(uint(level), levels - 1);
}

float3 VsmLightSpace(float4 rows[3], float3 world)
{
    return float3(dot(rows[0].xyz, world), dot(rows[1].xyz, world), dot(rows[2].xyz, world));
}

/// The physical page holding light-space point `l` at `level`, or false.
bool VsmLookup(ByteAddressBuffer table, uint level, float2 l, out uint phys, out float2 inPage)
{
    const uint4 header = table.Load4(level * 16);
    const float pageWorld = asfloat(header.z);
    const float2 pagePos = l / pageWorld;
    const int2 page = int2(floor(pagePos)) - int2(header.xy);
    phys = 0;
    inPage = pagePos - floor(pagePos);
    if (any(page < 0) || any(page >= int(VsmWindowPages)))
        return false;
    const uint entry = table.Load(VsmTableEntries + (level * VsmWindowPages * VsmWindowPages + uint(page.y) * VsmWindowPages + uint(page.x)) * 4);
    phys = entry - 1;
    return entry != 0;
}

#endif
