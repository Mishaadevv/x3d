// Contact shadows: a short ray march towards the sun through the depth buffer (after the depth pre-pass). Catches the
// small-scale occlusion the cascades are too coarse for (feet of furniture, stones on the ground, creases). The result
// multiplies the sun shadow in ForwardPBR; per-pixel jitter is resolved by TAA.

#include "Common/Bindless.hlsli"

struct ContactParams
{
    uint depth;       // SRV, reversed-Z infinite perspective
    uint output;      // UAV (R8, half the render size when halfResolution): 1 = lit
    uint frame;
    uint stepCount;
    float2 size;      // output size
    float2 invSize;   // 1 / render size
    float3 toSunView; // view space (x right, y up, z forward)
    float rayLength;  // meters
    float tanHalfX;
    float tanHalfY;
    float nearZ;
    float thickness;  // meters at 1 m (grows with distance)
    uint cascadeCount; // > 0: pixels the sun cascades already shadow skip the march
    uint halfResolution;
    uint2 pad;
    uint4 shadowMaps;
    float4x4 shadowFromView[4]; // view space -> cascade uv + depth
};
ZX_PASS_CONSTANTS(ContactParams, g_Contact);

/// 0 when the sun's cascades fully shadow view-space point `p` (2x2 comparison), else > 0 (or no cascade holds it).
float CascadeLight(float3 p)
{
    [loop] for (uint c = 0; c < g_Contact.cascadeCount; ++c) {
        const float4 h = mul(g_Contact.shadowFromView[c], float4(p, 1.0));
        const float3 uvz = h.xyz / h.w;
        if (all(uvz.xy > 0.01) && all(uvz.xy < 0.99) && uvz.z > 0.0 && uvz.z < 1.0)
            return GetTexture2D(g_Contact.shadowMaps[c]).SampleCmpLevelZero(g_ShadowSampler, uvz.xy, uvz.z);
    }
    return 1.0;
}

float ViewZ(float d) { return g_Contact.nearZ / max(d, 1e-7); }

float3 ViewPos(float2 uv, float d)
{
    const float z = ViewZ(d);
    return float3((uv.x * 2.0 - 1.0) * g_Contact.tanHalfX * z, (1.0 - uv.y * 2.0) * g_Contact.tanHalfY * z, z);
}

float2 ProjectUV(float3 p)
{
    return float2(p.x / (p.z * g_Contact.tanHalfX) * 0.5 + 0.5, 0.5 - p.y / (p.z * g_Contact.tanHalfY) * 0.5);
}

float InterleavedGradientNoise(float2 p) { return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715)))); }

// Beyond ~40 m the occluders contact shadows catch are under a pixel and the cascades cover the rest: fade them out
// (and skip the march) there.
static const float ContactFadeStart = 30.0, ContactFadeEnd = 45.0;

