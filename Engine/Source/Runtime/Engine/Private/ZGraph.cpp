#include "Engine/ZGraph.h"
#include "Core/Containers/Algorithm.h"
#include "Core/Serialization/Json.h"
#include "Core/String/Parse.h"

namespace zx {

namespace {

using T = GraphPinType;
using K = GraphNodeKind;

// Pin helpers: inputs with a default literal, outputs with an expression.
constexpr GraphPinDef In(const char* name, T type, const char* def = "") { return GraphPinDef{name, type, def, ""}; }
constexpr GraphPinDef Out(const char* name, T type, const char* expr = "") { return GraphPinDef{name, type, "", expr}; }
constexpr GraphPinDef ExecIn() { return GraphPinDef{"in", T::Exec, "", ""}; }
constexpr GraphPinDef ExecOut(const char* name = "then") { return GraphPinDef{name, T::Exec, "", ""}; }
constexpr GraphSettingDef Setting(const char* name, const char* def) { return GraphSettingDef{name, def}; }

const GraphNodeDef Library[] = {
    // --- Events ------------------------------------------------------------------------------------------------------------
    {"event.start", "On Start", "Events", K::Event, {}, {ExecOut()}, {}, "fn start()", "When the game starts (or the entity appears)"},
    {"event.update", "On Update", "Events", K::Event, {}, {ExecOut(), Out("dt", T::Number, "dt")}, {}, "fn update(dt)",
     "Every frame; dt = seconds since the last frame"},
    {"event.keyPressed", "On Key Pressed", "Events", K::Event, {}, {ExecOut()}, {Setting("key", "Space")}, "@update:input.pressed(\"{@key}\")",
     "The frame a key is pressed (Space, E, Enter, F1, GamepadA ...)"},
    {"event.keyReleased", "On Key Released", "Events", K::Event, {}, {ExecOut()}, {Setting("key", "Space")},
     "@update:input.released(\"{@key}\")", "The frame a key is released"},
    {"event.every", "Every N Seconds", "Events", K::Event, {}, {ExecOut()}, {Setting("seconds", "1")}, "@every", "Repeats while playing"},
    {"event.uiClick", "On UI Click", "Events", K::Event, {}, {ExecOut(), Out("name", T::String, "name")}, {Setting("widget", "")},
     "fn onUIClick(name)", "A UI Button was clicked (Widget: only that one; empty = any)"},
    {"event.uiChanged", "On UI Changed", "Events", K::Event, {}, {ExecOut(), Out("name", T::String, "name"), Out("value", T::Number, "value")},
     {Setting("widget", "")}, "fn onUIChanged(name, value)", "A UI Slider / Toggle / Input Field changed"},
    {"event.triggerEnter", "On Trigger Enter", "Events", K::Event, {}, {ExecOut(), Out("other", T::Entity, "other")}, {},
     "fn triggerEnter(other)", "Something entered this entity's trigger collider"},
    {"event.triggerExit", "On Trigger Exit", "Events", K::Event, {}, {ExecOut(), Out("other", T::Entity, "other")}, {},
     "fn triggerExit(other)", "Something left this entity's trigger collider"},
    {"event.collisionEnter", "On Collision", "Events", K::Event, {}, {ExecOut(), Out("other", T::Entity, "other"), Out("speed", T::Number, "speed")},
     {}, "fn collisionEnter(other, speed)", "This entity's collider hit something (speed in m/s)"},
    {"event.custom", "Custom Event", "Events", K::Event, {}, {ExecOut()}, {Setting("event", "MyEvent")}, "fn {@event}()",
     "Runs when a Call Event node calls it by name"},

    // --- Flow --------------------------------------------------------------------------------------------------------------
    {"flow.branch", "Branch", "Flow", K::Flow, {ExecIn(), In("condition", T::Bool, "true")}, {ExecOut("true"), ExecOut("false")}, {}, "",
     "If the condition is true, the True wire runs, otherwise False"},
    {"flow.sequence", "Sequence", "Flow", K::Flow, {ExecIn()}, {ExecOut("then0"), ExecOut("then1"), ExecOut("then2")}, {}, "",
     "Runs its outputs one after another"},
    {"flow.forLoop", "For Loop", "Flow", K::Flow, {ExecIn(), In("first", T::Number, "0"), In("last", T::Number, "9")},
     {ExecOut("body"), Out("index", T::Number, "_i{#id}"), ExecOut("completed")}, {}, "", "Runs Body for every index from First to Last"},
    {"flow.doOnce", "Do Once", "Flow", K::Flow, {ExecIn()}, {ExecOut()}, {}, "", "Lets the wire through only the first time"},
    {"flow.flipFlop", "Flip Flop", "Flow", K::Flow, {ExecIn()}, {ExecOut("a"), ExecOut("b"), Out("isA", T::Bool, "_ff{#id}")}, {}, "",
     "Alternates: A, B, A, B ..."},
    {"flow.delay", "Delay", "Flow", K::Flow, {ExecIn(), In("seconds", T::Number, "1")}, {ExecOut("completed")}, {}, "",
     "Waits, then continues (event values like Other aren't available after it)"},

    // --- Actions -----------------------------------------------------------------------------------------------------------
    {"action.print", "Print", "Debug", K::Action, {ExecIn(), In("text", T::String, "Hello")}, {ExecOut()}, {}, "log({text})", "Writes to the Output Log"},
    {"action.setVariable", "Set Variable", "Variables", K::Action, {ExecIn(), In("value", T::Any, "0")}, {ExecOut()}, {Setting("variable", "")},
     "{@variable} = {value}", "Stores a value in a graph variable"},
    {"action.callEvent", "Call Event", "Events", K::Action, {ExecIn()}, {ExecOut()}, {Setting("event", "MyEvent")}, "{@event}()",
     "Runs a Custom Event of this graph"},
    {"action.setPosition", "Set Position", "Transform", K::Action, {ExecIn(), In("target", T::Entity, "self"), In("position", T::Vec3, "0 0 0")},
     {ExecOut()}, {}, "{target}.position = {position}", "Local position (relative to the parent)"},
    {"action.setWorldPosition", "Set World Position", "Transform", K::Action,
     {ExecIn(), In("target", T::Entity, "self"), In("position", T::Vec3, "0 0 0")}, {ExecOut()}, {}, "{target}.worldPosition = {position}", ""},
    {"action.move", "Move", "Transform", K::Action, {ExecIn(), In("target", T::Entity, "self"), In("offset", T::Vec3, "0 0 1"), In("world", T::Bool, "false")},
     {ExecOut()}, {}, "{target}.translate({offset}, {world})", "Moves by an offset (its own axes, or the world's)"},
    {"action.rotate", "Rotate", "Transform", K::Action, {ExecIn(), In("target", T::Entity, "self"), In("degrees", T::Vec3, "0 90 0")}, {ExecOut()},
     {}, "{target}.rotate({degrees})", "Turns by degrees around X, Y, Z"},
    {"action.setRotation", "Set Rotation", "Transform", K::Action, {ExecIn(), In("target", T::Entity, "self"), In("degrees", T::Vec3, "0 0 0")},
     {ExecOut()}, {}, "{target}.rotation = {degrees}", "Angles in degrees"},
    {"action.setScale", "Set Scale", "Transform", K::Action, {ExecIn(), In("target", T::Entity, "self"), In("scale", T::Vec3, "1 1 1")}, {ExecOut()},
     {}, "{target}.scale = {scale}", ""},
    {"action.lookAt", "Look At", "Transform", K::Action, {ExecIn(), In("target", T::Entity, "self"), In("point", T::Vec3, "0 0 0")}, {ExecOut()},
     {}, "{target}.lookAt({point})", "Turns to face a point"},
    {"action.destroy", "Destroy", "Entity", K::Action, {ExecIn(), In("target", T::Entity, "self")}, {ExecOut()}, {}, "{target}.destroy()",
     "Removes the entity (at the end of the frame)"},
    {"action.setText", "Set Text", "UI", K::Action, {ExecIn(), In("target", T::Entity, "self"), In("text", T::String, "Text")}, {ExecOut()}, {},
     "{target}.text = {text}", "UI Text / Input Field"},
    {"action.setVisible", "Set Visible", "UI", K::Action, {ExecIn(), In("target", T::Entity, "self"), In("visible", T::Bool, "true")}, {ExecOut()},
     {}, "{target}.visible = {visible}", "Shows or hides a UI element"},
    {"action.setValue", "Set UI Value", "UI", K::Action, {ExecIn(), In("target", T::Entity, "self"), In("value", T::Number, "1")}, {ExecOut()},
     {}, "{target}.value = {value}", "Progress Bar / Slider / Toggle"},
    {"action.uiAnimate", "UI Animate", "UI", K::Action,
     {ExecIn(), In("target", T::Entity, "self"), In("property", T::String, "opacity"), In("to", T::Number, "0"), In("seconds", T::Number, "0.3")},
     {ExecOut()}, {}, "{target}.uiAnimate({property}, {to}, {seconds})", "Smoothly changes opacity, scale, x, y, width, value ..."},
    {"action.showCursor", "Show Mouse Cursor", "UI", K::Action, {ExecIn(), In("show", T::Bool, "true")}, {ExecOut()}, {}, "ui.cursor = {show}",
     "Frees the mouse for menus"},
    {"action.timeScale", "Set Time Scale", "Game", K::Action, {ExecIn(), In("scale", T::Number, "1")}, {ExecOut()}, {}, "time.scale = {scale}",
     "0 pauses the game, 0.5 = slow motion"},
    {"action.setLanguage", "Set Language", "UI", K::Action, {ExecIn(), In("code", T::String, "en")}, {ExecOut()}, {}, "lang.set({code})",
     "Switches the game's texts (Localization)"},
    {"action.playSound", "Play Sound", "Audio", K::Action,
     {ExecIn(), In("sound", T::String, "StarterContent/Sounds/UI/Click"), In("volume", T::Number, "1")}, {ExecOut()}, {}, "audio.play({sound}, {volume})",
     "A 2D sound (a path without extension picks a random variant)"},
    {"action.playSoundAt", "Play Sound At", "Audio", K::Action,
     {ExecIn(), In("sound", T::String, "StarterContent/Sounds/UI/Click"), In("position", T::Vec3, "0 0 0"), In("volume", T::Number, "1")},
     {ExecOut()}, {}, "audio.playAt({sound}, {position}, {volume})", "A 3D sound at a point"},
    {"action.addImpulse", "Add Impulse", "Physics", K::Action, {ExecIn(), In("target", T::Entity, "self"), In("impulse", T::Vec3, "0 5 0")}, {ExecOut()},
     {}, "{target}.addImpulse({impulse})", "A push to a Rigid Body (N*s)"},
    {"action.setVelocity", "Set Velocity", "Physics", K::Action, {ExecIn(), In("target", T::Entity, "self"), In("velocity", T::Vec3, "0 0 0")},
     {ExecOut()}, {}, "{target}.velocity = {velocity}", "Rigid Body velocity (m/s)"},
    {"action.emit", "Emit Particles", "Effects", K::Action, {ExecIn(), In("target", T::Entity, "self"), In("count", T::Number, "20")}, {ExecOut()},
     {}, "{target}.emit({count})", "A burst from a Particle Emitter"},
    {"action.ignite", "Ignite", "Effects", K::Action, {ExecIn(), In("target", T::Entity, "self")}, {ExecOut()}, {}, "{target}.ignite()",
     "Sets a Flammable object on fire"},
    {"action.animTrigger", "Animation Trigger", "Animation", K::Action, {ExecIn(), In("target", T::Entity, "self"), In("name", T::String, "Jump")},
     {ExecOut()}, {}, "{target}.animTrigger({name})", "Fires a trigger of the Animator's graph"},
    {"action.animParam", "Set Animation Parameter", "Animation", K::Action,
     {ExecIn(), In("target", T::Entity, "self"), In("name", T::String, "Speed"), In("value", T::Any, "0")}, {ExecOut()}, {},
     "{target}.animParam({name}, {value})", ""},
    {"action.characterMove", "Character Move", "Character", K::Action, {ExecIn(), In("velocity", T::Vec3, "0 0 0")}, {ExecOut()}, {},
     "character.move({velocity})", "Character Controller: horizontal speed this frame (m/s)"},
    {"action.characterJump", "Character Jump", "Character", K::Action, {ExecIn(), In("speed", T::Number, "5")}, {ExecOut()}, {},
     "character.jump({speed})", "Jumps if standing on the ground"},

    // --- Values ------------------------------------------------------------------------------------------------------------
    {"pure.getVariable", "Get Variable", "Variables", K::Pure, {}, {Out("value", T::Any, "{@variable}")}, {Setting("variable", "")}, "", ""},
    {"pure.self", "Self", "Entity", K::Pure, {}, {Out("entity", T::Entity, "entity")}, {}, "", "The entity this graph is on"},
    {"pure.find", "Find Entity", "Entity", K::Pure, {In("name", T::String, "Player")}, {Out("entity", T::Entity, "find({name})")}, {}, "",
     "By name (nil if there's none)"},
    {"pure.child", "Get Child", "Entity", K::Pure, {In("target", T::Entity, "self"), In("name", T::String, "Child")},
     {Out("entity", T::Entity, "{target}.child({name})")}, {}, "", ""},
    {"pure.parent", "Get Parent", "Entity", K::Pure, {In("target", T::Entity, "self")}, {Out("entity", T::Entity, "{target}.parent")}, {}, "", ""},
    {"pure.isValid", "Is Valid", "Entity", K::Pure, {In("value", T::Any, "nil")}, {Out("valid", T::Bool, "({value} != nil)")}, {}, "",
     "False for nil (e.g. Find Entity found nothing)"},
    {"pure.getName", "Get Name", "Entity", K::Pure, {In("target", T::Entity, "self")}, {Out("name", T::String, "{target}.name")}, {}, "", ""},
    {"pure.getPosition", "Get Position", "Transform", K::Pure, {In("target", T::Entity, "self")},
     {Out("position", T::Vec3, "{target}.position"), Out("world", T::Vec3, "{target}.worldPosition")}, {}, "", ""},
    {"pure.getForward", "Get Forward", "Transform", K::Pure, {In("target", T::Entity, "self")}, {Out("forward", T::Vec3, "{target}.worldForward")}, {}, "",
     "The direction it faces (world)"},
    {"pure.getVelocity", "Get Velocity", "Physics", K::Pure, {In("target", T::Entity, "self")}, {Out("velocity", T::Vec3, "{target}.velocity")}, {}, "", ""},
    {"pure.text", "Get Text", "UI", K::Pure, {In("target", T::Entity, "self")}, {Out("text", T::String, "{target}.text")}, {}, "", ""},
    {"pure.uiValue", "Get UI Value", "UI", K::Pure, {In("target", T::Entity, "self")}, {Out("value", T::Number, "{target}.value")}, {}, "", ""},
    {"pure.keyDown", "Key Down", "Input", K::Pure, {In("key", T::String, "W")}, {Out("down", T::Bool, "input.key({key})")}, {}, "", "Held this frame"},
    {"pure.moveInput", "Move Input", "Input", K::Pure, {}, {Out("move", T::Vec3, "input.move")}, {}, "", "WASD / arrows / stick (x right, z forward)"},
    {"pure.action", "Input Action", "Input", K::Pure, {In("action", T::String, "Jump")},
     {Out("down", T::Bool, "input.action({action})"), Out("pressed", T::Bool, "input.actionPressed({action})")}, {}, "",
     "Window > Input Actions"},
    {"pure.deltaTime", "Delta Time", "Game", K::Pure, {}, {Out("dt", T::Number, "time.dt")}, {}, "", "Seconds since the last frame"},
    {"pure.elapsed", "Time", "Game", K::Pure, {}, {Out("seconds", T::Number, "time.elapsed")}, {}, "", "Seconds since Play"},
    {"pure.random", "Random", "Math", K::Pure, {In("min", T::Number, "0"), In("max", T::Number, "1")}, {Out("value", T::Number, "random({min}, {max})")},
     {}, "", ""},
    {"pure.add", "Add", "Math", K::Pure, {In("a", T::Number, "0"), In("b", T::Number, "1")}, {Out("result", T::Number, "({a} + {b})")}, {}, "", ""},
    {"pure.subtract", "Subtract", "Math", K::Pure, {In("a", T::Number, "0"), In("b", T::Number, "1")}, {Out("result", T::Number, "({a} - {b})")}, {},
     "", ""},
    {"pure.multiply", "Multiply", "Math", K::Pure, {In("a", T::Number, "1"), In("b", T::Number, "2")}, {Out("result", T::Number, "({a} * {b})")}, {},
     "", ""},
    {"pure.divide", "Divide", "Math", K::Pure, {In("a", T::Number, "1"), In("b", T::Number, "2")}, {Out("result", T::Number, "({a} / {b})")}, {}, "", ""},
    {"pure.min", "Min", "Math", K::Pure, {In("a", T::Number, "0"), In("b", T::Number, "1")}, {Out("result", T::Number, "min({a}, {b})")}, {}, "", ""},
    {"pure.max", "Max", "Math", K::Pure, {In("a", T::Number, "0"), In("b", T::Number, "1")}, {Out("result", T::Number, "max({a}, {b})")}, {}, "", ""},
    {"pure.clamp", "Clamp", "Math", K::Pure, {In("value", T::Number, "0"), In("min", T::Number, "0"), In("max", T::Number, "1")},
     {Out("result", T::Number, "clamp({value}, {min}, {max})")}, {}, "", ""},
    {"pure.lerp", "Lerp", "Math", K::Pure, {In("a", T::Number, "0"), In("b", T::Number, "1"), In("t", T::Number, "0.5")},
     {Out("result", T::Number, "lerp({a}, {b}, {t})")}, {}, "", "Blend from A to B by T (0..1)"},
    {"pure.abs", "Abs", "Math", K::Pure, {In("value", T::Number, "0")}, {Out("result", T::Number, "abs({value})")}, {}, "", ""},
    {"pure.sin", "Sin", "Math", K::Pure, {In("radians", T::Number, "0")}, {Out("result", T::Number, "sin({radians})")}, {}, "", ""},
    {"pure.round", "Round", "Math", K::Pure, {In("value", T::Number, "0")}, {Out("result", T::Number, "round({value})")}, {}, "", ""},
    {"pure.greater", "Greater", "Compare", K::Pure, {In("a", T::Number, "0"), In("b", T::Number, "0")}, {Out("result", T::Bool, "({a} > {b})")}, {},
     "", "A > B"},
    {"pure.less", "Less", "Compare", K::Pure, {In("a", T::Number, "0"), In("b", T::Number, "0")}, {Out("result", T::Bool, "({a} < {b})")}, {}, "",
     "A < B"},
    {"pure.equal", "Equal", "Compare", K::Pure, {In("a", T::Any, "0"), In("b", T::Any, "0")}, {Out("result", T::Bool, "({a} == {b})")}, {}, "", "A == B"},
    {"pure.notEqual", "Not Equal", "Compare", K::Pure, {In("a", T::Any, "0"), In("b", T::Any, "0")}, {Out("result", T::Bool, "({a} != {b})")}, {},
     "", "A != B"},
    {"pure.and", "And", "Logic", K::Pure, {In("a", T::Bool, "true"), In("b", T::Bool, "true")}, {Out("result", T::Bool, "({a} and {b})")}, {}, "", ""},
    {"pure.or", "Or", "Logic", K::Pure, {In("a", T::Bool, "false"), In("b", T::Bool, "false")}, {Out("result", T::Bool, "({a} or {b})")}, {}, "", ""},
    {"pure.not", "Not", "Logic", K::Pure, {In("value", T::Bool, "false")}, {Out("result", T::Bool, "(not {value})")}, {}, "", ""},
    {"pure.makeVec3", "Make Vector", "Vector", K::Pure, {In("x", T::Number, "0"), In("y", T::Number, "0"), In("z", T::Number, "0")},
     {Out("vector", T::Vec3, "vec3({x}, {y}, {z})")}, {}, "", ""},
    {"pure.breakVec3", "Break Vector", "Vector", K::Pure, {In("vector", T::Vec3, "0 0 0")},
     {Out("x", T::Number, "({vector}).x"), Out("y", T::Number, "({vector}).y"), Out("z", T::Number, "({vector}).z")}, {}, "", ""},
    {"pure.addVec3", "Add Vectors", "Vector", K::Pure, {In("a", T::Vec3, "0 0 0"), In("b", T::Vec3, "0 0 0")}, {Out("vector", T::Vec3, "({a} + {b})")},
     {}, "", ""},
    {"pure.scaleVec3", "Scale Vector", "Vector", K::Pure, {In("vector", T::Vec3, "0 0 1"), In("scale", T::Number, "1")},
     {Out("vector", T::Vec3, "({vector} * {scale})")}, {}, "", ""},
    {"pure.length", "Vector Length", "Vector", K::Pure, {In("vector", T::Vec3, "0 0 0")}, {Out("length", T::Number, "({vector}).length()")}, {}, "", ""},
    {"pure.normalize", "Normalize", "Vector", K::Pure, {In("vector", T::Vec3, "0 0 1")}, {Out("vector", T::Vec3, "({vector}).normalized()")}, {},
     "", ""},
    {"pure.distance", "Distance", "Vector", K::Pure, {In("a", T::Vec3, "0 0 0"), In("b", T::Vec3, "0 0 0")},
     {Out("distance", T::Number, "({a}).distance({b})")}, {}, "", "Meters between two points"},
    {"pure.concat", "Join Text", "Text", K::Pure, {In("a", T::String, ""), In("b", T::String, "")}, {Out("text", T::String, "({a} + {b})")}, {}, "",
     "Numbers and the like become text"},
    {"pure.toString", "To Text", "Text", K::Pure, {In("value", T::Any, "0")}, {Out("text", T::String, "str({value})")}, {}, "", ""},
    {"pure.translate", "Translate", "Text", K::Pure, {In("key", T::String, "menu.play")}, {Out("text", T::String, "tr({key})")}, {}, "",
     "The text of a Localization key in the player's language"},
    {"pure.number", "Number", "Values", K::Pure, {In("value", T::Number, "0")}, {Out("value", T::Number, "{value}")}, {}, "", ""},
    {"pure.bool", "Boolean", "Values", K::Pure, {In("value", T::Bool, "true")}, {Out("value", T::Bool, "{value}")}, {}, "", ""},
    {"pure.string", "Text", "Values", K::Pure, {In("value", T::String, "")}, {Out("value", T::String, "{value}")}, {}, "", ""},
    {"pure.vec3", "Vector", "Values", K::Pure, {In("value", T::Vec3, "0 0 0")}, {Out("value", T::Vec3, "{value}")}, {}, "", ""},
};

bool IsIdentifier(StringView s)
{
    if (s.Empty() || (s[0] >= '0' && s[0] <= '9'))
        return false;
    for (char c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'))
            return false;
    return true;
}

String Identifier(StringView s)
{
    String out;
    for (char c : s)
        out += ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_') ? c : '_';
    if (out.Empty() || (out[0] >= '0' && out[0] <= '9'))
        out = "G" + out;
    return out;
}

String Quote(StringView s)
{
    String out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\')
            out += '\\';
        if (c == '\n') {
            out += "\\n";
            continue;
        }
        out += c;
    }
    out += '"';
    return out;
}

String FormatNumber(f64 v)
{
    String s = String::Format("{}", v);
    return s;
}

/// A literal typed into a pin (or a variable's value) -> ZeqouScript source.
String Literal(GraphPinType type, StringView raw)
{
    if (type == T::String)
        return Quote(raw); // text keeps its spaces
    const StringView text = raw.Trim();
    switch (type) {
    case T::Number: {
        const Optional<f32> v = ParseFloat(text);
        return v ? FormatNumber(f64(*v)) : String("0");
    }
    case T::Bool: return String(text.EqualsIgnoreCase("true") || text == "1" ? "true" : "false");
    case T::Vec3: {
        f32 n[3] = {};
        u32 count = 0;
        usize i = 0;
        while (i < text.Size() && count < 3) {
            while (i < text.Size() && (text[i] == ' ' || text[i] == ','))
                ++i;
            usize j = i;
            while (j < text.Size() && text[j] != ' ' && text[j] != ',')
                ++j;
            if (j > i)
                n[count++] = ParseFloat(text.SubStr(i, j - i)).ValueOr(0.0f);
            i = j;
        }
        if (count == 1)
            n[1] = n[2] = n[0];
        return String::Format("vec3({}, {}, {})", FormatNumber(n[0]), FormatNumber(n[1]), FormatNumber(n[2]));
    }
    case T::Entity:
        if (text.Empty() || text.EqualsIgnoreCase("self"))
            return String("entity");
        if (text.EqualsIgnoreCase("nil") || text.EqualsIgnoreCase("none"))
            return String("nil");
        return String::Format("find({})", Quote(text));
    case T::Any:
        if (text.Empty())
            return String("nil");
        if (ParseFloat(text) || text == "true" || text == "false" || text == "nil" || text.StartsWith("\"") || text.StartsWith("vec3("))
            return String(text);
        return Quote(text); // plain words are text
    default: return String("nil");
    }
}

GraphPinType ParsePinType(StringView s)
{
    for (u32 t = 0; t <= u32(T::Entity); ++t)
        if (s.EqualsIgnoreCase(GraphPinTypeName(GraphPinType(t))))
            return GraphPinType(t);
    return T::Any;
}

} // namespace

// --- Library -----------------------------------------------------------------------------------------------------------

u32 GraphNodeDef::InputCount() const
{
    u32 n = 0;
    while (n < 6 && inputs[n].name)
        ++n;
    return n;
}

u32 GraphNodeDef::OutputCount() const
{
    u32 n = 0;
    while (n < 5 && outputs[n].name)
        ++n;
    return n;
}

const GraphPinDef* GraphNodeDef::Input(StringView pin) const
{
    for (u32 i = 0; i < InputCount(); ++i)
        if (pin == inputs[i].name)
            return &inputs[i];
    return nullptr;
}

const GraphPinDef* GraphNodeDef::Output(StringView pin) const
{
    for (u32 i = 0; i < OutputCount(); ++i)
        if (pin == outputs[i].name)
            return &outputs[i];
    return nullptr;
}

Span<const GraphNodeDef> GraphNodeLibrary() { return Span<const GraphNodeDef>(Library, ZX_ARRAY_COUNT(Library)); }

const GraphNodeDef* FindGraphNode(StringView id)
{
    for (const GraphNodeDef& d : Library)
        if (id == d.id)
            return &d;
    return nullptr;
}

const char* GraphPinTypeName(GraphPinType t)
{
    static const char* const Names[] = {"exec", "any", "number", "bool", "string", "vec3", "entity"};
    return Names[u32(t)];
}

bool GraphPinsCompatible(GraphPinType from, GraphPinType to)
{
    if (from == T::Exec || to == T::Exec)
        return from == to;
    if (from == to || from == T::Any || to == T::Any)
        return true;
    if (to == T::String) // shown as text
        return true;
    if (to == T::Bool) // any value is true / false
        return true;
    return false;
}

// --- Graph -------------------------------------------------------------------------------------------------------------

GraphNode* Graph::FindNode(u32 id)
{
    for (GraphNode& n : nodes)
        if (n.id == id)
            return &n;
    return nullptr;
}

const GraphNode* Graph::FindNode(u32 id) const
{
    for (const GraphNode& n : nodes)
        if (n.id == id)
            return &n;
    return nullptr;
}

u32 Graph::NextId() const
{
    u32 id = 0;
    for (const GraphNode& n : nodes)
        id = Max(id, n.id);
    return id + 1;
}

const GraphLink* Graph::LinkInto(u32 node, StringView pin) const
{
    for (const GraphLink& l : links)
        if (l.toNode == node && l.toPin == pin)
            return &l;
    return nullptr;
}

void Graph::LinksFrom(u32 node, StringView pin, Array<const GraphLink*>& out) const
{
    out.Clear();
    for (const GraphLink& l : links)
        if (l.fromNode == node && l.fromPin == pin)
            out.PushBack(&l);
}

const GraphVariable* Graph::FindVariable(StringView variableName) const
{
    for (const GraphVariable& v : variables)
        if (v.name == variableName)
            return &v;
    return nullptr;
}

GraphPinType Graph::PinType(const GraphNode& node, const GraphPinDef& pin) const
{
    if (pin.type == T::Any && (node.type == "pure.getVariable" || node.type == "action.setVariable"))
        if (const String* var = node.settings.Find("variable"))
            if (const GraphVariable* v = FindVariable(*var))
                return v->type;
    return pin.type;
}

bool ParseGraph(StringView json, Graph& out, String* error)
{
    JsonValue root;
    JsonParseError pe;
    if (!ParseJson(json, root, &pe)) {
        if (error)
            *error = String::Format("line {}: {}", pe.line, pe.message);
        return false;
    }
    if (root.Get("format").AsString() != "zgraph") {
        if (error)
            *error = "not a .zgraph file";
        return false;
    }
    out = Graph();
    out.name = String(root.Get("name").AsString("Graph"));
    if (const JsonValue* vars = root.Find("variables"))
        for (const JsonValue& v : vars->Elements())
            out.variables.PushBack(GraphVariable{String(v.Get("name").AsString()), ParsePinType(v.Get("type").AsString("number")),
                                                 String(v.Get("value").AsString("0")), v.Get("exposed").AsBool(), String(v.Get("tooltip").AsString())});
    if (const JsonValue* nodes = root.Find("nodes"))
        for (const JsonValue& n : nodes->Elements()) {
            GraphNode node;
            node.id = u32(n.Get("id").AsInt());
            node.type = String(n.Get("type").AsString());
            if (const JsonValue* p = n.Find("pos"); p && p->Size() >= 2)
                node.position = Vec2((*p)[0].AsFloat(), (*p)[1].AsFloat());
            if (const JsonValue* vals = n.Find("values"))
                for (const JsonMember& m : vals->Members())
                    node.values.InsertOrAssign(m.first, String(m.second.AsString()));
            if (const JsonValue* st = n.Find("settings"))
                for (const JsonMember& m : st->Members())
                    node.settings.InsertOrAssign(m.first, String(m.second.AsString()));
            node.comment = String(n.Get("comment").AsString());
            out.nodes.PushBack(move(node));
        }
    if (const JsonValue* links = root.Find("links"))
        for (const JsonValue& l : links->Elements())
            out.links.PushBack(GraphLink{u32(l.Get("from").AsInt()), String(l.Get("fromPin").AsString()), u32(l.Get("to").AsInt()),
                                         String(l.Get("toPin").AsString())});
    return true;
}

String WriteGraph(const Graph& g)
{
    JsonValue root = JsonValue::MakeObject();
    root.Set("format", "zgraph");
    root.Set("version", 1);
    root.Set("name", g.name);
    JsonValue vars = JsonValue::MakeArray();
    for (const GraphVariable& v : g.variables) {
        JsonValue o = JsonValue::MakeObject();
        o.Set("name", v.name);
        o.Set("type", GraphPinTypeName(v.type));
        o.Set("value", v.value);
        if (v.exposed)
            o.Set("exposed", true);
        if (!v.tooltip.Empty())
            o.Set("tooltip", v.tooltip);
        vars.PushBack(move(o));
    }
    root.Set("variables", move(vars));
    JsonValue nodes = JsonValue::MakeArray();
    for (const GraphNode& n : g.nodes) {
        JsonValue o = JsonValue::MakeObject();
        o.Set("id", n.id);
        o.Set("type", n.type);
        JsonValue pos = JsonValue::MakeArray();
        pos.PushBack(JsonValue(Round(n.position.x)));
        pos.PushBack(JsonValue(Round(n.position.y)));
        o.Set("pos", move(pos));
        auto sorted = [](const HashMap<String, String>& map) {
            Array<String> keys = map.Keys();
            Sort(keys, [](const String& a, const String& b) { return a.View() < b.View(); });
            JsonValue obj = JsonValue::MakeObject();
            for (const String& k : keys)
                obj.Set(k, *map.Find(k));
            return obj;
        };
        if (!n.values.Empty())
            o.Set("values", sorted(n.values));
        if (!n.settings.Empty())
            o.Set("settings", sorted(n.settings));
        if (!n.comment.Empty())
            o.Set("comment", n.comment);
        nodes.PushBack(move(o));
    }
    root.Set("nodes", move(nodes));
    JsonValue links = JsonValue::MakeArray();
    for (const GraphLink& l : g.links) {
        JsonValue o = JsonValue::MakeObject();
        o.Set("from", l.fromNode);
        o.Set("fromPin", l.fromPin);
        o.Set("to", l.toNode);
        o.Set("toPin", l.toPin);
        links.PushBack(move(o));
    }
    root.Set("links", move(links));
    return ToJsonString(root);
}

// --- Code generation ---------------------------------------------------------------------------------------------------

namespace {

struct CodeLine
{
    String text;
    u32 node = 0;
};

struct Function
{
    String signature; // "fn update(dt)"
    Array<CodeLine> body;
};

struct Codegen
{
    const Graph& g;
    GraphCompileResult& out;
    Array<CodeLine> fields;
    Array<Function> functions;
    Array<CodeLine> updatePrelude; // delays, timers, key events (run before the On Update chains)
    bool delays = false;

