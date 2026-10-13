// Reflections: the sky as a cubemap, reflection probe captures (one cube face per frame), GGX prefiltering of the cube
// mips (roughness = mip / (mips - 1)), and screen-space reflections traced against the depth buffer.
// Cubes are stored at a fixed reference exposure (the forward pass rescales them to the current one).

#include "Common/Bindless.hlsli"
#include "Sky/Atmosphere.hlsli"

struct ReflectionParams
{
    float4x4 invViewProj;  // SSR: current view (clip -> world)
    float4x4 viewProj;     // SSR: current view
    uint input;            // SRV: capture color / sky-view LUT / source cube / scene color
    uint depth;            // SRV: capture depth / scene depth
    uint output;           // UAV: cube (2D array) mip / SSR target
    uint normals;          // SRV: SSR normal + roughness
    float3 skyScale;       // sky-view LUT -> radiance at the reference exposure
    float exposureScale;   // capture: reference exposure / capture exposure
    uint size;             // output face size (cube passes)
    uint face;             // capture: cube face
    float roughness;       // prefilter: roughness of this mip
    uint samples;          // prefilter: GGX samples
    float3 cameraPosition; // SSR
    float nearZ;
    float2 targetSize;     // SSR output size
    float2 sceneSize;      // SSR full-resolution size
    uint steps;            // SSR march steps
    float maxDistance;     // meters
    float thickness;       // meters at 1 m depth (grows with distance)
    float maxRoughness;
    uint frame;
    uint sky;              // SRV: sky-view LUT (capture copy)
    float2 pad;
    float3 cameraForward;  // SSR
    float pad2;
};
ZX_PASS_CONSTANTS(ReflectionParams, g_Refl);

static const float PI = 3.14159265359;
static const float MaxHalf = 60000.0;

// Direction through texel `uv` (0..1) of cube face `face` (D3D order +X, -X, +Y, -Y, +Z, -Z; v points down).
float3 CubeDirection(uint face, float2 uv)
{
    const float2 p = uv * 2.0 - 1.0;
    switch (face) {
    case 0: return normalize(float3(1.0, -p.y, -p.x));
    case 1: return normalize(float3(-1.0, -p.y, p.x));
    case 2: return normalize(float3(p.x, 1.0, p.y));
    case 3: return normalize(float3(p.x, -1.0, -p.y));
    case 4: return normalize(float3(p.x, -p.y, 1.0));
    default: return normalize(float3(-p.x, -p.y, -1.0));
    }
}

float3 SkyRadiance(float3 dir)
{
    return GetTexture2D(g_Refl.input).SampleLevel(g_LinearClamp, SkyViewUV(dir), 0).rgb * g_Refl.skyScale;
}

// Sky-view LUT -> cube mip 0 (dispatch: size/8, size/8, 6).
[numthreads(8, 8, 1)]
void SkyToCubeCS(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= g_Refl.size))
        return;
    const float3 dir = CubeDirection(id.z, (float2(id.xy) + 0.5) / float(g_Refl.size));
    GetRWTexture2DArray(g_Refl.output)[id] = float4(min(SkyRadiance(dir), MaxHalf), 1);
}

// One captured face -> the raw capture cube; where nothing was drawn (depth 0) the sky fills in.
[numthreads(8, 8, 1)]
void CopyFaceCS(uint2 id : SV_DispatchThreadID)
{
    if (any(id >= g_Refl.size))
        return;
    const float d = GetTexture2D(g_Refl.depth).Load(int3(id, 0)).r;
    float3 c;
    if (d <= 0.0) {
        const float3 dir = CubeDirection(g_Refl.face, (float2(id) + 0.5) / float(g_Refl.size));
        c = g_Refl.sky != ZX_INVALID_INDEX ? GetTexture2D(g_Refl.sky).SampleLevel(g_LinearClamp, SkyViewUV(dir), 0).rgb * g_Refl.skyScale : 0;
    } else {
        c = GetTexture2D(g_Refl.input).Load(int3(id, 0)).rgb * g_Refl.exposureScale;
    }
    GetRWTexture2DArray(g_Refl.output)[uint3(id, g_Refl.face)] = float4(min(max(c, 0.0), MaxHalf), 1);
}

// --- GGX prefiltering (split sum, N = V = R) -------------------------------------------------------------------------

float2 Hammersley(uint i, uint n)
{
    uint bits = reversebits(i);
    return float2(float(i) / float(n), float(bits) * 2.3283064365386963e-10);
}

float3 ImportanceSampleGGX(float2 xi, float3 n, float roughness)
{
    const float a = roughness * roughness;
    const float phi = 2.0 * PI * xi.x;
    const float cosTheta = sqrt((1.0 - xi.y) / (1.0 + (a * a - 1.0) * xi.y));
    const float sinTheta = sqrt(1.0 - cosTheta * cosTheta);
    const float3 h = float3(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta);
    const float3 up = abs(n.y) < 0.999 ? float3(0, 1, 0) : float3(1, 0, 0);
    const float3 t = normalize(cross(up, n));
    const float3 b = cross(n, t);
    return normalize(t * h.x + b * h.y + n * h.z);
}

