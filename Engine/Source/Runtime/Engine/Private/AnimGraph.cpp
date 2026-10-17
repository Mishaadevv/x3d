#include "Engine/AnimGraph.h"
#include "Core/Serialization/Json.h"

namespace zx {

i32 AnimGraph::FindState(StringView name) const
{
    for (usize i = 0; i < states.Size(); ++i)
        if (states[i].name.View().EqualsIgnoreCase(name))
            return i32(i);
    return -1;
}

i32 AnimGraph::FindParam(StringView name) const
{
    for (usize i = 0; i < params.Size(); ++i)
        if (params[i].name.View().EqualsIgnoreCase(name))
            return i32(i);
    return -1;
}

namespace {

const char* const ParamTypeNames[] = {"float", "bool", "trigger"};
const char* const OpNames[] = {">", "<", "true", "false", "triggered"};

} // namespace

bool ParseAnimGraph(StringView json, AnimGraph& out, String* error)
{
    auto fail = [&](String message) {
        if (error)
            *error = move(message);
        return false;
    };
    JsonValue root;
    JsonParseError err;
    if (!ParseJson(json, root, &err))
        return fail(String::Format("JSON error at {}:{}: {}", err.line, err.column, err.message));
    out = AnimGraph{};
    for (const JsonValue& p : root.Get("params").Elements()) {
        AnimGraph::Param& param = out.params.AddDefaulted();
        param.name = String(p.Get("name").AsString());
        const StringView type = p.Get("type").AsString("float");
        for (u32 t = 0; t < 3; ++t)
            if (type == ParamTypeNames[t])
                param.type = AnimGraph::ParamType(t);
        param.defaultValue = p.Get("default").IsBool() ? (p.Get("default").AsBool() ? 1.0f : 0.0f) : p.Get("default").AsFloat();
    }
    for (const JsonValue& s : root.Get("states").Elements()) {
        AnimGraph::State& state = out.states.AddDefaulted();
        state.name = String(s.Get("name").AsString());
        state.clip = String(s.Get("clip").AsString());
        state.blendParam = String(s.Get("blendParam").AsString());
        for (const JsonValue& b : s.Get("blend").Elements())
            state.blend.PushBack(AnimGraph::BlendSample{String(b.Get("clip").AsString()), b.Get("value").AsFloat()});
        for (usize i = 1; i < state.blend.Size(); ++i)
            if (state.blend[i].value < state.blend[i - 1].value)
                return fail(String::Format("state '{}': blend samples must be sorted by value", state.name));
        state.speed = s.Get("speed").AsFloat(1.0f);
        state.loop = s.Get("loop").AsBool(true);
        if (state.name.Empty())
            return fail("a state has no name");
        if (state.clip.Empty() && state.blend.Empty())
            return fail(String::Format("state '{}' has neither a clip nor blend samples", state.name));
    }
    if (out.states.Empty())
        return fail("the graph has no states");
    out.entry = Max(out.FindState(root.Get("entry").AsString()), 0);
    for (const JsonValue& t : root.Get("transitions").Elements()) {
        AnimGraph::Transition& tr = out.transitions.AddDefaulted();
        const StringView from = t.Get("from").AsString("*");
        tr.from = from == "*" ? -1 : out.FindState(from);
        tr.to = out.FindState(t.Get("to").AsString());
        if ((from != "*" && tr.from < 0) || tr.to < 0)
            return fail(String::Format("transition {} -> {}: unknown state", from, t.Get("to").AsString()));
        tr.duration = Max(t.Get("duration").AsFloat(0.2f), 0.0f);
        tr.exitTime = t.Get("exitTime").AsFloat(-1.0f);
        for (const JsonValue& c : t.Get("conditions").Elements()) {
            AnimGraph::Condition& cond = tr.conditions.AddDefaulted();
            cond.param = String(c.Get("param").AsString());
            const StringView op = c.Get("op").AsString(">");
            bool known = false;
            for (u32 o = 0; o < 5; ++o)
                if (op == OpNames[o]) {
                    cond.op = AnimGraph::Op(o);
                    known = true;
                }
            if (!known)
                return fail(String::Format("unknown condition operator '{}'", op));
            cond.value = c.Get("value").AsFloat();
            if (out.FindParam(cond.param) < 0)
                return fail(String::Format("condition on unknown parameter '{}'", cond.param));
        }
    }
    return true;
}

String WriteAnimGraph(const AnimGraph& g)
{
    JsonValue root = JsonValue::MakeObject();
    root.Set("entry", JsonValue(g.states.Empty() ? StringView() : g.states[usize(Clamp(g.entry, 0, i32(g.states.Size()) - 1))].name.View()));
    JsonValue params = JsonValue::MakeArray();
    for (const AnimGraph::Param& p : g.params) {
        JsonValue o = JsonValue::MakeObject();
        o.Set("name", JsonValue(p.name));
        o.Set("type", JsonValue(ParamTypeNames[u32(p.type)]));
        if (p.defaultValue != 0.0f)
            o.Set("default", JsonValue(f64(p.defaultValue)));
        params.PushBack(move(o));
    }
    root.Set("params", move(params));
    JsonValue states = JsonValue::MakeArray();
    for (const AnimGraph::State& s : g.states) {
        JsonValue o = JsonValue::MakeObject();
        o.Set("name", JsonValue(s.name));
        if (!s.clip.Empty())
            o.Set("clip", JsonValue(s.clip));
        if (!s.blend.Empty()) {
            o.Set("blendParam", JsonValue(s.blendParam));
            JsonValue blend = JsonValue::MakeArray();
            for (const AnimGraph::BlendSample& b : s.blend) {
                JsonValue bo = JsonValue::MakeObject();
                bo.Set("clip", JsonValue(b.clip));
                bo.Set("value", JsonValue(f64(b.value)));
                blend.PushBack(move(bo));
            }
            o.Set("blend", move(blend));
        }
        if (s.speed != 1.0f)
            o.Set("speed", JsonValue(f64(s.speed)));
        if (!s.loop)
            o.Set("loop", JsonValue(false));
        states.PushBack(move(o));
    }
    root.Set("states", move(states));
    JsonValue transitions = JsonValue::MakeArray();
    for (const AnimGraph::Transition& t : g.transitions) {
        JsonValue o = JsonValue::MakeObject();
        o.Set("from", JsonValue(t.from < 0 ? StringView("*") : g.states[usize(t.from)].name.View()));
        o.Set("to", JsonValue(g.states[usize(t.to)].name));
        o.Set("duration", JsonValue(f64(t.duration)));
        if (t.exitTime >= 0.0f)
            o.Set("exitTime", JsonValue(f64(t.exitTime)));
        JsonValue conditions = JsonValue::MakeArray();
        for (const AnimGraph::Condition& c : t.conditions) {
            JsonValue co = JsonValue::MakeObject();
            co.Set("param", JsonValue(c.param));
            co.Set("op", JsonValue(OpNames[u32(c.op)]));
            if (c.op == AnimGraph::Op::Greater || c.op == AnimGraph::Op::Less)
                co.Set("value", JsonValue(f64(c.value)));
            conditions.PushBack(move(co));
        }
        o.Set("conditions", move(conditions));
        transitions.PushBack(move(o));
    }
    root.Set("transitions", move(transitions));
    return ToJsonString(root);
}

AnimGraph MakeLocomotionGraph()
{
    AnimGraph g;
    g.params = {AnimGraph::Param{"Speed", AnimGraph::ParamType::Float, 0.0f}, AnimGraph::Param{"Grounded", AnimGraph::ParamType::Bool, 1.0f},
                AnimGraph::Param{"VerticalSpeed", AnimGraph::ParamType::Float, 0.0f}, AnimGraph::Param{"Jump", AnimGraph::ParamType::Trigger, 0.0f}};
    AnimGraph::State locomotion;
    locomotion.name = "Locomotion";
    locomotion.blendParam = "Speed";
    locomotion.blend = {AnimGraph::BlendSample{"Idle", 0.0f}, AnimGraph::BlendSample{"Walk", 2.0f}, AnimGraph::BlendSample{"Run", 6.0f}};
    AnimGraph::State air;
    air.name = "Jump";
    air.clip = "Jump";
    air.loop = false;
    air.speed = 1.2f;
    g.states = {locomotion, air};
    g.entry = 0;
    AnimGraph::Transition up;
    up.from = 0;
    up.to = 1;
    up.duration = 0.12f;
    up.conditions = {AnimGraph::Condition{"Grounded", AnimGraph::Op::IsFalse, 0.0f}};
    AnimGraph::Transition land;
    land.from = 1;
    land.to = 0;
    land.duration = 0.18f;
    land.conditions = {AnimGraph::Condition{"Grounded", AnimGraph::Op::IsTrue, 0.0f}};
    g.transitions = {up, land};
    return g;
}

} // namespace zx
