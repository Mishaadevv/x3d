// GPU particle simulation (Particle Emitter with GPU Simulation). Particles live in two state buffers used in turn:
// each simulated frame
//   EmitCS     - new particles (spawn ranges per emitter from the CPU) are appended to the current state buffer
//   SimulateCS - every particle of the current buffer is aged, moved and collided (against the depth buffer: they hit
//                what the camera sees); survivors are appended to the other buffer (compacted), each with a
//                GpuParticle record (Render/Particles.h) at the same index for the regular particle shaders
//   ArgsCS     - indirect draw arguments (6 vertices per particle), empties the consumed buffer
// Reads and writes are sequential: a pool with a free list scattered the particles and its 64-byte writes dominated.
// InitCS empties both buffers (scene change, Play start / stop).

#include "Common/Bindless.hlsli"

struct SimParams
{
    float4x4 viewProj;
    float4x4 invViewProj;
    uint stateIn;   // UAV: particles of this frame (Emit appends here)
    uint stateOut;  // UAV: survivors
    uint counters;  // UAV: particle count of state buffer [0] and [1], 64 bytes apart
    uint render;    // UAV: GpuParticle x capacity
    uint emitters;  // SRV: GpuEmitter rows (indexed by emitter slot)
    uint args;      // UAV: IndirectDrawArgsPlain
    uint emitterCount;
    uint totalSpawn;
    uint capacity;
    uint depth;     // scene depth SRV (reversed-Z infinite) or invalid: no collision
    uint inCounter;  // byte offset of the stateIn count
    uint outCounter; // byte offset of the stateOut count
    float3 cameraPosition;
    float nearPlane;
    float2 depthSize;
    float2 pad;
};
ZX_PASS_CONSTANTS(SimParams, g_S);

#define RENDER_STRIDE 64
#define EMITTER_STRIDE 256

// Emitter flags.
#define EMITTER_ACTIVE 1u
#define EMITTER_WORLD_SPACE 2u
#define EMITTER_BOUNCE 4u
#define EMITTER_DIE 8u

struct SimParticle
{
    float3 position; float age;
    float3 velocity; float life;
    float sizeScale, rotation, spin, seed;
    uint emitter; // slot | generation << 16
};

// Structure of arrays: four float4 planes of `capacity` entries, so a wave reads and writes whole cache lines.
uint Plane(uint k, uint index) { return (k * g_S.capacity + index) * 16; }

SimParticle LoadParticle(uint buffer, uint index)
{
    RWByteAddressBuffer b = GetRWBuffer(buffer);
    SimParticle p;
    float4 v = asfloat(b.Load4(Plane(0, index))); p.position = v.xyz; p.age = v.w;
    v = asfloat(b.Load4(Plane(1, index))); p.velocity = v.xyz; p.life = v.w;
    v = asfloat(b.Load4(Plane(2, index))); p.sizeScale = v.x; p.rotation = v.y; p.spin = v.z; p.seed = v.w;
    p.emitter = b.Load(Plane(3, index));
    return p;
}

void StoreParticle(uint buffer, uint index, SimParticle p)
{
    RWByteAddressBuffer b = GetRWBuffer(buffer);
    b.Store4(Plane(0, index), asuint(float4(p.position, p.age)));
    b.Store4(Plane(1, index), asuint(float4(p.velocity, p.life)));
    b.Store4(Plane(2, index), asuint(float4(p.sizeScale, p.rotation, p.spin, p.seed)));
    b.Store4(Plane(3, index), uint4(p.emitter, 0, 0, 0));
}

// PCG hash -> uniform [0, 1).
uint Pcg(uint v)
{
    const uint state = v * 747796405u + 2891336453u;
    const uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}
struct Rng
{
    uint s;
    float Next()
    {
        s = Pcg(s);
        return float(s >> 8) * (1.0 / 16777216.0);
    }
    float Range(float a, float b) { return lerp(a, b, Next()); }
};

static const float TWO_PI = 6.2831853;