// Source cube (SRV, input) -> one mip of the prefiltered cube (UAV, output); dispatch size/8, size/8, 6.
[numthreads(8, 8, 1)]
void PrefilterCS(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= g_Refl.size))
        return;
    const float3 n = CubeDirection(id.z, (float2(id.xy) + 0.5) / float(g_Refl.size));
    TextureCube<float4> src = GetTextureCube(g_Refl.input);
    float3 acc = 0;
    float total = 0;
    if (g_Refl.roughness < 0.02) {
        acc = src.SampleLevel(g_LinearClamp, n, 0).rgb;
        total = 1;
    } else {
        for (uint i = 0; i < g_Refl.samples; ++i) {
            const float3 h = ImportanceSampleGGX(Hammersley(i, g_Refl.samples), n, g_Refl.roughness);
            const float3 l = 2.0 * dot(n, h) * h - n;
            const float nl = dot(n, l);
            if (nl > 0.0) {
                // Soft clamp of very bright texels (sun glints in captures) against sparkles at low sample counts.
                const float3 c = src.SampleLevel(g_LinearClamp, l, 0).rgb;
                acc += c / (1.0 + max(c.r, max(c.g, c.b)) * 0.0005) * nl;
                total += nl;
            }
        }
    }
    GetRWTexture2DArray(g_Refl.output)[id] = float4(acc / max(total, 1e-4), 1);
}

// --- Screen-space reflections ------------------------------------------------------------------------------------------

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
    const float4 p = mul(g_Refl.invViewProj, float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, d, 1.0));
    return p.xyz / p.w;
}

// Screen uv + linear depth (clip w) of a world point.
float3 Project(float3 p)
{
    const float4 c = mul(g_Refl.viewProj, float4(p, 1.0));
    return float3(c.x / c.w * 0.5 + 0.5, 0.5 - c.y / c.w * 0.5, c.w);
}

// Half resolution trace: rgb = reflected radiance (this frame's lit opaque scene), a = confidence. The forward pass
// uses it the next frame (reprojected), blending it over the probe / sky reflection.
[numthreads(8, 8, 1)]
void SsrTraceCS(uint2 id : SV_DispatchThreadID)
{
    if (any(id >= uint2(g_Refl.targetSize)))
        return;
    RWTexture2D<float4> output = GetRWTexture2D(g_Refl.output);
    const float2 uv = (float2(id) + 0.5) / g_Refl.targetSize;
    Texture2D<float4> depthTex = GetTexture2D(g_Refl.depth);
    const float d = depthTex.SampleLevel(g_PointClamp, uv, 0).r;
    const float4 nr = GetTexture2D(g_Refl.normals).SampleLevel(g_PointClamp, uv, 0);
    if (d <= 0.0 || nr.a < 0.5 || nr.b > g_Refl.maxRoughness) {
        output[id] = 0;
        return;
    }
    const float3 n = DecodeOct(nr.rg);
    const float3 p = WorldFromDepth(uv, d);
    const float3 v = normalize(p - g_Refl.cameraPosition);
    const float3 r = reflect(v, n);
    // Ray end: limited by the max distance and kept in front of the camera.
    float len = g_Refl.maxDistance;
    const float3 camFwd = g_Refl.cameraForward;
    const float startZ = dot(p - g_Refl.cameraPosition, camFwd);
    const float rz = dot(r, camFwd);
    if (rz < 0.0)
        len = min(len, (startZ - g_Refl.nearZ * 2.0) / -rz);
    if (len <= 0.05) {
        output[id] = 0;
        return;
    }
    const float3 s0 = Project(p + n * 0.01);
    const float3 s1 = Project(p + r * len);
    // March in screen space, interpolating 1/w (perspective-correct depth along the ray).
    const float steps = float(max(g_Refl.steps, 4u));
    const float noise = frac(52.9829189 * frac(dot(float2(id) + float(g_Refl.frame % 8) * 7.0, float2(0.06711056, 0.00583715))));
    float prevT = 0.0;
    float hitT = -1.0;
    for (float i = 1.0; i <= steps; i += 1.0) {
        const float t = (i - 1.0 + noise) / steps;
        const float2 suv = lerp(s0.xy, s1.xy, t);
        if (any(suv < 0.0) || any(suv > 1.0))
            break;
        const float rayZ = 1.0 / lerp(1.0 / s0.z, 1.0 / s1.z, t);
        const float sd = depthTex.SampleLevel(g_PointClamp, suv, 0).r;
        const float sceneZ = sd > 0.0 ? g_Refl.nearZ / sd : 1e9;
        const float diff = rayZ - sceneZ;
        if (diff > 0.0 && diff < g_Refl.thickness * (1.0 + sceneZ * 0.05)) {
            // Refine between the previous sample and this one.
            float lo = prevT, hi = t;
            [unroll] for (uint b = 0; b < 5; ++b) {
                const float mid = (lo + hi) * 0.5;
                const float2 muv = lerp(s0.xy, s1.xy, mid);
                const float mz = 1.0 / lerp(1.0 / s0.z, 1.0 / s1.z, mid);
                const float md = depthTex.SampleLevel(g_PointClamp, muv, 0).r;
                if (mz > (md > 0.0 ? g_Refl.nearZ / md : 1e9))
                    hi = mid;
                else
                    lo = mid;
            }
            hitT = hi;
            break;
        }
        prevT = t;
    }
    if (hitT < 0.0) {
        output[id] = 0;
        return;
    }
    const float2 huv = lerp(s0.xy, s1.xy, hitT);
    // The hit must face the ray (no reflections of back faces seen through thin walls).
    const float3 hn = DecodeOct(GetTexture2D(g_Refl.normals).SampleLevel(g_PointClamp, huv, 0).rg);
    const float facing = saturate(-dot(hn, r) * 4.0);
    const float2 edge = saturate(min(huv, 1.0 - huv) * 10.0); // fade at the screen border
    const float conf = edge.x * edge.y * facing * (1.0 - hitT * hitT) * saturate(1.0 - nr.b / g_Refl.maxRoughness);
    const float3 c = GetTexture2D(g_Refl.input).SampleLevel(g_LinearClamp, huv, 0).rgb;
    output[id] = float4(min(c, MaxHalf), saturate(conf));
}