    static String Indent(u32 level) { return String(StringView("                                                ", Min<u32>(level * 4, 48))); }

    void Error(const GraphNode* n, StringView message)
    {
        const GraphNodeDef* d = n ? FindGraphNode(n->type) : nullptr;
        out.errors.PushBack(n ? String::Format("Node '{}' (#{}): {}", d ? d->title : n->type.View(), n->id, message) : String(message));
        if (n && out.errorNodes.Find(n->id) == NPos)
            out.errorNodes.PushBack(n->id);
    }

    Function& FunctionFor(StringView signature)
    {
        for (Function& f : functions)
            if (f.signature == signature)
                return f;
        functions.PushBack(Function{String(signature), {}});
        return functions.Back();
    }

    String Setting(const GraphNode& n, const GraphNodeDef& d, StringView name) const
    {
        if (const String* v = n.settings.Find(name))
            return *v;
        for (const GraphSettingDef& s : d.settings)
            if (s.name && name == s.name)
                return String(s.defaultValue);
        return {};
    }

    /// Expands {pin} {@setting} {$tmp} {#id} in a template.
    String Expand(const GraphNode& n, const GraphNodeDef& d, StringView tmpl, u32 depth)
    {
        String r;
        for (usize i = 0; i < tmpl.Size(); ++i) {
            if (tmpl[i] != '{') {
                r += tmpl[i];
                continue;
            }
            const usize end = tmpl.Find('}', i);
            if (end == StringView::NPos) {
                r += tmpl.DropLeft(i);
                break;
            }
            const StringView key = tmpl.SubStr(i + 1, end - i - 1);
            if (key.StartsWith("@")) {
                const String value = Setting(n, d, key.DropLeft(1));
                if (key == "@event" || key == "@variable") {
                    if (!IsIdentifier(value))
                        Error(&n, value.Empty() ? String("choose a variable") : String::Format("'{}' isn't a valid name (letters, digits, _)", value));
                    else if (key == "@variable" && !g.FindVariable(value))
                        Error(&n, String::Format("there's no variable '{}'", value));
                    r += Identifier(value);
                } else {
                    r += value;
                }
            } else if (key == "#id") {
                r += String::Format("{}", n.id);
            } else if (const GraphPinDef* pin = d.Input(key)) {
                r += Input(n, d, *pin, depth);
            } else {
                r += tmpl.SubStr(i, end - i + 1);
            }
            i = end;
        }
        return r;
    }

