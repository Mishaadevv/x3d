#include "Render/Water.h"
#include <cmath>
#include "Core/Log/Log.h"
#include "Render/Sky.h"
#include "RHI/ShaderCompiler.h"

namespace zx::render {

using namespace rhi;

ZX_DEFINE_LOG_CATEGORY_STATIC(LogWater, Info);

namespace {

constexpr f32 Gravity = 9.81f;

struct WaterParams
{
    Mat4 viewProj;
    Mat4 invViewProj;
    Mat4 world;
    Mat4 worldToLocal;
    Mat4 shadowMatrices[4];
    Vec4 cascadeSplits;
    u32 shadowMaps[4];
    Vec3 cameraPosition;
    f32 time;
    Vec2 size;
    u32 ringMode;
    u32 grid;
    Vec2 ringCenter;
    f32 ringInner;
    f32 ringGrowth;
    u32 ringSegments;
    u32 cascadeCount;
    f32 nearPlane;
    f32 amplitude;
    Vec4 cascadeSizes;
    u32 displacement[4];
    u32 derivatives[4];
    Vec3 shallowColor;
    f32 clarity;
    Vec3 deepColor;
    f32 refraction;
    Vec3 scatterColor;
    f32 foam;
    Vec3 sunRadiance;
    f32 shoreFoam;
    Vec3 dirToSun;
    f32 roughness;
    Vec3 ambient;
    f32 caustics;
    Vec3 skyScale;
    u32 skyView;
    u32 skyCube;
    f32 skyCubeMips;
    f32 reflectionExposure;
    u32 ssr;
    u32 sceneColor;
    u32 sceneDepth;
    Vec2 invRenderSize;
    u32 cascadesShadow;
    f32 edgeFade;
    u32 ssrSteps;
    u32 causticTexture;
    u32 foamTexture;
    Vec3 pad;
};

struct CopyParams
{
    u32 srcColor, srcDepth, dstColor, dstDepth, width, height;
};

struct OceanParams
{
    u32 h0, specA, specB, displacement, derivatives, srcDisplacement, srcDerivatives, columns;
    f32 patchSize, time, choppiness, dt, foamDecay, foamThreshold, foamBias;
    u32 size;
};

// --- Spectrum ----------------------------------------------------------------------------------------------------------

/// JONSWAP frequency spectrum S(omega) for wind `u` (m/s) over `fetch` (m).
f32 Jonswap(f32 omega, f32 u, f32 fetch, f32& peak)
{
    const f32 g = Gravity;
    peak = 22.0f * std::pow(g * g / (u * fetch), 1.0f / 3.0f);
    const f32 alpha = 0.076f * std::pow(u * u / (fetch * g), 0.22f);
    const f32 sigma = omega <= peak ? 0.07f : 0.09f;
    const f32 r = std::exp(-(omega - peak) * (omega - peak) / (2.0f * sigma * sigma * peak * peak));
    return alpha * g * g / std::pow(omega, 5.0f) * std::exp(-1.25f * std::pow(peak / omega, 4.0f)) * std::pow(3.3f, r);
}

/// Peak wavelength of the sea state (m).
f32 PeakWavelength(f32 u, f32 fetch)
{
    f32 peak = 1.0f;
    Jonswap(1.0f, u, fetch, peak);
    return 2.0f * Pi * Gravity / (peak * peak);
}

/// Directional spreading around the wind (normalized over -pi..pi): mostly downwind, a little across.
f32 Spreading(f32 theta)
{
    const f32 c = std::cos(theta);
    return c > 0.0f ? (8.0f / (3.0f * Pi)) * c * c * c * c * 0.92f + 0.08f / (2.0f * Pi) : 0.08f / (2.0f * Pi);
}

/// Standard normal sample from a hash of (a, b, c) (Box-Muller).
Vec2 Gaussian(u32 a, u32 b, u32 c)
{
    auto hash = [](u32 x) {
        x ^= x >> 16;
        x *= 0x7FEB352Du;
        x ^= x >> 15;
        x *= 0x846CA68Bu;
        x ^= x >> 16;
        return x;
    };
    const u32 h1 = hash(a * 73856093u ^ hash(b * 19349663u ^ hash(c * 83492791u + 1u)));
    const u32 h2 = hash(h1 + 0x9E3779B9u);
    const f32 u1 = Max((f32(h1 >> 8) + 0.5f) / 16777216.0f, 1e-7f);
    const f32 u2 = (f32(h2 >> 8) + 0.5f) / 16777216.0f;
    const f32 r = std::sqrt(-2.0f * std::log(u1));
    return Vec2(r * std::cos(2.0f * Pi * u2), r * std::sin(2.0f * Pi * u2));
}

} // namespace

u64 WaterSpectrumKey(const WaterSurface& s)
{
    auto q = [](f32 v, f32 step) { return u64(i64(std::round(v / step)) & 0xFFFF); };
    return q(s.windSpeed, 0.25f) | (q(s.fetch, s.fetch < 1.0f ? 0.005f : 0.5f) << 16) | (q(s.choppiness, 0.05f) << 32) |
           (q(s.windDirection, 0.02f) << 48);
}

WaterRenderer::WaterRenderer(Device& device) : m_device(device) {}

WaterRenderer::~WaterRenderer()
{
    for (Simulation& s : m_sims)
        Destroy(s);
    for (Pipeline* p : {m_copyPipeline, m_pipeline, m_evolve, m_fft, m_assemble, m_mip, m_bake})
        m_device.Destroy(p);
    m_device.Destroy(m_causticTexture);
    m_device.Destroy(m_foamTexture);
}

void WaterRenderer::Destroy(Simulation& sim)
{
    for (u32 c = 0; c < Cascades; ++c) {
        m_device.Destroy(sim.h0[c]);
        m_device.Destroy(sim.specA[c]);
        m_device.Destroy(sim.specB[c]);
        m_device.Destroy(sim.displacement[c]);
        m_device.Destroy(sim.derivatives[c]);
        sim.h0[c] = nullptr;
        sim.specA[c] = sim.specB[c] = sim.displacement[c] = sim.derivatives[c] = nullptr;
    }
    sim.key = 0;
}

bool WaterRenderer::Init(Format colorFormat, Format depthFormat, bool lite)
{
    m_lite = lite;
    m_n = lite ? 64 : 128;
    m_grid = lite ? 96 : 128;
    m_rings = lite ? 64 : 96;
    m_segments = lite ? 128 : 192;
    const String n = String::Format("{}", m_n);
    struct Job
    {
        ShaderBytecode* out;
        StringView path;
        StringView entry;
        ShaderStage stage;
    } jobs[] = {
        {&m_copyCS, "/Shaders/Water/Water.hlsl", "CopySceneCS", ShaderStage::Compute},
        {&m_vs, "/Shaders/Water/Water.hlsl", "WaterVS", ShaderStage::Vertex},
        {&m_ps, "/Shaders/Water/Water.hlsl", "WaterPS", ShaderStage::Pixel},
        {&m_evolveCS, "/Shaders/Water/Ocean.hlsl", "EvolveCS", ShaderStage::Compute},
        {&m_fftCS, "/Shaders/Water/Ocean.hlsl", "FftCS", ShaderStage::Compute},
        {&m_assembleCS, "/Shaders/Water/Ocean.hlsl", "AssembleCS", ShaderStage::Compute},
        {&m_mipCS, "/Shaders/Water/Ocean.hlsl", "MipCS", ShaderStage::Compute},
        {&m_bakeCS, "/Shaders/Water/Water.hlsl", "BakePatternsCS", ShaderStage::Compute},
    };
    for (const Job& j : jobs) {
        ShaderCompileDesc d;
        d.path = j.path;
        d.entryPoint = j.entry;
        d.stage = j.stage;
        const ShaderDefine define{"OCEAN_N", n.View()};
        d.defines = Span<const ShaderDefine>(&define, 1);
        Result<ShaderBytecode> r = ShaderCompiler::Compile(d);
        if (!r) {
            ZX_LOG_ERROR(LogWater, "{}", r.GetError().message);
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
    m_copyPipeline = compute(m_copyCS, "Water.CopyScene");
    m_evolve = compute(m_evolveCS, "Ocean.Evolve");
    m_fft = compute(m_fftCS, "Ocean.FFT");
    m_assemble = compute(m_assembleCS, "Ocean.Assemble");
    m_mip = compute(m_mipCS, "Ocean.Mip");
    m_bake = compute(m_bakeCS, "Water.BakePatterns");
    TextureDesc pt;
    pt.width = pt.height = 256;
    pt.format = Format::R16_FLOAT;
    pt.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess;
    pt.debugName = "Water.Caustics";
    m_causticTexture = m_device.CreateTexture(pt);
    pt.debugName = "Water.Foam";
    m_foamTexture = m_device.CreateTexture(pt);
    GraphicsPipelineDesc pd;
    pd.vertexShader = &m_vs;
    pd.pixelShader = &m_ps;
    pd.rasterizer.cullMode = CullMode::None; // seen from below too
    pd.depthFormat = depthFormat;
    pd.colorFormats[0] = colorFormat;
    pd.colorCount = 1;
    pd.debugName = "Water";
    m_pipeline = m_device.CreateGraphicsPipeline(pd);
    return m_copyPipeline && m_pipeline && m_evolve && m_fft && m_assemble && m_mip && m_bake && m_causticTexture && m_foamTexture;
}

void WaterRenderer::Build(Simulation& sim, const WaterSurface& s, u64 key)
{
    Destroy(sim);
    sim.key = key;
    sim.choppiness = Clamp(s.choppiness, 0.0f, 2.0f);
    const f32 u = Clamp(s.windSpeed, 0.5f, 40.0f);
    const f32 fetch = Clamp(s.fetch, 0.005f, 2000.0f) * 1000.0f;
    // Cascade sizes from the peak wavelength: the swell, waves ~6x shorter, ripples ~6x shorter still.
    const f32 peak = PeakWavelength(u, fetch);
    sim.patchSize[0] = Clamp(peak * 3.5f, 4.0f, 2500.0f);
    sim.patchSize[1] = sim.patchSize[0] / 6.3f;
    sim.patchSize[2] = sim.patchSize[1] / 6.1f;
    const u32 n = m_n;
    const Vec2 wind(std::sin(s.windDirection), std::cos(s.windDirection));
    Array<Vec4> data;
    data.Resize(usize(n) * n);
    for (u32 c = 0; c < Cascades; ++c) {
        const f32 dk = 2.0f * Pi / sim.patchSize[c];
        // Each cascade keeps its own band of wavelengths (no wave counted twice).
        const f32 kMin = c == 0 ? 0.0f : 2.0f * Pi / sim.patchSize[c] * 6.0f;
        const f32 kMax = c + 1 < Cascades ? 2.0f * Pi / sim.patchSize[c + 1] * 6.0f : 1e9f;
        auto h0 = [&](i32 x, i32 y) {
            const Vec2 k = Vec2(f32(x - i32(n) / 2), f32(y - i32(n) / 2)) * dk;
            const f32 kLen = Length(k);
            if (kLen < 1e-5f || kLen < kMin || kLen >= kMax)
                return Vec2(0.0f);
            const f32 omega = std::sqrt(Gravity * kLen);
            f32 p;
            const f32 sOmega = Jonswap(omega, u, fetch, p);
            const f32 theta = std::atan2(k.y * wind.x - k.x * wind.y, Dot(k, wind) / 1.0f);
            const f32 dOmegaDk = Gravity / (2.0f * omega);
            // Damp the tiniest waves (they only alias) and convert S(omega, theta) -> S(k).
            const f32 tiny = std::exp(-kLen * kLen * 0.0004f);
            const f32 sK = sOmega * Spreading(theta) * dOmegaDk / kLen * tiny;
            const Vec2 xi = Gaussian(u32(x & 0xFFFF), u32(y & 0xFFFF), c + u32(key & 0xFF) * 7u);
            return xi * std::sqrt(Max(sK, 0.0f) * dk * dk * 0.5f);
        };
        for (u32 y = 0; y < n; ++y)
            for (u32 x = 0; x < n; ++x) {
                const Vec2 a = h0(i32(x), i32(y));
                const Vec2 b = h0(i32((n - x) % n), i32((n - y) % n)); // h0(-k)
                data[usize(y) * n + x] = Vec4(a.x, a.y, b.x, -b.y);  // conj
            }
        sim.h0[c] = m_device.CreateBuffer(BufferDesc{data.SizeBytes(), BufferUsage::ShaderResource, MemoryType::GpuOnly, 16, "Ocean.H0"},
                                          data.Data());
        TextureDesc td;
        td.width = td.height = n;
        td.format = Format::R32G32B32A32_FLOAT;
        td.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess;
        td.debugName = "Ocean.SpectrumA";
        sim.specA[c] = m_device.CreateTexture(td);
        td.debugName = "Ocean.SpectrumB";
        sim.specB[c] = m_device.CreateTexture(td);
        td.format = Format::R16G16B16A16_FLOAT;
        td.mipLevels = u32(std::log2(f32(n))) + 1;
        td.debugName = "Ocean.Displacement";
        sim.displacement[c] = m_device.CreateTexture(td);
        td.debugName = "Ocean.Derivatives";
        sim.derivatives[c] = m_device.CreateTexture(td);
    }
    sim.lastTime = -1.0f;
    ZX_LOG_INFO(LogWater, "Ocean spectrum: wind {:.1f} m/s, fetch {:.2f} km, peak wavelength {:.2f} m, cascades {:.1f} / {:.1f} / {:.2f} m", u,
                fetch / 1000.0f, peak, sim.patchSize[0], sim.patchSize[1], sim.patchSize[2]);
}

void WaterRenderer::AddPasses(RenderGraph& graph, RGTexture color, RGTexture depth, const RenderView& view, Span<const WaterSurface> surfaces,
                              const SceneLighting& lighting, const SceneRenderer::LightingInputs* inputs, const SkyRenderer* sky, f32 time,
                              f32 exposure)
{
    if (surfaces.Empty() || !m_pipeline)
        return;
    m_surfaces.Clear();
    m_surfaces.Append(surfaces);

    // Simulations: one per kind of water (at most MaxSimulations; extra kinds share the first).
    for (Simulation& s : m_sims)
        s.used = false;
    m_surfaceSim.Clear();
    for (const WaterSurface& s : m_surfaces) {
        const u64 key = WaterSpectrumKey(s);
        i32 index = -1;
        for (u32 i = 0; i < MaxSimulations && index < 0; ++i)
            if (m_sims[i].key == key && m_sims[i].displacement[0])
                index = i32(i);
        for (u32 i = 0; i < MaxSimulations && index < 0; ++i)
            if (!m_sims[i].used) { // a free slot (or one no surface needs this frame): rebuild it for this kind
                Build(m_sims[i], s, key);
                index = i32(i);
            }
        if (index < 0)
            index = 0; // more kinds than slots: share the first
        m_sims[index].used = true;
        m_surfaceSim.PushBack(u32(index));
    }

    // Patterns (once): the caustic web and foam bubbles.
    const RGTexture causticRG = graph.ImportTexture(m_causticTexture, ResourceState::ShaderResource);
    const RGTexture foamRG = graph.ImportTexture(m_foamTexture, ResourceState::ShaderResource);
    if (!m_patternsBaked) {
        m_patternsBaked = true;
        graph.AddPass(
            "WaterPatterns", PassType::Compute,
            [=](PassBuilder& b) {
                b.WriteTexture(causticRG);
                b.WriteTexture(foamRG);
            },
            [this](PassContext& ctx) {
                struct
                {
                    u32 caustics, foam, size, period;
                } p{m_causticTexture->GetUavIndex(), m_foamTexture->GetUavIndex(), 256, 8};
                ctx.cmd.SetPipeline(m_bake);
                ctx.cmd.SetPushConstants(p);
                ctx.cmd.Dispatch(32, 32);
            });
    }

    // Simulate: evolve, FFT rows + columns, assemble with foam, mips - every used simulation.
    Array<RGTexture> simTextures;
    for (Simulation& sim : m_sims)
        if (sim.used)
            for (u32 c = 0; c < Cascades; ++c) {
                simTextures.PushBack(graph.ImportTexture(sim.specA[c], ResourceState::ShaderResource));
                simTextures.PushBack(graph.ImportTexture(sim.specB[c], ResourceState::ShaderResource));
                simTextures.PushBack(graph.ImportTexture(sim.displacement[c], ResourceState::ShaderResource));
                simTextures.PushBack(graph.ImportTexture(sim.derivatives[c], ResourceState::ShaderResource));
            }
    const u32 n = m_n;
    graph.AddPass(
        "OceanSimulation", PassType::Compute,
        [simTextures](PassBuilder& b) {
            for (RGTexture t : simTextures)
                b.WriteTexture(t);
        },
        [this, n, time](PassContext& ctx) {
            CommandList& cmd = ctx.cmd;
            for (Simulation& sim : m_sims) {
                if (!sim.used)
                    continue;
                const f32 dt = sim.lastTime < 0.0f ? 0.0f : Clamp(time - sim.lastTime, 0.0f, 0.1f);
                sim.lastTime = time;
                for (u32 c = 0; c < Cascades; ++c) {
                    OceanParams p = {};
                    p.h0 = sim.h0[c]->GetSrvIndex();
                    p.specA = sim.specA[c]->GetUavIndex();
                    p.specB = sim.specB[c]->GetUavIndex();
                    p.displacement = sim.displacement[c]->GetUavIndex(0);
                    p.derivatives = sim.derivatives[c]->GetUavIndex(0);
                    p.patchSize = sim.patchSize[c];
                    p.time = time;
                    p.choppiness = sim.choppiness;
                    p.dt = dt;
                    p.foamDecay = 0.45f;     // whitecaps linger a couple of seconds
                    p.foamThreshold = 0.5f; // where the surface folds over (breaking crests)
                    p.foamBias = 2.5f;
                    cmd.SetPipeline(m_evolve);
                    cmd.SetPushConstants(p);
                    cmd.Dispatch(n / 8, n / 8);
                    cmd.Barrier(sim.specA[c], ResourceState::UnorderedAccess);
                    cmd.Barrier(sim.specB[c], ResourceState::UnorderedAccess);
                    cmd.SetPipeline(m_fft);
                    for (u32 pass = 0; pass < 2; ++pass) {
                        p.columns = pass;
                        cmd.SetPushConstants(p);
                        cmd.Dispatch(n);
                        cmd.Barrier(sim.specA[c], ResourceState::UnorderedAccess);
                        cmd.Barrier(sim.specB[c], ResourceState::UnorderedAccess);
                    }
                    cmd.SetPipeline(m_assemble);
                    cmd.SetPushConstants(p);
                    cmd.Dispatch(n / 8, n / 8);
                    cmd.Barrier(sim.displacement[c], ResourceState::UnorderedAccess);
                    cmd.Barrier(sim.derivatives[c], ResourceState::UnorderedAccess);
                    cmd.SetPipeline(m_mip);
                    u32 size = n / 2;
                    for (u32 mip = 1; size >= 1; ++mip, size /= 2) {
                        p.srcDisplacement = sim.displacement[c]->GetUavIndex(mip - 1);
                        p.srcDerivatives = sim.derivatives[c]->GetUavIndex(mip - 1);
                        p.displacement = sim.displacement[c]->GetUavIndex(mip);
                        p.derivatives = sim.derivatives[c]->GetUavIndex(mip);
                        p.size = size;
                        cmd.SetPushConstants(p);
                        cmd.Dispatch((size + 7) / 8, (size + 7) / 8);
                        cmd.Barrier(sim.displacement[c], ResourceState::UnorderedAccess);
                        cmd.Barrier(sim.derivatives[c], ResourceState::UnorderedAccess);
                    }
                }
            }
        });

    // Copies of the scene for refraction, thickness and reflections (the water then writes color and depth in place).
    TextureDesc cd;
    cd.width = view.width;
    cd.height = view.height;
    cd.format = Format::R16G16B16A16_FLOAT;
    cd.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess;
    cd.debugName = "WaterSceneColor";
    const RGTexture colorCopy = graph.CreateTexture(cd);
    cd.format = Format::R32_FLOAT;
    cd.debugName = "WaterSceneDepth";
    const RGTexture depthCopy = graph.CreateTexture(cd);
    const UVec2 size(view.width, view.height);
    graph.AddPass(
        "WaterCopy", PassType::Compute,
        [=](PassBuilder& b) {
            b.ReadTexture(color);
            b.ReadTexture(depth);
            b.WriteTexture(colorCopy);
            b.WriteTexture(depthCopy);
        },
        [=, this](PassContext& ctx) {
            CopyParams p{ctx.Texture(color)->GetSrvIndex(), ctx.Texture(depth)->GetSrvIndex(), ctx.Texture(colorCopy)->GetUavIndex(),
                         ctx.Texture(depthCopy)->GetUavIndex(), size.x, size.y};
            ctx.cmd.SetPipeline(m_copyPipeline);
            ctx.cmd.SetPushConstants(p);
            ctx.cmd.Dispatch((size.x + 7) / 8, (size.y + 7) / 8);
        });

    WaterParams base = {};
    base.viewProj = view.viewProj;
    base.invViewProj = Inverse(view.viewProj);
    base.cameraPosition = view.position;
    base.time = time;
    base.nearPlane = view.nearPlane;
    base.sunRadiance = lighting.sunColor * (lighting.sunIlluminance * exposure);
    base.dirToSun = -lighting.sunDirection;
    base.ambient = lighting.skyColor * (lighting.skyLuminance * exposure);
    base.invRenderSize = Vec2(1.0f / f32(view.width), 1.0f / f32(view.height));
    base.skyView = InvalidDescriptor;
    base.skyCube = lighting.skyCube;
    base.skyCubeMips = lighting.skyCubeMips;
    base.reflectionExposure = lighting.reflectionExposure;
    base.cascadeCount = Cascades;
    for (u32 c = 0; c < 4; ++c)
        base.shadowMaps[c] = InvalidDescriptor;
    if (inputs && inputs->cascadeCount > 0) {
        base.cascadesShadow = 1;
        base.cascadeSplits = inputs->cascadeSplits;
        for (u32 c = 0; c < Min(inputs->cascadeCount, 4u); ++c) {
            base.shadowMatrices[c] = inputs->shadowMatrices[c];
            base.shadowMaps[c] = inputs->shadowMaps[c];
        }
        for (u32 c = inputs->cascadeCount; c < 4; ++c)
            base.cascadeSplits[c] = -1.0f;
    }
    const RGTexture skyView = sky ? sky->SkyViewTexture() : RGTexture{};
    if (skyView.IsValid())
        base.skyScale = sky->SkyRadianceScale();
    Array<RGTexture> shadowTextures;
    if (inputs)
        for (u32 c = 0; c < inputs->cascadeCount && c < 4; ++c)
            if (inputs->shadowTextures[c].IsValid())
                shadowTextures.PushBack(inputs->shadowTextures[c]);
    graph.AddPass(
        "Water", PassType::Graphics,
        [=](PassBuilder& b) {
            b.ReadTexture(colorCopy);
            b.ReadTexture(depthCopy);
            if (skyView.IsValid())
                b.ReadTexture(skyView);
            for (RGTexture t : simTextures)
                b.ReadTexture(t);
            for (RGTexture t : shadowTextures)
                b.ReadTexture(t);
            b.ReadTexture(causticRG);
            b.ReadTexture(foamRG);
            b.RenderTarget(color, LoadOp::Load);
            b.DepthStencil(depth, LoadOp::Load);
        },
        [=, this](PassContext& ctx) {
            ctx.cmd.SetViewport(Viewport{0, 0, f32(size.x), f32(size.y), 0, 1});
            ctx.cmd.SetScissor(0, 0, size.x, size.y);
            ctx.cmd.SetPipeline(m_pipeline);
            for (usize si = 0; si < m_surfaces.Size(); ++si) {
                const WaterSurface& s = m_surfaces[si];
                const Simulation& sim = m_sims[m_surfaceSim[si]];
                WaterParams p = base;
                p.world = s.world;
                p.worldToLocal = Inverse(s.world);
                p.size = Vec2(Max(s.size.x, 0.1f), Max(s.size.y, 0.1f));
                p.amplitude = Max(s.waveAmount, 0.0f);
                for (u32 c = 0; c < Cascades; ++c) {
                    p.cascadeSizes[c] = sim.patchSize[c];
                    p.displacement[c] = sim.displacement[c]->GetSrvIndex();
                    p.derivatives[c] = sim.derivatives[c]->GetSrvIndex();
                }
                p.shallowColor = s.shallowColor;
                p.deepColor = s.deepColor;
                p.scatterColor = s.scatterColor;
                p.clarity = s.clarity;
                p.refraction = s.refraction;
                p.foam = s.foam;
                p.shoreFoam = s.shoreFoam;
                p.roughness = Clamp(s.roughness, 0.01f, 1.0f);
                p.caustics = Max(s.caustics, 0.0f);
                p.ssr = s.reflections ? 1u : 0u;
                p.ssrSteps = m_lite ? 14u : 24u;
                p.causticTexture = m_causticTexture->GetSrvIndex();
                p.foamTexture = m_foamTexture->GetSrvIndex();
                p.edgeFade = Min(3.0f, Min(p.size.x, p.size.y) * 0.1f);
                p.sceneColor = ctx.Texture(colorCopy)->GetSrvIndex();
                p.sceneDepth = ctx.Texture(depthCopy)->GetSrvIndex();
                if (skyView.IsValid())
                    p.skyView = ctx.Texture(skyView)->GetSrvIndex();
                // Mesh: rings around the camera for large water (dense near, coarse far), a grid for small.
                const Vec3 camLocal = p.worldToLocal.TransformPoint(view.position);
                const Vec2 half = p.size * 0.5f;
                const bool rings = Max(p.size.x, p.size.y) > 80.0f;
                u32 vertices;
                if (rings) {
                    const Vec2 center(Clamp(camLocal.x, -half.x, half.x), Clamp(camLocal.z, -half.y, half.y));
                    // Snap to a coarse grid so the rings don't crawl over the waves with every small camera move.
                    const f32 snap = 2.0f;
                    p.ringCenter = Vec2(std::floor(center.x / snap) * snap, std::floor(center.y / snap) * snap);
                    f32 extent = 0.0f;
                    for (f32 sx : {-1.0f, 1.0f})
                        for (f32 sz : {-1.0f, 1.0f})
                            extent = Max(extent, Length(Vec2(half.x * sx, half.y * sz) - p.ringCenter));
                    extent = Min(extent, 12000.0f);
                    p.ringInner = Max(0.3f, Abs(camLocal.y) * 0.02f);
                    p.ringGrowth = std::pow(Max(extent / p.ringInner, 1.01f), 1.0f / f32(m_rings - 1));
                    p.ringSegments = m_segments;
                    p.grid = m_rings;
                    p.ringMode = 1;
                    vertices = m_rings * m_segments * 6;
                } else {
                    p.grid = m_grid;
                    p.ringMode = 0;
                    vertices = m_grid * m_grid * 6;
                }
                ctx.cmd.SetConstants(0, &p, sizeof(p));
                ctx.cmd.Draw(vertices);
            }
        });
}

} // namespace zx::render
