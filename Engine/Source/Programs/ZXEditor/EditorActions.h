#pragma once

// Editor actions: every command of the menus, the toolbar and the keyboard, in one registry. Shortcuts are configurable
// in Saved/Config/Keys.ini ("file.saveScene = Ctrl+S", up to two chords separated by ','), the command palette
// (Ctrl+P / Ctrl+Shift+P) searches them together with assets and entities.

#include "Core/Containers/Function.h"
#include "Platform/Input.h"
#include "UI/UI.h"

namespace zx::editor {

class Editor;

struct KeyChord
{
    Key key = Key::Unknown;
    KeyModifiers mods = KeyModifiers::None;
    bool IsValid() const { return key != Key::Unknown; }
    String ToString() const;
    static KeyChord Parse(StringView text); // "Ctrl+Shift+S", "F5", "Delete"
    friend bool operator==(const KeyChord&, const KeyChord&) = default;
};

struct EditorAction
{
    StringView id;
    StringView label;
    StringView category;
    StringView icon;
    KeyChord keys[2];        // current bindings
    KeyChord defaultKeys[2]; // built-in bindings
    bool repeat = false;     // fires on key auto-repeat (undo/redo)
    Function<void(Editor&), 16> run;
    Function<bool(Editor&), 16> enabled; // null = always
    String ShortcutText() const;
};

/// Builds the registry and applies Saved/Config/Keys.ini (written with the defaults if missing).
void RegisterEditorActions(Editor& ed);
const EditorAction* FindAction(Editor& ed, StringView id);
bool RunAction(Editor& ed, StringView id);
/// Dispatches a key press to the action bound to it. Returns true if one ran.
bool DispatchShortcut(Editor& ed, Key key, KeyModifiers mods, bool repeat);
/// A menu item for an action (label, icon, shortcut, enabled state).
void ActionMenuItem(Editor& ed, StringView id, bool selected = false);

void OpenCommandPalette(Editor& ed, bool commandsOnly);
void DrawCommandPalette(Editor& ed);

} // namespace zx::editor
