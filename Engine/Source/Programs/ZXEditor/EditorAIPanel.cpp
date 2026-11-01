// Zeqou AI: providers and models, settings (%LOCALAPPDATA%/ZeqouX3D/AI.json, API keys DPAPI-encrypted), the toolbar
// button, the chat panel (Markdown replies, tool activity, model picker, context ring), the MCP connection page and
// Settings > AI.
//
// Look: the editor's own palette (theme colors, one accent, 1 px borders, small radii) - a docked tool panel, not a
// floating glass card.

#include "EditorAI.h"
#include "Editor.h"
#include "Core/Console/Console.h"
#include "Core/Containers/Algorithm.h"
#include "Core/FileSystem/FileSystem.h"
#include "Core/HAL/PlatformFile.h"
#include "Core/String/Path.h"
#include "Core/String/Utf.h"
#include "Core/Time/Time.h"
#include "Core/Version.h"
#include "Platform/Window.h"

ZX_DEFINE_LOG_CATEGORY_STATIC(LogAI, Info);

namespace zx::editor {

// --- Providers ------------------------------------------------------------------------------------------------------

namespace {

using ai::Provider;

// Every provider except Anthropic speaks the OpenAI Chat Completions format at its base URL.
constexpr AIProviderInfo ProviderTable[] = {
    {"anthropic", "Anthropic", Provider::Anthropic, "https://api.anthropic.com", "claude-opus-5-5", "https://console.anthropic.com/settings/keys", "sk-ant-...", true, false, ""},
    {"openai", "OpenAI", Provider::OpenAI, "https://api.openai.com/v1", "gpt-5", "https://platform.openai.com/api-keys", "sk-...", true, false, ""},
    {"gemini", "Google Gemini", Provider::OpenAICompatible, "https://generativelanguage.googleapis.com/v1beta/openai", "gemini-2.5-pro", "https://aistudio.google.com/apikey", "AIza...", true, false, ""},
    {"xai", "xAI Grok", Provider::OpenAICompatible, "https://api.x.ai/v1", "grok-4", "https://console.x.ai", "xai-...", true, false, ""},
    {"deepseek", "DeepSeek", Provider::OpenAICompatible, "https://api.deepseek.com/v1", "deepseek-chat", "https://platform.deepseek.com/api_keys", "sk-...", true, false, ""},
    {"mistral", "Mistral", Provider::OpenAICompatible, "https://api.mistral.ai/v1", "mistral-large-latest", "https://console.mistral.ai/api-keys", "", true, false, ""},
    {"openrouter", "OpenRouter", Provider::OpenAICompatible, "https://openrouter.ai/api/v1", "anthropic/claude-sonnet-4.5", "https://openrouter.ai/keys", "sk-or-...", true, false, ""},
    {"groq", "Groq", Provider::OpenAICompatible, "https://api.groq.com/openai/v1", "llama-3.3-70b-versatile", "https://console.groq.com/keys", "gsk_...", true, false, ""},
    {"cerebras", "Cerebras", Provider::OpenAICompatible, "https://api.cerebras.ai/v1", "gpt-oss-120b", "https://cloud.cerebras.ai", "csk-...", true, false, ""},
    {"together", "Together AI", Provider::OpenAICompatible, "https://api.together.xyz/v1", "Qwen/Qwen3-Coder-480B-A35B-Instruct-FP8", "https://api.together.ai/settings/api-keys", "", true, false, ""},
    {"fireworks", "Fireworks AI", Provider::OpenAICompatible, "https://api.fireworks.ai/inference/v1", "accounts/fireworks/models/kimi-k2-instruct", "https://fireworks.ai/account/api-keys", "fw_...", true, false, ""},
    {"perplexity", "Perplexity", Provider::OpenAICompatible, "https://api.perplexity.ai", "sonar-pro", "https://www.perplexity.ai/settings/api", "pplx-...", true, false, ""},
    {"moonshot", "Moonshot (Kimi)", Provider::OpenAICompatible, "https://api.moonshot.ai/v1", "kimi-k2-0905-preview", "https://platform.moonshot.ai/console/api-keys", "sk-...", true, false, ""},
    {"zai", "Z.ai (GLM)", Provider::OpenAICompatible, "https://api.z.ai/api/paas/v4", "glm-4.6", "https://z.ai/manage-apikey/apikey-list", "", true, false, ""},
    {"qwen", "Alibaba Qwen", Provider::OpenAICompatible, "https://dashscope-intl.aliyuncs.com/compatible-mode/v1", "qwen3-coder-plus", "https://modelstudio.console.alibabacloud.com", "sk-...", true, false, ""},
    {"nvidia", "NVIDIA NIM", Provider::OpenAICompatible, "https://integrate.api.nvidia.com/v1", "meta/llama-3.3-70b-instruct", "https://build.nvidia.com", "nvapi-...", true, false, ""},
    {"github", "GitHub Models", Provider::OpenAICompatible, "https://models.github.ai/inference", "openai/gpt-4.1", "https://github.com/settings/tokens", "github_pat_...", true, false, "https://models.github.ai/catalog/models"},
    {"huggingface", "Hugging Face", Provider::OpenAICompatible, "https://router.huggingface.co/v1", "openai/gpt-oss-120b", "https://huggingface.co/settings/tokens", "hf_...", true, false, ""},
    {"ollama", "Ollama (local)", Provider::OpenAICompatible, "http://localhost:11434/v1", "", "", "", false, true, ""},
    {"lmstudio", "LM Studio (local)", Provider::OpenAICompatible, "http://localhost:1234/v1", "", "", "", false, true, ""},
    {"llamacpp", "llama.cpp / vLLM (local)", Provider::OpenAICompatible, "http://localhost:8080/v1", "", "", "", false, true, ""},
    {"custom", "Custom (OpenAI-compatible)", Provider::OpenAICompatible, "", "", "", "(optional)", false, true, ""},
};
static_assert(ZX_ARRAY_COUNT(ProviderTable) <= MaxAIProviders);

constexpr StringView Efforts[] = {"low", "medium", "high", "xhigh", "max"};
constexpr u32 DefaultContext = 128000;
constexpr u32 MinContext = 1000;

/// %LOCALAPPDATA%/ZeqouX3D/AI.json; -aisettings=<file> replaces it (automated tests).
String SettingsPath()
{
    StringView file;
    if (CommandLine::GetValue("aisettings", file))
        return Path::Normalize(platform::GetAbsolutePath(file));
    return Path::Join(Path::Normalize(platform::GetUserLocalAppDataDirectory()), "ZeqouX3D/AI.json");
}

const AIProviderInfo& Info(u32 provider) { return ProviderTable[Min<u32>(provider, ZX_ARRAY_COUNT(ProviderTable) - 1)]; }

String BaseUrl(const AISettings& s, u32 provider)
{
    const String& custom = s.baseUrls[provider];
    return custom.Trimmed().Empty() ? String(Info(provider).baseUrl) : custom.Trimmed();
}

/// A key is needed and present, or the provider needs none (local servers: a URL is enough).
bool ProviderReady(const AISettings& s, u32 provider)
{
    const AIProviderInfo& p = Info(provider);
    if (p.needsKey)
        return !s.apiKeys[provider].Trimmed().Empty();
    return !BaseUrl(s, provider).Empty();
}

String OverrideKey(StringView providerId, StringView model) { return String::Format("{}/{}", providerId, model); }

/// Identifies what a model list was fetched for: a different URL or key means a new list.
String ListKey(const AISettings& s, u32 provider)
{
    u64 h = 1469598103934665603ull;
    for (char c : s.apiKeys[provider].View())
        h = (h ^ u8(c)) * 1099511628211ull;
    return String::Format("{}|{}|{:x}", Info(provider).id, BaseUrl(s, provider), h);
}

String FormatTokens(u64 n)
{
    if (n >= 1000000)
        return n % 1000000 == 0 ? String::Format("{}M", n / 1000000) : String::Format("{:.2f}M", f64(n) / 1e6);
    if (n >= 10000)
        return String::Format("{}k", (n + 500) / 1000);
    if (n >= 1000)
        return String::Format("{:.1f}k", f64(n) / 1000.0);
    return String::Format("{}", n);
}

} // namespace

Span<const AIProviderInfo> AIProviders() { return Span<const AIProviderInfo>(ProviderTable, ZX_ARRAY_COUNT(ProviderTable)); }

u32 FindAIProvider(StringView id)
{
    for (u32 i = 0; i < ZX_ARRAY_COUNT(ProviderTable); ++i)
        if (id == ProviderTable[i].id)
            return i;
    return U32Max;
}

// --- Models and the context window ----------------------------------------------------------------------------------

namespace {

/// The model id a provider would use (the choice, else its default, else the first of its fetched list).
String ModelFor(const AIState& ai, u32 provider)
{
    if (!ai.settings.models[provider].Trimmed().Empty())
        return ai.settings.models[provider].Trimmed();
    if (Info(provider).defaultModel[0] != '\0')
        return Info(provider).defaultModel;
    const AIModelList& list = ai.lists[provider];
    return list.models.Empty() ? String() : list.models[0].id;
}

const ai::ModelInfo* FindModel(const AIState& ai, u32 provider, StringView id)
{
    for (const ai::ModelInfo& m : ai.lists[provider].models)
        if (m.id == id)
            return &m;
    return nullptr;
}

/// A short readable name: "claude-opus-5-5" -> "Claude Opus 5.5", provider paths dropped.
String DisplayName(const AIState& ai, u32 provider, StringView id)
{
    if (const ai::ModelInfo* m = FindModel(ai, provider, id); m && !m->name.Empty())
        return m->name;
    if (id.StartsWith("claude-")) {
        String out = "Claude";
        StringView rest = id.DropLeft(7);
        String version;
        while (!rest.Empty()) {
            const usize dash = rest.Find('-');
            const StringView part = dash == StringView::NPos ? rest : rest.Left(dash);
            rest = dash == StringView::NPos ? StringView() : rest.DropLeft(dash + 1);
            if (part.Size() == 8 && IsDigitAscii(part[0]))
                continue; // a date suffix
            if (!part.Empty() && IsDigitAscii(part[0])) {
                version += version.Empty() ? "" : ".";
                version += part;
            } else if (!part.Empty()) {
                out += ' ';
                out += char(part[0] >= 'a' && part[0] <= 'z' ? part[0] - 32 : part[0]);
                out += part.DropLeft(1);
            }
        }
        if (!version.Empty())
            out.Appendf(" {}", version);
        return out;
    }
    const usize slash = id.RFind('/');
    return String(slash == StringView::NPos ? id : id.DropLeft(slash + 1));
}

/// Context window of a model and where it came from.
u32 ContextOf(const AIState& ai, u32 provider, StringView model, StringView* source)
{
    StringView dummy;
    StringView& src = source ? *source : dummy;
    if (const u32* manual = ai.settings.contextOverrides.Find(OverrideKey(Info(provider).id, model)); manual && *manual > 0) {
        src = "manual";
        return Clamp(*manual, MinContext, ai::MaxContextWindow);
    }
    if (const ai::ModelInfo* m = FindModel(ai, provider, model); m && m->contextWindow > 0) {
        src = "provider";
        return Clamp(m->contextWindow, MinContext, ai::MaxContextWindow);
    }
    if (const u32 known = ai::KnownContextWindow(model)) {
        src = "known";
        return known;
    }
    src = "default";
    return DefaultContext;
}

} // namespace

String AIModel(const AIState& ai) { return ModelFor(ai, ai.settings.provider); }

u32 AIContextWindow(const AIState& ai) { return ContextOf(ai, ai.settings.provider, AIModel(ai), nullptr); }

StringView AIContextSource(const AIState& ai)
{
    StringView src;
    ContextOf(ai, ai.settings.provider, AIModel(ai), &src);
    return src;
}

void RefreshAIModels(AIState& ai, u32 provider, bool force)
{
    if (provider >= ZX_ARRAY_COUNT(ProviderTable) || ai.fetching.load())
        return;
    AIModelList& list = ai.lists[provider];
    const String key = ListKey(ai.settings, provider);
    if (!force && list.fetchedFor == key && (list.fetched || ai.time - list.attemptedAt < 30.0))
        return; // fresh, or failed recently (retried automatically after a while)
    if (!ProviderReady(ai.settings, provider)) {
        list = AIModelList{};
        list.fetchedFor = key;
        list.error = Info(provider).needsKey ? "Add an API key to list the models." : "Set the server URL.";
        list.attemptedAt = ai.time;
        return;
    }
    list.fetchedFor = key;
    list.attemptedAt = ai.time;
    if (ai.fetchThread.IsJoinable())
        ai.fetchThread.Join();
    platform::HttpRequestDesc req = ai::BuildModelListRequest(Info(provider).wire, BaseUrl(ai.settings, provider), ai.settings.apiKeys[provider].Trimmed());
    if (Info(provider).modelsUrl[0] != '\0')
        req.url = Info(provider).modelsUrl;
    ai.fetching = true;
    ai.fetchCancel = false;
    ai.fetchProvider = provider;
    AIState* state = &ai;
    if (!ai.fetchThread.Start("AIModels", [state, req = move(req), key] {
            AIModelList result;
            result.fetchedFor = key;
            const platform::HttpResponse res = platform::HttpRequest(req, &state->fetchCancel);
            if (res.status == 200) {
                if (ai::ParseModelList(res.body, result.models))
                    result.fetched = true;
                else
                    result.error = "The model list could not be read.";
            } else if (res.status == 0) {
                result.error = String::Format("Could not reach the server: {}", res.error);
            } else {
                result.error = ai::ApiErrorMessage(res.status, res.body);
            }
            ScopedLock lock(state->fetchMutex);
            state->pending = move(result);
            state->pendingReady = true;
            state->fetching = false;
        })) {
        ai.fetching = false;
        list.error = "Could not start the request.";
    }
}

void SelectAIModel(AIState& ai, u32 provider, StringView model)
{
    if (provider >= ZX_ARRAY_COUNT(ProviderTable))
        return;
    ai.settings.provider = provider;
    ai.settings.models[provider] = model;
    SaveAISettings(ai.settings);
}

// --- Settings -------------------------------------------------------------------------------------------------------

void LoadAISettings(AISettings& s)
{
    String text;
    JsonValue doc;
    if (fs::ReadTextFile(SettingsPath(), text) && ParseJson(text, doc)) {
        // 0.17.8 wrote "openai-compatible" with one model / URL for the active provider.
        StringView providerId = doc.Get("provider").AsString("anthropic");
        if (providerId == "openai-compatible")
            providerId = "custom";
        const u32 p = FindAIProvider(providerId);
        s.provider = p == U32Max ? 0 : p;
        for (const JsonMember& m : doc.Get("keys").Members()) {
            const u32 i = FindAIProvider(m.first == "openai-compatible" ? StringView("custom") : m.first.View());
            String key;
            if (i != U32Max && platform::UnprotectSecret(m.second.AsString(), key))
                s.apiKeys[i] = move(key);
        }
        for (const JsonMember& m : doc.Get("models").Members())
            if (const u32 i = FindAIProvider(m.first); i != U32Max)
                s.models[i] = m.second.AsString();
        for (const JsonMember& m : doc.Get("urls").Members())
            if (const u32 i = FindAIProvider(m.first); i != U32Max)
                s.baseUrls[i] = m.second.AsString();
        if (doc.Get("model").IsString() && s.models[s.provider].Empty())
            s.models[s.provider] = doc.Get("model").AsString();
        if (doc.Get("baseUrl").IsString() && s.baseUrls[s.provider].Empty())
            s.baseUrls[s.provider] = doc.Get("baseUrl").AsString();
        for (const JsonMember& m : doc.Get("contextWindows").Members())
            if (const u64 n = m.second.AsU64())
                s.contextOverrides.InsertOrAssign(m.first, Clamp(u32(Min<u64>(n, ai::MaxContextWindow)), MinContext, ai::MaxContextWindow));
        s.effort = doc.Get("effort").AsString("high");
        s.maxTokens = i32(doc.Get("maxTokens").AsInt(16000));
        s.serverFallbacks = doc.Get("serverFallbacks").AsBool(true);
        s.panelWidth = Clamp(doc.Get("panelWidth").AsFloat(440.0f), 340.0f, 900.0f);
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
    doc.Set("provider", Info(s.provider).id);
    JsonValue models = JsonValue::MakeObject(), urls = JsonValue::MakeObject(), keys = JsonValue::MakeObject();
    for (u32 i = 0; i < ZX_ARRAY_COUNT(ProviderTable); ++i) {
        if (!s.models[i].Empty())
            models.Set(ProviderTable[i].id, s.models[i]);
        if (!s.baseUrls[i].Empty())
            urls.Set(ProviderTable[i].id, s.baseUrls[i]);
        if (!s.apiKeys[i].Empty())
            keys.Set(ProviderTable[i].id, platform::ProtectSecret(s.apiKeys[i]));
    }
    doc.Set("models", move(models));
    doc.Set("urls", move(urls));
    doc.Set("keys", move(keys));
    JsonValue contexts = JsonValue::MakeObject();
    Array<String> names;
    for (const auto& e : s.contextOverrides)
        names.PushBack(e.first);
    Sort(names); // stable file content
    for (const String& n : names)
        contexts.Set(n, *s.contextOverrides.Find(n));
    doc.Set("contextWindows", move(contexts));
    doc.Set("effort", s.effort);
    doc.Set("maxTokens", s.maxTokens);
    doc.Set("serverFallbacks", s.serverFallbacks);
    doc.Set("panelWidth", s.panelWidth);
    doc["mcp"].Set("enabled", s.mcpEnabled);
    doc["mcp"].Set("port", s.mcpPort);
    doc["mcp"].Set("token", s.mcpToken);
    const String path = SettingsPath();
    platform::CreateDirectoryTree(Path::Parent(path));
    if (!fs::WriteTextFile(path, ToJsonString(doc)))
        ZX_LOG_WARN(LogAI, "Could not save the AI settings to {}", path);
}

// --- Sending --------------------------------------------------------------------------------------------------------

namespace {

void SendMessage(Editor& ed, StringView text)
{
    AIState& ai = *ed.ai;
    const String trimmed = String(text).Trimmed();
    if (trimmed.Empty() || ai.agent.IsBusy())
        return;
    const u32 provider = ai.settings.provider;
    if (!ProviderReady(ai.settings, provider)) {
        AIChatItem e{AIChatItem::Kind::Error,
                     Info(provider).needsKey ? String::Format("Add your {} API key in Settings > AI first (or connect an agent over MCP).", Info(provider).name)
                                             : String("Set the server URL in Settings > AI first.")};
        ai.chat.PushBack(move(e));
        return;
    }
    RefreshAIModels(ai, provider);
    const String model = AIModel(ai);
    if (model.Empty()) {
        ai.chat.PushBack(AIChatItem{AIChatItem::Kind::Error, "Choose a model first (click the model name under the message field)."});
        return;
    }
    ai::AgentConfig c;
    c.provider = Info(provider).wire;
    c.apiKey = ai.settings.apiKeys[provider].Trimmed();
    c.model = model;
    c.baseUrl = BaseUrl(ai.settings, provider);
    c.effort = c.provider == Provider::Anthropic ? ai.settings.effort : String();
    c.maxTokens = u32(Clamp(ai.settings.maxTokens, 1024, 128000));
    c.serverFallbacks = ai.settings.serverFallbacks && c.provider == Provider::Anthropic;
    c.contextWindow = AIContextWindow(ai);
    const String system = McpInstructions() +
                          "\n\nYou are Zeqou AI, the assistant built into the editor: the user chats with you in a side panel "
                          "while watching the viewport. Reply in the user's language. Act with the tools rather than "
                          "explaining how; keep replies short and say what you changed. Markdown is rendered (headings, "
                          "lists, bold, inline code, fenced code blocks).";
    if (ai.agent.Send(c, system, trimmed)) {
        ai.chat.PushBack(AIChatItem{AIChatItem::Kind::User, trimmed});
        ai.stickToBottom = true;
        ai.turnStarted = ai.time;
    } else if (c.apiKey.Empty() && c.provider != Provider::OpenAICompatible) {
        ai.chat.PushBack(AIChatItem{AIChatItem::Kind::Error, "Add an API key in Settings > AI first."});
    }
}

void NewChat(AIState& ai)
{
    if (ai.agent.IsBusy())
        return;
    ai.agent.Reset();
    ai.chat.Clear();
    ai.lastContextTokens = 0;
    ai.focusInput = true;
}

} // namespace

void SendAIMessage(Editor& ed, StringView text)
{
    if (ed.ai)
        SendMessage(ed, text);
}

// --- State ----------------------------------------------------------------------------------------------------------

AIState::AIState(Editor& ed) : host(ed), agent(host) {}

AIState::~AIState()
{
    host.Shutdown();
    mcp.Stop();
    agent.Cancel();
    agent.Wait();
    fetchCancel = true;
    if (fetchThread.IsJoinable())
        fetchThread.Join();
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
    ed.ai->fetchCancel = true;
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
    ai.frameDt = dt;
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

    // The model list (and with it the context window) of the active provider is fetched as soon as the panel is open,
    // so the ring and the trimming use what the provider reports. Cheap: it is requested once per URL / key.
    if (ai.open && ProviderReady(ai.settings, ai.settings.provider))
        RefreshAIModels(ai, ai.settings.provider);

    // A finished model list.
    {
        ScopedLock lock(ai.fetchMutex);
        if (ai.pendingReady) {
            ai.pendingReady = false;
            AIModelList& list = ai.lists[ai.fetchProvider];
            if (list.fetchedFor == ai.pending.fetchedFor) { // still the same URL / key
                const f64 attempted = list.attemptedAt;
                list = move(ai.pending);
                list.attemptedAt = attempted;
                if (list.fetched)
                    ZX_LOG_INFO(LogAI, "{}: {} models", Info(ai.fetchProvider).name, list.models.Size());
            }
            ai.pending = AIModelList{};
        }
    }
    if (ai.fetchThread.IsJoinable() && !ai.fetching.load())
        ai.fetchThread.Join();

    // Agent events -> chat.
    for (ai::AgentEvent& e : ai.agent.TakeEvents()) {
        switch (e.type) {
        case ai::AgentEventType::Text: ai.chat.PushBack(AIChatItem{AIChatItem::Kind::Assistant, move(e.text)}); break;
        case ai::AgentEventType::ToolStart: {
            AIChatItem item{AIChatItem::Kind::Tool, move(e.text)};
            item.tool = move(e.tool);
            item.running = true;
            item.started = ai.time;
            ai.chat.PushBack(move(item));
            break;
        }
        case ai::AgentEventType::ToolEnd:
            for (isize i = isize(ai.chat.Size()) - 1; i >= 0; --i) {
                AIChatItem& item = ai.chat[usize(i)];
                if (item.kind == AIChatItem::Kind::Tool && item.running && item.tool == e.tool) {
                    item.running = false;
                    item.isError = e.isError;
                    item.duration = f32(ai.time - item.started);
                    // Up to 12 lines / 1500 bytes of the result for the expanded view.
                    StringView r = e.text.View();
                    usize cut = 0, lines = 0;
                    while (cut < r.Size() && lines < 12 && cut < 1500)
                        lines += r[cut++] == '\n';
                    item.result = r.Left(Utf8TruncateBytes(r, cut));
                    item.result.TrimInPlace();
                    if (cut < r.Size())
                        item.result += "\n\xE2\x80\xA6";
                    break;
                }
            }
            break;
        case ai::AgentEventType::Error: ai.chat.PushBack(AIChatItem{AIChatItem::Kind::Error, move(e.text)}); break;
        case ai::AgentEventType::Notice: ai.chat.PushBack(AIChatItem{AIChatItem::Kind::Notice, move(e.text)}); break;
        default: break;
        }
    }
    for (AIChatItem& item : ai.chat)
        item.age += dt;
    const ai::AgentUsage usage = ai.agent.Usage();
    if (usage.contextTokens)
        ai.lastContextTokens = usage.contextTokens;

    // Open / close: a critically damped spring (eases to the target, no overshoot).
    {
        // Exact solution of x'' = -w^2 (x - target) - 2 w x' (critically damped), so every frame time is stable. The
        // previous explicit step was stable only for frames shorter than about 40 ms (w * dt < 0.83, w = sqrt(420));
        // longer frames made it overshoot and bounce around the target.
        const f32 target = ai.open ? 1.0f : 0.0f;
        const f32 w = Sqrt(420.0f);
        const f32 e = Exp(-w * dt);
        const f32 d = ai.openAnim - target; // displacement from the target
        const f32 b = ai.openVel + w * d;
        const f32 newD = (d + b * dt) * e;
        ai.openVel = (ai.openVel - w * b * dt) * e;
        ai.openAnim = target + newD;
        if (Abs(ai.openAnim - target) < 0.001f && Abs(ai.openVel) < 0.01f) {
            ai.openAnim = target;
            ai.openVel = 0.0f;
        }
        ai.openAnim = Clamp(ai.openAnim, 0.0f, 1.0f);
    }
}

// --- Toolbar button -------------------------------------------------------------------------------------------------

namespace {

ui::FontHandle Font(StringView name) { return ui::GetCurrentContext()->Fonts().FindFont(name); }

/// The context ring: a track and an arc filled to `fraction` (green / amber / red as it fills).
void DrawRing(DrawList& dl, Vec2 c, f32 r, f32 thickness, f32 fraction, const ui::Theme& t)
{
    dl.AddCircleOutline(c, r, WithAlpha(t.text, 0.16f), thickness);
    const f32 f = Saturate(fraction);
    if (f <= 0.0f)
        return;
    const Color col = f < 0.7f ? t.accentHovered : f < 0.9f ? t.warning : t.error;
    const u32 segments = Max(2u, u32(48.0f * f));
    Vec2 pts[50];
    for (u32 i = 0; i <= segments; ++i) {
        const f32 a = -HalfPi + TwoPi * f * f32(i) / f32(segments);
        pts[i] = c + Vec2(Cos(a), Sin(a)) * r;
    }
    dl.AddPolyline(Span<const Vec2>(pts, segments + 1), col, thickness);
}

} // namespace

void DrawAIOrb(Editor& ed, const Rect& r)
{
    if (!ed.ai)
        return;
    AIState& ai = *ed.ai;
    ui::Context& ctx = *ui::GetCurrentContext();
    const f32 s = ctx.GetScale();
    const ui::Theme& t = ctx.GetStyle().colors;
    // A labelled button: "✦ AI", accent while the panel is open, a pulsing dot while the agent works.
    const ui::FontHandle font = Font("Inter Medium");
    const f32 fs = 12.5f * s;
    const String label = ZX_ICON_SPARKLES "  AI";
    const f32 w = Round(ctx.Fonts().MeasureWidth(font, fs, label) + 22.0f * s);
    const Rect br(Vec2(r.max.x - w, r.min.y), r.max);
    ui::SetCursorPos(br.min);
    const bool clicked = ui::InvisibleButton("##zeqou_ai_button", br.Size() / s);
    const bool hovered = ui::IsItemHovered();
    ui::ItemTooltip(ai.open ? "Close Zeqou AI (Ctrl+Space)" : "Zeqou AI - build with words (Ctrl+Space)");
    if (clicked) {
        ai.open = !ai.open;
        ai.focusInput = ai.open;
    }
    ai.buttonRect = br;
    DrawList& dl = ui::GetWindowDrawList();
    const f32 rad = 5.0f * s;
    if (ai.open)
        dl.AddRect(br, WithAlpha(t.accent, hovered ? 0.32f : 0.22f), ui::CornerRadii(rad), 1.0f, WithAlpha(t.accent, 0.7f));
    else
        dl.AddRect(br, hovered ? t.buttonHovered : t.button, ui::CornerRadii(rad), 1.0f, t.border);
    dl.AddTextInRect(font, fs, br, ai.open ? t.accentText : t.text, label, ui::TextAlign::Center, false);
    if (ai.agent.IsBusy() || ai.mcp.IsRunning()) {
        const bool busy = ai.agent.IsBusy();
        const Vec2 p(br.max.x - 3.0f * s, br.min.y + 3.0f * s);
        const f32 pulse = busy ? 0.55f + 0.45f * Sin(f32(ai.time) * 5.0f) : 1.0f;
        dl.AddCircle(p, 3.5f * s, WithAlpha(busy ? t.accentHovered : t.success, pulse), 1.5f * s, t.menuBarBg);
    }
}

// --- Markdown -------------------------------------------------------------------------------------------------------
//
// Assistant replies are laid out as blocks (paragraphs, headings, list items, quotes, code blocks, rules) and inline
// runs (plain, bold, italic, code, link). Word wrapping is done here so styled runs flow together on one line.

namespace {

enum class Style : u8
{
    Plain,
    Bold,
    Italic,
    Code,
    Link,
};

struct Run
{
    String text;
    Style style = Style::Plain;
};

enum class BlockKind : u8
{
    Paragraph,
    Heading,
    Bullet,
    Numbered,
    Quote,
    Code,
    Rule,
};

struct Block
{
    BlockKind kind = BlockKind::Paragraph;
    u32 level = 0;  // heading level / list indent
    String marker;  // "1." for numbered items
    String text;    // raw inline Markdown / code
    String lang;    // code block language
};

Array<Block> ParseBlocks(StringView md)
{
    Array<Block> out;
    usize pos = 0;
    bool inCode = false;
    Block code;
    auto flushPara = [&](Block& b) {
        if (!b.text.Empty())
            out.PushBack(move(b));
        b = Block{};
    };
    Block para;
    while (pos <= md.Size()) {
        usize e = md.Find('\n', pos);
        if (e == StringView::NPos)
            e = md.Size();
        StringView line = md.SubStr(pos, e - pos);
        if (line.EndsWith('\r'))
            line = line.DropRight(1);
        const bool last = e >= md.Size();
        pos = e + 1;
        const StringView trimmed = line.Trim();
        if (inCode) {
            if (trimmed.StartsWith("```")) {
                inCode = false;
                out.PushBack(move(code));
                code = Block{};
            } else {
                if (!code.text.Empty() || code.level)
                    code.text += '\n';
                code.level = 1; // has content (an empty first line counts)
                code.text += line;
            }
        } else if (trimmed.StartsWith("```")) {
            flushPara(para);
            inCode = true;
            code = Block{};
            code.kind = BlockKind::Code;
            code.lang = trimmed.DropLeft(3).Trim();
        } else if (trimmed.Empty()) {
            flushPara(para);
        } else if (trimmed == "---" || trimmed == "***" || trimmed == "___") {
            flushPara(para);
            Block b;
            b.kind = BlockKind::Rule;
            out.PushBack(move(b));
        } else if (trimmed.StartsWith("#")) {
            flushPara(para);
            u32 level = 0;
            while (level < trimmed.Size() && trimmed[level] == '#')
                ++level;
            Block b;
            b.kind = BlockKind::Heading;
            b.level = Min(level, 3u);
            b.text = trimmed.DropLeft(level).Trim();
            out.PushBack(move(b));
        } else if (trimmed.StartsWith("> ") || trimmed == ">") {
            flushPara(para);
            Block b;
            b.kind = BlockKind::Quote;
            b.text = trimmed.DropLeft(1).Trim();
            if (!out.Empty() && out.Back().kind == BlockKind::Quote) {
                out.Back().text.Appendf(" {}", b.text);
            } else {
                out.PushBack(move(b));
            }
        } else {
            const usize indent = line.Size() - line.TrimLeft().Size();
            const bool bullet = trimmed.StartsWith("- ") || trimmed.StartsWith("* ") || trimmed.StartsWith("+ ");
            usize digits = 0;
            while (digits < trimmed.Size() && IsDigitAscii(trimmed[digits]))
                ++digits;
            const bool numbered = digits > 0 && digits < 4 && digits + 1 < trimmed.Size() && (trimmed[digits] == '.' || trimmed[digits] == ')') &&
                                  trimmed[digits + 1] == ' ';
            if (bullet || numbered) {
                flushPara(para);
                Block b;
                b.kind = bullet ? BlockKind::Bullet : BlockKind::Numbered;
                b.level = u32(Min<usize>(indent / 2, 3));
                if (numbered) {
                    b.marker = String(trimmed.Left(digits)) + ".";
                    b.text = trimmed.DropLeft(digits + 2).Trim();
                } else {
                    b.text = trimmed.DropLeft(2).Trim();
                    if (b.text.View().StartsWith("[ ] ") || b.text.View().StartsWith("[x] ")) { // task lists
                        b.marker = b.text.View()[1] == 'x' ? ZX_ICON_CHECK : "\xE2\x97\xA6";
                        b.text = String(b.text.View().DropLeft(4));
                    }
                }
                out.PushBack(move(b));
            } else if (indent >= 2 && para.text.Empty() && !out.Empty() &&
                       (out.Back().kind == BlockKind::Bullet || out.Back().kind == BlockKind::Numbered)) {
                out.Back().text.Appendf(" {}", trimmed); // continuation of a list item
            } else {
                if (!para.text.Empty())
                    para.text += line.EndsWith("  ") ? "\n" : " ";
                para.text += trimmed;
            }
        }
        if (last)
            break;
    }
    if (inCode)
        out.PushBack(move(code)); // unterminated fence: still show it as code
    flushPara(para);
    return out;
}

/// Inline Markdown -> styled runs: **bold**, __bold__, *italic*, _italic_, `code`, [text](url), ~~strike~~ (plain).
Array<Run> ParseInline(StringView s)
{
    Array<Run> runs;
    String cur;
    bool bold = false, italic = false;
    auto style = [&] { return bold ? Style::Bold : italic ? Style::Italic : Style::Plain; };
    auto flush = [&](Style st) {
        if (!cur.Empty()) {
            runs.PushBack(Run{move(cur), st});
            cur.Clear();
        }
    };
    usize i = 0;
    while (i < s.Size()) {
        const char c = s[i];
        if (c == '\\' && i + 1 < s.Size() && !IsAlnumAscii(s[i + 1])) {
            cur += s[i + 1];
            i += 2;
        } else if (c == '`') {
            const usize end = s.Find('`', i + 1);
            if (end == StringView::NPos) {
                cur += c;
                ++i;
                continue;
            }
            flush(style());
            runs.PushBack(Run{String(s.SubStr(i + 1, end - i - 1)), Style::Code});
            i = end + 1;
        } else if ((c == '*' || c == '_') && i + 1 < s.Size() && s[i + 1] == c) {
            flush(style());
            bold = !bold;
            i += 2;
        } else if (c == '~' && i + 1 < s.Size() && s[i + 1] == '~') {
            i += 2;
        } else if ((c == '*' || c == '_') && (italic || (i + 1 < s.Size() && s[i + 1] != ' ')) &&
                   (c == '*' || i == 0 || !IsAlnumAscii(s[i - 1]))) {
            flush(style());
            italic = !italic;
            ++i;
        } else if (c == '[') {
            const usize close = s.Find("](", i);
            const usize end = close == StringView::NPos ? StringView::NPos : s.Find(')', close);
            if (close == StringView::NPos || end == StringView::NPos || s.SubStr(i, close - i).Contains('\n')) {
                cur += c;
                ++i;
                continue;
            }
            flush(style());
            runs.PushBack(Run{String(s.SubStr(i + 1, close - i - 1)), Style::Link});
            i = end + 1;
        } else {
            cur += c;
            ++i;
        }
    }
    flush(style());
    return runs;
}

struct TextStyle
{
    ui::FontHandle regular, bold, italic, mono;
    f32 size;
    Color text, dim, code, codeBg, link;
};

/// Lays out (and draws if `draw`) styled runs wrapped to `width` starting at `origin`. Returns the height.
f32 FlowRuns(DrawList* dl, const TextStyle& ts, Span<const Run> runs, Vec2 origin, f32 width, f32 lineH, Color base, bool headingBold = false)
{
    ui::FontSystem& fonts = ui::GetCurrentContext()->Fonts();
    f32 x = 0.0f, y = 0.0f;
    const f32 s = ui::GetCurrentContext()->GetScale();
    for (const Run& run : runs) {
        const ui::FontHandle font = run.style == Style::Code ? ts.mono
                                    : run.style == Style::Bold || headingBold ? ts.bold
                                    : run.style == Style::Italic ? ts.italic
                                                                 : ts.regular;
        const f32 size = run.style == Style::Code ? ts.size * 0.93f : ts.size;
        const Color color = run.style == Style::Code ? ts.code : run.style == Style::Link ? ts.link : base;
        // Words (with their trailing spaces); hard line breaks from "  \n".
        StringView rest = run.text.View();
        while (!rest.Empty()) {
            if (rest[0] == '\n') {
                x = 0.0f;
                y += lineH;
                rest = rest.DropLeft(1);
                continue;
            }
            usize end = 0;
            while (end < rest.Size() && rest[end] == ' ')
                ++end;
            while (end < rest.Size() && rest[end] != ' ' && rest[end] != '\n')
                ++end;
            while (end < rest.Size() && rest[end] == ' ')
                ++end;
            StringView word = rest.Left(end);
            rest = rest.DropLeft(end);
            f32 w = fonts.MeasureWidth(font, size, word);
            const f32 wNoSpace = fonts.MeasureWidth(font, size, word.TrimRight());
            if (x > 0.0f && x + wNoSpace > width) {
                x = 0.0f;
                y += lineH;
                word = word.TrimLeft();
                w = fonts.MeasureWidth(font, size, word);
            }
            // A single word longer than the line: break it by characters.
            while (wNoSpace > width && !word.Empty() && x == 0.0f) {
                const usize fit = Max<usize>(ui::Utf8NextBoundary(word, 0), fonts.HitTest(font, size, word, width));
                const StringView piece = word.Left(fit);
                if (dl)
                    dl->AddText(font, size, origin + Vec2(x, y), color, piece);
                word = word.DropLeft(fit);
                if (word.Empty())
                    break;
                y += lineH;
                w = fonts.MeasureWidth(font, size, word);
                if (w <= width)
                    break;
            }
            if (word.Empty())
                continue;
            if (dl) {
                const Vec2 p = origin + Vec2(x, y);
                if (run.style == Style::Code) {
                    const f32 cw = fonts.MeasureWidth(font, size, word.TrimRight());
                    dl->AddRect(Rect(p + Vec2(-2.0f * s, 1.0f * s), p + Vec2(cw + 2.0f * s, lineH - 1.0f * s)), ts.codeBg, ui::CornerRadii(3.0f * s));
                }
                dl->AddText(font, size, p + Vec2(0.0f, (lineH - size * 1.25f) * 0.5f), color, word);
                if (run.style == Style::Link) {
                    const f32 lw = fonts.MeasureWidth(font, size, word.TrimRight());
                    dl->AddRect(Rect(p + Vec2(0.0f, lineH - 3.0f * s), p + Vec2(lw, lineH - 2.0f * s)), WithAlpha(color, 0.5f));
                }
            }
            x += w;
        }
    }
    return y + lineH;
}

} // namespace

// --- Panel ----------------------------------------------------------------------------------------------------------

namespace {

struct PanelCtx
{
    Editor& ed;
    AIState& ai;
    ui::Context& ctx;
    const ui::Theme& t;
    f32 s;
    TextStyle text;
    f32 lineH;
    // Visible part of the chat (physical px) - items outside are measured, not drawn.
    f32 visibleMin, visibleMax;
};

Color Mix(Color a, Color b, f32 t) { return ui::LerpColor(a, b, t); }

/// Small flat icon button (toolbar style). Returns true when clicked.
bool HeaderButton(PanelCtx& p, StringView id, StringView icon, Vec2 pos, f32 size, StringView tooltip, bool enabled = true)
{
    const Rect r(pos, pos + Vec2(size));
    ui::SetCursorPos(r.min);
    const bool clicked = ui::InvisibleButton(id, r.Size() / p.s) && enabled;
    const bool hov = ui::IsItemHovered() && enabled;
    if (!tooltip.Empty())
        ui::ItemTooltip(tooltip);
    DrawList& dl = ui::GetWindowDrawList();
    if (hov)
        dl.AddRect(r, p.t.buttonHovered, ui::CornerRadii(4.0f * p.s));
    dl.AddTextInRect(Font("Inter"), 14.0f * p.s, r, enabled ? (hov ? p.t.text : p.t.textDim) : p.t.textDisabled, icon, ui::TextAlign::Center, false);
    return clicked;
}

/// Copy button with "Copied" feedback.
void CopyButton(PanelCtx& p, StringView id, const Rect& r, StringView text)
{
    ui::SetCursorPos(r.min);
    if (ui::InvisibleButton(id, r.Size() / p.s)) {
        platform::SetClipboardText(text);
        p.ai.copiedId = id;
        p.ai.copiedAt = p.ai.time;
    }
    const bool hov = ui::IsItemHovered();
    ui::ItemTooltip("Copy");
    const bool done = p.ai.copiedId == id && p.ai.time - p.ai.copiedAt < 1.5;
    DrawList& dl = ui::GetWindowDrawList();
    if (hov)
        dl.AddRect(r, p.t.buttonHovered, ui::CornerRadii(4.0f * p.s));
    dl.AddTextInRect(Font("Inter Medium"), 11.0f * p.s, r, done ? p.t.success : (hov ? p.t.text : p.t.textDim),
                     done ? ZX_ICON_CHECK " Copied" : ZX_ICON_COPY " Copy", ui::TextAlign::Center, false);
}

/// A code block (header with language + Copy, monospace body). Returns its height.
f32 CodeBlock(PanelCtx& p, StringView id, StringView lang, StringView code, Vec2 at, f32 width, bool draw)
{
    const f32 s = p.s;
    ui::FontSystem& fonts = p.ctx.Fonts();
    const f32 size = p.text.size * 0.92f;
    const f32 lh = Round(size * 1.45f);
    const f32 headH = 26.0f * s;
    const f32 padX = 12.0f * s, padY = 9.0f * s;
    // Long lines wrap (the panel is narrow); count the wrapped lines.
    const f32 wrapW = width - padX * 2.0f;
    u32 lines = 0;
    usize pos = 0;
    for (;;) {
        usize e = code.Find('\n', pos);
        if (e == StringView::NPos)
            e = code.Size();
        StringView ln = code.SubStr(pos, e - pos);
        do {
            const usize fit = ln.Empty() ? 0 : Max<usize>(1, fonts.HitTest(p.text.mono, size, ln, wrapW));
            ln = ln.DropLeft(Min(fit, ln.Size()));
            ++lines;
        } while (!ln.Empty());
        if (e >= code.Size())
            break;
        pos = e + 1;
    }
    const f32 h = headH + padY * 2.0f + f32(lines) * lh;
    if (!draw || at.y > p.visibleMax || at.y + h < p.visibleMin)
        return h;
    DrawList& dl = ui::GetWindowDrawList();
    const Rect box(at, at + Vec2(width, h));
    const ui::CornerRadii rr(6.0f * s);
    dl.AddRect(box, Mix(p.t.childBg, Color(0, 0, 0), 0.25f), rr, 1.0f, p.t.border);
    const Rect head(box.min, Vec2(box.max.x, box.min.y + headH));
    dl.AddRect(head, WithAlpha(p.t.text, 0.04f), ui::CornerRadii::Top(6.0f * s));
    dl.AddRect(Rect(Vec2(head.min.x, head.max.y - 1.0f), head.max), p.t.border);
    dl.AddTextInRect(Font("Inter Medium"), 11.0f * s, Rect(head.min + Vec2(padX, 0.0f), head.max - Vec2(80.0f * s, 0.0f)), p.t.textDim,
                     lang.Empty() ? StringView("code") : lang);
    CopyButton(p, id, Rect(Vec2(head.max.x - 72.0f * s, head.min.y + 3.0f * s), Vec2(head.max.x - 4.0f * s, head.max.y - 3.0f * s)), code);
    // Body.
    f32 y = head.max.y + padY;
    pos = 0;
    for (;;) {
        usize e = code.Find('\n', pos);
        if (e == StringView::NPos)
            e = code.Size();
        StringView ln = code.SubStr(pos, e - pos);
        do {
            const usize fit = ln.Empty() ? 0 : Max<usize>(1, fonts.HitTest(p.text.mono, size, ln, wrapW));
            dl.AddText(p.text.mono, size, Vec2(box.min.x + padX, y), Color(0xCE, 0xD6, 0xE4), ln.Left(Min(fit, ln.Size())));
            ln = ln.DropLeft(Min(fit, ln.Size()));
            y += lh;
        } while (!ln.Empty());
        if (e >= code.Size())
            break;
        pos = e + 1;
    }
    return h;
}

/// Lays out (and draws) an assistant message. Returns its height.
f32 MarkdownMessage(PanelCtx& p, const AIChatItem& item, usize index, Vec2 at, f32 width, bool draw)
{
    const f32 s = p.s;
    const Array<Block> blocks = ParseBlocks(item.text);
    f32 y = 0.0f;
    const f32 gap = 8.0f * s;
    DrawList* dl = draw ? &ui::GetWindowDrawList() : nullptr;
    u32 codeIndex = 0;
    for (usize bi = 0; bi < blocks.Size(); ++bi) {
        const Block& b = blocks[bi];
        if (bi > 0)
            y += b.kind == BlockKind::Heading ? gap * 1.5f : (b.kind == BlockKind::Bullet || b.kind == BlockKind::Numbered) &&
                                                                       (blocks[bi - 1].kind == BlockKind::Bullet || blocks[bi - 1].kind == BlockKind::Numbered)
                                                                   ? 3.0f * s
                                                                   : gap;
        const Vec2 o = at + Vec2(0.0f, y);
        const bool visible = draw && o.y < p.visibleMax && o.y + 2000.0f * s > p.visibleMin;
        switch (b.kind) {
        case BlockKind::Code: {
            const String id = String::Format("##code_{}_{}", index, codeIndex++);
            y += CodeBlock(p, id, b.lang, b.text, o, width, draw);
            break;
        }
        case BlockKind::Rule:
            if (visible)
                dl->AddRect(Rect(o + Vec2(0.0f, 4.0f * s), o + Vec2(width, 5.0f * s)), p.t.border);
            y += 9.0f * s;
            break;
        case BlockKind::Heading: {
            TextStyle hs = p.text;
            hs.size = p.text.size * (b.level == 1 ? 1.28f : b.level == 2 ? 1.14f : 1.03f);
            hs.bold = Font("Inter SemiBold");
            const f32 lh = Round(hs.size * 1.4f);
            const Array<Run> runs = ParseInline(b.text);
            y += FlowRuns(visible ? dl : nullptr, hs, runs.View(), o, width, lh, p.t.text, true);
            break;
        }
        case BlockKind::Bullet:
        case BlockKind::Numbered: {
            const f32 indent = 18.0f * s + f32(b.level) * 16.0f * s;
            if (visible) {
                if (b.kind == BlockKind::Numbered || !b.marker.Empty())
                    dl->AddTextInRect(Font("Inter Medium"), p.text.size * 0.95f, Rect(o + Vec2(f32(b.level) * 16.0f * s, 0.0f), o + Vec2(indent - 4.0f * s, p.lineH)),
                                      p.t.textDim, b.kind == BlockKind::Numbered ? b.marker.View() : b.marker.View(), ui::TextAlign::Right, false);
                else
                    dl->AddCircle(o + Vec2(f32(b.level) * 16.0f * s + 7.0f * s, p.lineH * 0.5f), 2.2f * s, p.t.textDim);
            }
            const Array<Run> runs = ParseInline(b.text);
            y += FlowRuns(visible ? dl : nullptr, p.text, runs.View(), o + Vec2(indent, 0.0f), width - indent, p.lineH, p.t.text);
            break;
        }
        case BlockKind::Quote: {
            const Array<Run> runs = ParseInline(b.text);
            const f32 h = FlowRuns(nullptr, p.text, runs.View(), o + Vec2(12.0f * s, 0.0f), width - 12.0f * s, p.lineH, p.t.textDim);
            if (visible) {
                dl->AddRect(Rect(o, o + Vec2(3.0f * s, h)), WithAlpha(p.t.accent, 0.6f), ui::CornerRadii(1.5f * s));
                FlowRuns(dl, p.text, runs.View(), o + Vec2(12.0f * s, 0.0f), width - 12.0f * s, p.lineH, p.t.textDim);
            }
            y += h;
            break;
        }
        case BlockKind::Paragraph: {
            const Array<Run> runs = ParseInline(b.text);
            y += FlowRuns(visible ? dl : nullptr, p.text, runs.View(), o, width, p.lineH, p.t.text);
            break;
        }
        }
    }
    return y;
}

String ToolLabel(StringView tool)
{
    String label(tool);
    label.ReplaceAll("_", " ");
    if (!label.Empty() && label[0] >= 'a' && label[0] <= 'z')
        label[0] = char(label[0] - 32);
    return label;
}

/// One chat item at the cursor; returns nothing, advances the layout.
void DrawItem(PanelCtx& p, AIChatItem& item, usize index, f32 width)
{
    const f32 s = p.s;
    const Vec2 at = ui::GetCursorPos();
    // Off-screen items with a known layout are skipped (long chats stay cheap).
    if (item.layoutWidth == width && item.layoutHeight > 0.0f && (at.y > p.visibleMax || at.y + item.layoutHeight < p.visibleMin)) {
        ui::Dummy(Vec2(width / s, item.layoutHeight / s));
        return;
    }
    DrawList& dl = ui::GetWindowDrawList();
    // Entrance: the item fades in and settles up from 10 px below, on a decelerating curve (0.22 s). The layout does not
    // move: the next item is placed from the full height, so only the drawing is offset.
    const f32 t = Saturate(item.age / 0.22f);
    const f32 settle = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
    const f32 fade = settle;
    const Vec2 drawAt = at + Vec2(0.0f, (1.0f - settle) * 10.0f * s);
    f32 h = 0.0f;
    switch (item.kind) {
    case AIChatItem::Kind::User: {
        // Right-aligned, a quiet surface - not a colored bubble.
        const f32 pad = 11.0f * s;
        const f32 maxW = width * 0.86f;
        const Array<Run> runs{Run{item.text, Style::Plain}};
        // Width of the text (widest line), wrapped to maxW.
        f32 textW = 0.0f;
        for (StringView line : SplitString(item.text, '\n'))
            textW = Max(textW, p.ctx.Fonts().MeasureWidth(p.text.regular, p.text.size, line));
        const f32 bw = Min(maxW, textW + pad * 2.0f + 1.0f);
        const f32 th = FlowRuns(nullptr, p.text, runs.View(), Vec2::Zero(), bw - pad * 2.0f, p.lineH, p.t.text);
        h = th + pad * 1.5f;
        const Rect r(Vec2(drawAt.x + width - bw, drawAt.y), Vec2(drawAt.x + width, drawAt.y + h));
        dl.AddRect(r, WithAlpha(Mix(p.t.frameBg, p.t.accent, 0.12f), fade), ui::CornerRadii(8.0f * s), 1.0f, WithAlpha(Mix(p.t.border, p.t.accent, 0.25f), fade));
        FlowRuns(&dl, p.text, runs.View(), r.min + Vec2(pad, pad * 0.75f), bw - pad * 2.0f, p.lineH, WithAlpha(p.t.text, fade));
        // Copy on hover.
        ui::SetCursorPos(r.min);
        ui::InvisibleButton(String::Format("##user_{}", index), r.Size() / s);
        if (ui::IsItemHovered() && ui::IsItemClicked(MouseButton::Right)) {
            platform::SetClipboardText(item.text);
            p.ai.copiedId = String::Format("##user_{}", index);
            p.ai.copiedAt = p.ai.time;
        }
        ui::ItemTooltip("Right-click to copy");
        ui::SetCursorPos(at);
        break;
    }
    case AIChatItem::Kind::Assistant: {
        h = MarkdownMessage(p, item, index, drawAt, width, true);
        break;
    }
    case AIChatItem::Kind::Tool: {
        // A compact activity row: status icon, tool name, what it was asked; click to see the result.
        const f32 rowH = 26.0f * s;
        const Rect row(drawAt, drawAt + Vec2(width, rowH));
        ui::SetCursorPos(row.min);
        if (ui::InvisibleButton(String::Format("##tool_{}", index), row.Size() / s) && !item.running)
            item.expanded = !item.expanded;
        const bool hov = ui::IsItemHovered();
        if (hov && !item.running)
            dl.AddRect(row, WithAlpha(p.t.text, 0.05f), ui::CornerRadii(5.0f * s));
        const Vec2 ic(row.min.x + 12.0f * s, row.Center().y);
        if (item.running) {
            const f32 a0 = f32(p.ai.time) * 7.0f;
            Vec2 pts[13];
            for (u32 i = 0; i < 13; ++i) {
                const f32 a = a0 + f32(i) * (TwoPi * 0.7f / 12.0f);
                pts[i] = ic + Vec2(Cos(a), Sin(a)) * (5.0f * s);
            }
            dl.AddPolyline(Span<const Vec2>(pts, 13), p.t.accentHovered, 1.6f * s);
        } else {
            dl.AddTextInRect(Font("Inter"), 12.0f * s, Rect(ic - Vec2(8.0f * s), ic + Vec2(8.0f * s)), item.isError ? p.t.error : p.t.success,
                             item.isError ? ZX_ICON_X : ZX_ICON_CHECK, ui::TextAlign::Center, false);
        }
        const String name = ToolLabel(item.tool);
        const ui::FontHandle medium = Font("Inter Medium");
        const f32 fs = 12.0f * s;
        f32 x = row.min.x + 26.0f * s;
        const f32 nameW = p.ctx.Fonts().MeasureWidth(medium, fs, name);
        dl.AddTextInRect(medium, fs, Rect(Vec2(x, row.min.y), Vec2(Min(x + nameW + 1.0f, row.max.x), row.max.y)), p.t.text, name, ui::TextAlign::Left, false);
        x += nameW + 8.0f * s;
        String right = item.running ? String::Format("{:.0f}s", Max(0.0, p.ai.time - item.started)) : String::Format("{:.1f}s", item.duration);
        const f32 rightW = p.ctx.Fonts().MeasureWidth(p.text.regular, 11.0f * s, right) + 10.0f * s;
        const String& detail = item.isError && !item.result.Empty() ? item.result : item.text;
        if (!detail.Empty() && x < row.max.x - rightW - 20.0f * s) {
            const usize nl = detail.Find('\n');
            dl.AddTextInRect(p.text.regular, fs, Rect(Vec2(x, row.min.y), Vec2(row.max.x - rightW - 14.0f * s, row.max.y)),
                             item.isError ? Mix(p.t.error, p.t.text, 0.3f) : p.t.textDim, nl == StringView::NPos ? detail.View() : detail.View().Left(nl));
        }
        dl.AddTextInRect(p.text.regular, 11.0f * s, Rect(Vec2(row.max.x - rightW - 14.0f * s, row.min.y), Vec2(row.max.x - 14.0f * s, row.max.y)),
                         p.t.textDisabled, right, ui::TextAlign::Right, false);
        if (!item.running && !item.result.Empty())
            dl.AddTextInRect(Font("Inter"), 11.0f * s, Rect(Vec2(row.max.x - 14.0f * s, row.min.y), row.max), p.t.textDisabled,
                             item.expanded ? ZX_ICON_CHEVRON_DOWN : ZX_ICON_CHEVRON_RIGHT, ui::TextAlign::Center, false);
        h = rowH;
        if (item.expanded && !item.result.Empty()) {
            const f32 inner = width - 26.0f * s;
            const Vec2 o(row.min.x + 26.0f * s, row.max.y + 2.0f * s);
            const f32 rs = 11.5f * s;
            const Vec2 sz = p.ctx.Fonts().MeasureText(p.text.mono, rs, item.result, inner - 16.0f * s);
            const Rect box(o, o + Vec2(inner, sz.y + 14.0f * s));
            dl.AddRect(box, Mix(p.t.childBg, Color(0, 0, 0), 0.25f), ui::CornerRadii(5.0f * s), 1.0f, p.t.border);
            dl.AddText(p.text.mono, rs, box.min + Vec2(8.0f * s, 7.0f * s), item.isError ? Mix(p.t.error, p.t.text, 0.35f) : p.t.textDim, item.result,
                       inner - 16.0f * s);
            h += box.Height() + 4.0f * s;
        }
        break;
    }
    case AIChatItem::Kind::Error:
    case AIChatItem::Kind::Notice: {
        const bool error = item.kind == AIChatItem::Kind::Error;
        const Color tint = error ? p.t.error : p.t.warning;
        const f32 pad = 10.0f * s;
        const Array<Run> runs = ParseInline(item.text);
        const f32 th = FlowRuns(nullptr, p.text, runs.View(), Vec2::Zero(), width - pad * 2.0f - 22.0f * s, p.lineH, p.t.text);
        h = th + pad * 1.6f;
        const Rect r(drawAt, drawAt + Vec2(width, h));
        dl.AddRect(r, WithAlpha(tint, 0.08f), ui::CornerRadii(6.0f * s), 1.0f, WithAlpha(tint, 0.35f));
        dl.AddTextInRect(Font("Inter"), 13.0f * s, Rect(r.min + Vec2(pad - 2.0f * s, pad * 0.8f), r.min + Vec2(pad + 16.0f * s, pad * 0.8f + p.lineH)), tint,
                         error ? ZX_ICON_CIRCLE_ALERT : ZX_ICON_INFO, ui::TextAlign::Center, false);
        FlowRuns(&dl, p.text, runs.View(), r.min + Vec2(pad + 22.0f * s, pad * 0.8f), width - pad * 2.0f - 22.0f * s, p.lineH,
                 Mix(tint, p.t.text, 0.55f));
        break;
    }
    }
    item.layoutWidth = width;
    item.layoutHeight = h;
    // Some kinds move the cursor while they draw (copy buttons, inner widgets): always advance from the item start.
    ui::SetCursorPos(at);
    ui::Dummy(Vec2(width / s, h / s));
}

/// Spacing between items: tools group together, a message after a different kind gets room.
f32 GapBefore(const AIChatItem& prev, const AIChatItem& cur)
{
    if (prev.kind == AIChatItem::Kind::Tool && cur.kind == AIChatItem::Kind::Tool)
        return 0.0f;
    if (cur.kind == AIChatItem::Kind::User)
        return 18.0f;
    return 10.0f;
}

/// "Thinking" row while a turn runs without a tool in flight.
void DrawWorking(PanelCtx& p, f32 width)
{
    const f32 s = p.s;
    const Vec2 at = ui::GetCursorPos();
    DrawList& dl = ui::GetWindowDrawList();
    for (u32 i = 0; i < 3; ++i) {
        const f32 ph = f32(p.ai.time) * 5.0f - f32(i) * 0.7f;
        dl.AddCircle(at + Vec2(6.0f * s + f32(i) * 9.0f * s, 12.0f * s), 2.6f * s, WithAlpha(p.t.textDim, 0.35f + 0.65f * Max(0.0f, Sin(ph))));
    }
    const String text = String::Format("Thinking  {:.0f}s", Max(0.0, p.ai.time - p.ai.turnStarted));
    dl.AddTextInRect(p.text.regular, 12.0f * s, Rect(at + Vec2(36.0f * s, 0.0f), at + Vec2(width, 24.0f * s)), p.t.textDim, text);
    ui::Dummy(Vec2(width / s, 24.0f));
}

/// Empty chat: what the assistant does, how to set it up, example requests.
void DrawWelcome(PanelCtx& p, f32 width)
{
    AIState& ai = p.ai;
    const f32 s = p.s;
    DrawList& dl = ui::GetWindowDrawList();
    Vec2 at = ui::GetCursorPos() + Vec2(0.0f, 20.0f * s);
    dl.AddTextInRect(Font("Inter"), 26.0f * s, Rect(at, at + Vec2(width, 32.0f * s)), p.t.accentHovered, ZX_ICON_SPARKLES, ui::TextAlign::Center, false);
    at.y += 40.0f * s;
    dl.AddTextInRect(Font("Inter SemiBold"), 17.0f * s, Rect(at, at + Vec2(width, 24.0f * s)), p.t.text, "What should we build?", ui::TextAlign::Center, false);
    at.y += 26.0f * s;
    dl.AddTextInRect(p.text.regular, 12.5f * s, Rect(at, at + Vec2(width, 20.0f * s)), p.t.textDim,
                     "Zeqou AI edits the scene, writes scripts and runs the game.", ui::TextAlign::Center, false);
    at.y += 38.0f * s;
    ui::SetCursorPos(at);

    // A list row: icon, title, text, chevron.
    auto row = [&](StringView id, StringView icon, StringView title, StringView text) {
        const Vec2 c = ui::GetCursorPos();
        const Rect r(c, c + Vec2(width, 54.0f * s));
        const bool clicked = ui::InvisibleButton(id, r.Size() / s);
        const bool hov = ui::IsItemHovered();
        dl.AddRect(r, hov ? p.t.frameBgHovered : p.t.frameBg, ui::CornerRadii(6.0f * s), 1.0f, hov ? p.t.borderStrong : p.t.border);
        dl.AddTextInRect(Font("Inter"), 16.0f * s, Rect(r.min, Vec2(r.min.x + 44.0f * s, r.max.y)), p.t.accentHovered, icon, ui::TextAlign::Center, false);
        dl.AddTextInRect(Font("Inter SemiBold"), 13.0f * s, Rect(Vec2(r.min.x + 44.0f * s, r.min.y + 8.0f * s), Vec2(r.max.x - 28.0f * s, r.min.y + 26.0f * s)),
                         p.t.text, title);
        dl.AddTextInRect(p.text.regular, 12.0f * s, Rect(Vec2(r.min.x + 44.0f * s, r.min.y + 27.0f * s), Vec2(r.max.x - 28.0f * s, r.min.y + 45.0f * s)),
                         p.t.textDim, text);
        dl.AddTextInRect(Font("Inter"), 13.0f * s, Rect(Vec2(r.max.x - 28.0f * s, r.min.y), r.max), p.t.textDisabled, ZX_ICON_CHEVRON_RIGHT,
                         ui::TextAlign::Center, false);
        ui::SetCursorPos(Vec2(c.x, r.max.y + 8.0f * s));
        ui::Dummy(Vec2(0.0f, 0.0f));
        return clicked;
    };
    if (!ProviderReady(ai.settings, ai.settings.provider)) {
        if (row("##ai_key", ZX_ICON_KEY, "Add an API key", String::Format("{} providers - Claude, GPT, Gemini, Grok, DeepSeek, local models", AIProviders().Size())))
            OpenSettingsPage(p.ed, "AI");
        if (row("##ai_mcp", ZX_ICON_PLUG, "Connect an AI agent (MCP)", "Claude Code, Cursor, VS Code build here from your agent"))
            ai.page = 1;
        return;
    }
    dl.AddTextInRect(Font("Inter Medium"), 11.5f * s, Rect(ui::GetCursorPos(), ui::GetCursorPos() + Vec2(width, 18.0f * s)), p.t.textDisabled, "TRY");
    ui::Dummy(Vec2(0.0f, 20.0f));
    static constexpr StringView Suggestions[][2] = {
        {ZX_ICON_SWORDS, "Build a small arena with a player and three enemies"},
        {ZX_ICON_SUNSET, "Make the lighting cinematic at sunset"},
        {ZX_ICON_CODE, "Write a script that opens a door when the player is near"},
        {ZX_ICON_SCAN_SEARCH, "What is in this scene? Anything wrong?"},
    };
    for (const auto& sug : Suggestions) {
        const Vec2 c = ui::GetCursorPos();
        const Rect r(c, c + Vec2(width, 32.0f * s));
        ui::PushID(sug[1]);
        const bool clicked = ui::InvisibleButton("##suggestion", r.Size() / s);
        const bool hov = ui::IsItemHovered();
        ui::PopID();
        if (hov)
            dl.AddRect(r, p.t.frameBgHovered, ui::CornerRadii(5.0f * s));
        dl.AddTextInRect(Font("Inter"), 13.0f * s, Rect(r.min, Vec2(r.min.x + 30.0f * s, r.max.y)), hov ? p.t.accentHovered : p.t.textDim, sug[0],
                         ui::TextAlign::Center, false);
        dl.AddTextInRect(p.text.regular, 12.5f * s, Rect(Vec2(r.min.x + 32.0f * s, r.min.y), r.max - Vec2(8.0f * s, 0.0f)), hov ? p.t.text : p.t.textDim, sug[1]);
        ui::SetCursorPos(Vec2(c.x, r.max.y + 2.0f * s));
        ui::Dummy(Vec2(0.0f, 0.0f));
        if (clicked)
            SendMessage(p.ed, sug[1]);
    }
}

/// MCP: the switch, its state and how to connect each client (the panel's connect page and Settings > AI).
void DrawMcpConnect(Editor& ed, f32 width)
{
    AIState& ai = *ed.ai;
    AISettings& st = ai.settings;
    ui::Context& ctx = *ui::GetCurrentContext();
    const ui::Theme& t = ctx.GetStyle().colors;
    const f32 s = ctx.GetScale();
    if (ui::Toggle("Allow AI agents to connect (MCP server)", &st.mcpEnabled)) {
        ai.mcpError.Clear();
        SaveAISettings(st);
    }
    const String url = String::Format("http://127.0.0.1:{}/mcp", st.mcpPort);
    if (ai.mcp.IsRunning()) {
        const String client = ai.mcp.LastClient();
        ui::TextColored(t.success, client.Empty() ? String::Format(ZX_ICON_CIRCLE_CHECK "  Listening on {}", url)
                                                  : String::Format(ZX_ICON_CIRCLE_CHECK "  {} connected - {} tool calls", client, ai.mcp.ToolCallCount()));
    } else if (!ai.mcpError.Empty()) {
        ui::TextColored(t.error, ai.mcpError);
    } else {
        ui::TextDisabled("Off. Turn it on, then add the server to your AI tool:");
    }
    ui::Spacing();
    const String auth = String::Format("Bearer {}", st.mcpToken);
    PanelCtx p{ed, ai, ctx, t, s, {}, 0.0f, -F32Max, F32Max};
    p.text.mono = ai.mono.IsValid() ? ai.mono : Font("Inter");
    p.text.size = 13.0f * s;
    auto block = [&](StringView id, StringView title, const String& text) {
        ui::PushFont(ui::FontStyle::SemiBold);
        ui::Text(title);
        ui::PopFont();
        const Vec2 at = ui::GetCursorPos();
        const f32 h = CodeBlock(p, id, "", text, at, width, true);
        ui::Dummy(Vec2(width / s, h / s));
        ui::Spacing();
    };
    block("##cc", "Claude Code", String::Format("claude mcp add --transport http zeqoux3d {} --header \"Authorization: {}\"", url, auth));
    block("##cursor", "Cursor  (~/.cursor/mcp.json)",
          String::Format("{{\n  \"mcpServers\": {{\n    \"zeqoux3d\": {{\n      \"url\": \"{}\",\n      \"headers\": {{ \"Authorization\": \"{}\" }}\n    }}\n  }}\n}}", url, auth));
    block("##vscode", "VS Code / Copilot  (.vscode/mcp.json)",
          String::Format("{{\n  \"servers\": {{\n    \"zeqoux3d\": {{\n      \"type\": \"http\",\n      \"url\": \"{}\",\n      \"headers\": {{ \"Authorization\": \"{}\" }}\n    }}\n  }}\n}}",
                         url, auth));
    block("##desktop", "Claude Desktop, other stdio clients  (Node.js)",
          String::Format("{{\n  \"mcpServers\": {{\n    \"zeqoux3d\": {{\n      \"command\": \"npx\",\n      \"args\": [\"-y\", \"mcp-remote\", \"{}\", \"--header\", \"Authorization:{}\"]\n    }}\n  }}\n}}",
                         url, auth));
    ui::TextWrapped("The server listens on this computer only (127.0.0.1) and requires the token. Then ask your agent, e.g. "
                    "\"Build a small arena level with a player and three patrolling enemies\".");
}

// --- Model picker ---------------------------------------------------------------------------------------------------

bool MatchesFilter(StringView text, StringView filter)
{
    if (filter.Empty())
        return true;
    // Every space-separated word must appear (case-insensitive).
    for (StringView word : SplitString(filter, ' ', true)) {
        if (word.Empty())
            continue;
        bool found = false;
        for (usize i = 0; i + word.Size() <= text.Size() && !found; ++i)
            found = text.SubStr(i, word.Size()).EqualsIgnoreCase(word);
        if (!found)
            return false;
    }
    return true;
}

/// The picker popup: providers on the left, the provider's models (search, context size) on the right.
void DrawModelPicker(PanelCtx& p)
{
    AIState& ai = p.ai;
    const f32 s = p.s;
    // Fixed size from the panel: it never grows with its content and it fits the window (the UI library clamps it to
    // the display). Columns and lists derive from this size.
    const f32 pickerW = Clamp(ai.settings.panelWidth - 24.0f, 340.0f, 560.0f);
    const f32 pickerH = Clamp(ui::GetDisplaySize().y * 0.45f, 260.0f, 440.0f);
    ui::SetPopupSize("##ai_model_picker", Vec2(pickerW, pickerH));
    if (!ui::BeginPopup("##ai_model_picker")) {
        ai.popupOpen = false;
        return;
    }
    ai.popupOpen = true;
    const u32 prov = ai.pickerProvider;
    RefreshAIModels(ai, prov);
    // Columns share the popup width: providers take a narrow fixed column, models get the rest.
    const f32 provW = Clamp(pickerW * 0.40f, 150.0f, 200.0f);
    const f32 gap = 8.0f;
    const f32 modelsW = Max(pickerW - provW - gap - 16.0f, 160.0f);
    const f32 listH = pickerH - 40.0f;
    // Providers. Both columns start at the same top position. The providers child advances the line by its full
    // height, so the models column is placed explicitly at that top position rather than with SameLine.
    const Vec2 columnsTop = ui::GetCursorPos();
    if (ui::BeginChild("##ai_picker_providers", Vec2(provW, listH), false)) {
        for (u32 i = 0; i < AIProviders().Size(); ++i) {
            const AIProviderInfo& info = AIProviders()[i];
            const bool ready = ProviderReady(ai.settings, i);
            ui::PushID(i32(i));
            const String label = String::Format("{}  {}{}", ready ? ZX_ICON_CIRCLE_CHECK : ZX_ICON_CIRCLE, info.name, i == ai.settings.provider ? "  \xC2\xB7" : "");
            if (!ready)
                ui::PushTextColor(p.t.textDim);
            if (ui::Selectable(label, i == prov, Vec2(provW - 8.0f, 26.0f))) {
                ai.pickerProvider = i;
                ai.modelSearch.Clear();
                ai.focusModelSearch = true;
            }
            if (!ready)
                ui::PopTextColor();
            ui::PopID();
        }
    }
    ui::EndChild();
    // Models column: to the right of the providers, at the same top position. SetCursorPos takes physical pixels,
    // while provW and gap are logical, so the offset is scaled.
    ui::SetCursorPos(Vec2(Round(columnsTop.x + (provW + gap) * s), columnsTop.y));
    ui::BeginGroup();
    const AIProviderInfo& info = AIProviders()[prov];
    if (ai.focusModelSearch) {
        ui::SetKeyboardFocusHere();
        ai.focusModelSearch = false;
    }
    const f32 reloadW = ui::GetFrameHeight() / s;
    ui::SetNextItemWidth(Max(modelsW - reloadW - 6.0f, 80.0f));
    ui::InputText("##ai_model_search", ai.modelSearch, ui::InputTextFlags::None, ZX_ICON_SEARCH "  Search or type a model id");
    ui::SameLine(0.0f, 4.0f);
    const bool fetching = ai.fetching.load() && ai.fetchProvider == prov;
    ui::BeginDisabled(fetching || !ProviderReady(ai.settings, prov));
    if (ui::IconButton(ZX_ICON_REFRESH_CW, "Reload the model list"))
        RefreshAIModels(ai, prov, true);
    ui::EndDisabled();
    const AIModelList& list = ai.lists[prov];
    const String current = ModelFor(ai, prov);
    bool chosen = false;
    if (ui::BeginChild("##ai_picker_models", Vec2(modelsW, listH - ui::GetFrameHeight() / s - 8.0f), false)) {
        auto modelRow = [&](StringView id, u32 context, StringView note) {
            ui::PushID(id);
            const Vec2 c = ui::GetCursorPos();
            // Leave room for the child's scrollbar, which appears when the list is longer than its box.
            const f32 scrollbar = p.ctx.GetStyle().scrollbarSize;
            const f32 w = Max(modelsW - scrollbar - 6.0f, 120.0f) * s, h = 28.0f * s;
            const Rect r(c, c + Vec2(w, h));
            const bool clicked = ui::InvisibleButton("##m", r.Size() / s);
            const bool hov = ui::IsItemHovered();
            const bool sel = prov == ai.settings.provider && id == current;
            DrawList& dl = ui::GetWindowDrawList();
            if (sel || hov)
                dl.AddRect(r, sel ? p.t.selection : p.t.headerHovered, ui::CornerRadii(4.0f * s));
            const String ctxText = context ? FormatTokens(context) : String();
            const f32 cw = ctxText.Empty() ? 0.0f : p.ctx.Fonts().MeasureWidth(p.text.regular, 11.0f * s, ctxText) + 12.0f * s;
            dl.AddTextInRect(p.text.regular, 12.5f * s, Rect(r.min + Vec2(8.0f * s, 0.0f), Vec2(r.max.x - cw - 4.0f * s, r.max.y)), p.t.text,
                             note.Empty() ? id : String::Format("{}  {}", id, note).View());
            if (!ctxText.Empty())
                dl.AddTextInRect(p.text.regular, 11.0f * s, Rect(Vec2(r.max.x - cw, r.min.y), r.max - Vec2(8.0f * s, 0.0f)), p.t.textDim, ctxText,
                                 ui::TextAlign::Right, false);
            if (ui::IsItemHovered())
                ui::ItemTooltip(context ? String::Format("{}\nContext window: {} tokens", id, FormatTokens(context)) : String(id));
            ui::PopID();
            if (clicked) {
                SelectAIModel(ai, prov, id);
                chosen = true;
            }
        };
        const String typed = ai.modelSearch.Trimmed();
        u32 shown = 0;
        bool typedListed = false;
        for (const ai::ModelInfo& m : list.models) {
            if (!MatchesFilter(m.id, typed) && !MatchesFilter(m.name, typed))
                continue;
            typedListed |= m.id == typed;
            StringView src;
            modelRow(m.id, ContextOf(ai, prov, m.id, &src), {});
            ++shown;
        }
        // The default / current model even if the list could not be fetched.
        if (list.models.Empty() && !current.Empty() && MatchesFilter(current, typed)) {
            modelRow(current, ContextOf(ai, prov, current, nullptr), "(default)");
            typedListed |= current == typed;
        }
        if (!typed.Empty() && !typedListed && !typed.View().Contains(' '))
            modelRow(typed, ContextOf(ai, prov, typed, nullptr), "(use this id)");
        if (fetching) {
            ui::Spacing();
            ui::Spinner(7.0f);
            ui::SameLine();
            ui::TextDisabled("Loading models...");
        } else if (!list.error.Empty()) {
            ui::Spacing();
            ui::PushTextColor(list.fetched ? p.t.textDim : Mix(p.t.error, p.t.text, 0.3f));
            ui::TextWrapped(list.error);
            ui::PopTextColor();
            if (info.needsKey && ai.settings.apiKeys[prov].Trimmed().Empty() && ui::Button(ZX_ICON_KEY "  Add a key in Settings")) {
                OpenSettingsPage(p.ed, "AI");
                ui::CloseCurrentPopup();
            }
        } else if (list.fetched && shown == 0 && typed.Empty()) {
            ui::TextDisabled("The provider lists no chat models.");
        }
    }
    ui::EndChild();
    ui::EndGroup();
    if (chosen) {
        ai.focusInput = true;
        ui::CloseCurrentPopup();
    }
    ui::EndPopup();
}

// --- Composer -------------------------------------------------------------------------------------------------------

/// The message box: multi-line text, then a row with the model pill, the context ring and send / stop.
void DrawComposer(PanelCtx& p, const Rect& area)
{
    AIState& ai = p.ai;
    const f32 s = p.s;
    DrawList& dl = ui::GetWindowDrawList();
    const bool busy = ai.agent.IsBusy();
    const Rect box = area;
    const bool focused = ai.inputActive;
    // The border eases between the normal and the accent colour (exponential approach, ~0.12 s), so focus never snaps.
    const f32 focusTarget = focused ? 1.0f : 0.0f;
    ai.focusAnim += (focusTarget - ai.focusAnim) * Saturate(ai.frameDt * 14.0f);
    dl.AddRect(box, p.t.frameBg, ui::CornerRadii(8.0f * s), 1.0f, Mix(p.t.border, WithAlpha(p.t.accent, 0.85f), ai.focusAnim));

    // Text field (transparent, inside the box).
    const f32 barH = 32.0f * s;
    const Rect textArea(box.min + Vec2(4.0f * s, 4.0f * s), Vec2(box.max.x - 4.0f * s, box.max.y - barH));
    ui::Style& style = p.ctx.GetStyle();
    const ui::Theme saved = style.colors;
    style.colors.frameBg = style.colors.frameBgHovered = style.colors.frameBgActive = Color(0, 0, 0, 0);
    style.colors.border = style.colors.accent = Color(0, 0, 0, 0);
    ui::SetCursorPos(textArea.min);
    if (ai.focusInput && ai.openAnim > 0.6f && !ai.popupOpen) {
        ui::SetKeyboardFocusHere();
        ai.focusInput = false;
    }
    const bool submit = ui::InputTextMultiline("##ai_input", ai.input, textArea.Size() / s,
                                               ui::InputTextFlags::SubmitOnEnter | ui::InputTextFlags::KeepFocusOnEnter,
                                               busy ? StringView("Working... (you can type the next message)") : StringView("Ask Zeqou AI to build something"));
    ai.inputActive = ui::IsItemActive();
    style.colors = saved;

    // Bottom bar.
    const Rect bar(Vec2(box.min.x + 6.0f * s, box.max.y - barH), Vec2(box.max.x - 6.0f * s, box.max.y - 4.0f * s));
    // Model pill: provider · model ▾
    const u32 prov = ai.settings.provider;
    const String model = AIModel(ai);
    const String label = model.Empty() ? String("Choose a model") : DisplayName(ai, prov, model);
    const ui::FontHandle medium = Font("Inter Medium");
    const f32 fs = 12.0f * s;
    const f32 pillW = Min(bar.Width() - 90.0f * s, p.ctx.Fonts().MeasureWidth(medium, fs, label) + 34.0f * s);
    const Rect pill(Vec2(bar.min.x, bar.min.y + 1.0f * s), Vec2(bar.min.x + pillW, bar.max.y - 1.0f * s));
    ui::SetCursorPos(pill.min);
    const bool pillClicked = ui::InvisibleButton("##ai_model_pill", pill.Size() / s);
    if (pillClicked || ai.pickerRequest) {
        // The console (ai.picker) can pick the provider to show; a click always starts from the provider in use.
        ai.pickerProvider = ai.pickerRequest ? ai.pickerProvider : prov;
        ai.pickerRequest = false;
        ai.modelSearch.Clear();
        ai.focusModelSearch = true;
        // Above the composer when there is no room below.
        ui::OpenPopupAnchored("##ai_model_picker", pill);
        RefreshAIModels(ai, ai.pickerProvider);
    }
    const bool pillHov = ui::IsItemHovered();
    ui::ItemTooltip(model.Empty() ? String("Choose the model") : String::Format("{} - {}\nClick to change the model", Info(prov).name, model));
    if (pillHov || ai.popupOpen)
        dl.AddRect(pill, p.t.buttonHovered, ui::CornerRadii(5.0f * s));
    dl.AddTextInRect(medium, fs, Rect(pill.min + Vec2(8.0f * s, 0.0f), pill.max - Vec2(20.0f * s, 0.0f)), model.Empty() ? p.t.warning : p.t.textDim, label);
    dl.AddTextInRect(Font("Inter"), 11.0f * s, Rect(Vec2(pill.max.x - 20.0f * s, pill.min.y), pill.max), p.t.textDisabled, ZX_ICON_CHEVRON_DOWN,
                     ui::TextAlign::Center, false);

    // Send / stop.
    const f32 bs = 26.0f * s;
    const Rect send(Vec2(bar.max.x - bs, bar.Center().y - bs * 0.5f), Vec2(bar.max.x, bar.Center().y + bs * 0.5f));
    const bool hasText = !ai.input.Trimmed().Empty();
    ui::SetCursorPos(send.min);
    const bool clicked = ui::InvisibleButton("##ai_send", send.Size() / s);
    const bool sendHov = ui::IsItemHovered();
    ui::ItemTooltip(busy ? "Stop (Esc)" : "Send (Enter)  -  new line: Shift+Enter");
    const bool active = busy || hasText;
    dl.AddRect(send, active ? (sendHov ? p.t.accentHovered : p.t.accent) : p.t.button, ui::CornerRadii(6.0f * s));
    dl.AddTextInRect(Font("Inter"), (busy ? 11.0f : 14.0f) * s, send, active ? p.t.accentText : p.t.textDisabled, busy ? ZX_ICON_SQUARE : ZX_ICON_ARROW_UP,
                     ui::TextAlign::Center, false);

    // Context ring: how much of the model's context window the conversation uses.
    const u32 window = AIContextWindow(ai);
    const u64 used = ai.lastContextTokens;
    const f32 frac = window ? f32(f64(used) / f64(window)) : 0.0f;
    // The ring eases towards the real fill, so a new turn animates instead of snapping (about 0.3 s).
    ai.ringAnim += (frac - ai.ringAnim) * Saturate(ai.frameDt * 9.0f);
    if (Abs(frac - ai.ringAnim) < 0.0005f)
        ai.ringAnim = frac;
    const f32 shownFrac = ai.ringAnim;
    const f32 rr = 8.0f * s;
    const Vec2 rc(send.min.x - 14.0f * s - rr, bar.Center().y);
    const Rect ringRect(rc - Vec2(rr + 4.0f * s), rc + Vec2(rr + 4.0f * s));
    ui::SetCursorPos(ringRect.min);
    if (ui::InvisibleButton("##ai_ctx_ring", ringRect.Size() / s))
        OpenSettingsPage(p.ed, "AI");
    if (ui::IsItemHovered()) {
        StringView src;
        ContextOf(ai, prov, model, &src);
        const StringView how = src == "manual" ? "set by hand" : src == "provider" ? "reported by the provider" : src == "known" ? "known for this model" : "default (unknown model)";
        ui::SetTooltip(String::Format("Context: {} / {} tokens ({:.0f}%)\nWindow {}: {}\nOlder messages are dropped when it fills up.\nClick to change.",
                                      FormatTokens(used), FormatTokens(window), f64(frac) * 100.0, how, FormatTokens(window)));
    }
    DrawRing(dl, rc, rr, 2.2f * s, shownFrac, p.t); // the arc eases; the numbers below show the real value
    if (frac >= 0.995f)
        dl.AddCircle(rc, 2.0f * s, p.t.error);
    // Percentage left of the ring once it matters.
    if (frac >= 0.5f) {
        const String pct = String::Format("{:.0f}%", f64(frac) * 100.0);
        const f32 pw = p.ctx.Fonts().MeasureWidth(p.text.regular, 11.0f * s, pct);
        dl.AddTextInRect(p.text.regular, 11.0f * s, Rect(Vec2(ringRect.min.x - pw - 4.0f * s, bar.min.y), Vec2(ringRect.min.x - 2.0f * s, bar.max.y)),
                         frac < 0.9f ? p.t.warning : p.t.error, pct, ui::TextAlign::Right, false);
    }

    if (busy && clicked) {
        ai.agent.Cancel();
    } else if ((submit || clicked) && hasText && !busy) {
        SendMessage(p.ed, ai.input);
        ai.input.Clear();
        ai.focusInput = true;
    }
    DrawModelPicker(p);
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
    if (ai.open && ai.openAnim > 0.5f && ui::IsKeyPressed(Key::Escape, false) && !ed.IsPlaying() && !ai.popupOpen) {
        if (ai.agent.IsBusy() && ai.inputActive)
            ai.agent.Cancel(); // Esc in the message box stops the turn
        else if (!ai.inputActive)
            ai.open = false;
    }
    if (ai.openAnim <= 0.001f || ai.buttonRect.Width() <= 0.0f)
        return;
    for (const Editor::CaptureRequest& r : ed.captureRequests)
        if (r.hideOverlays)
            return; // an AI screenshot of the viewport is taken this frame
    if (!ai.monoLoaded) {
        ai.monoLoaded = true;
        const StringView faces[] = {"C:/Windows/Fonts/consola.ttf"};
        ai.mono = platform::FileExists(faces[0]) ? ctx.Fonts().AddFont("AI Mono", Span<const StringView>(faces, 1)) : ui::FontHandle{};
        if (!ai.mono.IsValid())
            ai.mono = Font("Inter");
    }
    const f32 s = ctx.GetScale();
    const ui::Theme& t = ctx.GetStyle().colors;
    const Vec2 display(f32(ed.swapChain->GetSize().x), f32(ed.swapChain->GetSize().y));
    // Slides in from the right edge, below the toolbar, down to the status bar.
    // The spring is critically damped and stays within [0, 1]; the clamp below is only a guard against numerical overshoot.
    const f32 ease = 1.0f - (1.0f - Saturate(ai.openAnim)) * (1.0f - Saturate(ai.openAnim)) * (1.0f - Saturate(ai.openAnim));
    const f32 w = Round(Min(ai.settings.panelWidth * s, display.x * 0.7f));
    const f32 top = Round(ai.buttonRect.max.y + 8.0f * s);
    const f32 bottom = Round(display.y - 30.0f * s);
    const f32 margin = 8.0f * s;
    const f32 x = Round(display.x - margin - w + (1.0f - ease) * (w * 0.15f + margin));
    const Rect panel(Vec2(x, top), Vec2(x + w, bottom));
    if (panel.Height() < 200.0f * s)
        return;
    const f32 alpha = Saturate(ease * 1.3f);

    ui::SetNextWindowPos(panel.min / s, ui::Cond::Always);
    ui::SetNextWindowSize(panel.Size() / s, ui::Cond::Always);
    ui::Begin("##ZeqouAI", nullptr,
              ui::WindowFlags::NoTitleBar | ui::WindowFlags::NoResize | ui::WindowFlags::NoMove | ui::WindowFlags::NoScrollbar | ui::WindowFlags::NoCollapse |
                  ui::WindowFlags::NoBackground);
    DrawList& dl = ui::GetWindowDrawList();
    dl.PushClipRect(Rect(Vec2::Zero(), display), 0.0f, false);
    const f32 radius = 8.0f * s;
    dl.AddShadow(panel, WithAlpha(Color(0, 0, 0), 0.45f * alpha), radius, 24.0f * s, Vec2(0.0f, 6.0f * s));
    dl.AddRect(panel, WithAlpha(t.windowBg, alpha), ui::CornerRadii(radius), 1.0f, WithAlpha(t.borderStrong, alpha));
    dl.PopClipRect();
    if (alpha < 0.6f) {
        ui::End();
        return;
    }
    dl.PushClipRect(panel, radius);

    PanelCtx p{ed, ai, ctx, t, s, {}, 0.0f, 0.0f, 0.0f};
    p.text.regular = Font("Inter");
    p.text.bold = Font("Inter SemiBold");
    p.text.italic = Font("Inter Italic").IsValid() ? Font("Inter Italic") : p.text.regular;
    p.text.mono = ai.mono;
    p.text.size = 13.5f * s;
    p.text.code = Color(0xE6, 0xB8, 0x7A);
    p.text.codeBg = WithAlpha(t.text, 0.07f);
    p.text.link = t.accentHovered;
    p.lineH = Round(p.text.size * 1.5f);

    // Resize from the left edge.
    {
        const Rect grip(Vec2(panel.min.x - 3.0f * s, panel.min.y + radius), Vec2(panel.min.x + 4.0f * s, panel.max.y - radius));
        ui::SetCursorPos(grip.min);
        ui::InvisibleButton("##ai_resize", grip.Size() / s);
        if (ui::IsItemHovered() || ui::IsItemActive())
            ctx.SetCursorShape(CursorShape::ResizeEW);
        if (ui::IsItemActive()) {
            if (ai.resizeStartW == 0.0f) {
                ai.resizeStartW = ai.settings.panelWidth;
                ai.resizeStartX = ui::GetMousePos().x;
            }
            ai.settings.panelWidth = Clamp(ai.resizeStartW + (ai.resizeStartX - ui::GetMousePos().x) / s, 340.0f, Min(900.0f, display.x * 0.7f / s));
            dl.AddRect(Rect(Vec2(panel.min.x, panel.min.y + radius), Vec2(panel.min.x + 2.0f * s, panel.max.y - radius)), t.accent);
        } else if (ai.resizeStartW != 0.0f) {
            ai.resizeStartW = 0.0f;
            SaveAISettings(ai.settings);
        }
    }

    // Header: title, connection state, actions.
    const f32 headH = 42.0f * s;
    const f32 pad = 14.0f * s;
    const bool busy = ai.agent.IsBusy();
    {
        const Rect head(panel.min, Vec2(panel.max.x, panel.min.y + headH));
        dl.AddRect(Rect(Vec2(head.min.x, head.max.y - 1.0f), Vec2(head.max.x, head.max.y)), t.border);
        dl.AddTextInRect(Font("Inter"), 14.0f * s, Rect(head.min + Vec2(pad - 2.0f * s, 0.0f), Vec2(head.min.x + pad + 16.0f * s, head.max.y)), t.accentHovered,
                         ZX_ICON_SPARKLES, ui::TextAlign::Center, false);
        const f32 titleX = head.min.x + pad + 24.0f * s;
        dl.AddTextInRect(Font("Inter SemiBold"), 13.5f * s, Rect(Vec2(titleX, head.min.y), Vec2(titleX + 80.0f * s, head.max.y)), t.text,
                         ai.page == 1 ? "Connect" : "Zeqou AI");
        // Status chip: MCP client / state.
        String status;
        Color dot = t.textDisabled;
        if (ai.mcp.IsRunning()) {
            const String client = ai.mcp.LastClient();
            status = client.Empty() ? String("MCP on") : String::Format("{} connected", client);
            dot = t.success;
        }
        if (!status.Empty()) {
            const f32 sx = titleX + p.ctx.Fonts().MeasureWidth(Font("Inter SemiBold"), 13.5f * s, ai.page == 1 ? "Connect" : "Zeqou AI") + 12.0f * s;
            dl.AddCircle(Vec2(sx + 3.0f * s, head.Center().y), 3.0f * s, dot);
            dl.AddTextInRect(p.text.regular, 11.5f * s, Rect(Vec2(sx + 10.0f * s, head.min.y), Vec2(head.max.x - 120.0f * s, head.max.y)), t.textDim, status);
        }
        const f32 bsz = 26.0f * s;
        Vec2 bp(head.max.x - pad + 4.0f * s - bsz, head.Center().y - bsz * 0.5f);
        if (HeaderButton(p, "##ai_close", ZX_ICON_X, bp, bsz, "Close (Esc)"))
            ai.open = false;
        bp.x -= bsz + 2.0f * s;
        if (HeaderButton(p, "##ai_settings", ZX_ICON_SETTINGS, bp, bsz, "Settings > AI"))
            OpenSettingsPage(ed, "AI");
        bp.x -= bsz + 2.0f * s;
        if (HeaderButton(p, "##ai_page", ai.page == 1 ? ZX_ICON_MESSAGE_CIRCLE : ZX_ICON_PLUG, bp, bsz,
                         ai.page == 1 ? "Back to the chat" : "Connect an AI agent (MCP)"))
            ai.page = ai.page == 1 ? 0 : 1;
        bp.x -= bsz + 2.0f * s;
        if (ai.page == 0 && HeaderButton(p, "##ai_new", ZX_ICON_SQUARE_PEN, bp, bsz, busy ? "New chat (after this turn)" : "New chat", !ai.chat.Empty() && !busy))
            NewChat(ai);
    }

    // Composer height follows its text (1..8 lines).
    const u32 lines = Clamp<u32>(u32(SplitString(ai.input, '\n').Size()), 1u, 8u);
    const f32 inputLine = ctx.Fonts().GetLineMetrics(p.text.regular, ctx.GetStyle().fontSize * s).lineHeight;
    const f32 composerH = ai.page == 0 ? Round(f32(lines) * inputLine + 2.0f * ctx.GetStyle().framePadding.y * s + 8.0f * s + 32.0f * s) : 0.0f;
    const Rect body(Vec2(panel.min.x + 1.0f, panel.min.y + headH), Vec2(panel.max.x - 1.0f, panel.max.y - (ai.page == 0 ? composerH + pad * 1.4f : 1.0f)));
    ui::SetCursorPos(body.min);
    if (ui::BeginChild("##ai_body", body.Size() / s, false)) {
        const f32 cw = body.Width() - pad * 2.0f - ctx.GetStyle().scrollbarSize * s * 0.5f;
        p.visibleMin = body.min.y;
        p.visibleMax = body.max.y;
        ui::Indent(pad / s);
        ui::Dummy(Vec2(0.0f, 12.0f));
        if (ai.page == 1) {
            ui::TextWrapped("Any agent that speaks the Model Context Protocol can build in this editor: create objects, write "
                            "scripts, run commands, play and take screenshots.");
            ui::Spacing();
            DrawMcpConnect(ed, cw);
        } else if (ai.chat.Empty()) {
            DrawWelcome(p, cw);
        } else {
            // Follow new content unless the user scrolled up.
            const f32 scrollY = ui::GetScrollY(), scrollMax = ui::GetScrollMaxY();
            if (Abs(scrollY - ai.lastSetScroll) > 1.0f)
                ai.stickToBottom = scrollY >= scrollMax - 24.0f * s;
            for (usize i = 0; i < ai.chat.Size(); ++i) {
                if (i > 0)
                    ui::Dummy(Vec2(0.0f, GapBefore(ai.chat[i - 1], ai.chat[i])));
                DrawItem(p, ai.chat[i], i, cw);
            }
            const bool runningTool = ai.chat.Back().kind == AIChatItem::Kind::Tool && ai.chat.Back().running;
            if (busy && !runningTool) {
                ui::Dummy(Vec2(0.0f, 10.0f));
                DrawWorking(p, cw);
            }
            const ai::AgentUsage u = ai.agent.Usage();
            if (u.requests && !busy) {
                ui::Dummy(Vec2(0.0f, 10.0f));
                String usage = String::Format("{} \xC2\xB7 {} requests \xC2\xB7 {} in", DisplayName(ai, ai.settings.provider, AIModel(ai)), u.requests,
                                              FormatTokens(u.inputTokens + u.cacheReadTokens));
                if (u.cacheReadTokens)
                    usage.Appendf(" ({} cached)", FormatTokens(u.cacheReadTokens));
                usage.Appendf(" \xC2\xB7 {} out", FormatTokens(u.outputTokens));
                ui::PushFont(ui::FontStyle::Regular, 11.5f);
                ui::TextDisabled(usage);
                ui::PopFont();
            }
            ui::Dummy(Vec2(0.0f, 14.0f));
            // Follow the newest message with an ease instead of a jump. The target is the scroll range of the last
            // frame (the layout is only known after this pass); while the user has scrolled up, the chat stays put.
            if (ai.stickToBottom) {
                const f32 target = ui::GetScrollMaxY();
                const f32 cur = ui::GetScrollY();
                const f32 next = cur + (target - cur) * Saturate(ai.frameDt * 10.0f);
                ui::SetScrollY(Abs(target - next) < 0.5f ? target : next);
                ai.lastSetScroll = ui::GetScrollY();
            }
            if (!ai.stickToBottom) {
                // "Jump to latest" when scrolled up.
                const Vec2 c(body.Center().x, body.max.y - 24.0f * s);
                const Rect jr(c - Vec2(15.0f * s), c + Vec2(15.0f * s));
                DrawList& fg = ui::GetWindowDrawList();
                ui::SetCursorPos(jr.min);
                if (ui::InvisibleButton("##ai_jump", jr.Size() / s))
                    ai.stickToBottom = true;
                ui::ItemTooltip("Jump to the latest message");
                fg.AddRect(jr, ui::IsItemHovered() ? t.buttonHovered : t.button, ui::CornerRadii(15.0f * s), 1.0f, t.borderStrong);
                fg.AddTextInRect(Font("Inter"), 14.0f * s, jr, t.text, ZX_ICON_ARROW_DOWN, ui::TextAlign::Center, false);
            }
        }
        ui::Unindent(pad / s);
    }
    ui::EndChild();
    if (ai.page == 0) {
        DrawComposer(p, Rect(Vec2(panel.min.x + pad, panel.max.y - pad - composerH), Vec2(panel.max.x - pad, panel.max.y - pad)));
    }
    dl.PopClipRect();
    ui::End();
}

// --- Settings > AI --------------------------------------------------------------------------------------------------

void DrawAISettingsPage(Editor& ed)
{
    if (!ed.ai)
        return;
    AIState& ai = *ed.ai;
    AISettings& st = ai.settings;
    ui::Context& ctx = *ui::GetCurrentContext();
    const ui::Theme& t = ctx.GetStyle().colors;
    bool changed = false;
    ui::SeparatorText("Model");
    ui::TextWrapped("Zeqou AI talks to the model with your own API key. Keys are encrypted for your Windows account and "
                    "stored in %LOCALAPPDATA%/ZeqouX3D/AI.json - never in a project.");
    ui::Spacing();

    // Provider.
    Array<StringView> names;
    Array<String> labels;
    for (const AIProviderInfo& p : AIProviders())
        labels.PushBack(String::Format("{}{}", p.name, ProviderReady(st, u32(&p - AIProviders().Data())) ? "  \xE2\x9C\x93" : ""));
    for (const String& l : labels)
        names.PushBack(l);
    i32 provider = i32(st.provider);
    if (ui::Combo("Provider", &provider, names.View())) {
        st.provider = u32(provider);
        changed = true;
    }
    const u32 prov = st.provider;
    const AIProviderInfo& info = AIProviders()[prov];

    // Key / URL.
    String& key = st.apiKeys[prov];
    if (info.needsKey || !info.local) {
        if (ui::InputText("API Key", key, ui::InputTextFlags::Password, info.keyHint[0] ? StringView(info.keyHint) : StringView("API key"))) {
            changed = true;
            ai.lists[prov] = AIModelList{};
        }
    }
    if (info.keyUrl[0] != '\0') {
        if (ui::TextLink(String::Format("Get a {} API key", info.name)))
            platform::OpenUrl(info.keyUrl);
    }
    if (info.local || prov == FindAIProvider("custom")) {
        if (ui::InputText("Server URL", st.baseUrls[prov], ui::InputTextFlags::None, info.baseUrl[0] ? StringView(info.baseUrl) : StringView("https://.../v1"))) {
            changed = true;
            ai.lists[prov] = AIModelList{};
        }
        if (info.local)
            ui::TextDisabled("Start the server first; any model it has loaded appears in the list.");
    } else if (ui::TreeNode("Advanced##ai_url")) {
        if (ui::InputText("Base URL", st.baseUrls[prov], ui::InputTextFlags::None, info.baseUrl))
            changed = true;
        ui::ItemTooltip("Leave empty for the provider's own endpoint (proxies, regional endpoints)");
        ui::TreePop();
    }

    // Model: the fetched list with context sizes, or a typed id.
    RefreshAIModels(ai, prov);
    const AIModelList& list = ai.lists[prov];
    const String current = ModelFor(ai, prov);
    ui::Spacing();
    ui::AlignTextToFramePadding();
    ui::Text("Model");
    ui::SameLine(ui::GetContentRegionAvail().x * 0.0f + 120.0f);
    ui::SetNextItemWidth(-36.0f);
    String typed = st.models[prov];
    if (ui::InputText("##ai_model_id", typed, ui::InputTextFlags::None, info.defaultModel[0] ? StringView(info.defaultModel) : StringView("model id"))) {
        st.models[prov] = typed.Trimmed();
        changed = true;
    }
    ui::SameLine(0.0f, 4.0f);
    const bool fetching = ai.fetching.load() && ai.fetchProvider == prov;
    ui::BeginDisabled(fetching || !ProviderReady(st, prov));
    if (ui::IconButton(ZX_ICON_REFRESH_CW, "Reload the model list"))
        RefreshAIModels(ai, prov, true);
    ui::EndDisabled();
    if (fetching) {
        ui::Spinner(6.0f);
        ui::SameLine();
        ui::TextDisabled("Loading models...");
    } else if (!list.error.Empty()) {
        ui::TextColored(list.fetched ? t.textDim : t.error, list.error);
    } else if (list.fetched) {
        ui::SetNextItemWidth(-1.0f);
        ui::InputText("##ai_models_filter", ai.settingsFilter, ui::InputTextFlags::None,
                      String::Format(ZX_ICON_SEARCH "  Filter {} models", list.models.Size()));
        if (ui::BeginChild("##ai_models_list", Vec2(0.0f, 150.0f), true)) {
            for (const ai::ModelInfo& m : list.models) {
                if (!MatchesFilter(m.id, ai.settingsFilter) && !MatchesFilter(m.name, ai.settingsFilter))
                    continue;
                const u32 ctxTokens = ContextOf(ai, prov, m.id, nullptr);
                ui::PushID(m.id);
                if (ui::Selectable(String::Format("{}{}", m.id, ctxTokens ? String::Format("   \xC2\xB7  {}", FormatTokens(ctxTokens)) : String()), m.id == current)) {
                    st.models[prov] = m.id;
                    changed = true;
                }
                ui::PopID();
            }
        }
        ui::EndChild();
    }

    // Context window: detected for the model, or set by hand (1k .. 1M).
    ui::Spacing();
    ui::SeparatorText("Context window");
    if (!current.Empty()) {
        StringView src;
        const u32 detected = [&] {
            // What it would be without a manual value.
            const String k = OverrideKey(info.id, current);
            u32 saved = 0;
            if (const u32* v = st.contextOverrides.Find(k))
                saved = *v;
            st.contextOverrides.Remove(k);
            const u32 d = ContextOf(ai, prov, current, &src);
            if (saved)
                st.contextOverrides.InsertOrAssign(k, saved);
            return d;
        }();
        const String overrideKey = OverrideKey(info.id, current);
        bool manual = st.contextOverrides.Contains(overrideKey);
        const StringView how = src == "provider" ? "reported by the provider" : src == "known" ? "known for this model family" : "unknown model - default";
        if (ui::Checkbox("Set by hand", &manual)) {
            if (manual)
                st.contextOverrides.InsertOrAssign(overrideKey, detected);
            else
                st.contextOverrides.Remove(overrideKey);
            changed = true;
        }
        ui::SameLine();
        ui::TextDisabled(String::Format("Detected: {} tokens ({})", FormatTokens(detected), how));
        if (manual) {
            i32 k = i32(*st.contextOverrides.Find(overrideKey) / 1000);
            ui::SetNextItemWidth(-90.0f);
            if (ui::SliderInt("##ai_ctx_k", &k, 1, i32(ai::MaxContextWindow / 1000))) {
                st.contextOverrides.InsertOrAssign(overrideKey, u32(Clamp(k, 1, i32(ai::MaxContextWindow / 1000))) * 1000u);
                changed = true;
            }
            ui::SameLine();
            ui::Text(String::Format("{}k tokens", k));
            static constexpr u32 Presets[] = {8000, 32000, 128000, 200000, 400000, 1000000};
            for (u32 v : Presets) {
                if (ui::Button(FormatTokens(v))) {
                    st.contextOverrides.InsertOrAssign(overrideKey, v);
                    changed = true;
                }
                ui::SameLine(0.0f, 4.0f);
            }
            ui::NewLine();
        }
        ui::TextDisabled(String::Format("Using {} tokens for {}. When the conversation gets bigger, the oldest messages are dropped.",
                                        FormatTokens(ContextOf(ai, prov, current, nullptr)), current));
    } else {
        ui::TextDisabled("Choose a model first.");
    }

    // Generation.
    ui::Spacing();
    ui::SeparatorText("Generation");
    if (info.wire == Provider::Anthropic) {
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
    DrawMcpConnect(ed, ui::GetContentRegionAvail().x - 12.0f * ctx.GetScale());
}

} // namespace zx::editor
