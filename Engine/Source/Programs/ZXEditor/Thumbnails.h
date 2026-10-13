#pragma once

// Content Browser thumbnails: 128x128 sRGB-encoded textures generated on demand, at most one image and one model per
// frame. Images are blitted from the texture asset; models are instantiated into a private world and rendered by a
// small ViewportRenderer (no TAA, fixed exposure), framed on their bounds.

#include "Engine/Materials.h"
#include "Engine/ViewportRenderer.h"
#include "Render/RenderAssets.h"

namespace zx::editor {

class ThumbnailCache
{
public:
    ThumbnailCache(rhi::Device& device, asset::AssetManager& assets, render::RenderAssets& renderAssets);
    ~ThumbnailCache();
    ZX_NON_COPYABLE(ThumbnailCache)

    bool Init();
    static bool Supports(StringView path);
    /// Material graphs (.zmat) are previewed on a sphere; needs the project's library.
    void SetMaterialLibrary(MaterialLibrary* library) { m_materials = library; }
    /// Regenerates the thumbnail of a file (e.g. after saving a material).
    void Invalidate(StringView physicalPath);

    /// Bindless SRV of the thumbnail, or InvalidDescriptor while it is being generated (the request is queued; only
    /// files asked for recently - i.e. visible tiles - are processed).
    u32 Get(StringView physicalPath);
    /// Records this frame's generation passes. Rendered thumbnails are appended to `sampled` (read by the UI pass).
    void Update(render::RenderGraph& graph, Array<render::RGTexture>& sampled);

    static constexpr u32 Size = 128;

private:
    enum class State : u8
    {
        Queued,
        Loading,
        Ready,
        Failed,
    };
    struct Entry
    {
        String path;
        bool model = false;
        asset::AssetId id;
        State state = State::Queued;
        rhi::Texture* texture = nullptr;
        u64 lastWanted = 0;
    };

    bool EnsureTexture(Entry& e);
    bool UpdateImage(render::RenderGraph& graph, Array<render::RGTexture>& sampled, Entry& e);
    void UpdateModel(render::RenderGraph& graph, Array<render::RGTexture>& sampled);

    rhi::Device& m_device;
    asset::AssetManager& m_assets;
    render::RenderAssets& m_renderAssets;
    rhi::Pipeline* m_blitPipeline = nullptr;
    UniquePtr<ViewportRenderer> m_renderer;
    ecs::World m_world;
    Array<Entry> m_entries;
    HashMap<String, u32> m_index;
    u64 m_frame = 0;
    MaterialLibrary* m_materials = nullptr;
    i32 m_modelJob = -1;
    bool m_modelInstantiated = false;
    u32 m_modelWait = 0;
};

} // namespace zx::editor
