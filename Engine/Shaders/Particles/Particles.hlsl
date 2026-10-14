// Particles: camera-facing quads pulled from an upload buffer (64-byte GpuParticle, see Render/Particles.h),
// procedural shapes, premultiplied output (additive particles write alpha 0, flames partly cover the background), soft
// depth fade against the scene.

#include "Common/Bindless.hlsli"

struct ParticleParams
{
    float4x4 viewProj;
    float3 cameraRight;
    uint particles;
    float3 cameraUp;
    uint depth;
    float3 cameraPosition;
    float nearPlane;
    float3 ambient;
    float time;
    float3 sunLight;
    float emissiveScale;
    float3 dirToSun;
    float pad0;
    float3 cameraForward;
    float pad1;
};
ZX_PASS_CONSTANTS(ParticleParams, g_P);

struct VSOut
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;            // -1..1 across the quad
    nointerpolation float4 color : COLOR0;
    nointerpolation float4 info : TEXCOORD1; // x = emissive, y = additive, z = shape, w = seed
    float viewDepth : TEXCOORD2;
    nointerpolation float radius : TEXCOORD3;
    float3 worldPos : TEXCOORD4;
};

VSOut ParticleVS(uint vertexId : SV_VertexID)
{
    ByteAddressBuffer b = GetBuffer(g_P.particles);
    const uint offset = (vertexId / 6) * 64;
    static const float2 Corners[6] = {float2(-1, -1), float2(-1, 1), float2(1, 1), float2(-1, -1), float2(1, 1), float2(1, -1)};
    const float2 corner = Corners[vertexId % 6];
    const float3 center = asfloat(b.Load3(offset));
    const float size = asfloat(b.Load(offset + 12));
    const float4 color = asfloat(b.Load4(offset + 16));
    const float rotation = asfloat(b.Load(offset + 32));
    const float emissive = asfloat(b.Load(offset + 36));
    const float additive = asfloat(b.Load(offset + 40));
    const uint shape = b.Load(offset + 44);
    const float3 velocity = asfloat(b.Load3(offset + 48));
    const float seed = asfloat(b.Load(offset + 60));

    float3 right = g_P.cameraRight, up = g_P.cameraUp;
    float2 extent = size * 0.5;
    if (shape == 2) {
        // Sparks: long side along the velocity as seen from the camera.
        const float3 toCam = normalize(g_P.cameraPosition - center);
        const float3 along = velocity - toCam * dot(velocity, toCam);
        const float speed = length(along);
        if (speed > 1e-3) {
            up = along / speed;
            right = normalize(cross(up, toCam));
            extent.y *= 1.0 + speed * 0.08;
        }
    } else if (shape == 1) {
        // Flames: upright tongues (cylindrical billboards), leaning a little with their sideways motion and swaying.
        const float3 toCam = normalize(g_P.cameraPosition - center);
        const float3 worldUp = normalize(float3(0, 1, 0) + float3(velocity.x, 0, velocity.z) * 0.15 + float3(sin(rotation), 0, cos(rotation)) * 0.12);
        float3 u = worldUp - toCam * dot(worldUp, toCam);
        if (dot(u, u) < 1e-4)
            u = g_P.cameraUp; // looking straight down at it
        up = normalize(u);
        right = normalize(cross(up, toCam));
        extent.y *= 1.6;
    } else {
        float s, c;
        sincos(rotation, s, c);
        const float3 r = right * c + up * s;
        up = up * c - right * s;
        right = r;
    }
    const float3 world = center + right * (corner.x * extent.x) + up * (corner.y * extent.y);
    VSOut o;
    o.position = mul(g_P.viewProj, float4(world, 1.0));
    o.uv = corner;
    o.color = color;
    o.info = float4(emissive, additive, float(shape), seed);
    o.viewDepth = dot(world - g_P.cameraPosition, g_P.cameraForward);
    o.radius = size * 0.5;
    o.worldPos = world;
    return o;
}

float Hash21(float2 p) { return frac(sin(dot(p, float2(127.1, 311.7))) * 43758.5453); }

