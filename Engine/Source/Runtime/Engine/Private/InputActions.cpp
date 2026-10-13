#include "Engine/InputActions.h"
#include "Core/FileSystem/FileSystem.h"
#include "Core/Memory/Memory.h"
#include "Core/HAL/PlatformFile.h"
#include "Core/String/Path.h"
#include "Platform/Input.h"

namespace zx {

namespace {

struct ControlInfo
{
    StringView name;
    enum Kind : u8
    {
        KeyControl,
        MouseButtonControl,
        MouseAxis,
        PadButton,
        PadAxis,
    } kind;
    u32 index;
};

const ControlInfo SpecialControls[] = {
    {"Shift", ControlInfo::KeyControl, u32(Key::LeftShift)},
    {"Ctrl", ControlInfo::KeyControl, u32(Key::LeftControl)},
    {"Alt", ControlInfo::KeyControl, u32(Key::LeftAlt)},
    {"Mouse Left", ControlInfo::MouseButtonControl, u32(MouseButton::Left)},
    {"Mouse Right", ControlInfo::MouseButtonControl, u32(MouseButton::Right)},
    {"Mouse Middle", ControlInfo::MouseButtonControl, u32(MouseButton::Middle)},
    {"Mouse X", ControlInfo::MouseAxis, 0},
    {"Mouse Y", ControlInfo::MouseAxis, 1},
    {"Gamepad A", ControlInfo::PadButton, u32(GamepadButton::A)},
    {"Gamepad B", ControlInfo::PadButton, u32(GamepadButton::B)},
    {"Gamepad X", ControlInfo::PadButton, u32(GamepadButton::X)},
    {"Gamepad Y", ControlInfo::PadButton, u32(GamepadButton::Y)},
    {"Gamepad LB", ControlInfo::PadButton, u32(GamepadButton::LeftShoulder)},
    {"Gamepad RB", ControlInfo::PadButton, u32(GamepadButton::RightShoulder)},
    {"Gamepad Back", ControlInfo::PadButton, u32(GamepadButton::Back)},
    {"Gamepad Start", ControlInfo::PadButton, u32(GamepadButton::Start)},
    {"Gamepad LS", ControlInfo::PadButton, u32(GamepadButton::LeftThumb)},
    {"Gamepad RS", ControlInfo::PadButton, u32(GamepadButton::RightThumb)},
    {"Gamepad Up", ControlInfo::PadButton, u32(GamepadButton::DPadUp)},
    {"Gamepad Down", ControlInfo::PadButton, u32(GamepadButton::DPadDown)},
    {"Gamepad Left", ControlInfo::PadButton, u32(GamepadButton::DPadLeft)},
    {"Gamepad Right", ControlInfo::PadButton, u32(GamepadButton::DPadRight)},
    {"Gamepad LeftX", ControlInfo::PadAxis, 0},
    {"Gamepad LeftY", ControlInfo::PadAxis, 1},
    {"Gamepad RightX", ControlInfo::PadAxis, 2},
    {"Gamepad RightY", ControlInfo::PadAxis, 3},
    {"Gamepad LT", ControlInfo::PadAxis, 4},
    {"Gamepad RT", ControlInfo::PadAxis, 5},
};

bool ResolveControl(StringView name, ControlInfo& out)
{
    for (const ControlInfo& c : SpecialControls)
        if (c.name.EqualsIgnoreCase(name)) {
            out = c;
            return true;
        }
    for (u32 k = 1; k < u32(Key::Count); ++k)
        if (name.EqualsIgnoreCase(KeyName(Key(k)))) {
            out = ControlInfo{name, ControlInfo::KeyControl, k};
            return true;
        }
    return false;
}

/// Value of one control: buttons 0/1, axes signed.
f32 ReadControl(const ControlInfo& c, const InputState& in)
{
    switch (c.kind) {
    case ControlInfo::KeyControl: {
        const Key k = Key(c.index);
        bool down = in.IsKeyDown(k);
        if (k == Key::LeftShift)
            down |= in.IsKeyDown(Key::RightShift);
        else if (k == Key::LeftControl)
            down |= in.IsKeyDown(Key::RightControl);
        else if (k == Key::LeftAlt)
            down |= in.IsKeyDown(Key::RightAlt);
        return down ? 1.0f : 0.0f;
    }
    case ControlInfo::MouseButtonControl: return in.IsMouseDown(MouseButton(c.index)) ? 1.0f : 0.0f;
    case ControlInfo::MouseAxis: return c.index == 0 ? in.MouseDelta().x : in.MouseDelta().y;
    case ControlInfo::PadButton: return in.Gamepad(0).connected && in.Gamepad(0).buttons[c.index] ? 1.0f : 0.0f;
    case ControlInfo::PadAxis: {
        const GamepadState& p = in.Gamepad(0);
        if (!p.connected)
            return 0.0f;
        switch (c.index) {
        case 0: return p.leftStick.x;
        case 1: return p.leftStick.y;
        case 2: return p.rightStick.x;
        case 3: return p.rightStick.y;
        case 4: return p.leftTrigger;
        default: return p.rightTrigger;
        }
    }
    }
    return 0.0f;
}

} // namespace

const InputAction* InputActionMap::Find(StringView name) const
{
    for (const InputAction& a : actions)
        if (a.name.View().EqualsIgnoreCase(name))
            return &a;
    return nullptr;
}

JsonValue InputActionMap::ToJson() const
{
    JsonValue root = JsonValue::MakeObject();
    JsonValue list = JsonValue::MakeArray();
    for (const InputAction& a : actions) {
        JsonValue j = JsonValue::MakeObject();
        j.Set("name", a.name);
        j.Set("type", a.axis ? "axis" : "button");
        JsonValue b = JsonValue::MakeArray();
        for (const InputBinding& bind : a.bindings) {
            JsonValue jb = JsonValue::MakeObject();
            jb.Set("control", bind.control);
            if (a.axis)
                jb.Set("scale", bind.scale);
            b.PushBack(move(jb));
        }
        j.Set("bindings", move(b));
        list.PushBack(move(j));
    }
    root.Set("actions", move(list));
    return root;
}

bool InputActionMap::FromJson(const JsonValue& json)
{
    const JsonValue* list = json.Find("actions");
    if (!list || !list->IsArray())
        return false;
    actions.Clear();
    for (const JsonValue& j : list->Elements()) {
        InputAction a;
        a.name = j.Get("name").AsString();
        a.axis = j.Get("type").AsString() == "axis";
        for (const JsonValue& jb : j.Get("bindings").Elements())
            a.bindings.PushBack(InputBinding{String(jb.Get("control").AsString()), jb.Get("scale").AsFloat(1.0f)});
        if (!a.name.Empty())
            actions.PushBack(move(a));
    }
    return true;
}

InputActionMap InputActionMap::Defaults()
{
    InputActionMap m;
    auto button = [&](StringView name, std::initializer_list<StringView> controls) {
        InputAction a;
        a.name = name;
        for (StringView c : controls)
            a.bindings.PushBack(InputBinding{String(c), 1.0f});
        m.actions.PushBack(move(a));
    };
    auto axis = [&](StringView name, std::initializer_list<Pair<StringView, f32>> controls) {
        InputAction a;
        a.name = name;
        a.axis = true;
        for (const Pair<StringView, f32>& c : controls)
            a.bindings.PushBack(InputBinding{String(c.first), c.second});
        m.actions.PushBack(move(a));
    };
    button("Jump", {"Space", "Gamepad A"});
    button("Sprint", {"Shift", "Gamepad LS"});
    button("Crouch", {"C", "Gamepad B"});
    button("Interact", {"E", "Gamepad X"});
    button("Fire", {"Mouse Left", "Gamepad RB"});
    button("Aim", {"Mouse Right", "Gamepad LB"});
    button("Pause", {"Escape", "Gamepad Start"});
    axis("MoveForward", {{"W", 1.0f}, {"S", -1.0f}, {"Up", 1.0f}, {"Down", -1.0f}, {"Gamepad LeftY", 1.0f}});
    axis("MoveRight", {{"D", 1.0f}, {"A", -1.0f}, {"Right", 1.0f}, {"Left", -1.0f}, {"Gamepad LeftX", 1.0f}});
    axis("LookX", {{"Mouse X", 1.0f}, {"Gamepad RightX", 12.0f}});
    axis("LookY", {{"Mouse Y", 1.0f}, {"Gamepad RightY", -12.0f}});
    return m;
}

InputActionMap LoadInputActions(StringView projectRoot)
{
    String text;
    JsonValue json;
    InputActionMap map;
    if (!projectRoot.Empty() && fs::ReadTextFile(Path::Join(projectRoot, "Config/Input.json"), text) && ParseJson(text, json) && map.FromJson(json))
        return map;
    return InputActionMap::Defaults();
}

bool SaveInputActions(StringView projectRoot, const InputActionMap& map)
{
    platform::CreateDirectoryTree(Path::Join(projectRoot, "Config"));
    return fs::WriteTextFile(Path::Join(projectRoot, "Config/Input.json"), ToJsonString(map.ToJson()));
}

Span<const StringView> InputControlNames()
{
    static Array<StringView>* names = [] {
        LeakIgnoreScope permanent;
        Array<StringView>* n = New<Array<StringView>>();
        for (u32 k = 1; k < u32(Key::Count); ++k)
            n->PushBack(KeyName(Key(k)));
        for (const ControlInfo& c : SpecialControls)
            n->PushBack(c.name);
        return n;
    }();
    return names->View();
}

void InputActionState::Update(const InputActionMap& map, const InputState* input)
{
    for (auto& [name, v] : m_values) {
        v.wasDown = v.down;
        v.down = false;
        v.axis = 0.0f;
    }
    for (const InputAction& a : map.actions) {
        Value* v = m_values.Find(a.name);
        if (!v)
            v = &m_values.InsertOrAssign(a.name, Value{});
        if (!input)
            continue;
        f32 axis = 0.0f;
        bool down = false, unbounded = false; // mouse movement is not limited to [-1, 1]
        for (const InputBinding& b : a.bindings) {
            ControlInfo c;
            if (!ResolveControl(b.control, c))
                continue;
            const f32 value = ReadControl(c, *input);
            unbounded |= c.kind == ControlInfo::MouseAxis;
            if (a.axis)
                axis += value * b.scale;
            else
                down |= value > 0.5f;
        }
        v->axis = a.axis ? (unbounded ? axis : Clamp(axis, -1.0f, 1.0f)) : (down ? 1.0f : 0.0f);
        v->down = a.axis ? Abs(v->axis) > 0.5f : down;
    }
}

bool InputActionState::Down(StringView action) const
{
    const Value* v = m_values.Find(action);
    return v && v->down;
}

bool InputActionState::Pressed(StringView action) const
{
    const Value* v = m_values.Find(action);
    return v && v->down && !v->wasDown;
}

bool InputActionState::Released(StringView action) const
{
    const Value* v = m_values.Find(action);
    return v && !v->down && v->wasDown;
}

f32 InputActionState::Axis(StringView action) const
{
    const Value* v = m_values.Find(action);
    return v ? v->axis : 0.0f;
}

} // namespace zx
