// Screen-space GI (GlobalIllumination::AddScreenPass): the short-range bounce the probes are too coarse for - a red
// ball tinting the floor under it, light from moving and emissive objects, contact bounce in corners.
//   TraceCS     half resolution: a few cosine-distributed rays per pixel march through the depth buffer (up to a few
//               meters); a hit takes the radiance of this frame's lit opaque scene there (direct light + probe GI:
//               more bounces). rgb = average hit radiance x hit fraction, a = hit fraction (0 = leave it to the probes)
//   TemporalCS  blends with last frame's result (reprojected, clamped to the current neighborhood: no ghosting)
// The forward / deferred lighting reads the result next frame (reprojected), over the probes: probes x (1 - a) + rgb.

#include "Common/Bindless.hlsli"

// Keep in sync with render::SsgiParams (GlobalIllumination.cpp).
struct SsgiParams
{
    float4x4 invViewProj;
    float4x4 viewProj;
    float4x4 prevViewProj;  // the view the history was made with
    float3 cameraPosition;
    float nearZ;
    float3 cameraForward;
    float maxDistance;      // meters a ray marches
    float2 targetSize;      // half resolution
    float2 sceneSize;
    uint color;             // this frame's lit opaque scene (SRV)
    uint depth;
    uint normals;           // octahedral normal + roughness, a >= 0.5: opaque surface
    uint traceUav;          // TraceCS output
    uint traceSrv;
    uint history;           // last frame's result (SRV), invalid = none
    uint output;            // UAV
    uint frame;
    uint rays;
    uint steps;
    float thickness;
    float historyWeight;
};
ZX_PASS_CONSTANTS(SsgiParams, g_Ssgi);

float3 DecodeOct(float2 e)
{
    e = e * 2.0 - 1.0;
    float3 n = float3(e.xy, 1.0 - abs(e.x) - abs(e.y));
    const float t = saturate(-n.z);
    n.xy += float2(n.x >= 0.0 ? -t : t, n.y >= 0.0 ? -t : t);
    return normalize(n);
}

float3 WorldFromDepth(float2 uv, float d)
{
    const float4 p = mul(g_Ssgi.invViewProj, float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, d, 1.0));
    return p.xyz / p.w;
}

float3 Project(float3 p)
{
    const float4 c = mul(g_Ssgi.viewProj, float4(p, 1.0));
    return float3(c.x / c.w * 0.5 + 0.5, 0.5 - c.y / c.w * 0.5, c.w);
}

float Hash(uint3 v)
{
    uint h = v.x * 1664525u + v.y * 1013904223u + v.z * 2654435761u;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    return float(h & 0xFFFFFFu) / 16777216.0;
}

