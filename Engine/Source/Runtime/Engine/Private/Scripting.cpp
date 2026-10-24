#include "Engine/Scripting.h"
#include "Engine/Localization.h"
#include "Engine/ZGraph.h"
#include "Core/String/Path.h"
#include "Engine/PhysicsSystem.h"
#include "Engine/Surfaces.h"
#include "Core/FileSystem/FileSystem.h"
#include "Core/HAL/PlatformFile.h"
#include "Core/Log/Log.h"
#include "Engine/Animation.h"
#include "Engine/SoftBody.h"
#include "Engine/GameUI.h"
#include "Engine/AudioSystem.h"
#include "Engine/Fire.h"
#include "Engine/Gameplay.h"
#include "Engine/InputActions.h"
#include "Engine/Navigation.h"
#include "Engine/Particles.h"
#include "Platform/Input.h"
#include "Scene/Hierarchy.h"

namespace zx {

ZX_DEFINE_LOG_CATEGORY_STATIC(LogScript, Info);

ZX_REFLECT_STRUCT_BEGIN(Script)
    ZX_FIELD(file)
    ZX_FIELD(properties)
ZX_REFLECT_STRUCT_END()

using script::Value;
using script::ValueType;

// --- Properties ------------------------------------------------------------------------------------------------------------

JsonValue ParseScriptProperties(const Script& s)
{
    JsonValue json;
    if (s.properties.Empty() || !ParseJson(s.properties, json) || !json.IsObject())
        json = JsonValue::MakeObject();
    return json;
}

script::Value JsonToScriptValue(const JsonValue& json, const script::Property& prop)
{
    switch (prop.kind) {
    case script::PropertyKind::Number: return json.IsNumber() ? Value(json.AsDouble()) : prop.defaultValue;
    case script::PropertyKind::Bool: return json.IsBool() ? Value(json.AsBool()) : prop.defaultValue;
    case script::PropertyKind::String: return json.IsString() ? Value(json.AsString()) : prop.defaultValue;
    case script::PropertyKind::Vec3:
    case script::PropertyKind::Color:
        if (json.IsArray() && json.Size() == 3)
            return Value(Vec3(json[0].AsFloat(), json[1].AsFloat(), json[2].AsFloat()));
        return prop.defaultValue;
    }
    return prop.defaultValue;
}

JsonValue ScriptValueToJson(const script::Value& value)
{
    switch (value.Type()) {
    case ValueType::Bool: return JsonValue(value.AsBool());
    case ValueType::Number: return JsonValue(value.AsNumber());
    case ValueType::String: return JsonValue(value.AsString());
    case ValueType::Vec3: {
        const Vec3 v = value.AsVec3();
        JsonValue a = JsonValue::MakeArray();
        a.PushBack(JsonValue(v.x));
        a.PushBack(JsonValue(v.y));
        a.PushBack(JsonValue(v.z));
        return a;
    }
    default: return JsonValue();
    }
}

void SetScriptProperty(Script& s, const script::Property& prop, const script::Value& value)
{
    JsonValue json = ParseScriptProperties(s);
    if (value == prop.defaultValue)
        json.Remove(prop.name);
    else
        json.Set(prop.name, ScriptValueToJson(value));
    s.properties = json.Size() ? ToJsonString(json, JsonWriteOptions{.pretty = false}) : String();
}

String ScriptTemplate(StringView name)
{
    String id;
    for (char c : name)
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || (c >= '0' && c <= '9' && !id.Empty()))
            id += c;
    if (id.Empty())
        id = "NewScript";
    return String::Format(R"zs(// {0}.zs - ZeqouScript (reference: docs/ZeqouScript.md).
// Attach it to an entity with Add Component > Script; props show up in the Details panel.

script {0} {{
    @range(-360, 360) @tooltip("Turn speed in degrees per second")
    prop speed = 45.0

    fn start() {{
        log("{0} started on " + entity.name)
    }}

    fn update(dt) {{
        entity.rotate(vec3(0, speed * dt, 0))
    }}
}}
)zs",
                          id);
}

// --- Native bindings ---------------------------------------------------------------------------------------------------------

namespace {

ScriptSystem& SystemOf(script::NativeObject& o) { return *static_cast<ScriptSystem*>(o.handle); }

Key ParseKey(StringView name)
{
    if (name.EqualsIgnoreCase("ctrl") || name.EqualsIgnoreCase("control"))
        return Key::LeftControl;
    if (name.EqualsIgnoreCase("shift"))
        return Key::LeftShift;
    if (name.EqualsIgnoreCase("alt"))
        return Key::LeftAlt;
    if (name.EqualsIgnoreCase("esc"))
        return Key::Escape;
    for (u32 k = 1; k < u32(Key::Count); ++k)
        if (name.EqualsIgnoreCase(KeyName(Key(k))))
            return Key(k);
    return Key::Unknown;
}

/// Shift/Ctrl/Alt match either side.
bool KeyState(const InputState& in, Key k, i32 mode) // 0 down, 1 pressed, 2 released
{
    auto test = [&](Key key) {
        return mode == 0 ? in.IsKeyDown(key) : mode == 1 ? in.WasKeyPressed(key) : in.WasKeyReleased(key);
    };
    if (k == Key::LeftShift)
        return test(Key::LeftShift) || test(Key::RightShift);
    if (k == Key::LeftControl)
        return test(Key::LeftControl) || test(Key::RightControl);
    if (k == Key::LeftAlt)
        return test(Key::LeftAlt) || test(Key::RightAlt);
    return test(k);
}

bool ParseMouseButton(StringView name, MouseButton& out)
{
    if (name.EqualsIgnoreCase("left"))
        out = MouseButton::Left;
    else if (name.EqualsIgnoreCase("right"))
        out = MouseButton::Right;
    else if (name.EqualsIgnoreCase("middle"))
        out = MouseButton::Middle;
    else
        return false;
    return true;
}

Transform* EntityTransform(script::VM& vm, script::NativeObject& o, ecs::Entity& e)
{
    ScriptSystem& sys = SystemOf(o);
    e = ecs::Entity::Unpack(o.id);
    ecs::World* world = sys.context.world;
    if (!world || !world->IsAlive(e)) {
        vm.Fail("The entity was destroyed");
        return nullptr;
    }
    Transform* t = world->Get<Transform>(e);
    if (!t)
        t = world->Add(e, Transform::Identity());
    return t;
}

bool ToRotation(script::VM& vm, const Value& v, Quat& out)
{
    if (v.Type() == ValueType::Quat)
        out = v.AsQuat();
    else if (v.Type() == ValueType::Vec3)
        out = Quat::FromEulerDegrees(v.AsVec3()); // euler angles in degrees
    else
        return vm.Fail(String::Format("rotation must be a quat or vec3 (degrees), got {}", script::TypeName(v.Type())));
    return true;
}

} // namespace

// --- ScriptSystem --------------------------------------------------------------------------------------------------------------

struct ScriptSystem::CachedProgram
{
    String file;
    script::Program* program = nullptr;
    i64 modifiedTime = -1;
    String error;
    bool errorReported = false;
};

struct ScriptSystem::RunningScript
{
    ecs::Entity entity;
    CachedProgram* source = nullptr;
    UniquePtr<script::Instance> instance;
    Value self;
    Value character;
    bool started = false;
    bool failed = false;
};

