// Ground-truth ambient occlusion (Jimenez et al. 2016), own implementation, from the depth buffer alone (normals are
// reconstructed from depth, so it runs right after the depth pre-pass of the forward renderer), plus a depth-aware blur.

#include "Common/Bindless.hlsli"

struct AOParams
{
    uint depth;          // SRV, reversed-Z infinite perspective
    uint output;         // UAV (R8)
    uint input;          // blur input SRV
    uint frame;
    float2 aoSize;       // AO target size
    float2 invAoSize;
    float tanHalfX;
    float tanHalfY;
    float nearZ;
    float radius;        // meters
    float maxRadiusPixels;
    float intensity;     // exponent
    uint sliceCount;
    uint stepCount;
};
ZX_PUSH_CONSTANTS(AOParams, g_AO);

static const float PI = 3.14159265;

float ViewZ(float d) { return g_AO.nearZ / max(d, 1e-7); }

float3 ViewPos(float2 uv, float d)
{
    const float z = ViewZ(d);
    return float3((uv.x * 2.0 - 1.0) * g_AO.tanHalfX * z, (1.0 - uv.y * 2.0) * g_AO.tanHalfY * z, z);
}

float SampleDepth(float2 uv) { return GetTexture2D(g_AO.depth).SampleLevel(g_PointClamp, uv, 0).r; }

float InterleavedGradientNoise(float2 p) { return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715)))); }

[numthreads(8, 8, 1)]
void GTAOCS(uint2 id : SV_DispatchThreadID)
{
    if (any(id >= uint2(g_AO.aoSize)))
        return;
    RWTexture2D<float4> output = GetRWTexture2D(g_AO.output);
    const float2 uv = (float2(id) + 0.5) * g_AO.invAoSize;
    const float d = SampleDepth(uv);
    if (d <= 0.0) {
        output[id] = 1.0; // sky
        return;
    }
    const float3 P = ViewPos(uv, d);
    // Normal from the neighbours with the smaller depth discontinuity (edges stay sharp).
    const float2 t = g_AO.invAoSize;
    const float3 px = ViewPos(uv + float2(t.x, 0), SampleDepth(uv + float2(t.x, 0)));
    const float3 nx = ViewPos(uv - float2(t.x, 0), SampleDepth(uv - float2(t.x, 0)));
    const float3 py = ViewPos(uv + float2(0, t.y), SampleDepth(uv + float2(0, t.y)));
    const float3 ny = ViewPos(uv - float2(0, t.y), SampleDepth(uv - float2(0, t.y)));
    const float3 dx = abs(px.z - P.z) < abs(P.z - nx.z) ? px - P : P - nx;
    const float3 dy = abs(py.z - P.z) < abs(P.z - ny.z) ? py - P : P - ny;
    float3 N = normalize(cross(dx, dy));
    const float3 V = normalize(-P);
    if (dot(N, V) < 0.0)
        N = -N;

    const float radiusPixels = min(g_AO.radius * g_AO.aoSize.y * 0.5 / (g_AO.tanHalfY * P.z), g_AO.maxRadiusPixels);
    if (radiusPixels < 1.0) {
        output[id] = 1.0;
        return;
    }
    const float noiseDir = InterleavedGradientNoise(float2(id));
    const float noiseStep = frac(noiseDir * 7.31 + 0.37);
    // Linear falloff over the outer 62 % of the radius (as in XeGTAO).
    const float falloffRange = 0.615 * g_AO.radius;
    const float falloffMul = -1.0 / falloffRange, falloffAdd = (g_AO.radius - falloffRange) / falloffRange + 1.0;
    float visibility = 0.0;
    for (uint s = 0; s < g_AO.sliceCount; ++s) {
        const float phi = (s + noiseDir) / g_AO.sliceCount * PI;
        const float2 dirScreen = float2(cos(phi), -sin(phi)); // pixels, y down
        const float3 dirView = float3(cos(phi), sin(phi), 0.0);
        const float3 orthoDir = dirView - dot(dirView, V) * V;
        const float3 axis = normalize(cross(dirView, V));
        const float3 projN = N - axis * dot(N, axis);
        const float projLen = length(projN);
        const float cosN = saturate(dot(projN, V) / max(projLen, 1e-4));
        const float nAngle = sign(dot(orthoDir, projN)) * acos(cosN);
        float horizonCos[2] = {-1.0, -1.0};
        [unroll] for (uint side = 0; side < 2; ++side) {
            const float sgn = side == 0 ? -1.0 : 1.0;
            for (uint j = 0; j < g_AO.stepCount; ++j) {
                const float stepT = (j + noiseStep) / g_AO.stepCount;
                const float2 offset = dirScreen * sgn * max(stepT * stepT * radiusPixels, j + 1.0); // denser near the pixel
                const float2 suv = uv + offset * t;
                const float sd = SampleDepth(suv);
                if (sd <= 0.0)
                    continue;
                const float3 D = ViewPos(suv, sd) - P;
                const float dist2 = dot(D, D);
                const float cosH = dot(D, V) * rsqrt(max(dist2, 1e-8));
                const float w = saturate(sqrt(dist2) * falloffMul + falloffAdd);
                horizonCos[side] = max(horizonCos[side], lerp(-1.0, cosH, w));
            }
        }
        float h0 = -acos(clamp(horizonCos[0], -1.0, 1.0));
        float h1 = acos(clamp(horizonCos[1], -1.0, 1.0));
        h0 = nAngle + max(h0 - nAngle, -PI * 0.5);
        h1 = nAngle + min(h1 - nAngle, PI * 0.5);
        const float sinN = sin(nAngle);
        const float arc0 = (cosN + 2.0 * h0 * sinN - cos(2.0 * h0 - nAngle)) * 0.25;
        const float arc1 = (cosN + 2.0 * h1 * sinN - cos(2.0 * h1 - nAngle)) * 0.25;
        visibility += projLen * (arc0 + arc1);
    }
    const float ao = pow(saturate(visibility / g_AO.sliceCount), g_AO.intensity);
    output[id] = ao;
}

// Depth-aware 4x4 blur (removes the per-pixel noise pattern).
[numthreads(8, 8, 1)]
void BlurCS(uint2 id : SV_DispatchThreadID)
{
    if (any(id >= uint2(g_AO.aoSize)))
        return;
    const float2 uv = (float2(id) + 0.5) * g_AO.invAoSize;
    const float zc = ViewZ(SampleDepth(uv));
    Texture2D<float4> input = GetTexture2D(g_AO.input);
    float sum = 0.0, weight = 0.0;
    [unroll] for (int y = -2; y < 2; ++y)
        [unroll] for (int x = -2; x < 2; ++x) {
            const float2 o = (float2(x, y) + 0.5) * g_AO.invAoSize;
            const float z = ViewZ(SampleDepth(uv + o));
            const float w = exp(-abs(z - zc) / (zc * 0.05 + 1e-3));
            sum += input.SampleLevel(g_PointClamp, uv + o, 0).r * w;
            weight += w;
        }
    GetRWTexture2D(g_AO.output)[id] = sum / max(weight, 1e-4);
}
