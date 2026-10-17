#pragma once

// AnimGraph (.zanimgraph, JSON): a state machine that picks and blends an Animator's clips from parameters, like Unity's
// Animator Controller / UE's AnimBP state machine.
//
//   params      float / bool / trigger values set by scripts, C++ or automatically from a Character Controller
//               ("Speed" = horizontal m/s, "VerticalSpeed", "Grounded")
//   states      one clip, or a blend space over one parameter (1D) or two (2D), phase-synchronized: walk / run feet
//               match
//   transitions from a state (or any state) to another when all conditions hold (and the exit time has passed),
//               cross-fading over `duration` seconds
//   layers      more state machines on top of the base one (same parameters): each overrides, or adds to (additive),
//               the bones under its mask - e.g. an upper-body punch while the legs keep running. A layer state without
//               motion leaves the layer silent (fading in / out with the transitions).
//
// Clips are referenced by name (the model's clips: "Idle", "Walk", ...), so one graph serves every model with the same
// clip names.

#include "Core/Containers/Array.h"
#include "Core/Containers/Span.h"
#include "Core/String/String.h"

namespace zx {

/// States and transitions of one layer (the graph itself is the base layer).
struct AnimStateMachine
{
    struct BlendSample
    {
        String clip;
        f32 value = 0.0f; // parameter value at which this clip plays alone
        f32 y = 0.0f;     // 2D blend spaces: the second parameter's value
    };
    struct Event // fired when the state's normalized time passes `time` (scripts: onAnimEvent(name))
    {
        String name;
        f32 time = 0.0f;
    };
    struct State
    {
        String name;
        String clip;              // single clip...
        String blendParam;        // ...or a blend space over this parameter
        String blendParamY;       // 2D blend space: the second parameter (samples anywhere in the plane)
        Array<BlendSample> blend; // 1D: sorted by value
        f32 speed = 1.0f;
        bool loop = true;
        Array<Event> events;
        f32 x = 0.0f, y = 0.0f; // position in the graph editor

        bool HasMotion() const { return !clip.Empty() || !blend.Empty(); }
        bool Is2D() const { return !blend.Empty() && !blendParamY.Empty(); }
    };
    enum class Op : u8
    {
        Greater,
        Less,
        IsTrue,
        IsFalse,
        Triggered,
    };
    struct Condition
    {
        String param;
        Op op = Op::Greater;
        f32 value = 0.0f;
    };
    struct Transition
    {
        i32 from = -1; // state index, -1 = any state
        i32 to = 0;
        Array<Condition> conditions;
        f32 duration = 0.2f;  // cross-fade (seconds)
        f32 exitTime = -1.0f; // normalized time of the source state before it may leave (-1 = any time)
    };

    Array<State> states;
    Array<Transition> transitions;
    i32 entry = 0;
    f32 anyX = -240.0f, anyY = 0.0f; // the "Any State" node in the graph editor

    i32 FindState(StringView name) const;
};

struct AnimGraph : AnimStateMachine
{
    enum class ParamType : u8
    {
        Float,
        Bool,
        Trigger, // set, then consumed by the transition it fires
    };
    struct Param
    {
        String name;
        ParamType type = ParamType::Float;
        f32 defaultValue = 0.0f;
    };
    struct Layer : AnimStateMachine
    {
        String name;
        Array<String> mask;   // bones affected with everything below them (empty = the whole skeleton)
        f32 weight = 1.0f;
        String weightParam;   // float parameter (0..1) multiplying `weight`; empty = none
        bool additive = false; // adds the motion relative to its first frame instead of replacing the pose
    };

    Array<Param> params;
    Array<Layer> layers; // evaluated in order on top of the base machine

    i32 FindParam(StringView name) const;
    /// The base machine (layer 0) or layers[layer - 1].
    AnimStateMachine& Machine(usize layer)
    {
        if (layer == 0)
            return *this;
        return layers[layer - 1];
    }
    const AnimStateMachine& Machine(usize layer) const
    {
        if (layer == 0)
            return *this;
        return layers[layer - 1];
    }
    usize MachineCount() const { return layers.Size() + 1; }
};

/// Parses .zanimgraph JSON (states and transitions reference states by name). False with `error` on invalid input.
bool ParseAnimGraph(StringView json, AnimGraph& out, String* error = nullptr);
String WriteAnimGraph(const AnimGraph& graph);
/// Idle / Walk / Run blended by Speed, Jump when leaving the ground, back on landing (clips "Idle", "Walk", "Run",
/// "Jump"), and an "UpperBody" layer that punches on the "Punch" trigger (clip "Punch", masked to the spine); the
/// Starter Content's Characters/Locomotion.zanimgraph.
AnimGraph MakeLocomotionGraph();

/// Weights of the samples of a 2D blend space at `p` (gradient band interpolation: a sample alone at its point,
/// smooth in between, weights sum to 1). `weights` gets one entry per sample.
void BlendSpace2DWeights(Span<const AnimStateMachine::BlendSample> samples, f32 x, f32 y, Array<f32>& weights);

} // namespace zx
