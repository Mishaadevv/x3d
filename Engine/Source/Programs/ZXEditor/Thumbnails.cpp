#include "Thumbnails.h"
#include "Core/Log/Log.h"
#include "Core/String/Path.h"
#include "Engine/BuiltinAssets.h"
#include "Engine/SceneSerialization.h"
#include "RHI/ShaderCompiler.h"

ZX_DEFINE_LOG_CATEGORY_STATIC(LogThumbnails, Info);

namespace zx::editor {

using namespace rhi;
using namespace render;

namespace {

struct BlitPush
{
    u32 source;
    u32 encodeSRGB;
    Vec2 scale;
};

bool IsMaterialPath(StringView p) { return Path::HasExtension(p, ".zmat") || Path::HasExtension(p, ".zmati"); }

/// Rendered through the model path (a private world): meshes and materials (on a sphere).
bool IsModelPath(StringView p)
{
    return Path::HasExtension(p, ".gltf") || Path::HasExtension(p, ".glb") || Path::HasExtension(p, ".obj") || IsMaterialPath(p);
}

bool IsImagePath(StringView p)
{
    return Path::HasExtension(p, ".png") || Path::HasExtension(p, ".jpg") || Path::HasExtension(p, ".jpeg") ||
           Path::HasExtension(p, ".tga") || Path::HasExtension(p, ".bmp") || Path::HasExtension(p, ".hdr");
}

bool IsSrgbFormat(Format f)
{
    return f == Format::R8G8B8A8_SRGB || f == Format::B8G8R8A8_SRGB || f == Format::BC1_SRGB || f == Format::BC3_SRGB ||
           f == Format::BC7_SRGB;
}

constexpr u64 WantedFrames = 3; // a tile counts as visible for this many frames after Get()
constexpr u32 MaxModelWait = 600;

} // namespace

ThumbnailCache::ThumbnailCache(Device& device, asset::AssetManager& assets, RenderAssets& renderAssets)
    : m_device(device), m_assets(assets), m_renderAssets(renderAssets)
{
}

ThumbnailCache::~ThumbnailCache()
{
    for (Entry& e : m_entries)
        m_device.Destroy(e.texture);
    m_device.Destroy(m_blitPipeline);
}

bool ThumbnailCache::Supports(StringView path) { return IsModelPath(path) || IsImagePath(path); }

bool ThumbnailCache::Init()
{
    ShaderCompileDesc sd;
    sd.path = "/Shaders/Editor/Thumbnail.hlsl";
    sd.entryPoint = "FullscreenVS";
    sd.stage = ShaderStage::Vertex;
    Result<ShaderBytecode> vs = ShaderCompiler::Compile(sd);
    sd.entryPoint = "BlitPS";
    sd.stage = ShaderStage::Pixel;
    Result<ShaderBytecode> ps = ShaderCompiler::Compile(sd);
    if (!vs || !ps) {
        ZX_LOG_ERROR(LogThumbnails, "{}", !vs ? vs.GetError().message : ps.GetError().message);
        return false;
    }
    GraphicsPipelineDesc pd;
    pd.vertexShader = &*vs;
    pd.pixelShader = &*ps;
    pd.rasterizer.cullMode = CullMode::None;
    pd.depthStencil.depthTest = false;
    pd.depthStencil.depthWrite = false;
    pd.colorFormats[0] = Format::R8G8B8A8_UNORM;
    pd.colorCount = 1;
    pd.debugName = "Thumbnail.Blit";
    m_blitPipeline = m_device.CreateGraphicsPipeline(pd);

    // Model renderer: Lite settings, no temporal accumulation (one frame per thumbnail), crisp at full resolution.
    m_renderer.Reset(new ViewportRenderer(m_device, m_renderAssets));
    if (!m_renderer->Init(true, Format::R8G8B8A8_UNORM))
        return false;
    ViewportSettings& s = m_renderer->Settings();
    s.taa.enabled = false;
    s.renderScale = 1.0f;
    s.scene.ao.enabled = false;
    scene::InstallHierarchyHooks(m_world);
    return m_blitPipeline != nullptr;
}

void ThumbnailCache::Invalidate(StringView physicalPath)
{
    if (const u32* index = m_index.Find(String(physicalPath))) {
        Entry& e = m_entries[*index];
        if (e.state == State::Ready || e.state == State::Failed)
            e.state = State::Queued;
    }
}

u32 ThumbnailCache::Get(StringView physicalPath)
{
    if (!Supports(physicalPath))
        return InvalidDescriptor;
    const String key(physicalPath);
    u32 index;
    if (const u32* found = m_index.Find(key)) {
        index = *found;
    } else {
        index = u32(m_entries.Size());
        Entry& e = m_entries.AddDefaulted();
        e.path = key;
        e.model = IsModelPath(key);
        m_index.InsertOrAssign(key, index);
    }
    Entry& e = m_entries[index];
    e.lastWanted = m_frame;
    return e.state == State::Ready ? e.texture->GetSrvIndex() : InvalidDescriptor;
}

bool ThumbnailCache::EnsureTexture(Entry& e)
{
    if (e.texture)
        return true;
    TextureDesc td;
    td.width = td.height = Size;
    td.format = Format::R8G8B8A8_UNORM;
    td.usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource;
    td.debugName = "Thumbnail";
    e.texture = m_device.CreateTexture(td);
    return e.texture != nullptr;
}

bool ThumbnailCache::UpdateImage(RenderGraph& graph, Array<RGTexture>& sampled, Entry& e)
{
    if (!e.id.IsValid()) {
        e.id = m_assets.LoadPath(e.path);
        e.state = e.id.IsValid() ? State::Loading : State::Failed;
        return false;
    }
    if (m_assets.GetState(e.id) == asset::AssetState::Failed) {
        e.state = State::Failed;
        return false;
    }
    Texture* source = m_renderAssets.GetTexture(e.id);
    if (!source || !EnsureTexture(e))
        return false;
    const TextureDesc& sd = source->GetDesc();
    BlitPush push{source->GetSrvIndex(), IsSrgbFormat(sd.format) ? 1u : 0u, Vec2(1.0f)};
    // Aspect fit inside the square.
    if (sd.width > sd.height)
        push.scale.y = f32(sd.width) / f32(Max(sd.height, 1u));
    else
        push.scale.x = f32(sd.height) / f32(Max(sd.width, 1u));
    const RGTexture target = graph.ImportTexture(e.texture, ResourceState::ShaderResource);
    graph.AddPass(
        "Thumbnail.Image", PassType::Graphics, [target](PassBuilder& b) { b.RenderTarget(target, LoadOp::DontCare); },
        [this, push](PassContext& ctx) {
            ctx.cmd.SetViewport(Viewport{0, 0, f32(Size), f32(Size), 0, 1});
            ctx.cmd.SetScissor(0, 0, Size, Size);
            ctx.cmd.SetPipeline(m_blitPipeline);
            ctx.cmd.SetPushConstants(push);
            ctx.cmd.Draw(3);
        });
    sampled.PushBack(target);
    e.state = State::Ready;
    return true;
}

void ThumbnailCache::UpdateModel(RenderGraph& graph, Array<RGTexture>& sampled)
{
    if (m_modelJob < 0) {
        for (usize i = 0; i < m_entries.Size(); ++i) {
            Entry& e = m_entries[i];
            if (e.model && e.state == State::Queued && e.lastWanted + WantedFrames >= m_frame) {
                if (IsMaterialPath(e.path)) {
                    MaterialLibrary::Material* mat = m_materials ? m_materials->FindByPath(e.path) : nullptr;
                    e.id = mat ? mat->guid : (m_materials ? m_materials->LoadFile(e.path) : Guid());
                } else {
                    e.id = m_assets.LoadPath(e.path);
                }
                if (!e.id.IsValid()) {
                    e.state = State::Failed;
                    continue;
                }
                e.state = State::Loading;
                m_modelJob = i32(i);
                m_modelInstantiated = false;
                m_modelWait = 0;
                ClearScene(m_world);
                break;
            }
        }
        if (m_modelJob < 0)
            return;
    }
    Entry& e = m_entries[usize(m_modelJob)];
    auto finish = [&](State state) {
        e.state = state;
        m_modelJob = -1;
    };
    if (++m_modelWait > MaxModelWait)
        return finish(State::Failed);
    const bool material = IsMaterialPath(e.path);
    if (!m_modelInstantiated && material) {
        const ecs::Entity sphere = scene::CreateNode(m_world, "Preview");
        MeshRenderer mr;
        mr.mesh = builtin::Mesh(BuiltinShape::Sphere);
        mr.materials.PushBack(e.id);
        m_world.Add(sphere, mr);
        scene::UpdateWorldTransforms(m_world);
        m_modelInstantiated = true;
    }
    if (material && m_materials && m_materials->ShaderState(e.id) == render::MaterialShaderCache::State::Compiling)
        return; // wait for the graph's shader
    if (!m_modelInstantiated) {
        const asset::PrefabAsset* prefab = m_assets.GetPrefab(e.id);
        if (!prefab) {
            if (m_assets.GetState(e.id) == asset::AssetState::Failed)
                finish(State::Failed);
            return;
        }
        InstantiatePrefab(m_world, *prefab);
        scene::UpdateWorldTransforms(m_world);
        m_modelInstantiated = true;
    }

    // Wait for the meshes (and give materials/textures a moment) before rendering the single frame.
    bool resident = true;
    AABB bounds;
    m_world.Query<const MeshRenderer, const scene::WorldTransform>().Each([&](const MeshRenderer& mr, const scene::WorldTransform& wt) {
        for (const asset::AssetId& m : mr.materials)
            m_renderAssets.GetMaterialIndex(m);
        const GpuMesh* mesh = m_renderAssets.GetMesh(mr.mesh);
        if (!mesh) {
            resident = false;
            return;
        }
        bounds.Expand(mesh->bounds.Transformed(wt.matrix));
    });
    if (!resident || (m_renderAssets.GetStats().pendingUploads > 0 && m_modelWait < 120))
        return;
    if (!EnsureTexture(e))
        return finish(State::Failed);

    // Frame the bounds from the front-right, slightly above.
    const Vec3 center = bounds.IsValid() ? bounds.Center() : Vec3::Zero();
    const f32 radius = bounds.IsValid() ? Max(Length(bounds.Extents()), 0.01f) : 1.0f;
    const f32 fov = DegToRad(30.0f);
    const Vec3 dir = material ? Normalize(Vec3(-0.3f, 0.35f, -1.0f)) : Normalize(Vec3(0.55f, 0.35f, 0.75f));
    const Vec3 eye = center + dir * (radius / Sin(fov * 0.5f) * 0.8f); // the bounding sphere is loose: frame a bit tighter
    ViewportCamera cam;
    cam.view = Mat4::LookAt(eye, center, Vec3::Up());
    cam.position = eye;
    cam.fovY = fov;
    cam.nearPlane = Max(radius * 0.01f, 0.001f);
    ViewportEnvironment env;
    env.sunElevation = DegToRad(40.0f);
    env.sunAzimuth = DegToRad(35.0f);
    env.autoExposure = false;
    const RGTexture target = graph.ImportTexture(e.texture, ResourceState::ShaderResource);
    m_renderer->ResetHistory();
    m_renderer->AddPasses(graph, m_world, cam, env, target, UVec2(Size, Size), 1.0f / 60.0f);
    sampled.PushBack(target);
    finish(State::Ready);
}

void ThumbnailCache::Update(RenderGraph& graph, Array<RGTexture>& sampled)
{
    ++m_frame;
    // One image per frame (cheap blit; the texture upload itself is budgeted by RenderAssets).
    for (Entry& e : m_entries)
        if (!e.model && (e.state == State::Queued || e.state == State::Loading) && e.lastWanted + WantedFrames >= m_frame &&
            UpdateImage(graph, sampled, e))
            break;
    UpdateModel(graph, sampled);
}

} // namespace zx::editor
