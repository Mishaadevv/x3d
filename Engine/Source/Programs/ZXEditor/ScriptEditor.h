#pragma once

// Script Editor: built-in editor for ZeqouScript files (.zs) - syntax colors, errors checked while typing, Ctrl+S saves
// (running scripts reload at once while playing).

#include "UI/UI.h"

namespace zx::editor {

class Editor;

struct ScriptEditorState
{
    bool open = false;
    String path;  // physical
    String text;  // being edited
    String saved; // last saved / loaded
    i64 diskTime = 0;
    // Live check (compiled a moment after typing stops).
    String checkedText;
    String error;
    u32 errorLine = 0;
    f64 lastEditTime = 0.0;
    u32 scrollToLine = 0;
    bool confirmClose = false;
    bool capturesKeys = false; // the code field has the keyboard (scene shortcuts are suspended)
    ui::FontHandle mono;
    bool monoLoaded = false;
};

inline constexpr StringView ScriptEditorTitle = ZX_ICON_FILE_CODE "  Script Editor";

/// Opens a .zs file (physical path), optionally scrolled to a 1-based line.
void OpenScriptEditor(Editor& ed, StringView physicalPath, u32 line = 0);
void DrawScriptEditorPanel(Editor& ed);
/// Writes a starter script (named after the file) and returns true on success.
bool CreateScriptFile(StringView physicalPath);
/// Syntax colors for ZeqouScript (ui::CodeColorizer).
void ColorizeZeqouScript(StringView line, Color* colors, u32& state);

} // namespace zx::editor
