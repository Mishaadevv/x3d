#pragma once

// Localization: the game's texts in several languages, switchable while playing.
//
// One .zloc file per language in Content/Localization (and StarterContent/Localization); the file name is the
// language code (en.zloc, ru.zloc, de.zloc...). UTF-8 text:
//
//   # comment
//   @language: Русский            name shown in language menus
//   menu.resume = Продолжить      key = text  (\n = new line, {0} {1} = arguments)
//   hud.score = Очки: {0}
//
// UI Text has a Key field: with a key the widget shows the translation (its own Text is the fallback). Scripts:
// tr("hud.score", score), lang.current, lang.set("ru"), lang.list(), lang.name("ru"). The project's tables override
// StarterContent's; a missing key falls back to the default language (en), then to the key itself.
// The first language: the player's last choice (saved by hosts that give a path), else the system language if
// there is a table for it, else the default.

#include "Core/Containers/Array.h"
#include "Core/Containers/HashMap.h"
#include "Core/String/String.h"

namespace zx {

class Localization
{
public:
    struct Language
    {
        String code;     // "ru"
        String name;     // "Русский" (@language), or the code
        HashMap<String, String> texts;
        Array<String> files; // where it was read from (later files override earlier ones)
        Array<i64> modified;
    };

    /// Reads every .zloc in `<contentRoot>/StarterContent/Localization` and `<contentRoot>/Localization` (virtual or
    /// physical paths). Keeps the current language if it still exists.
    void Load(StringView contentRoot);
    /// Re-reads changed files (cheap; checks at most twice a second).
    void Poll();
    bool Empty() const { return m_languages.Empty(); }
    Span<const Language> Languages() const { return m_languages.View(); }
    const Language* Find(StringView code) const;

    /// Switches the language (false if there's no table for it). Saves the choice when a preference file is set.
    bool SetLanguage(StringView code);
    StringView Current() const { return m_current.View(); }
    /// Text for `key` in the current language, else the default language; null if neither has it.
    const String* Find(StringView key, StringView language) const;
    const String* Translate(StringView key) const;
    /// Translate + {0} {1} ... replaced by `args`; the key itself when it has no text anywhere.
    String Format(StringView key, Span<const String> args = {}) const;

    String defaultLanguage = "en";
    /// Where the player's choice is remembered (empty = not saved, e.g. while editing).
    String preferenceFile;
    /// Bumped whenever texts or the language change (widgets re-resolve their keys).
    u32 Version() const { return m_version; }

    // Editing (the editor's Localization window).
    /// Sets one text (creates the language when needed; an empty text removes it from that language).
    void SetText(StringView code, StringView key, StringView text);
    void RemoveKey(StringView key);
    bool AddLanguage(StringView code, StringView name);
    /// Every key of every language, sorted.
    Array<String> AllKeys() const;
    /// Writes every language into `<contentRoot>/Localization/<code>.zloc` (the project's tables).
    bool Save(StringView contentRoot);

    /// Writes a language table (sorted keys) - the editor's Localization window.
    static bool WriteTable(StringView path, StringView languageName, const HashMap<String, String>& texts);
    /// Parses a .zloc text into `out` (keys / texts) and the @language name. Returns the number of entries.
    static u32 Parse(StringView text, HashMap<String, String>& out, String* languageName);
    /// "RU" -> "ru".
    static String NormalizeCode(StringView code);

private:
    Array<Language> m_languages;
    String m_contentRoot;
    String m_current;
    u32 m_version = 1;
    f64 m_lastPoll = 0.0;
};

} // namespace zx
