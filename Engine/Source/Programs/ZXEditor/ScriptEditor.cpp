// Script Editor window and ZeqouScript syntax colors.

#include "ScriptEditor.h"
#include "Editor.h"
#include "Core/FileSystem/FileSystem.h"
#include "Core/HAL/PlatformFile.h"
#include "Core/String/Path.h"
#include "Platform/Platform.h"

namespace zx::editor {

ZX_DEFINE_LOG_CATEGORY_STATIC(LogScriptEditor, Info);

namespace {

// VS Code "Dark+"-like palette.
constexpr Color ColText(0xD4, 0xD4, 0xD4);
constexpr Color ColKeyword(0x56, 0x9C, 0xD6);  // declarations, literals
constexpr Color ColControl(0xC5, 0x86, 0xC0);  // control flow
constexpr Color ColFunction(0xDC, 0xDC, 0xAA);
constexpr Color ColGlobal(0x4E, 0xC9, 0xB0);   // engine objects / types
constexpr Color ColNumber(0xB5, 0xCE, 0xA8);
constexpr Color ColString(0xCE, 0x91, 0x78);
constexpr Color ColComment(0x6A, 0x99, 0x55);
constexpr Color ColAnnotation(0xD7, 0xBA, 0x7D);

bool IsIdentStart(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
bool IsIdentChar(char c) { return IsIdentStart(c) || (c >= '0' && c <= '9'); }

bool OneOf(StringView word, std::initializer_list<StringView> list)
{
    for (StringView w : list)
        if (w == word)
            return true;
    return false;
}

i64 FileTime(StringView path)
{
    platform::FileInfo info;
    return platform::GetFileInfo(path, info) ? info.modifiedTime : 0;
}

void Save(Editor& ed, ScriptEditorState& st)
{
    if (!fs::WriteTextFile(st.path, st.text)) {
        ZX_LOG_ERROR(LogScriptEditor, "Could not save {}", st.path);
        return;
    }
    st.saved = st.text;
    st.diskTime = FileTime(st.path);
    if (ed.scripts)
        ed.scripts->CheckForChanges(); // reloads running instances while playing
    ZX_LOG_INFO(LogScriptEditor, "Saved {}", Path::FileName(st.path));
}

void Check(ScriptEditorState& st)
{
    st.checkedText = st.text;
    script::CompileError e;
    Result<script::Program*> p = script::Compile(st.text, Path::FileName(st.path), &e);
    if (p) {
        script::DestroyProgram(*p);
        st.error.Clear();
        st.errorLine = 0;
    } else {
        st.error = e.message;
        st.errorLine = e.line;
    }
}

/// Quick reference shown by the "?" button.
void DrawReference()
{
    struct Entry
    {
        const char* code;
        const char* text;
    };
    static const Entry Language[] = {
        {"script Name { ... }", "one script per file"},
        {"@range(0, 10) @tooltip(\"...\") prop speed = 1.0", "editable in the Details panel, saved per entity"},
        {"@color prop tint = vec3(1, 0.5, 0)", "vec3 edited with a color picker"},
        {"var count = 0  /  let x = 1", "instance field / local variable"},
        {"fn start() { }  fn update(dt) { }", "called by the engine while playing"},
        {"if a { } elif b { } else { }", "also: and, or, not, ==, !=, <, <="},
        {"for i in 0..10 { }  for item in list { }", "while cond { }, break, continue, return"},
        {"[1, 2, 3]  list.push(x)  list.len()", "lists; strings join with +"},
    };
    static const Entry Engine[] = {
        {"entity.position / rotation / scale", "this entity (also: transform.*), forward/right/up"},
        {"entity.rotate(vec3(0, 90 * dt, 0))", "turn by euler degrees in local space"},
        {"entity.translate(vec3(0, 0, 1) * dt)", "move along its own axes (true = world axes)"},
        {"entity.lookAt(target)  entity.destroy()", "entity.name, find(\"Name\")"},
        {"input.move  input.look", "WASD/arrows/stick as vec3(x, y, z); mouse/stick delta"},
        {"input.key(\"Space\")  input.pressed(\"E\")", "held / pressed this frame; input.mouse(\"left\")"},
        {"time.dt  time.elapsed  time.frame", "seconds"},
        {"vec3(x, y, z)  quat.euler(p, y, r)", "v.length(), v.normalized(), v.dot(b), lerp(a, b, t)"},
        {"sin cos sqrt abs min max clamp random", "math (radians); deg(), rad(), PI"},
        {"log(\"value:\", x)", "prints to the Output Log"},
    };
    auto table = [](StringView title, Span<const Entry> entries) {
        ui::PushFont(ui::FontStyle::SemiBold);
        ui::Text(title);
        ui::PopFont();
        for (const Entry& e : entries) {
            ui::TextColored(ColFunction, e.code);
            ui::SameLine(330.0f);
            ui::TextDisabled(e.text);
        }
    };
    table("Language", Span<const Entry>(Language, ZX_ARRAY_COUNT(Language)));
    ui::Spacing();
    table("Engine", Span<const Entry>(Engine, ZX_ARRAY_COUNT(Engine)));
    ui::Spacing();
    ui::TextDisabled("Full reference: docs/ZeqouScript.md");
}

} // namespace

void ColorizeZeqouScript(StringView line, Color* colors, u32& state)
{
    usize i = 0;
    const usize n = line.Size();
    auto fill = [&](usize from, usize to, Color c) {
        for (usize k = from; k < to && k < n; ++k)
            colors[k] = c;
    };
    fill(0, n, ColText);
    while (i < n) {
        if (state == 1) { // inside /* */
            const usize start = i;
            while (i < n && !(line[i] == '*' && i + 1 < n && line[i + 1] == '/'))
                ++i;
            if (i < n) {
                i += 2;
                state = 0;
            }
            fill(start, i, ColComment);
            continue;
        }
        const char c = line[i];
        if (c == '/' && i + 1 < n && line[i + 1] == '/') {
            fill(i, n, ColComment);
            return;
        }
        if (c == '/' && i + 1 < n && line[i + 1] == '*') {
            state = 1;
            fill(i, i + 2, ColComment);
            i += 2;
            continue;
        }
        if (c == '"') {
            const usize start = i++;
            while (i < n && line[i] != '"') {
                if (line[i] == '\\')
                    ++i;
                ++i;
            }
            i = Min(i + 1, n);
            fill(start, i, ColString);
            continue;
        }
        if ((c >= '0' && c <= '9') || (c == '.' && i + 1 < n && line[i + 1] >= '0' && line[i + 1] <= '9')) {
            const usize start = i;
            while (i < n && (IsIdentChar(line[i]) || (line[i] == '.' && !(i + 1 < n && line[i + 1] == '.'))))
                ++i;
            fill(start, i, ColNumber);
            continue;
        }
        if (c == '@') {
            const usize start = i++;
            while (i < n && IsIdentChar(line[i]))
                ++i;
            fill(start, i, ColAnnotation);
            continue;
        }
        if (IsIdentStart(c)) {
            const usize start = i;
            while (i < n && IsIdentChar(line[i]))
                ++i;
            const StringView word = line.SubStr(start, i - start);
            usize next = i;
            while (next < n && line[next] == ' ')
                ++next;
            const bool call = next < n && line[next] == '(';
            const bool member = start > 0 && line[start - 1] == '.';
            Color col = ColText;
            if (!member && OneOf(word, {"script", "prop", "var", "let", "fn", "self", "true", "false", "nil"}))
                col = ColKeyword;
            else if (!member && OneOf(word, {"if", "elif", "else", "while", "for", "in", "break", "continue", "return", "and", "or", "not"}))
                col = ColControl;
            else if (call)
                col = ColFunction;
            else if (!member && OneOf(word, {"entity", "transform", "input", "time", "vec3", "quat", "PI"}))
                col = ColGlobal;
            fill(start, i, col);
            continue;
        }
        ++i;
    }
}

bool CreateScriptFile(StringView physicalPath)
{
    return fs::WriteTextFile(physicalPath, ScriptTemplate(Path::Stem(physicalPath)));
}

void OpenScriptEditor(Editor& ed, StringView physicalPath, u32 line)
{
    ScriptEditorState& st = ed.scriptEditor;
    if (st.open && st.path != physicalPath && st.text != st.saved)
        Save(ed, st); // switching files keeps the edits
    if (st.path != physicalPath || !st.open) {
        String text;
        if (!fs::ReadTextFile(physicalPath, text)) {
            ZX_LOG_ERROR(LogScriptEditor, "Could not open {}", physicalPath);
            return;
        }
        text.ReplaceAll("\r\n", "\n");
        st.path = physicalPath;
        st.text = text;
        st.saved = move(text);
        st.diskTime = FileTime(physicalPath);
        Check(st);
    }
    st.open = true;
    st.confirmClose = false;
    st.scrollToLine = line ? line : st.errorLine;
    ui::FocusDockedWindow(ScriptEditorTitle);
}

void DrawScriptEditorPanel(Editor& ed)
{
    ScriptEditorState& st = ed.scriptEditor;
    st.capturesKeys = false;
    if (!st.open)
        return;
    ui::Context& ctx = *ui::GetCurrentContext();
    if (!st.monoLoaded) {
        st.monoLoaded = true;
        const StringView faces[] = {"C:/Windows/Fonts/consola.ttf"};
        st.mono = platform::FileExists(faces[0]) ? ctx.Fonts().AddFont("Mono", Span<const StringView>(faces, 1)) : ui::FontHandle{};
        if (!st.mono.IsValid())
            st.mono = ctx.Fonts().FindFont("Inter");
    }
    {
        const f32 dpi = ctx.GetScale();
        const Vec2 display = Vec2(f32(ed.swapChain->GetSize().x), f32(ed.swapChain->GetSize().y)) / dpi;
        const Vec2 size = Vec2(Min(display.x * 0.6f, 900.0f), display.y * 0.8f);
        ui::SetNextWindowSize(size, ui::Cond::FirstUse);
        ui::SetNextWindowPos((display - size) * 0.5f, ui::Cond::FirstUse);
    }
    bool open = true;
    if (!ui::Begin(ScriptEditorTitle, &open, ui::WindowFlags::NoScrollbar)) {
        ui::End();
        if (!open)
            st.open = false;
        return;
    }
    const bool dirty = st.text != st.saved;
    if (!open) {
        if (dirty)
            st.confirmClose = true;
        else
            st.open = false;
    }

    // Reload when the file changed outside (and there are no local edits).
    const i64 diskTime = FileTime(st.path);
    if (diskTime != st.diskTime && !dirty) {
        String text;
        if (fs::ReadTextFile(st.path, text)) {
            text.ReplaceAll("\r\n", "\n");
            st.text = text;
            st.saved = move(text);
        }
        st.diskTime = diskTime;
    }
    // Errors are checked shortly after typing stops.
    const f64 now = ed.timer.TotalSeconds();
    if (st.text != st.checkedText && now - st.lastEditTime > 0.25)
        Check(st);

    // --- Toolbar ---
    if (ui::Button(ZX_ICON_SAVE "  Save", Vec2::Zero(), dirty ? ui::ButtonVariant::Primary : ui::ButtonVariant::Default))
        Save(ed, st);
    ui::ItemTooltip("Save (Ctrl+S); running scripts reload immediately");
    ui::SameLine();
    ui::BeginDisabled(!dirty);
    if (ui::IconButton(ZX_ICON_UNDO_2, "Revert to the saved file"))
        st.text = st.saved;
    ui::EndDisabled();
    ui::SameLine();
    ui::AlignTextToFramePadding();
    ui::PushFont(ui::FontStyle::SemiBold);
    ui::Text(String::Format("{}{}", Path::FileName(st.path), dirty ? " *" : ""));
    ui::PopFont();
    ui::ItemTooltip(st.path);
    ui::SameLine(0.0f, 16.0f);
    if (st.error.Empty()) {
        ui::TextColored(Color(0x6C, 0xC8, 0x6A), ZX_ICON_CIRCLE_CHECK "  No errors");
    } else {
        ui::PushTextColor(Color(0xF0, 0x60, 0x60));
        if (ui::Button(String::Format(ZX_ICON_CIRCLE_X "  Line {}: {}##err", st.errorLine, st.error), Vec2::Zero(), ui::ButtonVariant::Ghost))
            st.scrollToLine = st.errorLine;
        ui::PopTextColor();
        ui::ItemTooltip("Go to the error");
    }
    const f32 s = ctx.GetScale();
    const f32 right = ui::GetWindowContentRect().Width() / s;
    ui::SameLine(Max(0.0f, right - 70.0f));
    if (ui::IconButton(ZX_ICON_EXTERNAL_LINK, "Open in the default external editor"))
        platform::OpenUrl(st.path);
    ui::SameLine(0.0f, 4.0f);
    if (ui::IconButton(ZX_ICON_CIRCLE_HELP, "ZeqouScript quick reference"))
        ui::OpenPopup("##zs_reference");
    if (ui::BeginPopup("##zs_reference")) {
        DrawReference();
        ui::EndPopup();
    }

    // --- Code ---
    ui::CodeEditorOptions options;
    options.colorizer = &ColorizeZeqouScript;
    options.errorLine = st.errorLine;
    options.scrollToLine = st.scrollToLine;
    st.scrollToLine = 0;
    ui::PushFont(st.mono, 13.5f);
    if (ui::InputTextCode("##code", st.text, Vec2::Zero(), options))
        st.lastEditTime = now;
    const bool active = ui::IsItemActive();
    ui::PopFont();
    st.capturesKeys = active;
    if (ui::IsShortcutPressed(KeyModifiers::Control, Key::S) && (active || ui::IsWindowHovered()))
        Save(ed, st);

    // Closing with unsaved edits.
    if (st.confirmClose) {
        ui::OpenPopup("Unsaved Script##zs_close");
        st.confirmClose = false;
    }
    if (ui::BeginPopupModal("Unsaved Script##zs_close")) {
        ui::Text(String::Format("Save changes to {}?", Path::FileName(st.path)));
        ui::Spacing();
        if (ui::Button("Save", Vec2(90.0f, 0.0f), ui::ButtonVariant::Primary)) {
            Save(ed, st);
            st.open = false;
            ui::CloseCurrentPopup();
        }
        ui::SameLine();
        if (ui::Button("Don't Save", Vec2(90.0f, 0.0f))) {
            st.text = st.saved;
            st.open = false;
            ui::CloseCurrentPopup();
        }
        ui::SameLine();
        if (ui::Button("Cancel", Vec2(90.0f, 0.0f)))
            ui::CloseCurrentPopup();
        ui::EndPopup();
    }
    ui::End();
}

} // namespace zx::editor
