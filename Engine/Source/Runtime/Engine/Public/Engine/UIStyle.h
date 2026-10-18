#pragma once

// UI style sheets (.zstyle): CSS-like rules that give a whole canvas its look in one place (UI Canvas > Style).
//
//   /* Main menu */
//   @accent: #3B8CF5;                         variables, used as $accent
//   Button { color: #1E2430E6; hover-color: $accent; corner-radius: 12; transition: 0.12; }
//   Button:hover { scale: 1.06; }             states: hover, pressed, focused (gamepad / keyboard), disabled
//   Text.title { font-size: 72; weight: Bold; }   classes come from UI Element > Style Class ("title big")
//   Panel.menu Button { margin: 4 0; }         descendant selectors; #Name = an entity name; * = any widget
//   Button:hover Text { color: #FFFFFF; }      (a Text inside a hovered Button)
//
// Properties are the fields shown in Details (camelCase, kebab-case or with spaces: "font-size", "fontSize"): the
// widget's first (Button, Text, Panel, Image, Slider, Toggle, Progress Bar, Input Field), then the UI Element's
// (size, width, height, padding, margin, gap, direction, justify, align, opacity, scale, translate, transition...).
// Aliases: background / background-color = color, border-radius / radius = corner-radius, font-weight = weight,
// text-align = align. Numbers may end in px / s. Vec4 values (padding, margin) follow CSS: 1, 2, 3 or 4 numbers
// (top right bottom left). Colors: #RGB, #RGBA, #RRGGBB, #RRGGBBAA, rgb(r, g, b), rgba(r, g, b, a) (0..255, alpha
// 0..1), transparent, white, black. Enums by name (Bold, Center, SpaceBetween / space-between).
// More specific selectors win (#name > .class / :state > Type), then later rules. Properties set by a style
// override the widget's own values (the style is the source of the look).

#include "Core/Containers/Array.h"
#include "Core/Reflection/Reflection.h"
#include "Core/Math/Color.h"
#include "Core/String/String.h"

namespace zx {

enum class UIStyleState : u8
{
    None = 0,
    Hover = 1 << 0,
    Pressed = 1 << 1,
    Focused = 1 << 2,
    Disabled = 1 << 3,
};
ZX_ENUM_FLAGS(UIStyleState)

/// One step of a selector: Type.class1.class2#Name:state.
struct UIStyleCompound
{
    String type;  // "" = any widget; widget names: Element, Canvas, Panel, Text, Image, Button, ProgressBar, Slider, Toggle, InputField
    String name;  // #Name (entity name)
    Array<String> classes;
    UIStyleState states = UIStyleState::None;
};

struct UIStyleSelector
{
    Array<UIStyleCompound> parts; // ancestors first; the last part is the styled widget
    u32 specificity = 0;          // ids * 10000 + (classes + states) * 100 + types
    bool HasStates() const;
};

struct UIStyleProperty
{
    String name;  // normalized: lower case, no '-', '_' or spaces
    String value; // variables already substituted
    u32 line = 0;
};

struct UIStyleRule
{
    Array<UIStyleSelector> selectors; // "A, B { }"
    Array<UIStyleProperty> properties;
    u32 line = 0;
};

struct UIStyleSheet
{
    Array<UIStyleRule> rules;
    Array<String> errors; // "line N: message"
};

/// Parses a .zstyle text (errors are collected, the rest still loads). Returns false if there were errors.
bool ParseUIStyle(StringView text, UIStyleSheet& out);

/// "font-size" / "Font Size" / "fontSize" -> "fontsize".
String NormalizeStyleName(StringView name);

/// Parses a style value into a reflected value of `type` (numbers, bools, enums, strings, Vec2, Vec4 in CSS order,
/// LinearColor). False if the text doesn't fit the type.
bool ParseStyleValue(StringView text, const TypeInfo& type, void* object);
/// A color in the style syntax (#hex, rgb(), rgba(), names).
bool ParseStyleColor(StringView text, LinearColor& out);

} // namespace zx