    /// The expression feeding an input pin: a linked output, else the typed literal, else the default.
    String Input(const GraphNode& n, const GraphNodeDef& d, const GraphPinDef& pin, u32 depth)
    {
        if (const GraphLink* l = g.LinkInto(n.id, pin.name)) {
            const GraphNode* src = g.FindNode(l->fromNode);
            const GraphNodeDef* sd = src ? FindGraphNode(src->type) : nullptr;
            const GraphPinDef* sp = sd ? sd->Output(l->fromPin) : nullptr;
            if (!sp) {
                Error(&n, String::Format("the wire into '{}' comes from a missing pin", pin.name));
                return String("nil");
            }
            const GraphPinType fromType = g.PinType(*src, *sp), toType = g.PinType(n, pin);
            if (!GraphPinsCompatible(fromType, toType))
                Error(&n, String::Format("'{}' expects {} but gets {}", pin.name, GraphPinTypeName(toType), GraphPinTypeName(fromType)));
            if (depth > 64) {
                Error(&n, "the values loop back into themselves");
                return String("nil");
            }
            String e = Expand(*src, *sd, sp->expr, depth + 1);
            if (pin.type == T::String && sp->type != T::String && sp->type != T::Any)
                e = String::Format("str({})", e);
            return e;
        }
        (void)d;
        const String* typed = n.values.Find(pin.name);
        return Literal(g.PinType(n, pin), typed ? typed->View() : StringView(pin.defaultValue));
    }

