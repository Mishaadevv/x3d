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
    uint lightingVolume;   // volumetric fog: light per unit of scattering (invalid: none)
    uint integratedVolume; // in-scatter + transmittance from the camera
    float fogDistance;
    float fogSlices;
    uint sceneColor;   // distortion composite: the scene before
    uint distortion;   // distortion composite: screen-space offsets
    float2 invSize;
};
ZX_PASS_CONSTANTS(ParticleParams, g_P);

struct VSOut
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;            // -1..1 across the quad
    nointerpolation float4 color : COLOR0;
    nointerpolation float4 info : TEXCOORD1; // x = emissive, y = additive, z = shape, w = seed (flipbook: frame 0..1)
    nointerpolation uint3 flipbook : TEXCOORD5; // atlas SRV (invalid = procedural), columns, rows
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
    const uint flags = b.Load(offset + 36);
    const uint atlas = b.Load(offset + 40);
    const uint packedShape = b.Load(offset + 44);
    const uint shape = packedShape & 0xFF;
    const float emissive = (flags & 1u) ? 1.0 : 0.0;
    const float additive = (flags & 2u) ? 1.0 : 0.0;
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
            // Per emitter: the streak is the path covered in `stretch` seconds (like a camera exposure); 0 = the
            // classic look (proportional to the size: rain).
            const uint stretchBits = (flags >> 8) & 0xFF;
            if (stretchBits)
                extent.y += speed * (float(stretchBits) / 1000.0) * 0.5;
            else
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
        if (atlas == ZX_INVALID_INDEX)
            extent.y *= 1.6; // procedural tongues are tall; flipbook frames are drawn square
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
    o.flipbook = uint3(atlas, max((packedShape >> 8) & 0xFF, 1u), max((packedShape >> 16) & 0xFF, 1u));
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
    if (shape == 3)
        discard; // heat haze: drawn by HeatPS into the distortion target
    const float seed = i.info.w;
    float2 uv = i.uv;
    float mask;
    float3 tint = i.color.rgb;
    if (i.flipbook.x != ZX_INVALID_INDEX) {
        // Flipbook (baked from a fluid simulation): frame `seed` of a columns x rows atlas, blended with the next.
        const float count = float(i.flipbook.y * i.flipbook.z);
        const float f = frac(seed) * count;
        const float f0 = floor(f), f1 = fmod(f0 + 1.0, count);
        const float2 local = float2(uv.x * 0.5 + 0.5, 0.5 - uv.y * 0.5);
        const float2 cell = 1.0 / float2(i.flipbook.yz);
        const float2 uv0 = (float2(fmod(f0, float(i.flipbook.y)), floor(f0 / float(i.flipbook.y))) + local) * cell;
        const float2 uv1 = (float2(fmod(f1, float(i.flipbook.y)), floor(f1 / float(i.flipbook.y))) + local) * cell;
        Texture2D<float4> atlasTex = GetTexture2D(i.flipbook.x);
        const float4 s = lerp(atlasTex.SampleLevel(g_LinearClamp, uv0, 0), atlasTex.SampleLevel(g_LinearClamp, uv1, 0), f - f0);
        mask = s.a;
        if (shape == 1) {
            // Fire: r = temperature -> deep orange edges to a yellow-white core.
            const float peak = max(i.color.r, max(i.color.g, i.color.b));
            const float3 edge = i.color.rgb * float3(1.0, 0.45, 0.2);
            const float3 hot = float3(1.0, 0.78, 0.42) * peak * 1.2;
            tint = lerp(edge, hot, saturate(s.r * s.r)) * (0.35 + 0.65 * s.r);
        } else {
            tint = i.color.rgb * s.rgb; // smoke: baked self-shadowing
        }
    } else if (shape == 1) {
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
        // Spark / streak: a hot white core, the head brighter than the fading tail.
        const float r2 = uv.x * uv.x * 4.0 + uv.y * uv.y;
        mask = exp(-r2 * 5.0) * saturate(1.0 - r2) * lerp(0.35, 1.0, saturate(uv.y * 0.5 + 0.5));
        if (i.info.x > 0.5)
            tint = lerp(tint, max(tint.r, max(tint.g, tint.b)).xxx, saturate(exp(-uv.x * uv.x * 30.0) * 0.6)); // white-hot core
    } else if (i.info.y > 0.5) {
        // Glow (additive puff): a bright center with a soft halo, keeps its color instead of washing out to white.
        const float r2 = dot(uv, uv);
        mask = exp(-r2 * 4.0) * saturate(1.0 - r2) * 1.4;
        tint *= lerp(0.6, 1.0, exp(-r2 * 8.0));
    } else {
        // Billow: a round puff carved by three octaves of noise drifting through it.
        const float r2 = dot(uv, uv);
        const float2 q = uv * 1.6 + seed * 23.0;
        const float tq = g_P.time * 0.12;
        const float n = Noise2(q + tq) * 0.55 + Noise2(q * 2.1 - tq * 1.3 + 3.7) * 0.3 + Noise2(q * 4.3 + tq * 2.0 - 1.9) * 0.15;
        const float edge = saturate(1.0 - r2);
        mask = saturate(edge * edge * 1.6 - (1.0 - n) * 0.55 * (1.0 - edge * 0.5));
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
    const float bulge = sqrt(saturate(1.0 - dot(i.uv, i.uv)));
    const float3 n = normalize((i.uv.x * g_P.cameraRight + i.uv.y * g_P.cameraUp) - g_P.cameraForward * (bulge + 0.2));
    const float sunWrap = saturate(dot(n, g_P.dirToSun) * 0.6 + 0.4);
    // Thick middles shadow themselves; thin edges let the sun through (bright rims seen against the sun).
    const float thickness = saturate(mask * 1.4);
    const float selfShadow = lerp(1.0, 0.45, thickness * saturate(-dot(n, g_P.dirToSun) * 0.5 + 0.6));
    const float backlit = pow(saturate(dot(g_P.cameraForward, g_P.dirToSun)), 6.0) * (1.0 - thickness) * 1.5;
    const float skyFromAbove = 0.75 + 0.25 * n.y;
    float3 lit = tint * (g_P.ambient * skyFromAbove + g_P.sunLight * (sunWrap * selfShadow + backlit) * 0.9);
    // Volumetric fog: the cell's light (sun through the shadows, point lights, fires) lights the smoke too.
    float3 fogInscatter = 0.0;
    float fogTransmittance = 1.0;
    if (g_P.lightingVolume != ZX_INVALID_INDEX) {
        const float4 clip = mul(g_P.viewProj, float4(i.worldPos, 1.0));
        const float2 uv = float2(clip.x / clip.w * 0.5 + 0.5, 0.5 - clip.y / clip.w * 0.5);
        const float t = sqrt(saturate(i.viewDepth / g_P.fogDistance));
        if (t < 1.0 && clip.w > 0.0) {
            const float3 coord = float3(saturate(uv), max(t - 0.5 / g_P.fogSlices, 0.5 / g_P.fogSlices));
            const float3 L = GetTexture3D(g_P.lightingVolume).SampleLevel(g_LinearClamp, coord, 0).rgb;
            lit = lerp(lit, tint * L * 4.0 * 3.14159, saturate((1.0 - t) * 8.0)); // L carries the phase (1/4pi)
            const float4 f = GetTexture3D(g_P.integratedVolume).SampleLevel(g_LinearClamp, coord, 0);
            fogInscatter = f.rgb;
            fogTransmittance = f.a;
        }
    }
    const float3 glow = tint * g_P.emissiveScale;
    float3 rgb = lerp(lit, glow, i.info.x);
    rgb = rgb * fogTransmittance + fogInscatter; // fogged like the scene behind it
    // Additive particles don't cover what is behind them, except flames: dense enough to partly hide it (otherwise a
    // bright sky washes them out to beige).
    const float cover = shape == 1 ? max(1.0 - i.info.y, 0.6) : 1.0 - i.info.y;
    return float4(min(rgb * alpha, 60000.0), alpha * cover);
}

