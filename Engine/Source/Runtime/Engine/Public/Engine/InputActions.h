#pragma once

// Input Actions: named buttons ("Jump") and axes ("MoveForward") bound to keys, mouse and gamepad, so gameplay code
// never hard-codes a key. Stored per project in Config/Input.json; edited in the editor (Window > Input Actions).
//
//   InputActionMap map = InputActionMap::Defaults();          // or LoadInputActions(projectRoot)
//   InputActionState actions;
//   actions.Update(map, &input);                               // once per frame
//   if (actions.Pressed("Jump")) ...;  f32 f = actions.Axis("MoveForward");
//
// Binding names: key names as in Platform/Input.h ("W", "Space", "LeftShift", "Up"; "Shift"/"Ctrl"/"Alt" match either
// side), "Mouse Left|Right|Middle", "Mouse X|Y" (movement, axes), "Gamepad A|B|X|Y|LB|RB|Back|Start|LS|RS|Up|Down|Left|Right",
// "Gamepad LeftX|LeftY|RightX|RightY|LT|RT" (axes).

#include "Core/Containers/Array.h"
#include "Core/Containers/HashMap.h"
#include "Core/Serialization/Json.h"
#include "Core/String/String.h"

namespace zx {

class InputState;

struct InputBinding
{
    String control;     // see above
    f32 scale = 1.0f;   // axes: contribution (e.g. -1 for "S" on MoveForward)
};

struct InputAction
{
    String name;
    bool axis = false;  // false: button (down / pressed / released); true: value in [-1, 1]
    Array<InputBinding> bindings;
};

struct InputActionMap
{
    Array<InputAction> actions;

    const InputAction* Find(StringView name) const;
    JsonValue ToJson() const;
    bool FromJson(const JsonValue& json);
    /// Jump, Sprint, Crouch, Interact, Fire, Aim, Pause; MoveForward, MoveRight, LookX, LookY.
    static InputActionMap Defaults();
};

/// <root>/Config/Input.json (defaults when missing).
InputActionMap LoadInputActions(StringView projectRoot);
bool SaveInputActions(StringView projectRoot, const InputActionMap& map);

/// Every control name a binding can use (for pickers).
Span<const StringView> InputControlNames();

class InputActionState
{
public:
    /// Evaluates every action from the current input (null input = everything released).
    void Update(const InputActionMap& map, const InputState* input);
    bool Down(StringView action) const;
    bool Pressed(StringView action) const;
    bool Released(StringView action) const;
    f32 Axis(StringView action) const;

private:
    struct Value
    {
        bool down = false, wasDown = false;
        f32 axis = 0.0f;
    };
    HashMap<String, Value> m_values;
};

} // namespace zx