    /// Statements along the exec wire leaving `from`.`pin`.
    void Chain(const GraphNode& from, StringView pin, Array<CodeLine>& body, u32 level, Array<u32>& stack)
    {
        Array<const GraphLink*> links;
        g.LinksFrom(from.id, pin, links);
        if (links.Empty())
            return;
        if (links.Size() > 1)
            Error(&from, String::Format("'{}' has {} wires; an execution pin leads to one node (use a Sequence)", pin, links.Size()));
        const GraphNode* next = g.FindNode(links[0]->toNode);
        if (next)
            Statement(*next, body, level, stack);
    }

    void Emit(Array<CodeLine>& body, u32 level, String text, u32 node) { body.PushBack(CodeLine{Indent(level) + text, node}); }

    void Statement(const GraphNode& n, Array<CodeLine>& body, u32 level, Array<u32>& stack)
    {
        const GraphNodeDef* d = FindGraphNode(n.type);
        if (!d) {
            Error(&n, String::Format("unknown node type '{}'", n.type));
            return;
        }
        if (stack.Find(n.id) != NPos) {
            Error(&n, "the execution wires loop back here (use a For Loop)");
            return;
        }
        if (d->kind == K::Event || d->kind == K::Pure) {
            Error(&n, "this node has no execution input");
            return;
        }
        stack.PushBack(n.id);
        const String id = String::Format("{}", n.id);
        const StringView t = n.type.View();
        if (d->kind == K::Action) {
            Emit(body, level, Expand(n, *d, d->code, 0), n.id);
            Chain(n, "then", body, level, stack);
        } else if (t == "flow.branch") {
            Emit(body, level, String::Format("if {} {{", Input(n, *d, *d->Input("condition"), 0)), n.id);
            Chain(n, "true", body, level + 1, stack);
            Emit(body, level, "} else {", n.id);
            Chain(n, "false", body, level + 1, stack);
            Emit(body, level, "}", n.id);
        } else if (t == "flow.sequence") {
            for (const char* p : {"then0", "then1", "then2"})
                Chain(n, p, body, level, stack);
        } else if (t == "flow.forLoop") {
            Emit(body, level,
                 String::Format("for _i{} in {}..({} + 1) {{", id, Input(n, *d, *d->Input("first"), 0), Input(n, *d, *d->Input("last"), 0)), n.id);
            Chain(n, "body", body, level + 1, stack);
            Emit(body, level, "}", n.id);
            Chain(n, "completed", body, level, stack);
        } else if (t == "flow.doOnce") {
            fields.PushBack(CodeLine{String::Format("    var _once{} = false", id), n.id});
            Emit(body, level, String::Format("if not _once{} {{", id), n.id);
            Emit(body, level + 1, String::Format("_once{} = true", id), n.id);
            Chain(n, "then", body, level + 1, stack);
            Emit(body, level, "}", n.id);
        } else if (t == "flow.flipFlop") {
            fields.PushBack(CodeLine{String::Format("    var _ff{} = false", id), n.id});
            Emit(body, level, String::Format("_ff{} = not _ff{}", id, id), n.id);
            Emit(body, level, String::Format("if _ff{} {{", id), n.id);
            Chain(n, "a", body, level + 1, stack);
            Emit(body, level, "} else {", n.id);
            Chain(n, "b", body, level + 1, stack);
            Emit(body, level, "}", n.id);
        } else if (t == "flow.delay") {
            // The rest runs from a function of its own, called by On Update when the time comes.
            delays = true;
            Emit(body, level, String::Format("_delays.push([time.elapsed + {}, _after{}])", Input(n, *d, *d->Input("seconds"), 0), id), n.id);
            Function after{String::Format("fn _after{}()", id), {}};
            Array<u32> inner;
            Chain(n, "completed", after.body, 2, inner);
            functions.PushBack(move(after));
        }
        stack.PopBack();
    }

