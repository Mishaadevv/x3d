#pragma once

// Animation editors:
//   Animation Viewer - an animated model (.glb / .gltf with a skin) in its own preview: clip list, timeline with
//                      scrubbing, speed, loop, orbit camera.
//   AnimGraph Editor - .zanimgraph state machines: states as nodes, transitions as arrows, parameters, the selected
//                      state / transition's properties; while playing, the state of the selected entity's Animator
//                      is highlighted.

#include "Engine/AnimGraph.h"
#include "Engine/Animation.h"
#include "Engine/ViewportRenderer.h"
#include "UI/UI.h"

namespace zx::editor {

class Editor;

/// Renders an animated model into a texture (own world, animation system and renderer).
class AnimationPreview
{
public:
    AnimationPreview(rhi::Device& device, render::RenderAssets& renderAssets, asset::AssetManager& assets);
    ~AnimationPreview();
    ZX_NON_COPYABLE(AnimationPreview)

    bool Init();
    /// Instantiates the model (a prefab asset); false while it is not loaded.
    bool SetModel(asset::AssetId prefab);
    /// The model's clips (empty until loaded).
    Span<const asset::AssetId> Clips() const;
    void Play(asset::AssetId clip);
    /// Advances (playing) or holds the pose at `time`.
    void Update(f32 dt, bool playing, f32 speed, bool loop, f32& time);
    void AddPasses(render::RenderGraph& graph, Array<render::RGTexture>& sampled, f32 dt);
    u32 Srv() const { return m_texture ? m_texture->GetSrvIndex() : rhi::InvalidDescriptor; }

    f32 yaw = 0.5f, pitch = 0.15f, distance = 3.4f;
    static constexpr u32 Width = 640, Height = 480;

private:
    rhi::Device& m_device;
    render::RenderAssets& m_renderAssets;
    asset::AssetManager& m_assets;
    UniquePtr<ViewportRenderer> m_renderer;
    UniquePtr<AnimationSystem> m_animation;
    ecs::World m_world;
    ecs::Entity m_root, m_animated;
    asset::AssetId m_model;
    rhi::Texture* m_texture = nullptr;
    Vec3 m_focus = Vec3(0.0f, 0.9f, 0.0f);
};

struct AnimationViewerState
{
    bool open = false;
    String model; // physical path of the .glb / .gltf
    asset::AssetId prefab;
    UniquePtr<AnimationPreview> preview;
    i32 clip = -1;
    f32 time = 0.0f;
    f32 speed = 1.0f;
    bool playing = true;
    bool loop = true;
};

struct AnimGraphEditorState
{
    bool open = false;
    String path; // physical path of the .zanimgraph
    AnimGraph graph;
    bool dirty = false;
    String error;
    Vec2 pan = Vec2(300.0f, 200.0f);
    f32 zoom = 1.0f;
    u32 layer = 0;               // edited machine: 0 = base, 1.. = AnimGraph::layers
    i32 selectedState = -1;      // index, -2 = the Any State node
    i32 selectedTransition = -1;
    i32 linkFrom = -3;           // "Make Transition": source state (-1 = any state), -3 = not linking
    enum class Drag : u8
    {
        None,
        Node,
        Pan,
    };
    Drag drag = Drag::None;
    Vec2 dragOffset;
    Array<String> undo, redo;
    String beforeEdit; // snapshot for one-step property edits
    Vec2 blendPreview; // 2D blend space plot: the previewed parameter point
};

void OpenAnimationViewer(Editor& ed, StringView modelPath);
void OpenAnimGraphEditor(Editor& ed, StringView graphPath);
/// Writes a new graph (the locomotion graph as a starting point) and opens it.
void CreateAnimGraph(Editor& ed, StringView directory);
void DrawAnimationViewer(Editor& ed);
void DrawAnimGraphEditor(Editor& ed);
void RenderAnimationPreview(Editor& ed, render::RenderGraph& graph, Array<render::RGTexture>& sampled);

inline constexpr StringView AnimationViewerTitle = ZX_ICON_CLAPPERBOARD "  Animation Viewer";
inline constexpr StringView AnimGraphEditorTitle = ZX_ICON_WORKFLOW "  AnimGraph Editor";

} // namespace zx::editor
