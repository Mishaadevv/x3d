#include "Engine/GameModule.h"
#include "Engine/PhysicsSystem.h"
#include "Core/FileSystem/FileSystem.h"
#include "Core/HAL/PlatformFile.h"
#include "Core/Log/Log.h"
#include "Core/String/Path.h"
#include "Core/String/Utf.h"
#include "Core/Time/Time.h"
#include "Engine/AudioSystem.h"
#include "Engine/BuiltinAssets.h"
#include "Engine/Gameplay.h"
#include "Engine/SceneRendering.h"
#include "Platform/Input.h"
#include "Platform/Window.h"
#include "Scene/Hierarchy.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace zx {

ZX_REFLECT_STRUCT_BEGIN(NativeScript)
    ZX_FIELD(className)
    ZX_FIELD(properties)
ZX_REFLECT_STRUCT_END()

ZX_DEFINE_LOG_CATEGORY_STATIC(LogGame, Info);

namespace {

Vec3 ToEngine(game::Vec3 v) { return Vec3(v.x, v.y, v.z); }
game::Vec3 ToGame(Vec3 v) { return game::Vec3(v.x, v.y, v.z); }
Quat ToEngine(game::Quat q) { return Quat(q.x, q.y, q.z, q.w); }
game::Quat ToGame(Quat q) { return game::Quat{q.x, q.y, q.z, q.w}; }

game::EntityId ToId(ecs::Entity e) { return e ? e.Pack() : 0; }
ecs::Entity FromId(game::EntityId id) { return ecs::Entity::Unpack(id); }

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

bool KeyState(const InputState& in, Key k, bool pressed)
{
    auto test = [&](Key key) { return pressed ? in.WasKeyPressed(key) : in.IsKeyDown(key); };
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
    if (name.EqualsIgnoreCase("left") || name.EqualsIgnoreCase("Mouse Left"))
        out = MouseButton::Left;
    else if (name.EqualsIgnoreCase("right") || name.EqualsIgnoreCase("Mouse Right"))
        out = MouseButton::Right;
    else if (name.EqualsIgnoreCase("middle") || name.EqualsIgnoreCase("Mouse Middle"))
        out = MouseButton::Middle;
    else
        return false;
    return true;
}

template <typename T>
T& Field(game::ScriptComponent* object, u32 offset)
{
    return *reinterpret_cast<T*>(reinterpret_cast<u8*>(object) + offset);
}

template <typename T>
const T& Field(const game::ScriptComponent* object, u32 offset)
{
    return *reinterpret_cast<const T*>(reinterpret_cast<const u8*>(object) + offset);
}

f32 ClampProp(const game::PropDesc& p, f32 v) { return p.min < p.max ? Clamp(v, p.min, p.max) : v; }

} // namespace

// --- NativeScript props ---------------------------------------------------------------------------------------------------

JsonValue ParseNativeScriptProperties(const NativeScript& script)
{
    JsonValue json;
    if (script.properties.Empty() || !ParseJson(script.properties, json) || !json.IsObject())
        return JsonValue::MakeObject();
    return json;
}

void SetNativeScriptProperty(NativeScript& script, const game::ComponentDesc& desc, StringView prop, const JsonValue& value)
{
    JsonValue props = ParseNativeScriptProperties(script);
    const JsonValue defaults = GameModuleSystem::DefaultProps(desc);
    const JsonValue* def = defaults.Find(prop);
    if (def && *def == value)
        props.Remove(prop);
    else
        props.Set(prop, value);
    script.properties = props.Size() ? ToJsonString(props) : String();
}

JsonValue GameModuleSystem::ReadProps(const game::ComponentDesc& desc, const game::ScriptComponent* object)
{
    JsonValue out = JsonValue::MakeObject();
    for (u32 i = 0; i < desc.propCount; ++i) {
        const game::PropDesc& p = desc.props[i];
        switch (p.type) {
        case game::PropType::Float: out.Set(p.name, Field<float>(object, p.offset)); break;
        case game::PropType::Int: out.Set(p.name, i64(Field<int>(object, p.offset))); break;
        case game::PropType::Bool: out.Set(p.name, Field<bool>(object, p.offset)); break;
        case game::PropType::Vec3: {
            const game::Vec3& v = Field<game::Vec3>(object, p.offset);
            JsonValue a = JsonValue::MakeArray();
            a.PushBack(v.x);
            a.PushBack(v.y);
            a.PushBack(v.z);
            out.Set(p.name, move(a));
            break;
        }
        }
    }
    return out;
}

