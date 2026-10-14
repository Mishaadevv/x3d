// Real-time smoke and fire: stable fluids (Stam 1999) on a 3D grid in a box, on the GPU. Each step: advect the
// velocity (semi-Lagrangian, trilinear), add buoyancy (hot gas rises, dense smoke sinks), vorticity confinement (keeps
// the small swirls numerical diffusion would smear out) and the emitter's push; then make the velocity divergence-free
// (Jacobi pressure solve + gradient subtraction) and carry smoke density and temperature along it while they fade.
// Rendering raymarches the box: smoke absorbs and scatters the sun and sky, hot gas glows (blackbody colour).
//
// Volumes (RGBA16F unless noted): velocity xyz; scalars r = smoke density, g = temperature (0 = ambient, 1 = flame);
// pressure / divergence r (R16F); curl xyz.

#include "Common/Bindless.hlsli"

struct FluidParams
{
    uint3 grid;
    float dt;
    float3 sourcePosition;   // grid cells
    float sourceRadius;      // grid cells
    float3 sourceVelocity;   // cells per second
    float sourceDensity;     // per second
    float sourceTemperature; // per second
    float buoyancy;          // upward acceleration per unit temperature (cells / s^2)
    float weight;            // downward acceleration per unit density
    float vorticity;         // confinement strength
    float densityFade;       // fraction lost per second
    float temperatureFade;
    float velocityFade;
    float time;
    float noise;             // turbulence of the source
    uint velocityIn;
    uint velocityOut;
    uint scalarIn;
    uint scalarOut;
    uint pressureIn;
    uint pressureOut;
    uint divergence;
    uint curl;
    float pad0;
    float pad1;
};
ZX_PASS_CONSTANTS(FluidParams, g_F);

float3 CellUvw(float3 cell) { return (cell + 0.5) / float3(g_F.grid); }

float4 SampleField(uint texture, float3 cell) { return GetTexture3D(texture).SampleLevel(g_LinearClamp, CellUvw(cell), 0); }

float4 LoadField(uint texture, int3 c)
{
    return GetTexture3D(texture).Load(int4(clamp(c, int3(0, 0, 0), int3(g_F.grid) - 1), 0));
}

/// Pressure with open sides and top (0 outside: gas flows out freely) and a solid floor (mirrored).
float LoadPressure(int3 c)
{
    if (c.y < 0)
        c.y = 0;
    if (any(c < 0) || any(c >= int3(g_F.grid)))
        return 0.0;
    return GetTexture3D(g_F.pressureIn).Load(int4(c, 0)).r;
}

float Hash(float3 p)
{
    p = frac(p * 0.1031);
    p += dot(p, p.yzx + 33.33);
    return frac((p.x + p.y) * p.z);
}

[numthreads(4, 4, 4)]
void CurlCS(uint3 id : SV_DispatchThreadID)
{
    if (any(id >= g_F.grid))
        return;
    const int3 c = int3(id);
    const float3 l = LoadField(g_F.velocityIn, c - int3(1, 0, 0)).xyz, r = LoadField(g_F.velocityIn, c + int3(1, 0, 0)).xyz;
    const float3 d = LoadField(g_F.velocityIn, c - int3(0, 1, 0)).xyz, u = LoadField(g_F.velocityIn, c + int3(0, 1, 0)).xyz;
    const float3 b = LoadField(g_F.velocityIn, c - int3(0, 0, 1)).xyz, f = LoadField(g_F.velocityIn, c + int3(0, 0, 1)).xyz;
    const float3 curl = 0.5 * float3((u.z - d.z) - (f.y - b.y), (f.x - b.x) - (r.z - l.z), (r.y - l.y) - (u.x - d.x));
    GetRWTexture3D(g_F.curl)[id] = float4(curl, 0.0);
}

[numthreads(4, 4, 4)]
void AdvectVelocityCS(uint3 id : SV_DispatchThreadID)
{
    if (any(id >= g_F.grid))
        return;
    const float3 cell = float3(id);
    const float3 v0 = LoadField(g_F.velocityIn, int3(id)).xyz;
    float3 v = SampleField(g_F.velocityIn, cell - v0 * g_F.dt).xyz * (1.0 - g_F.velocityFade * g_F.dt);
    // Buoyancy: hot gas rises, smoke weighs it down.
    const float2 s = LoadField(g_F.scalarIn, int3(id)).rg;
    v.y += (g_F.buoyancy * s.g - g_F.weight * s.r) * g_F.dt;
    // Vorticity confinement: push along N x curl, N = gradient of |curl| (towards the swirl's centre).
    if (g_F.vorticity > 0.0) {
        const int3 c = int3(id);
        const float cl = length(LoadField(g_F.curl, c - int3(1, 0, 0)).xyz), cr = length(LoadField(g_F.curl, c + int3(1, 0, 0)).xyz);
        const float cd = length(LoadField(g_F.curl, c - int3(0, 1, 0)).xyz), cu = length(LoadField(g_F.curl, c + int3(0, 1, 0)).xyz);
        const float cb = length(LoadField(g_F.curl, c - int3(0, 0, 1)).xyz), cf = length(LoadField(g_F.curl, c + int3(0, 0, 1)).xyz);
        float3 n = float3(cr - cl, cu - cd, cf - cb);
        const float len = length(n);
        if (len > 1e-5) {
            n /= len;
            v += cross(n, LoadField(g_F.curl, c).xyz) * (g_F.vorticity * g_F.dt);
        }
    }
    // Emitter: pushes gas out of its sphere, with noise so the plume breaks up.
    const float d = distance(cell, g_F.sourcePosition);
    if (d < g_F.sourceRadius) {
        const float w = 1.0 - d / g_F.sourceRadius;
        const float3 jitter = float3(Hash(cell + g_F.time * 13.1), Hash(cell.zxy - g_F.time * 7.7), Hash(cell.yzx + g_F.time * 3.3)) - 0.5;
        v = lerp(v, g_F.sourceVelocity + jitter * g_F.noise * length(g_F.sourceVelocity), saturate(w * 4.0 * g_F.dt * 10.0));
    }
    // Solid floor; the sides and the top are open air.
    if (id.y == 0) v.y = max(v.y, 0.0);
    GetRWTexture3D(g_F.velocityOut)[id] = float4(v, 0.0);
}

