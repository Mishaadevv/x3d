// ZUI 2D renderer: one instanced quad per primitive, shapes evaluated as signed distance fields in pixel space.
// Colors are sRGB-encoded and blended in display space (like browsers / design tools); output is premultiplied alpha.
// Keep the primitive layout in sync with ui::GpuPrimitive (UI/DrawList.h).

#include "Common/Bindless.hlsli"
#include "Common/Color.hlsli"

#define PRIM_RECT 0u
#define PRIM_SHADOW 1u
#define PRIM_GLYPH 2u
#define PRIM_IMAGE 3u
#define PRIM_LINE 4u
#define PRIM_TRIANGLE 5u

struct UIPush
{
    float2 invViewport;  // 1 / render target size
    uint primitives;     // ByteAddressBuffer SRV
    uint clips;          // ByteAddressBuffer SRV
    uint atlas;          // Texture2DArray SRV (R8 glyph pages)
    uint primOffset;     // first primitive of this draw
    float atlasInvSize;  // 1 / atlas page size
    float textGamma;     // coverage exponent for text (< 1 thickens)
};
ZX_PUSH_CONSTANTS(UIPush, g_Push);

struct VSOutput
{
    float4 position : SV_Position;
    nointerpolation float4 rect : RECT;
    nointerpolation float4 data : DATA;
    nointerpolation uint4 colors : COLORS;   // color0, color1, border, header
    nointerpolation float4 params : PARAMS;  // borderWidth, blur, texture (asfloat), extra (image radius / hollow inset)
    nointerpolation float4 clipRect : CLIPRECT;
    nointerpolation float clipRadius : CLIPRADIUS;
};