void GameModuleSystem::WriteProps(const game::ComponentDesc& desc, game::ScriptComponent* object, const JsonValue& values)
{
    if (!values.IsObject())
        return;
    for (u32 i = 0; i < desc.propCount; ++i) {
        const game::PropDesc& p = desc.props[i];
        const JsonValue* v = values.Find(p.name);
        if (!v)
            continue;
        switch (p.type) {
        case game::PropType::Float:
            if (v->IsNumber())
                Field<float>(object, p.offset) = ClampProp(p, v->AsFloat());
            break;
        case game::PropType::Int:
            if (v->IsNumber())
                Field<int>(object, p.offset) = int(ClampProp(p, f32(v->AsDouble() < 0.0 ? v->AsDouble() - 0.5 : v->AsDouble() + 0.5)));
            break;
        case game::PropType::Bool:
            if (v->IsBool())
                Field<bool>(object, p.offset) = v->AsBool();
            else if (v->IsNumber())
                Field<bool>(object, p.offset) = v->AsDouble() != 0.0;
            break;
        case game::PropType::Vec3:
            if (v->IsArray() && v->Size() == 3)
                Field<game::Vec3>(object, p.offset) = game::Vec3((*v)[0].AsFloat(), (*v)[1].AsFloat(), (*v)[2].AsFloat());
            else if (v->IsNumber())
                Field<game::Vec3>(object, p.offset) = game::Vec3(v->AsFloat(), v->AsFloat(), v->AsFloat());
            break;
        }
    }
}

JsonValue GameModuleSystem::DefaultProps(const game::ComponentDesc& desc)
{
    game::ScriptComponent* object = desc.create();
    JsonValue out = ReadProps(desc, object);
    desc.destroy(object);
    return out;
}

// --- Host --------------------------------------------------------------------------------------------------------------------

class GameModuleSystem::Host final : public game::Host
{
public:
    explicit Host(GameModuleSystem& system) : m_system(system) {}

    ecs::World* world = nullptr;
    GameTickContext context;
    f32 dt = 0.0f;
    f64 elapsed = 0.0;
    Array<ecs::Entity> destroyQueue;

    void Log(const char* message, bool isError) override { m_system.Report(StringView(message ? message : ""), isError); }

    const char* GetName(game::EntityId id) override
    {
        const ecs::Entity e = FromId(id);
        const scene::EntityName* n = Alive(e) ? world->Get<const scene::EntityName>(e) : nullptr;
        m_name = n ? n->value.View() : StringView();
        return m_name.CStr();
    }

    game::EntityId Find(const char* name) override
    {
        if (!world || !name)
            return 0;
        const StringView wanted(name);
        ecs::Entity found;
        world->Query<const scene::EntityName>().Each([&](ecs::Entity e, const scene::EntityName& n) {
            if (!found && n.value == wanted)
                found = e;
        });
        return ToId(found);
    }

    bool IsAlive(game::EntityId id) override { return Alive(FromId(id)); }

    void Destroy(game::EntityId id) override
    {
        const ecs::Entity e = FromId(id);
        if (Alive(e))
            destroyQueue.PushBack(e);
    }

    game::EntityId Spawn(const char* shape, game::Vec3 position) override
    {
        if (!world)
            return 0;
        const StringView s(shape ? shape : "");
        const ecs::Entity e = scene::CreateNode(*world, s.Empty() ? StringView("Entity") : s, Transform(ToEngine(position)));
        for (u32 i = 0; i < u32(BuiltinShape::Count); ++i)
            if (s.EqualsIgnoreCase(builtin::ShapeName(BuiltinShape(i)))) {
                MeshRenderer mr;
                mr.mesh = builtin::Mesh(BuiltinShape(i));
                world->Add(e, mr);
                break;
            }
        return ToId(e);
    }

