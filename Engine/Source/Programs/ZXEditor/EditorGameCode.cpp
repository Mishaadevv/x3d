// C++ game code in the editor: the project's game module (build in the background, hot reload, also while playing),
// the Input Actions window and the -cpptest self-test.

#include "Editor.h"
#include "Core/FileSystem/FileSystem.h"
#include "Core/HAL/PlatformFile.h"
#include "Core/String/Path.h"
#include "Engine/SceneRendering.h"

namespace zx::editor {

ZX_DEFINE_LOG_CATEGORY_STATIC(LogGameCode, Info);

namespace {

/// Loads Binaries/<Name>.dll when it changed since it was loaded (or nothing is loaded yet).
void LoadIfChanged(Editor& ed)
{
    platform::FileInfo info;
    if (!platform::GetFileInfo(ed.game->DllPath(), info))
        return;
    if (ed.game->IsLoaded() && info.modifiedTime == ed.gameDllTime)
        return;
    String error;
    if (ed.game->Reload(&error))
        ed.gameDllTime = info.modifiedTime;
    else
        ZX_LOG_ERROR(LogGameCode, "Hot reload failed: {}", error);
}

/// Compiler errors from the build log ("file(line): error C1234: ..."), so the Output Log shows what went wrong.
void ReportBuildErrors(StringView log)
{
    u32 shown = 0;
    usize start = 0;
    while (start < log.Size() && shown < 20) {
        usize end = log.Find('\n', start);
        if (end == StringView::NPos)
            end = log.Size();
        StringView line = log.SubStr(start, end - start).Trim();
        start = end + 1;
        if (line.Contains(": error") || line.Contains(": fatal error") || line.StartsWith("CMake Error") || line.StartsWith("error:")) {
            ZX_LOG_ERROR(LogGameCode, "{}", line);
            ++shown;
        }
    }
    if (!shown)
        ZX_LOG_ERROR(LogGameCode, "The build failed without compiler errors - see Saved/Logs/GameBuild.log");
}

} // namespace

String GameModuleName(const ProjectInfo& project)
{
    String name;
    for (char c : project.descriptor.name.View())
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')
            name += c;
    if (name.Empty() || (name[0] >= '0' && name[0] <= '9'))
        name = "Game" + name;
    return name;
}

void SetupGameModule(Editor& ed)
{
    if (!ed.game)
        return;
    ed.gameDllTime = 0;
    ed.gameBuildState = GameBuildState::Idle;
    if (ed.project.descriptorPath.Empty()) {
        ed.game->SetProject({}, {}, {});
        ed.inputActions = InputActionMap::Defaults();
        return;
    }
    ed.game->SetProject(ed.project.root, GameModuleName(ed.project), ed.engineRoot);
    ed.inputActions = LoadInputActions(ed.project.root);
    if (!HasGameModuleSource(ed.project.root, GameModuleName(ed.project)))
        return;
    // Use the DLL that is there right away, and bring it up to date in the background (a no-op build is quick).
    LoadIfChanged(ed);
    CompileGameCode(ed);
}

void UpdateGameModule(Editor& ed)
{
    if (!ed.game)
        return;
    const GameBuildState before = ed.gameBuildState;
    ed.gameBuildState = ed.game->PollBuild();
    if (before != GameBuildState::Building || ed.gameBuildState == GameBuildState::Building)
        return;
    if (ed.gameBuildState == GameBuildState::Succeeded) {
        ZX_LOG_INFO(LogGameCode, "Game code compiled ({:.1f} s)", ed.game->LastBuildSeconds());
        LoadIfChanged(ed);
    } else {
        ReportBuildErrors(ed.game->BuildLog());
    }
}

bool CompileGameCode(Editor& ed)
{
    if (!ed.game || ed.project.descriptorPath.Empty())
        return false;
    if (!HasGameModuleSource(ed.project.root, GameModuleName(ed.project))) {
        ZX_LOG_WARN(LogGameCode, "This project has no C++ game module yet (Tools > Create C++ Game Module)");
        return false;
    }
    if (ed.gameBuildState == GameBuildState::Building)
        return false;
    if (!ed.game->StartBuild()) {
        ZX_LOG_ERROR(LogGameCode, "Can't start the game build");
        return false;
    }
    ed.gameBuildState = GameBuildState::Building;
    ZX_LOG_INFO(LogGameCode, "Compiling game code ({})...", GameModuleName(ed.project));
    return true;
}

bool CreateGameModule(Editor& ed)
{
    if (ed.project.descriptorPath.Empty())
        return false;
    const String name = GameModuleName(ed.project);
    const bool existed = HasGameModuleSource(ed.project.root, name);
    if (!WriteGameModuleSource(ed.project.root, name, ed.engineRoot)) {
        ZX_LOG_ERROR(LogGameCode, "Can't write Source/{}", name);
        return false;
    }
    if (!existed)
        ZX_LOG_INFO(LogGameCode, "Created Source/{}/{}.cpp (Spinner, Bouncer, PlayerController) - edit it in your IDE, then Compile (F7)",
                    name, name);
    ed.panels.browserListedDir.Clear();
    return CompileGameCode(ed);
}

// --- Input Actions window ------------------------------------------------------------------------------------------------------

void DrawInputActionsPanel(Editor& ed)
{
    if (!ed.showInputActions)
        return;
    ui::SetNextWindowSize(Vec2(460.0f, 520.0f), ui::Cond::FirstUse);
    if (!ui::Begin(InputActionsTitle, &ed.showInputActions)) {
        ui::End();
        return;
    }
    InputActionMap& map = ed.inputActions;
    bool changed = false;
    const Span<const StringView> controls = InputControlNames();
    static const StringView Kinds[] = {"Button", "Axis"};

    ui::TextDisabled("Named inputs for gameplay code: input.action(\"Jump\") / host->ActionDown(\"Jump\").");
    if (ui::Button(ZX_ICON_PLUS "  Add Action")) {
        InputAction a;
        a.name = "NewAction";
        for (u32 n = 2; map.Find(a.name); ++n)
            a.name = String::Format("NewAction{}", n);
        a.bindings.PushBack(InputBinding{String("Space"), 1.0f});
        map.actions.PushBack(move(a));
        changed = true;
    }
    ui::SameLine(0.0f, 6.0f);
    if (ui::Button(ZX_ICON_ROTATE_CCW "  Reset to Defaults")) {
        map = InputActionMap::Defaults();
        changed = true;
    }
    ui::Separator();

    i32 removeAction = -1;
    for (usize i = 0; i < map.actions.Size(); ++i) {
        InputAction& a = map.actions[i];
        ui::PushID(i32(i));
        ui::SetNextItemWidth(-110.0f);
        changed |= ui::InputText("##name", a.name);
        ui::SameLine(0.0f, 4.0f);
        ui::SetNextItemWidth(70.0f);
        i32 kind = a.axis ? 1 : 0;
        if (ui::Combo("##kind", &kind, Span<const StringView>(Kinds, 2))) {
            a.axis = kind == 1;
            changed = true;
        }
        ui::SameLine(0.0f, 4.0f);
        if (ui::IconButton(ZX_ICON_TRASH_2, "Remove this action"))
            removeAction = i32(i);

        i32 removeBinding = -1;
        for (usize b = 0; b < a.bindings.Size(); ++b) {
            InputBinding& bind = a.bindings[b];
            ui::PushID(i32(b));
            ui::Indent(16.0f);
            i32 current = -1;
            for (usize c = 0; c < controls.Size(); ++c)
                if (controls[c].EqualsIgnoreCase(bind.control))
                    current = i32(c);
            ui::SetNextItemWidth(a.axis ? -110.0f : -30.0f);
            if (ui::Combo("##control", &current, controls) && current >= 0) {
                bind.control = controls[usize(current)];
                changed = true;
            }
            if (a.axis) {
                ui::SameLine(0.0f, 4.0f);
                ui::SetNextItemWidth(72.0f);
                changed |= ui::DragFloat("##scale", &bind.scale, 0.05f, -100.0f, 100.0f, "x {:.2f}");
                ui::ItemTooltip("Scale: how much this control adds to the axis (-1 for the opposite direction)");
            }
            ui::SameLine(0.0f, 4.0f);
            if (ui::IconButton(ZX_ICON_X, "Remove this binding"))
                removeBinding = i32(b);
            ui::Indent(-16.0f);
            ui::PopID();
        }
        if (removeBinding >= 0) {
            a.bindings.RemoveAt(usize(removeBinding));
            changed = true;
        }
        ui::Indent(16.0f);
        if (ui::Button(ZX_ICON_PLUS "  Binding")) {
            a.bindings.PushBack(InputBinding{String(a.axis ? "W" : "Space"), 1.0f});
            changed = true;
        }
        ui::Indent(-16.0f);
        ui::Separator();
        ui::PopID();
    }
    if (removeAction >= 0) {
        map.actions.RemoveAt(usize(removeAction));
        changed = true;
    }
    if (changed && !ed.project.descriptorPath.Empty() && !SaveInputActions(ed.project.root, map))
        ZX_LOG_ERROR(LogGameCode, "Can't save Config/Input.json");
    ui::End();
}

// --- Self-test ---------------------------------------------------------------------------------------------------------------

void Editor::RunCppSelfTest()
{
    auto fail = [this](StringView why) {
        ZX_LOG_ERROR(LogGameCode, "C++ self-test FAILED: {}", why);
        if (IsPlaying())
            EndPlay();
        selfTestFailed = true;
        m_cppTestStage = 0;
    };
    const String name = GameModuleName(project);
    if (m_cppTestStage > 1 && Clock::NowSeconds() - m_cppTestStart > 300.0)
        return fail(String::Format("timed out in stage {}", m_cppTestStage));
    auto findCube = [this] {
        ecs::Entity found;
        world.Query<const scene::EntityName>().Each([&](ecs::Entity e, const scene::EntityName& n) {
            if (n.value == "CppTestCube")
                found = e;
        });
        return found;
    };

    switch (m_cppTestStage) {
    case 1: // project with a game module, first build running
        if (project.descriptorPath.Empty() || !HasGameModuleSource(project.root, name))
            return fail("no project with a C++ game module (use -newproject=... -language=cpp -cpptest)");
        if (gameBuildState != GameBuildState::Building && !game->IsLoaded())
            CompileGameCode(*this);
        m_cppTestStart = Clock::NowSeconds();
        m_cppTestStage = 2;
        return;
    case 2: { // build done -> place a cube running Spinner with speed 45, play
        if (gameBuildState == GameBuildState::Building)
            return;
        if (gameBuildState == GameBuildState::Failed)
            return fail(String::Format("the game module didn't build:\n{}", game->BuildLog()));
        if (!game->IsLoaded() || !game->FindComponent("Spinner"))
            return fail("the game module didn't load or has no Spinner");
        ZX_LOG_INFO(LogGameCode, "C++ self-test: first build {:.1f} s", game->LastBuildSeconds());
        const ecs::Entity cube = CreateEntity("CppTestCube", Transform(Vec3(0.0f, 1.0f, 0.0f)));
        MeshRenderer mr;
        mr.mesh = builtin::Mesh(BuiltinShape::Cube);
        world.Add(cube, mr);
        NativeScript ns{String("Spinner"), String()};
        SetNativeScriptProperty(ns, *game->FindComponent("Spinner"), "speed", JsonValue(45.0f));
        world.Add(cube, ns);
        BeginPlay();
        m_cppTestFrame = frameIndex;
        m_cppTestStage = 3;
        return;
    }
    case 3: { // spinning -> change the code (also lift the entity) and rebuild while playing
        if (frameIndex < m_cppTestFrame + 30)
            return;
        const ecs::Entity cube = findCube();
        const Transform* t = cube ? world.Get<Transform>(cube) : nullptr;
        if (!t || game->RunningComponents() != 1)
            return fail("Spinner is not running in Play-in-Editor");
        if (Abs(t->rotation.w) > 0.99999f)
            return fail("Spinner did not rotate the cube");
        m_cppTestRotation = t->rotation;
        const String source = Path::Join(project.root, String::Format("Source/{}/{}.cpp", name, name));
        String code;
        const StringView line = "        SetRotation(Rotation() * Quat::AxisAngle(axis, speed * dt));\n";
        if (!fs::ReadTextFile(source, code) || !code.Contains(line))
            return fail("can't find Spinner::OnUpdate in the generated source");
        code.ReplaceAll(line, String::Format("{}        SetPosition(Position() + Vec3(0, 0.5f * dt, 0)); // hot reload test\n", line));
        if (!fs::WriteTextFile(source, code))
            return fail("can't write the game source");
        m_cppTestGeneration = game->Generation();
        if (!CompileGameCode(*this))
            return fail("can't start the second build");
        m_cppTestStage = 4;
        return;
    }
    case 4: { // rebuilt and hot-reloaded while the game kept running
        if (gameBuildState == GameBuildState::Building)
            return;
        if (gameBuildState == GameBuildState::Failed)
            return fail(String::Format("the changed game module didn't build:\n{}", game->BuildLog()));
        if (game->Generation() == m_cppTestGeneration)
            return fail("the rebuilt module was not hot-reloaded");
        if (!IsPlaying())
            return fail("Play-in-Editor stopped during the hot reload");
        ZX_LOG_INFO(LogGameCode, "C++ self-test: rebuild + hot reload {:.1f} s", game->LastBuildSeconds());
        const ecs::Entity cube = findCube();
        m_cppTestY = cube ? world.Get<Transform>(cube)->position.y : 0.0f;
        m_cppTestFrame = frameIndex;
        m_cppTestStage = 5;
        return;
    }
    case 5: { // new behavior, same props; Stop restores the scene
        if (frameIndex < m_cppTestFrame + 30)
            return;
        const ecs::Entity cube = findCube();
        const Transform* t = cube ? world.Get<Transform>(cube) : nullptr;
        if (!t || game->RunningComponents() != 1)
            return fail("Spinner stopped running after the hot reload");
        if (t->position.y < m_cppTestY + 0.01f)
            return fail(String::Format("the new code doesn't run (y {} -> {})", m_cppTestY, t->position.y));
        if (Abs(Dot(t->rotation, m_cppTestRotation)) > 0.999999f)
            return fail("the cube stopped spinning after the hot reload");
        const JsonValue props = game->RunningProps(cube);
        if (Abs(props.Get("speed").AsFloat() - 45.0f) > 1e-4f)
            return fail(String::Format("the speed prop was not kept ({})", props.Get("speed").AsFloat()));
        EndPlay();
        const ecs::Entity restored = findCube();
        if (!restored || Abs(world.Get<Transform>(restored)->position.y - 1.0f) > 1e-4f || game->IsRunning())
            return fail("Stop did not restore the scene");
        ZX_LOG_INFO(LogGameCode, "C++ self-test passed (generation {})", game->Generation());
        m_cppTestStage = 0;
        return;
    }
    default: m_cppTestStage = 0; return;
    }
}

} // namespace zx::editor
