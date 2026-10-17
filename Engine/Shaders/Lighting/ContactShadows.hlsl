// Contact shadows: a short ray march towards the sun through the depth buffer (after the depth pre-pass). Catches the
// small-scale occlusion the cascades are too coarse for (feet of furniture, stones on the ground, creases). The result
// multiplies the sun shadow in ForwardPBR; per-pixel jitter is resolved by TAA.

#include "Common/Bindless.hlsli"

struct ContactParams
{
    uint depth;       // SRV, reversed-Z infinite perspective
    uint output;      // UAV (R8): 1 = lit
    uint frame;
    uint stepCount;
    float2 size;      // render size
    float2 invSize;
    float3 toSunView; // view space (x right, y up, z forward)
    float rayLength;  // meters
    float tanHalfX;
    float tanHalfY;
    float nearZ;
    float thickness;  // meters at 1 m (grows with distance)
};
ZX_PUSH_CONSTANTS(ContactParams, g_Contact);

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

[numthreads(8, 8, 1)]
void ContactShadowsCS(uint2 id : SV_DispatchThreadID)
{
    if (any(id >= uint2(g_Contact.size)))
        return;
    RWTexture2D<float4> output = GetRWTexture2D(g_Contact.output);
    Texture2D<float4> depthTex = GetTexture2D(g_Contact.depth);
    const float2 uv = (float2(id) + 0.5) * g_Contact.invSize;
    const float d = depthTex.SampleLevel(g_PointClamp, uv, 0).r;
    if (d <= 0.0 || g_Contact.toSunView.z < -0.999) {
        output[id] = 1.0;
        return;
    }
    float3 P = ViewPos(uv, d);
    // Geometric normal from the depth neighbours (the smaller discontinuity, as in GTAO): the ray starts off the
    // surface, otherwise rays at grazing sun angles hit the pixel's own (bumpy) surface a few centimeters on.
    const float2 t1 = g_Contact.invSize;
    const float3 px = ViewPos(uv + float2(t1.x, 0), depthTex.SampleLevel(g_PointClamp, uv + float2(t1.x, 0), 0).r);
    const float3 nx = ViewPos(uv - float2(t1.x, 0), depthTex.SampleLevel(g_PointClamp, uv - float2(t1.x, 0), 0).r);
    const float3 py = ViewPos(uv + float2(0, t1.y), depthTex.SampleLevel(g_PointClamp, uv + float2(0, t1.y), 0).r);
    const float3 ny = ViewPos(uv - float2(0, t1.y), depthTex.SampleLevel(g_PointClamp, uv - float2(0, t1.y), 0).r);
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
    // Longer rays far away (a fixed world length shrinks below a pixel), capped so near surfaces stay local.
    const float rayLength = g_Contact.rayLength * clamp(P.z * 0.1, 1.0, 2.0);
    const float3 rayEnd = P + g_Contact.toSunView * rayLength;
    if (rayEnd.z <= g_Contact.nearZ) {
        output[id] = 1.0;
        return;
    }
    const float jitter = frac(InterleavedGradientNoise(float2(id)) + float(g_Contact.frame % 8) * 0.618034);
    const float thickness = g_Contact.thickness * max(P.z, 1.0);
    float occlusion = 0.0;
    for (uint s = 0; s < g_Contact.stepCount; ++s) {
        const float t = (s + jitter) / g_Contact.stepCount;
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
    output[id] = 1.0 - occlusion * saturate(NdotL * 4.0);
}
