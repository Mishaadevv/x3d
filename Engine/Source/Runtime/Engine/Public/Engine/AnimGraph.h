#pragma once

// AnimGraph (.zanimgraph, JSON): a state machine that picks and blends an Animator's clips from parameters, like Unity's
// Animator Controller / UE's AnimBP state machine.
//
//   params      float / bool / trigger values set by scripts, C++ or automatically from a Character Controller
//               ("Speed" = horizontal m/s, "VerticalSpeed", "Grounded")
//   states      one clip, or a 1D blend space (clips at parameter values, phase-synchronized: walk / run feet match)
//   transitions from a state (or any state) to another when all conditions hold (and the exit time has passed),
//               cross-fading over `duration` seconds
//
// Clips are referenced by name (the model's clips: "Idle", "Walk", ...), so one graph serves every model with the same
// clip names.

#include "Core/Containers/Array.h"
#include "Core/String/String.h"

namespace zx {

struct AnimGraph
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
    struct BlendSample
    {
        String clip;
        f32 value = 0.0f; // parameter value at which this clip plays alone
    };
    struct Event // fired when the state's normalized time passes `time` (scripts: onAnimEvent(name))
    {
        String name;
        f32 time = 0.0f;
    };
    struct State
    {
        String name;
        String clip;            // single clip...
        String blendParam;      // ...or a 1D blend space over this parameter
        Array<BlendSample> blend; // sorted by value
        f32 speed = 1.0f;
        bool loop = true;
        Array<Event> events;
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

    Array<Param> params;
    Array<State> states;
    Array<Transition> transitions;
    i32 entry = 0;

    i32 FindState(StringView name) const;
    i32 FindParam(StringView name) const;
};

/// Parses .zanimgraph JSON (states and transitions reference states by name). False with `error` on invalid input.
bool ParseAnimGraph(StringView json, AnimGraph& out, String* error = nullptr);
String WriteAnimGraph(const AnimGraph& graph);
/// Idle / Walk / Run blended by Speed, Jump when leaving the ground, back on landing (clips "Idle", "Walk", "Run",
/// "Jump"); the Starter Content's Characters/Locomotion.zanimgraph.
AnimGraph MakeLocomotionGraph();

} // namespace zx
