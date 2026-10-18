#include "Engine/Localization.h"
#include "Core/Containers/Algorithm.h"
#include "Core/FileSystem/FileSystem.h"
#include "Core/HAL/PlatformMisc.h"
#include "Core/Log/Log.h"
#include "Core/String/Path.h"
#include "Core/Time/Time.h"

ZX_DEFINE_LOG_CATEGORY_STATIC(LogLocalization, Info);

namespace zx {

u32 Localization::Parse(StringView text, HashMap<String, String>& out, String* languageName)
{
    if (text.StartsWith("\xEF\xBB\xBF"))
        text = text.DropLeft(3);
    u32 count = 0;
    StringView rest = text;
    while (!rest.Empty()) {
        StringView line, next;
        if (!rest.Split('\n', line, next))
            next = {};
        rest = next;
        line = line.Trim();
        if (line.Empty() || line.StartsWith("#") || line.StartsWith("//"))
            continue;
        if (line.StartsWith("@")) {
            StringView name, value;
            if (line.DropLeft(1).Split(':', name, value) && name.Trim().EqualsIgnoreCase("language") && languageName)
                *languageName = String(value.Trim());
            continue;
        }
        StringView key, value;
        if (!line.Split('=', key, value))
            continue;
        key = key.Trim();
        if (key.Empty())
            continue;
        // Escapes: \n new line, \\ backslash.
        String v;
        const StringView raw = value.Trim();
        for (usize i = 0; i < raw.Size(); ++i) {
            if (raw[i] == '\\' && i + 1 < raw.Size()) {
                const char c = raw[++i];
                v += c == 'n' ? '\n' : c == 't' ? '\t' : c;
            } else {
                v += raw[i];
            }
        }
        out.InsertOrAssign(String(key), move(v));
        ++count;
    }
    return count;
}

bool Localization::WriteTable(StringView path, StringView languageName, const HashMap<String, String>& texts)
{
    Array<String> keys;
    for (const auto& [k, v] : texts)
        keys.PushBack(k);
    Sort(keys, [](const String& a, const String& b) { return a.View() < b.View(); });
    String out = String::Format("# ZeqouX3D localization ({}): key = text   (\\n = new line, {{0}} = an argument)\n@language: {}\n\n",
                                Path::Stem(path), languageName);
    for (const String& k : keys) {
        String v;
        for (char c : *texts.Find(k)) {
            if (c == '\n')
                v += "\\n";
            else if (c == '\\')
                v += "\\\\";
            else
                v += c;
        }
        out += String::Format("{} = {}\n", k, v);
    }
    return fs::WriteTextFile(path, out);
}

void Localization::Load(StringView contentRoot)
{
    m_contentRoot = contentRoot;
    m_languages.Clear();
    // Starter Content first: the project's own tables override it.
    for (StringView dir : {StringView("StarterContent/Localization"), StringView("Localization")}) {
        const String folder = Path::Join(contentRoot, dir);
        Array<platform::DirectoryEntry> entries;
        fs::ListOptions opt;
        opt.includeDirectories = false;
        opt.extension = ".zloc";
        if (!fs::IsDirectory(folder) || !fs::List(folder, entries, opt))
            continue;
        Sort(entries, [](const platform::DirectoryEntry& a, const platform::DirectoryEntry& b) { return a.name.View() < b.name.View(); });
        for (const platform::DirectoryEntry& e : entries) {
            const String path = Path::Join(folder, e.name);
            String text;
            if (!fs::ReadTextFile(path, text))
                continue;
            const String code = NormalizeCode(Path::Stem(e.name));
            Language* lang = nullptr;
            for (Language& l : m_languages)
                if (l.code == code)
                    lang = &l;
            if (!lang) {
                m_languages.PushBack(Language{code, code, {}, {}, {}});
                lang = &m_languages.Back();
            }
            String name;
            const u32 n = Parse(text.View(), lang->texts, &name);
            if (!name.Empty())
                lang->name = name;
            platform::FileInfo info;
            lang->files.PushBack(path);
            lang->modified.PushBack(fs::GetInfo(path, info) ? info.modifiedTime : 0);
            ZX_LOG_INFO(LogLocalization, "{}: {} texts ({})", path, n, lang->name);
        }
    }
    // The language: kept, else the saved choice, else the system's, else the default, else the first table.
    String want = m_current;
    if (want.Empty() || !Find(want)) {
        want.Clear();
        String saved;
        if (!preferenceFile.Empty() && fs::ReadTextFile(preferenceFile, saved) && Find(saved.View().Trim()))
            want = String(saved.View().Trim());
        else if (Find(platform::GetUserLanguage()))
            want = platform::GetUserLanguage();
        else if (Find(defaultLanguage))
            want = defaultLanguage;
        else if (!m_languages.Empty())
            want = m_languages[0].code;
    }
    m_current = want;
    ++m_version;
    m_lastPoll = Clock::NowSeconds();
}

String Localization::NormalizeCode(StringView code)
{
    String out;
    for (char c : code)
        out += (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c;
    return out;
}

void Localization::Poll()
{
    const f64 now = Clock::NowSeconds();
    if (m_contentRoot.Empty() || now - m_lastPoll < 0.5)
        return;
    m_lastPoll = now;
    bool changed = false;
    for (const Language& l : m_languages)
        for (usize i = 0; i < l.files.Size() && !changed; ++i) {
            platform::FileInfo info;
            changed = !fs::GetInfo(l.files[i], info) || info.modifiedTime != l.modified[i];
        }
    // New tables appear too (the folder's file count).
    if (!changed) {
        usize files = 0, now2 = 0;
        for (const Language& l : m_languages)
            files += l.files.Size();
        for (StringView dir : {StringView("StarterContent/Localization"), StringView("Localization")}) {
            Array<platform::DirectoryEntry> entries;
            fs::ListOptions opt;
            opt.includeDirectories = false;
            opt.extension = ".zloc";
            const String folder = Path::Join(m_contentRoot, dir);
            if (fs::IsDirectory(folder) && fs::List(folder, entries, opt))
                now2 += entries.Size();
        }
        changed = files != now2;
    }
    if (changed)
        Load(m_contentRoot);
}

void Localization::SetText(StringView code, StringView key, StringView text)
{
    if (key.Empty())
        return;
    if (!Find(code))
        AddLanguage(code, code);
    for (Language& l : m_languages)
        if (l.code.EqualsIgnoreCase(code)) {
            if (text.Empty())
                l.texts.Remove(key);
            else
                l.texts.InsertOrAssign(String(key), String(text));
        }
    ++m_version;
}

void Localization::RemoveKey(StringView key)
{
    for (Language& l : m_languages)
        l.texts.Remove(key);
    ++m_version;
}

bool Localization::AddLanguage(StringView code, StringView name)
{
    const String c = NormalizeCode(code.Trim());
    if (c.Empty() || Find(c))
        return false;
    m_languages.PushBack(Language{c, String(name.Trim().Empty() ? StringView(c) : name.Trim()), {}, {}, {}});
    if (m_current.Empty())
        m_current = c;
    ++m_version;
    return true;
}

Array<String> Localization::AllKeys() const
{
    HashMap<String, bool> seen;
    Array<String> keys;
    for (const Language& l : m_languages)
        for (const auto& [k, v] : l.texts)
            if (!seen.Find(k)) {
                seen.InsertOrAssign(k, true);
                keys.PushBack(k);
            }
    Sort(keys, [](const String& a, const String& b) { return a.View() < b.View(); });
    return keys;
}

bool Localization::Save(StringView contentRoot)
{
    bool ok = true;
    for (const Language& l : m_languages)
        ok = WriteTable(Path::Join(contentRoot, String::Format("Localization/{}.zloc", l.code)), l.name, l.texts) && ok;
    m_contentRoot = contentRoot;
    m_lastPoll = 0.0; // pick the files up (their times) on the next poll
    return ok;
}

const Localization::Language* Localization::Find(StringView code) const
{
    for (const Language& l : m_languages)
        if (l.code.EqualsIgnoreCase(code))
            return &l;
    return nullptr;
}

bool Localization::SetLanguage(StringView code)
{
    const Language* l = Find(code);
    if (!l)
        return false;
    if (m_current != l->code) {
        m_current = l->code;
        ++m_version;
        ZX_LOG_INFO(LogLocalization, "Language: {} ({})", l->name, l->code);
    }
    if (!preferenceFile.Empty())
        fs::WriteTextFile(preferenceFile, m_current);
    return true;
}

const String* Localization::Find(StringView key, StringView language) const
{
    const Language* l = Find(language);
    return l ? l->texts.Find(key) : nullptr;
}

const String* Localization::Translate(StringView key) const
{
    if (const String* t = Find(key, m_current))
        return t;
    return Find(key, defaultLanguage);
}

String Localization::Format(StringView key, Span<const String> args) const
{
    const String* t = Translate(key);
    const StringView text = t ? t->View() : key;
    if (args.Empty())
        return String(text);
    String out;
    for (usize i = 0; i < text.Size(); ++i) {
        if (text[i] == '{' && i + 2 < text.Size() && text[i + 1] >= '0' && text[i + 1] <= '9' && text[i + 2] == '}') {
            const usize a = usize(text[i + 1] - '0');
            if (a < args.Size()) {
                out += args[a];
                i += 2;
                continue;
            }
        }
        out += text[i];
    }
    return out;
}

} // namespace zx
