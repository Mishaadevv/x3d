// Zeqou AI: the toolbar orb, the glass chat panel (spring animation, animated glow), the MCP connection page and
// Settings > AI. Settings: %LOCALAPPDATA%/ZeqouX3D/AI.json (API keys DPAPI-encrypted).

#include "EditorAI.h"
#include "Editor.h"
#include "Core/Console/Console.h"
#include "Core/FileSystem/FileSystem.h"
#include "Core/HAL/PlatformFile.h"
#include "Core/String/Path.h"
#include "Core/Time/Time.h"
#include "Core/Version.h"
#include "Platform/Window.h"

ZX_DEFINE_LOG_CATEGORY_STATIC(LogAI, Info);

namespace zx::editor {

namespace {

constexpr StringView ProviderIds[] = {"anthropic", "openai", "openai-compatible"};
constexpr StringView ProviderNames[] = {"Anthropic (Claude)", "OpenAI", "OpenAI-compatible (local / other)"};
constexpr StringView Efforts[] = {"low", "medium", "high", "xhigh", "max"};

/// %LOCALAPPDATA%/ZeqouX3D/AI.json; -aisettings=<file> replaces it (automated tests).
String SettingsPath()
{
    StringView file;
    if (CommandLine::GetValue("aisettings", file))
        return Path::Normalize(platform::GetAbsolutePath(file));
    return Path::Join(Path::Normalize(platform::GetUserLocalAppDataDirectory()), "ZeqouX3D/AI.json");
}

// The orb's and the glow's palette (iOS-like: cyan, indigo, violet, pink, amber).
constexpr Color GlowColors[] = {Color(0x3B, 0xD5, 0xFF), Color(0x5B, 0x6C, 0xFF), Color(0xA8, 0x5C, 0xFF), Color(0xFF, 0x4F, 0xA3),
                                Color(0xFF, 0xB5, 0x47)};
constexpr Color AccentA = Color(0x3B, 0x8C, 0xF5);
constexpr Color AccentB = Color(0x8A, 0x5C, 0xF6);
constexpr Color White = Color(255, 255, 255);
constexpr Color TextSecondary = Color(0xA8, 0xAA, 0xBC);

f32 EaseOutCubic(f32 t)
{
    const f32 u = 1.0f - Saturate(t);
    return 1.0f - u * u * u;
}

Color Fade(Color c, f32 alpha) { return WithAlpha(c, alpha); }

ui::FontHandle Font(StringView name) { return ui::GetCurrentContext()->Fonts().FindFont(name); }

/// The Siri-like orb: colored blobs orbiting inside a dark glass sphere, a highlight and an outer glow.
void DrawOrb(DrawList& dl, Vec2 center, f32 r, f32 time, f32 energy, f32 alpha = 1.0f)
{
    const Rect box(center - Vec2(r), center + Vec2(r));
    for (u32 i = 0; i < 3; ++i) {
        const f32 a = time * (0.9f + 0.25f * f32(i)) + f32(i) * 2.1f;
        const Vec2 off(Cos(a) * r * 0.18f, Sin(a) * r * 0.18f);
        dl.AddShadow(box, Fade(GlowColors[(i * 2) % 5], (0.22f + 0.45f * energy) * alpha), r, r * (0.8f + 0.7f * energy), off, true);
    }
    dl.AddCircle(center, r, Fade(Color(0x10, 0x10, 0x1C), alpha));
    dl.PushClipRect(box, r);
    for (u32 i = 0; i < 4; ++i) {
        const f32 speed = 0.8f + 0.35f * f32(i) + energy * 1.2f;
        const f32 a = time * speed * (i % 2 ? -1.0f : 1.0f) + f32(i) * 1.7f;
        const Vec2 p = center + Vec2(Cos(a), Sin(a * 1.3f)) * (r * 0.42f);
        const f32 br = r * (0.62f + 0.08f * Sin(time * 2.0f + f32(i)));
        dl.AddShadow(Rect(p - Vec2(br * 0.5f), p + Vec2(br * 0.5f)), Fade(GlowColors[i], 0.95f * alpha), br * 0.5f, br, Vec2::Zero(), false);
    }
    dl.AddShadow(Rect(center + Vec2(-r * 0.5f, -r * 0.72f), center + Vec2(r * 0.2f, -r * 0.45f)), Fade(White, 0.35f * alpha), r * 0.2f,
                 r * 0.3f, Vec2::Zero(), false);
    dl.PopClipRect();
    dl.AddCircleOutline(center, r - 0.5f, Fade(White, 0.22f * alpha), 1.0f);
}

bool Configured(const AISettings& s)
{
    return s.provider == ai::Provider::OpenAICompatible ? !s.baseUrl.Empty() || !s.model.Empty() : !s.apiKeys[u32(s.provider)].Empty();
}

String ModelLabel(const AISettings& s)
{
    const StringView model = s.model.Empty() ? ai::DefaultModel(s.provider) : s.model.View();
    if (model == "claude-opus-5-5")
        return "Claude Opus 5.5";
    return model.Empty() ? String("Local model") : String(model);
}

String McpUrl(const AISettings& s) { return String::Format("http://127.0.0.1:{}/mcp", s.mcpPort); }

/// Lightweight Markdown -> plain text for chat bubbles (code fences, heading marks, bullets, bold).
String PlainText(StringView md)
{
    String out;
    usize pos = 0;
    while (pos <= md.Size()) {
        usize e = md.Find('\n', pos);
        if (e == StringView::NPos)
            e = md.Size();
        StringView l = md.SubStr(pos, e - pos);
        pos = e + 1;
        if (l.Trim().StartsWith("```"))
            continue;
        bool heading = false;
        while (l.StartsWith("#")) {
            l = l.DropLeft(1);
            heading = true;
        }
        if (heading)
            l = l.Trim();
        if (l.StartsWith("- ") || l.StartsWith("* "))
            out.Appendf("\xE2\x80\xA2 {}", l.DropLeft(2));
        else
            out += l;
        if (e < md.Size())
            out += '\n';
    }
    out.ReplaceAll("**", "");
    out.TrimInPlace();
    return out;
}

void SendMessage(Editor& ed, StringView text)
{
    AIState& ai = *ed.ai;
    const String trimmed = String(text).Trimmed();
    if (trimmed.Empty() || ai.agent.IsBusy())
        return;
    if (!Configured(ai.settings)) {
        ai.chat.PushBack(AIChatItem{AIChatItem::Kind::Error, "Add an API key in Settings > AI first (or connect an agent over MCP)."});
        return;
    }
    ai::AgentConfig c;
    c.provider = ai.settings.provider;
    c.apiKey = ai.settings.apiKeys[u32(c.provider)];
    c.model = ai.settings.model;
    c.baseUrl = ai.settings.baseUrl;
    c.effort = c.provider == ai::Provider::Anthropic ? ai.settings.effort : String();
    c.maxTokens = u32(Clamp(ai.settings.maxTokens, 1024, 128000));
    c.serverFallbacks = ai.settings.serverFallbacks && c.provider == ai::Provider::Anthropic;
    const String system = McpInstructions() +
                          "\n\nYou are Zeqou AI, the assistant built into the editor: the user chats with you in a side panel "
                          "while watching the viewport. Reply in the user's language. Act with the tools rather than "
                          "explaining how; keep replies short and say what you changed.";
    if (ai.agent.Send(c, system, trimmed)) {
        ai.chat.PushBack(AIChatItem{AIChatItem::Kind::User, trimmed});
        ai.scrollToBottom = true;
    }
}

} // namespace

void SendAIMessage(Editor& ed, StringView text)
{
    if (ed.ai)
        SendMessage(ed, text);
}

// --- Settings -------------------------------------------------------------------------------------------------------

void LoadAISettings(AISettings& s)
{
    String text;
    JsonValue doc;
    if (fs::ReadTextFile(SettingsPath(), text) && ParseJson(text, doc)) {
        for (u32 i = 0; i < 3; ++i) {
            if (doc.Get("provider").AsString() == ProviderIds[i])
                s.provider = ai::Provider(i);
            String key;
            if (platform::UnprotectSecret(doc.Get("keys").Get(ProviderIds[i]).AsString(), key))
                s.apiKeys[i] = move(key);
        }
        s.model = doc.Get("model").AsString();
        s.baseUrl = doc.Get("baseUrl").AsString();
        s.effort = doc.Get("effort").AsString("high");
        s.maxTokens = i32(doc.Get("maxTokens").AsInt(16000));
        s.serverFallbacks = doc.Get("serverFallbacks").AsBool(true);
        s.mcpEnabled = doc.Get("mcp").Get("enabled").AsBool(false);
        s.mcpPort = i32(doc.Get("mcp").Get("port").AsInt(47800));
        s.mcpToken = doc.Get("mcp").Get("token").AsString();
    }
    if (s.mcpToken.Size() < 16)
        s.mcpToken = platform::RandomHex(16);
}

void SaveAISettings(const AISettings& s)
{
    JsonValue doc = JsonValue::MakeObject();
    doc.Set("provider", ProviderIds[u32(s.provider)]);
    doc.Set("model", s.model);
    doc.Set("baseUrl", s.baseUrl);
    doc.Set("effort", s.effort);
    doc.Set("maxTokens", s.maxTokens);
    doc.Set("serverFallbacks", s.serverFallbacks);
    JsonValue& keys = doc["keys"];
    keys = JsonValue::MakeObject();
    for (u32 i = 0; i < 3; ++i)
        if (!s.apiKeys[i].Empty())
            keys.Set(ProviderIds[i], platform::ProtectSecret(s.apiKeys[i]));
    doc["mcp"].Set("enabled", s.mcpEnabled);
    doc["mcp"].Set("port", s.mcpPort);
    doc["mcp"].Set("token", s.mcpToken);
    const String path = SettingsPath();
    platform::CreateDirectoryTree(Path::Parent(path));
    if (!fs::WriteTextFile(path, ToJsonString(doc)))
        ZX_LOG_WARN(LogAI, "Could not save the AI settings to {}", path);
}

// --- State ----------------------------------------------------------------------------------------------------------

AIState::AIState(Editor& ed) : host(ed), agent(host) {}

AIState::~AIState()
{
    host.Shutdown();
    mcp.Stop();
    agent.Cancel();
    agent.Wait();
}

void InitAI(Editor& ed)
{
    ed.ai.Reset(new AIState(ed));
    LoadAISettings(ed.ai->settings);
}

void ShutdownAI(Editor& ed)
{
    if (!ed.ai)
        return;
    ed.ai->host.Shutdown(); // unblocks waiting MCP connections before the server joins them
    ed.ai->agent.Cancel();
    ed.ai->mcp.Stop();
    ed.ai->agent.Wait();
    ed.ai.Reset();
}

void UpdateAI(Editor& ed)
{
    if (!ed.ai)
        return;
    AIState& ai = *ed.ai;
    const f32 dt = Clamp(ed.dt, 0.0f, 0.05f);
    ai.time += dt;
    ai.host.Pump();

    // The MCP server follows the setting (a failed start waits until the user changes something).
    if (ai.settings.mcpEnabled) {
        if (ai.mcp.IsRunning() && ai.startedPort != ai.settings.mcpPort)
            ai.mcp.Stop();
        if (!ai.mcp.IsRunning() && ai.mcpError.Empty()) {
            ai::McpOptions o;
            o.token = ai.settings.mcpToken;
            o.instructions = McpInstructions();
            o.serverVersion = EngineVersionString;
            String error;
            if (ai.mcp.Start(u16(Clamp(ai.settings.mcpPort, 1024, 65535)), move(o), ai.host, &error))
                ai.startedPort = ai.settings.mcpPort;
            else
                ai.mcpError = error.Empty() ? String("Could not start the server") : error;
        }
    } else if (ai.mcp.IsRunning()) {
        ai.mcp.Stop();
    }

    // Agent events -> chat.
    for (ai::AgentEvent& e : ai.agent.TakeEvents()) {
        switch (e.type) {
        case ai::AgentEventType::Text: ai.chat.PushBack(AIChatItem{AIChatItem::Kind::Assistant, PlainText(e.text)}); break;
        case ai::AgentEventType::ToolStart: {
            AIChatItem item{AIChatItem::Kind::Tool, move(e.text)};
            item.tool = move(e.tool);
            item.running = true;
            ai.chat.PushBack(move(item));
            break;
        }
        case ai::AgentEventType::ToolEnd:
            for (isize i = isize(ai.chat.Size()) - 1; i >= 0; --i) {
                AIChatItem& item = ai.chat[usize(i)];
                if (item.kind == AIChatItem::Kind::Tool && item.running && item.tool == e.tool) {
                    item.running = false;
                    item.isError = e.isError;
                    const usize nl = e.text.Find('\n');
                    item.result = nl == StringView::NPos ? e.text : e.text.SubStr(0, nl);
                    break;
                }
            }
            break;
        case ai::AgentEventType::Error: ai.chat.PushBack(AIChatItem{AIChatItem::Kind::Error, move(e.text)}); break;
        default: break;
        }
        ai.scrollToBottom = true;
    }
    for (AIChatItem& item : ai.chat)
        item.age += dt;

    // Spring towards open / closed: a little overshoot when opening, settles in ~0.4 s.
    const f32 target = ai.open ? 1.0f : 0.0f;
    const f32 k = 210.0f, c = ai.open ? 19.0f : 30.0f;
    for (u32 i = 0; i < 4; ++i) {
        const f32 h = dt / 4.0f;
        ai.openVelocity += (k * (target - ai.openAnim) - c * ai.openVelocity) * h;
        ai.openAnim += ai.openVelocity * h;
    }
    if (!ai.open && ai.openAnim < 0.002f) {
        ai.openAnim = 0.0f;
        ai.openVelocity = 0.0f;
    }
}

// --- Orb ------------------------------------------------------------------------------------------------------------

void DrawAIOrb(Editor& ed, const Rect& r)
{
    if (!ed.ai)
        return;
    AIState& ai = *ed.ai;
    ui::Context& ctx = *ui::GetCurrentContext();
    const f32 s = ctx.GetScale();
    ui::SetCursorPos(r.min);
    const bool clicked = ui::InvisibleButton("##zeqou_ai_orb", r.Size() / s);
    const bool hovered = ui::IsItemHovered();
    ui::ItemTooltip(ai.open ? "Close Zeqou AI" : "Zeqou AI - build with words (Ctrl+Space)");
    if (clicked) {
        ai.open = !ai.open;
        ai.focusInput = ai.open;
    }
    const f32 dt = Clamp(ed.dt, 0.0f, 0.05f);
    ai.orbHover += ((hovered ? 1.0f : 0.0f) - ai.orbHover) * Min(1.0f, dt * 12.0f);
    ai.orbPress += ((ui::IsItemActive() ? 1.0f : 0.0f) - ai.orbPress) * Min(1.0f, dt * 20.0f);
    ai.orbRect = r;
    const bool working = ai.agent.IsBusy();
    const f32 energy = Max(ai.orbHover, working ? 0.6f + 0.4f * Sin(f32(ai.time) * 4.0f) : 0.0f);
    const f32 radius = r.Height() * 0.5f * (1.0f + 0.06f * ai.orbHover - 0.08f * ai.orbPress);
    DrawOrb(ui::GetWindowDrawList(), r.Center(), radius, f32(ai.time), energy);
    if (ai.mcp.IsRunning()) {
        // An agent may connect: green dot.
        const Vec2 p(r.max.x - 2.0f * s, r.min.y + 3.0f * s);
        ui::GetWindowDrawList().AddCircle(p, 3.5f * s, Color(0x34, 0xC7, 0x59), 1.5f * s, Color(0x1A, 0x1A, 0x1A));
    }
}

// --- Panel ----------------------------------------------------------------------------------------------------------

namespace {

/// A round icon button on glass.
bool GlassIconButton(StringView id, StringView icon, Vec2 center, f32 radius, f32 alpha, StringView tooltip)
{
    const f32 s = ui::GetCurrentContext()->GetScale();
    const Rect r(center - Vec2(radius), center + Vec2(radius));
    ui::SetCursorPos(r.min);
    const bool clicked = ui::InvisibleButton(id, r.Size() / s);
    const bool hovered = ui::IsItemHovered();
    if (!tooltip.Empty())
        ui::ItemTooltip(tooltip);
    DrawList& dl = ui::GetWindowDrawList();
    dl.AddCircle(center, radius, Fade(White, (hovered ? 0.16f : 0.07f) * alpha));
    dl.AddTextInRect(Font("Inter"), 14.0f * s, r, Fade(Color(235, 235, 245), (hovered ? 1.0f : 0.75f) * alpha), icon, ui::TextAlign::Center, false);
    return clicked;
}

/// A titled code block with a Copy button.
void CopyBlock(StringView id, StringView title, StringView text, f32 width, f32 alpha)
{
    ui::Context& ctx = *ui::GetCurrentContext();
    const f32 s = ctx.GetScale();
    DrawList& dl = ui::GetWindowDrawList();
    const ui::FontHandle font = Font("Inter");
    const f32 size = 11.5f * s;
    const f32 wrap = width - 24.0f * s;
    const Vec2 textSize = ctx.Fonts().MeasureText(font, size, text, wrap);
    const Vec2 p = ui::GetCursorPos();
    const f32 titleH = 22.0f * s;
    const Rect box(p + Vec2(0.0f, titleH), p + Vec2(width, titleH + textSize.y + 20.0f * s));
    dl.AddTextInRect(Font("Inter SemiBold"), 12.0f * s, Rect(p, Vec2(p.x + width - 80.0f * s, p.y + titleH - 4.0f * s)), Fade(Color(235, 235, 245), alpha),
                     title);
    dl.AddRect(box, Fade(Color(0, 0, 0), 0.35f * alpha), ui::CornerRadii(12.0f * s), 1.0f, Fade(White, 0.08f * alpha));
    dl.AddText(font, size, box.min + Vec2(12.0f * s, 10.0f * s), Fade(Color(0xB8, 0xE0, 0xFF), alpha), text, wrap);
    const Rect btn(Vec2(p.x + width - 72.0f * s, p.y), Vec2(p.x + width, p.y + titleH - 4.0f * s));
    ui::SetCursorPos(btn.min);
    static String s_copied;
    static f64 s_copiedAt = -10.0;
    if (ui::InvisibleButton(id, btn.Size() / s)) {
        platform::SetClipboardText(text);
        s_copied = id;
        s_copiedAt = Clock::NowSeconds();
    }
    const bool done = s_copied == id && Clock::NowSeconds() - s_copiedAt < 1.5;
    dl.AddRect(btn, Fade(done ? Color(0x34, 0xC7, 0x59) : White, (ui::IsItemHovered() ? 0.24f : 0.1f) * alpha), ui::CornerRadii(btn.Height() * 0.5f));
    dl.AddTextInRect(Font("Inter Medium"), 11.0f * s, btn, Fade(Color(240, 240, 248), alpha), done ? ZX_ICON_CHECK " Copied" : ZX_ICON_COPY " Copy",
                     ui::TextAlign::Center, false);
    ui::SetCursorPos(Vec2(p.x, box.max.y + 12.0f * s));
    ui::Dummy(Vec2(0.0f, 0.0f));
}

/// MCP: the switch, its state and how to connect each client (the panel's connect page and Settings > AI).
void DrawMcpConnect(Editor& ed, f32 width, f32 alpha)
{
    AIState& ai = *ed.ai;
    AISettings& st = ai.settings;
    if (ui::Toggle("Allow AI agents to connect (MCP server)", &st.mcpEnabled)) {
        ai.mcpError.Clear();
        SaveAISettings(st);
    }
    if (ai.mcp.IsRunning()) {
        const String client = ai.mcp.LastClient();
        ui::TextColored(Color(0x34, 0xC7, 0x59), client.Empty() ? String::Format(ZX_ICON_CIRCLE_CHECK "  Listening on {}", McpUrl(st))
                                                                 : String::Format(ZX_ICON_CIRCLE_CHECK "  {} connected - {} tool calls", client,
                                                                                  ai.mcp.ToolCallCount()));
    } else if (!ai.mcpError.Empty()) {
        ui::TextColored(Color(0xFF, 0x6B, 0x6B), ai.mcpError);
    } else {
        ui::TextDisabled("Off. Turn it on, then add the server to your AI tool:");
    }
    ui::Spacing();
    const String url = McpUrl(st);
    const String auth = String::Format("Bearer {}", st.mcpToken);
    CopyBlock("##cc", "Claude Code", String::Format("claude mcp add --transport http zeqoux3d {} --header \"Authorization: {}\"", url, auth), width,
              alpha);
    CopyBlock("##cursor", "Cursor  (~/.cursor/mcp.json)",
              String::Format("{{\n  \"mcpServers\": {{\n    \"zeqoux3d\": {{\n      \"url\": \"{}\",\n      \"headers\": {{ \"Authorization\": \"{}\" }}\n    }}\n  }}\n}}",
                             url, auth),
              width, alpha);
    CopyBlock("##vscode", "VS Code / Copilot  (.vscode/mcp.json)",
              String::Format("{{\n  \"servers\": {{\n    \"zeqoux3d\": {{\n      \"type\": \"http\",\n      \"url\": \"{}\",\n      \"headers\": {{ \"Authorization\": \"{}\" }}\n    }}\n  }}\n}}",
                             url, auth),
              width, alpha);
    CopyBlock("##desktop", "Claude Desktop, other stdio clients  (Node.js)",
              String::Format("{{\n  \"mcpServers\": {{\n    \"zeqoux3d\": {{\n      \"command\": \"npx\",\n      \"args\": [\"-y\", \"mcp-remote\", \"{}\", \"--header\", \"Authorization:{}\"]\n    }}\n  }}\n}}",
                             url, auth),
              width, alpha);
    ui::TextWrapped("The server listens on this computer only (127.0.0.1) and requires the token. Then ask your agent, e.g. "
                    "\"Build a small arena level with a player and three patrolling enemies\".");
}

void DrawChatItem(Editor& ed, AIChatItem& item, f32 width, f32 alpha)
{
    ui::Context& ctx = *ui::GetCurrentContext();
    const f32 s = ctx.GetScale();
    DrawList& dl = ui::GetWindowDrawList();
    const f32 appear = EaseOutCubic(item.age / 0.35f);
    const f32 a = alpha * appear;
    const f32 lift = (1.0f - appear) * 12.0f * s;
    const Vec2 p = ui::GetCursorPos() + Vec2(0.0f, lift);
    const ui::FontHandle font = Font("Inter");
    const f32 size = 13.5f * s;
    const f32 pad = 13.0f * s;
    f32 height = 0.0f;
    if (item.kind == AIChatItem::Kind::Tool) {
        // Activity chip: state icon, tool, what it did.
        String label = item.tool;
        label.ReplaceAll("_", " ");
        // What it was asked to do; a failed call shows why.
        const String& detail = item.isError && !item.result.Empty() ? item.result : item.text;
        const String line = detail.Empty() ? label : String::Format("{}  \xC2\xB7  {}", label, detail);
        const f32 h = 28.0f * s;
        const f32 w = Min(width, ctx.Fonts().MeasureWidth(Font("Inter Medium"), 12.0f * s, line) + 48.0f * s);
        const Rect r(p, p + Vec2(w, h));
        const Color tint = item.running ? Color(0x7B, 0x8C, 0xFF) : item.isError ? Color(0xFF, 0x6B, 0x6B) : Color(0x34, 0xC7, 0x59);
        dl.AddRect(r, Fade(tint, 0.13f * a), ui::CornerRadii(h * 0.5f), 1.0f, Fade(tint, 0.30f * a));
        const Vec2 ic(r.min.x + h * 0.5f + 2.0f * s, r.Center().y);
        if (item.running) {
            const f32 t = f32(ed.ai->time) * 6.0f;
            for (u32 i = 0; i < 3; ++i) {
                const f32 ang = t + f32(i) * 2.094f;
                dl.AddCircle(ic + Vec2(Cos(ang), Sin(ang)) * (5.0f * s), 1.8f * s, Fade(Color(0xC8, 0xD0, 0xFF), a));
            }
        } else {
            dl.AddTextInRect(font, 12.0f * s, Rect(ic - Vec2(8.0f * s), ic + Vec2(8.0f * s)), Fade(tint, a), item.isError ? ZX_ICON_X : ZX_ICON_CHECK,
                             ui::TextAlign::Center, false);
        }
        dl.AddTextInRect(Font("Inter Medium"), 12.0f * s, Rect(Vec2(r.min.x + h + 4.0f * s, r.min.y), r.max - Vec2(12.0f * s, 0.0f)),
                         Fade(Color(0xD8, 0xDA, 0xE6), a), line);
        height = h;
    } else {
        const bool user = item.kind == AIChatItem::Kind::User;
        const f32 maxW = width * (user ? 0.82f : 0.96f);
        const Vec2 ts = ctx.Fonts().MeasureText(font, size, item.text, maxW - pad * 2.0f);
        const f32 bw = Min(maxW, ts.x + pad * 2.0f + 2.0f * s);
        const f32 bh = ts.y + 19.0f * s;
        const Rect r = user ? Rect(Vec2(p.x + width - bw, p.y), Vec2(p.x + width, p.y + bh)) : Rect(p, p + Vec2(bw, bh));
        const f32 rad = Min(19.0f * s, bh * 0.5f), tail = 6.0f * s;
        if (user) {
            dl.AddShadow(r, Fade(Color(0x5B, 0x6C, 0xFF), 0.35f * a), rad, 14.0f * s, Vec2(0.0f, 4.0f * s));
            dl.AddRectGradient(r, Fade(AccentA, a), Fade(AccentB, a), ui::GradientDirection::Horizontal, ui::CornerRadii(rad, rad, tail, rad));
        } else if (item.kind == AIChatItem::Kind::Error) {
            dl.AddRect(r, Fade(Color(0xFF, 0x45, 0x3A), 0.16f * a), ui::CornerRadii(rad, rad, rad, tail), 1.0f, Fade(Color(0xFF, 0x6B, 0x6B), 0.35f * a));
        } else {
            dl.AddRect(r, Fade(White, 0.075f * a), ui::CornerRadii(rad, rad, rad, tail), 1.0f, Fade(White, 0.09f * a));
        }
        const Color tc = user ? White : item.kind == AIChatItem::Kind::Error ? Color(0xFF, 0xB4, 0xAE) : Color(0xEC, 0xEC, 0xF2);
        dl.AddText(font, size, r.min + Vec2(pad, 9.5f * s), Fade(tc, a), item.text, maxW - pad * 2.0f);
        height = bh;
    }
    ui::Dummy(Vec2(width / s, (height + lift) / s));
    ui::Dummy(Vec2(0.0f, 2.0f));
}

void DrawTyping(Editor& ed, f32 alpha)
{
    const f32 s = ui::GetCurrentContext()->GetScale();
    DrawList& dl = ui::GetWindowDrawList();
    const Vec2 p = ui::GetCursorPos();
    const Rect r(p, p + Vec2(58.0f * s, 34.0f * s));
    dl.AddRect(r, Fade(White, 0.075f * alpha), ui::CornerRadii(17.0f * s, 17.0f * s, 17.0f * s, 6.0f * s));
    for (u32 i = 0; i < 3; ++i) {
        const f32 t = f32(ed.ai->time) * 7.0f - f32(i) * 0.9f;
        const f32 bounce = Max(0.0f, Sin(t)) * 3.5f * s;
        dl.AddCircle(Vec2(r.min.x + 17.0f * s + f32(i) * 12.0f * s, r.Center().y - bounce), 3.2f * s,
                     Fade(Color(0xC8, 0xCC, 0xE0), alpha * (0.55f + 0.45f * Max(0.0f, Sin(t)))));
    }
    ui::Dummy(Vec2(58.0f, 34.0f));
}

/// Empty chat: what the assistant does and how to set it up.
void DrawWelcome(Editor& ed, f32 width, f32 alpha)
{
    AIState& ai = *ed.ai;
    ui::Context& ctx = *ui::GetCurrentContext();
    const f32 s = ctx.GetScale();
    DrawList& dl = ui::GetWindowDrawList();
    Vec2 p = ui::GetCursorPos();
    const f32 orbR = 34.0f * s;
    DrawOrb(dl, Vec2(p.x + width * 0.5f, p.y + orbR + 16.0f * s), orbR, f32(ai.time), 0.35f, alpha);
    p.y += orbR * 2.0f + 32.0f * s;
    dl.AddTextInRect(Font("Inter Bold"), 22.0f * s, Rect(p, Vec2(p.x + width, p.y + 28.0f * s)), Fade(White, alpha), "Zeqou AI", ui::TextAlign::Center,
                     false);
    p.y += 30.0f * s;
    dl.AddTextInRect(Font("Inter"), 13.5f * s, Rect(p, Vec2(p.x + width, p.y + 20.0f * s)), Fade(TextSecondary, alpha),
                     "Describe a game. Watch it being built.", ui::TextAlign::Center, false);
    p.y += 36.0f * s;
    ui::SetCursorPos(p);

    auto card = [&](StringView id, StringView icon, StringView title, StringView text, Color tint) {
        const Vec2 c = ui::GetCursorPos();
        const Rect r(c, c + Vec2(width, 76.0f * s));
        const bool clicked = ui::InvisibleButton(id, r.Size() / s);
        const bool hov = ui::IsItemHovered();
        dl.AddRect(r, Fade(White, (hov ? 0.11f : 0.065f) * alpha), ui::CornerRadii(20.0f * s), 1.0f, Fade(White, 0.09f * alpha));
        const Rect ir(r.min + Vec2(14.0f * s, 18.0f * s), r.min + Vec2(54.0f * s, 58.0f * s));
        dl.AddRectGradient(ir, Fade(tint, alpha), Fade(ui::LerpColor(tint, AccentB, 0.6f), alpha), ui::GradientDirection::Vertical, ui::CornerRadii(12.0f * s));
        dl.AddTextInRect(Font("Inter"), 18.0f * s, ir, Fade(White, alpha), icon, ui::TextAlign::Center, false);
        const f32 tx = ir.max.x + 14.0f * s;
        dl.AddTextInRect(Font("Inter SemiBold"), 14.0f * s, Rect(Vec2(tx, r.min.y + 14.0f * s), Vec2(r.max.x - 28.0f * s, r.min.y + 34.0f * s)),
                         Fade(White, alpha), title);
        dl.AddText(Font("Inter"), 12.0f * s, Vec2(tx, r.min.y + 36.0f * s), Fade(TextSecondary, alpha), text, r.max.x - tx - 30.0f * s);
        dl.AddTextInRect(Font("Inter"), 14.0f * s, Rect(Vec2(r.max.x - 30.0f * s, r.min.y), r.max), Fade(Color(0x8E, 0x90, 0xA6), alpha),
                         ZX_ICON_CHEVRON_RIGHT, ui::TextAlign::Center, false);
        ui::SetCursorPos(Vec2(c.x, r.max.y + 10.0f * s));
        ui::Dummy(Vec2(0.0f, 0.0f));
        return clicked;
    };
    if (!Configured(ai.settings)) {
        if (card("##ai_key", ZX_ICON_KEY, "Use your API key", "Claude, OpenAI or a local model. The key stays on this computer, encrypted.", AccentA))
            OpenSettingsPage(ed, "AI");
        if (card("##ai_mcp", ZX_ICON_PLUG, "Connect an AI agent (MCP)", "Claude Code, Cursor, VS Code... build in the editor from your agent.",
                 Color(0xFF, 0x4F, 0xA3)))
            ai.page = 1;
        return;
    }
    static constexpr StringView Suggestions[] = {"Build a small arena with a player and three enemies", "Make the lighting cinematic at sunset",
                                                 "Add a day-night cycle and clouds", "What is in this scene?"};
    for (StringView text : Suggestions) {
        const Vec2 c = ui::GetCursorPos();
        const f32 w = Min(width, ctx.Fonts().MeasureWidth(Font("Inter Medium"), 12.5f * s, text) + 30.0f * s);
        const Rect r(Vec2(c.x + (width - w) * 0.5f, c.y), Vec2(c.x + (width + w) * 0.5f, c.y + 32.0f * s));
        ui::SetCursorPos(r.min);
        ui::PushID(text);
        const bool clicked = ui::InvisibleButton("##suggestion", r.Size() / s);
        const bool hov = ui::IsItemHovered();
        ui::PopID();
        dl.AddRect(r, Fade(White, (hov ? 0.13f : 0.06f) * alpha), ui::CornerRadii(16.0f * s), 1.0f, Fade(White, 0.1f * alpha));
        dl.AddTextInRect(Font("Inter Medium"), 12.5f * s, r, Fade(Color(0xDC, 0xDE, 0xEA), alpha), text, ui::TextAlign::Center, false);
        ui::SetCursorPos(Vec2(c.x, r.max.y + 8.0f * s));
        ui::Dummy(Vec2(0.0f, 0.0f));
        if (clicked)
            SendMessage(ed, text);
    }
}

/// The input row: a pill text field and the round send / stop button.
void DrawInputRow(Editor& ed, const Rect& row, f32 alpha)
{
    AIState& ai = *ed.ai;
    ui::Context& ctx = *ui::GetCurrentContext();
    const f32 s = ctx.GetScale();
    DrawList& dl = ui::GetWindowDrawList();
    const f32 h = row.Height();
    const Rect pill(row.min, Vec2(row.max.x - h - 8.0f * s, row.max.y));
    dl.AddRect(pill, Fade(White, 0.08f * alpha), ui::CornerRadii(h * 0.5f), 1.0f, Fade(White, 0.13f * alpha));
    // A borderless text field inside the pill.
    ui::Style& style = ctx.GetStyle();
    const ui::Theme saved = style.colors;
    style.colors.frameBg = style.colors.frameBgHovered = style.colors.frameBgActive = Color(0, 0, 0, 0);
    style.colors.border = Color(0, 0, 0, 0);
    const f32 fh = ui::GetFrameHeight();
    ui::SetCursorPos(Vec2(pill.min.x + 10.0f * s, Round(pill.Center().y - fh * 0.5f)));
    ui::SetNextItemWidth((pill.Width() - 20.0f * s) / s);
    if (ai.focusInput && ai.openAnim > 0.7f) {
        ui::SetKeyboardFocusHere();
        ai.focusInput = false;
    }
    const bool busy = ai.agent.IsBusy();
    const bool enter = ui::InputText("##ai_input", ai.input, ui::InputTextFlags::EnterReturnsTrue | ui::InputTextFlags::KeepFocusOnEnter,
                                     busy ? "Working..." : "Ask Zeqou AI to build something");
    style.colors = saved;
    const Vec2 bc(row.max.x - h * 0.5f, row.Center().y);
    const bool hasText = !ai.input.Trimmed().Empty();
    const Rect br(bc - Vec2(h * 0.5f), bc + Vec2(h * 0.5f));
    ui::SetCursorPos(br.min);
    const bool clicked = ui::InvisibleButton("##ai_send", br.Size() / s);
    const bool hov = ui::IsItemHovered();
    ui::ItemTooltip(busy ? "Stop" : "Send (Enter)");
    if (busy || hasText) {
        dl.AddShadow(br, Fade(Color(0x5B, 0x6C, 0xFF), (hov ? 0.6f : 0.42f) * alpha), h * 0.5f, 12.0f * s, Vec2(0.0f, 3.0f * s));
        dl.AddRectGradient(br, Fade(AccentA, alpha), Fade(AccentB, alpha), ui::GradientDirection::Vertical, ui::CornerRadii(h * 0.5f));
    } else {
        dl.AddCircle(bc, h * 0.5f, Fade(White, (hov ? 0.16f : 0.1f) * alpha));
    }
    dl.AddTextInRect(Font("Inter"), (busy ? 12.0f : 16.0f) * s, br, Fade(White, alpha * (busy || hasText ? 1.0f : 0.45f)),
                     busy ? ZX_ICON_SQUARE : ZX_ICON_ARROW_UP, ui::TextAlign::Center, false);
    if (busy && clicked) {
        ai.agent.Cancel();
    } else if ((enter || clicked) && hasText && !busy) {
        SendMessage(ed, ai.input);
        ai.input.Clear();
        ai.focusInput = true;
    }
}

} // namespace

void DrawAIPanel(Editor& ed)
{
    if (!ed.ai)
        return;
    AIState& ai = *ed.ai;
    ui::Context& ctx = *ui::GetCurrentContext();
    if (ui::IsShortcutPressed(KeyModifiers::Control, Key::Space)) {
        ai.open = !ai.open;
        ai.focusInput = ai.open;
    }
    if (ai.open && ai.openAnim > 0.5f && ui::IsKeyPressed(Key::Escape, false) && !ed.IsPlaying())
        ai.open = false;
    if (ai.openAnim <= 0.002f || ai.orbRect.Width() <= 0.0f)
        return;
    for (const Editor::CaptureRequest& r : ed.captureRequests)
        if (r.hideOverlays)
            return; // an AI screenshot of the viewport is taken this frame
    const f32 s = ctx.GetScale();
    const Vec2 display(f32(ed.swapChain->GetSize().x), f32(ed.swapChain->GetSize().y));
    const f32 t = ai.openAnim;
    const f32 alpha = Saturate(t * 1.4f);
    // Final rect: below the toolbar on the right; the panel grows out of the orb.
    const f32 w = Min(430.0f * s, display.x - 24.0f * s);
    const f32 top = ai.orbRect.max.y + 12.0f * s;
    const Rect full(Vec2(display.x - w - 12.0f * s, top), Vec2(display.x - 12.0f * s, Min(top + 720.0f * s, display.y - 34.0f * s)));
    const Rect from(ai.orbRect.Center() - Vec2(14.0f * s), ai.orbRect.Center() + Vec2(14.0f * s));
    const Rect panel(Lerp(from.min, full.min, t), Lerp(from.max, full.max, t));
    if (panel.Width() < 4.0f || panel.Height() < 4.0f)
        return;
    const f32 radius = Min(28.0f * s, Min(panel.Width(), panel.Height()) * 0.5f);

    ui::SetNextWindowPos(panel.min / s, ui::Cond::Always);
    ui::SetNextWindowSize(panel.Size() / s, ui::Cond::Always);
    ui::Begin("##ZeqouAI", nullptr,
              ui::WindowFlags::NoTitleBar | ui::WindowFlags::NoResize | ui::WindowFlags::NoMove | ui::WindowFlags::NoScrollbar |
                  ui::WindowFlags::NoCollapse | ui::WindowFlags::NoBackground);
    DrawList& dl = ui::GetWindowDrawList();
    dl.PushClipRect(Rect(Vec2::Zero(), display), 0.0f, false); // the glow spills outside the window
    const f32 time = f32(ai.time);
    const bool busy = ai.agent.IsBusy();

    // Drop shadow, colored light circling the edge (stronger while working), dark glass with a sheen.
    dl.AddShadow(panel, Fade(Color(0, 0, 0), 0.55f * alpha), radius, 42.0f * s, Vec2(0.0f, 16.0f * s));
    const f32 glow = (busy ? 0.8f + 0.2f * Sin(time * 3.0f) : 0.42f) * alpha;
    for (u32 i = 0; i < 4; ++i) {
        const f32 a = time * (busy ? 1.6f : 0.6f) + f32(i) * 1.5708f;
        const Vec2 off(Cos(a) * 8.0f * s, Sin(a) * 8.0f * s);
        dl.AddShadow(panel, Fade(GlowColors[i + 1], glow * 0.55f), radius, (busy ? 32.0f : 24.0f) * s, off, true);
    }
    dl.AddRectGradient(panel, Fade(Color(0x22, 0x22, 0x2E), 0.99f * alpha), Fade(Color(0x12, 0x12, 0x18), alpha), ui::GradientDirection::Vertical,
                       ui::CornerRadii(radius), 1.0f, Fade(White, 0.15f * alpha));
    dl.PushClipRect(panel, radius);
    dl.AddRectGradient(Rect(panel.min, Vec2(panel.max.x, panel.min.y + 90.0f * s)), Fade(White, 0.07f * alpha), Color(255, 255, 255, 0),
                       ui::GradientDirection::Vertical);
    dl.PopClipRect();
    dl.PopClipRect();

    const f32 contentAlpha = Saturate((t - 0.55f) / 0.4f);
    if (contentAlpha > 0.01f && panel.Height() > 160.0f * s && panel.Width() > 200.0f * s) {
        dl.PushClipRect(panel, radius);
        const f32 pad = 18.0f * s;
        const f32 innerW = panel.Width() - pad * 2.0f;
        // Header: orb, name, model / state, buttons.
        const Vec2 orbC(panel.min.x + pad + 15.0f * s, panel.min.y + pad + 15.0f * s);
        DrawOrb(dl, orbC, 15.0f * s, time, busy ? 0.9f : 0.2f, contentAlpha);
        const f32 textX = orbC.x + 25.0f * s;
        dl.AddTextInRect(Font("Inter SemiBold"), 15.5f * s, Rect(Vec2(textX, orbC.y - 18.0f * s), Vec2(panel.max.x - 150.0f * s, orbC.y + 1.0f * s)),
                         Fade(White, contentAlpha), "Zeqou AI");
        const String client = ai.mcp.IsRunning() ? ai.mcp.LastClient() : String();
        String status = busy ? String("Working...") : Configured(ai.settings) ? ModelLabel(ai.settings) : String();
        if (!client.Empty())
            status += String::Format("{}{} connected", status.Empty() ? "" : "  \xC2\xB7  ", client);
        else if (ai.mcp.IsRunning())
            status += status.Empty() ? "MCP server on" : "  \xC2\xB7  MCP on";
        if (status.Empty())
            status = "Not set up";
        dl.AddTextInRect(Font("Inter"), 11.5f * s, Rect(Vec2(textX, orbC.y + 1.0f * s), Vec2(panel.max.x - 150.0f * s, orbC.y + 18.0f * s)),
                         Fade(busy ? Color(0xB8, 0xC4, 0xFF) : Color(0x8E, 0x90, 0xA6), contentAlpha), status);
        const f32 br = 15.0f * s;
        Vec2 bc(panel.max.x - pad - br, orbC.y);
        if (GlassIconButton("##ai_close", ZX_ICON_X, bc, br, contentAlpha, "Close (Esc)"))
            ai.open = false;
        bc.x -= br * 2.0f + 8.0f * s;
        if (GlassIconButton("##ai_settings", ZX_ICON_SETTINGS, bc, br, contentAlpha, "Settings > AI"))
            OpenSettingsPage(ed, "AI");
        bc.x -= br * 2.0f + 8.0f * s;
        if (GlassIconButton("##ai_page", ai.page == 1 ? ZX_ICON_MESSAGE_CIRCLE : ZX_ICON_PLUG, bc, br, contentAlpha,
                            ai.page == 1 ? "Back to the chat" : "Connect an AI agent (MCP)"))
            ai.page = ai.page == 1 ? 0 : 1;
        bc.x -= br * 2.0f + 8.0f * s;
        if (ai.page == 0 && !ai.chat.Empty() && !busy && GlassIconButton("##ai_new", ZX_ICON_SQUARE_PEN, bc, br, contentAlpha, "New chat")) {
            ai.agent.Reset();
            ai.chat.Clear();
        }

        const f32 headerH = pad + 30.0f * s + 16.0f * s;
        const f32 inputH = 44.0f * s;
        const Rect body(Vec2(panel.min.x + pad, panel.min.y + headerH),
                        Vec2(panel.max.x - pad + 8.0f * s, panel.max.y - (ai.page == 0 ? inputH + pad * 1.6f : pad)));
        dl.AddRect(Rect(Vec2(panel.min.x + pad, body.min.y - 8.0f * s), Vec2(panel.max.x - pad, body.min.y - 7.0f * s)), Fade(White, 0.07f * contentAlpha));
        ui::SetCursorPos(body.min);
        if (ui::BeginChild("##ai_body", body.Size() / s, false)) {
            const f32 cw = innerW - 4.0f * s;
            ui::Dummy(Vec2(0.0f, 4.0f));
            if (ai.page == 1) {
                ui::PushFont(ui::FontStyle::SemiBold, 15.0f);
                ui::Text("Connect an AI agent");
                ui::PopFont();
                ui::TextWrapped("Any agent that speaks the Model Context Protocol can build in this editor: create objects, write "
                                "scripts, run commands, play and take screenshots.");
                ui::Spacing();
                DrawMcpConnect(ed, cw, contentAlpha);
            } else if (ai.chat.Empty()) {
                DrawWelcome(ed, cw, contentAlpha);
            } else {
                for (AIChatItem& item : ai.chat)
                    DrawChatItem(ed, item, cw, contentAlpha);
                const bool runningTool = ai.chat.Back().kind == AIChatItem::Kind::Tool && ai.chat.Back().running;
                if (busy && !runningTool)
                    DrawTyping(ed, contentAlpha);
                const ai::AgentUsage u = ai.agent.Usage();
                if (u.requests && !busy) {
                    auto tokens = [](u64 n) { return n < 1000 ? String::Format("{}", n) : String::Format("{:.1f}k", f64(n) / 1000.0); };
                    String usage = String::Format("{} requests  \xC2\xB7  {} tokens in", u.requests, tokens(u.inputTokens + u.cacheReadTokens));
                    if (u.cacheReadTokens)
                        usage.Appendf(" ({} cached)", tokens(u.cacheReadTokens));
                    usage.Appendf("  \xC2\xB7  {} out", tokens(u.outputTokens));
                    ui::TextDisabled(usage);
                }
                ui::Dummy(Vec2(0.0f, 6.0f));
                if (ai.scrollToBottom) {
                    ui::SetScrollHereY(1.0f);
                    ai.scrollToBottom = false;
                }
            }
        }
        ui::EndChild();
        if (ai.page == 0)
            DrawInputRow(ed, Rect(Vec2(panel.min.x + pad, panel.max.y - pad - inputH), Vec2(panel.max.x - pad, panel.max.y - pad)), contentAlpha);
        dl.PopClipRect();
    }
    ui::End();
}

// --- Settings > AI --------------------------------------------------------------------------------------------------

void DrawAISettingsPage(Editor& ed)
{
    if (!ed.ai)
        return;
    AIState& ai = *ed.ai;
    AISettings& st = ai.settings;
    bool changed = false;
    ui::SeparatorText("Built-in assistant");
    ui::TextWrapped("Zeqou AI talks to the model with your own API key. The key is encrypted for your Windows account and "
                    "stored in %LOCALAPPDATA%/ZeqouX3D/AI.json - never in a project.");
    ui::Spacing();
    i32 provider = i32(st.provider);
    if (ui::Combo("Provider", &provider, Span<const StringView>(ProviderNames, 3))) {
        st.provider = ai::Provider(provider);
        st.model.Clear();
        changed = true;
    }
    String& key = st.apiKeys[u32(st.provider)];
    changed |= ui::InputText("API Key", key, ui::InputTextFlags::Password,
                             st.provider == ai::Provider::Anthropic ? "sk-ant-..." : st.provider == ai::Provider::OpenAI ? "sk-..." : "(optional)");
    if (st.provider == ai::Provider::Anthropic) {
        if (ui::TextLink("Get a Claude API key (console.anthropic.com)"))
            platform::OpenUrl("https://console.anthropic.com/settings/keys");
    } else if (st.provider == ai::Provider::OpenAI) {
        if (ui::TextLink("Get an OpenAI API key (platform.openai.com)"))
            platform::OpenUrl("https://platform.openai.com/api-keys");
    }
    changed |= ui::InputText("Model", st.model, ui::InputTextFlags::None,
                             st.provider == ai::Provider::OpenAICompatible ? StringView("model name, e.g. qwen3-coder") : ai::DefaultModel(st.provider));
    if (st.provider == ai::Provider::OpenAICompatible)
        changed |= ui::InputText("Server URL", st.baseUrl, ui::InputTextFlags::None, "http://localhost:11434/v1 (Ollama, LM Studio, OpenRouter...)");
    if (st.provider == ai::Provider::Anthropic) {
        i32 effort = 2;
        for (i32 i = 0; i < 5; ++i)
            if (st.effort == Efforts[i])
                effort = i;
        if (ui::Combo("Effort", &effort, Span<const StringView>(Efforts, 5))) {
            st.effort = Efforts[effort];
            changed = true;
        }
        ui::ItemTooltip("How hard the model thinks: higher builds better, takes longer and uses more tokens");
        changed |= ui::Checkbox("Fallback on declined requests", &st.serverFallbacks);
        ui::ItemTooltip("If the model declines a request, Anthropic re-runs it on its recommended fallback model (beta)");
    }
    changed |= ui::SliderInt("Max Tokens", &st.maxTokens, 2048, 64000);
    ui::ItemTooltip("The longest single answer (code included)");
    if (changed)
        SaveAISettings(st);
    ui::Spacing();
    ui::SeparatorText("MCP server (external agents)");
    i32 port = st.mcpPort;
    if (ui::InputInt("Port", &port) && port != st.mcpPort) {
        st.mcpPort = Clamp(port, 1024, 65535);
        ai.mcpError.Clear();
        SaveAISettings(st);
    }
    ui::LabelText("Token", st.mcpToken);
    if (ui::Button(ZX_ICON_REFRESH_CW "  New Token")) {
        st.mcpToken = platform::RandomHex(16);
        SaveAISettings(st);
        ai.mcp.Stop(); // restarts with the new token next frame
        ai.mcpError.Clear();
    }
    ui::ItemTooltip("Connected agents must be set up again with the new token");
    ui::Spacing();
    DrawMcpConnect(ed, ui::GetContentRegionAvail().x - 12.0f * ui::GetCurrentContext()->GetScale(), 1.0f);
}

} // namespace zx::editor
