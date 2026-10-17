#include "Engine/AnimGraph.h"
#include "Core/Math/Math.h"
#include "Core/Serialization/Json.h"

namespace zx {

i32 AnimStateMachine::FindState(StringView name) const
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

void BlendSpace2DWeights(Span<const AnimStateMachine::BlendSample> samples, f32 x, f32 y, Array<f32>& weights)
{
    // Gradient band interpolation (Johansen 2009): each sample's influence falls off along the direction to every other
    // sample; the minimum over the others, normalized.
    const usize n = samples.Size();
    weights.Resize(n);
    f32 total = 0.0f;
    for (usize i = 0; i < n; ++i) {
        const Vec2 pi(samples[i].value, samples[i].y);
        const Vec2 pp = Vec2(x, y) - pi;
        f32 w = 1.0f;
        for (usize j = 0; j < n && w > 0.0f; ++j) {
            if (j == i)
                continue;
            const Vec2 pij = Vec2(samples[j].value, samples[j].y) - pi;
            const f32 len2 = Dot(pij, pij);
            if (len2 < 1e-10f)
                continue; // duplicate points share the weight
            w = Min(w, Clamp(1.0f - Dot(pp, pij) / len2, 0.0f, 1.0f));
        }
        weights[i] = w;
        total += w;
    }
    if (total <= 1e-6f) { // cannot happen with distinct points; be safe: nearest sample
        usize best = 0;
        f32 bestD = 1e30f;
        for (usize i = 0; i < n; ++i) {
            const f32 d = LengthSq(Vec2(x, y) - Vec2(samples[i].value, samples[i].y));
            if (d < bestD) {
                bestD = d;
                best = i;
            }
            weights[i] = 0.0f;
        }
        if (n)
            weights[best] = 1.0f;
        return;
    }
    for (usize i = 0; i < n; ++i)
        weights[i] /= total;
}

namespace {

const char* const ParamTypeNames[] = {"float", "bool", "trigger"};
const char* const OpNames[] = {">", "<", "true", "false", "triggered"};

bool ParseMachine(const JsonValue& root, const AnimGraph& graph, AnimStateMachine& out, bool allowEmptyStates, String& error)
{
    for (const JsonValue& s : root.Get("states").Elements()) {
        AnimStateMachine::State& state = out.states.AddDefaulted();
        state.name = String(s.Get("name").AsString());
        state.clip = String(s.Get("clip").AsString());
        state.blendParam = String(s.Get("blendParam").AsString());
        state.blendParamY = String(s.Get("blendParamY").AsString());
        for (const JsonValue& b : s.Get("blend").Elements())
            state.blend.PushBack(AnimStateMachine::BlendSample{String(b.Get("clip").AsString()), b.Get("value").AsFloat(), b.Get("y").AsFloat()});
        if (state.blendParamY.Empty())
            for (usize i = 1; i < state.blend.Size(); ++i)
                if (state.blend[i].value < state.blend[i - 1].value) {
                    error = String::Format("state '{}': blend samples must be sorted by value", state.name);
                    return false;
                }
        for (const JsonValue& ev : s.Get("events").Elements())
            state.events.PushBack(AnimStateMachine::Event{String(ev.Get("name").AsString()), Clamp(ev.Get("time").AsFloat(), 0.0f, 1.0f)});
        state.speed = s.Get("speed").AsFloat(1.0f);
        if (s.Get("pos").Size() == 2) {
            state.x = s.Get("pos")[0].AsFloat();
            state.y = s.Get("pos")[1].AsFloat();
        }
        state.loop = s.Get("loop").AsBool(true);
        if (state.name.Empty()) {
            error = "a state has no name";
            return false;
        }
        if (!allowEmptyStates && !state.HasMotion()) {
            error = String::Format("state '{}' has neither a clip nor blend samples", state.name);
            return false;
        }
    }
    if (out.states.Empty()) {
        error = "a state machine has no states";
        return false;
    }
    out.entry = Max(out.FindState(root.Get("entry").AsString()), 0);
    if (root.Get("anyPos").Size() == 2) {
        out.anyX = root.Get("anyPos")[0].AsFloat();
        out.anyY = root.Get("anyPos")[1].AsFloat();
    }
    for (const JsonValue& t : root.Get("transitions").Elements()) {
        AnimStateMachine::Transition& tr = out.transitions.AddDefaulted();
        const StringView from = t.Get("from").AsString("*");
        tr.from = from == "*" ? -1 : out.FindState(from);
        tr.to = out.FindState(t.Get("to").AsString());
        if ((from != "*" && tr.from < 0) || tr.to < 0) {
            error = String::Format("transition {} -> {}: unknown state", from, t.Get("to").AsString());
            return false;
        }
        tr.duration = Max(t.Get("duration").AsFloat(0.2f), 0.0f);
        tr.exitTime = t.Get("exitTime").AsFloat(-1.0f);
        for (const JsonValue& c : t.Get("conditions").Elements()) {
            AnimStateMachine::Condition& cond = tr.conditions.AddDefaulted();
            cond.param = String(c.Get("param").AsString());
            const StringView op = c.Get("op").AsString(">");
            bool known = false;
            for (u32 o = 0; o < 5; ++o)
                if (op == OpNames[o]) {
                    cond.op = AnimStateMachine::Op(o);
                    known = true;
                }
            if (!known) {
                error = String::Format("unknown condition operator '{}'", op);
                return false;
            }
            cond.value = c.Get("value").AsFloat();
            if (graph.FindParam(cond.param) < 0) {
                error = String::Format("condition on unknown parameter '{}'", cond.param);
                return false;
            }
        }
    }
    return true;
}

void WriteMachine(const AnimStateMachine& g, JsonValue& root)
{
    root.Set("entry", JsonValue(g.states.Empty() ? StringView() : g.states[usize(Clamp(g.entry, 0, i32(g.states.Size()) - 1))].name.View()));
    JsonValue states = JsonValue::MakeArray();
    for (const AnimStateMachine::State& s : g.states) {
        JsonValue o = JsonValue::MakeObject();
        o.Set("name", JsonValue(s.name));
        if (!s.clip.Empty())
            o.Set("clip", JsonValue(s.clip));
        if (!s.blend.Empty()) {
            o.Set("blendParam", JsonValue(s.blendParam));
            if (!s.blendParamY.Empty())
                o.Set("blendParamY", JsonValue(s.blendParamY));
            JsonValue blend = JsonValue::MakeArray();
            for (const AnimStateMachine::BlendSample& b : s.blend) {
                JsonValue bo = JsonValue::MakeObject();
                bo.Set("clip", JsonValue(b.clip));
                bo.Set("value", JsonValue(f64(b.value)));
                if (!s.blendParamY.Empty())
                    bo.Set("y", JsonValue(f64(b.y)));
                blend.PushBack(move(bo));
            }
            o.Set("blend", move(blend));
        }
        if (!s.events.Empty()) {
            JsonValue events = JsonValue::MakeArray();
            for (const AnimStateMachine::Event& ev : s.events) {
                JsonValue eo = JsonValue::MakeObject();
                eo.Set("name", JsonValue(ev.name));
                eo.Set("time", JsonValue(f64(ev.time)));
                events.PushBack(move(eo));
            }
            o.Set("events", move(events));
        }
        if (s.speed != 1.0f)
            o.Set("speed", JsonValue(f64(s.speed)));
        if (!s.loop)
            o.Set("loop", JsonValue(false));
        JsonValue pos = JsonValue::MakeArray();
        pos.PushBack(JsonValue(f64(Round(s.x))));
        pos.PushBack(JsonValue(f64(Round(s.y))));
        o.Set("pos", move(pos));
        states.PushBack(move(o));
    }
    root.Set("states", move(states));
    JsonValue anyPos = JsonValue::MakeArray();
    anyPos.PushBack(JsonValue(f64(Round(g.anyX))));
    anyPos.PushBack(JsonValue(f64(Round(g.anyY))));
    root.Set("anyPos", move(anyPos));
    JsonValue transitions = JsonValue::MakeArray();
    for (const AnimStateMachine::Transition& t : g.transitions) {
        JsonValue o = JsonValue::MakeObject();
        o.Set("from", JsonValue(t.from < 0 ? StringView("*") : g.states[usize(t.from)].name.View()));
        o.Set("to", JsonValue(g.states[usize(t.to)].name));
        o.Set("duration", JsonValue(f64(t.duration)));
        if (t.exitTime >= 0.0f)
            o.Set("exitTime", JsonValue(f64(t.exitTime)));
        JsonValue conditions = JsonValue::MakeArray();
        for (const AnimStateMachine::Condition& c : t.conditions) {
            JsonValue co = JsonValue::MakeObject();
            co.Set("param", JsonValue(c.param));
            co.Set("op", JsonValue(OpNames[u32(c.op)]));
            if (c.op == AnimStateMachine::Op::Greater || c.op == AnimStateMachine::Op::Less)
                co.Set("value", JsonValue(f64(c.value)));
            conditions.PushBack(move(co));
        }
        o.Set("conditions", move(conditions));
        transitions.PushBack(move(o));
    }
    root.Set("transitions", move(transitions));
}

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
    String message;
    if (!ParseMachine(root, out, out, false, message))
        return fail(move(message));
    for (const JsonValue& l : root.Get("layers").Elements()) {
        AnimGraph::Layer& layer = out.layers.AddDefaulted();
        layer.name = String(l.Get("name").AsString("Layer"));
        for (const JsonValue& bone : l.Get("mask").Elements())
            layer.mask.PushBack(String(bone.AsString()));
        layer.weight = Clamp(l.Get("weight").AsFloat(1.0f), 0.0f, 1.0f);
        layer.weightParam = String(l.Get("weightParam").AsString());
        layer.additive = l.Get("blend").AsString("override") == "additive";
        if (!ParseMachine(l, out, layer, true, message))
            return fail(String::Format("layer '{}': {}", layer.name, message));
    }
    return true;
}

String WriteAnimGraph(const AnimGraph& g)
{
    JsonValue root = JsonValue::MakeObject();
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
    WriteMachine(g, root);
    if (!g.layers.Empty()) {
        JsonValue layers = JsonValue::MakeArray();
        for (const AnimGraph::Layer& l : g.layers) {
            JsonValue o = JsonValue::MakeObject();
            o.Set("name", JsonValue(l.name));
            JsonValue mask = JsonValue::MakeArray();
            for (const String& bone : l.mask)
                mask.PushBack(JsonValue(bone));
            o.Set("mask", move(mask));
            o.Set("weight", JsonValue(f64(l.weight)));
            if (!l.weightParam.Empty())
                o.Set("weightParam", JsonValue(l.weightParam));
            o.Set("blend", JsonValue(l.additive ? "additive" : "override"));
            WriteMachine(l, o);
            layers.PushBack(move(o));
        }
        root.Set("layers", move(layers));
    }
    return ToJsonString(root);
}

AnimGraph MakeLocomotionGraph()
{
    AnimGraph g;
    g.params = {AnimGraph::Param{"Speed", AnimGraph::ParamType::Float, 0.0f}, AnimGraph::Param{"Grounded", AnimGraph::ParamType::Bool, 1.0f},
                AnimGraph::Param{"VerticalSpeed", AnimGraph::ParamType::Float, 0.0f}, AnimGraph::Param{"Jump", AnimGraph::ParamType::Trigger, 0.0f},
                AnimGraph::Param{"Punch", AnimGraph::ParamType::Trigger, 0.0f}};
    AnimGraph::State locomotion;
    locomotion.name = "Locomotion";
    locomotion.blendParam = "Speed";
    locomotion.blend = {AnimGraph::BlendSample{"Idle", 0.0f}, AnimGraph::BlendSample{"Walk", 2.0f}, AnimGraph::BlendSample{"Run", 6.0f}};
    AnimGraph::State air;
    air.name = "Jump";
    air.clip = "Jump";
    air.loop = false;
    air.speed = 1.2f;
    locomotion.x = 0.0f;
    air.x = 320.0f;
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

    // Upper body: silent until "Punch" fires, then plays the punch once over the running legs.
    AnimGraph::Layer upper;
    upper.name = "UpperBody";
    upper.mask = {String("Spine")};
    AnimGraph::State none;
    none.name = "None";
    AnimGraph::State punch;
    punch.name = "Punch";
    punch.clip = "Punch";
    punch.loop = false;
    punch.speed = 1.3f;
    punch.events = {AnimGraph::Event{"PunchHit", 0.4f}}; // the fist is out: scripts push what is in front
    punch.x = 320.0f;
    upper.states = {none, punch};
    AnimGraph::Transition hit;
    hit.from = -1;
    hit.to = 1;
    hit.duration = 0.1f;
    hit.conditions = {AnimGraph::Condition{"Punch", AnimGraph::Op::Triggered, 0.0f}};
    AnimGraph::Transition back;
    back.from = 1;
    back.to = 0;
    back.duration = 0.25f;
    back.exitTime = 0.85f;
    upper.transitions = {hit, back};
    g.layers.PushBack(move(upper));
    return g;
}

} // namespace zx