float3 OnUnitSphere(inout Rng r)
{
    const float z = r.Range(-1.0, 1.0), a = r.Next() * TWO_PI;
    const float s = sqrt(max(1.0 - z * z, 0.0));
    return float3(s * cos(a), s * sin(a), z);
}

/// Same field as the CPU simulation (Particles.cpp Swirl).
float3 Swirl(float3 p, float t)
{
    return float3(sin(p.y * 1.7 + t * 1.3) + sin(p.z * 2.3 - t * 0.7), sin(p.z * 1.9 + t * 1.1) + sin(p.x * 2.1 + t * 0.9) * 0.5,
                  sin(p.x * 1.5 - t * 1.2) + sin(p.y * 2.7 + t * 0.8)) * 0.5;
}

/// Appends the active lanes to a list counter at `offset` with one atomic per wave; returns this lane's slot.
uint WaveAppend(RWByteAddressBuffer counters, uint offset)
{
    const uint count = WaveActiveCountBits(true);
    uint base = 0;
    if (WaveIsFirstLane())
        counters.InterlockedAdd(offset, count, base);
    return WaveReadLaneFirst(base) + WavePrefixCountBits(true);
}

[numthreads(1, 1, 1)]
void InitCS()
{
    RWByteAddressBuffer c = GetRWBuffer(g_S.counters);
    c.Store(0, 0u);
    c.Store(64, 0u);
    GetRWBuffer(g_S.args).Store4(0, uint4(0, 1, 0, 0));
}

[numthreads(64, 1, 1)]
void EmitCS(uint id : SV_DispatchThreadID)
{
    if (id >= g_S.totalSpawn)
        return;
    // The emitter whose spawn range holds this thread (offsets ascend with the slot).
    ByteAddressBuffer eb = GetBuffer(g_S.emitters);
    uint lo = 0, hi = g_S.emitterCount - 1;
    while (lo < hi) {
        const uint mid = (lo + hi + 1) / 2;
        if (eb.Load(mid * EMITTER_STRIDE + 196) <= id)
            lo = mid;
        else
            hi = mid - 1;
    }
    const uint eo = lo * EMITTER_STRIDE;
    const uint4 counts = eb.Load4(eo + 192); // spawn count, spawn offset, seed, generation
    const uint emitShape = eb.Load(eo + 208);
    const uint flags = eb.Load(eo + 224);
    const float4 shapeBox = asfloat(eb.Load4(eo + 112)); // box size, radius
    const float4 motion = asfloat(eb.Load4(eo + 144));   // speed, variation, lifetime, variation
    const float4 sizes = asfloat(eb.Load4(eo + 128));    // start, end, variation, spin
    const float coneAngle = asfloat(eb.Load(eo + 160));

    Rng r;
    r.s = Pcg(counts.z ^ Pcg(id - counts.y + 0x9E3779B9u));
    float3 local = 0.0, dir = float3(0, 1, 0);
    if (emitShape == 0) { // point
        dir = OnUnitSphere(r);
    } else if (emitShape == 1) { // sphere
        const float3 d = OnUnitSphere(r);
        local = d * shapeBox.w * pow(r.Next(), 1.0 / 3.0);
        dir = d;
    } else if (emitShape == 2) { // box
        local = float3(r.Range(-0.5, 0.5), r.Range(-0.5, 0.5), r.Range(-0.5, 0.5)) * shapeBox.xyz;
    } else { // cone (3), ring (4: on the circle)
        const float a = r.Next() * TWO_PI, rr = emitShape == 4 ? 1.0 : sqrt(r.Next());
        local = float3(cos(a) * rr, 0.0, sin(a) * rr) * shapeBox.w;
        const float angle = coneAngle * sqrt(r.Next());
        const float around = r.Next() * TWO_PI;
        dir = float3(sin(angle) * cos(around), cos(angle), sin(angle) * sin(around));
    }
    SimParticle p;
    if (flags & EMITTER_WORLD_SPACE) {
        const float4 c0 = asfloat(eb.Load4(eo)), c1 = asfloat(eb.Load4(eo + 16)), c2 = asfloat(eb.Load4(eo + 32)), c3 = asfloat(eb.Load4(eo + 48));
        p.position = c0.xyz * local.x + c1.xyz * local.y + c2.xyz * local.z + c3.xyz;
        if (emitShape != 0) {
            const float3 wd = c0.xyz * dir.x + c1.xyz * dir.y + c2.xyz * dir.z;
            dir = dot(wd, wd) > 1e-12 ? normalize(wd) : float3(0, 1, 0);
        }
    } else {
        p.position = local;
    }
    p.velocity = dir * (motion.x * (1.0 + motion.y * r.Range(-1.0, 1.0)));
    p.life = max(motion.z * (1.0 + motion.w * r.Range(-1.0, 1.0)), 0.02);
    p.age = 0.0;
    p.sizeScale = 1.0 + sizes.z * r.Range(-1.0, 1.0);
    p.rotation = r.Next() * TWO_PI;
    p.spin = sizes.w * r.Range(-1.0, 1.0);
    p.seed = r.Next();
    p.emitter = lo | (counts.w << 16);
    const uint slot = WaveAppend(GetRWBuffer(g_S.counters), g_S.inCounter);
    if (slot < g_S.capacity)
        StoreParticle(g_S.stateIn, slot, p);
}

