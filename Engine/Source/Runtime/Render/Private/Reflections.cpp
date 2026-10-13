#include "Render/Reflections.h"
#include "Core/Log/Log.h"
#include "RHI/ShaderCompiler.h"

ZX_DEFINE_LOG_CATEGORY_STATIC(LogReflections, Info);

namespace zx::render {

using namespace rhi;

namespace {

struct GpuProbe
{
    Vec3 boxMin;
    u32 cube;
    Vec3 boxMax;
    f32 mips;
    Vec3 position;
    f32 blend;
    f32 intensity;
    u32 boxProjection;
    f32 pad[2];
};
static_assert(sizeof(GpuProbe) == 64);

// D3D cube face order +X, -X, +Y, -Y, +Z, -Z; the up vectors give the texture layout of each face (v down).
const Vec3 FaceForward[6] = {Vec3(1, 0, 0), Vec3(-1, 0, 0), Vec3(0, 1, 0), Vec3(0, -1, 0), Vec3(0, 0, 1), Vec3(0, 0, -1)};
const Vec3 FaceUp[6] = {Vec3(0, 1, 0), Vec3(0, 1, 0), Vec3(0, 0, -1), Vec3(0, 0, 1), Vec3(0, 1, 0), Vec3(0, 1, 0)};

constexpr u32 MaxGpuProbes = 32;

} // namespace

f32 ReflectionRenderer::ReferenceExposure() { return ExposureFromEV100(10.0f); }

u32 ReflectionRenderer::MipCount(u32 size)
{
    u32 mips = 1;
    while ((size >> mips) >= 4)
        ++mips;
    return mips;
}

ReflectionRenderer::ReflectionRenderer(Device& device) : m_device(device) {}

ReflectionRenderer::~ReflectionRenderer()
{
    for (Pipeline* p : {m_skyToCube, m_copyFace, m_prefilter, m_ssr})
        m_device.Destroy(p);
    for (Texture* t : {m_skyRaw, m_skyCube, m_captureRaw, m_ssrTextures[0], m_ssrTextures[1]})
        m_device.Destroy(t);
    for (Slot& s : m_slots)
        m_device.Destroy(s.cube);
    for (Buffer* b : m_probeBuffers)
        m_device.Destroy(b);
}

bool ReflectionRenderer::Init()
{
    struct Job
    {
        ShaderBytecode* out;
        Pipeline** pipeline;
        StringView entry;
    } jobs[] = {
        {&m_skyToCubeCS, &m_skyToCube, "SkyToCubeCS"},
        {&m_copyFaceCS, &m_copyFace, "CopyFaceCS"},
        {&m_prefilterCS, &m_prefilter, "PrefilterCS"},
        {&m_ssrCS, &m_ssr, "SsrTraceCS"},
    };
    for (const Job& j : jobs) {
        ShaderCompileDesc d;
        d.path = "/Shaders/Lighting/Reflections.hlsl";
        d.entryPoint = j.entry;
        d.stage = ShaderStage::Compute;
        Result<ShaderBytecode> r = ShaderCompiler::Compile(d);
        if (!r) {
            ZX_LOG_ERROR(LogReflections, "{}", r.GetError().message);
            return false;
        }
        *j.out = move(*r);
        ComputePipelineDesc cd;
        cd.computeShader = j.out;
        cd.debugName = j.entry;
        *j.pipeline = m_device.CreateComputePipeline(cd);
        if (!*j.pipeline)
            return false;
    }
    for (Buffer*& b : m_probeBuffers)
        b = m_device.CreateBuffer(BufferDesc{u64(MaxGpuProbes) * sizeof(GpuProbe), BufferUsage::ShaderResource, MemoryType::Upload, 0, "ReflectionProbes"});
    m_skySize = ReflectionSettings().skyResolution;
    m_skyRaw = CreateCube(m_skySize, 1, "SkyCubeRaw");
    m_skyCube = CreateCube(m_skySize, MipCount(m_skySize), "SkyCube");
    return m_skyRaw && m_skyCube && m_probeBuffers[0];
}

Texture* ReflectionRenderer::CreateCube(u32 size, u32 mips, StringView name)
{
    TextureDesc td;
    td.dimension = TextureDimension::TextureCube;
    td.width = size;
    td.height = size;
    td.depthOrArraySize = 1;
    td.mipLevels = mips;
    td.format = Format::R16G16B16A16_FLOAT;
    td.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess;
    td.debugName = name;
    return m_device.CreateTexture(td);
}

void ReflectionRenderer::RecaptureAll()
{
    for (Slot& s : m_slots)
        s.capturedKey = 0;
}

u32 ReflectionRenderer::CapturedProbes() const
{
    u32 n = 0;
    for (const Slot& s : m_slots)
        n += s.valid ? 1u : 0u;
    return n;
}

void ReflectionRenderer::Prepare(Span<const ReflectionProbeDesc> probes, const RenderView& view, f32 exposure, const ReflectionSettings& settings,
                                 u64 environmentKey, SceneLighting& lighting)
{
    ++m_frame;
    m_settings = settings;
    const u32 skySize = Clamp(settings.skyResolution, 16u, 512u);
    if (skySize != m_skySize) {
        Texture* raw = CreateCube(skySize, 1, "SkyCubeRaw");
        Texture* cube = CreateCube(skySize, MipCount(skySize), "SkyCube");
        if (raw && cube) {
            m_device.Destroy(m_skyRaw);
            m_device.Destroy(m_skyCube);
            m_skyRaw = raw;
            m_skyCube = cube;
            m_skySize = skySize;
            m_skyValid = false;
            m_skyDirty = true;
        } else {
            m_device.Destroy(raw);
            m_device.Destroy(cube);
        }
    }
    if (environmentKey != m_environmentKey) {
        m_environmentKey = environmentKey;
        m_skyDirty = true;
        RecaptureAll();
    }
    // --- Probe slots ---
    const u32 size = Clamp(settings.probeResolution, 16u, 512u);
    if (size != m_captureSize) {
        // Resolution changed: new cubes for everything.
        m_device.Destroy(m_captureRaw);
        m_captureRaw = nullptr;
        for (Slot& s : m_slots)
            m_device.Destroy(s.cube);
        m_slots.Clear();
        m_capture = -1;
        m_captureSize = size;
    }
    for (Slot& s : m_slots)
        s.seen = false;
    const u32 limit = Min(settings.maxProbes, MaxGpuProbes);
    if (settings.probes) {
        for (const ReflectionProbeDesc& p : probes) {
            Slot* slot = nullptr;
            for (Slot& s : m_slots)
                if (s.desc.id == p.id)
                    slot = &s;
            if (!slot) {
                if (m_slots.Size() >= limit)
                    continue;
                Slot s;
                s.cube = CreateCube(size, MipCount(size), "ReflectionProbe");
                if (!s.cube)
                    continue;
                m_slots.PushBack(s);
                slot = &m_slots.Back();
            }
            slot->desc = p;
            slot->seen = true;
        }
    }
    for (usize i = 0; i < m_slots.Size();) {
        if (m_slots[i].seen) {
            ++i;
            continue;
        }
        m_device.Destroy(m_slots[i].cube);
        if (m_capture == i32(i))
            m_capture = -1;
        else if (m_capture > i32(i))
            --m_capture;
        m_slots.RemoveAt(i);
    }
    auto captureKey = [](const Slot& s) { return s.desc.key ^ 0x9E3779B97F4A7C15ull; };
    // A changed probe restarts its capture.
    if (m_capture >= 0 && captureKey(m_slots[usize(m_capture)]) != m_captureKey)
        m_capture = -1;
    if (m_capture < 0) {
        // Next probe to capture: the nearest one whose capture is missing or out of date.
        f32 best = F32Max;
        for (usize i = 0; i < m_slots.Size(); ++i) {
            const Slot& s = m_slots[i];
            if (s.capturedKey == captureKey(s) && s.valid)
                continue;
            const f32 d = LengthSq(s.desc.position - view.position);
            if (d < best) {
                best = d;
                m_capture = i32(i);
            }
        }
        if (m_capture >= 0) {
            m_captureFace = 0;
            m_captureKey = captureKey(m_slots[usize(m_capture)]);
            if (!m_captureRaw)
                m_captureRaw = CreateCube(size, 1, "ProbeCapture");
        }
    }

    // --- GPU table: valid probes, higher priority first, then smaller boxes (more local) first ---
    Array<const Slot*> order;
    for (const Slot& s : m_slots)
        if (s.valid)
            order.PushBack(&s);
    auto volume = [](const Slot* s) {
        const Vec3 e = s->desc.boxMax - s->desc.boxMin;
        return e.x * e.y * e.z;
    };
    for (usize i = 1; i < order.Size(); ++i)
        for (usize j = i; j > 0; --j) {
            const Slot* a = order[j];
            const Slot* b = order[j - 1];
            const bool before = a->desc.priority > b->desc.priority || (a->desc.priority == b->desc.priority && volume(a) < volume(b));
            if (!before)
                break;
            order[j] = b;
            order[j - 1] = a;
        }
    Buffer* buffer = m_probeBuffers[m_device.GetFrameIndex() % FramesInFlight];
    GpuProbe* gpu = static_cast<GpuProbe*>(buffer->GetMappedData());
    const u32 count = Min<u32>(u32(order.Size()), MaxGpuProbes);
    for (u32 i = 0; i < count; ++i) {
        const ReflectionProbeDesc& d = order[i]->desc;
        GpuProbe g = {};
        g.boxMin = d.boxMin;
        g.boxMax = d.boxMax;
        g.cube = order[i]->cube->GetSrvIndex();
        g.mips = f32(MipCount(size) - 1);
        g.position = d.position;
        g.blend = Max(d.blendDistance, 0.01f);
        g.intensity = d.intensity;
        g.boxProjection = d.boxProjection ? 1u : 0u;
        gpu[i] = g;
    }

    // --- Bindings ---
    lighting.skyCube = settings.sky && m_skyValid ? m_skyCube->GetSrvIndex() : InvalidDescriptor;
    lighting.skyCubeMips = f32(MipCount(m_skySize) - 1);
    lighting.probeBuffer = count ? buffer->GetSrvIndex() : InvalidDescriptor;
    lighting.probeCount = count;
    lighting.reflectionExposure = exposure / ReferenceExposure();
    // SSR: last frame's trace (when the render size didn't change).
    const UVec2 ssrSize = settings.ssrHalfResolution ? UVec2(Max(1u, (view.width + 1) / 2), Max(1u, (view.height + 1) / 2))
                                                     : UVec2(view.width, view.height);
    if (!settings.ssr || ssrSize != m_ssrSize)
        m_ssrValid = false;
    lighting.ssrTexture = m_ssrValid ? m_ssrTextures[m_ssrWrite]->GetSrvIndex() : InvalidDescriptor;
    lighting.ssrViewProj = m_ssrViewProj;
    lighting.ssrMaxRoughness = settings.ssrMaxRoughness;
    if (settings.ssr) {
        if (ssrSize != m_ssrSize) {
            for (Texture*& t : m_ssrTextures) {
                m_device.Destroy(t);
                TextureDesc td;
                td.width = ssrSize.x;
                td.height = ssrSize.y;
                td.format = Format::R16G16B16A16_FLOAT;
                td.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess;
                td.debugName = "SSR";
                t = m_device.CreateTexture(td);
            }
            m_ssrSize = ssrSize;
        }
        m_ssrWrite ^= 1; // this frame traces into the other texture
    }
}

bool ReflectionRenderer::CaptureView(RenderView& face) const
{
    if (m_capture < 0 || !m_captureRaw)
        return false;
    const Slot& s = m_slots[usize(m_capture)];
    face.view = Mat4::LookTo(s.desc.position, FaceForward[m_captureFace], FaceUp[m_captureFace]);
    face.nearPlane = 0.05f;
    face.fovY = HalfPi;
    face.projection = Mat4::PerspectiveInfiniteReversedZ(face.fovY, 1.0f, face.nearPlane);
    face.viewProj = face.projection * face.view;
    face.position = s.desc.position;
    face.width = m_captureSize;
    face.height = m_captureSize;
    return true;
}

void ReflectionRenderer::AddSsrPass(RenderGraph& graph, RGTexture color, RGTexture depth, RGTexture normals, const RenderView& view)
{
    if (!m_settings.ssr || !normals.IsValid() || !m_ssrTextures[m_ssrWrite])
        return;
    const RGTexture target = graph.ImportTexture(m_ssrTextures[m_ssrWrite], ResourceState::ShaderResource);
    Params p = {};
    p.invViewProj = Inverse(view.viewProj);
    p.viewProj = view.viewProj;
    p.cameraPosition = view.position;
    p.cameraForward = Normalize(Vec3(view.view.columns[0].z, view.view.columns[1].z, view.view.columns[2].z));
    p.nearZ = view.nearPlane;
    p.targetSize = Vec2(f32(m_ssrSize.x), f32(m_ssrSize.y));
    p.sceneSize = Vec2(f32(view.width), f32(view.height));
    p.steps = m_settings.ssrSteps;
    p.maxDistance = m_settings.ssrMaxDistance;
    p.thickness = m_settings.ssrThickness;
    p.maxRoughness = m_settings.ssrMaxRoughness;
    p.frame = m_frame;
    p.sky = InvalidDescriptor;
    const UVec2 size = m_ssrSize;
    graph.AddPass(
        "SSR", PassType::Compute,
        [=](PassBuilder& b) {
            b.ReadTexture(color);
            b.ReadTexture(depth);
            b.ReadTexture(normals);
            b.WriteTexture(target);
            b.SideEffects(); // read by the next frame
        },
        [this, p, color, depth, normals, target, size](PassContext& ctx) {
            Params q = p;
            q.input = ctx.Texture(color)->GetSrvIndex();
            q.depth = ctx.Texture(depth)->GetSrvIndex();
            q.normals = ctx.Texture(normals)->GetSrvIndex();
            q.output = ctx.Texture(target)->GetUavIndex();
            ctx.cmd.SetPipeline(m_ssr);
            ctx.cmd.SetConstants(0, &q, sizeof(q));
            ctx.cmd.Dispatch((size.x + 7) / 8, (size.y + 7) / 8);
        });
    m_ssrViewProj = view.viewProj;
    m_ssrValid = true;
}

void ReflectionRenderer::AddPrefilterPasses(RenderGraph& graph, RGTexture source, RGTexture target, Texture* targetTexture, StringView name)
{
    const u32 mips = targetTexture->GetDesc().mipLevels;
    const u32 size0 = targetTexture->GetDesc().width;
    for (u32 m = 0; m < mips; ++m) {
        Params p = {};
        p.size = Max(1u, size0 >> m);
        p.roughness = mips > 1 ? f32(m) / f32(mips - 1) : 0.0f;
        p.samples = m == 0 ? 1u : (m < 3 ? 32u : 64u);
        graph.AddPass(
            name.Data(), PassType::Compute,
            [source, target](PassBuilder& b) {
                b.ReadTexture(source);
                b.WriteTexture(target);
                b.SideEffects();
            },
            [this, p, source, targetTexture, m](PassContext& ctx) {
                Params q = p;
                q.input = ctx.Texture(source)->GetSrvIndex();
                q.output = targetTexture->GetUavIndex(m);
                ctx.cmd.SetPipeline(m_prefilter);
                ctx.cmd.SetConstants(0, &q, sizeof(q));
                ctx.cmd.Dispatch((q.size + 7) / 8, (q.size + 7) / 8, 6);
            });
    }
}

void ReflectionRenderer::AddUpdatePasses(RenderGraph& graph, RGTexture skyLut, Vec3 skyScale, RGTexture faceColor, RGTexture faceDepth, f32 exposure,
                                         const HdriSkySettings* hdri)
{
    // Sky-view LUT values times skyScale are pre-exposed with `exposure`: store them at the reference exposure.
    const Vec3 skyRef = skyScale * (ReferenceExposure() / Max(exposure, 1e-12f));
    // --- Sky cube (when the sun / sky changed) ---
    if (m_settings.sky && m_skyDirty && skyLut.IsValid()) {
        const RGTexture raw = graph.ImportTexture(m_skyRaw, ResourceState::ShaderResource);
        const RGTexture cube = graph.ImportTexture(m_skyCube, ResourceState::ShaderResource);
        Params p = {};
        p.size = m_skySize;
        p.skyScale = skyRef;
        // HDRI: sample the panorama itself (about one texel per cube texel), the sun clamped like in the LUT.
        struct HdriPush
        {
            u32 texture;
            f32 rotation;
            f32 lod;
            f32 clampCos;
            Vec3 clampDir;
            f32 clampLuminance;
        } hp = {InvalidDescriptor, 0.0f, 0.0f, 2.0f, Vec3(0, 1, 0), 0.0f};
        if (hdri && hdri->IsActive()) {
            hp.texture = hdri->texture;
            hp.rotation = hdri->rotation;
            hp.lod = Max(0.0f, Log2(f32(Max(hdri->width, 1u)) / f32(4 * m_skySize)));
            hp.clampCos = hdri->clampCos;
            hp.clampDir = hdri->clampDirection;
            hp.clampLuminance = hdri->clampLuminance;
            p.skyScale = Vec3(hdri->intensity * ReferenceExposure());
        }
        graph.AddPass(
            "SkyToCube", PassType::Compute,
            [skyLut, raw](PassBuilder& b) {
                b.ReadTexture(skyLut);
                b.WriteTexture(raw);
                b.SideEffects();
            },
            [this, p, hp, skyLut](PassContext& ctx) {
                Params q = p;
                q.input = ctx.Texture(skyLut)->GetSrvIndex();
                q.output = m_skyRaw->GetUavIndex(0);
                ctx.cmd.SetPipeline(m_skyToCube);
                ctx.cmd.SetConstants(0, &q, sizeof(q));
                ctx.cmd.SetPushConstants(hp);
                ctx.cmd.Dispatch((q.size + 7) / 8, (q.size + 7) / 8, 6);
            });
        AddPrefilterPasses(graph, raw, cube, m_skyCube, "SkyCubePrefilter");
        m_skyDirty = false;
        m_skyValid = true;
    }
    // --- Probe capture: this frame's face, then prefiltering after the last one ---
    if (m_capture < 0 || !m_captureRaw || !faceColor.IsValid() || !faceDepth.IsValid())
        return;
    const RGTexture raw = graph.ImportTexture(m_captureRaw, ResourceState::ShaderResource);
    Params p = {};
    p.size = m_captureSize;
    p.face = m_captureFace;
    p.skyScale = skyRef;
    p.exposureScale = ReferenceExposure() / Max(exposure, 1e-12f);
    const bool hasSky = skyLut.IsValid();
    graph.AddPass(
        "ProbeCaptureFace", PassType::Compute,
        [=](PassBuilder& b) {
            b.ReadTexture(faceColor);
            b.ReadTexture(faceDepth);
            if (hasSky)
                b.ReadTexture(skyLut);
            b.WriteTexture(raw);
            b.SideEffects();
        },
        [this, p, faceColor, faceDepth, skyLut, hasSky](PassContext& ctx) {
            Params q = p;
            q.input = ctx.Texture(faceColor)->GetSrvIndex();
            q.depth = ctx.Texture(faceDepth)->GetSrvIndex();
            q.sky = hasSky ? ctx.Texture(skyLut)->GetSrvIndex() : InvalidDescriptor;
            q.output = m_captureRaw->GetUavIndex(0);
            ctx.cmd.SetPipeline(m_copyFace);
            ctx.cmd.SetConstants(0, &q, sizeof(q));
            ctx.cmd.Dispatch((q.size + 7) / 8, (q.size + 7) / 8);
        });
    if (++m_captureFace < 6)
        return;
    Slot& slot = m_slots[usize(m_capture)];
    const RGTexture cube = graph.ImportTexture(slot.cube, ResourceState::ShaderResource);
    AddPrefilterPasses(graph, raw, cube, slot.cube, "ProbePrefilter");
    slot.valid = true;
    slot.capturedKey = m_captureKey;
    m_capture = -1;
}

} // namespace zx::render