[numthreads(4, 4, 4)]
void DivergenceCS(uint3 id : SV_DispatchThreadID)
{
    if (any(id >= g_F.grid))
        return;
    const int3 c = int3(id);
    const float div = 0.5 * ((LoadField(g_F.velocityIn, c + int3(1, 0, 0)).x - LoadField(g_F.velocityIn, c - int3(1, 0, 0)).x) +
                             (LoadField(g_F.velocityIn, c + int3(0, 1, 0)).y - LoadField(g_F.velocityIn, c - int3(0, 1, 0)).y) +
                             (LoadField(g_F.velocityIn, c + int3(0, 0, 1)).z - LoadField(g_F.velocityIn, c - int3(0, 0, 1)).z));
    GetRWTexture3D(g_F.divergence)[id] = float4(div, 0, 0, 0);
}

[numthreads(4, 4, 4)]
void JacobiCS(uint3 id : SV_DispatchThreadID)
{
    if (any(id >= g_F.grid))
        return;
    const int3 c = int3(id);
    const float sum = LoadPressure(c - int3(1, 0, 0)) + LoadPressure(c + int3(1, 0, 0)) + LoadPressure(c - int3(0, 1, 0)) +
                      LoadPressure(c + int3(0, 1, 0)) + LoadPressure(c - int3(0, 0, 1)) + LoadPressure(c + int3(0, 0, 1));
    const float p = (sum - LoadField(g_F.divergence, c).r) / 6.0;
    GetRWTexture3D(g_F.pressureOut)[id] = float4(p, 0, 0, 0);
}

[numthreads(4, 4, 4)]
void ProjectCS(uint3 id : SV_DispatchThreadID)
{
    if (any(id >= g_F.grid))
        return;
    const int3 c = int3(id);
    const float3 grad = 0.5 * float3(LoadPressure(c + int3(1, 0, 0)) - LoadPressure(c - int3(1, 0, 0)),
                                     LoadPressure(c + int3(0, 1, 0)) - LoadPressure(c - int3(0, 1, 0)),
                                     LoadPressure(c + int3(0, 0, 1)) - LoadPressure(c - int3(0, 0, 1)));
    const float3 v = LoadField(g_F.velocityIn, c).xyz - grad;
    GetRWTexture3D(g_F.velocityOut)[id] = float4(v, 0.0);
}

[numthreads(4, 4, 4)]
void AdvectScalarsCS(uint3 id : SV_DispatchThreadID)
{
    if (any(id >= g_F.grid))
        return;
    const float3 cell = float3(id);
    const float3 v = LoadField(g_F.velocityIn, int3(id)).xyz;
    float2 s = SampleField(g_F.scalarIn, cell - v * g_F.dt).rg;
    s.r *= 1.0 - saturate(g_F.densityFade * g_F.dt);
    s.g *= 1.0 - saturate(g_F.temperatureFade * g_F.dt);
    const float d = distance(cell, g_F.sourcePosition);
    if (d < g_F.sourceRadius) {
        const float w = 1.0 - d / g_F.sourceRadius;
        const float flicker = 0.6 + 0.8 * Hash(floor(cell * 0.5) + floor(g_F.time * 8.0));
        s.r += g_F.sourceDensity * w * g_F.dt * flicker;
        s.g = max(s.g, saturate(g_F.sourceTemperature * w * flicker));
    }
    // Gas reaching the open sides and top leaves the box: fade it out over the last cells (no pile-up at the edge).
    const float3 edge = min(cell, float3(g_F.grid) - 1.0 - cell);
    const float wall = saturate(min(edge.x, min(edge.z, float(g_F.grid.y) - 1.0 - cell.y)) / 4.0);
    s *= lerp(0.6, 1.0, wall);
    GetRWTexture3D(g_F.scalarOut)[id] = float4(max(s, 0.0), 0, 0);
}

// --- Rendering ---------------------------------------------------------------------------------------------------