    void Run()
    {
        // Variables.
        for (const GraphVariable& v : g.variables) {
            if (!IsIdentifier(v.name)) {
                out.errors.PushBack(String::Format("Variable '{}': not a valid name (letters, digits, _; no spaces)", v.name));
                continue;
            }
            const String value = Literal(v.type, v.value);
            if (v.exposed && v.type != T::Entity && v.type != T::Any) {
                if (!v.tooltip.Empty())
                    fields.PushBack(CodeLine{String::Format("    @tooltip({})", Quote(v.tooltip)), 0});
                fields.PushBack(CodeLine{String::Format("    prop {} = {}", v.name, value), 0});
            } else {
                fields.PushBack(CodeLine{String::Format("    var {} = {}", v.name, value), 0});
            }
        }
        // Events, in node order (several of one kind run one after another).
        for (const GraphNode& n : g.nodes) {
            const GraphNodeDef* d = FindGraphNode(n.type);
            if (!d) {
                Error(&n, String::Format("unknown node type '{}'", n.type));
                continue;
            }
            if (d->kind != K::Event)
                continue;
            Array<u32> stack;
            const StringView code = d->code;
            if (code.StartsWith("@update:")) {
                Emit(updatePrelude, 2, String::Format("if {} {{", Expand(n, *d, code.DropLeft(8), 0)), n.id);
                Chain(n, "then", updatePrelude, 3, stack);
                Emit(updatePrelude, 2, "}", n.id);
            } else if (code == "@every") {
                const String seconds = Literal(T::Number, Setting(n, *d, "seconds"));
                fields.PushBack(CodeLine{String::Format("    var _t{} = 0", n.id), n.id});
                Emit(updatePrelude, 2, String::Format("_t{} += dt", n.id), n.id);
                Emit(updatePrelude, 2, String::Format("if _t{} >= {} {{", n.id, seconds), n.id);
                Emit(updatePrelude, 3, String::Format("_t{} -= {}", n.id, seconds), n.id);
                Chain(n, "then", updatePrelude, 3, stack);
                Emit(updatePrelude, 2, "}", n.id);
            } else {
                Function& f = FunctionFor(Expand(n, *d, code, 0));
                const String widget = Setting(n, *d, "widget");
                if (!widget.Empty()) {
                    Emit(f.body, 2, String::Format("if name == {} {{", Quote(widget)), n.id);
                    Chain(n, "then", f.body, 3, stack);
                    Emit(f.body, 2, "}", n.id);
                } else {
                    Chain(n, "then", f.body, 2, stack);
                }
            }
        }
        // Nodes nothing reaches (a warning: they never run).
        for (const GraphNode& n : g.nodes) {
            const GraphNodeDef* d = FindGraphNode(n.type);
            if (!d || d->kind == K::Event || d->kind == K::Pure)
                continue;
            bool reached = false;
            for (const GraphLink& l : g.links)
                reached = reached || (l.toNode == n.id && l.toPin == "in");
            if (!reached)
                out.warnings.PushBack(String::Format("Node '{}' (#{}) isn't connected to an event: it never runs", d->title, n.id));
        }
    }

