#include "Engine/BehaviorTree.h"
#include "Core/Containers/Function.h"
#include "Core/FileSystem/FileSystem.h"
#include "Core/Log/Log.h"
#include "Core/Serialization/Json.h"
#include "Core/String/Path.h"
#include "Engine/Navigation.h"
#include "Engine/PhysicsSystem.h"
#include "Scene/Hierarchy.h"

#include <cmath>

namespace zx {

ZX_DEFINE_LOG_CATEGORY_STATIC(LogAI, Info);

ZX_REFLECT_STRUCT_BEGIN(BehaviorTree)
    ZX_FIELD(tree, Meta::Tooltip("Behaviour tree (.zbt, Content-relative): what the AI decides to do"))
    ZX_FIELD(enabled)
    ZX_FIELD(targetName, Meta::Category("Sight"), Meta::Tooltip("The entity it looks for (blackboard: target, targetVisible, targetLastSeen)"))
    ZX_FIELD(sightRange, Meta::Range(0.0f, 500.0f), Meta::Units("m"))
    ZX_FIELD(sightAngle, Meta::Range(1.0f, 360.0f), Meta::Units("deg"), Meta::Tooltip("Full width of the view cone"))
    ZX_FIELD(eyeHeight, Meta::Range(0.0f, 10.0f), Meta::Units("m"))
ZX_REFLECT_STRUCT_END()

namespace {

struct TypeName
{
    BTNodeType type;
    const char* name;
};
constexpr TypeName TypeNames[] = {
    {BTNodeType::Selector, "Selector"},       {BTNodeType::Sequence, "Sequence"},     {BTNodeType::Parallel, "Parallel"},
    {BTNodeType::Inverter, "Inverter"},       {BTNodeType::Succeeder, "Succeeder"},   {BTNodeType::Repeat, "Repeat"},
    {BTNodeType::Cooldown, "Cooldown"},       {BTNodeType::Wait, "Wait"},             {BTNodeType::MoveTo, "MoveTo"},
    {BTNodeType::Stop, "Stop"},               {BTNodeType::Condition, "Condition"},   {BTNodeType::SetValue, "SetValue"},
    {BTNodeType::ClearValue, "ClearValue"},   {BTNodeType::RandomPoint, "RandomPoint"}, {BTNodeType::NextWaypoint, "NextWaypoint"},
    {BTNodeType::FaceTarget, "FaceTarget"},   {BTNodeType::Event, "Event"},           {BTNodeType::Log, "Log"},
};

JsonValue ToJson(const BBValue& v)
{
    switch (v.type) {
    case BBType::Bool: return JsonValue(v.number != 0.0);
    case BBType::Number: return JsonValue(v.number);
    case BBType::Vec3: {
        JsonValue a = JsonValue::MakeArray();
        a.PushBack(JsonValue(v.vec.x));
        a.PushBack(JsonValue(v.vec.y));
        a.PushBack(JsonValue(v.vec.z));
        return a;
    }
    case BBType::String: return JsonValue(v.text.View());
    default: return JsonValue();
    }
}

BBValue FromJson(const JsonValue& j)
{
    if (j.IsBool())
        return BBValue::Bool(j.AsBool());
    if (j.IsNumber())
        return BBValue::Number(j.AsDouble());
    if (j.IsString())
        return BBValue::Text(j.AsString());
    if (j.IsArray() && j.Size() == 3)
        return BBValue::Vector(Vec3(j[0].AsFloat(), j[1].AsFloat(), j[2].AsFloat()));
    return BBValue();
}

f32 Yaw(const Quat& q)
{
    const Vec3 f = q * Vec3(0, 0, 1);
    return Atan2(f.x, f.z);
}

} // namespace

const char* BTNodeTypeName(BTNodeType t)
{
    for (const TypeName& n : TypeNames)
        if (n.type == t)
            return n.name;
    return "Sequence";
}

bool BTNodeTypeFromName(StringView name, BTNodeType& out)
{
    for (const TypeName& n : TypeNames)
        if (name.EqualsIgnoreCase(n.name)) {
            out = n.type;
            return true;
        }
    return false;
}

bool BTIsComposite(BTNodeType t) { return t == BTNodeType::Selector || t == BTNodeType::Sequence || t == BTNodeType::Parallel; }
bool BTIsDecorator(BTNodeType t)
{
    return t == BTNodeType::Inverter || t == BTNodeType::Succeeder || t == BTNodeType::Repeat || t == BTNodeType::Cooldown;
}

BBValue BBValue::Bool(bool b)
{
    BBValue v;
    v.type = BBType::Bool;
    v.number = b ? 1.0 : 0.0;
    return v;
}
BBValue BBValue::Number(f64 n)
{
    BBValue v;
    v.type = BBType::Number;
    v.number = n;
    return v;
}
BBValue BBValue::Vector(Vec3 p)
{
    BBValue v;
    v.type = BBType::Vec3;
    v.vec = p;
    return v;
}
BBValue BBValue::Text(StringView s)
{
    BBValue v;
    v.type = BBType::String;
    v.text = String(s);
    return v;
}
BBValue BBValue::Of(ecs::Entity e)
{
    BBValue v;
    v.type = BBType::Entity;
    v.entity = e;
    return v;
}
bool BBValue::Truthy() const
{
    switch (type) {
    case BBType::Bool:
    case BBType::Number: return number != 0.0;
    case BBType::String: return !text.Empty();
    case BBType::Entity: return bool(entity);
    case BBType::Vec3: return true;
    default: return false;
    }
}

bool ParseBehaviorTree(StringView json, BehaviorTreeAsset& out, String* error, bool validate)
{
    JsonValue root;
    JsonParseError pe;
    if (!ParseJson(json, root, &pe)) {
        if (error)
            *error = String::Format("line {}: {}", pe.line, pe.message);
        return false;
    }
    if (root.Get("format").AsString() != "zbt") {
        if (error)
            *error = "not a .zbt file";
        return false;
    }
    out = BehaviorTreeAsset();
    out.name = String(root.Get("name").AsString("Behavior Tree"));
    if (const JsonValue* bb = root.Find("blackboard"); bb && bb->IsObject())
        for (const JsonMember& m : bb->Members())
            out.defaults.PushBack({m.first, FromJson(m.second)});
    const JsonValue& nodes = root.Get("nodes");
    for (const JsonValue& j : nodes.Elements()) {
        BTNode n;
        if (!BTNodeTypeFromName(j.Get("type").AsString(), n.type)) {
            if (error)
                *error = String::Format("node {}: unknown type '{}'", out.nodes.Size(), j.Get("type").AsString());
            return false;
        }
        n.name = String(j.Get("name").AsString());
        for (const JsonValue& c : j.Get("children").Elements())
            n.children.PushBack(u32(c.AsInt()));
        n.key = String(j.Get("key").AsString());
        n.op = String(j.Get("op").AsString());
        if (const JsonValue* v = j.Find("value"))
            n.value = FromJson(*v);
        n.seconds = j.Get("seconds").AsFloat(n.seconds);
        n.deviation = j.Get("deviation").AsFloat(n.deviation);
        n.radius = j.Get("radius").AsFloat(n.radius);
        n.count = i32(j.Get("count").AsInt(n.count));
        n.text = String(j.Get("text").AsString());
        const JsonValue& pos = j.Get("pos");
        if (pos.Size() == 2)
            n.editorPosition = Vec2(pos[0].AsFloat(), pos[1].AsFloat());
        out.nodes.PushBack(move(n));
    }
    if (out.nodes.Empty()) {
        if (error)
            *error = "the tree has no nodes";
        return false;
    }
    if (!validate) {
        for (BTNode& n : out.nodes)
            n.children.RemoveIf([&](u32 c) { return c >= out.nodes.Size(); });
        return true;
    }
    // Children must exist and form a tree under node 0 (no cycles: every child index is visited once).
    Array<u8> seen;
    seen.Resize(out.nodes.Size(), 0);
    Array<u32> stack;
    stack.PushBack(0);
    seen[0] = 1;
    while (!stack.Empty()) {
        const u32 i = stack.PopBackValue();
        const BTNode& n = out.nodes[i];
        if (BTIsDecorator(n.type) && n.children.Size() != 1) {
            if (error)
                *error = String::Format("{} '{}' needs exactly one child", BTNodeTypeName(n.type), n.name);
            return false;
        }
        for (u32 c : n.children) {
            if (c >= out.nodes.Size() || seen[c]) {
                if (error)
                    *error = String::Format("node {} has a bad child {}", i, c);
                return false;
            }
            seen[c] = 1;
            stack.PushBack(c);
        }
    }
    return true;
}

void CheckBehaviorTree(const BehaviorTreeAsset& tree, Array<String>& problems)
{
    Array<u8> reached;
    reached.Resize(tree.nodes.Size(), 0);
    Array<u32> stack;
    if (!tree.nodes.Empty()) {
        stack.PushBack(0);
        reached[0] = 1;
    }
    while (!stack.Empty()) {
        const u32 i = stack.PopBackValue();
        const BTNode& n = tree.nodes[i];
        const StringView label = n.name.Empty() ? StringView(BTNodeTypeName(n.type)) : n.name.View();
        if (BTIsDecorator(n.type) && n.children.Size() != 1)
            problems.PushBack(String::Format("{} '{}' needs exactly one child", BTNodeTypeName(n.type), label));
        if (BTIsComposite(n.type) && n.children.Empty())
            problems.PushBack(String::Format("{} '{}' has no children (it always fails)", BTNodeTypeName(n.type), label));
        if ((n.type == BTNodeType::MoveTo || n.type == BTNodeType::Condition || n.type == BTNodeType::SetValue || n.type == BTNodeType::RandomPoint ||
             n.type == BTNodeType::NextWaypoint || n.type == BTNodeType::FaceTarget) &&
            n.key.Empty())
            problems.PushBack(String::Format("{} '{}' has no blackboard key", BTNodeTypeName(n.type), label));
        for (u32 c : n.children)
            if (c < tree.nodes.Size() && !reached[c]) {
                reached[c] = 1;
                stack.PushBack(c);
            }
    }
    u32 loose = 0;
    for (u8 r : reached)
        loose += r ? 0u : 1u;
    if (loose)
        problems.PushBack(String::Format("{} node{} not connected to the root (ignored)", loose, loose == 1 ? " is" : "s are"));
}

String WriteBehaviorTree(const BehaviorTreeAsset& tree)
{
    JsonValue root = JsonValue::MakeObject();
    root.Set("format", JsonValue("zbt"));
    root.Set("name", JsonValue(tree.name.View()));
    JsonValue bb = JsonValue::MakeObject();
    for (const auto& d : tree.defaults)
        bb.Set(d.first.View(), ToJson(d.second));
    root.Set("blackboard", move(bb));
    JsonValue nodes = JsonValue::MakeArray();
    for (const BTNode& n : tree.nodes) {
        JsonValue j = JsonValue::MakeObject();
        j.Set("type", JsonValue(BTNodeTypeName(n.type)));
        if (!n.name.Empty())
            j.Set("name", JsonValue(n.name.View()));
        if (!n.children.Empty()) {
            JsonValue c = JsonValue::MakeArray();
            for (u32 k : n.children)
                c.PushBack(JsonValue(k));
            j.Set("children", move(c));
        }
        if (!n.key.Empty())
            j.Set("key", JsonValue(n.key.View()));
        if (!n.op.Empty())
            j.Set("op", JsonValue(n.op.View()));
        if (n.value.IsSet())
            j.Set("value", ToJson(n.value));
        switch (n.type) {
        case BTNodeType::Wait:
            j.Set("seconds", JsonValue(n.seconds));
            j.Set("deviation", JsonValue(n.deviation));
            break;
        case BTNodeType::Cooldown: j.Set("seconds", JsonValue(n.seconds)); break;
        case BTNodeType::MoveTo:
        case BTNodeType::RandomPoint: j.Set("radius", JsonValue(n.radius)); break;
        case BTNodeType::Repeat:
        case BTNodeType::Parallel: j.Set("count", JsonValue(n.count)); break;
        default: break;
        }
        if (!n.text.Empty())
            j.Set("text", JsonValue(n.text.View()));
        JsonValue pos = JsonValue::MakeArray();
        pos.PushBack(JsonValue(n.editorPosition.x));
        pos.PushBack(JsonValue(n.editorPosition.y));
        j.Set("pos", move(pos));
        nodes.PushBack(move(j));
    }
    root.Set("nodes", move(nodes));
    return ToJsonString(root);
}

void LayoutBehaviorTree(BehaviorTreeAsset& tree, Vec2 nodeSize, Vec2 gap)
{
    if (tree.nodes.Empty())
        return;
    Array<u8> placed;
    placed.Resize(tree.nodes.Size(), 0);
    f32 nextX = 0.0f;
    f32 maxY = 0.0f;
    // Returns the node's x; leaves take the next free slot, parents center over their children.
    Function<f32(u32, u32), 64> place = [&](u32 i, u32 depth) -> f32 {
        placed[i] = 1;
        BTNode& n = tree.nodes[i];
        const f32 y = f32(depth) * (nodeSize.y + gap.y);
        maxY = Max(maxY, y);
        f32 lo = 1e30f, hi = -1e30f;
        for (u32 c : n.children)
            if (c < tree.nodes.Size() && !placed[c]) {
                const f32 x = place(c, depth + 1);
                lo = Min(lo, x);
                hi = Max(hi, x);
            }
        f32 x;
        if (lo > hi) {
            x = nextX;
            nextX += nodeSize.x + gap.x;
        } else {
            x = (lo + hi) * 0.5f;
        }
        n.editorPosition = Vec2(x, y);
        return x;
    };
    place(0, 0);
    // Loose nodes: a row under the tree.
    f32 x = 0.0f;
    for (u32 i = 0; i < tree.nodes.Size(); ++i)
        if (!placed[i]) {
            tree.nodes[i].editorPosition = Vec2(x, maxY + 2.0f * (nodeSize.y + gap.y));
            x += nodeSize.x + gap.x;
        }
}

BehaviorTreeAsset MakeGuardTree()
{
    BehaviorTreeAsset t;
    t.name = "Guard";
    auto add = [&](BTNodeType type, StringView name, Vec2 pos) -> u32 {
        BTNode n;
        n.type = type;
        n.name = String(name);
        n.editorPosition = pos;
        t.nodes.PushBack(move(n));
        return u32(t.nodes.Size() - 1);
    };
    auto child = [&](u32 parent, u32 c) { t.nodes[parent].children.PushBack(c); };
    const u32 root = add(BTNodeType::Selector, "Guard", Vec2(0, 0));
    // 1. Chase while the target is seen.
    const u32 chase = add(BTNodeType::Sequence, "Chase", Vec2(-420, 140));
    const u32 seen = add(BTNodeType::Condition, "Target visible?", Vec2(-560, 280));
    t.nodes[seen].key = "targetVisible";
    t.nodes[seen].op = "==";
    t.nodes[seen].value = BBValue::Bool(true);
    const u32 alert = add(BTNodeType::SetValue, "Remember to search", Vec2(-420, 280));
    t.nodes[alert].key = "search";
    t.nodes[alert].value = BBValue::Bool(true);
    const u32 follow = add(BTNodeType::MoveTo, "Run to target", Vec2(-280, 280));
    t.nodes[follow].key = "target";
    t.nodes[follow].radius = 1.5f;
    const u32 caught = add(BTNodeType::Event, "Caught", Vec2(-140, 280));
    t.nodes[caught].text = "Caught";
    child(chase, seen);
    child(chase, alert);
    child(chase, follow);
    child(chase, caught);
    // 2. Search where it was last seen.
    const u32 search = add(BTNodeType::Sequence, "Search", Vec2(0, 140));
    const u32 wants = add(BTNodeType::Condition, "Searching?", Vec2(-140, 280));
    t.nodes[wants].key = "search";
    t.nodes[wants].op = "==";
    t.nodes[wants].value = BBValue::Bool(true);
    const u32 goLast = add(BTNodeType::MoveTo, "Go to last seen", Vec2(0, 280));
    t.nodes[goLast].key = "targetLastSeen";
    t.nodes[goLast].radius = 1.0f;
    const u32 look = add(BTNodeType::Wait, "Look around", Vec2(140, 280));
    t.nodes[look].seconds = 2.5f;
    const u32 giveUp = add(BTNodeType::SetValue, "Give up", Vec2(280, 280));
    t.nodes[giveUp].key = "search";
    t.nodes[giveUp].value = BBValue::Bool(false);
    child(search, wants);
    child(search, goLast);
    child(search, look);
    child(search, giveUp);
    // 3. Patrol the waypoints.
    const u32 patrol = add(BTNodeType::Sequence, "Patrol", Vec2(420, 140));
    const u32 next = add(BTNodeType::NextWaypoint, "Next waypoint", Vec2(280, 280));
    t.nodes[next].key = "waypoint";
    t.nodes[next].text = "Waypoints";
    const u32 walk = add(BTNodeType::MoveTo, "Walk there", Vec2(420, 280));
    t.nodes[walk].key = "waypoint";
    const u32 pause = add(BTNodeType::Wait, "Pause", Vec2(560, 280));
    t.nodes[pause].seconds = 1.5f;
    t.nodes[pause].deviation = 0.5f;
    child(patrol, next);
    child(patrol, walk);
    child(patrol, pause);
    child(root, chase);
    child(root, search);
    child(root, patrol);
    t.defaults.PushBack({String("search"), BBValue::Bool(false)});
    t.defaults.PushBack({String("targetVisible"), BBValue::Bool(false)});
    LayoutBehaviorTree(t, Vec2(168.0f, 52.0f), Vec2(16.0f, 48.0f));
    return t;
}

// ---------------------------------------------------------------------------------------------------------------------

struct BehaviorSystem::Context
{
    ecs::World& world;
    ecs::Entity entity;
    BehaviorTree& bt;
    const BehaviorTreeAsset& tree;
    f32 dt;