struct FluidDrawParams
{
    float4x4 invViewProj;    // clip (no jitter) -> world
    float4x4 worldToVolume;  // world -> volume uvw (0..1 box)
    float3 cameraPosition;
    float densityScale;      // extinction per unit density (1/m)
    float3 dirToSun;
    float nearPlane;
    float3 sunColor;         // pre-exposed illuminance
    float fireIntensity;     // nits at temperature 1 (x exposure applied on the CPU)
    float3 ambient;          // pre-exposed sky light
    float stepLength;        // world meters per raymarch step
    float3 smokeColor;       // albedo
    uint scalars;
    float3 volumeSize;       // world size of the box
    uint depthTexture;
    float2 renderSize;
    float fireTemperature;   // temperature above which gas glows
    uint steps;
};
ZX_PASS_CONSTANTS(FluidDrawParams, g_D);

/// Blackbody-ish ramp: deep red -> orange -> yellow -> white as the gas heats up (0..1).
float3 FireColor(float t)
{
    const float3 a = float3(1.0, 0.18, 0.02), b = float3(1.0, 0.55, 0.12), c = float3(1.0, 0.9, 0.6);
    return t < 0.5 ? lerp(a, b, t * 2.0) : lerp(b, c, (t - 0.5) * 2.0);
}

struct DrawOut
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

DrawOut FluidVS(uint vertexId : SV_VertexID)
{
    DrawOut o;
    o.uv = float2((vertexId << 1) & 2, vertexId & 2);
    o.position = float4(o.uv * float2(2, -2) + float2(-1, 1), 0.0, 1.0);
    return o;
}

float4 FluidPS(DrawOut i) : SV_Target0
{
    const float4 p = mul(g_D.invViewProj, float4(i.uv.x * 2.0 - 1.0, 1.0 - i.uv.y * 2.0, 0.5, 1.0));
    const float3 dir = normalize(p.xyz / p.w - g_D.cameraPosition);
    // Ray vs the box in volume space.
    const float3 o = mul(g_D.worldToVolume, float4(g_D.cameraPosition, 1.0)).xyz;
    const float3 d = mul((float3x3)g_D.worldToVolume, dir);
    const float3 inv = 1.0 / select(abs(d) > 1e-6, d, 1e-6);
    const float3 t0 = (0.0 - o) * inv, t1 = (1.0 - o) * inv;
    const float tEnter = max(max(min(t0.x, t1.x), min(t0.y, t1.y)), max(min(t0.z, t1.z), 0.0));
    float tExit = min(min(max(t0.x, t1.x), max(t0.y, t1.y)), max(t0.z, t1.z));
    if (tExit <= tEnter)
        discard;
    // Scene depth: stop at opaque geometry (t is in world meters because worldToVolume scales dir).
    const float raw = GetTexture2D(g_D.depthTexture).Load(int3(i.position.xy, 0)).r;
    if (raw > 0.0) {
        const float4 q = mul(g_D.invViewProj, float4(i.uv.x * 2.0 - 1.0, 1.0 - i.uv.y * 2.0, raw, 1.0));
        tExit = min(tExit, distance(q.xyz / q.w, g_D.cameraPosition));
    }
    float3 color = 0.0;
    float transmittance = 1.0;
    const float step = g_D.stepLength;
    float t = tEnter + step * frac(sin(dot(i.position.xy, float2(12.9898, 78.233))) * 43758.5453);
    [loop] for (uint s = 0; s < g_D.steps && t < tExit && transmittance > 0.01; ++s, t += step) {
        const float3 uvw = o + d * t;
        const float2 v = GetTexture3D(g_D.scalars).SampleLevel(g_LinearClamp, uvw, 0).rg;
        const float sigma = v.r * g_D.densityScale;
        const float heat = saturate((v.g - g_D.fireTemperature) / max(1.0 - g_D.fireTemperature, 1e-3));
        if (sigma <= 1e-4 && heat <= 0.0)
            continue;
        // Self-shadowing towards the sun: two samples.
        const float3 sunDir = normalize(mul((float3x3)g_D.worldToVolume, g_D.dirToSun));
        float shadowDepth = 0.0;
        [unroll] for (uint k = 1; k <= 2; ++k)
            shadowDepth += GetTexture3D(g_D.scalars).SampleLevel(g_LinearClamp, uvw + sunDir * (0.08 * k), 0).r;
        const float sunVis = exp(-shadowDepth * g_D.densityScale * length(g_D.volumeSize) * 0.08);
        const float3 lit = g_D.smokeColor * (g_D.sunColor * (sunVis * 0.25 / 3.14159) + g_D.ambient * 0.5);
        // Flames: light given off per meter, strongest where the gas is hottest.
        const float3 glow = FireColor(heat) * (heat * heat * g_D.fireIntensity);
        const float tStep = exp(-sigma * step);
        color += transmittance * (lit * (1.0 - tStep) + glow * step);
        transmittance *= tStep;
    }
    // Premultiplied over the scene (blend: src One, dst SrcAlpha).
    return float4(min(color, 60000.0), transmittance);
}
