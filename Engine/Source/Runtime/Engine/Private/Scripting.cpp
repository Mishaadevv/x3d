#include "Engine/Scripting.h"
#include "Core/FileSystem/FileSystem.h"
#include "Core/HAL/PlatformFile.h"
#include "Core/Log/Log.h"
#include "Engine/Gameplay.h"
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
        if (m == "name") {
            e = ecs::Entity::Unpack(o.id);
            ecs::World* w = SystemOf(o).context.world;
            if (!w || !w->IsAlive(e))
                return v.Fail("The entity was destroyed");
            w->Set(e, scene::EntityName{String(value.AsString())});
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
        if (m == "destroy") {
            e = ecs::Entity::Unpack(o.id);
            SystemOf(o).context.destroyQueue.PushBack(e);
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
        if (m != "grounded" && m != "velocity")
            return false;
        CharacterState* st = nullptr;
        if (!characterOf(v, o, st))
            return false;
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
            const Key k = ParseKey(a[0].AsString());
            if (k == Key::Unknown)
                return v.Fail(String::Format("Unknown key '{}' (examples: \"W\", \"Space\", \"Shift\", \"Escape\", \"F1\")", a[0].AsString()));
            out = Value(in && KeyState(*in, k, keyMode));
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
        else if (m == "frame")
            out = Value(f64(c.frame));
        else
            return false;
        return true;
    };
    vm.SetGlobal("time", script::MakeNative(timeType, this));

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
        const i64 mtime = platform::GetFileInfo(path, info) ? info.modifiedTime : 0;
        if (mtime == cp.modifiedTime)
            continue;
        cp.modifiedTime = mtime;
        String source;
        if (!mtime || !fs::ReadTextFile(path, source)) {
            cp.error = String::Format("{}: file not found", cp.file);
            cp.errorReported = false;
            continue;
        }
        script::CompileError ce;
        Result<script::Program*> compiled = script::Compile(source, cp.file, &ce);
        if (!compiled) {
            cp.error = String::Format("{}:{}:{}: {}", cp.file, ce.line, ce.column, ce.message);
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

void ScriptSystem::RebindGlobals(RunningScript& rs)
{
    m_vm->SetGlobal("entity", rs.self);
    m_vm->SetGlobal("transform", rs.self);
    m_vm->SetGlobal("character", rs.character);
}

void ScriptSystem::Begin(ecs::World& world)
{
    End();
    m_running = true;
    context = Context{};
    context.world = &world;
    CheckForChanges();
    for (auto& [file, cp] : m_programs)
        cp->errorReported = false;
}

void ScriptSystem::StartInstances(ecs::World& world)
{
    // Drop instances whose entity or component is gone.
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

void ScriptSystem::End()
{
    m_instances.Clear();
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