    game::Vec3 GetPosition(game::EntityId id) override
    {
        const Transform* t = Get(id);
        return t ? ToGame(t->position) : game::Vec3();
    }
    void SetPosition(game::EntityId id, game::Vec3 p) override
    {
        if (Transform* t = Get(id))
            t->position = ToEngine(p);
    }
    game::Quat GetRotation(game::EntityId id) override
    {
        const Transform* t = Get(id);
        return t ? ToGame(t->rotation) : game::Quat();
    }
    void SetRotation(game::EntityId id, game::Quat q) override
    {
        if (Transform* t = Get(id))
            t->rotation = Normalize(ToEngine(q));
    }
    game::Vec3 GetScale(game::EntityId id) override
    {
        const Transform* t = Get(id);
        return t ? ToGame(t->scale) : game::Vec3(1, 1, 1);
    }
    void SetScale(game::EntityId id, game::Vec3 s) override
    {
        if (Transform* t = Get(id))
            t->scale = ToEngine(s);
    }

    bool KeyDown(const char* key) override { return KeyTest(key, false); }
    bool KeyPressed(const char* key) override { return KeyTest(key, true); }
    bool ActionDown(const char* action) override { return context.actions && action && context.actions->Down(action); }
    bool ActionPressed(const char* action) override { return context.actions && action && context.actions->Pressed(action); }
    float Axis(const char* axis) override { return context.actions && axis ? context.actions->Axis(axis) : 0.0f; }
    game::Vec3 MouseDelta() override
    {
        if (!context.input)
            return game::Vec3();
        const Vec2 d = context.input->MouseDelta();
        return game::Vec3(d.x, d.y, 0.0f);
    }

    float DeltaTime() override { return dt; }
    double Time() override { return elapsed; }

    void CharacterMove(game::EntityId id, game::Vec3 velocity) override
    {
        if (CharacterState* st = Character(id))
            st->desiredMove = Vec3(velocity.x, 0.0f, velocity.z);
    }
    void CharacterJump(game::EntityId id, float speed) override
    {
        if (CharacterState* st = Character(id))
            st->jumpSpeed = speed > 0.0f ? speed : 5.0f;
    }
    bool CharacterGrounded(game::EntityId id) override
    {
        const CharacterState* st = Character(id);
        return st && st->grounded;
    }

    uint32_t PlaySound(const char* file, float volume, bool spatial, game::Vec3 position) override
    {
        if (!context.audio || !file)
            return 0;
        return u32(context.audio->PlayOneShot(StringView(file), volume, spatial, ToEngine(position)));
    }
    void StopSound(uint32_t voice) override
    {
        if (context.audio && voice)
            context.audio->Engine().Stop(audio::VoiceId(voice), 0.05f);
    }

    void AddForce(game::EntityId id, game::Vec3 force) override
    {
        if (context.physics)
            context.physics->AddForce(FromId(id), ToEngine(force));
    }
    void AddImpulse(game::EntityId id, game::Vec3 impulse) override
    {
        if (context.physics)
            context.physics->AddImpulse(FromId(id), ToEngine(impulse));
    }
    game::Vec3 GetVelocity(game::EntityId id) override
    {
        return ToGame(context.physics ? context.physics->GetVelocity(FromId(id)) : Vec3());
    }
    void SetVelocity(game::EntityId id, game::Vec3 v) override
    {
        if (context.physics)
            context.physics->SetVelocity(FromId(id), ToEngine(v));
    }
    bool Raycast(game::Vec3 origin, game::Vec3 direction, float maxDistance, game::RaycastResult* hit, uint32_t layerMask) override
    {
        PhysicsHit h;
        if (!context.physics || !context.physics->Raycast(ToEngine(origin), ToEngine(direction), maxDistance, h, layerMask))
            return false;
        if (hit) {
            hit->entity = ToId(h.entity);
            hit->point = ToGame(h.point);
            hit->normal = ToGame(h.normal);
            hit->distance = h.distance;
        }
        return true;
    }

private:
    bool Alive(ecs::Entity e) const { return world && e && world->IsAlive(e); }

    Transform* Get(game::EntityId id)
    {
        const ecs::Entity e = FromId(id);
        if (!Alive(e))
            return nullptr;
        Transform* t = world->Get<Transform>(e);
        return t ? t : world->Add(e, Transform::Identity());
    }

    CharacterState* Character(game::EntityId id)
    {
        const ecs::Entity e = FromId(id);
        if (!Alive(e) || !world->Has<CharacterController>(e))
            return nullptr;
        CharacterState* st = world->Get<CharacterState>(e);
        return st ? st : world->Add(e, CharacterState{});
    }

