#pragma once

// C++ gameplay: a project's game module (Source/<Name>, built to Binaries/<Name>.dll with the Game SDK, ZeqouGame.h)
// runs its ScriptComponents on entities with a NativeScript component. The editor builds the DLL in the background
// and hot-reloads it - also while playing: props are carried over by name, components get OnReloaded().
//
//   GameModuleSystem game;
//   game.SetProject(root, "MyGame");
//   game.StartBuild();  ...  if (game.PollBuild() == BuildState::Succeeded) game.Reload();
//   game.Begin(world); game.Tick(world, dt, ctx); game.End();

#include "Core/Containers/Function.h"
#include "Core/Memory/Pointers.h"
#include "Core/Reflection/Reflection.h"
#include "Core/Serialization/Json.h"
#include "Core/Threading/Thread.h"
#include "Engine/InputActions.h"
#include "Scene/ECS/World.h"
#include "ZeqouGame.h"

#include <atomic>

namespace zx {

class InputState;
class AudioSystem;

/// Runs the game module's C++ component `className` on the entity. `properties` holds prop values that differ from the
/// component's defaults (JSON object, like Script::properties).
struct NativeScript
{
    String className;
    String properties;
};
ZX_DECLARE_TYPE(NativeScript);

enum class GameBuildState : u8
{
    Idle,
    Building,
    Succeeded,
    Failed,
};

struct GameTickContext
{
    const InputState* input = nullptr;
    const InputActionState* actions = nullptr;
    AudioSystem* audio = nullptr;
};

/// The prop overrides stored on the component (JSON object, empty when none).
JsonValue ParseNativeScriptProperties(const NativeScript& script);
/// Stores a prop value (removed again when it equals the component's default).
void SetNativeScriptProperty(NativeScript& script, const game::ComponentDesc& desc, StringView prop, const JsonValue& value);

/// Writes Source/<Name>/CMakeLists.txt and <Name>.cpp (sample components) for a new or existing project.
bool WriteGameModuleSource(StringView projectRoot, StringView name, StringView engineRoot);
/// True when the project has a game module to build (Source/<Name>/CMakeLists.txt).
bool HasGameModuleSource(StringView projectRoot, StringView name);

class GameModuleSystem
{
public:
    GameModuleSystem();
    ~GameModuleSystem();
    ZX_NON_COPYABLE(GameModuleSystem)

    void SetProject(StringView projectRoot, StringView name, StringView engineRoot);
    StringView ProjectRoot() const { return m_root.View(); }
    String DllPath() const;

    /// Builds the DLL in the background (Tools/build_game.ps1: CMake + Ninja with MSVC).
    bool StartBuild();
    /// Blocking build (tests).
    bool BuildNow();
    GameBuildState PollBuild();
    StringView BuildLog() const { return m_buildLog.View(); }
    f64 LastBuildSeconds() const { return m_buildSeconds; }

    /// Loads the newest DLL; running components are recreated on the new code with their props.
    bool Reload(String* error = nullptr);
    /// Loads an in-process module description instead of a DLL (tests).
    bool LoadFromDesc(const game::ModuleDesc* desc);
    void Unload();
    bool IsLoaded() const { return m_desc != nullptr; }
    StringView ModuleName() const { return m_desc ? StringView(m_desc->name) : StringView(); }
    u32 Generation() const { return m_generation; }
    Array<const game::ComponentDesc*> Components() const;
    const game::ComponentDesc* FindComponent(StringView className) const;

    void Begin(ecs::World& world);
    void Tick(ecs::World& world, f32 dt, const GameTickContext& context);
    void End();
    bool IsRunning() const { return m_running; }
    u32 RunningComponents() const { return u32(m_instances.Size()); }
    /// Current prop values of the component running on the entity (null when none runs there).
    JsonValue RunningProps(ecs::Entity entity) const;

    /// Log output of components (Log, errors).
    Function<void(StringView message, bool isError)> onLog;

    // Props <-> JSON (Details panel, scene files, hot reload).
    static JsonValue ReadProps(const game::ComponentDesc& desc, const game::ScriptComponent* object);
    static void WriteProps(const game::ComponentDesc& desc, game::ScriptComponent* object, const JsonValue& values);
    /// Default values of a component's props (creates a temporary object).
    static JsonValue DefaultProps(const game::ComponentDesc& desc);

private:
    struct Instance
    {
        ecs::Entity entity;
        String className;
        const game::ComponentDesc* desc = nullptr;
        game::ScriptComponent* object = nullptr;
        bool started = false;
    };
    class Host;
    void DestroyInstances(bool callStop);
    void StartInstances(ecs::World& world);
    bool SwapModule(const game::ModuleDesc* desc, void* library, StringView copyPath, String* error);
    void Report(StringView message, bool isError);

    String m_root, m_name, m_engineRoot;
    void* m_library = nullptr; // HMODULE
    const game::ModuleDesc* m_desc = nullptr;
    String m_loadedCopy;
    u32 m_generation = 0;
    u32 m_copyCounter = 0;
    UniquePtr<Host> m_host;
    Array<Instance> m_instances;
    bool m_running = false;
    ecs::World* m_world = nullptr;
    Array<String> m_reportedMissing; // class names already reported as missing this session
    // Build.
    Thread m_buildThread;
    std::atomic<u32> m_buildState{u32(GameBuildState::Idle)};
    std::atomic<i32> m_buildExitCode{0};
    std::atomic<bool> m_buildFinished{false};
    String m_buildLog;
    f64 m_buildStart = 0.0;
    f64 m_buildSeconds = 0.0;
};

} // namespace zx
