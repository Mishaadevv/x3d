#pragma once

// Animator component (plays animation clips on the skinned Mesh Renderer of its entity) and the AnimationSystem that
// poses every animator once per frame:
//
//   AnimationSystem animation(assets);
//   animation.contentRoot = "<project>/Content";   // where Animator::graph files are read from
//   animation.Tick(world, dt, playing);          // editor: animators with Preview In Editor run while editing too
//   Span<const Mat4> palette = animation.Palette(entity); // skinning matrices (mesh space) for the renderer
//
// Clips come with the imported model (glTF animations made for its skin). Without a graph the Animator plays `clip`
// (changing it cross-fades over Blend Time). With an AnimGraph (.zanimgraph, Engine/AnimGraph.h) the graph's state
// machine picks and blends clips from parameters; a Character Controller on the entity or an ancestor sets "Speed",
// "VerticalSpeed" and "Grounded" automatically. Scripts and C++ set parameters (SetParam / SetTrigger) or play clips.

#include "Asset/Animation.h"
#include "Asset/AssetManager.h"
#include "Core/Containers/HashMap.h"
#include "Core/Reflection/Reflection.h"
#include "Engine/AnimGraph.h"
#include "Scene/ECS/World.h"

namespace zx {

struct Animator
{
    asset::AssetId skeleton;     // the skin of the model (set on import)
    asset::AssetId clip;         // what plays without a graph: an Animation asset made for that skeleton
    Array<asset::AssetId> clips; // the clips imported with the model
    String graph;                // AnimGraph (.zanimgraph, Content-relative); empty = play `clip`
    bool playing = true;
    bool loop = true;
    bool previewInEditor = true; // animate while editing (off: bind pose until Play)
    f32 speed = 1.0f;
    f32 blendTime = 0.25f;       // cross-fade when the clip changes (seconds)
    Transform skinToNode;        // skeleton space -> this entity (set on import)
};

ZX_DECLARE_TYPE(Animator);

class AnimationSystem
{
public:
    explicit AnimationSystem(asset::AssetManager& assets) : m_assets(assets) {}

    /// Directory Animator::graph paths are relative to (the project's Content).
    String contentRoot;

    /// Advances and poses every animator: all of them while `playing`, otherwise those with Preview In Editor (the
    /// rest show the bind pose). Skeletons and clips are requested on first use.
    void Tick(ecs::World& world, f32 dt, bool playing);
    /// Skinning matrices of the entity's mesh this frame (mesh space; empty until its skeleton is loaded).
    Span<const Mat4> Palette(ecs::Entity e) const;
    /// Switches the entity's clip to the one named `clip` among Animator::clips (cross-fading; without a graph). False
    /// if unknown.
    bool Play(ecs::World& world, ecs::Entity e, StringView clip);
    /// Graph parameters of the entity's animator (applied on the next Tick). Unknown names are ignored.
    void SetParam(ecs::Entity e, StringView name, f32 value);
    void SetTrigger(ecs::Entity e, StringView name) { SetParam(e, name, 1.0f); }
    f32 GetParam(ecs::Entity e, StringView name) const;
    /// Name of the graph state the entity is in ("Locomotion"), empty without a graph.
    String CurrentState(ecs::Entity e) const;
    /// Name of a clip asset ("Run"), empty while unknown.
    String ClipName(asset::AssetId clip) const { return m_assets.GetName(clip); }
    /// Seconds into the current clip (without a graph).
    f32 Time(ecs::Entity e) const;
    /// Drops all runtime state (Play start / stop, scene change); graph files are re-read.
    void Clear()
    {
        m_states.Clear();
        m_graphs.Clear();
    }
    u32 AnimatorCount() const { return u32(m_states.Size()); }

private:
    struct GraphFile
    {
        AnimGraph graph;
        bool ok = false;
    };
    struct State
    {
        asset::AssetId clip;
        f32 time = 0.0f;
        asset::AssetId fromClip; // cross-fade source (clip mode)
        f32 fromTime = 0.0f;
        f32 fade = 1.0f; // 1 = fully on the current clip / state
        f32 fadeDuration = 0.0f;
        // Graph mode.
        String graphPath;
        i32 graphState = -1;
        f32 phase = 0.0f; // normalized time in the state
        i32 fromState = -1;
        f32 fromPhase = 0.0f;
        HashMap<String, f32> params; // by lower-case name, set before / between ticks
        Array<Transform> pose, fromPose, scratch;
        Array<Mat4> model, palette;
        bool alive = false;
    };
    const GraphFile* LoadGraph(StringView path);
    const asset::AnimationClip* ClipByName(const Animator& a, StringView name);
    /// Pose of a graph state at a normalized phase; returns the state's length in seconds (for advancing the phase).
    f32 EvaluateState(const AnimGraph& g, i32 stateIndex, f32 phase, const Animator& a, const asset::Skeleton& skeleton, State& s,
                      Span<Transform> out);
    f32 ParamValue(const State& s, const AnimGraph& g, StringView name) const;
    void TickGraph(const AnimGraph& g, const Animator& a, const asset::Skeleton& skeleton, State& s, f32 dt, bool animate);

    asset::AssetManager& m_assets;
    HashMap<u64, State> m_states; // entity -> state
    HashMap<String, GraphFile> m_graphs;
    bool m_wasPlaying = false;
};

} // namespace zx