    bool KeyTest(const char* name, bool pressed) const
    {
        if (!context.input || !name)
            return false;
        MouseButton b;
        if (ParseMouseButton(name, b))
            return pressed ? context.input->WasMousePressed(b) : context.input->IsMouseDown(b);
        const zx::Key k = ParseKey(name);
        return k != zx::Key::Unknown && KeyState(*context.input, k, pressed);
    }

    GameModuleSystem& m_system;
    String m_name;
};

// --- GameModuleSystem -------------------------------------------------------------------------------------------------------

GameModuleSystem::GameModuleSystem() : m_host(MakeUnique<Host>(*this)) {}

GameModuleSystem::~GameModuleSystem()
{
    if (m_buildThread.IsJoinable())
        m_buildThread.Join();
    End();
    Unload();
}

void GameModuleSystem::SetProject(StringView projectRoot, StringView name, StringView engineRoot)
{
    if (m_root == projectRoot && m_name == name)
        return;
    End();
    Unload();
    m_root = projectRoot;
    m_name = name;
    m_engineRoot = engineRoot;
    m_reportedMissing.Clear();
    // Copies from earlier sessions (no longer locked).
    if (!m_root.Empty())
        platform::DeleteDirectoryTree(Path::Join(m_root, "Intermediate/HotReload"));
}

String GameModuleSystem::DllPath() const { return Path::Join(m_root, String::Format("Binaries/{}.dll", m_name)); }

void GameModuleSystem::Report(StringView message, bool isError)
{
    if (onLog)
        onLog(message, isError);
    else if (isError)
        ZX_LOG_ERROR(LogGame, "{}", message);
    else
        ZX_LOG_INFO(LogGame, "{}", message);
}

bool GameModuleSystem::StartBuild()
{
    if (m_root.Empty() || m_name.Empty() || GameBuildState(m_buildState.load()) == GameBuildState::Building)
        return false;
    if (m_buildThread.IsJoinable())
        m_buildThread.Join();
    const String logPath = Path::Join(m_root, "Saved/Logs/GameBuild.log");
    platform::CreateDirectoryTree(Path::Join(m_root, "Saved/Logs"));
    const String script = Path::Join(m_engineRoot, "Tools/build_game.ps1");
    String command = String::Format("powershell -NoProfile -ExecutionPolicy Bypass -File \"{}\" -Project \"{}\" -Name \"{}\" -Engine \"{}\" -Log \"{}\"",
                                    script, m_root, m_name, m_engineRoot, logPath);
    command.ReplaceChar('/', '\\');
    m_buildFinished = false;
    m_buildExitCode = 0;
    m_buildState = u32(GameBuildState::Building);
    m_buildStart = Clock::NowSeconds();
    m_buildLog.Clear();
    String root = m_root;
    return m_buildThread.Start("GameBuild", [this, command = move(command), root = move(root)] {
        i32 code = 1;
        if (!platform::RunProcess(command, root, &code, 10 * 60 * 1000))
            code = code ? code : 1;
        m_buildExitCode = code;
        m_buildFinished = true;
    });
}

bool GameModuleSystem::BuildNow()
{
    if (!StartBuild())
        return false;
    m_buildThread.Join();
    return PollBuild() == GameBuildState::Succeeded;
}

GameBuildState GameModuleSystem::PollBuild()
{
    if (GameBuildState(m_buildState.load()) == GameBuildState::Building && m_buildFinished.load()) {
        if (m_buildThread.IsJoinable())
            m_buildThread.Join();
        m_buildSeconds = Clock::NowSeconds() - m_buildStart;
        if (!fs::ReadTextFile(Path::Join(m_root, "Saved/Logs/GameBuild.log"), m_buildLog))
            m_buildLog = "(no build log)";
        const bool ok = m_buildExitCode.load() == 0 && platform::FileExists(DllPath());
        m_buildState = u32(ok ? GameBuildState::Succeeded : GameBuildState::Failed);
    }
    return GameBuildState(m_buildState.load());
}