    void Write()
    {
        Array<CodeLine> lines;
        lines.PushBack(CodeLine{String::Format("// Generated from the graph '{}' - edit the graph (.zgraph), not this text.", g.name), 0});
        lines.PushBack(CodeLine{String::Format("script {} {{", Identifier(g.name)), 0});
        for (const CodeLine& f : fields)
            lines.PushBack(f);
        if (delays)
            lines.PushBack(CodeLine{"    var _delays = []", 0});
        // On Update gets the timers, key events and delays first.
        if (!updatePrelude.Empty() || delays) {
            Function& u = FunctionFor("fn update(dt)");
            Array<CodeLine> body;
            if (delays) {
                body.PushBack(CodeLine{"        var _k = 0", 0});
                body.PushBack(CodeLine{"        while _k < len(_delays) {", 0});
                body.PushBack(CodeLine{"            if _delays[_k][0] <= time.elapsed {", 0});
                body.PushBack(CodeLine{"                let _f = _delays[_k][1]", 0});
                body.PushBack(CodeLine{"                _delays.remove(_k)", 0});
                body.PushBack(CodeLine{"                _f()", 0});
                body.PushBack(CodeLine{"            } else { _k += 1 }", 0});
                body.PushBack(CodeLine{"        }", 0});
            }
            body.Append(updatePrelude.View());
            body.Append(u.body.View());
            u.body = move(body);
        }
        for (const Function& f : functions) {
            lines.PushBack(CodeLine{"", 0});
            lines.PushBack(CodeLine{String::Format("    {} {{", f.signature), 0});
            for (const CodeLine& l : f.body)
                lines.PushBack(l);
            lines.PushBack(CodeLine{"    }", 0});
        }
        lines.PushBack(CodeLine{"}", 0});
        out.source.Clear();
        out.lineNodes.Clear();
        for (const CodeLine& l : lines) {
            out.source += l.text;
            out.source += "\n";
            out.lineNodes.PushBack(l.node);
        }
    }
};

} // namespace

bool CompileGraph(const Graph& graph, GraphCompileResult& out)
{
    out = GraphCompileResult();
    Codegen c{graph, out, {}, {}, {}, false};
    c.Run();
    c.Write();
    return out.errors.Empty();
}

String DescribeGraphError(const Graph& graph, const GraphCompileResult& result, u32 line, StringView message)
{
    // The node of that line, else the nearest one above it.
    for (i64 l = i64(line) - 1; l >= 0 && l < i64(result.lineNodes.Size()); --l)
        if (const u32 id = result.lineNodes[usize(l)]) {
            const GraphNode* n = graph.FindNode(id);
            const GraphNodeDef* d = n ? FindGraphNode(n->type) : nullptr;
            return String::Format("Node '{}' (#{}): {}", d ? d->title : "?", id, message);
        }
    return String(message);
}

Graph MakeExampleGraph()
{
    // Spinner: turns Speed degrees per second; Space logs how many times it was pressed.
    Graph g;
    g.name = "SpinnerGraph";
    g.variables.PushBack(GraphVariable{"speed", T::Number, "90", true, "Degrees per second"});
    g.variables.PushBack(GraphVariable{"presses", T::Number, "0", false, {}});
    auto node = [&](u32 id, const char* type, Vec2 pos) -> GraphNode& {
        GraphNode n;
        n.id = id;
        n.type = type;
        n.position = pos;
        g.nodes.PushBack(move(n));
        return g.nodes.Back();
    };
    auto link = [&](u32 a, const char* ap, u32 b, const char* bp) { g.links.PushBack(GraphLink{a, ap, b, bp}); };
    node(1, "event.update", Vec2(0, 0));
    node(2, "action.rotate", Vec2(800, 0));
    node(3, "pure.getVariable", Vec2(0, 140)).settings.InsertOrAssign("variable", "speed");
    node(4, "pure.multiply", Vec2(270, 140));
    node(5, "pure.makeVec3", Vec2(540, 140));
    link(1, "then", 2, "in");
    link(3, "value", 4, "a");
    link(1, "dt", 4, "b");
    link(4, "result", 5, "y");
    link(5, "vector", 2, "degrees");
    node(6, "event.keyPressed", Vec2(0, 340)).settings.InsertOrAssign("key", "Space");
    GraphNode& set = node(7, "action.setVariable", Vec2(540, 340));
    set.settings.InsertOrAssign("variable", "presses");
    node(8, "pure.getVariable", Vec2(0, 480)).settings.InsertOrAssign("variable", "presses");
    node(9, "pure.add", Vec2(270, 480));
    node(10, "action.print", Vec2(1080, 340));
    node(11, "pure.concat", Vec2(810, 480)).values.InsertOrAssign("a", "Space pressed: ");
    link(6, "then", 7, "in");
    link(8, "value", 9, "a");
    link(9, "result", 7, "value");
    link(7, "then", 10, "in");
    link(8, "value", 11, "b"); // read after Set Variable: the new count
    link(11, "text", 10, "text");
    return g;
}

} // namespace zx