float SceneDepth(float2 uv) { return GetTexture2D(g_S.depth).SampleLevel(g_PointClamp, uv, 0).r; }

float3 WorldFromDepth(float2 uv, float d)
{
    const float4 h = mul(g_S.invViewProj, float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, d, 1.0));
    return h.xyz / h.w;
}

[numthreads(64, 1, 1)]
void SimulateCS(uint id : SV_DispatchThreadID)
{
    RWByteAddressBuffer counters = GetRWBuffer(g_S.counters);
    if (id >= min(counters.Load(g_S.inCounter), g_S.capacity))
        return;
    SimParticle p = LoadParticle(g_S.stateIn, id);
    const uint slot = p.emitter & 0xFFFFu;
    if (slot >= g_S.emitterCount)
        return; // dies: not carried over
    // Emitter fields are loaded where they are used (a whole emitter kept live spills registers on small GPUs).
    ByteAddressBuffer eb = GetBuffer(g_S.emitters);
    const uint eo = slot * EMITTER_STRIDE;
    const uint flags = eb.Load(eo + 224);
    const float4 timing = asfloat(eb.Load4(eo + 176)); // dt, time, flipbook fps, flipbook frames
    if (!(flags & EMITTER_ACTIVE) || eb.Load(eo + 204) != (p.emitter >> 16))
        return; // its emitter is gone
    const float dt = timing.x;
    p.age += dt;
    if (p.age >= p.life)
        return;
    const bool world = (flags & EMITTER_WORLD_SPACE) != 0;
    if (dt > 0.0) {
        const float4 accelDrag = asfloat(eb.Load4(eo + 64));
        const float turbulence = asfloat(eb.Load(eo + 164));
        float3 accel = accelDrag.xyz;
        if (turbulence > 0.0)
            accel += Swirl(p.position * 1.3 + p.seed * 10.0, timing.y) * turbulence;
        // Drag pulls the velocity towards the wind (still air: towards zero).
        const float3 wind = world ? asfloat(eb.Load3(eo + 228)) : float3(0.0, 0.0, 0.0);
        p.velocity = wind + (p.velocity - wind + accel * dt) / (1.0 + accelDrag.w * dt);
        p.position += p.velocity * dt;
        p.rotation += p.spin * dt;
        // Collision with the visible surfaces (depth buffer): behind the surface by less than a thickness = a hit.
        if (world && (flags & (EMITTER_BOUNCE | EMITTER_DIE)) && g_S.depth != ZX_INVALID_INDEX) {
            const float4 clip = mul(g_S.viewProj, float4(p.position, 1.0));
            if (clip.w > g_S.nearPlane) {
                const float2 uv = float2(clip.x / clip.w * 0.5 + 0.5, 0.5 - clip.y / clip.w * 0.5);
                if (all(uv > 0.002) && all(uv < 0.998)) {
                    const float d = SceneDepth(uv);
                    const float sceneZ = d > 0.0 ? g_S.nearPlane / d : 1e9;
                    const float thickness = 0.15 + 0.02 * sceneZ + length(p.velocity) * dt;
                    if (clip.w > sceneZ && clip.w < sceneZ + thickness) {
                        if (flags & EMITTER_DIE)
                            return;
                        // Surface normal from the neighbouring depth samples (2 px apart).
                        const float2 t = 2.0 / g_S.depthSize;
                        const float3 c = WorldFromDepth(uv, d);
                        const float3 dx = WorldFromDepth(uv + float2(t.x, 0), SceneDepth(uv + float2(t.x, 0))) - c;
                        const float3 dy = WorldFromDepth(uv + float2(0, t.y), SceneDepth(uv + float2(0, t.y))) - c;
                        float3 n = normalize(cross(dx, dy));
                        if (dot(n, g_S.cameraPosition - c) < 0.0)
                            n = -n;
                        const float bounciness = asfloat(eb.Load(eo + 172));
                        const float vn = dot(p.velocity, n);
                        if (vn < 0.0)
                            p.velocity = (p.velocity - n * (vn * (1.0 + bounciness))) * (0.5 + 0.5 * bounciness);
                        p.position = c + n * 0.01;
                        if (n.y > 0.7 && dot(p.velocity, p.velocity) < 0.04)
                            p.velocity = 0.0; // at rest on a floor (on walls they slide down)
                    }
                }
            }
        }
    }
    const uint outSlot = WaveAppend(counters, g_S.outCounter);
    StoreParticle(g_S.stateOut, outSlot, p);

    // Render record (GpuParticle).
    const float t = saturate(p.age / p.life);
    float3 position = p.position, velocity = p.velocity;
    if (!world) {
        const float4 c0 = asfloat(eb.Load4(eo)), c1 = asfloat(eb.Load4(eo + 16)), c2 = asfloat(eb.Load4(eo + 32)), c3 = asfloat(eb.Load4(eo + 48));
        position = c0.xyz * p.position.x + c1.xyz * p.position.y + c2.xyz * p.position.z + c3.xyz;
        velocity = c0.xyz * p.velocity.x + c1.xyz * p.velocity.y + c2.xyz * p.velocity.z;
    }
    const float4 sizes = asfloat(eb.Load4(eo + 128)); // start, end, variation, spin
    const float4 startColor = asfloat(eb.Load4(eo + 80)), endColor = asfloat(eb.Load4(eo + 96));
    const float fadeIn = asfloat(eb.Load(eo + 168));
    const float fade = fadeIn > 0.0 ? min(t / fadeIn, 1.0) : 1.0;
    const float4 color = float4(lerp(startColor.rgb, endColor.rgb, t), saturate(lerp(startColor.a, endColor.a, t) * fade));
    const uint4 look = eb.Load4(eo + 208); // emit shape, packed shape, render flags, texture
    float seed = p.seed;
    if (look.w != ZX_INVALID_INDEX)
        seed = timing.z > 0.0 ? frac(p.seed + p.age * timing.z / max(timing.w, 1.0)) : min(t, 0.999);
    RWByteAddressBuffer rb = GetRWBuffer(g_S.render);
    const uint o = outSlot * RENDER_STRIDE;
    rb.Store4(o, asuint(float4(position, max(lerp(sizes.x, sizes.y, t) * p.sizeScale, 0.0))));
    rb.Store4(o + 16, asuint(color));
    rb.Store4(o + 32, uint4(asuint(p.rotation), look.z, look.w, look.y));
    rb.Store4(o + 48, uint4(asuint(velocity), asuint(seed)));
}

[numthreads(1, 1, 1)]
void ArgsCS()
{
    RWByteAddressBuffer counters = GetRWBuffer(g_S.counters);
    const uint alive = min(counters.Load(g_S.outCounter), g_S.capacity);
    GetRWBuffer(g_S.args).Store4(0, uint4(alive * 6, 1, 0, 0));
    counters.Store(g_S.inCounter, 0u); // next frame appends into this buffer
}