bool GameModuleSystem::Reload(String* error)
{
    auto fail = [&](StringView why) {
        if (error)
            *error = why;
        return false;
    };
    const String dll = DllPath();
    if (!platform::FileExists(dll))
        return fail(String::Format("{} was not built yet", dll));
    // Load a copy, so the build can overwrite the DLL while this one is loaded.
    const String dir = Path::Join(m_root, "Intermediate/HotReload");
    platform::CreateDirectoryTree(dir);
    const String copy = Path::Join(dir, String::Format("{}-{}.dll", m_name, ++m_copyCounter));
    if (!platform::CopyFileAt(dll, copy, true))
        return fail(String::Format("Can't copy {} to {}", dll, copy));
    const Array<wchar_t> wide = Utf8ToUtf16(copy);
    HMODULE module = ::LoadLibraryW(wide.Data());
    if (!module) {
        platform::DeleteFileAt(copy);
        return fail(String::Format("LoadLibrary failed for {} (error {})", copy, u32(::GetLastError())));
    }
    using GetModuleFn = const game::ModuleDesc* (*)();
    auto get = reinterpret_cast<GetModuleFn>(reinterpret_cast<void*>(::GetProcAddress(module, "ZxGetGameModule")));
    const game::ModuleDesc* desc = get ? get() : nullptr;
    if (!desc || desc->apiVersion != game::ApiVersion) {
        ::FreeLibrary(module);
        platform::DeleteFileAt(copy);
        return fail(desc ? String::Format("The module was built with Game SDK version {} (the engine uses {})", desc->apiVersion, game::ApiVersion)
                         : String("The DLL does not export ZxGetGameModule (ZX_IMPLEMENT_GAME_MODULE missing?)"));
    }
    return SwapModule(desc, module, copy, error);
}

bool GameModuleSystem::LoadDll(StringView path, String* error)
{
    const Array<wchar_t> wide = Utf8ToUtf16(path);
    HMODULE module = ::LoadLibraryW(wide.Data());
    if (!module) {
        if (error)
            *error = String::Format("LoadLibrary failed for {} (error {})", path, u32(::GetLastError()));
        return false;
    }
    using GetModuleFn = const game::ModuleDesc* (*)();
    auto get = reinterpret_cast<GetModuleFn>(reinterpret_cast<void*>(::GetProcAddress(module, "ZxGetGameModule")));
    const game::ModuleDesc* desc = get ? get() : nullptr;
    if (!desc || desc->apiVersion != game::ApiVersion) {
        ::FreeLibrary(module);
        if (error)
            *error = desc ? String::Format("{} was built with Game SDK version {} (the engine uses {})", path, desc->apiVersion, game::ApiVersion)
                          : String::Format("{} does not export ZxGetGameModule", path);
        return false;
    }
    return SwapModule(desc, module, {}, error);
}

bool GameModuleSystem::LoadFromDesc(const game::ModuleDesc* desc)
{
    if (!desc || desc->apiVersion != game::ApiVersion)
        return false;
    return SwapModule(desc, nullptr, {}, nullptr);
}

bool GameModuleSystem::SwapModule(const game::ModuleDesc* desc, void* library, StringView copyPath, String*)
{
    // Save the running components (class + props), destroy them on the old code.
    struct Saved
    {
        ecs::Entity entity;
        String className;
        JsonValue props;
    };
    Array<Saved> saved;
    for (const Instance& in : m_instances)
        saved.PushBack(Saved{in.entity, in.className, ReadProps(*in.desc, in.object)});
    DestroyInstances(false);

    void* oldLibrary = m_library;
    const String oldCopy = m_loadedCopy;
    m_desc = desc;
    m_library = library;
    m_loadedCopy = copyPath;
    ++m_generation;
    m_reportedMissing.Clear();
    if (oldLibrary) {
        ::FreeLibrary(static_cast<HMODULE>(oldLibrary));
        platform::DeleteFileAt(oldCopy);
    }

    // Recreate on the new code with the same props.
    for (Saved& s : saved) {
        const game::ComponentDesc* cd = FindComponent(s.className);
        if (!cd) {
            Report(String::Format("Hot reload: component {} no longer exists", s.className), true);
            continue;
        }
        Instance in;
        in.entity = s.entity;
        in.className = s.className;
        in.desc = cd;
        in.object = cd->create();
        in.object->entity = ToId(s.entity);
        in.object->host = m_host.Get();
        WriteProps(*cd, in.object, s.props);
        in.started = true;
        m_instances.PushBack(move(in));
    }
    for (Instance& in : m_instances)
        in.object->OnReloaded();
    if (m_world)
        for (ecs::Entity e : m_host->destroyQueue)
            if (m_world->IsAlive(e))
                scene::DestroyHierarchy(*m_world, e);
    m_host->destroyQueue.Clear();
    Report(String::Format("Game module {} loaded (generation {}, {} components)", ModuleName(), m_generation, Components().Size()), false);
    return true;
}

