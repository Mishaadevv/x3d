#include "Render/Fluid.h"
#include "Core/Log/Log.h"
#include "RHI/ShaderCompiler.h"

namespace zx::render {

using namespace rhi;

ZX_DEFINE_LOG_CATEGORY_STATIC(LogFluid, Info);

namespace {

// Keep in sync with FluidParams (Shaders/Volumetric/Fluid.hlsl).
struct FluidParams
{
    u32 grid[3];
    f32 dt;
    Vec3 sourcePosition;
    f32 sourceRadius;
    Vec3 sourceVelocity;
    f32 sourceDensity;
    f32 sourceTemperature;
    f32 buoyancy;
    f32 weight;
    f32 vorticity;
    f32 densityFade;
    f32 temperatureFade;
    f32 velocityFade;
    f32 time;
    f32 noise;
    u32 velocityIn;
    u32 velocityOut;
    u32 scalarIn;
    u32 scalarOut;
    u32 pressureIn;
    u32 pressureOut;
    u32 divergence;
    u32 curl;
    f32 pad0;
    f32 pad1;
};

// Keep in sync with FluidDrawParams.
struct FluidDrawParams
{
    Mat4 invViewProj;
    Mat4 worldToVolume;
    Vec3 cameraPosition;
    f32 densityScale;
    Vec3 dirToSun;
    f32 nearPlane;
    Vec3 sunColor;
    f32 fireIntensity;
    Vec3 ambient;
    f32 stepLength;
    Vec3 smokeColor;
    u32 scalars;
    Vec3 volumeSize;
    u32 depthTexture;
    Vec2 renderSize;
    f32 fireTemperature;
    u32 steps;
};

constexpr u32 JacobiIterations = 20;

} // namespace

FluidRenderer::FluidRenderer(Device& device) : m_device(device) {}

FluidRenderer::~FluidRenderer()
{
    for (auto& [id, s] : m_states)
        Release(s);
    for (Pipeline* p : {m_curl, m_advectVelocity, m_divergence, m_jacobi, m_project, m_advectScalars, m_draw})
        m_device.Destroy(p);
}

bool FluidRenderer::Init(Format colorFormat)
{
    struct Job
    {
        ShaderBytecode* out;
        StringView entry;
        ShaderStage stage;
    } shaderJobs[] = {{&m_curlCS, "CurlCS", ShaderStage::Compute},
                      {&m_advectVelocityCS, "AdvectVelocityCS", ShaderStage::Compute},
                      {&m_divergenceCS, "DivergenceCS", ShaderStage::Compute},
                      {&m_jacobiCS, "JacobiCS", ShaderStage::Compute},
                      {&m_projectCS, "ProjectCS", ShaderStage::Compute},
                      {&m_advectScalarsCS, "AdvectScalarsCS", ShaderStage::Compute},
                      {&m_vs, "FluidVS", ShaderStage::Vertex},
                      {&m_ps, "FluidPS", ShaderStage::Pixel}};
    for (const Job& j : shaderJobs) {
        ShaderCompileDesc d;
        d.path = "/Shaders/Volumetric/Fluid.hlsl";
        d.entryPoint = j.entry;
        d.stage = j.stage;
        Result<ShaderBytecode> r = ShaderCompiler::Compile(d);
        if (!r) {
            ZX_LOG_ERROR(LogFluid, "{}", r.GetError().message);
            return false;
        }
        *j.out = move(*r);
    }
    auto compute = [&](ShaderBytecode& cs, StringView name) {
        ComputePipelineDesc cd;
        cd.computeShader = &cs;
        cd.debugName = name;
        return m_device.CreateComputePipeline(cd);
    };
    m_curl = compute(m_curlCS, "FluidCurl");
    m_advectVelocity = compute(m_advectVelocityCS, "FluidAdvectVelocity");
    m_divergence = compute(m_divergenceCS, "FluidDivergence");
    m_jacobi = compute(m_jacobiCS, "FluidJacobi");
    m_project = compute(m_projectCS, "FluidProject");
    m_advectScalars = compute(m_advectScalarsCS, "FluidAdvectScalars");
    GraphicsPipelineDesc pd;
    pd.vertexShader = &m_vs;
    pd.pixelShader = &m_ps;
    pd.rasterizer.cullMode = CullMode::None;
    pd.depthStencil.depthTest = false;
    pd.depthStencil.depthWrite = false;
    pd.colorFormats[0] = colorFormat;
    pd.colorCount = 1;
    pd.blend[0].enable = true;
    pd.blend[0].srcColor = BlendFactor::One;
    pd.blend[0].dstColor = BlendFactor::SrcAlpha;
    pd.blend[0].srcAlpha = BlendFactor::Zero;
    pd.blend[0].dstAlpha = BlendFactor::One;
    pd.debugName = "FluidDraw";
    m_draw = m_device.CreateGraphicsPipeline(pd);
    return m_curl && m_advectVelocity && m_divergence && m_jacobi && m_project && m_advectScalars && m_draw;
}

void FluidRenderer::Release(State& s)
{
    for (u32 i = 0; i < 2; ++i) {
        m_device.Destroy(s.velocity[i]);
        m_device.Destroy(s.scalars[i]);
        m_device.Destroy(s.pressure[i]);
    }
    m_device.Destroy(s.divergence);
    m_device.Destroy(s.curl);
    s = State{};
}

FluidRenderer::State* FluidRenderer::StateFor(const FluidVolumeDesc& v, UVec3 grid)
{
    State* s = m_states.Find(v.id);
    if (s && s->grid != grid) {
        Release(*s);
        m_states.Remove(v.id);
        s = nullptr;
    }
    if (!s) {
        if (m_states.Size() >= MaxVolumes)
            return nullptr;
        State fresh;
        fresh.grid = grid;
        TextureDesc d;
        d.dimension = TextureDimension::Texture3D;
        d.width = grid.x;
        d.height = grid.y;
        d.depthOrArraySize = grid.z;
        d.format = Format::R16G16B16A16_FLOAT;
        d.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess;
        // Zero-filled to start with still, empty air.
        Array<u8> zeros;
        zeros.ResizeZeroed(usize(grid.x) * grid.y * grid.z * 8);
        SubresourceData init;
        init.data = zeros.Data();
        init.rowPitch = u64(grid.x) * 8;
        init.slicePitch = u64(grid.x) * grid.y * 8;
        const Span<const SubresourceData> data(&init, 1);
        for (u32 i = 0; i < 2; ++i) {
            d.debugName = "FluidVelocity";
            fresh.velocity[i] = m_device.CreateTexture(d, data);
            d.debugName = "FluidScalars";
            fresh.scalars[i] = m_device.CreateTexture(d, data);
            d.debugName = "FluidPressure";
            fresh.pressure[i] = m_device.CreateTexture(d, data);
        }
        d.debugName = "FluidDivergence";
        fresh.divergence = m_device.CreateTexture(d, data);
        d.debugName = "FluidCurl";
        fresh.curl = m_device.CreateTexture(d, data);
        m_states.InsertOrAssign(v.id, move(fresh));
        s = m_states.Find(v.id);
        if (!s->divergence || !s->curl || !s->velocity[1] || !s->scalars[1] || !s->pressure[1]) {
            Release(*s);
            m_states.Remove(v.id);
            return nullptr;
        }
    }
    return s;
}

void FluidRenderer::AddPasses(RenderGraph& graph, RGTexture color, RGTexture depth, const RenderView& view, const Mat4& viewProjNoJitter,
                              const SceneLighting& lighting, Span<const FluidVolumeDesc> volumes, f32 exposure, f32 dt, f32 time)
{
    ++m_frame;
    // Grids of volumes that are gone.
    m_states.RemoveIf([this](u64, State& s) {
        if (m_frame - s.lastFrame < 120)
            return false;
        Release(s);
        return true;
    });
    if (volumes.Empty() || !m_draw)
        return;
    dt = Clamp(dt, 0.0f, 1.0f / 30.0f);
    const UVec2 size(view.width, view.height);
    for (const FluidVolumeDesc& v : volumes) {
        const Vec3 boxSize = v.world.GetScale();
        const f32 longest = Max(Max(boxSize.x, boxSize.y), Max(boxSize.z, 0.01f));
        const u32 res = Clamp(v.resolution, 16u, 96u);
        const f32 h = longest / f32(res); // meters per cell
        const UVec3 grid(Max(u32(boxSize.x / h + 0.5f), 8u), Max(u32(boxSize.y / h + 0.5f), 8u), Max(u32(boxSize.z / h + 0.5f), 8u));
        State* s = StateFor(v, grid);
        if (!s)
            continue;
        s->lastFrame = m_frame;

        RGTexture vel[2], sca[2], pre[2];
        for (u32 i = 0; i < 2; ++i) {
            vel[i] = graph.ImportTexture(s->velocity[i], ResourceState::ShaderResource);
            sca[i] = graph.ImportTexture(s->scalars[i], ResourceState::ShaderResource);
            pre[i] = graph.ImportTexture(s->pressure[i], ResourceState::ShaderResource);
        }
        const RGTexture divRG = graph.ImportTexture(s->divergence, ResourceState::ShaderResource);
        const RGTexture curlRG = graph.ImportTexture(s->curl, ResourceState::ShaderResource);
        // Velocity lives in [0] (advected into [1], projected back into [0]); scalars ping-pong via `current`.
        const u32 a = s->current, b = a ^ 1;

        if (v.simulate && dt > 0.0f) {
            FluidParams p = {};
            p.grid[0] = grid.x;
            p.grid[1] = grid.y;
            p.grid[2] = grid.z;
            p.dt = dt;
            p.sourcePosition = (v.sourcePosition + Vec3(0.5f)) * Vec3(f32(grid.x), f32(grid.y), f32(grid.z)) - Vec3(0.5f);
            p.sourceRadius = Max(v.sourceRadius * f32(res), 1.5f);
            p.sourceVelocity = v.sourceVelocity / h;
            p.sourceDensity = v.smokeRate;
            p.sourceTemperature = v.heat;
            p.buoyancy = v.buoyancy / h;
            p.weight = v.weight / h;
            p.vorticity = v.vorticity;
            p.densityFade = v.smokeFade;
            p.temperatureFade = v.coolRate;
            p.velocityFade = 0.1f;
            p.time = time;
            p.noise = v.noise;
            const UVec3 groups((grid.x + 3) / 4, (grid.y + 3) / 4, (grid.z + 3) / 4);
            auto pass = [&](StringView name, Pipeline* pipeline, FluidParams params, std::initializer_list<RGTexture> reads,
                            std::initializer_list<RGTexture> writes) {
                Array<RGTexture> r(reads), w(writes);
                graph.AddPass(
                    name, PassType::Compute,
                    [r, w](PassBuilder& builder) {
                        for (RGTexture t : r)
                            builder.ReadTexture(t);
                        for (RGTexture t : w)
                            builder.WriteTexture(t);
                    },
                    [pipeline, params, groups](PassContext& ctx) {
                        ctx.cmd.SetPipeline(pipeline);
                        ctx.cmd.SetConstants(0, &params, sizeof(params));
                        ctx.cmd.Dispatch(groups.x, groups.y, groups.z);
                    });
            };
            // Velocity: curl, then advect + forces into [1].
            FluidParams q = p;
            q.velocityIn = s->velocity[0]->GetSrvIndex();
            q.curl = s->curl->GetUavIndex();
            pass("FluidCurl", m_curl, q, {vel[0]}, {curlRG});
            q.curl = s->curl->GetSrvIndex();
            q.scalarIn = s->scalars[a]->GetSrvIndex();
            q.velocityOut = s->velocity[1]->GetUavIndex();
            pass("FluidAdvectVelocity", m_advectVelocity, q, {vel[0], curlRG, sca[a]}, {vel[1]});
            // Pressure projection: [1] -> [0].
            q = p;
            q.velocityIn = s->velocity[1]->GetSrvIndex();
            q.divergence = s->divergence->GetUavIndex();
            pass("FluidDivergence", m_divergence, q, {vel[1]}, {divRG});
            q.divergence = s->divergence->GetSrvIndex();
            for (u32 it = 0; it < JacobiIterations; ++it) {
                const u32 from = it & 1, to = from ^ 1;
                q.pressureIn = s->pressure[from]->GetSrvIndex();
                q.pressureOut = s->pressure[to]->GetUavIndex();
                pass("FluidJacobi", m_jacobi, q, {pre[from], divRG}, {pre[to]});
            }
            q.pressureIn = s->pressure[JacobiIterations & 1]->GetSrvIndex();
            q.velocityIn = s->velocity[1]->GetSrvIndex();
            q.velocityOut = s->velocity[0]->GetUavIndex();
            pass("FluidProject", m_project, q, {pre[JacobiIterations & 1], vel[1]}, {vel[0]});
            // Smoke and heat along the new velocity: a -> b.
            q = p;
            q.velocityIn = s->velocity[0]->GetSrvIndex();
            q.scalarIn = s->scalars[a]->GetSrvIndex();
            q.scalarOut = s->scalars[b]->GetUavIndex();
            pass("FluidAdvectScalars", m_advectScalars, q, {vel[0], sca[a]}, {sca[b]});
            s->current = b;
        }

        // Draw: raymarch the box over the scene.
        const u32 cur = s->current;
        FluidDrawParams dp = {};
        dp.invViewProj = Inverse(viewProjNoJitter);
        // World -> volume uvw: inverse of (world * translate to the -0.5 corner).
        dp.worldToVolume = Inverse(v.world * Mat4::Translation(Vec3(-0.5f)));
        dp.cameraPosition = view.position;
        dp.densityScale = Max(v.density, 0.0f);
        dp.dirToSun = -lighting.sunDirection;
        dp.nearPlane = view.nearPlane;
        dp.sunColor = lighting.sunColor * (lighting.sunIlluminance * exposure);
        dp.fireIntensity = v.fireIntensity * 40000.0f * exposure; // nits per meter of fully hot gas
        dp.ambient = lighting.skyColor * (lighting.skyLuminance * exposure);
        dp.stepLength = Max(h * 0.75f, 0.01f);
        dp.smokeColor = v.smokeColor;
        dp.scalars = s->scalars[cur]->GetSrvIndex();
        dp.volumeSize = boxSize;
        dp.renderSize = Vec2(f32(size.x), f32(size.y));
        dp.fireTemperature = Clamp(v.fireThreshold, 0.0f, 0.95f);
        dp.steps = Min(u32(Length(boxSize) / dp.stepLength) + 2, 256u);
        const RGTexture scalarsRG = sca[cur];
        graph.AddPass(
            "FluidDraw", PassType::Graphics,
            [=](PassBuilder& builder) {
                builder.ReadTexture(scalarsRG);
                builder.ReadTexture(depth);
                builder.RenderTarget(color, LoadOp::Load);
            },
            [=, this](PassContext& ctx) {
                FluidDrawParams params = dp;
                params.depthTexture = ctx.Texture(depth)->GetSrvIndex();
                ctx.cmd.SetViewport(Viewport{0, 0, f32(size.x), f32(size.y), 0, 1});
                ctx.cmd.SetScissor(0, 0, size.x, size.y);
                ctx.cmd.SetPipeline(m_draw);
                ctx.cmd.SetConstants(0, &params, sizeof(params));
                ctx.cmd.Draw(3);
            });
    }
}

} // namespace zx::render