    BehaviorTree::NodeState& State(u32 node) { return bt.states[node]; }
    const BBValue* Get(StringView key) const { return bt.blackboard.Find(String(key)); }
    Vec3 Position() const
    {
        const scene::WorldTransform* wt = world.Get<const scene::WorldTransform>(entity);
        return wt ? wt->GetPosition() : Vec3();
    }
    /// A position from a blackboard value (vec3, or an entity's position).
    bool PointOf(StringView key, Vec3& out) const
    {
        const BBValue* v = Get(key);
        if (!v)
            return false;
        if (v->type == BBType::Vec3) {
            out = v->vec;
            return true;
        }
        if (v->type == BBType::Entity && world.IsAlive(v->entity)) {
            out = scene::ComputeWorldMatrix(world, v->entity).GetTranslation();
            return true;
        }
        return false;
    }
};

f32 BehaviorSystem::Random01()
{
    m_rng ^= m_rng << 13;
    m_rng ^= m_rng >> 17;
    m_rng ^= m_rng << 5;
    return f32(m_rng & 0xFFFFFF) / 16777216.0f;
}

const BehaviorTreeAsset* BehaviorSystem::Load(StringView path)
{
    if (File* f = m_files.Find(String(path)))
        return f->ok ? &f->tree : nullptr;
    File& f = m_files.InsertOrAssign(String(path), File{});
    f.version = m_nextVersion++;
    String text, error;
    const String full = contentRoot.Empty() ? String(path) : Path::Join(contentRoot, path);
    if (!fs::ReadTextFile(full, text))
        ZX_LOG_WARN(LogAI, "Behavior tree not found: {}", full);
    else if (!ParseBehaviorTree(text.View(), f.tree, &error))
        ZX_LOG_ERROR(LogAI, "{}: {}", path, error);
    else
        f.ok = true;
    return f.ok ? &f.tree : nullptr;
}

void BehaviorSystem::Reload(StringView path)
{
    if (path.Empty())
        m_files.Clear();
    else
        m_files.Remove(String(path));
}

void BehaviorSystem::Reset(ecs::World& world)
{
    m_events.Clear();
    world.Query<BehaviorTree>().Each([](ecs::Entity, BehaviorTree& bt) {
        bt.states.Clear();
        bt.blackboard.Clear();
        bt.loadedVersion = -1;
        bt.waypoint = 0;
        bt.clock = 0.0f;
    });
}

Span<const BehaviorTree::NodeState> BehaviorSystem::NodeStates(const ecs::World& world, ecs::Entity e)
{
    const BehaviorTree* bt = world.IsAlive(e) ? world.Get<const BehaviorTree>(e) : nullptr;
    return bt ? bt->states.View() : Span<const BehaviorTree::NodeState>();
}

void BehaviorSystem::Abort(Context& c, u32 node)
{
    BehaviorTree::NodeState& st = c.State(node);
    if (st.status == BTStatus::Running) {
        const BTNode& n = c.tree.nodes[node];
        for (u32 ch : n.children)
            Abort(c, ch);
        // Stop the agent only if it still walks where this node sent it (a branch that took over may have sent it on).
        const NavMeshAgent* agent = c.world.Get<const NavMeshAgent>(c.entity);
        if (n.type == BTNodeType::MoveTo && navigation && agent && agent->hasDestination && Distance(agent->destination, st.point) < 1e-3f)
            navigation->Stop(c.world, c.entity);
    }
    st.status = BTStatus::Idle;
}

BTStatus BehaviorSystem::Run(Context& c, u32 node)
{
    const BTNode& n = c.tree.nodes[node];
    BehaviorTree::NodeState& st = c.State(node);
    const bool starting = st.status != BTStatus::Running;
    auto finish = [&](BTStatus s) {
        c.State(node).status = s;
        return s;
    };
    if (starting) {
        st.child = 0;
        st.aux = 0;
        if (n.type != BTNodeType::Cooldown)
            st.timer = 0.0f;
        st.status = BTStatus::Running;
    }
    switch (n.type) {
    case BTNodeType::Selector: {
        for (u32 k = 0; k < n.children.Size(); ++k) {
            const u32 running = st.child;
            const BTStatus r = Run(c, n.children[k]);
            if (r == BTStatus::Failure)
                continue;
            // A higher-priority child took over: stop the one that was running.
            if (!starting && running != k && running < n.children.Size())
                Abort(c, n.children[running]);
            if (r == BTStatus::Running) {
                c.State(node).child = k;
                return BTStatus::Running;
            }
            return finish(BTStatus::Success);
        }
        return finish(BTStatus::Failure);
    }
    case BTNodeType::Sequence: {
        // Guards before the running child are checked again (a lost target ends the chase).
        for (u32 k = 0; k < st.child && k < n.children.Size(); ++k)
            if (c.tree.nodes[n.children[k]].type == BTNodeType::Condition && Run(c, n.children[k]) == BTStatus::Failure) {
                Abort(c, n.children[st.child]);
                return finish(BTStatus::Failure);
            }
        while (st.child < n.children.Size()) {
            const BTStatus r = Run(c, n.children[st.child]);
            if (r == BTStatus::Running)
                return BTStatus::Running;
            if (r == BTStatus::Failure)
                return finish(BTStatus::Failure);
            ++c.State(node).child;
        }
        return finish(BTStatus::Success);
    }
    case BTNodeType::Parallel: {
        if (starting)
            for (u32 ch : n.children)
                c.State(ch).status = BTStatus::Idle;
        u32 succeeded = 0;
        bool failed = false;
        for (u32 ch : n.children) {
            const BTStatus prev = c.State(ch).status;
            if (prev == BTStatus::Success && !starting) {
                ++succeeded;
                continue;
            }
            if (prev == BTStatus::Failure && !starting) {
                failed = true;
                continue;
            }
            const BTStatus r = Run(c, ch);
            succeeded += r == BTStatus::Success;
            failed |= r == BTStatus::Failure;
        }
        const u32 need = n.count > 0 ? Min(u32(n.count), u32(n.children.Size())) : u32(n.children.Size());
        if (failed || succeeded >= need) {
            for (u32 ch : n.children)
                Abort(c, ch);
            return finish(failed ? BTStatus::Failure : BTStatus::Success);
        }
        return BTStatus::Running;
    }
    case BTNodeType::Inverter: {
        const BTStatus r = Run(c, n.children[0]);
        return r == BTStatus::Running ? r : finish(r == BTStatus::Success ? BTStatus::Failure : BTStatus::Success);
    }
    case BTNodeType::Succeeder: {
        const BTStatus r = Run(c, n.children[0]);
        return r == BTStatus::Running ? r : finish(BTStatus::Success);
    }
    case BTNodeType::Repeat: {
        const BTStatus r = Run(c, n.children[0]);
        if (r == BTStatus::Failure)
            return finish(BTStatus::Failure);
        if (r == BTStatus::Success) {
            const u32 done = ++c.State(node).child;
            if (n.count > 0 && done >= u32(n.count))
                return finish(BTStatus::Success);
        }
        return BTStatus::Running;
    }
    case BTNodeType::Cooldown: {
        if (starting && c.bt.clock < st.timer)
            return finish(BTStatus::Failure);
        const BTStatus r = Run(c, n.children[0]);
        if (r == BTStatus::Running)
            return r;
        c.State(node).timer = c.bt.clock + n.seconds;
        return finish(r);
    }
    case BTNodeType::Wait: {
        if (starting)
            st.timer = Max(0.0f, n.seconds + n.deviation * (Random01() * 2.0f - 1.0f));
        st.timer -= c.dt;
        return st.timer <= 0.0f ? finish(BTStatus::Success) : BTStatus::Running;
    }
    case BTNodeType::MoveTo: {
        NavMeshAgent* agent = c.world.Get<NavMeshAgent>(c.entity);
        Vec3 target;
        if (!agent || !navigation || !c.PointOf(n.key.View(), target))
            return finish(BTStatus::Failure);
        const Vec3 pos = c.Position();
        const f32 flat = Length(Vec2(target.x - pos.x, target.z - pos.z));
        if (flat <= Max(n.radius, agent->stoppingDistance + 0.05f)) {
            if (!starting)
                navigation->Stop(c.world, c.entity);
            return finish(BTStatus::Success);
        }
        // (Re)send when starting or when the target moved away from where the path leads.
        if (starting || Distance(st.point, target) > Max(1.0f, n.radius)) {
            if (!navigation->SetDestination(c.world, c.entity, target))
                return finish(BTStatus::Failure);
            st.point = target;
            return BTStatus::Running;
        }
        switch (agent->status) {
        case NavAgentStatus::Arrived: return finish(BTStatus::Success);
        case NavAgentStatus::NoMesh: return finish(BTStatus::Failure);
        case NavAgentStatus::Partial:
            return agent->hasDestination ? BTStatus::Running : finish(BTStatus::Failure);
        case NavAgentStatus::Idle: return finish(BTStatus::Failure); // stopped from outside
        default: return BTStatus::Running;
        }
    }
    case BTNodeType::Stop:
        if (navigation)
            navigation->Stop(c.world, c.entity);
        return finish(BTStatus::Success);
    case BTNodeType::Condition: {
        const BBValue* v = c.Get(n.key.View());
        const StringView op = n.op.Empty() ? StringView("set") : n.op.View();
        bool ok = false;
        if (op == "set")
            ok = v && v->Truthy();
        else if (op == "notset")
            ok = !v || !v->Truthy();
        else if (v) {
            const BBValue& b = n.value;
            if (v->type == BBType::String || b.type == BBType::String) {
                const bool eq = v->text == b.text;
                ok = op == "==" ? eq : op == "!=" ? !eq : false;
            } else if (v->type == BBType::Entity) {
                ok = op == "!=" ? v->entity != b.entity : op == "==" && v->entity == b.entity;
            } else if (v->type == BBType::Vec3) {
                const bool eq = LengthSq(v->vec - b.vec) < 1e-6f;
                ok = op == "==" ? eq : op == "!=" ? !eq : false;
            } else {
                const f64 a = v->number, d = b.number;
                ok = op == "==" ? a == d : op == "!=" ? a != d : op == "<" ? a < d : op == ">" ? a > d : op == "<=" ? a <= d : op == ">=" ? a >= d : false;
            }
        } else {
            ok = op == "!=";
        }
        return finish(ok ? BTStatus::Success : BTStatus::Failure);
    }
    case BTNodeType::SetValue:
        c.bt.blackboard.InsertOrAssign(n.key, n.value);
        return finish(BTStatus::Success);
    case BTNodeType::ClearValue:
        c.bt.blackboard.Remove(n.key);
        return finish(BTStatus::Success);
    case BTNodeType::RandomPoint: {
        if (!navigation)
            return finish(BTStatus::Failure);
        const Vec3 pos = c.Position();
        for (u32 attempt = 0; attempt < 8; ++attempt) {
            const f32 a = Random01() * TwoPi, r = Sqrt(Random01()) * Max(n.radius, 0.1f);
            Vec3 p;
            if (navigation->NearestPoint(pos + Vec3(Sin(a) * r, 0.0f, Cos(a) * r), 2.0f, p)) {
                c.bt.blackboard.InsertOrAssign(n.key, BBValue::Vector(p));
                return finish(BTStatus::Success);
            }
        }
        return finish(BTStatus::Failure);
    }
    case BTNodeType::NextWaypoint: {
        ecs::Entity group;
        c.world.Query<const scene::EntityName>().Each([&](ecs::Entity e, const scene::EntityName& name) {
            if (!group && name.value == n.text)
                group = e;
        });
        const Span<const ecs::Entity> points = group ? scene::GetChildren(c.world, group) : Span<const ecs::Entity>();
        if (points.Empty())
            return finish(BTStatus::Failure);
        const ecs::Entity p = points[c.bt.waypoint++ % points.Size()];
        c.bt.blackboard.InsertOrAssign(n.key, BBValue::Vector(scene::ComputeWorldMatrix(c.world, p).GetTranslation()));
        return finish(BTStatus::Success);
    }
    case BTNodeType::FaceTarget: {
        Vec3 target;
        Transform* t = c.world.Get<Transform>(c.entity);
        if (!t || !c.PointOf(n.key.View(), target))
            return finish(BTStatus::Failure);
        const Vec3 pos = c.Position();
        const f32 want = Atan2(target.x - pos.x, target.z - pos.z);
        const f32 diff = WrapAngle(want - Yaw(t->rotation));
        const f32 step = DegToRad(360.0f) * c.dt;
        const f32 turn = Clamp(diff, -step, step);
        t->rotation = Normalize(Quat::FromAxisAngle(Vec3::Up(), turn) * t->rotation);
        return Abs(diff) <= step + DegToRad(5.0f) ? finish(BTStatus::Success) : BTStatus::Running;
    }
    case BTNodeType::Event:
        m_events.PushBack(AIEvent{c.entity, n.text});
        return finish(BTStatus::Success);
    case BTNodeType::Log:
        ZX_LOG_INFO(LogAI, "{}", n.text);
        return finish(BTStatus::Success);
    default: return finish(BTStatus::Failure);
    }
}

void BehaviorSystem::UpdateSight(ecs::World& world, ecs::Entity e, BehaviorTree& bt)
{
    if (bt.targetName.Empty() || bt.sightRange <= 0.0f)
        return;
    ecs::Entity target;
    world.Query<const scene::EntityName>().Each([&](ecs::Entity x, const scene::EntityName& name) {
        if (!target && x != e && name.value == bt.targetName)
            target = x;
    });
    bool visible = false;
    Vec3 targetPos;
    if (target) {
        const Mat4 self = scene::ComputeWorldMatrix(world, e);
        const Vec3 eye = self.GetTranslation() + Vec3(0.0f, bt.eyeHeight, 0.0f);
        targetPos = scene::ComputeWorldMatrix(world, target).GetTranslation();
        const Vec3 aim = targetPos + Vec3(0.0f, 1.0f, 0.0f);
        const Vec3 to = aim - eye;
        const f32 dist = Length(to);
        const Vec3 forward = SafeNormalize(self.TransformVector(Vec3(0, 0, 1)), Vec3(0, 0, 1));
        const f32 cosHalf = Cos(DegToRad(Clamp(bt.sightAngle, 1.0f, 360.0f) * 0.5f));
        visible = dist <= bt.sightRange && (dist < 1e-3f || Dot(to / dist, forward) >= cosHalf);
        if (visible && physics && physics->IsRunning() && dist > 0.5f) {
            PhysicsHit hit;
            if (physics->Raycast(eye, to / dist, dist - 0.3f, hit, ~0u, e))
                visible = hit.entity == target || scene::IsAncestorOf(world, target, hit.entity) || scene::IsAncestorOf(world, e, hit.entity);
        }
    }
    bt.blackboard.InsertOrAssign(String("targetVisible"), BBValue::Bool(visible));
    if (visible) {
        bt.blackboard.InsertOrAssign(String("target"), BBValue::Of(target));
        bt.blackboard.InsertOrAssign(String("targetLastSeen"), BBValue::Vector(targetPos));
    }
}

void BehaviorSystem::Tick(ecs::World& world, f32 dt)
{
    m_events.Clear();
    Array<ecs::Entity> agents;
    world.Query<const BehaviorTree>().Each([&](ecs::Entity e, const BehaviorTree& bt) {
        if (bt.enabled && !bt.tree.Empty())
            agents.PushBack(e);
    });
    for (ecs::Entity e : agents) {
        BehaviorTree* bt = world.Get<BehaviorTree>(e);
        const BehaviorTreeAsset* tree = Load(bt->tree.View());
        if (!tree)
            continue;
        const File* file = m_files.Find(bt->tree);
        if (bt->loadedVersion != file->version) {
            bt->loadedVersion = file->version;
            bt->states.Clear();
            bt->states.Resize(tree->nodes.Size());
            for (const auto& d : tree->defaults)
                if (!bt->blackboard.Contains(d.first))
                    bt->blackboard.InsertOrAssign(d.first, d.second);
        }
        bt->clock += dt;
        UpdateSight(world, e, *bt);
        Context c{world, e, *bt, *tree, dt};
        const BTStatus r = Run(c, 0);
        if (r != BTStatus::Running)
            bt->states[0].status = BTStatus::Idle; // the root restarts next tick
        // A finished root keeps its children's results for debug views until they run again.
        (void)r;
    }
}

} // namespace zx