void GameModuleSystem::Unload()
{
    DestroyInstances(m_running);
    m_desc = nullptr;
    if (m_library) {
        ::FreeLibrary(static_cast<HMODULE>(m_library));
        m_library = nullptr;
        platform::DeleteFileAt(m_loadedCopy);
    }
    m_loadedCopy.Clear();
}

Array<const game::ComponentDesc*> GameModuleSystem::Components() const
{
    Array<const game::ComponentDesc*> out;
    for (const game::ComponentDesc* c = m_desc ? m_desc->first : nullptr; c; c = c->next)
        out.PushBack(c);
    // Registration order is reversed by the linked list; show them alphabetically.
    for (usize i = 1; i < out.Size(); ++i)
        for (usize j = i; j > 0 && StringView(out[j]->name).Compare(StringView(out[j - 1]->name)) < 0; --j)
        {
            const game::ComponentDesc* t = out[j];
            out[j] = out[j - 1];
            out[j - 1] = t;
        }
    return out;
}

const game::ComponentDesc* GameModuleSystem::FindComponent(StringView className) const
{
    for (const game::ComponentDesc* c = m_desc ? m_desc->first : nullptr; c; c = c->next)
        if (className == c->name)
            return c;
    return nullptr;
}

JsonValue GameModuleSystem::RunningProps(ecs::Entity entity) const
{
    for (const Instance& in : m_instances)
        if (in.entity == entity)
            return ReadProps(*in.desc, in.object);
    return JsonValue();
}

void GameModuleSystem::DestroyInstances(bool callStop)
{
    if (callStop)
        for (Instance& in : m_instances)
            if (in.started)
                in.object->OnStop();
    for (Instance& in : m_instances)
        in.desc->destroy(in.object);
    m_instances.Clear();
}

void GameModuleSystem::Begin(ecs::World& world)
{
    End();
    m_running = true;
    m_world = &world;
    m_host->world = &world;
    m_host->elapsed = 0.0;
    m_reportedMissing.Clear();
}

void GameModuleSystem::StartInstances(ecs::World& world)
{
    // Drop instances whose entity or component went away (or now names another class).
    for (usize i = 0; i < m_instances.Size();) {
        Instance& in = m_instances[i];
        const NativeScript* ns = world.IsAlive(in.entity) ? world.Get<const NativeScript>(in.entity) : nullptr;
        if (ns && ns->className == in.className) {
            ++i;
            continue;
        }
        if (in.started)
            in.object->OnStop();
        in.desc->destroy(in.object);
        m_instances.RemoveAt(i);
    }
    if (!m_desc)
        return;
    struct Pending
    {
        ecs::Entity entity;
        String className;
        String properties;
    };
    Array<Pending> pending;
    world.Query<const NativeScript>().Each([&](ecs::Entity e, const NativeScript& ns) {
        if (ns.className.Empty())
            return;
        for (const Instance& in : m_instances)
            if (in.entity == e)
                return;
        pending.PushBack(Pending{e, ns.className, ns.properties});
    });
    for (Pending& p : pending) {
        const game::ComponentDesc* cd = FindComponent(p.className);
        if (!cd) {
            if (!m_reportedMissing.Contains(p.className)) {
                m_reportedMissing.PushBack(p.className);
                Report(String::Format("C++ Script: the game module has no component '{}' (compile the game code?)", p.className), true);
            }
            continue;
        }
        Instance in;
        in.entity = p.entity;
        in.className = move(p.className);
        in.desc = cd;
        in.object = cd->create();
        in.object->entity = ToId(p.entity);
        in.object->host = m_host.Get();
        NativeScript tmp;
        tmp.properties = move(p.properties);
        WriteProps(*cd, in.object, ParseNativeScriptProperties(tmp));
        m_instances.PushBack(move(in));
    }
}