ScriptSystem::ScriptSystem() : m_vm(MakeUnique<script::VM>())
{
    script::VM& vm = *m_vm;
    script::RegisterStandardLibrary(vm);
    vm.SetInstructionLimit(20'000'000);
    vm.onLog = [this](StringView s) { Report(s, false); };

    // Entity (also reachable as `transform`): name, position, rotation, scale, forward/right/up, rotate(), translate(),
    // lookAt(), destroy().
    script::NativeType& entityType = vm.AddType("Entity");
    m_entityType = &entityType;
    entityType.get = [](script::VM& v, script::NativeObject& o, StringView m, Value& out) {
        ecs::Entity e;
        if (m == "name") {
            e = ecs::Entity::Unpack(o.id);
            ecs::World* w = SystemOf(o).context.world;
            const scene::EntityName* n = w && w->IsAlive(e) ? w->Get<scene::EntityName>(e) : nullptr;
            out = Value(n ? n->value.View() : StringView());
            return true;
        }
        if (m == "worldPosition" || m == "worldRotation" || m == "worldForward") {
            // World space (positions, rotation and forward otherwise are relative to the parent).
            ecs::World* w = SystemOf(o).context.world;
            e = ecs::Entity::Unpack(o.id);
            if (!w || !w->IsAlive(e))
                return v.Fail("The entity was destroyed");
            const Transform t = Transform::FromMatrix(scene::ComputeWorldMatrix(*w, e));
            if (m == "worldPosition")
                out = Value(t.position);
            else if (m == "worldRotation")
                out = Value(t.rotation);
            else
                out = Value(t.rotation.GetForward());
            return true;
        }
        if (m == "text" || m == "value" || m == "visible") {
            // UI widgets: text (UI Text), value (UI Progress Bar / Slider / Toggle), visible (UI Element).
            ecs::World* w = SystemOf(o).context.world;
            e = ecs::Entity::Unpack(o.id);
            if (!w || !w->IsAlive(e))
                return v.Fail("The entity was destroyed");
            if (m == "text") {
                if (const UIInputField* in = w->Get<const UIInputField>(e)) {
                    out = Value(in->text.View());
                    return true;
                }
                const UIText* t = w->Get<const UIText>(e);
                if (!t)
                    return v.Fail("No UI Text or UI Input Field on this entity");
                out = Value(t->text.View());
            } else if (m == "visible") {
                const UIElement* el = w->Get<const UIElement>(e);
                out = Value(el ? el->visible : true);
            } else if (const UIProgressBar* p = w->Get<const UIProgressBar>(e)) {
                out = Value(f64(p->value));
            } else if (const UISlider* sl = w->Get<const UISlider>(e)) {
                out = Value(f64(sl->value));
            } else if (const UIToggle* tg = w->Get<const UIToggle>(e)) {
                out = Value(tg->on);
            } else {
                return v.Fail("No UI Progress Bar, Slider or Toggle on this entity");
            }
            return true;
        }
        if (m == "textKey") { // UI Text's localization key
            ecs::World* w = SystemOf(o).context.world;
            e = ecs::Entity::Unpack(o.id);
            const UIText* t = w && w->IsAlive(e) ? w->Get<const UIText>(e) : nullptr;
            out = Value(t ? t->key.View() : StringView());
            return true;
        }
        if (m == "alive") {
            ecs::World* w = SystemOf(o).context.world;
            out = Value(w && w->IsAlive(ecs::Entity::Unpack(o.id)));
            return true;
        }
        if (m == "parent") {
            ecs::World* w = SystemOf(o).context.world;
            e = ecs::Entity::Unpack(o.id);
            const ecs::Entity p = w && w->IsAlive(e) ? scene::GetParent(*w, e) : ecs::NullEntity;
            out = p ? script::MakeNative(*o.nativeType, o.handle, p.Pack()) : Value();
            return true;
        }
        if (m == "velocity" || m == "angularVelocity") {
            // The Rigid Body's velocity (zero without one).
            ScriptSystem& sys = SystemOf(o);
            e = ecs::Entity::Unpack(o.id);
            out = Value(sys.physics ? (m == "velocity" ? sys.physics->GetVelocity(e) : sys.physics->GetAngularVelocity(e)) : Vec3());
            return true;
        }
        if (m == "emitting") {
            // The entity's Particle Emitter is emitting (false without one).
            ecs::World* w = SystemOf(o).context.world;
            e = ecs::Entity::Unpack(o.id);
            const ParticleEmitter* pe = w && w->IsAlive(e) ? w->Get<const ParticleEmitter>(e) : nullptr;
            out = Value(pe && pe->playing);
            return true;
        }
        if (m == "burning" || m == "burnt" || m == "fire" || m == "temperature") {
            // Flammable: on fire / burnt out / how much of it is in flames (0..1) / its temperature (degrees C).
            ecs::World* w = SystemOf(o).context.world;
            e = ecs::Entity::Unpack(o.id);
            const Flammable* f = w && w->IsAlive(e) ? w->Get<const Flammable>(e) : nullptr;
            if (m == "burning")
                out = Value(f && f->state == FireState::Burning);
            else if (m == "burnt")
                out = Value(f && f->state == FireState::Burnt);
            else if (m == "fire")
                out = Value(f64(f && f->state == FireState::Burning ? f->intensity : 0.0f));
            else
                out = Value(f64(f ? f->temperature : 20.0f));
            return true;
        }
        if (m == "jointValue") {
            // Hinge angle (radians) / slider offset / distance of the entity's joint (0 without one).
            ScriptSystem& sys = SystemOf(o);
            e = ecs::Entity::Unpack(o.id);
            out = Value(f64(sys.physics ? sys.physics->GetJointValue(e) : 0.0f));
            return true;
        }
        const bool known = m == "position" || m == "rotation" || m == "scale" || m == "forward" || m == "right" || m == "up" ||
                           m == "transform";
        if (!known)
            return false;
        Transform* t = EntityTransform(v, o, e);
        if (!t)
            return false;
        if (m == "position")
            out = Value(t->position);
        else if (m == "rotation")
            out = Value(t->rotation);
        else if (m == "scale")
            out = Value(t->scale);
        else if (m == "forward")
            out = Value(t->rotation.GetForward());
        else if (m == "right")
            out = Value(t->rotation.GetRight());
        else if (m == "up")
            out = Value(t->rotation.GetUp());
        else
            out = script::MakeNative(*o.nativeType, o.handle, o.id); // entity.transform is the entity itself
        return true;
    };
    entityType.set = [](script::VM& v, script::NativeObject& o, StringView m, const Value& value) {
        ecs::Entity e;
        if (m == "text" || m == "value" || m == "visible") {
            ecs::World* w = SystemOf(o).context.world;
            e = ecs::Entity::Unpack(o.id);
            if (!w || !w->IsAlive(e))
                return v.Fail("The entity was destroyed");
            if (m == "text") {
                const String s = value.Type() == ValueType::String ? String(value.AsString()) : value.ToString();
                if (UIInputField* in = w->Get<UIInputField>(e)) {
                    in->text = s;
                    return true;
                }
                UIText* t = w->Get<UIText>(e);
                if (!t)
                    return v.Fail("No UI Text or UI Input Field on this entity");
                t->text = s;
                t->key.Clear(); // the script's text, not a translation
            } else if (m == "visible") {
                UIElement* el = w->Get<UIElement>(e);
                if (!el)
                    return v.Fail("No UI Element on this entity");
                el->visible = value.AsBool();
            } else if (UIProgressBar* p = w->Get<UIProgressBar>(e)) {
                p->value = Clamp(value.AsFloat(), 0.0f, 1.0f);
            } else if (UISlider* sl = w->Get<UISlider>(e)) {
                sl->value = Clamp(value.AsFloat(), Min(sl->minValue, sl->maxValue), Max(sl->minValue, sl->maxValue));
            } else if (UIToggle* tg = w->Get<UIToggle>(e)) {
                tg->on = value.AsBool();
            } else {
                return v.Fail("No UI Progress Bar, Slider or Toggle on this entity");
            }
            return true;
        }
        if (m == "textKey") {
            e = ecs::Entity::Unpack(o.id);
            ecs::World* w = SystemOf(o).context.world;
            UIText* t = w && w->IsAlive(e) ? w->Get<UIText>(e) : nullptr;
            if (!t)
                return v.Fail("No UI Text on this entity");
            t->key = String(value.AsString());
            return true;
        }
        if (m == "name") {
            e = ecs::Entity::Unpack(o.id);
            ecs::World* w = SystemOf(o).context.world;
            if (!w || !w->IsAlive(e))
                return v.Fail("The entity was destroyed");
            w->Set(e, scene::EntityName{String(value.AsString())});
            return true;
        }
        if (m == "worldPosition") {
            if (value.Type() != ValueType::Vec3)
                return v.Fail("worldPosition must be a vec3");
            e = ecs::Entity::Unpack(o.id);
            ecs::World* w = SystemOf(o).context.world;
            Transform* t = w && w->IsAlive(e) ? w->Get<Transform>(e) : nullptr;
            if (!t)
                return v.Fail("The entity was destroyed");
            const ecs::Entity parent = scene::GetParent(*w, e);
            t->position = parent ? Transform::FromMatrix(scene::ComputeWorldMatrix(*w, parent)).Inverse().TransformPoint(value.AsVec3())
                                 : value.AsVec3();
            return true;
        }
        if (m == "emitting") {
            e = ecs::Entity::Unpack(o.id);
            ecs::World* w = SystemOf(o).context.world;
            ParticleEmitter* pe = w && w->IsAlive(e) ? w->Get<ParticleEmitter>(e) : nullptr;
            if (!pe)
                return v.Fail("This entity has no Particle Emitter component (Add Component > Particle Emitter)");
            pe->playing = value.AsBool();
            return true;
        }
        if (m == "velocity" || m == "angularVelocity") {
            if (value.Type() != ValueType::Vec3)
                return v.Fail(String::Format("{} must be a vec3", m));
            ScriptSystem& sys = SystemOf(o);
            e = ecs::Entity::Unpack(o.id);
            if (sys.physics) {
                if (m == "velocity")
                    sys.physics->SetVelocity(e, value.AsVec3());
                else
                    sys.physics->SetAngularVelocity(e, value.AsVec3());
            }
            return true;
        }
        if (m != "position" && m != "rotation" && m != "scale")
            return false;
        Transform* t = EntityTransform(v, o, e);
        if (!t)
            return false;
        if (m == "position") {
            if (value.Type() != ValueType::Vec3)
                return v.Fail("position must be a vec3");
            t->position = value.AsVec3();
        } else if (m == "rotation") {
            if (!ToRotation(v, value, t->rotation))
                return false;
        } else {
            if (value.Type() != ValueType::Vec3 && !value.IsNumber())
                return v.Fail("scale must be a vec3 or a number");
            t->scale = value.AsVec3();
        }
        return true;
    };
    entityType.call = [](script::VM& v, script::NativeObject& o, StringView m, Span<const Value> a, Value& out) {
        ecs::Entity e;
        if (m == "playSound" || m == "stopSound") {
            // The entity's Audio Source.
            ScriptSystem& sys = SystemOf(o);
            e = ecs::Entity::Unpack(o.id);
            if (!sys.audio || !sys.context.world)
                return true;
            if (m == "stopSound") {
                sys.audio->StopEntity(e);
                return true;
            }
            if (!sys.context.world->Has<AudioSource>(e))
                return v.Fail("This entity has no Audio Source component (Add Component > Audio Source)");
            out = Value(sys.audio->PlayEntity(*sys.context.world, e));
            return true;
        }
        if (m == "destroy") {
            e = ecs::Entity::Unpack(o.id);
            SystemOf(o).context.destroyQueue.PushBack(e);
            return true;
        }
        if (m == "moveTo") {
            // moveTo(position, seconds): a smooth move of the local position (doors, lifts, platforms).
            if (a.Empty() || a[0].Type() != ValueType::Vec3 || (a.Size() > 1 && !a[1].IsNumber()))
                return v.Fail("entity.moveTo(position: vec3, seconds)");
            ScriptSystem& sys = SystemOf(o);
            e = ecs::Entity::Unpack(o.id);
            const Transform* t = sys.context.world && sys.context.world->IsAlive(e) ? sys.context.world->Get<const Transform>(e) : nullptr;
            if (!t)
                return v.Fail("The entity was destroyed");
            sys.m_moves.RemoveIf([&](const MoveTween& mt) { return mt.entity == e; });
            sys.m_moves.PushBack(MoveTween{e, t->position, a[0].AsVec3(), a.Size() > 1 ? Max(a[1].AsFloat(), 0.0f) : 1.0f, 0.0f});
            return true;
        }
        if (m == "bbGet" || m == "bbSet") {
            // Behavior Tree blackboard: bbGet(key) -> value (nil if unset), bbSet(key, value) (nil clears it).
            ScriptSystem& sys = SystemOf(o);
            e = ecs::Entity::Unpack(o.id);
            ecs::World* w = sys.context.world;
            if (!w || !w->IsAlive(e))
                return v.Fail("The entity was destroyed");
            BehaviorTree* bt = w->Get<BehaviorTree>(e);
            if (!bt)
                return v.Fail("This entity has no Behavior Tree component (Add Component > Behavior Tree)");
            if (a.Empty() || a[0].Type() != ValueType::String || (m == "bbSet" && a.Size() != 2))
                return v.Fail(m == "bbGet" ? "entity.bbGet(key)" : "entity.bbSet(key, value)");
            const String key(a[0].AsString());
            if (m == "bbGet") {
                const BBValue* bb = bt->blackboard.Find(key);
                if (!bb)
                    out = Value();
                else if (bb->type == BBType::Bool)
                    out = Value(bb->number != 0.0);
                else if (bb->type == BBType::Number)
                    out = Value(bb->number);
                else if (bb->type == BBType::Vec3)
                    out = Value(bb->vec);
                else if (bb->type == BBType::String)
                    out = Value(bb->text.View());
                else if (bb->type == BBType::Entity && w->IsAlive(bb->entity))
                    out = script::MakeNative(*o.nativeType, o.handle, bb->entity.Pack());
                else
                    out = Value();
                return true;
            }
            const Value& val = a[1];
            if (val.Type() == ValueType::Nil)
                bt->blackboard.Remove(key);
            else if (val.Type() == ValueType::Bool)
                bt->blackboard.InsertOrAssign(key, BBValue::Bool(val.AsBool()));
            else if (val.IsNumber())
                bt->blackboard.InsertOrAssign(key, BBValue::Number(val.AsNumber()));
            else if (val.Type() == ValueType::Vec3)
                bt->blackboard.InsertOrAssign(key, BBValue::Vector(val.AsVec3()));
            else if (val.Type() == ValueType::String)
                bt->blackboard.InsertOrAssign(key, BBValue::Text(val.AsString()));
            else if (val.Type() == ValueType::Native && static_cast<const script::NativeObject*>(val.AsObject())->nativeType == o.nativeType)
                bt->blackboard.InsertOrAssign(key, BBValue::Of(ecs::Entity::Unpack(static_cast<const script::NativeObject*>(val.AsObject())->id)));
            else
                return v.Fail("entity.bbSet: the value must be nil, a bool, a number, a vec3, a string or an entity");
            return true;
        }
        if (m == "navTo" || m == "navStop" || m == "navStatus" || m == "navRemaining") {
            // Nav Mesh Agent: navTo(position) walks there on the navigation mesh (false: no agent / mesh), navStop(),
            // navStatus() = "idle" / "moving" / "arrived" / "partial" (unreachable, going as close as possible) /
            // "nomesh", navRemaining() = meters still to walk.
            ScriptSystem& sys = SystemOf(o);
            e = ecs::Entity::Unpack(o.id);
            ecs::World* w = sys.context.world;
            if (!w || !w->IsAlive(e))
                return v.Fail("The entity was destroyed");
            NavMeshAgent* agent = w->Get<NavMeshAgent>(e);
            if (!agent)
                return v.Fail("This entity has no Nav Mesh Agent component (Add Component > Nav Mesh Agent)");
            if (m == "navTo") {
                if (a.Size() != 1 || a[0].Type() != ValueType::Vec3)
                    return v.Fail("entity.navTo(position: vec3)");
                out = Value(sys.navigation && sys.navigation->HasMesh() && sys.navigation->SetDestination(*w, e, a[0].AsVec3()));
            } else if (m == "navStop") {
                if (sys.navigation)
                    sys.navigation->Stop(*w, e);
            } else if (m == "navStatus") {
                out = Value(StringView(NavAgentStatusName(agent->status)));
            } else {
                out = Value(f64(NavigationSystem::RemainingDistance(*w, e)));
            }
            return true;
        }
        if (m == "uiAnimate" || m == "uiPlay" || m == "focus" || m == "addClass" || m == "removeClass" || m == "hasClass") {
            // Game UI: tweens, UI Animation, gamepad focus, style classes.
            ScriptSystem& sys = SystemOf(o);
            ecs::World* w = sys.context.world;
            e = ecs::Entity::Unpack(o.id);
            if (!w || !w->IsAlive(e))
                return v.Fail("The entity was destroyed");
            if (m == "uiAnimate") {
                // uiAnimate(property, to (number / vec3 / color as vec4-ish array), seconds[, easing[, delay]])
                if (a.Size() < 3 || a[0].Type() != ValueType::String || !a[2].IsNumber())
                    return v.Fail("entity.uiAnimate(\"opacity\", 0, 0.3[, \"easeOut\"[, delay]])");
                f32 to[4] = {};
                u32 count = 1;
                if (a[1].IsNumber())
                    to[0] = a[1].AsFloat();
                else if (a[1].Type() == ValueType::Vec3) {
                    const Vec3 c = a[1].AsVec3();
                    to[0] = c.x;
                    to[1] = c.y;
                    to[2] = c.z;
                    to[3] = 1.0f;
                    count = 4;
                } else if (a[1].Type() == ValueType::String) {
                    LinearColor c;
                    if (!ParseStyleColor(a[1].AsString(), c))
                        return v.Fail("uiAnimate: a color is \"#RRGGBB\" / \"#RRGGBBAA\"");
                    to[0] = c.r;
                    to[1] = c.g;
                    to[2] = c.b;
                    to[3] = c.a;
                    count = 4;
                } else
                    return v.Fail("uiAnimate: the target is a number, a vec3 (color) or \"#RRGGBB\"");
                UIEasing easing = UIEasing::EaseOut;
                if (a.Size() > 3 && a[3].Type() == ValueType::String) {
                    const String want = NormalizeStyleName(a[3].AsString());
                    bool found = false;
                    for (const EnumEntry& en : TypeOf<UIEasing>().enumEntries)
                        if (NormalizeStyleName(en.name.View()) == want) {
                            easing = UIEasing(en.value);
                            found = true;
                        }
                    if (!found)
                        return v.Fail("uiAnimate easing: linear, easeIn, easeOut, easeInOut, back, elastic, bounce");
                }
                String error;
                if (!sys.gameUI || !sys.gameUI->Animate(*w, e, a[0].AsString(), Span<const f32>(to, count), a[2].AsFloat(), easing,
                                                         a.Size() > 4 && a[4].IsNumber() ? a[4].AsFloat() : 0.0f, &error))
                    return v.Fail(String::Format("uiAnimate: {}", error));
                return true;
            }
            if (m == "uiPlay") {
                if (!sys.gameUI || !sys.gameUI->PlayAnimation(*w, e))
                    return v.Fail("This entity has no UI Animation component");
                return true;
            }
            if (m == "focus") {
                if (sys.gameUI)
                    sys.gameUI->Focus(e);
                return true;
            }
            UIElement* el = w->Get<UIElement>(e);
            if (!el)
                return v.Fail("This entity has no UI Element");
            if (a.Empty() || a[0].Type() != ValueType::String)
                return v.Fail(String::Format("entity.{}(\"className\")", m));
            const StringView cls = a[0].AsString();
            // Classes are space-separated words.
            Array<String> words;
            {
                StringView rest = el->styleClass.View();
                while (!rest.Empty()) {
                    StringView word, next;
                    if (!rest.Split(' ', word, next))
                        next = {};
                    if (!word.Empty())
                        words.PushBack(String(word));
                    rest = next;
                }
            }
            bool has = false;
            for (const String& wd : words)
                has = has || wd.EqualsIgnoreCase(cls);
            if (m == "hasClass") {
                out = Value(has);
                return true;
            }
            if (m == "addClass" && !has)
                words.PushBack(String(cls));
            if (m == "removeClass")
                words.RemoveIf([&](const String& wd) { return wd.EqualsIgnoreCase(cls); });
            String joined;
            for (const String& wd : words) {
                if (!joined.Empty())
                    joined += " ";
                joined += wd;
            }
            el->styleClass = joined;
            return true;
        }
        if (m == "addForce" || m == "addImpulse" || m == "addTorque") {
            // Rigid Body forces: addForce(newtons), addImpulse(N*s[, worldPoint]), addTorque(N*m).
            if (a.Empty() || a[0].Type() != ValueType::Vec3 || (a.Size() > 1 && a[1].Type() != ValueType::Vec3))
                return v.Fail(String::Format("entity.{}(vec3{})", m, m == "addImpulse" ? "[, point: vec3]" : ""));
            ScriptSystem& sys = SystemOf(o);
            e = ecs::Entity::Unpack(o.id);
            if (!sys.physics)
                return true;
            if (m == "addForce")
                sys.physics->AddForce(e, a[0].AsVec3());
            else if (m == "addTorque")
                sys.physics->AddTorque(e, a[0].AsVec3());
            else if (a.Size() > 1)
                sys.physics->AddImpulseAtPoint(e, a[0].AsVec3(), a[1].AsVec3());
            else
                sys.physics->AddImpulse(e, a[0].AsVec3());
            return true;
        }
        if (m == "launch") {
            // launch(velocity): throws a character (vertical speed, e.g. jump pads) or sets a Rigid Body's velocity.
            if (a.Size() != 1 || a[0].Type() != ValueType::Vec3)
                return v.Fail("entity.launch(velocity: vec3)");
            ScriptSystem& sys = SystemOf(o);
            e = ecs::Entity::Unpack(o.id);
            ecs::World* w = sys.context.world;
            if (CharacterState* st = w && w->IsAlive(e) ? w->Get<CharacterState>(e) : nullptr) {
                st->velocity.y = a[0].AsVec3().y;
                st->jumpSpeed = 0.0f;
                st->grounded = false;
                st->groundBodyIndex = ~0u;
            } else if (sys.physics) {
                sys.physics->SetVelocity(e, a[0].AsVec3());
            }
            return true;
        }
        if (m == "carInput" || m == "carSpeed" || m == "carDamage" || m == "carImpact" || m == "carReset") {
            // Soft-body vehicle on this entity or an ancestor: carInput(throttle, steer, brake[, handbrake]), carSpeed()
            // (m/s, negative in reverse), carDamage() 0..1, carImpact() (hardest hit since the last call, m/s), carReset().
            ScriptSystem& sys = SystemOf(o);
            e = ecs::Entity::Unpack(o.id);
            ecs::World* w = sys.context.world;
            if (!w || !w->IsAlive(e))
                return v.Fail("The entity was destroyed");
            ecs::Entity car;
            for (ecs::Entity x = e; w->IsAlive(x) && !car;) {
                if (w->Has<SoftBody>(x))
                    car = x;
                const scene::Parent* p = w->Get<const scene::Parent>(x);
                if (!p)
                    break;
                x = p->entity;
            }
            if (!car)
                return v.Fail("No Soft Body on this entity or its parents");
            SoftBodySystem* cars = sys.softBodies;
            if (m == "carInput") {
                if (a.Size() < 2 || a.Size() > 4 || !a[0].IsNumber() || !a[1].IsNumber() || (a.Size() > 2 && !a[2].IsNumber()))
                    return v.Fail("entity.carInput(throttle, steer, brake[, handbrake])");
                VehicleInput in;
                in.throttle = a[0].AsFloat();
                in.steer = a[1].AsFloat();
                in.brake = a.Size() > 2 ? a[2].AsFloat() : 0.0f;
                in.handbrake = a.Size() > 3 && (a[3].Type() == ValueType::Bool ? a[3].AsBool() : a[3].IsNumber() && a[3].AsNumber() > 0.5);
                if (cars)
                    cars->SetInput(car, in);
            } else if (m == "carSpeed") {
                out = Value(f64(cars ? cars->Speed(car) : 0.0f));
            } else if (m == "carDamage") {
                out = Value(f64(cars ? cars->Damage(car) : 0.0f));
            } else if (m == "carImpact") {
                out = Value(f64(cars ? cars->TakeImpact(car) : 0.0f));
            } else if (cars) {
                cars->Reset(car);
            }
            return true;
        }
        if (m == "setMorph" || m == "morph") {
            // setMorph(name, weight) / morph(name): Morph Targets weights on this entity or below it (the model's mesh).
            if (a.Empty() || a[0].Type() != ValueType::String || (m == "setMorph" && (a.Size() != 2 || !a[1].IsNumber())))
                return v.Fail(m == "setMorph" ? "entity.setMorph(name, weight)" : "entity.morph(name)");
            ScriptSystem& sys = SystemOf(o);
            e = ecs::Entity::Unpack(o.id);
            ecs::World* w = sys.context.world;
            if (!w || !w->IsAlive(e))
                return v.Fail("The entity was destroyed");
            Array<ecs::Entity> queue;
            queue.PushBack(e);
            for (usize q = 0; q < queue.Size() && q < 256; ++q) {
                if (MorphTargets* mt = w->Get<MorphTargets>(queue[q]))
                    for (usize i = 0; i < mt->names.Size(); ++i)
                        if (mt->names[i].View().EqualsIgnoreCase(a[0].AsString())) {
                            mt->weights.Resize(mt->names.Size(), 0.0f);
                            if (m == "setMorph")
                                mt->weights[i] = f32(a[1].AsNumber());
                            else
                                out = Value(f64(mt->weights[i]));
                            return true;
                        }
                queue.Append(scene::GetChildren(*w, queue[q]));
            }
            return v.Fail(String::Format("No morph target named '{}' on this entity or its children", a[0].AsString()));
        }
        // lookAt: the head of an animated model (Animator here or below); other entities (cameras) turn their transform.
        auto animatedLookAt = [&]() {
            ecs::World* w = SystemOf(o).context.world;
            const ecs::Entity self = ecs::Entity::Unpack(o.id);
            if (!w || !w->IsAlive(self))
                return false;
            Array<ecs::Entity> queue;
            queue.PushBack(self);
            for (usize q = 0; q < queue.Size() && q < 256; ++q) {
                if (w->Has<Animator>(queue[q]))
                    return true;
                queue.Append(scene::GetChildren(*w, queue[q]));
            }
            return false;
        };
        if (m == "animParam" || m == "animTrigger" || m == "playAnim" || m == "animState" || (m == "lookAt" && animatedLookAt())) {
            // Animator on this entity or its first descendant that has one (the mesh under a player):
            // animParam(name, value) sets a graph parameter, animTrigger(name) fires a trigger, playAnim(clip) plays
            // a clip by name (no graph), animState() returns the graph state's name.
            ScriptSystem& sys = SystemOf(o);
            e = ecs::Entity::Unpack(o.id);
            ecs::World* w = sys.context.world;
            if (!w || !w->IsAlive(e))
                return v.Fail("The entity was destroyed");
            ecs::Entity target;
            Array<ecs::Entity> queue;
            queue.PushBack(e);
            for (usize q = 0; q < queue.Size() && !target && q < 256; ++q) {
                if (w->Has<Animator>(queue[q]))
                    target = queue[q];
                else
                    queue.Append(scene::GetChildren(*w, queue[q]));
            }
            if (!target && m == "animState") { // lets scripts check for an animated model
                out = Value(StringView());
                return true;
            }
            if (!target)
                return v.Fail("No Animator on this entity or its children");
            if (m == "lookAt") {
                // lookAt(point [, weight]): the head turns towards a world point; lookAt(nil) looks ahead again.
                if (a.Empty() || a.Size() > 2 || (a[0].Type() != ValueType::Vec3 && !a[0].IsNil()) || (a.Size() == 2 && !a[1].IsNumber()))
                    return v.Fail("entity.lookAt(point: vec3 [, weight]) / entity.lookAt(nil)");
                if (sys.animation)
                    sys.animation->SetLookAt(target, a[0].IsNil() ? Vec3() : a[0].AsVec3(), a[0].IsNil() ? 0.0f : (a.Size() == 2 ? f32(a[1].AsNumber()) : 1.0f));
                return true;
            }
            if (m == "animState") {
                // animState([layer]): 0 / none = the base machine, 1.. = the graph's layers.
                if (a.Size() > 1 || (a.Size() == 1 && !a[0].IsNumber()))
                    return v.Fail("entity.animState([layer: number])");
                const u32 layer = a.Empty() ? 0u : u32(Max(a[0].AsNumber(), 0.0));
                const String state = sys.animation ? sys.animation->CurrentState(target, layer) : String();
                out = Value(state.View());
                return true;
            }
            if (a.Empty() || a[0].Type() != ValueType::String || (m == "animParam" && (a.Size() != 2 || (!a[1].IsNumber() && a[1].Type() != ValueType::Bool))))
                return v.Fail(m == "animParam" ? "entity.animParam(name, value)" : m == "animTrigger" ? "entity.animTrigger(name)" : "entity.playAnim(clip)");
            if (!sys.animation)
                return true;
            const StringView name = a[0].AsString();
            if (m == "animParam")
                sys.animation->SetParam(target, name, a[1].Type() == ValueType::Bool ? (a[1].AsBool() ? 1.0f : 0.0f) : f32(a[1].AsNumber()));
            else if (m == "animTrigger")
                sys.animation->SetTrigger(target, name);
            else if (!sys.animation->Play(*w, target, name))
                return v.Fail(String::Format("The model has no clip named '{}'", name));
            return true;
        }
        if (m == "emit") {
            // emit([count]): a burst from the entity's Particle Emitter (default: its Burst setting, else 10).
            if (a.Size() > 1 || (a.Size() == 1 && !a[0].IsNumber()))
                return v.Fail("entity.emit([count])");
            ScriptSystem& sys = SystemOf(o);
            e = ecs::Entity::Unpack(o.id);
            const ParticleEmitter* pe = sys.context.world ? sys.context.world->Get<const ParticleEmitter>(e) : nullptr;
            if (!pe)
                return v.Fail("This entity has no Particle Emitter component (Add Component > Particle Emitter)");
            const u32 count = a.Size() == 1 ? u32(Clamp(a[0].AsNumber(), 0.0, 100000.0)) : u32(pe->burst > 0 ? pe->burst : 10);
            if (sys.particles)
                sys.particles->Emit(e, count);
            return true;
        }
        if (m == "ignite" || m == "extinguish") {
            // ignite([point]): sets a Flammable entity on fire (at the point closest to `point`, default its bottom);
            // returns false if it isn't Flammable or has burnt out. extinguish(): puts it out.
            if (a.Size() > 1 || (a.Size() == 1 && a[0].Type() != ValueType::Vec3) || (m == "extinguish" && !a.Empty()))
                return v.Fail(m == "ignite" ? "entity.ignite([point: vec3])" : "entity.extinguish()");
            ScriptSystem& sys = SystemOf(o);
            e = ecs::Entity::Unpack(o.id);
            ecs::World* w = sys.context.world;
            if (!w || !w->IsAlive(e))
                return v.Fail("The entity was destroyed");
            if (!w->Has<Flammable>(e))
                return v.Fail("This entity is not Flammable (Add Component > Flammable)");
            if (!sys.fire) {
                out = Value(false);
                return true;
            }
            if (m == "extinguish") {
                sys.fire->Extinguish(*w, e);
                return true;
            }
            const Vec3 point = a.Empty() ? Vec3() : a[0].AsVec3();
            out = Value(sys.fire->Ignite(*w, e, a.Empty() ? nullptr : &point));
            return true;
        }
        if (m == "setMotor") {
            // setMotor(speed[, force]): drives the entity's hinge (rad/s) or slider (m/s) joint; setMotor(nil) turns it off.
            if (a.Empty() || (!a[0].IsNumber() && a[0].Type() != ValueType::Nil) || (a.Size() > 1 && !a[1].IsNumber()))
                return v.Fail("entity.setMotor(speed[, force]) or entity.setMotor(nil)");
            ScriptSystem& sys = SystemOf(o);
            e = ecs::Entity::Unpack(o.id);
            if (sys.physics)
                sys.physics->SetJointMotor(e, a[0].IsNumber(), a[0].IsNumber() ? f32(a[0].AsNumber()) : 0.0f, a.Size() > 1 ? f32(a[1].AsNumber()) : 0.0f);
            return true;
        }
        if (m == "child") {
            // child("Name"): a direct or deeper descendant with that name (nil if none).
            if (a.Size() != 1 || a[0].Type() != ValueType::String)
                return v.Fail("child(\"Name\")");
            ecs::World* w = SystemOf(o).context.world;
            e = ecs::Entity::Unpack(o.id);
            if (!w || !w->IsAlive(e))
                return v.Fail("The entity was destroyed");
            Array<ecs::Entity> stack;
            stack.PushBack(e);
            while (!stack.Empty()) {
                const ecs::Entity cur = stack.PopBackValue();
                if (const scene::Children* ch = w->Get<const scene::Children>(cur))
                    for (ecs::Entity c : ch->entities) {
                        const scene::EntityName* n = w->Get<const scene::EntityName>(c);
                        if (n && n->value == a[0].AsString()) {
                            out = script::MakeNative(*o.nativeType, o.handle, c.Pack());
                            return true;
                        }
                        stack.PushBack(c);
                    }
            }
            return true;
        }
        if (m != "rotate" && m != "translate" && m != "lookAt")
            return false;
        Transform* t = EntityTransform(v, o, e);
        if (!t)
            return false;
        if (m == "rotate") {
            // rotate(vec3 euler degrees) or rotate(axis, degrees); in the entity's own space.
            Quat delta;
            if (a.Size() == 1 && a[0].Type() == ValueType::Vec3)
                delta = Quat::FromEulerDegrees(a[0].AsVec3());
            else if (a.Size() == 2 && a[0].Type() == ValueType::Vec3 && a[1].IsNumber())
                delta = Quat::FromAxisAngle(SafeNormalize(a[0].AsVec3(), Vec3::Up()), DegToRad(a[1].AsFloat()));
            else
                return v.Fail("rotate(eulerDegrees: vec3) or rotate(axis: vec3, degrees)");
            t->rotation = Normalize(t->rotation * delta);
            return true;
        }
        if (m == "translate") {
            // translate(offset) moves along the entity's own axes; translate(offset, true) along world axes.
            if (a.Empty() || a[0].Type() != ValueType::Vec3)
                return v.Fail("translate(offset: vec3[, worldSpace: bool])");
            const Vec3 d = a[0].AsVec3();
            t->position += a.Size() > 1 && a[1].AsBool() ? d : t->rotation * d;
            return true;
        }
        // lookAt(target position[, up])
        if (a.Empty() || a[0].Type() != ValueType::Vec3)
            return v.Fail("lookAt(target: vec3[, up: vec3])");
        const Vec3 dir = a[0].AsVec3() - t->position;
        if (LengthSq(dir) > 1e-10f)
            t->rotation = Quat::LookRotation(Normalize(dir), a.Size() > 1 ? a[1].AsVec3() : Vec3::Up());
        ZX_UNUSED(out);
        return true;
    };

    // character: the entity's CharacterController - move(velocity), jump(speed); grounded, velocity.
    script::NativeType& characterType = vm.AddType("Character");
    m_characterType = &characterType;
    auto characterOf = [](script::VM& v, script::NativeObject& o, CharacterState*& st) -> bool {
        ecs::World* w = SystemOf(o).context.world;
        const ecs::Entity e = ecs::Entity::Unpack(o.id);
        if (!w || !w->IsAlive(e))
            return v.Fail("The entity was destroyed");
        if (!w->Has<CharacterController>(e))
            return v.Fail("This entity has no Character Controller component (Add Component > Character Controller)");
        st = w->Get<CharacterState>(e);
        if (!st)
            st = w->Add(e, CharacterState{});
        return true;
    };
    characterType.get = [characterOf](script::VM& v, script::NativeObject& o, StringView m, Value& out) {
        if (m != "grounded" && m != "velocity" && m != "surface")
            return false;
        CharacterState* st = nullptr;
        if (!characterOf(v, o, st))
            return false;
        if (m == "surface") {
            // What is under the feet (footsteps): a short ray down through the physics world.
            ScriptSystem& sys = SystemOf(o);
            const ecs::Entity e = ecs::Entity::Unpack(o.id);
            StringView surface = "Concrete";
            PhysicsHit hit;
            if (sys.physics && sys.physics->Raycast(scene::ComputeWorldMatrix(*sys.context.world, e).GetTranslation() + Vec3(0.0f, 0.3f, 0.0f), Vec3(0, -1, 0), 1.2f, hit, ~0u, e))
                surface = SurfaceOf(*sys.context.world, hit.entity, sys.materialName);
            out = Value(String(surface));
            return true;
        }
        out = m == "grounded" ? Value(st->grounded) : Value(st->velocity);
        return true;
    };
    characterType.call = [characterOf](script::VM& v, script::NativeObject& o, StringView m, Span<const Value> a, Value&) {
        if (m != "move" && m != "jump")
            return false;
        CharacterState* st = nullptr;
        if (!characterOf(v, o, st))
            return false;
        if (m == "move") {
            // move(velocity): horizontal meters per second for this frame (y is ignored; gravity handles it).
            if (a.Size() != 1 || a[0].Type() != ValueType::Vec3)
                return v.Fail("character.move(velocity: vec3)");
            const Vec3 d = a[0].AsVec3();
            st->desiredMove = Vec3(d.x, 0.0f, d.z);
            return true;
        }
        if (a.Size() > 1 || (a.Size() == 1 && !a[0].IsNumber()))
            return v.Fail("character.jump([speed: number])");
        st->jumpSpeed = a.Empty() ? 5.0f : a[0].AsFloat();
        return true;
    };

    // input: key(name), pressed(name), released(name), mouse(button), mousePressed(button); move, look, mouseDelta,
    // mousePosition, wheel.
    script::NativeType& inputType = vm.AddType("Input");
    inputType.get = [](script::VM&, script::NativeObject& o, StringView m, Value& out) {
        const InputState* in = SystemOf(o).context.input;
        if (m == "move") {
            Vec3 d(0.0f);
            if (in) {
                auto down = [&](Key a, Key b) { return in->IsKeyDown(a) || in->IsKeyDown(b); };
                d.x = (down(Key::D, Key::Right) ? 1.0f : 0.0f) - (down(Key::A, Key::Left) ? 1.0f : 0.0f);
                d.z = (down(Key::W, Key::Up) ? 1.0f : 0.0f) - (down(Key::S, Key::Down) ? 1.0f : 0.0f);
                d.y = (in->IsKeyDown(Key::E) ? 1.0f : 0.0f) - (in->IsKeyDown(Key::Q) ? 1.0f : 0.0f);
                const GamepadState& pad = in->Gamepad(0);
                if (pad.connected) {
                    d.x += pad.leftStick.x;
                    d.z += pad.leftStick.y;
                }
                const f32 len = Length(Vec3(d.x, 0.0f, d.z));
                if (len > 1.0f) {
                    d.x /= len;
                    d.z /= len;
                }
            }
            out = Value(d);
            return true;
        }
        if (m == "look" || m == "mouseDelta") {
            Vec2 d = in ? in->MouseDelta() : Vec2(0.0f, 0.0f);
            if (m == "look" && in && in->Gamepad(0).connected)
                d += in->Gamepad(0).rightStick * Vec2(12.0f, -12.0f);
            out = Value(Vec3(d.x, d.y, 0.0f));
            return true;
        }
        if (m == "mousePosition") {
            const Vec2 p = in ? in->MousePosition() : Vec2(0.0f, 0.0f);
            out = Value(Vec3(p.x, p.y, 0.0f));
            return true;
        }
        if (m == "wheel") {
            out = Value(in ? in->MouseWheel() : 0.0f);
            return true;
        }
        return false;
    };
    inputType.call = [](script::VM& v, script::NativeObject& o, StringView m, Span<const Value> a, Value& out) {
        const InputState* in = SystemOf(o).context.input;
        const i32 keyMode = m == "key" ? 0 : m == "pressed" ? 1 : m == "released" ? 2 : -1;
        if (keyMode >= 0) {
            if (a.Size() != 1 || a[0].Type() != ValueType::String)
                return v.Fail(String::Format("input.{}(\"W\") takes a key name", m));
            // Gamepad buttons: "GamepadA", "Gamepad Start", "Gamepad Up" (the D-pad), ... (the first gamepad).
            if (StringView name = a[0].AsString(); name.Size() > 7 && name.Left(7).EqualsIgnoreCase("Gamepad")) {
                static const char* const PadNames[] = {"a", "b", "x", "y", "lb", "rb", "back", "start", "ls", "rs", "up", "down", "left", "right"};
                String want;
                for (char c : name.DropLeft(7))
                    if (c != ' ')
                        want += (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c;
                for (u32 b = 0; b < ZX_ARRAY_COUNT(PadNames); ++b)
                    if (want == PadNames[b]) {
                        const GamepadButton pb = GamepadButton(b);
                        out = Value(in && (keyMode == 0 ? in->Gamepad(0).buttons[b] : keyMode == 1 ? in->WasGamepadPressed(0, pb) : in->WasGamepadReleased(0, pb)));
                        return true;
                    }
                return v.Fail(String::Format("Unknown gamepad button '{}' (GamepadA, GamepadB, GamepadX, GamepadY, GamepadLB, GamepadRB, "
                                             "GamepadBack, GamepadStart, GamepadLS, GamepadRS, GamepadUp / Down / Left / Right)", name));
            }
            const Key k = ParseKey(a[0].AsString());
            if (k == Key::Unknown)
                return v.Fail(String::Format("Unknown key '{}' (examples: \"W\", \"Space\", \"Shift\", \"Escape\", \"F1\")", a[0].AsString()));
            out = Value(in && KeyState(*in, k, keyMode));
            return true;
        }
        if (m == "action" || m == "actionPressed" || m == "actionReleased" || m == "axis") {
            // Input Actions (Window > Input Actions): action("Jump"), axis("MoveForward").
            if (a.Size() != 1 || a[0].Type() != ValueType::String)
                return v.Fail(String::Format("input.{}(\"Name\") takes an Input Action name", m));
            const InputActionState* actions = SystemOf(o).actions;
            const StringView name = a[0].AsString();
            if (m == "axis")
                out = Value(actions ? actions->Axis(name) : 0.0f);
            else if (m == "action")
                out = Value(actions && actions->Down(name));
            else if (m == "actionPressed")
                out = Value(actions && actions->Pressed(name));
            else
                out = Value(actions && actions->Released(name));
            return true;
        }
        if (m == "mouse" || m == "mousePressed") {
            MouseButton b;
            if (a.Size() != 1 || !ParseMouseButton(a[0].AsString(), b))
                return v.Fail(String::Format("input.{}(\"left\" | \"right\" | \"middle\")", m));
            out = Value(in && (m == "mouse" ? in->IsMouseDown(b) : in->WasMousePressed(b)));
            return true;
        }
        return false;
    };
    vm.SetGlobal("input", script::MakeNative(inputType, this));

    script::NativeType& timeType = vm.AddType("Time");
    timeType.get = [](script::VM&, script::NativeObject& o, StringView m, Value& out) {
        const Context& c = SystemOf(o).context;
        if (m == "dt")
            out = Value(c.dt);
        else if (m == "elapsed")
            out = Value(c.elapsed);
        else if (m == "realDt")
            out = Value(c.realDt);
        else if (m == "scale")
            out = Value(f64(SystemOf(o).timeScale));
        else if (m == "frame")
            out = Value(f64(c.frame));
        else
            return false;
        return true;
    };
    timeType.set = [](script::VM& v, script::NativeObject& o, StringView m, const Value& value) {
        if (m != "scale")
            return false;
        if (!value.IsNumber())
            return v.Fail("time.scale must be a number (0 = paused, 1 = normal)");
        SystemOf(o).timeScale = Clamp(value.AsFloat(), 0.0f, 10.0f);
        return true;
    };
    vm.SetGlobal("time", script::MakeNative(timeType, this));

    // ui: cursor (true shows and frees the mouse while playing: menus), overUI (the mouse is over a button / slider).
    script::NativeType& uiType = vm.AddType("UI");
    uiType.get = [](script::VM&, script::NativeObject& o, StringView m, Value& out) {
        const GameUISystem* ui = SystemOf(o).gameUI;
        if (m == "cursor")
            out = Value(ui && ui->cursorRequested);
        else if (m == "overUI")
            out = Value(ui && ui->MouseOverUI());
        else if (m == "focused") { // name of the widget with the gamepad / keyboard focus ("" = none)
            const ecs::World* w = SystemOf(o).context.world;
            const ecs::Entity f = ui && ui->NavActive() ? ui->NavFocus() : ecs::Entity();
            const scene::EntityName* n = w && f && w->IsAlive(f) ? w->Get<const scene::EntityName>(f) : nullptr;
            out = Value(n ? n->value.View() : StringView());
        }
        else
            return false;
        return true;
    };
    uiType.set = [](script::VM&, script::NativeObject& o, StringView m, const Value& value) {
        if (m != "cursor")
            return false;
        if (GameUISystem* ui = SystemOf(o).gameUI)
            ui->cursorRequested = value.AsBool();
        return true;
    };
    vm.SetGlobal("ui", script::MakeNative(uiType, this));

    // audio: play(file[, volume]) / playAt(file, position[, volume]) -> id, stop(id), setVolume(bus, volume).
    script::NativeType& audioType = vm.AddType("Audio");
    audioType.call = [](script::VM& v, script::NativeObject& o, StringView m, Span<const Value> a, Value& out) {
        AudioSystem* sys = SystemOf(o).audio;
        if (m == "play" || m == "playAt") {
            const bool at = m == "playAt";
            if (a.Empty() || a[0].Type() != ValueType::String || (at && (a.Size() < 2 || a[1].Type() != ValueType::Vec3)))
                return v.Fail(at ? "audio.playAt(\"Sounds/x.wav\", position[, volume])" : "audio.play(\"Sounds/x.wav\"[, volume])");
            const usize volumeArg = at ? 2 : 1;
            const f32 volume = a.Size() > volumeArg && a[volumeArg].IsNumber() ? a[volumeArg].AsFloat() : 1.0f;
            out = Value(f64(sys ? sys->PlayOneShot(a[0].AsString(), volume, at, at ? a[1].AsVec3() : Vec3(0.0f)) : 0u));
            return true;
        }
        if (m == "stop") {
            if (sys && !a.Empty() && a[0].IsNumber())
                sys->Engine().Stop(audio::VoiceId(a[0].AsNumber()), 0.05f);
            return true;
        }
        if (m == "setVolume") {
            if (a.Size() != 2 || a[0].Type() != ValueType::String || !a[1].IsNumber())
                return v.Fail("audio.setVolume(\"Music\" | \"SFX\" | \"UI\" | \"Voice\" | \"Master\", volume)");
            for (u32 b = 0; b < u32(audio::Bus::Count); ++b)
                if (a[0].AsString().EqualsIgnoreCase(audio::BusName(audio::Bus(b)))) {
                    if (sys)
                        sys->Engine().SetBusVolume(audio::Bus(b), a[1].AsFloat());
                    return true;
                }
            return v.Fail(String::Format("Unknown bus '{}'", a[0].AsString()));
        }
        return false;
    };
    vm.SetGlobal("audio", script::MakeNative(audioType, this));

    // fire: ignite(point[, radius]) / extinguish(point[, radius]) -> how many Flammable objects caught fire / went out;
    // burning -> how many are on fire.
    script::NativeType& fireType = vm.AddType("Fire");
    fireType.get = [this](script::VM&, script::NativeObject&, StringView m, Value& out) {
        if (m != "burning")
            return false;
        out = Value(f64(fire ? fire->BurningCount() : 0u));
        return true;
    };
    fireType.call = [this](script::VM& v, script::NativeObject&, StringView m, Span<const Value> a, Value& out) {
        if (m != "ignite" && m != "extinguish")
            return false;
        if (a.Empty() || a.Size() > 2 || a[0].Type() != ValueType::Vec3 || (a.Size() == 2 && !a[1].IsNumber()))
            return v.Fail(String::Format("fire.{}(point: vec3[, radius])", m));
        const f32 radius = a.Size() == 2 ? Max(a[1].AsFloat(), 0.0f) : 0.5f;
        u32 count = 0;
        if (fire && context.world)
            count = m == "ignite" ? fire->IgniteAt(*context.world, a[0].AsVec3(), radius) : fire->ExtinguishAt(*context.world, a[0].AsVec3(), radius);
        out = Value(f64(count));
        return true;
    };
    vm.SetGlobal("fire", script::MakeNative(fireType, this));

    // physics: raycast(origin, direction[, maxDistance[, layers]]) / sphereCast(origin, radius, direction[, maxDistance[,
    // layers]]) -> hit (entity, point, normal, distance) or nil; overlapSphere(center, radius[, layers]) -> [entities];
    // closestPoint(point[, maxDistance[, layers]]) -> hit (nearest surface point; distance 0 inside) or nil;
    // gravity (vec3). `layers` is a layer number or a list of them (default: all).
    script::NativeType& hitType = vm.AddType("PhysicsHit");
    hitType.get = [this](script::VM& v, script::NativeObject& o, StringView m, Value& out) {
        const u32 index = u32(o.id), frame = u32(o.id >> 32);
        if (frame != u32(context.frame) || index >= context.hits.Size())
            return v.Fail("A physics hit is only valid during the frame it was returned");
        const Context::Hit& h = context.hits[index];
        if (m == "entity")
            out = script::MakeNative(*m_entityType, this, h.entity.Pack());
        else if (m == "surface")
            out = Value(String(context.world ? SurfaceOf(*context.world, h.entity, materialName) : StringView("Concrete")));
        else if (m == "point")
            out = Value(h.point);
        else if (m == "normal")
            out = Value(h.normal);
        else if (m == "distance")
            out = Value(h.distance);
        else
            return false;
        return true;
    };
    script::NativeType& physicsType = vm.AddType("Physics");
    auto layerMask = [](const Value* layers) -> u32 {
        if (!layers || layers->IsNil())
            return ~0u;
        if (layers->IsNumber())
            return 1u << (u32(Clamp(layers->AsFloat(), 0.0f, 31.0f)) & 31);
        u32 mask = 0;
        if (const script::ListObject* list = static_cast<const script::ListObject*>(layers->AsObject());
            list && layers->Type() == ValueType::List)
            for (const Value& item : list->items)
                if (item.IsNumber())
                    mask |= 1u << (u32(Clamp(item.AsFloat(), 0.0f, 31.0f)) & 31);
        return mask;
    };
    physicsType.get = [this](script::VM&, script::NativeObject&, StringView m, Value& out) {
        if (m != "gravity")
            return false;
        out = Value(physics && physics->World() ? physics->World()->Settings().gravity : Vec3(0.0f, -9.81f, 0.0f));
        return true;
    };
    physicsType.set = [this](script::VM& v, script::NativeObject&, StringView m, const Value& value) {
        if (m != "gravity")
            return false;
        if (value.Type() != ValueType::Vec3)
            return v.Fail("physics.gravity must be a vec3");
        if (physics && physics->World())
            physics->World()->Settings().gravity = value.AsVec3();
        return true;
    };
    physicsType.call = [this, layerMask](script::VM& v, script::NativeObject&, StringView m, Span<const Value> a, Value& out) {
        auto hitValue = [&](const PhysicsHit& h) {
            context.hits.PushBack(Context::Hit{h.entity, h.point, h.normal, h.distance});
            out = script::MakeNative(*m_hitType, this, (u64(u32(context.frame)) << 32) | u64(context.hits.Size() - 1));
        };
        if (m == "raycast") {
            if (a.Size() < 2 || a[0].Type() != ValueType::Vec3 || a[1].Type() != ValueType::Vec3)
                return v.Fail("physics.raycast(origin: vec3, direction: vec3[, maxDistance[, layers]])");
            const f32 maxDistance = a.Size() > 2 && a[2].IsNumber() ? a[2].AsFloat() : 1000.0f;
            PhysicsHit h;
            if (physics && physics->Raycast(a[0].AsVec3(), a[1].AsVec3(), maxDistance, h, layerMask(a.Size() > 3 ? &a[3] : nullptr)))
                hitValue(h);
            return true;
        }
        if (m == "sphereCast") {
            if (a.Size() < 3 || a[0].Type() != ValueType::Vec3 || !a[1].IsNumber() || a[2].Type() != ValueType::Vec3)
                return v.Fail("physics.sphereCast(origin: vec3, radius, direction: vec3[, maxDistance[, layers]])");
            const f32 maxDistance = a.Size() > 3 && a[3].IsNumber() ? a[3].AsFloat() : 1000.0f;
            PhysicsHit h;
            if (physics && physics->SphereCast(a[0].AsVec3(), a[1].AsFloat(), a[2].AsVec3(), maxDistance, h,
                                               layerMask(a.Size() > 4 ? &a[4] : nullptr)))
                hitValue(h);
            return true;
        }
        if (m == "closestPoint") {
            if (a.Size() < 1 || a[0].Type() != ValueType::Vec3)
                return v.Fail("physics.closestPoint(point: vec3[, maxDistance[, layers]])");
            const f32 maxDistance = a.Size() > 1 && a[1].IsNumber() ? a[1].AsFloat() : 10.0f;
            PhysicsHit h;
            if (physics && physics->ClosestPoint(a[0].AsVec3(), maxDistance, h, layerMask(a.Size() > 2 ? &a[2] : nullptr)))
                hitValue(h);
            return true;
        }
        if (m == "overlapSphere") {
            if (a.Size() < 2 || a[0].Type() != ValueType::Vec3 || !a[1].IsNumber())
                return v.Fail("physics.overlapSphere(center: vec3, radius[, layers])");
            Array<ecs::Entity> found;
            if (physics)
                physics->OverlapSphere(a[0].AsVec3(), a[1].AsFloat(), found, layerMask(a.Size() > 2 ? &a[2] : nullptr));
            script::ListObject* list = new script::ListObject();
            for (ecs::Entity e : found)
                list->items.PushBack(script::MakeNative(*m_entityType, this, e.Pack()));
            out = Value::FromObject(list);
            return true;
        }
        return false;
    };
    m_hitType = &hitType;
    vm.SetGlobal("physics", script::MakeNative(physicsType, this));

    // Localization: tr("key", args...) -> the text in the player's language ({0} {1} = the arguments);
    // lang.current, lang.set("ru"), lang.list() (codes), lang.name("ru") ("Русский").
    vm.SetGlobal("tr", script::MakeNativeFunction("tr", [this](script::VM& v, Span<const Value> a, Value& out) {
                     if (a.Empty() || a[0].Type() != ValueType::String)
                         return v.Fail("tr(\"key\"[, args...])");
                     Array<String> args;
                     for (usize i = 1; i < a.Size(); ++i)
                         args.PushBack(a[i].Type() == ValueType::String ? String(a[i].AsString()) : a[i].ToString());
                     out = Value(localization ? localization->Format(a[0].AsString(), args.View()).View() : a[0].AsString());
                     return true;
                 }));
    // game: values shared by all scripts and graphs while playing - game.get("coins"[, default]), game.set(key, value),
    // game.add(key, amount) (numbers; returns the new value). Cleared when Play starts.
    script::NativeType& gameType = vm.AddType("Game");
    gameType.call = [](script::VM& v, script::NativeObject& o, StringView m, Span<const Value> a, Value& out) {
        ScriptSystem& sys = SystemOf(o);
        if (a.Empty() || a[0].Type() != ValueType::String)
            return v.Fail(String::Format("game.{}(\"key\", ...)", m));
        const String key(a[0].AsString());
        if (m == "get") {
            const Value* found = sys.m_gameValues.Find(key);
            out = found ? *found : (a.Size() > 1 ? a[1] : Value(0.0));
            return true;
        }
        if (m == "set") {
            sys.m_gameValues.InsertOrAssign(key, a.Size() > 1 ? a[1] : Value());
            return true;
        }
        if (m == "add") {
            const Value* found = sys.m_gameValues.Find(key);
            const f64 now = (found && found->IsNumber() ? found->AsFloat() : 0.0) + (a.Size() > 1 && a[1].IsNumber() ? a[1].AsFloat() : 1.0);
            sys.m_gameValues.InsertOrAssign(key, Value(now));
            out = Value(now);
            return true;
        }
        return false;
    };
    vm.SetGlobal("game", script::MakeNative(gameType, this));
    script::NativeType& langType = vm.AddType("Language");
    langType.get = [](script::VM&, script::NativeObject& o, StringView m, Value& out) {
        if (m != "current")
            return false;
        const Localization* l = SystemOf(o).localization;
        out = Value(l ? l->Current() : StringView());
        return true;
    };
    langType.call = [](script::VM& v, script::NativeObject& o, StringView m, Span<const Value> a, Value& out) {
        Localization* l = SystemOf(o).localization;
        if (m == "set") {
            if (a.Size() != 1 || a[0].Type() != ValueType::String)
                return v.Fail("lang.set(\"ru\")");
            out = Value(l && l->SetLanguage(a[0].AsString()));
            return true;
        }
        if (m == "list") {
            script::ListObject* list = new script::ListObject();
            if (l)
                for (const Localization::Language& lg : l->Languages())
                    list->items.PushBack(Value(lg.code.View()));
            out = Value::FromObject(list);
            return true;
        }
        if (m == "name") {
            if (a.Size() != 1 || a[0].Type() != ValueType::String)
                return v.Fail("lang.name(\"ru\")");
            const Localization::Language* lg = l ? l->Find(a[0].AsString()) : nullptr;
            out = Value(lg ? lg->name.View() : a[0].AsString());
            return true;
        }
        return false;
    };
    vm.SetGlobal("lang", script::MakeNative(langType, this));

    // Graph debugging: compiled graphs call __trace(node) before each step (when traceGraphs is on).
    vm.SetGlobal("__trace", script::MakeNativeFunction("__trace", [this](script::VM&, Span<const Value> a, Value&) {
                     if (onGraphTrace && m_current && m_current->source && !a.Empty() && a[0].IsNumber())
                         onGraphTrace(m_current->source->file, m_current->entity, u32(a[0].AsFloat()));
                     return true;
                 }));

    vm.SetGlobal("find", script::MakeNativeFunction("find", [this](script::VM& v, Span<const Value> a, Value& out) {
                     if (a.Size() != 1 || a[0].Type() != ValueType::String)
                         return v.Fail("find(\"Entity Name\")");
                     ecs::World* w = context.world;
                     if (!w)
                         return true;
                     const StringView name = a[0].AsString();
                     ecs::Entity found;
                     w->Query<const scene::EntityName>().Each([&](ecs::Entity e, const scene::EntityName& n) {
                         if (!found && n.value == name)
                             found = e;
                     });
                     if (found)
                         out = script::MakeNative(*m_entityType, this, found.Pack());
                     return true;
                 }));
}

ScriptSystem::~ScriptSystem()
{
    End();
    for (auto& [file, cp] : m_programs)
        script::DestroyProgram(cp->program);
    for (script::Program* p : m_retired)
        script::DestroyProgram(p);
}

void ScriptSystem::SetContentRoot(StringView physicalDir)
{
    if (m_contentRoot == physicalDir)
        return;
    End();
    for (auto& [file, cp] : m_programs)
        script::DestroyProgram(cp->program);
    m_programs.Clear();
    m_contentRoot = physicalDir;
}

void ScriptSystem::Report(StringView message, bool isError)
{
    if (onLog)
        onLog(message, isError);
    else if (isError)
        ZX_LOG_ERROR(LogScript, "{}", message);
    else
        ZX_LOG_INFO(LogScript, "{}", message);
}

ScriptSystem::CachedProgram* ScriptSystem::Load(StringView file)
{
    if (UniquePtr<CachedProgram>* found = m_programs.Find(file))
        return found->Get();
    UniquePtr<CachedProgram> cp = MakeUnique<CachedProgram>();
    cp->file = file;
    CachedProgram* raw = cp.Get();
    m_programs.InsertOrAssign(String(file), move(cp));
    m_checkTimer = 1e9; // compile below
    CheckForChanges();
    return raw;
}

const script::Program* ScriptSystem::GetProgram(StringView file, String* error)
{
    if (file.Empty()) {
        if (error)
            *error = "No script file";
        return nullptr;
    }
    CachedProgram* cp = Load(file);
    if (error)
        *error = cp->error;
    return cp->error.Empty() ? cp->program : nullptr;
}

void ScriptSystem::CheckForChanges()
{
    m_checkTimer = 0.0;
    for (auto& [file, cpPtr] : m_programs) {
        CachedProgram& cp = *cpPtr;
        const String path = String::Format("{}/{}", m_contentRoot, cp.file);
        platform::FileInfo info;
        const i64 mtime = fs::GetInfo(path, info) ? Max<i64>(info.modifiedTime, 1) : 0; // packaged files may have no time
        if (mtime == cp.modifiedTime)
            continue;
        cp.modifiedTime = mtime;
        String source;
        if (!mtime || !fs::ReadTextFile(path, source)) {
            cp.error = String::Format("{}: file not found", cp.file);
            cp.errorReported = false;
            continue;
        }
        // A ZGraph (.zgraph) becomes ZeqouScript source first; its errors name the node they come from.
        Graph graph;
        GraphCompileResult generated;
        const bool isGraph = Path::HasExtension(cp.file, ".zgraph");
        if (isGraph) {
            String graphError;
            if (!ParseGraph(source, graph, &graphError) || !CompileGraph(graph, generated, traceGraphs)) {
                cp.error = String::Format("{}: {}", cp.file, graphError.Empty() ? generated.errors[0] : graphError);
                cp.errorReported = false;
                if (m_running) {
                    Report(cp.error, true);
                    cp.errorReported = true;
                }
                continue;
            }
            source = generated.source;
        }
        script::CompileError ce;
        Result<script::Program*> compiled = script::Compile(source, cp.file, &ce);
        if (!compiled) {
            cp.error = isGraph ? String::Format("{}: {}", cp.file, DescribeGraphError(graph, generated, ce.line, ce.message))
                               : String::Format("{}:{}:{}: {}", cp.file, ce.line, ce.column, ce.message);
            cp.errorReported = false;
            if (m_running) {
                Report(cp.error, true);
                cp.errorReported = true;
            }
            continue; // running instances keep the previous version
        }
        script::Program* old = cp.program;
        cp.program = *compiled;
        cp.error.Clear();
        if (!old)
            continue;

        // Hot reload: rebuild the running instances on the new program, keeping fields with matching names.
        u32 reloaded = 0;
        for (UniquePtr<RunningScript>& rs : m_instances) {
            if (rs->source != &cp || !rs->instance)
                continue;
            script::Instance& prev = *rs->instance;
            Array<Value> preset;
            preset.Resize(cp.program->fields.Size());
            for (usize i = 0; i < cp.program->fields.Size(); ++i)
                if (const Value* v = prev.Field(cp.program->fields[i]); v && v->Type() != ValueType::Function)
                    preset[i] = *v; // (function values point into the old program)
            RebindGlobals(*rs);
            UniquePtr<script::Instance> next = MakeUnique<script::Instance>(*m_vm, cp.program, preset.View());
            // Plain vars keep their running values too (initializers ran for new ones only).
            for (usize i = 0; i < cp.program->fields.Size(); ++i)
                if (!preset[i].IsNil())
                    next->Fields()[i] = preset[i];
            rs->instance = move(next);
            rs->failed = !rs->instance->Ok();
            if (rs->failed)
                Report(m_vm->LastError(), true);
            ++reloaded;
        }
        if (old->refCount == 0)
            script::DestroyProgram(old);
        else
            m_retired.PushBack(old);
        if (m_running)
            Report(String::Format("Reloaded {} ({} running instance(s))", cp.file, reloaded), false);
    }
    m_retired.RemoveIf([](script::Program* p) {
        if (p->refCount)
            return false;
        script::DestroyProgram(p);
        return true;
    });
}

void ScriptSystem::InstancesOf(StringView file, Array<ecs::Entity>& out) const
{
    out.Clear();
    for (const UniquePtr<RunningScript>& rs : m_instances)
        if (rs->source && rs->source->file.EqualsIgnoreCase(file) && !rs->failed)
            out.PushBack(rs->entity);
}

bool ScriptSystem::ReadField(ecs::Entity entity, StringView file, StringView field, String& out) const
{
    for (const UniquePtr<RunningScript>& rs : m_instances)
        if (rs->entity == entity && rs->source && rs->source->file.EqualsIgnoreCase(file) && rs->instance)
            if (const Value* v = rs->instance->Field(field)) {
                out = v->ToString();
                return true;
            }
    return false;
}

void ScriptSystem::RebindGlobals(RunningScript& rs)
{
    m_current = &rs;
    m_vm->SetGlobal("entity", rs.self);
    m_vm->SetGlobal("transform", rs.self);
    m_vm->SetGlobal("character", rs.character);
}

void ScriptSystem::Begin(ecs::World& world)
{
    End();
    m_running = true;
    m_gameValues.Clear();
    m_moves.Clear();
    context = Context{};
    context.world = &world;
    timeScale = 1.0f;
    CheckForChanges();
    for (auto& [file, cp] : m_programs)
        cp->errorReported = false;
}

void ScriptSystem::StartInstances(ecs::World& world)
{
    // Drop instances whose entity or component is gone.
    m_current = nullptr; // (instances may go away)
    m_instances.RemoveIf([&](const UniquePtr<RunningScript>& rs) {
        const Script* s = world.IsAlive(rs->entity) ? world.Get<const Script>(rs->entity) : nullptr;
        return !s || !rs->source || s->file != rs->source->file;
    });
    // New Script components (at Begin: all of them).
    struct Pending
    {
        ecs::Entity entity;
        String file;
        String properties;
    };
    Array<Pending> pending;
    world.Query<const Script>().Each([&](ecs::Entity e, const Script& s) {
        if (s.file.Empty())
            return;
        for (const UniquePtr<RunningScript>& rs : m_instances)
            if (rs->entity == e)
                return;
        pending.PushBack(Pending{e, s.file, s.properties});
    });
    for (Pending& p : pending) {
        UniquePtr<RunningScript> rs = MakeUnique<RunningScript>();
        rs->entity = p.entity;
        rs->source = Load(p.file);
        rs->self = script::MakeNative(*m_entityType, this, p.entity.Pack());
        rs->character = script::MakeNative(*m_characterType, this, p.entity.Pack());
        if (!rs->source->program || !rs->source->error.Empty()) {
            if (!rs->source->errorReported) {
                Report(rs->source->error, true);
                rs->source->errorReported = true;
            }
            rs->failed = true;
            m_instances.PushBack(move(rs));
            continue;
        }
        script::Program* program = rs->source->program;
        Array<Value> preset;
        preset.Resize(program->fields.Size());
        Script tmp;
        tmp.properties = p.properties;
        const JsonValue props = ParseScriptProperties(tmp);
        for (const script::Property& prop : program->properties)
            if (const JsonValue* v = props.Find(prop.name))
                preset[prop.field] = JsonToScriptValue(*v, prop);
        RebindGlobals(*rs);
        rs->instance = MakeUnique<script::Instance>(*m_vm, program, preset.View());
        if (!rs->instance->Ok()) {
            Report(m_vm->LastError(), true);
            rs->failed = true;
        }
        m_instances.PushBack(move(rs));
    }
}

void ScriptSystem::Tick(ecs::World& world, f32 dt, const InputState* input)
{
    if (!m_running)
        Begin(world);
    context.world = &world;
    context.input = input;
    context.dt = dt;
    context.elapsed += f64(dt);
    ++context.frame;
    context.hits.Clear();
    // entity.moveTo: smooth moves (ease in / out), before the scripts look at the positions.
    for (usize i = 0; i < m_moves.Size();) {
        MoveTween& m = m_moves[i];
        Transform* t = world.IsAlive(m.entity) ? world.Get<Transform>(m.entity) : nullptr;
        if (!t) {
            m_moves.RemoveAt(i);
            continue;
        }
        m.elapsed += dt;
        const f32 k = m.duration > 0.0f ? SmoothStep(0.0f, 1.0f, Saturate(m.elapsed / m.duration)) : 1.0f;
        t->position = Lerp(m.from, m.to, k);
        if (k >= 1.0f)
            m_moves.RemoveAt(i);
        else
            ++i;
    }
    m_checkTimer += f64(dt);
    if (m_checkTimer > 0.5)
        CheckForChanges();

    StartInstances(world);
    for (UniquePtr<RunningScript>& rs : m_instances) {
        if (rs->failed || rs->started)
            continue;
        rs->started = true;
        RebindGlobals(*rs);
        if (!rs->instance->Call("start")) {
            Report(m_vm->LastError(), true);
            rs->failed = true;
        }
    }
    const Value dtValue(dt);
    for (UniquePtr<RunningScript>& rs : m_instances) {
        if (rs->failed)
            continue;
        RebindGlobals(*rs);
        if (!rs->instance->Call("update", Span<const Value>(&dtValue, 1))) {
            Report(m_vm->LastError(), true);
            rs->failed = true;
        }
    }
    for (ecs::Entity e : context.destroyQueue)
        if (world.IsAlive(e))
            scene::DestroyHierarchy(world, e);
    context.destroyQueue.Clear();
}

void ScriptSystem::DispatchPhysicsEvents(ecs::World& world, Span<const PhysicsEvent> events)
{
    if (!m_running || events.Empty())
        return;
    context.world = &world;
    // Stay events come every step for every touching pair: skip them unless some script listens.
    bool stayListeners = false;
    for (UniquePtr<RunningScript>& rs : m_instances)
        stayListeners |= !rs->failed && (rs->instance->HasFunction("collisionStay") || rs->instance->HasFunction("triggerStay"));
    for (const PhysicsEvent& ev : events) {
        if (!stayListeners && (ev.type == physics::ContactEventType::TouchStay || ev.type == physics::ContactEventType::SensorStay))
            continue;
        for (u32 side = 0; side < 2; ++side) {
            const ecs::Entity self = side == 0 ? ev.a : ev.b, other = side == 0 ? ev.b : ev.a;
            const char* fn = nullptr;
            switch (ev.type) {
            case physics::ContactEventType::TouchBegin: fn = "collisionEnter"; break;
            case physics::ContactEventType::TouchEnd: fn = "collisionExit"; break;
            case physics::ContactEventType::SensorEnter: fn = "triggerEnter"; break;
            case physics::ContactEventType::SensorExit: fn = "triggerExit"; break;
            case physics::ContactEventType::TouchStay: fn = "collisionStay"; break;
            case physics::ContactEventType::SensorStay: fn = "triggerStay"; break;
            }
            const Value args[2] = {script::MakeNative(*m_entityType, this, other.Pack()), Value(ev.speed)};
            for (UniquePtr<RunningScript>& rs : m_instances) {
                if (rs->failed || !rs->started || rs->entity != self || !rs->instance->HasFunction(fn))
                    continue;
                RebindGlobals(*rs);
                if (!rs->instance->Call(fn, Span<const Value>(args, 2))) {
                    Report(m_vm->LastError(), true);
                    rs->failed = true;
                }
            }
        }
    }
}

void ScriptSystem::DispatchUIEvents(ecs::World& world, Span<const UIEvent> events)
{
    if (!m_running || events.Empty())
        return;
    context.world = &world;
    for (const UIEvent& ev : events) {
        if (!world.IsAlive(ev.entity))
            continue;
        const scene::EntityName* n = world.Get<const scene::EntityName>(ev.entity);
        const bool click = ev.type == UIEvent::Type::Click;
        const bool submit = ev.type == UIEvent::Type::Submit;
        const bool back = ev.type == UIEvent::Type::Back;
        const StringView fn = click ? "onUIClick" : submit ? "onUISubmit" : back ? "onUIBack" : "onUIChanged";
        const Value args[2] = {Value(n ? n->value.View() : StringView()), submit ? Value(ev.text.View()) : Value(f64(ev.value))};
        for (ecs::Entity x = ev.entity; world.IsAlive(x);) {
            for (UniquePtr<RunningScript>& rs : m_instances) {
                if (rs->failed || !rs->started || rs->entity != x || !rs->instance->HasFunction(fn))
                    continue;
                RebindGlobals(*rs);
                if (!rs->instance->Call(fn, Span<const Value>(args, click || back ? 1 : 2))) {
                    Report(m_vm->LastError(), true);
                    rs->failed = true;
                }
            }
            const scene::Parent* p = world.Get<const scene::Parent>(x);
            if (!p)
                break;
            x = p->entity;
        }
    }
}

void ScriptSystem::DispatchAnimEvents(ecs::World& world, Span<const AnimEvent> events)
{
    if (!m_running || events.Empty())
        return;
    context.world = &world;
    for (const AnimEvent& ev : events) {
        const Value args[1] = {Value(ev.name.View())};
        for (ecs::Entity x = ev.entity; world.IsAlive(x);) {
            for (UniquePtr<RunningScript>& rs : m_instances) {
                if (rs->failed || !rs->started || rs->entity != x || !rs->instance->HasFunction("onAnimEvent"))
                    continue;
                RebindGlobals(*rs);
                if (!rs->instance->Call("onAnimEvent", Span<const Value>(args, 1))) {
                    Report(m_vm->LastError(), true);
                    rs->failed = true;
                }
            }
            const scene::Parent* p = world.Get<const scene::Parent>(x);
            if (!p)
                break;
            x = p->entity;
        }
    }
}

void ScriptSystem::DispatchAIEvents(ecs::World& world, Span<const BehaviorSystem::AIEvent> events)
{
    if (!m_running || events.Empty())
        return;
    context.world = &world;
    for (const BehaviorSystem::AIEvent& ev : events) {
        const Value args[1] = {Value(ev.name.View())};
        for (ecs::Entity x = ev.entity; world.IsAlive(x);) {
            for (UniquePtr<RunningScript>& rs : m_instances) {
                if (rs->failed || !rs->started || rs->entity != x || !rs->instance->HasFunction("onAIEvent"))
                    continue;
                RebindGlobals(*rs);
                if (!rs->instance->Call("onAIEvent", Span<const Value>(args, 1))) {
                    Report(m_vm->LastError(), true);
                    rs->failed = true;
                }
            }
            const scene::Parent* p = world.Get<const scene::Parent>(x);
            if (!p)
                break;
            x = p->entity;
        }
    }
}

void ScriptSystem::End()
{
    m_gameValues.Clear();
    m_moves.Clear();
    m_instances.Clear();
    m_current = nullptr;
    if (m_vm) {
        m_vm->SetGlobal("entity", Value());
        m_vm->SetGlobal("transform", Value());
        m_vm->SetGlobal("character", Value());
    }
    context = Context{};
    m_running = false;
    m_retired.RemoveIf([](script::Program* p) {
        if (p->refCount)
            return false;
        script::DestroyProgram(p);
        return true;
    });
}

} // namespace zx
