#pragma once

// Console variables (CVars) and console commands.
//
//   static CVar<i32> CVarShadowQuality("r.Shadow.Quality", 2, "Shadow quality 0..3", CVarFlags::Scalability);
//   if (CVarShadowQuality.Get() > 1) ...
//
//   static ConCommand CmdQuit("quit", "Exits the application", [](Span<const StringView> args) { ... });
//
//   Console::Execute("r.Shadow.Quality 3");
//
// Names are case-insensitive. Scalar CVars may be read from any thread (benign races); String CVars and all
// writes are main-thread only. A value set with a lower priority (CVarSetBy) than the current one is ignored.

#include "Core/Containers/Function.h"
#include "Core/String/String.h"

namespace zx {

enum class CVarFlags : u32
{
    None = 0,
    ReadOnly = 1 << 0,    // can't be changed from console/config
    Cheat = 1 << 1,       // disabled in Shipping
    Archive = 1 << 2,     // saved to user config
    Scalability = 1 << 3, // part of quality presets
    Hidden = 1 << 4,      // not listed in auto-complete
};
ZX_ENUM_FLAGS(CVarFlags)

enum class CVarSetBy : u8
{
    Default,
    Scalability,
    ProjectConfig,
    UserConfig,
    CommandLine,
    Code,
    Console,
};

enum class CVarType : u8
{
    Bool,
    Int,
    Float,
    String
};

class ConsoleObject
{
public:
    ConsoleObject(const char* name, const char* help);
    virtual ~ConsoleObject() = default;
    ZX_NON_COPYABLE(ConsoleObject)

    const char* GetName() const { return m_name; }
    const char* GetHelp() const { return m_help; }
    virtual bool IsCommand() const = 0;
    ConsoleObject* NextObject() const { return m_next; }

private:
    const char* m_name;
    const char* m_help;
    ConsoleObject* m_next;
};

class CVarBase : public ConsoleObject
{
public:
    CVarBase(const char* name, const char* help, CVarType type, CVarFlags flags)
        : ConsoleObject(name, help), m_type(type), m_flags(flags)
    {
    }
    bool IsCommand() const override { return false; }

    CVarType GetType() const { return m_type; }
    CVarFlags GetFlags() const { return m_flags; }
    CVarSetBy GetSetBy() const { return m_setBy; }

    virtual String GetValueString() const = 0;
    virtual String GetDefaultString() const = 0;
    /// Parses and sets. Returns false on parse error, read-only or lower priority.
    virtual bool SetFromString(StringView value, CVarSetBy setBy = CVarSetBy::Console) = 0;
    virtual void ResetToDefault() = 0;

    /// Called after the value changes.
    void SetOnChanged(Function<void(CVarBase&)> callback) { m_onChanged = move(callback); }

protected:
    bool CanSet(CVarSetBy setBy) const;
    void NotifyChanged(CVarSetBy setBy);

    CVarType m_type;
    CVarFlags m_flags;
    CVarSetBy m_setBy = CVarSetBy::Default;
    Function<void(CVarBase&)> m_onChanged;
};

namespace detail {
template <typename T>
constexpr CVarType CVarTypeOf()
{
    if constexpr (std::is_same_v<T, bool>)
        return CVarType::Bool;
    else if constexpr (std::is_integral_v<T>)
        return CVarType::Int;
    else if constexpr (std::is_floating_point_v<T>)
        return CVarType::Float;
    else
        return CVarType::String;
}
bool ParseCVarValue(StringView text, bool& out);
bool ParseCVarValue(StringView text, i32& out);
bool ParseCVarValue(StringView text, f32& out);
bool ParseCVarValue(StringView text, String& out);
String CVarValueToString(bool v);
String CVarValueToString(i32 v);
String CVarValueToString(f32 v);
String CVarValueToString(const String& v);
} // namespace detail

/// T is one of bool, i32, f32, String.
template <typename T>
class CVar final : public CVarBase
{
    static_assert(std::is_same_v<T, bool> || std::is_same_v<T, i32> || std::is_same_v<T, f32> ||
                      std::is_same_v<T, String>,
                  "CVar<T>: T must be bool, i32, f32 or String");

public:
    using ArgType = std::conditional_t<std::is_same_v<T, String>, StringView, T>;

    CVar(const char* name, ArgType defaultValue, const char* help, CVarFlags flags = CVarFlags::None)
        : CVarBase(name, help, detail::CVarTypeOf<T>(), flags), m_value(defaultValue), m_default(defaultValue)
    {
    }

    const T& Get() const { return m_value; }
    operator const T&() const { return m_value; }

    bool Set(ArgType value, CVarSetBy setBy = CVarSetBy::Code)
    {
        if (!CanSet(setBy))
            return false;
        const bool changed = !(m_value == T(value));
        m_value = T(value);
        if (changed)
            NotifyChanged(setBy);
        else
            m_setBy = setBy;
        return true;
    }

    String GetValueString() const override { return detail::CVarValueToString(m_value); }
    String GetDefaultString() const override { return detail::CVarValueToString(m_default); }
    bool SetFromString(StringView text, CVarSetBy setBy = CVarSetBy::Console) override
    {
        T parsed{};
        if (!detail::ParseCVarValue(text, parsed))
            return false;
        if constexpr (std::is_same_v<T, String>)
            return Set(parsed.View(), setBy);
        else
            return Set(parsed, setBy);
    }
    void ResetToDefault() override
    {
        m_setBy = CVarSetBy::Default;
        const bool changed = !(m_value == m_default);
        m_value = m_default;
        if (changed)
            NotifyChanged(CVarSetBy::Default);
    }

private:
    T m_value;
    T m_default;
};

class ConCommand final : public ConsoleObject
{
public:
    using Handler = Function<void(Span<const StringView> args)>;
    ConCommand(const char* name, const char* help, Handler handler)
        : ConsoleObject(name, help), m_handler(move(handler))
    {
    }
    bool IsCommand() const override { return true; }
    void Invoke(Span<const StringView> args) const { m_handler(args); }

private:
    Handler m_handler;
};

namespace Console {

ConsoleObject* FindObject(StringView name);
CVarBase* FindCVar(StringView name);
ConCommand* FindCommand(StringView name);
/// All registered objects (linked list).
ConsoleObject* GetObjects();

/// Executes "name", "name value" or "command args...". Output (value printouts, errors) is logged.
/// Multiple statements may be separated by ';'. Returns false if any statement failed.
bool Execute(StringView line, CVarSetBy setBy = CVarSetBy::Console);

/// Names starting with prefix (case-insensitive), sorted.
Array<const ConsoleObject*> AutoComplete(StringView prefix, usize maxResults = 32);

/// Splits a command line respecting double quotes.
Array<StringView> Tokenize(StringView line);

} // namespace Console

/// Process command line access.
///   -flag            HasFlag("flag")
///   -key=value       GetValue("key") -> "value"
///   +r.Cvar=value    applied by ApplyCVarOverrides()
///   other tokens     positional
namespace CommandLine {

void Initialize(StringView rawCommandLine);
StringView Get();
bool HasFlag(StringView name);
/// Value of -name=value (or empty optional view if absent). Returns false if absent.
bool GetValue(StringView name, StringView& out);
Span<const StringView> GetPositional();
/// Applies "+name=value" tokens to CVars (setBy = CommandLine). Returns count applied.
u32 ApplyCVarOverrides();

} // namespace CommandLine

} // namespace zx