void GameModuleSystem::Tick(ecs::World& world, f32 dt, const GameTickContext& context)
{
    if (!m_running)
        Begin(world);
    m_world = &world;
    Host& host = *m_host;
    host.world = &world;
    host.context = context;
    host.dt = dt;
    host.elapsed += f64(dt);

    StartInstances(world);
    // OnStart may add entities with components; they start next frame. Index loops: callbacks never touch m_instances.
    for (usize i = 0; i < m_instances.Size(); ++i)
        if (!m_instances[i].started) {
            m_instances[i].started = true;
            m_instances[i].object->OnStart();
        }
    for (usize i = 0; i < m_instances.Size(); ++i)
        m_instances[i].object->OnUpdate(dt);

    for (ecs::Entity e : host.destroyQueue)
        if (world.IsAlive(e))
            scene::DestroyHierarchy(world, e);
    host.destroyQueue.Clear();
}

void GameModuleSystem::DispatchPhysicsEvents(Span<const PhysicsEvent> events)
{
    if (!m_running)
        return;
    for (const PhysicsEvent& ev : events)
        for (u32 side = 0; side < 2; ++side) {
            const ecs::Entity self = side == 0 ? ev.a : ev.b, other = side == 0 ? ev.b : ev.a;
            for (usize i = 0; i < m_instances.Size(); ++i) {
                Instance& in = m_instances[i];
                if (!in.started || in.entity != self || !in.object)
                    continue;
                switch (ev.type) {
                case physics::ContactEventType::TouchBegin: in.object->OnCollisionEnter(ToId(other), ev.speed); break;
                case physics::ContactEventType::TouchEnd: in.object->OnCollisionExit(ToId(other)); break;
                case physics::ContactEventType::SensorEnter: in.object->OnTriggerEnter(ToId(other)); break;
                case physics::ContactEventType::SensorExit: in.object->OnTriggerExit(ToId(other)); break;
                case physics::ContactEventType::TouchStay: in.object->OnCollisionStay(ToId(other)); break;
                case physics::ContactEventType::SensorStay: in.object->OnTriggerStay(ToId(other)); break;
                }
            }
        }
}

void GameModuleSystem::End()
{
    DestroyInstances(m_running);
    m_running = false;
    m_world = nullptr;
    if (m_host) {
        m_host->world = nullptr;
        m_host->context = GameTickContext{};
        m_host->destroyQueue.Clear();
    }
}

// --- Project source ------------------------------------------------------------------------------------------------------------

bool HasGameModuleSource(StringView projectRoot, StringView name)
{
    return platform::FileExists(Path::Join(projectRoot, String::Format("Source/{}/CMakeLists.txt", name)));
}

