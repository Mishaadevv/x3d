#pragma once

// Material Editor: node canvas for material graphs (.zmat) with a live preview. Every edit is applied to the renderer
// at once (values without recompiling; structural edits after the background shader compile).

#include "Engine/BuiltinAssets.h"
#include "Engine/ViewportRenderer.h"
#include "UI/UI.h"

namespace zx::editor {

class Editor;

/// Renders a shape with the edited material into a small texture (own world and renderer, fixed lighting).
class MaterialPreview
{
public:
    MaterialPreview(rhi::Device& device, render::RenderAssets& renderAssets);
    ~MaterialPreview();
    ZX_NON_COPYABLE(MaterialPreview)

    bool Init();
    void SetMaterial(const Guid& material, BuiltinShape shape);
    void AddPasses(render::RenderGraph& graph, Array<render::RGTexture>& sampled, f32 dt);
    u32 Srv() const { return m_texture ? m_texture->GetSrvIndex() : rhi::InvalidDescriptor; }

    f32 yaw = 0.6f, pitch = 0.35f;
    static constexpr u32 Size = 384;

private:
    rhi::Device& m_device;
    render::RenderAssets& m_renderAssets;
    UniquePtr<ViewportRenderer> m_renderer;
    ecs::World m_world;
    ecs::Entity m_entity;
    rhi::Texture* m_texture = nullptr;
};

struct MaterialEditorState
{
    bool open = false;
    Guid material;
    Vec2 pan = Vec2(260.0f, 120.0f); // graph units
    f32 zoom = 1.0f;
    Array<u32> selection;

    enum class Drag : u8
    {
        None,
        Nodes,
        Box,
        Link,
        Pan,
    };
    Drag drag = Drag::None;
    Vec2 dragStart; // screen
    bool dragMoved = false;
    u32 linkNode = 0, linkPin = 0;
    bool linkFromOutput = true;
    String beforeDrag; // graph JSON at drag start (undo)

    Array<String> undo, redo; // graph JSON snapshots
    String clipboard;

    // "Add node" popup.
    String addSearch;
    Vec2 addPosition; // graph space
    bool addHasLink = false;
    u32 addLinkNode = 0, addLinkPin = 0;
    bool addLinkFromOutput = true;
    Vec2 rightPress;

    UniquePtr<MaterialPreview> preview;
    BuiltinShape previewShape = BuiltinShape::Sphere;
    bool widgetEdit = false;   // a property widget edit is in progress (one undo step)
    bool capturesKeys = false; // the canvas has the keyboard this frame (scene shortcuts are suspended)
    bool frameRequested = true;
};

void OpenMaterialEditor(Editor& ed, const Guid& material);
void DrawMaterialEditorPanel(Editor& ed);
void RenderMaterialPreview(Editor& ed, render::RenderGraph& graph, Array<render::RGTexture>& sampled);

inline constexpr StringView MaterialEditorTitle = ZX_ICON_PALETTE "  Material Editor";

} // namespace zx::editor
