#include "Core/Console/Console.h"
#include "Core/Containers/Algorithm.h"
#include "Core/Log/Log.h"
#include "Core/String/Parse.h"

ZX_DEFINE_LOG_CATEGORY_STATIC(LogConsole, Info);

namespace zx {

namespace {
constinit ConsoleObject* g_objectsHead = nullptr;
} // namespace

ConsoleObject::ConsoleObject(const char* name, const char* help) : m_name(name), m_help(help), m_next(g_objectsHead)
{
    g_objectsHead = this;
}

bool CVarBase::CanSet(CVarSetBy setBy) const
{
    if (HasAnyFlags(m_flags, CVarFlags::ReadOnly) && setBy != CVarSetBy::Code && setBy != CVarSetBy::Default)
        return false;
#if ZX_BUILD_SHIPPING
    if (HasAnyFlags(m_flags, CVarFlags::Cheat) && setBy == CVarSetBy::Console)
        return false;
#endif
    // Console and Code always win; otherwise a lower-priority source can't override a higher one.
    if (setBy == CVarSetBy::Console || setBy == CVarSetBy::Code)
        return true;
    return u8(setBy) >= u8(m_setBy);
}

void CVarBase::NotifyChanged(CVarSetBy setBy)
{
    m_setBy = setBy;
    if (m_onChanged)
        m_onChanged(*this);
}

namespace detail {

bool ParseCVarValue(StringView text, bool& out)
{
    if (auto v = ParseBool(text)) {
        out = *v;
        return true;
    }
    return false;
}
bool ParseCVarValue(StringView text, i32& out)
{
    if (auto v = ParseInt(text); v && *v >= I32Min && *v <= I32Max) {
        out = i32(*v);
        return true;
    }
    return false;
}
bool ParseCVarValue(StringView text, f32& out)
{
    if (auto v = ParseFloat(text)) {
        out = *v;
        return true;
    }
    return false;
}
bool ParseCVarValue(StringView text, String& out)
{
    text = text.Trim();
    if (text.Size() >= 2 && text.Front() == '"' && text.Back() == '"')
        text = text.SubStr(1, text.Size() - 2);
    out = text;
    return true;
}
String CVarValueToString(bool v) { return String(v ? "1" : "0"); }
String CVarValueToString(i32 v) { return String::Format("{}", v); }
String CVarValueToString(f32 v) { return String::Format("{}", v); }
String CVarValueToString(const String& v) { return v; }

} // namespace detail

namespace Console {

ConsoleObject* GetObjects() { return g_objectsHead; }

ConsoleObject* FindObject(StringView name)
{
    for (ConsoleObject* o = g_objectsHead; o; o = o->NextObject())
        if (name.EqualsIgnoreCase(o->GetName()))
            return o;
    return nullptr;
}

CVarBase* FindCVar(StringView name)
{
    ConsoleObject* o = FindObject(name);
    return (o && !o->IsCommand()) ? static_cast<CVarBase*>(o) : nullptr;
}

ConCommand* FindCommand(StringView name)
{
    ConsoleObject* o = FindObject(name);
    return (o && o->IsCommand()) ? static_cast<ConCommand*>(o) : nullptr;
}

Array<StringView> Tokenize(StringView line)
{
    Array<StringView> tokens;
    usize i = 0;
    while (i < line.Size()) {
        while (i < line.Size() && IsSpaceAscii(line[i]))
            ++i;
        if (i >= line.Size())
            break;
        if (line[i] == '"') {
            const usize start = ++i;
            while (i < line.Size() && line[i] != '"')
                ++i;
            tokens.PushBack(line.SubStr(start, i - start));
            if (i < line.Size())
                ++i;
        } else {
            const usize start = i;
            while (i < line.Size() && !IsSpaceAscii(line[i]))
                ++i;
            tokens.PushBack(line.SubStr(start, i - start));
        }
    }
    return tokens;
}

namespace {
bool ExecuteStatement(StringView statement, CVarSetBy setBy)
{
    const Array<StringView> tokens = Tokenize(statement);
    if (tokens.Empty())
        return true;
    StringView name = tokens[0];
    StringView inlineValue;
    bool hasInlineValue = false;
    // Allow "name=value".
    if (const usize eq = name.Find('='); eq != StringView::NPos) {
        inlineValue = name.SubStr(eq + 1);
        name = name.Left(eq);
        hasInlineValue = true;
    }
    ConsoleObject* obj = FindObject(name);
    if (!obj) {
        ZX_LOG_WARN(LogConsole, "Unknown command or variable '{}'", name);
        return false;
    }
    if (obj->IsCommand()) {
        static_cast<ConCommand*>(obj)->Invoke(Span<const StringView>(tokens.Data() + 1, tokens.Size() - 1));
        return true;
    }
    CVarBase* cvar = static_cast<CVarBase*>(obj);
    if (!hasInlineValue && tokens.Size() < 2) {
        ZX_LOG_INFO(LogConsole, "{} = {}  (default {}){}{}", cvar->GetName(), cvar->GetValueString(),
                    cvar->GetDefaultString(), *cvar->GetHelp() ? "  - " : "", cvar->GetHelp());
        return true;
    }
    const StringView value = hasInlineValue ? inlineValue : tokens[1];
    if (!cvar->SetFromString(value, setBy)) {
        ZX_LOG_WARN(LogConsole, "Can't set {} to '{}'", cvar->GetName(), value);
        return false;
    }
    ZX_LOG_INFO(LogConsole, "{} = {}", cvar->GetName(), cvar->GetValueString());
    return true;
}
} // namespace

bool Execute(StringView line, CVarSetBy setBy)
{
    bool ok = true;
    for (StringView statement : SplitString(line, ';', true))
        ok &= ExecuteStatement(statement.Trim(), setBy);
    return ok;
}

Array<const ConsoleObject*> AutoComplete(StringView prefix, usize maxResults)
{
    Array<const ConsoleObject*> out;
    for (ConsoleObject* o = g_objectsHead; o; o = o->NextObject()) {
        if (!o->IsCommand() && HasAnyFlags(static_cast<CVarBase*>(o)->GetFlags(), CVarFlags::Hidden))
            continue;
        const StringView name(o->GetName());
        if (name.Size() >= prefix.Size() && name.Left(prefix.Size()).EqualsIgnoreCase(prefix))
            out.PushBack(o);
    }
    Sort(out, [](const ConsoleObject* a, const ConsoleObject* b) {
        return StringView(a->GetName()).CompareIgnoreCase(b->GetName()) < 0;
    });
    if (out.Size() > maxResults)
        out.Resize(maxResults);
    return out;
}

namespace {

// Built-in: lists the registered variables/commands (hidden ones only when asked for by prefix).
ConCommand CmdHelp("help", "Lists console variables and commands: help [prefix]", [](Span<const StringView> args) {
    const StringView prefix = args.Empty() ? StringView() : args[0];
    const Array<const ConsoleObject*> found = AutoComplete(prefix, 512);
    if (found.Empty()) {
        ZX_LOG_INFO(LogConsole, "Nothing matches '{}'", prefix);
        return;
    }
    for (const ConsoleObject* o : found) {
        if (o->IsCommand())
            ZX_LOG_INFO(LogConsole, "  {}  -  {}", o->GetName(), o->GetHelp());
        else
            ZX_LOG_INFO(LogConsole, "  {} = {}  -  {}", o->GetName(), static_cast<const CVarBase*>(o)->GetValueString(), o->GetHelp());
    }
});

} // namespace

} // namespace Console

namespace CommandLine {

namespace {
struct CommandLineState
{
    String raw;
    Array<StringView> tokens;
    Array<StringView> positional;
};
CommandLineState& CL()
{
    static CommandLineState s;
    return s;
}
} // namespace

void Initialize(StringView rawCommandLine)
{
    LeakIgnoreScope permanent; // process-lifetime state
    CommandLineState& s = CL();
    s.raw = rawCommandLine;
    s.tokens = Console::Tokenize(s.raw);
    s.positional.Clear();
    for (StringView t : s.tokens)
        if (!t.StartsWith('-') && !t.StartsWith('+'))
            s.positional.PushBack(t);
}

StringView Get() { return CL().raw; }

bool HasFlag(StringView name)
{
    for (StringView t : CL().tokens)
        if (t.StartsWith('-') && t.DropLeft(1).EqualsIgnoreCase(name))
            return true;
    return false;
}

bool GetValue(StringView name, StringView& out)
{
    for (StringView t : CL().tokens) {
        if (!t.StartsWith('-'))
            continue;
        const usize eq = t.Find('=');
        if (eq == StringView::NPos)
            continue;
        if (t.SubStr(1, eq - 1).EqualsIgnoreCase(name)) {
            out = t.SubStr(eq + 1);
            return true;
        }
    }
    return false;
}

Span<const StringView> GetPositional() { return Span<const StringView>(CL().positional.Data(), CL().positional.Size()); }

u32 ApplyCVarOverrides()
{
    u32 applied = 0;
    for (StringView t : CL().tokens) {
        if (!t.StartsWith('+'))
            continue;
        StringView name, value;
        if (!t.DropLeft(1).Split('=', name, value))
            continue;
        if (CVarBase* cvar = Console::FindCVar(name)) {
            if (cvar->SetFromString(value, CVarSetBy::CommandLine))
                ++applied;
        } else {
            ZX_LOG_WARN(LogConsole, "Command line: unknown CVar '{}'", name);
        }
    }
    return applied;
}

} // namespace CommandLine

} // namespace zx