// Heat haze: invisible particles that bend the light behind them. HeatPS accumulates screen-space offsets (noise
// rising through each particle) into an RG16F target; DistortPS resamples the scene through them.
float4 HeatPS(VSOut i) : SV_Target0
{
    if (uint(i.info.z + 0.5) != 3)
        discard;
    const float r2 = dot(i.uv, i.uv);
    float mask = saturate(1.0 - r2);
    mask *= mask;
    const float sceneRaw = GetTexture2D(g_P.depth).Load(int3(i.position.xy, 0)).r;
    const float sceneDepth = sceneRaw > 0.0 ? g_P.nearPlane / sceneRaw : 1e9;
    const float soft = saturate((sceneDepth - i.viewDepth) / max(i.radius * 0.6, 0.02));
    const float nearFade = saturate((i.viewDepth - g_P.nearPlane) / max(i.radius, 0.1));
    const float2 p = i.uv * 2.5 + float2(i.info.w * 13.0, -g_P.time * 3.0);
    const float2 n = float2(Noise2(p) - 0.5, Noise2(p + 17.3) - 0.5) + float2(Noise2(p * 2.3 + 5.1) - 0.5, Noise2(p * 2.3 - 3.7) - 0.5) * 0.5;
    // Offset in screen uv: a few percent of the particle's size on screen at full strength.
    const float strength = i.color.a * mask * soft * nearFade * 0.06 * i.radius / max(i.viewDepth, 0.1);
    return float4(n * strength, 0.0, 0.0);
}

struct DistortOut
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

DistortOut DistortVS(uint vertexId : SV_VertexID)
{
    DistortOut o;
    o.uv = float2((vertexId << 1) & 2, vertexId & 2);
    o.position = float4(o.uv * float2(2, -2) + float2(-1, 1), 0.0, 1.0);
    return o;
}

float4 DistortPS(DistortOut i) : SV_Target0
{
    const float2 offset = GetTexture2D(g_P.distortion).Load(int3(i.position.xy, 0)).rg;
    return GetTexture2D(g_P.sceneColor).SampleLevel(g_LinearClamp, i.uv + offset, 0);
}