[numthreads(8, 8, 1)]
void ContactShadowsCS(uint2 id : SV_DispatchThreadID)
{
    if (any(id >= uint2(g_Contact.size)))
        return;
    RWTexture2D<float4> output = GetRWTexture2D(g_Contact.output);
    Texture2D<float4> depthTex = GetTexture2D(g_Contact.depth);
    // Half resolution: one pixel of each 2x2 block, a different one every frame (TAA blends the four).
    const uint2 pixel = g_Contact.halfResolution != 0 ? id * 2 + uint2(g_Contact.frame & 1u, (g_Contact.frame >> 1) & 1u) : id;
    const float2 uv = (float2(pixel) + 0.5) * g_Contact.invSize;
    const float d = depthTex.SampleLevel(g_PointClamp, uv, 0).r;
    if (d <= 0.0 || g_Contact.toSunView.z < -0.999) {
        output[id] = 1.0;
        return;
    }
    float3 P = ViewPos(uv, d);
    const float fade = 1.0 - saturate((P.z - ContactFadeStart) / (ContactFadeEnd - ContactFadeStart));
    if (fade <= 0.0) {
        output[id] = 1.0;
        return;
    }
    // Geometric normal from the depth neighbours (the smaller discontinuity, as in GTAO): the ray starts off the
    // surface, otherwise rays at grazing sun angles hit the pixel's own (bumpy) surface a few centimeters on.
    // Two gathers fetch the four neighbours: (x+1, y), (x, y+1) from the 2x2 block below-right, (x-1, y), (x, y-1) from
    // the one above-left (gather order: x = (u0, v1), y = (u1, v1), z = (u1, v0), w = (u0, v0)).
    const float2 t1 = g_Contact.invSize;
    const float4 g1 = depthTex.GatherRed(g_PointClamp, uv + 0.5 * t1);
    const float4 g2 = depthTex.GatherRed(g_PointClamp, uv - 0.5 * t1);
    const float3 px = ViewPos(uv + float2(t1.x, 0), g1.z);
    const float3 py = ViewPos(uv + float2(0, t1.y), g1.x);
    const float3 nx = ViewPos(uv - float2(t1.x, 0), g2.x);
    const float3 ny = ViewPos(uv - float2(0, t1.y), g2.z);
    const float3 dx = abs(px.z - P.z) < abs(P.z - nx.z) ? px - P : P - nx;
    const float3 dy = abs(py.z - P.z) < abs(P.z - ny.z) ? py - P : P - ny;
    float3 N = normalize(cross(dx, dy));
    if (dot(N, P) > 0.0)
        N = -N; // towards the camera
    const float NdotL = dot(N, g_Contact.toSunView);
    if (NdotL <= 0.0) {
        output[id] = 1.0; // facing away: the sun shadow / shading already darkens it
        return;
    }
    P += N * (0.01 + 0.004 * P.z);
    // Already in the cascades' shadow (tested a little towards the sun, past the surface's own depth bias): the
    // forward pass darkens it anyway, so the march would be wasted - often half the screen.
    if (g_Contact.cascadeCount > 0 && CascadeLight(P + g_Contact.toSunView * (0.15 + 0.01 * P.z)) <= 0.0) {
        output[id] = 1.0;
        return;
    }
    // Longer rays far away (a fixed world length shrinks below a pixel), capped so near surfaces stay local.
    const float rayLength = g_Contact.rayLength * clamp(P.z * 0.1, 1.0, 2.0);
    const float3 rayEnd = P + g_Contact.toSunView * rayLength;
    if (rayEnd.z <= g_Contact.nearZ) {
        output[id] = 1.0;
        return;
    }
    const float jitter = frac(InterleavedGradientNoise(float2(pixel)) + float(g_Contact.frame % 8) * 0.618034);
    const float thickness = g_Contact.thickness * max(P.z, 1.0);
    float occlusion = 0.0;
    // Far away the ray covers few pixels: half the steps.
    const uint steps = P.z > 15.0 ? max(g_Contact.stepCount / 2, 4u) : g_Contact.stepCount;
    for (uint s = 0; s < steps; ++s) {
        const float t = (s + jitter) / steps;
        const float3 q = lerp(P, rayEnd, t * t * 0.9 + t * 0.1); // denser near the pixel
        const float2 suv = ProjectUV(q);
        if (any(suv < 0.0) || any(suv > 1.0))
            break;
        const float sd = depthTex.SampleLevel(g_PointClamp, suv, 0).r;
        if (sd <= 0.0)
            continue;
        const float delta = q.z - ViewZ(sd); // > 0: the ray point is behind the surface seen there
        // Skip the start (self-intersection with the pixel's own surface) and thin-surface misses.
        if (delta > 0.02 * max(P.z * 0.05, 0.1) && delta < thickness) {
            // Fade at the screen edges and towards the end of the ray.
            const float2 edge = saturate(min(suv, 1.0 - suv) * 20.0);
            occlusion = max(occlusion, edge.x * edge.y * (1.0 - t * t));
            if (occlusion > 0.95)
                break;
        }
    }
    output[id] = 1.0 - occlusion * saturate(NdotL * 4.0) * fade;
}
