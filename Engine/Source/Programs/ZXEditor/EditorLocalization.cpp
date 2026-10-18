// Window > Localization: the game's texts per language (Content/Localization/<code>.zloc) as a table - keys down,
// languages across. Edits save to the project's tables; the preview language switches the viewport's UI.

#include "Editor.h"
#include "UI/UI.h"

ZX_DEFINE_LOG_CATEGORY_STATIC(LogLocalizationUI, Info);

namespace zx::editor {

void DrawLocalizationPanel(Editor& ed)
{
    if (!ed.showLocalization)
        return;
    ui::SetNextWindowSize(Vec2(760.0f, 520.0f), ui::Cond::FirstUse);
    if (!ui::Begin(LocalizationTitle, &ed.showLocalization)) {
        ui::End();
        return;
    }
    Editor::LocalizationWindowState& st = ed.localizationWindow;
    Localization& loc = ed.localization;
    const Span<const Localization::Language> langs = loc.Languages();
    const ui::Theme& colors = ui::GetCurrentContext()->GetStyle().colors;

    ui::TextDisabled("UI Text > Key shows the text of the player's language; scripts: tr(\"key\"), lang.set(\"ru\").");
    // Preview language (not saved: the player's choice belongs to the game).
    Array<String> labels;
    Array<StringView> items;
    i32 current = -1;
    for (usize i = 0; i < langs.Size(); ++i) {
        labels.PushBack(String::Format("{} ({})", langs[i].name, langs[i].code));
        if (langs[i].code == loc.Current())
            current = i32(i);
    }
    for (const String& l : labels)
        items.PushBack(l.View());
    ui::AlignTextToFramePadding();
    ui::Text("Preview");
    ui::SameLine();
    ui::SetNextItemWidth(180.0f);
    if (ui::Combo("##locPreview", &current, items.View()) && current >= 0)
        loc.SetLanguage(langs[usize(current)].code);
    ui::ItemTooltip("Language of the game UI in the viewport while editing");
    ui::SameLine(0.0f, 16.0f);
    ui::SetNextItemWidth(70.0f);
    ui::InputText("##locCode", st.newCode, ui::InputTextFlags::None, "code");
    ui::ItemTooltip("Language code: en, ru, de, fr, es, ja, zh...");
    ui::SameLine(0.0f, 4.0f);
    ui::SetNextItemWidth(120.0f);
    ui::InputText("##locName", st.newName, ui::InputTextFlags::None, "name");
    ui::ItemTooltip("Shown in language menus: Deutsch, Français...");
    ui::SameLine(0.0f, 4.0f);
    ui::BeginDisabled(st.newCode.View().Trim().Empty());
    if (ui::Button(ZX_ICON_PLUS "  Language")) {
        if (loc.AddLanguage(st.newCode.View(), st.newName.View())) {
            st.dirty = true;
            st.newCode.Clear();
            st.newName.Clear();
        }
    }
    ui::EndDisabled();
    ui::SameLine(0.0f, 16.0f);
    ui::BeginDisabled(!st.dirty);
    if (ui::Button(ZX_ICON_SAVE "  Save", Vec2::Zero(), st.dirty ? ui::ButtonVariant::Primary : ui::ButtonVariant::Default)) {
        if (loc.Save(ed.contentRoot)) {
            st.dirty = false;
            ZX_LOG_INFO(LogLocalizationUI, "Saved {} language tables into Content/Localization", loc.Languages().Size());
        } else {
            ZX_LOG_ERROR(LogLocalizationUI, "Can't write Content/Localization");
        }
    }
    ui::EndDisabled();

    // New key (optionally put on the selected UI Text right away).
    ui::SetNextItemWidth(240.0f);
    ui::InputText("##locNewKey", st.newKey, ui::InputTextFlags::None, "new.key");
    ui::SameLine(0.0f, 4.0f);
    const String newKey(st.newKey.View().Trim());
    ui::BeginDisabled(newKey.Empty() || langs.Empty());
    if (ui::Button(ZX_ICON_PLUS "  Key")) {
        // The selected UI Text's current text becomes the first language's text.
        const UIText* t = ed.world.IsAlive(ed.selected) ? ed.world.Get<const UIText>(ed.selected) : nullptr;
        loc.SetText(langs[0].code, newKey, t ? t->text.View() : newKey.View());
        st.newKey.Clear();
        st.dirty = true;
    }
    ui::EndDisabled();
    ui::ItemTooltip("Adds the key (with the selected UI Text's text, if any)");
    ui::SameLine(0.0f, 16.0f);
    ui::SetNextItemWidth(-1.0f);
    ui::InputText("##locFilter", st.filter, ui::InputTextFlags::None, ZX_ICON_SEARCH "  Filter keys and texts");

    if (langs.Empty()) {
        ui::Spacing();
        ui::TextWrapped("No languages yet. Add one above (for example code \"en\", name \"English\"), then keys, then Save: "
                        "the tables are written to Content/Localization.");
        ui::End();
        return;
    }
    const Array<String> keys = loc.AllKeys();
    const bool textSelected = ed.world.IsAlive(ed.selected) && ed.world.Has<UIText>(ed.selected);
    String removeKey;
    if (ui::BeginTable("##locTable", u32(langs.Size()) + 2, ui::TableFlags::Borders | ui::TableFlags::RowBg | ui::TableFlags::Resizable | ui::TableFlags::ScrollY,
                       Vec2(0.0f, -1.0f))) {
        ui::TableSetupColumn("Key", 200.0f);
        for (const Localization::Language& l : langs)
            ui::TableSetupColumn(String::Format("{} ({})", l.name, l.code));
        ui::TableSetupColumn(" ", 64.0f);
        ui::TableHeadersRow();
        for (const String& key : keys) {
            // Filter by key or any text.
            if (!st.filter.Empty()) {
                bool match = key.View().Contains(st.filter.View());
                for (const Localization::Language& l : langs)
                    if (const String* t = l.texts.Find(key); t && t->View().Contains(st.filter.View()))
                        match = true;
                if (!match)
                    continue;
            }
            ui::PushID(key.View());
            ui::TableNextRow();
            ui::TableNextColumn();
            ui::AlignTextToFramePadding();
            ui::Text(key);
            for (const Localization::Language& l : langs) {
                ui::TableNextColumn();
                const String* t = l.texts.Find(key);
                // New lines are edited as \n (one line per cell).
                String text = t ? *t : String();
                text.ReplaceAll("\n", "\\n");
                ui::PushID(l.code.View());
                ui::SetNextItemWidth(-1.0f);
                if (ui::InputText("##t", text, ui::InputTextFlags::None, "missing")) {
                    text.ReplaceAll("\\n", "\n");
                    loc.SetText(l.code, key, text);
                    st.dirty = true;
                }
                if (!t)
                    ui::ItemTooltip("No text in this language yet: the default language's text is shown");
                ui::PopID();
            }
            ui::TableNextColumn();
            ui::BeginDisabled(!textSelected);
            if (ui::IconButton(ZX_ICON_LINK, "Use this key on the selected UI Text")) {
                ed.undo.Begin("Set Localization Key");
                ed.undo.Record(ed.selected);
                ed.world.Get<UIText>(ed.selected)->key = key;
                ed.undo.End();
            }
            ui::EndDisabled();
            ui::SameLine(0.0f, 2.0f);
            if (ui::IconButton(ZX_ICON_TRASH_2, "Remove this key from every language"))
                removeKey = key;
            ui::PopID();
        }
        ui::EndTable();
    }
    if (!removeKey.Empty()) {
        loc.RemoveKey(removeKey);
        st.dirty = true;
    }
    (void)colors;
    ui::End();
}

} // namespace zx::editor