VSOutput VSMain(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID)
{
    ByteAddressBuffer prims = GetBuffer(g_Push.primitives);
    const uint base = (instanceId + g_Push.primOffset) * 64;
    VSOutput o;
    o.rect = asfloat(prims.Load4(base));
    o.data = asfloat(prims.Load4(base + 16));
    o.colors = prims.Load4(base + 32);
    o.params = asfloat(prims.Load4(base + 48));
    const uint type = o.colors.w & 0xF;
    const uint clipIndex = o.colors.w >> 16;
    ByteAddressBuffer clips = GetBuffer(g_Push.clips);
    o.clipRect = asfloat(clips.Load4(clipIndex * 32));
    o.clipRadius = asfloat(clips.Load(clipIndex * 32 + 16));

    // 24 vertices per instance = up to 4 quads. Solid primitives use quad 0 only (the rest collapse to a point);
    // hollow rects/shadows (params.w = inset > 0) emit a ring of 4 bands around the inset interior.
    const uint quad = vertexId / 6;
    const uint corners[6] = {0, 1, 2, 2, 1, 3};
    const uint c = corners[vertexId % 6];
    const float2 t = float2(c & 1, c >> 1);
    const bool hollow = (type == PRIM_RECT || type == PRIM_SHADOW) && o.params.w > 0.0;
    float2 pos;
    if (quad > 0 && !hollow) {
        o.position = float4(0, 0, 0, 1); // degenerate
        return o;
    }
    if (hollow) {
        const float expand = type == PRIM_SHADOW ? o.params.y + 1.0 : 1.0;
        const float2 outerMin = o.rect.xy - expand, outerMax = o.rect.zw + expand;
        const float2 innerMin = o.rect.xy + o.params.w, innerMax = o.rect.zw - o.params.w;
        float2 bmin, bmax;
        if (quad == 0) { bmin = outerMin; bmax = float2(outerMax.x, innerMin.y); }                 // top
        else if (quad == 1) { bmin = float2(outerMin.x, innerMax.y); bmax = outerMax; }            // bottom
        else if (quad == 2) { bmin = float2(outerMin.x, innerMin.y); bmax = float2(innerMin.x, innerMax.y); } // left
        else { bmin = float2(innerMax.x, innerMin.y); bmax = float2(outerMax.x, innerMax.y); }     // right
        pos = lerp(bmin, bmax, t);
    } else if (type == PRIM_LINE) {
        const float2 p0 = o.rect.xy, p1 = o.rect.zw;
        const float h = max(o.data.x, 1.0) * 0.5 + 1.0;
        float2 dir = p1 - p0;
        const float len = length(dir);
        dir = len > 1e-4 ? dir / len : float2(1, 0);
        const float2 n = float2(-dir.y, dir.x);
        pos = lerp(p0 - dir * h, p1 + dir * h, t.x) + n * (t.y * 2.0 - 1.0) * h;
    } else if (type == PRIM_TRIANGLE) {
        const float2 a = o.rect.xy, b = o.rect.zw, cc = o.data.xy;
        const float2 mn = min(a, min(b, cc)) - 1.0, mx = max(a, max(b, cc)) + 1.0;
        pos = lerp(mn, mx, t);
    } else {
        float expand = 0.0;
        if (type == PRIM_RECT)
            expand = 1.0;
        else if (type == PRIM_SHADOW)
            expand = o.params.y + 1.0;
        pos = lerp(o.rect.xy - expand, o.rect.zw + expand, t);
    }
    // Axis-aligned quads are clamped to the clip rectangle: pixels outside it are never rasterized.
    if (type != PRIM_LINE && type != PRIM_TRIANGLE)
        pos = clamp(pos, o.clipRect.xy, max(o.clipRect.zw, o.clipRect.xy));
    o.position = float4(pos * g_Push.invViewport * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

/// Rounded box SDF, p relative to the center (y down), radii = (tl, tr, br, bl).
float RoundBoxSDF(float2 p, float2 halfSize, float4 radii)
{
    const float r = p.x > 0.0 ? (p.y > 0.0 ? radii.z : radii.y) : (p.y > 0.0 ? radii.w : radii.x);
    const float2 q = abs(p) - halfSize + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

float4 PSMain(VSOutput i) : SV_Target0
{
    const float2 p = i.position.xy;
    // Clip: rectangle (+ rounded corners).
    if (any(p < i.clipRect.xy) || any(p >= i.clipRect.zw))
        discard;
    float clipCoverage = 1.0;
    if (i.clipRadius > 0.0) {
        const float2 c = (i.clipRect.xy + i.clipRect.zw) * 0.5, h = (i.clipRect.zw - i.clipRect.xy) * 0.5;
        clipCoverage = saturate(0.5 - RoundBoxSDF(p - c, h, i.clipRadius.xxxx));
    }

    const uint header = i.colors.w;
    const uint type = header & 0xF;
    const float4 color0 = UnpackRGBA8(i.colors.x);
    float4 result = 0.0; // premultiplied

    if (type == PRIM_RECT) {
        const float2 center = (i.rect.xy + i.rect.zw) * 0.5, halfSize = (i.rect.zw - i.rect.xy) * 0.5;
        const float d = RoundBoxSDF(p - center, halfSize, i.data);
        const float outer = saturate(0.5 - d);
        float4 fill = color0;
        const uint gradient = (header >> 4) & 3;
        if (gradient != 0) {
            const float2 local = saturate((p - i.rect.xy) / max(i.rect.zw - i.rect.xy, 1e-3));
            fill = lerp(color0, UnpackRGBA8(i.colors.y), gradient == 1 ? local.y : local.x);
        }
        const float bw = i.params.x;
        if (bw > 0.0) {
            const float inner = saturate(0.5 - (d + bw));
            const float4 border = UnpackRGBA8(i.colors.z);
            result = float4(fill.rgb * fill.a, fill.a) * inner + float4(border.rgb * border.a, border.a) * (outer - inner);
        } else {
            result = float4(fill.rgb * fill.a, fill.a) * outer;
        }
    } else if (type == PRIM_SHADOW) {
        const float2 center = (i.rect.xy + i.rect.zw) * 0.5, halfSize = (i.rect.zw - i.rect.xy) * 0.5;
        const float d = RoundBoxSDF(p - center, halfSize, i.data);
        const float blur = i.params.y;
        // Smooth falloff approximating a Gaussian-blurred box.
        const float x = saturate((d + blur * 0.5) / (blur * 1.5));
        const float a = color0.a * (1.0 - x) * (1.0 - x) * (1.0 + 2.0 * x) * 1.0;
        result = float4(color0.rgb * a, a);
    } else if (type == PRIM_GLYPH) {
        const float2 local = (p - i.rect.xy) / max(i.rect.zw - i.rect.xy, 1e-3);
        const float2 uv = lerp(i.data.xy, i.data.zw, local) * g_Push.atlasInvSize;
        const uint page = (header >> 8) & 0xF;
        float a = GetTexture2DArray(g_Push.atlas).SampleLevel(g_LinearClamp, float3(uv, page), 0).r;
        a = pow(a, g_Push.textGamma) * color0.a;
        result = float4(color0.rgb * a, a);
    } else if (type == PRIM_IMAGE) {
        const float2 local = (p - i.rect.xy) / max(i.rect.zw - i.rect.xy, 1e-3);
        const float2 uv = lerp(i.data.xy, i.data.zw, local);
        float4 c = GetTexture2DNonUniform(asuint(i.params.z)).SampleLevel(g_LinearClamp, uv, 0) * color0;
        float coverage = 1.0;
        if (i.params.w > 0.0) {
            const float2 center = (i.rect.xy + i.rect.zw) * 0.5, halfSize = (i.rect.zw - i.rect.xy) * 0.5;
            coverage = saturate(0.5 - RoundBoxSDF(p - center, halfSize, i.params.wwww));
        }
        c.a *= coverage;
        result = float4(c.rgb * c.a, c.a);
    } else if (type == PRIM_LINE) {
        const float2 pa = p - i.rect.xy, ba = i.rect.zw - i.rect.xy;
        const float h = saturate(dot(pa, ba) / max(dot(ba, ba), 1e-6));
        const float thickness = i.data.x;
        const float d = length(pa - ba * h) - max(thickness, 1.0) * 0.5;
        const float a = saturate(0.5 - d) * min(thickness, 1.0) * color0.a;
        result = float4(color0.rgb * a, a);
    } else if (type == PRIM_TRIANGLE) {
        const float2 v[3] = {i.rect.xy, i.rect.zw, i.data.xy};
        const uint aaMask = asuint(i.data.z);
        float dAA = -1e9;
        bool inside = true;
        [unroll] for (uint k = 0; k < 3; ++k) {
            const float2 e = v[(k + 1) % 3] - v[k];
            const float2 nIn = float2(-e.y, e.x) / max(length(e), 1e-6);
            const float d = -dot(p - v[k], nIn);
            if (aaMask & (1u << k))
                dAA = max(dAA, d);
            else
                inside = inside && d < 0.0;
        }
        const float a = (inside ? saturate(0.5 - dAA) : 0.0) * color0.a;
        result = float4(color0.rgb * a, a);
    }
    result *= clipCoverage;
    if (result.a <= 1.0 / 512.0)
        discard;
    return result;
}