bool WriteGameModuleSource(StringView projectRoot, StringView name, StringView engineRoot)
{
    const String dir = Path::Join(projectRoot, String::Format("Source/{}", name));
    if (!platform::CreateDirectoryTree(dir))
        return false;
    String engine = engineRoot;
    engine.ReplaceChar('\\', '/');

    String cmake = R"cmake(# Game module: built by the editor (Compile, F7) with Tools/build_game.ps1 into Binaries/@NAME@.dll and
# hot-reloaded while the game plays. Every .cpp / .h in this folder is part of the module.
cmake_minimum_required(VERSION 3.20)
project(@NAME@ LANGUAGES CXX)

if(NOT ZX_ENGINE_ROOT)
    set(ZX_ENGINE_ROOT "@ENGINE@")
endif()
if(NOT ZX_GAME_OUTPUT)
    set(ZX_GAME_OUTPUT "${CMAKE_CURRENT_SOURCE_DIR}/../../Binaries")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")

file(GLOB_RECURSE GAME_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/*.h")
add_library(@NAME@ SHARED ${GAME_SOURCES})
target_include_directories(@NAME@ PRIVATE "${ZX_ENGINE_ROOT}/Engine/Source/GameSDK")
target_compile_options(@NAME@ PRIVATE /W4 /permissive- /utf-8 /MP)
set_target_properties(@NAME@ PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${ZX_GAME_OUTPUT}"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/pdb")
)cmake";
    cmake.ReplaceAll("@NAME@", name);
    cmake.ReplaceAll("@ENGINE@", engine);

    String source = R"cpp(// @NAME@ game code. Compile with the Compile button (F7): the editor rebuilds this DLL in the background and
// hot-reloads it, even while the game is playing. Add a component to an entity with Add Component > C++ Script.
//
// Game SDK reference: ZeqouGame.h (in the engine's Engine/Source/GameSDK folder).

#include "ZeqouGame.h"

using namespace zx::game;

// Spins the entity around an axis.
class Spinner : public ScriptComponent
{
public:
    float speed = 90.0f;      // degrees per second
    Vec3 axis = Vec3(0, 1, 0);

    void OnUpdate(float dt) override
    {
        SetRotation(Rotation() * Quat::AxisAngle(axis, speed * dt));
    }
};
ZX_GAME_COMPONENT(Spinner, "Spins the entity around an axis")
    ZX_GAME_PROP(speed, -720, 720, "Degrees per second")
    ZX_GAME_PROP(axis, 0, 0, "Rotation axis (local)")
ZX_GAME_COMPONENT_END()

// Moves the entity up and down.
class Bouncer : public ScriptComponent
{
public:
    float height = 0.5f;      // meters
    float frequency = 1.0f;   // bounces per second

    void OnStart() override { m_base = Position(); }
    void OnReloaded() override { m_base = Position(); }
    void OnUpdate(float dt) override
    {
        m_time += dt;
        const float y = height * std::sin(m_time * frequency * 6.2831853f);
        SetPosition(Vec3(m_base.x, m_base.y + y, m_base.z));
    }

private:
    Vec3 m_base;
    float m_time = 0.0f;
};
ZX_GAME_COMPONENT(Bouncer, "Moves the entity up and down")
    ZX_GAME_PROP(height, 0, 10, "Meters")
    ZX_GAME_PROP(frequency, 0, 10, "Bounces per second")
ZX_GAME_COMPONENT_END()

// Walks a Character Controller with the project's Input Actions (Window > Input Actions).
class PlayerController : public ScriptComponent
{
public:
    float walkSpeed = 4.0f;
    float sprintMultiplier = 1.8f;
    float jumpSpeed = 5.0f;
    float turnSpeed = 0.15f;  // degrees per pixel of mouse movement

    void OnUpdate(float) override
    {
        // Turn with the mouse / right stick.
        m_yaw += host->Axis("LookX") * turnSpeed;
        SetRotation(Quat::Euler(0, m_yaw, 0));

        Vec3 move = Forward() * host->Axis("MoveForward") + Right() * host->Axis("MoveRight");
        if (move.Length() > 1.0f)
            move = move.Normalized();
        float speed = walkSpeed;
        if (host->ActionDown("Sprint"))
            speed *= sprintMultiplier;
        host->CharacterMove(entity, move * speed);
        if (host->ActionPressed("Jump") && host->CharacterGrounded(entity))
            host->CharacterJump(entity, jumpSpeed);
    }

private:
    float m_yaw = 0.0f;
};
ZX_GAME_COMPONENT(PlayerController, "Walks a Character Controller with Input Actions")
    ZX_GAME_PROP(walkSpeed, 0, 20, "Meters per second")
    ZX_GAME_PROP(sprintMultiplier, 1, 5, "Speed multiplier while Sprint is held")
    ZX_GAME_PROP(jumpSpeed, 0, 20, "Jump speed (m/s)")
    ZX_GAME_PROP(turnSpeed, 0, 2, "Degrees per pixel of mouse movement")
ZX_GAME_COMPONENT_END()

ZX_IMPLEMENT_GAME_MODULE("@NAME@")
)cpp";
    source.ReplaceAll("@NAME@", name);

    const String cmakePath = Path::Join(dir, "CMakeLists.txt");
    const String sourcePath = Path::Join(dir, String::Format("{}.cpp", name));
    bool ok = fs::WriteTextFile(cmakePath, cmake);
    if (!platform::FileExists(sourcePath)) // never overwrite the user's code
        ok &= fs::WriteTextFile(sourcePath, source);

    // Build outputs stay out of version control.
    const String ignorePath = Path::Join(projectRoot, ".gitignore");
    String ignore;
    if (fs::ReadTextFile(ignorePath, ignore) && !ignore.Contains("Binaries/")) {
        if (!ignore.Empty() && ignore[ignore.Size() - 1] != '\n')
            ignore += "\n";
        ignore += "Binaries/\n";
        fs::WriteTextFile(ignorePath, ignore);
    }
    return ok;
}

} // namespace zx
