#include "Render/Particles.h"
#include "Core/Log/Log.h"
#include "RHI/ShaderCompiler.h"

namespace zx::render {

using namespace rhi;

ZX_DEFINE_LOG_CATEGORY_STATIC(LogParticles, Info);

namespace {

struct ParticleParams
{
    Mat4 viewProj;
    Vec3 cameraRight;
    u32 particles;
    Vec3 cameraUp;
    u32 depth;
    Vec3 cameraPosition;
    f32 nearPlane;
    Vec3 ambient;   // pre-exposed sky light
    f32 time;
    Vec3 sunLight;  // pre-exposed sun illuminance / pi
    f32 emissiveScale;
    Vec3 dirToSun;
    f32 pad0;
    Vec3 cameraForward;
    f32 pad1;
    u32 lightingVolume;   // volumetric fog (invalid: none)
    u32 integratedVolume;
    f32 fogDistance;
    f32 fogSlices;
    u32 sceneColor;
    u32 distortion;
    Vec2 invSize;
};

struct SimParams // ParticleSim.hlsl
{
    Mat4 viewProj;
    Mat4 invViewProj;
    u32 stateIn, stateOut, counters, render;
    u32 emitters, args, emitterCount, totalSpawn;
    u32 capacity, depth, inCounter, outCounter;
    Vec3 cameraPosition;
    f32 nearPlane;
    Vec2 depthSize;
    Vec2 pad;
};

} // namespace

ParticleRenderer::ParticleRenderer(Device& device) : m_device(device) {}

ParticleRenderer::~ParticleRenderer()
{
    m_device.Destroy(m_pipeline);
    m_device.Destroy(m_heatPipeline);
    m_device.Destroy(m_distortPipeline);
    for (Buffer* b : m_buffers)
        m_device.Destroy(b);
    for (Pipeline* p : {m_initPipeline, m_emitPipeline, m_simulatePipeline, m_argsPipeline})
        m_device.Destroy(p);
    for (Buffer* b : {m_simState[0], m_simState[1], m_simCounters, m_simRender, m_simArgs, m_simEmitters})
        m_device.Destroy(b);
    for (Buffer* b : m_emitterBuffers)
        m_device.Destroy(b);
}

bool ParticleRenderer::EnsureGpuSimulation()
{
    if (m_simulatePipeline)
        return true;
    if (m_gpuSimFailed)
        return false;
    m_gpuSimFailed = true; // until everything below succeeds
    struct Job
    {
        ShaderBytecode* out;
        Pipeline** pipeline;
        StringView entry;
    } jobs[] = {{&m_initCS, &m_initPipeline, "InitCS"},
                {&m_emitCS, &m_emitPipeline, "EmitCS"},
                {&m_simulateCS, &m_simulatePipeline, "SimulateCS"},
                {&m_argsCS, &m_argsPipeline, "ArgsCS"}};
    for (const Job& j : jobs) {
        ShaderCompileDesc d;
        d.path = "/Shaders/Particles/ParticleSim.hlsl";
        d.entryPoint = j.entry;
        d.stage = ShaderStage::Compute;
        Result<ShaderBytecode> r = ShaderCompiler::Compile(d);
        if (!r) {
            ZX_LOG_ERROR(LogParticles, "{}", r.GetError().message);
            return false;
        }
        *j.out = move(*r);
        ComputePipelineDesc pd;
        pd.computeShader = j.out;
        pd.debugName = "ParticleSim";
        *j.pipeline = m_device.CreateComputePipeline(pd);
        if (!*j.pipeline)
            return false;
    }
    const BufferUsage uav = BufferUsage::UnorderedAccess | BufferUsage::ShaderResource;
    for (Buffer*& b : m_simState)
        b = m_device.CreateBuffer(BufferDesc{u64(GpuCapacity) * 64, uav, MemoryType::GpuOnly, 0, "ParticleSim.State"});
    m_simCounters = m_device.CreateBuffer(BufferDesc{256, uav, MemoryType::GpuOnly, 0, "ParticleSim.Counters"});
    m_simRender = m_device.CreateBuffer(BufferDesc{u64(GpuCapacity) * sizeof(GpuParticle), uav, MemoryType::GpuOnly, 0, "ParticleSim.Render"});
    m_simArgs = m_device.CreateBuffer(BufferDesc{sizeof(IndirectDrawArgsPlain), uav | BufferUsage::Indirect, MemoryType::GpuOnly, 0, "ParticleSim.Args"});
    for (Buffer*& b : m_emitterBuffers)
        b = m_device.CreateBuffer(BufferDesc{u64(MaxGpuEmitters) * sizeof(GpuEmitter), BufferUsage::CopySource, MemoryType::Upload, 0, "ParticleSim.EmitterUpload"});
    // Every particle reads its emitter row: from GPU memory, not the upload heap.
    m_simEmitters = m_device.CreateBuffer(BufferDesc{u64(MaxGpuEmitters) * sizeof(GpuEmitter), BufferUsage::ShaderResource | BufferUsage::CopyDest,
                                                     MemoryType::GpuOnly, 0, "ParticleSim.Emitters"});
    for (Buffer* b : {m_simState[0], m_simState[1], m_simCounters, m_simRender, m_simArgs, m_simEmitters})
        if (!b)
            return false;
    for (Buffer* b : m_emitterBuffers)
        if (!b)
            return false;
    m_gpuSimFailed = false;
    m_gpuNeedsInit = true;
    return true;
}

bool ParticleRenderer::Init(Format colorFormat)
{
    struct Job
    {
        ShaderBytecode* out;
        StringView entry;
        ShaderStage stage;
    } jobs[] = {{&m_vs, "ParticleVS", ShaderStage::Vertex},     {&m_ps, "ParticlePS", ShaderStage::Pixel},
                {&m_heatPS, "HeatPS", ShaderStage::Pixel},       {&m_distortVS, "DistortVS", ShaderStage::Vertex},
                {&m_distortPS, "DistortPS", ShaderStage::Pixel}};
    m_colorFormat = colorFormat;
    for (const Job& j : jobs) {
        ShaderCompileDesc d;
        d.path = "/Shaders/Particles/Particles.hlsl";
        d.entryPoint = j.entry;
        d.stage = j.stage;
        Result<ShaderBytecode> r = ShaderCompiler::Compile(d);
        if (!r) {
            ZX_LOG_ERROR(LogParticles, "{}", r.GetError().message);
            return false;
        }
        *j.out = move(*r);
    }
    GraphicsPipelineDesc pd;
    pd.vertexShader = &m_vs;
    pd.pixelShader = &m_ps;
    pd.rasterizer.cullMode = CullMode::None;
    pd.colorFormats[0] = colorFormat;
    pd.colorCount = 1;
    // Premultiplied alpha: color = src + dst * (1 - a); additive particles output a = 0.
    BlendTarget blend = BlendTarget::AlphaBlend();
    blend.srcColor = BlendFactor::One;
    blend.dstColor = BlendFactor::InvSrcAlpha;
    blend.writeMask = 0x7; // keep the scene alpha
    pd.blend[0] = blend;
    pd.debugName = "Particles";
    m_pipeline = m_device.CreateGraphicsPipeline(pd);
    // Heat haze: offsets summed into an RG16F target.
    GraphicsPipelineDesc hd = pd;
    hd.pixelShader = &m_heatPS;
    hd.colorFormats[0] = Format::R16G16_FLOAT;
    BlendTarget add;
    add.enable = true;
    add.srcColor = BlendFactor::One;
    add.dstColor = BlendFactor::One;
    add.srcAlpha = BlendFactor::One;
    add.dstAlpha = BlendFactor::One;
    hd.blend[0] = add;
    hd.debugName = "HeatHaze";
    m_heatPipeline = m_device.CreateGraphicsPipeline(hd);
    GraphicsPipelineDesc dd;
    dd.vertexShader = &m_distortVS;
    dd.pixelShader = &m_distortPS;
    dd.rasterizer.cullMode = CullMode::None;
    dd.depthStencil.depthTest = false;
    dd.depthStencil.depthWrite = false;
    dd.colorFormats[0] = colorFormat;
    dd.colorCount = 1;
    dd.debugName = "HeatDistort";
    m_distortPipeline = m_device.CreateGraphicsPipeline(dd);
    for (u32 i = 0; i < FramesInFlight; ++i)
        m_buffers[i] = m_device.CreateBuffer(
            BufferDesc{u64(MaxParticles) * sizeof(GpuParticle), BufferUsage::ShaderResource, MemoryType::Upload, 0, "Particles"});
    return m_pipeline && m_heatPipeline && m_distortPipeline;
}

RGTexture ParticleRenderer::AddPass(RenderGraph& graph, RGTexture color, RGTexture depth, const RenderView& view, Span<const GpuParticle> particles,
                               const SceneLighting& lighting, f32 exposure, f32 time, const FogVolumes& fog, const GpuParticleFrame* gpu)
{
    if (!m_pipeline)
        return color;
    const bool gpuEmitters = gpu && !gpu->emitters.Empty();
    if (particles.Empty() && !gpuEmitters && !m_gpuHasParticles)
        return color;
    Buffer* buffer = m_buffers[m_device.GetFrameIndex() % FramesInFlight];
    if (!buffer)
        return color;
    m_count = Min(u32(particles.Size()), MaxParticles);
    // The farthest particles are first: when capped, drop the nearest instead of popping holes in the middle.
    if (m_count)
        MemCopy(buffer->GetMappedData(), particles.Data(), usize(m_count) * sizeof(GpuParticle));

    ParticleParams p = {};
    p.viewProj = view.viewProj;
    const Mat4 invView = Inverse(view.view);
    p.cameraRight = SafeNormalize(invView.TransformVector(Vec3(1, 0, 0)), Vec3(1, 0, 0));
    p.cameraUp = SafeNormalize(invView.TransformVector(Vec3(0, 1, 0)), Vec3(0, 1, 0));
    p.cameraForward = SafeNormalize(invView.TransformVector(Vec3(0, 0, 1)), Vec3(0, 0, 1));
    p.cameraPosition = view.position;
    p.nearPlane = view.nearPlane;
    p.ambient = lighting.skyColor * (lighting.skyLuminance * exposure);
    p.sunLight = lighting.sunColor * (lighting.sunIlluminance * exposure / Pi);
    p.dirToSun = -lighting.sunDirection;
    p.emissiveScale = EmissiveNits * exposure;
    p.time = time;
    p.particles = buffer->GetSrvIndex();
    p.lightingVolume = p.integratedVolume = InvalidDescriptor;
    p.fogDistance = fog.distance;
    p.fogSlices = f32(Max(fog.slices, 1u));
    const UVec2 size(view.width, view.height);
    const u32 count = m_count;
    const bool hasFog = fog.lighting.IsValid() && fog.integrated.IsValid() && fog.distance > 0.0f;

    // GPU simulation: once per tick (a paused or repeated tick only draws what the pool holds).
    if (!gpuEmitters && m_gpuHasParticles) {
        m_gpuHasParticles = false; // every GPU emitter is gone: empty the pool next time
        m_gpuNeedsInit = true;
    }
    const bool drawGpu = gpuEmitters && EnsureGpuSimulation();
    if (drawGpu) {
        if (gpu->resetGeneration != m_resetGeneration) {
            m_resetGeneration = gpu->resetGeneration;
            m_gpuNeedsInit = true;
        }
        const RGBuffer state0 = graph.ImportBuffer(m_simState[0], ResourceState::UnorderedAccess);
        const RGBuffer state1 = graph.ImportBuffer(m_simState[1], ResourceState::UnorderedAccess);
        const RGBuffer counters = graph.ImportBuffer(m_simCounters, ResourceState::UnorderedAccess);
        const RGBuffer render = graph.ImportBuffer(m_simRender, ResourceState::ShaderResource);
        const RGBuffer args = graph.ImportBuffer(m_simArgs, ResourceState::IndirectArgument);
        const bool simulate = gpu->tick != m_lastTick || m_gpuNeedsInit;
        if (simulate) {
            m_lastTick = gpu->tick;
            // Emitter rows with their spawn ranges.
            const u32 frame = m_device.GetFrameIndex() % FramesInFlight;
            const u32 emitterCount = Min(u32(gpu->emitters.Size()), MaxGpuEmitters);
            GpuEmitter* rows = static_cast<GpuEmitter*>(m_emitterBuffers[frame]->GetMappedData());
            u32 totalSpawn = 0;
            for (u32 i = 0; i < emitterCount; ++i) {
                rows[i] = gpu->emitters[i];
                rows[i].spawnOffset = totalSpawn;
                rows[i].spawnCount = Min(rows[i].spawnCount, GpuCapacity - totalSpawn);
                totalSpawn += rows[i].spawnCount;
            }
            SimParams sp = {};
            sp.viewProj = view.viewProj;
            sp.invViewProj = Inverse(view.viewProj);
            sp.emitterCount = emitterCount;
            sp.totalSpawn = totalSpawn;
            sp.capacity = GpuCapacity;
            sp.cameraPosition = view.position;
            sp.nearPlane = view.nearPlane;
            sp.depthSize = Vec2(f32(view.width), f32(view.height));
            sp.inCounter = m_parity * 64;
            sp.outCounter = (1 - m_parity) * 64;
            const u32 parity = m_parity;
            m_parity ^= 1u;
            Buffer* emitterUpload = m_emitterBuffers[frame];
            const RGBuffer emitterTable = graph.ImportBuffer(m_simEmitters, ResourceState::ShaderResource);
            graph.AddPass(
                "ParticleEmitters", PassType::Compute, [emitterTable](PassBuilder& b) { b.WriteBuffer(emitterTable, ResourceState::CopyDest); },
                [=](PassContext& ctx) { ctx.cmd.CopyBuffer(ctx.Buffer(emitterTable), 0, emitterUpload, 0, u64(Max(emitterCount, 1u)) * sizeof(GpuEmitter)); });
            auto bind = [=, this](PassContext& ctx) {
                SimParams q = sp;
                q.stateIn = ctx.Buffer(parity ? state1 : state0)->GetUavIndex();
                q.stateOut = ctx.Buffer(parity ? state0 : state1)->GetUavIndex();
                q.counters = ctx.Buffer(counters)->GetUavIndex();
                q.render = ctx.Buffer(render)->GetUavIndex();
                q.args = ctx.Buffer(args)->GetUavIndex();
                q.emitters = ctx.Buffer(emitterTable)->GetSrvIndex();
                q.depth = ctx.Texture(depth)->GetSrvIndex();
                return q;
            };
            auto writesAll = [=](PassBuilder& b) {
                b.ReadTexture(depth);
                b.ReadBuffer(emitterTable);
                for (RGBuffer x : {state0, state1, counters, render, args})
                    b.WriteBuffer(x);
            };
            if (m_gpuNeedsInit) {
                m_gpuNeedsInit = false;
                graph.AddPass("ParticleSimInit", PassType::Compute, writesAll, [=, this](PassContext& ctx) {
                    const SimParams q = bind(ctx);
                    ctx.cmd.SetPipeline(m_initPipeline);
                    ctx.cmd.SetConstants(0, &q, sizeof(q));
                    ctx.cmd.Dispatch(1);
                });
            }
            if (totalSpawn > 0)
                graph.AddPass("ParticleEmit", PassType::Compute, writesAll, [=, this](PassContext& ctx) {
                    const SimParams q = bind(ctx);
                    ctx.cmd.SetPipeline(m_emitPipeline);
                    ctx.cmd.SetConstants(0, &q, sizeof(q));
                    ctx.cmd.Dispatch((totalSpawn + 63) / 64);
                });
            graph.AddPass("ParticleSimulate", PassType::Compute, writesAll, [=, this](PassContext& ctx) {
                const SimParams q = bind(ctx);
                ctx.cmd.SetPipeline(m_simulatePipeline);
                ctx.cmd.SetConstants(0, &q, sizeof(q));
                ctx.cmd.Dispatch((GpuCapacity + 63) / 64);
                ctx.cmd.SetPipeline(m_argsPipeline);
                ctx.cmd.UavBarrier(); // ArgsCS reads the counters the simulation incremented
                ctx.cmd.Dispatch(1);
            });
            m_gpuHasParticles = true;
        }
        graph.AddPass(
            "GpuParticles", PassType::Graphics,
            [=](PassBuilder& b) {
                b.ReadTexture(depth);
                if (hasFog) {
                    b.ReadTexture(fog.lighting);
                    b.ReadTexture(fog.integrated);
                }
                b.ReadBuffer(render);
                b.ReadBuffer(args, ResourceState::IndirectArgument);
                b.RenderTarget(color, LoadOp::Load);
            },
            [=, this](PassContext& ctx) {
                ParticleParams params = p;
                params.particles = ctx.Buffer(render)->GetSrvIndex();
                params.depth = ctx.Texture(depth)->GetSrvIndex();
                if (hasFog) {
                    params.lightingVolume = ctx.Texture(fog.lighting)->GetSrvIndex();
                    params.integratedVolume = ctx.Texture(fog.integrated)->GetSrvIndex();
                }
                ctx.cmd.SetViewport(Viewport{0, 0, f32(size.x), f32(size.y), 0, 1});
                ctx.cmd.SetScissor(0, 0, size.x, size.y);
                ctx.cmd.SetPipeline(m_pipeline);
                ctx.cmd.SetConstants(0, &params, sizeof(params));
                ctx.cmd.DrawIndirect(ctx.Buffer(args), 0);
            });
    }
    if (count == 0)
        return color;
    graph.AddPass(
        "Particles", PassType::Graphics,
        [=](PassBuilder& b) {
            b.ReadTexture(depth);
            if (hasFog) {
                b.ReadTexture(fog.lighting);
                b.ReadTexture(fog.integrated);
            }
            b.RenderTarget(color, LoadOp::Load);
        },
        [=, this](PassContext& ctx) {
            ParticleParams params = p;
            params.depth = ctx.Texture(depth)->GetSrvIndex();
            if (hasFog) {
                params.lightingVolume = ctx.Texture(fog.lighting)->GetSrvIndex();
                params.integratedVolume = ctx.Texture(fog.integrated)->GetSrvIndex();
            }
            ctx.cmd.SetViewport(Viewport{0, 0, f32(size.x), f32(size.y), 0, 1});
            ctx.cmd.SetScissor(0, 0, size.x, size.y);
            ctx.cmd.SetPipeline(m_pipeline);
            ctx.cmd.SetConstants(0, &params, sizeof(params));
            ctx.cmd.Draw(count * 6);
        });

    // Heat haze: offsets from the heat particles, then the scene resampled through them into a new color target.
    bool anyHeat = false;
    for (u32 i = 0; i < count && !anyHeat; ++i)
        anyHeat = (particles[i].shape & 0xFFu) == u32(ParticleShape::Heat);
    if (!anyHeat || !m_heatPipeline || !m_distortPipeline)
        return color;
    TextureDesc od;
    od.width = size.x;
    od.height = size.y;
    od.format = Format::R16G16_FLOAT;
    od.usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource;
    od.debugName = "HeatOffsets";
    const RGTexture offsets = graph.CreateTexture(od);
    graph.AddPass(
        "HeatHaze", PassType::Graphics,
        [=](PassBuilder& b) {
            b.ReadTexture(depth);
            b.RenderTarget(offsets, LoadOp::Clear, LinearColor(0, 0, 0, 0));
        },
        [=, this](PassContext& ctx) {
            ParticleParams params = p;
            params.depth = ctx.Texture(depth)->GetSrvIndex();
            ctx.cmd.SetViewport(Viewport{0, 0, f32(size.x), f32(size.y), 0, 1});
            ctx.cmd.SetScissor(0, 0, size.x, size.y);
            ctx.cmd.SetPipeline(m_heatPipeline);
            ctx.cmd.SetConstants(0, &params, sizeof(params));
            ctx.cmd.Draw(count * 6);
        });
    TextureDesc cd;
    cd.width = size.x;
    cd.height = size.y;
    cd.format = m_colorFormat;
    cd.usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource;
    cd.debugName = "SceneColorDistorted";
    const RGTexture distorted = graph.CreateTexture(cd);
    graph.AddPass(
        "HeatDistort", PassType::Graphics,
        [=](PassBuilder& b) {
            b.ReadTexture(color);
            b.ReadTexture(offsets);
            b.RenderTarget(distorted, LoadOp::Clear, LinearColor(0, 0, 0, 1));
        },
        [=, this](PassContext& ctx) {
            ParticleParams params = p;
            params.sceneColor = ctx.Texture(color)->GetSrvIndex();
            params.distortion = ctx.Texture(offsets)->GetSrvIndex();
            ctx.cmd.SetViewport(Viewport{0, 0, f32(size.x), f32(size.y), 0, 1});
            ctx.cmd.SetScissor(0, 0, size.x, size.y);
            ctx.cmd.SetPipeline(m_distortPipeline);
            ctx.cmd.SetConstants(0, &params, sizeof(params));
            ctx.cmd.Draw(3);
        });
    return distorted;
}

} // namespace zx::render