float Noise2(float2 p)
{
    const float2 i = floor(p);
    const float2 f = frac(p);
    const float2 u = f * f * (3.0 - 2.0 * f);
    return lerp(lerp(Hash21(i), Hash21(i + float2(1, 0)), u.x), lerp(Hash21(i + float2(0, 1)), Hash21(i + float2(1, 1)), u.x), u.y);
}

float4 ParticlePS(VSOut i) : SV_Target0
{
    const uint shape = uint(i.info.z + 0.5);
    const float seed = i.info.w;
    float2 uv = i.uv;
    float mask;
    float3 tint = i.color.rgb;
    if (shape == 1) {
        // Flame: a tongue (round at the bottom, licking up to a point), torn by noise rising through it. Hot core
        // (yellow-white) to cooler edges and tip (deep orange-red), like a real flame.
        const float t = g_P.time * 2.6 + seed * 37.0;
        const float n = Noise2(float2(uv.x * 2.0 + seed * 11.0, uv.y * 1.4 - t)) * 0.6 + Noise2(float2(uv.x * 4.5 - seed * 5.0, uv.y * 3.6 - t * 1.8)) * 0.4;
        const float h = saturate(uv.y * 0.5 + 0.5); // 0 bottom .. 1 top
        const float width = lerp(0.95, 0.22, pow(h, 0.8)) * (0.85 + 0.3 * n);
        const float r = length(float2(uv.x / max(width, 0.05), (uv.y + 0.35) * 0.75));
        mask = saturate((1.0 - r) * 2.0 - (n - 0.5) * 1.2);
        const float core = saturate((mask - 0.35) * 1.8) * (1.0 - h * 0.6);
        mask = mask * mask * (3.0 - 2.0 * mask) * (1.0 - h * h * 0.6);
        const float peak = max(i.color.r, max(i.color.g, i.color.b));
        const float3 edge = i.color.rgb * float3(1.0, 0.5, 0.25);
        const float3 hot = float3(1.0, 0.75, 0.38) * peak * 1.15;
        tint = lerp(edge, hot, core * core);
    } else if (shape == 2) {
        const float r2 = dot(uv, uv);
        mask = exp(-r2 * 6.0) * saturate(1.0 - r2);
    } else {
        // Puff: smooth falloff broken up by slow noise.
        const float r2 = dot(uv, uv);
        const float n = Noise2(uv * 2.1 + seed * 23.0 + g_P.time * 0.15) * 0.5 + Noise2(uv * 4.3 - seed * 7.0) * 0.5;
        mask = saturate(1.0 - r2) * saturate(1.0 - r2);
        mask *= lerp(0.55, 1.15, n);
    }
    if (mask <= 0.002)
        discard;

    // Depth: hidden behind geometry, fading out where it gets close (soft particles) and near the camera.
    const float sceneRaw = GetTexture2D(g_P.depth).Load(int3(i.position.xy, 0)).r;
    const float sceneDepth = sceneRaw > 0.0 ? g_P.nearPlane / sceneRaw : 1e9;
    const float soft = saturate((sceneDepth - i.viewDepth) / max(i.radius * 0.6, 0.02));
    const float nearFade = saturate((i.viewDepth - g_P.nearPlane) / max(i.radius, 0.1));
    const float alpha = saturate(i.color.a * mask * soft * nearFade);
    if (alpha <= 0.001)
        discard;

    // Lit (smoke): sky light plus the sun, brighter on the side facing it; emissive (fire, sparks): glows.
    const float3 n = normalize(float3(i.uv.x * g_P.cameraRight + i.uv.y * g_P.cameraUp) * 0.6 - g_P.cameraForward);
    const float sunWrap = saturate(dot(n, g_P.dirToSun) * 0.5 + 0.5);
    const float3 lit = tint * (g_P.ambient + g_P.sunLight * (0.35 + 0.65 * sunWrap));
    const float3 glow = tint * g_P.emissiveScale;
    const float3 rgb = lerp(lit, glow, i.info.x);
    // Additive particles don't cover what is behind them, except flames: dense enough to partly hide it (otherwise a
    // bright sky washes them out to beige).
    const float cover = shape == 1 ? max(1.0 - i.info.y, 0.6) : 1.0 - i.info.y;
    return float4(min(rgb * alpha, 60000.0), alpha * cover);
}