[numthreads(8, 8, 1)]
void TraceCS(uint2 id : SV_DispatchThreadID)
{
    if (any(id >= uint2(g_Ssgi.targetSize)))
        return;
    RWTexture2D<float4> output = GetRWTexture2D(g_Ssgi.traceUav);
    const float2 uv = (float2(id) + 0.5) / g_Ssgi.targetSize;
    Texture2D<float4> depthTex = GetTexture2D(g_Ssgi.depth);
    const float d = depthTex.SampleLevel(g_PointClamp, uv, 0).r;
    const float4 nr = GetTexture2D(g_Ssgi.normals).SampleLevel(g_PointClamp, uv, 0);
    if (d <= 0.0 || nr.a < 0.5) {
        output[id] = 0;
        return;
    }
    const float3 n = DecodeOct(nr.rg);
    const float3 p = WorldFromDepth(uv, d);
    const float3 t = normalize(abs(n.y) < 0.99 ? cross(float3(0, 1, 0), n) : cross(float3(1, 0, 0), n));
    const float3 b = cross(n, t);
    const float3 s0 = Project(p + n * 0.02);
    const float steps = float(max(g_Ssgi.steps, 4u));
    Texture2D<float4> colorTex = GetTexture2D(g_Ssgi.color);
    Texture2D<float4> normalTex = GetTexture2D(g_Ssgi.normals);
    float3 sum = 0.0;
    float hits = 0.0;
    [loop] for (uint r = 0; r < g_Ssgi.rays; ++r) {
        // Cosine-distributed direction, a new pattern per pixel and frame.
        const float u1 = Hash(uint3(id, g_Ssgi.frame * 8 + r)), u2 = Hash(uint3(id.yx + 17, g_Ssgi.frame * 8 + r));
        const float phi = 6.2831853 * u2, radius = sqrt(u1);
        const float3 dir = normalize(t * (radius * cos(phi)) + b * (radius * sin(phi)) + n * sqrt(max(1.0 - u1, 0.0)));
        // Keep the ray in front of the camera.
        float len = g_Ssgi.maxDistance;
        const float startZ = dot(p - g_Ssgi.cameraPosition, g_Ssgi.cameraForward);
        const float rz = dot(dir, g_Ssgi.cameraForward);
        if (rz < 0.0)
            len = min(len, (startZ - g_Ssgi.nearZ * 2.0) / -rz);
        if (len <= 0.05)
            continue;
        const float3 s1 = Project(p + dir * len);
        const float noise = Hash(uint3(id, g_Ssgi.frame * 8 + r + 4));
        [loop] for (float i = 1.0; i <= steps; i += 1.0) {
            // Denser near the start (contact bounce matters most).
            const float f = (i - 1.0 + noise) / steps;
            const float k = f * f;
            const float2 suv = lerp(s0.xy, s1.xy, k);
            if (any(suv < 0.0) || any(suv > 1.0))
                break;
            const float rayZ = 1.0 / lerp(1.0 / s0.z, 1.0 / s1.z, k);
            const float sd = depthTex.SampleLevel(g_PointClamp, suv, 0).r;
            const float sceneZ = sd > 0.0 ? g_Ssgi.nearZ / sd : 1e9;
            const float diff = rayZ - sceneZ;
            if (diff > 0.0 && diff < g_Ssgi.thickness * (1.0 + sceneZ * 0.05)) {
                // Only surfaces that face the ray give light.
                const float3 hn = DecodeOct(normalTex.SampleLevel(g_PointClamp, suv, 0).rg);
                const float facing = saturate(-dot(hn, dir) * 4.0);
                const float2 edge = saturate(min(suv, 1.0 - suv) * 10.0);
                const float w = facing * edge.x * edge.y;
                sum += colorTex.SampleLevel(g_PointClamp, suv, 0).rgb * w;
                hits += w;
                break;
            }
        }
    }
    const float inv = 1.0 / float(max(g_Ssgi.rays, 1u));
    output[id] = float4(min(sum * inv, 60000.0), hits * inv);
}

[numthreads(8, 8, 1)]
void TemporalCS(uint2 id : SV_DispatchThreadID)
{
    if (any(id >= uint2(g_Ssgi.targetSize)))
        return;
    Texture2D<float4> current = GetTexture2D(g_Ssgi.traceSrv);
    const float4 c = current.Load(int3(id, 0));
    float4 result = c;
    if (g_Ssgi.history != ZX_INVALID_INDEX) {
        const float2 uv = (float2(id) + 0.5) / g_Ssgi.targetSize;
        const float d = GetTexture2D(g_Ssgi.depth).SampleLevel(g_PointClamp, uv, 0).r;
        if (d > 0.0) {
            const float4 prev = mul(g_Ssgi.prevViewProj, float4(WorldFromDepth(uv, d), 1.0));
            const float2 puv = float2(prev.x / prev.w * 0.5 + 0.5, 0.5 - prev.y / prev.w * 0.5);
            if (prev.w > 0.0 && all(puv > 0.0) && all(puv < 1.0)) {
                // Neighborhood clamp: the history may only be as bright / dark as what this frame sees around.
                float4 lo = c, hi = c;
                [unroll] for (int y = -1; y <= 1; ++y)
                    [unroll] for (int x = -1; x <= 1; ++x) {
                        const float4 s = current.Load(int3(clamp(int2(id) + int2(x, y), 0, int2(g_Ssgi.targetSize) - 1), 0));
                        lo = min(lo, s);
                        hi = max(hi, s);
                    }
                const float4 h = clamp(GetTexture2D(g_Ssgi.history).SampleLevel(g_LinearClamp, puv, 0), lo, hi);
                result = lerp(c, h, g_Ssgi.historyWeight);
            }
        }
    }
    GetRWTexture2D(g_Ssgi.output)[id] = result;
}
