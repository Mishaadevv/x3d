#include "Engine/AIAssistant.h"

#include "Core/Log/Log.h"
#include "Core/String/Utf.h"

ZX_DEFINE_LOG_CATEGORY_STATIC(LogAI, Info);

namespace zx::ai {

// --- MCP server ---------------------------------------------------------------------------------------------------

namespace {

constexpr StringView McpSupportedVersions[] = {"2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05"};

// JSON-RPC error codes.
constexpr i64 ParseError = -32700;
constexpr i64 InvalidRequest = -32600;
constexpr i64 MethodNotFound = -32601;
constexpr i64 InvalidParams = -32602;

String RpcResult(const JsonValue& id, JsonValue result)
{
    JsonValue r = JsonValue::MakeObject();
    r.Set("jsonrpc", "2.0");
    r.Set("id", id);
    r.Set("result", move(result));
    return ToJsonString(r, JsonWriteOptions{.pretty = false});
}

String RpcError(const JsonValue& id, i64 code, StringView message)
{
    JsonValue e = JsonValue::MakeObject();
    e.Set("code", code);
    e.Set("message", message);
    JsonValue r = JsonValue::MakeObject();
    r.Set("jsonrpc", "2.0");
    r.Set("id", id);
    r.Set("error", move(e));
    return ToJsonString(r, JsonWriteOptions{.pretty = false});
}

/// Browsers send Origin; only pages served from this machine may talk to the server (DNS rebinding protection).
bool IsLocalOrigin(StringView origin)
{
    if (origin.Empty() || origin == "null")
        return true;
    for (StringView prefix : {StringView("http://127.0.0.1"), StringView("http://localhost"), StringView("https://127.0.0.1"),
                              StringView("https://localhost"), StringView("http://[::1]"), StringView("vscode-file://"),
                              StringView("vscode-webview://")}) {
        if (origin.StartsWith(prefix)) {
            const StringView rest = origin.DropLeft(prefix.Size());
            if (rest.Empty() || rest[0] == ':' || rest[0] == '/' || prefix.EndsWith("//"))
                return true;
        }
    }
    return false;
}

bool ConstantTimeEquals(StringView a, StringView b)
{
    u32 diff = u32(a.Size() ^ b.Size());
    for (usize i = 0; i < a.Size(); ++i)
        diff |= u32(u8(a[i]) ^ u8(i < b.Size() ? b[i] : 0));
    return diff == 0;
}

JsonValue ToolToMcp(const ToolDef& t)
{
    JsonValue o = JsonValue::MakeObject();
    o.Set("name", t.name);
    if (!t.title.Empty())
        o.Set("title", t.title);
    o.Set("description", t.description);
    JsonValue schema = t.inputSchema;
    if (!schema.IsObject()) {
        schema = JsonValue::MakeObject();
        schema.Set("type", "object");
    }
    o.Set("inputSchema", move(schema));
    JsonValue ann = JsonValue::MakeObject();
    ann.Set("readOnlyHint", t.readOnly);
    ann.Set("openWorldHint", false);
    o.Set("annotations", move(ann));
    return o;
}

const ToolDef* FindTool(Span<const ToolDef> tools, StringView name)
{
    for (const ToolDef& t : tools)
        if (t.name == name)
            return &t;
    return nullptr;
}

} // namespace

void McpServer::Configure(McpOptions options, ToolHost& host)
{
    m_options = move(options);
    m_host = &host;
}

bool McpServer::Start(u16 port, McpOptions options, ToolHost& host, String* error)
{
    Configure(move(options), host);
    return m_http.Start(port, [this](const platform::HttpServerRequest& r) { return Handle(r); }, error);
}

void McpServer::Stop() { m_http.Stop(); }

String McpServer::LastClient() const
{
    ScopedLock lock(m_mutex);
    return m_lastClient;
}

platform::HttpServerResponse McpServer::Handle(const platform::HttpServerRequest& request)
{
    platform::HttpServerResponse res;
    auto plain = [&](u32 status, StringView text) {
        res.status = status;
        res.contentType = "text/plain; charset=utf-8";
        res.body = text;
        return res;
    };
    if (request.path != "/mcp" && request.path != "/mcp/" && request.path != "/")
        return plain(404, "Not found. The MCP endpoint is /mcp");
    if (!IsLocalOrigin(request.Header("Origin")))
        return plain(403, "Origin not allowed");
    if (!m_options.token.Empty()) {
        const StringView auth = request.Header("Authorization");
        if (!auth.StartsWith("Bearer ") || !ConstantTimeEquals(auth.DropLeft(7).Trim(), m_options.token)) {
            res.headers.PushBack({"WWW-Authenticate", "Bearer realm=\"ZeqouX3D\""});
            return plain(401, "Missing or wrong token: send \"Authorization: Bearer <token>\" (Settings > AI in the editor)");
        }
    }
    if (request.method != "POST") {
        res.headers.PushBack({"Allow", "POST"});
        return plain(405, "This server answers POST requests with JSON (no SSE stream)");
    }
    JsonValue message;
    JsonParseError parseError;
    if (!ParseJson(request.body, message, &parseError)) {
        res.status = 400;
        res.body = RpcError(JsonValue(), ParseError, String::Format("Parse error: {}", parseError.message));
        return res;
    }
    if (message.IsArray()) {
        // Batches (protocol 2025-03-26).
        String out = "[";
        bool any = false;
        for (const JsonValue& m : message.Elements()) {
            const String r = HandleMessage(m);
            if (r.Empty())
                continue;
            if (any)
                out += ",";
            out += r;
            any = true;
        }
        out += "]";
        if (!any) {
            res.status = 202;
            res.contentType.Clear();
            return res;
        }
        res.body = move(out);
        return res;
    }
    res.body = HandleMessage(message);
    if (res.body.Empty()) {
        res.status = 202; // notification or response: accepted, no body
        res.contentType.Clear();
    }
    return res;
}

String McpServer::HandleMessage(const JsonValue& message)
{
    if (!message.IsObject())
        return RpcError(JsonValue(), InvalidRequest, "Invalid request");
    const JsonValue* id = message.Find("id");
    const StringView method = message.Get("method").AsString();
    if (!id || id->IsNull())
        return {}; // notification (notifications/initialized, cancelled, ...)
    if (method.Empty())
        return {}; // a response to a server request (we send none)
    const JsonValue& params = message.Get("params");

    if (method == "initialize") {
        StringView version = params.Get("protocolVersion").AsString();
        bool supported = false;
        for (StringView v : McpSupportedVersions)
            supported |= v == version;
        if (!supported)
            version = McpProtocolVersion;
        const JsonValue& client = params.Get("clientInfo");
        {
            ScopedLock lock(m_mutex);
            m_lastClient = String::Format("{} {}", client.Get("name").AsString("client"), client.Get("version").AsString());
            m_lastClient.TrimInPlace();
        }
        ZX_LOG_INFO(LogAI, "MCP client connected: {} (protocol {})", LastClient(), version);
        JsonValue r = JsonValue::MakeObject();
        r.Set("protocolVersion", version);
        JsonValue caps = JsonValue::MakeObject();
        caps["tools"].Set("listChanged", false);
        r.Set("capabilities", move(caps));
        JsonValue info = JsonValue::MakeObject();
        info.Set("name", m_options.serverName);
        info.Set("title", m_options.serverTitle);
        info.Set("version", m_options.serverVersion);
        r.Set("serverInfo", move(info));
        if (!m_options.instructions.Empty())
            r.Set("instructions", m_options.instructions);
        return RpcResult(*id, move(r));
    }
    if (method == "ping")
        return RpcResult(*id, JsonValue::MakeObject());
    if (!m_host)
        return RpcError(*id, InvalidRequest, "Server not ready");
    if (method == "tools/list") {
        JsonValue list = JsonValue::MakeArray();
        for (const ToolDef& t : m_host->Tools())
            list.PushBack(ToolToMcp(t));
        JsonValue r = JsonValue::MakeObject();
        r.Set("tools", move(list));
        return RpcResult(*id, move(r));
    }
    if (method == "tools/call") {
        const StringView name = params.Get("name").AsString();
        if (!FindTool(m_host->Tools(), name))
            return RpcError(*id, InvalidParams, String::Format("Unknown tool: {}", name));
        const JsonValue* args = params.Find("arguments");
        m_toolCalls.fetch_add(1);
        const ToolResult tr = m_host->Call(name, args && args->IsObject() ? *args : JsonValue(JsonValue::MakeObject()));
        JsonValue content = JsonValue::MakeArray();
        if (!tr.text.Empty() || tr.pngBase64.Empty()) {
            JsonValue t = JsonValue::MakeObject();
            t.Set("type", "text");
            t.Set("text", tr.text);
            content.PushBack(move(t));
        }
        if (!tr.pngBase64.Empty()) {
            JsonValue img = JsonValue::MakeObject();
            img.Set("type", "image");
            img.Set("data", tr.pngBase64);
            img.Set("mimeType", "image/png");
            content.PushBack(move(img));
        }
        JsonValue r = JsonValue::MakeObject();
        r.Set("content", move(content));
        r.Set("isError", tr.isError);
        return RpcResult(*id, move(r));
    }
    if (method == "resources/list" || method == "prompts/list") {
        JsonValue r = JsonValue::MakeObject();
        r.Set(method == "resources/list" ? "resources" : "prompts", JsonValue::MakeArray());
        return RpcResult(*id, move(r));
    }
    return RpcError(*id, MethodNotFound, String::Format("Method not found: {}", method));
}

// --- Wire helpers ---------------------------------------------------------------------------------------------------

StringView DefaultModel(Provider provider)
{
    switch (provider) {
    case Provider::Anthropic: return "claude-opus-5-5";
    case Provider::OpenAI: return "gpt-5";
    case Provider::OpenAICompatible: return "";
    }
    return "";
}

StringView DefaultBaseUrl(Provider provider)
{
    switch (provider) {
    case Provider::Anthropic: return "https://api.anthropic.com";
    case Provider::OpenAI: return "https://api.openai.com/v1";
    case Provider::OpenAICompatible: return "http://localhost:11434/v1";
    }
    return "";
}

namespace {

constexpr StringView AnthropicVersion = "2026-10-21";
constexpr StringView FallbackBeta = "server-side-fallback-2026-07-01";

StringView ModelOf(const AgentConfig& c) { return c.model.Empty() ? DefaultModel(c.provider) : c.model.View(); }

String BaseUrlOf(const AgentConfig& c)
{
    String base = c.baseUrl.Empty() ? String(DefaultBaseUrl(c.provider)) : c.baseUrl.Trimmed();
    while (base.EndsWith("/"))
        base.PopBack();
    return base;
}

JsonValue SchemaOf(const ToolDef& t)
{
    if (t.inputSchema.IsObject())
        return t.inputSchema;
    JsonValue s = JsonValue::MakeObject();
    s.Set("type", "object");
    s.Set("properties", JsonValue::MakeObject());
    return s;
}

} // namespace

JsonValue BuildAnthropicRequest(const AgentConfig& config, StringView system, Span<const ToolDef> tools, const JsonValue& messages)
{
    JsonValue body = JsonValue::MakeObject();
    body.Set("model", ModelOf(config));
    body.Set("max_tokens", config.maxTokens);
    if (!system.Empty()) {
        JsonValue block = JsonValue::MakeObject();
        block.Set("type", "text");
        block.Set("text", system);
        JsonValue sys = JsonValue::MakeArray();
        sys.PushBack(move(block));
        body.Set("system", move(sys));
    }
    if (!tools.Empty()) {
        JsonValue list = JsonValue::MakeArray();
        for (const ToolDef& t : tools) {
            JsonValue o = JsonValue::MakeObject();
            o.Set("name", t.name);
            o.Set("description", t.description);
            o.Set("input_schema", SchemaOf(t));
            list.PushBack(move(o));
        }
        body.Set("tools", move(list));
    }
    body.Set("messages", messages);
    if (!config.effort.Empty())
        body["output_config"].Set("effort", config.effort);
    // The conversation grows by appending: cache everything up to the last block.
    body["cache_control"].Set("type", "ephemeral");
    if (config.serverFallbacks)
        body.Set("fallbacks", "default");
    return body;
}

JsonValue BuildOpenAIRequest(const AgentConfig& config, StringView system, Span<const ToolDef> tools, const JsonValue& messages)
{
    JsonValue body = JsonValue::MakeObject();
    body.Set("model", ModelOf(config));
    JsonValue list = JsonValue::MakeArray();
    if (!system.Empty()) {
        JsonValue m = JsonValue::MakeObject();
        m.Set("role", config.provider == Provider::OpenAI ? "developer" : "system");
        m.Set("content", system);
        list.PushBack(move(m));
    }
    for (const JsonValue& m : messages.Elements())
        list.PushBack(m);
    body.Set("messages", move(list));
    if (!tools.Empty()) {
        JsonValue arr = JsonValue::MakeArray();
        for (const ToolDef& t : tools) {
            JsonValue fn = JsonValue::MakeObject();
            fn.Set("name", t.name);
            fn.Set("description", t.description);
            fn.Set("parameters", SchemaOf(t));
            JsonValue o = JsonValue::MakeObject();
            o.Set("type", "function");
            o.Set("function", move(fn));
            arr.PushBack(move(o));
        }
        body.Set("tools", move(arr));
    }
    body.Set(config.provider == Provider::OpenAI ? "max_completion_tokens" : "max_tokens", config.maxTokens);
    return body;
}

JsonValue AnthropicHistoryContent(const JsonValue& content)
{
    usize lastFallback = usize(-1);
    for (usize i = 0; i < content.Size(); ++i)
        if (content[i].Get("type").AsString() == "fallback")
            lastFallback = i;
    if (lastFallback == usize(-1))
        return content;
    JsonValue out = JsonValue::MakeArray();
    for (usize i = 0; i < content.Size(); ++i) {
        const StringView type = content[i].Get("type").AsString();
        if (i < lastFallback && (type == "thinking" || type == "redacted_thinking" || type == "tool_use"))
            continue;
        out.PushBack(content[i]);
    }
    return out;
}

String SummarizeArguments(const JsonValue& arguments, usize maxLength)
{
    String out;
    for (const JsonMember& m : arguments.Members()) {
        if (!out.Empty())
            out += ", ";
        out += m.first;
        out += ": ";
        if (m.second.IsString()) {
            StringView v = m.second.AsString();
            const usize nl = v.Find('\n');
            const bool cut = nl != StringView::NPos || v.Size() > 40;
            v = v.Left(Utf8TruncateBytes(v.Left(nl), 40));
            out += v;
            if (cut)
                out += "\xE2\x80\xA6";
        } else {
            String json = ToJsonString(m.second, JsonWriteOptions{.pretty = false});
            if (json.Size() > 40) {
                json.Resize(Utf8TruncateBytes(json, 40));
                json += "\xE2\x80\xA6";
            }
            out += json;
        }
        if (out.Size() > maxLength)
            break;
    }
    if (out.Size() > maxLength) {
        out.Resize(Utf8TruncateBytes(out, maxLength));
        out += "\xE2\x80\xA6";
    }
    return out;
}

String ApiErrorMessage(u32 status, StringView body)
{
    JsonValue doc;
    String message;
    if (ParseJson(body, doc)) {
        const JsonValue& err = doc.Get("error");
        message = err.IsString() ? String(err.AsString()) : String(err.Get("message").AsString());
    }
    if (message.Empty())
        message = body.Left(Utf8TruncateBytes(body, 300));
    switch (status) {
    case 401: return String::Format("The API key was rejected (401): {}. Check it in Settings > AI.", message);
    case 403: return String::Format("Access denied (403): {}", message);
    case 404: return String::Format("Not found (404): {}. Check the model name and the server URL in Settings > AI.", message);
    case 429: return String::Format("Rate limited (429): {}", message);
    case 529: return String::Format("The service is overloaded (529): {}", message);
    default: return String::Format("HTTP {}: {}", status, message);
    }
}

// --- Agent ----------------------------------------------------------------------------------------------------------

Agent::Agent(ToolHost& host) : m_host(host) {}

Agent::~Agent()
{
    Cancel();
    Wait();
}

bool Agent::Send(const AgentConfig& config, StringView systemPrompt, StringView userText)
{
    if (m_busy.load() || (config.apiKey.Empty() && config.provider != Provider::OpenAICompatible))
        return false;
    if (m_thread.IsJoinable())
        m_thread.Join();
    if (config.provider != m_historyProvider) {
        // The two wire formats do not mix: a new provider starts a new conversation.
        m_messages = JsonValue::MakeArray();
        m_historyProvider = config.provider;
    }
    // Consecutive user messages (a cancelled or cut-off turn) are merged.
    JsonValue* last = m_messages.Size() ? &m_messages[m_messages.Size() - 1] : nullptr;
    if (config.provider == Provider::Anthropic) {
        JsonValue text = JsonValue::MakeObject();
        text.Set("type", "text");
        text.Set("text", userText);
        if (last && last->Get("role").AsString() == "user" && last->Get("content").IsArray()) {
            (*last)["content"].PushBack(move(text));
        } else {
            JsonValue m = JsonValue::MakeObject();
            m.Set("role", "user");
            m["content"].PushBack(move(text));
            m_messages.PushBack(move(m));
        }
    } else {
        JsonValue m = JsonValue::MakeObject();
        m.Set("role", "user");
        m.Set("content", userText);
        m_messages.PushBack(move(m));
    }
    m_cancel = false;
    m_busy = true;
    if (!m_thread.Start("AIAgent", [this, config, system = String(systemPrompt)] { Run(config, system); })) {
        m_busy = false;
        return false;
    }
    return true;
}

void Agent::Cancel() { m_cancel = true; }

void Agent::Wait()
{
    if (m_thread.IsJoinable())
        m_thread.Join();
}

void Agent::Reset()
{
    if (m_busy.load())
        return;
    Wait();
    m_messages = JsonValue::MakeArray();
    ScopedLock lock(m_mutex);
    m_events.Clear();
}

Array<AgentEvent> Agent::TakeEvents()
{
    ScopedLock lock(m_mutex);
    Array<AgentEvent> out = move(m_events);
    m_events.Clear();
    return out;
}

AgentUsage Agent::Usage() const
{
    ScopedLock lock(m_mutex);
    return m_usage;
}

void Agent::Emit(AgentEvent event)
{
    ScopedLock lock(m_mutex);
    m_events.PushBack(move(event));
}

platform::HttpResponse Agent::Post(const platform::HttpRequestDesc& request)
{
    // Rate limits, overload and server errors are retried with backoff.
    platform::HttpResponse res;
    for (u32 attempt = 0;; ++attempt) {
        res = m_transport ? m_transport(request, &m_cancel) : platform::HttpRequest(request, &m_cancel);
        const bool retry = res.status == 429 || res.status == 529 || res.status == 500 || res.status == 502 || res.status == 503 ||
                           res.status == 504 || (res.status == 0 && res.error != "cancelled" && attempt == 0);
        if (!retry || attempt >= 3 || m_cancel.load())
            return res;
        for (u32 waited = 0; waited < (2000u << attempt) && !m_cancel.load(); waited += 50)
            Thread::SleepMs(m_transport ? 0 : 50);
    }
}

void Agent::Run(AgentConfig config, String system)
{
    const bool ok = config.provider == Provider::Anthropic ? RunAnthropic(config, system) : RunOpenAI(config, system);
    AgentEvent done;
    done.type = AgentEventType::Done;
    done.isError = !ok;
    Emit(move(done));
    m_busy = false;
}

namespace {

AgentEvent ErrorEvent(StringView text)
{
    AgentEvent e;
    e.type = AgentEventType::Error;
    e.text = text;
    e.isError = true;
    return e;
}

} // namespace

bool Agent::RunAnthropic(const AgentConfig& configIn, StringView system)
{
    AgentConfig config = configIn;
    const Span<const ToolDef> tools = m_host.Tools();
    for (u32 round = 0; round <= config.maxToolRounds; ++round) {
        if (m_cancel.load()) {
            Emit(ErrorEvent("Stopped."));
            return false;
        }
        platform::HttpRequestDesc req;
        req.url = BaseUrlOf(config) + "/v1/messages";
        req.headers.PushBack({"content-type", "application/json"});
        req.headers.PushBack({"x-api-key", config.apiKey});
        req.headers.PushBack({"anthropic-version", String(AnthropicVersion)});
        if (config.serverFallbacks)
            req.headers.PushBack({"anthropic-beta", String(FallbackBeta)});
        req.body = ToJsonString(BuildAnthropicRequest(config, system, tools, m_messages), JsonWriteOptions{.pretty = false});
        const platform::HttpResponse res = Post(req);
        if (res.status == 0) {
            Emit(ErrorEvent(res.error == "cancelled" ? String("Stopped.") : String::Format("Network error: {}", res.error)));
            return false;
        }
        if (res.status == 400) {
            // Optional features the chosen model / endpoint may not accept: drop them and try again.
            const String message = ApiErrorMessage(res.status, res.body);
            if (config.serverFallbacks && message.Contains("fallback")) {
                config.serverFallbacks = false;
                --round;
                continue;
            }
            if (!config.effort.Empty() && (message.Contains("effort") || message.Contains("output_config"))) {
                config.effort.Clear();
                --round;
                continue;
            }
        }
        if (res.status != 200) {
            Emit(ErrorEvent(ApiErrorMessage(res.status, res.body)));
            return false;
        }
        JsonValue doc;
        if (!ParseJson(res.body, doc) || !doc.Get("content").IsArray()) {
            Emit(ErrorEvent("The response could not be read."));
            return false;
        }
        {
            ScopedLock lock(m_mutex);
            const JsonValue& u = doc.Get("usage");
            m_usage.inputTokens += u.Get("input_tokens").AsU64() + u.Get("cache_creation_input_tokens").AsU64();
            m_usage.cacheReadTokens += u.Get("cache_read_input_tokens").AsU64();
            m_usage.outputTokens += u.Get("output_tokens").AsU64();
            ++m_usage.requests;
        }
        const StringView stop = doc.Get("stop_reason").AsString();
        JsonValue content = AnthropicHistoryContent(doc.Get("content"));
        for (const JsonValue& block : content.Elements())
            if (block.Get("type").AsString() == "text" && !block.Get("text").AsString().Trim().Empty()) {
                AgentEvent e;
                e.text = block.Get("text").AsString();
                Emit(move(e));
            }

        if (stop == "max_tokens" || stop == "refusal") {
            // Never run the tools of a cut-off or declined turn; keep only what is safe to send back.
            JsonValue kept = JsonValue::MakeArray();
            for (const JsonValue& block : content.Elements())
                if (block.Get("type").AsString() == "text")
                    kept.PushBack(block);
            if (kept.Size()) {
                JsonValue m = JsonValue::MakeObject();
                m.Set("role", "assistant");
                m.Set("content", move(kept));
                m_messages.PushBack(move(m));
            }
            Emit(ErrorEvent(stop == "refusal" ? StringView("The model declined this request.")
                                              : StringView("The answer was cut off (output limit). Ask to continue, or raise Max Tokens in Settings > AI.")));
            return false;
        }

        JsonValue assistant = JsonValue::MakeObject();
        assistant.Set("role", "assistant");
        assistant.Set("content", content);
        m_messages.PushBack(move(assistant));

        if (stop == "pause_turn")
            continue; // the server paused a long turn: send the conversation back as-is to resume
        if (stop != "tool_use")
            return true;

        JsonValue results = JsonValue::MakeArray();
        for (const JsonValue& block : content.Elements()) {
            if (block.Get("type").AsString() != "tool_use")
                continue;
            const StringView name = block.Get("name").AsString();
            const JsonValue& input = block.Get("input");
            ToolResult tr;
            if (m_cancel.load()) {
                tr = ToolResult::Error("Cancelled by the user.");
            } else {
                AgentEvent start;
                start.type = AgentEventType::ToolStart;
                start.tool = name;
                start.text = SummarizeArguments(input);
                Emit(move(start));
                tr = m_host.Call(name, input.IsObject() ? input : JsonValue(JsonValue::MakeObject()));
                AgentEvent end;
                end.type = AgentEventType::ToolEnd;
                end.tool = name;
                end.isError = tr.isError;
                end.text = tr.text.View().Left(Utf8TruncateBytes(tr.text, 400));
                Emit(move(end));
                if (!tr.pngBase64.Empty()) {
                    AgentEvent img;
                    img.type = AgentEventType::Image;
                    img.tool = name;
                    img.pngBase64 = tr.pngBase64;
                    Emit(move(img));
                }
            }
            JsonValue r = JsonValue::MakeObject();
            r.Set("type", "tool_result");
            r.Set("tool_use_id", block.Get("id"));
            JsonValue parts = JsonValue::MakeArray();
            JsonValue text = JsonValue::MakeObject();
            text.Set("type", "text");
            text.Set("text", tr.text.Empty() ? String("(no output)") : tr.text);
            parts.PushBack(move(text));
            if (!tr.pngBase64.Empty()) {
                JsonValue img = JsonValue::MakeObject();
                img.Set("type", "image");
                JsonValue& src = img["source"];
                src.Set("type", "base64");
                src.Set("media_type", "image/png");
                src.Set("data", tr.pngBase64);
                parts.PushBack(move(img));
            }
            r.Set("content", move(parts));
            if (tr.isError)
                r.Set("is_error", true);
            results.PushBack(move(r));
        }
        JsonValue user = JsonValue::MakeObject();
        user.Set("role", "user");
        user.Set("content", move(results));
        m_messages.PushBack(move(user));
    }
    Emit(ErrorEvent(String::Format("Stopped after {} tool rounds. Send a message to continue.", config.maxToolRounds)));
    return false;
}

bool Agent::RunOpenAI(const AgentConfig& config, StringView system)
{
    const Span<const ToolDef> tools = m_host.Tools();
    for (u32 round = 0; round <= config.maxToolRounds; ++round) {
        if (m_cancel.load()) {
            Emit(ErrorEvent("Stopped."));
            return false;
        }
        platform::HttpRequestDesc req;
        req.url = BaseUrlOf(config) + "/chat/completions";
        req.headers.PushBack({"Content-Type", "application/json"});
        if (!config.apiKey.Empty())
            req.headers.PushBack({"Authorization", String::Format("Bearer {}", config.apiKey)});
        req.body = ToJsonString(BuildOpenAIRequest(config, system, tools, m_messages), JsonWriteOptions{.pretty = false});
        const platform::HttpResponse res = Post(req);
        if (res.status == 0) {
            Emit(ErrorEvent(res.error == "cancelled" ? String("Stopped.") : String::Format("Network error: {}", res.error)));
            return false;
        }
        if (res.status != 200) {
            Emit(ErrorEvent(ApiErrorMessage(res.status, res.body)));
            return false;
        }
        JsonValue doc;
        if (!ParseJson(res.body, doc) || !doc.Get("choices").IsArray() || doc.Get("choices").Size() == 0) {
            Emit(ErrorEvent("The response could not be read."));
            return false;
        }
        {
            ScopedLock lock(m_mutex);
            const JsonValue& u = doc.Get("usage");
            m_usage.inputTokens += u.Get("prompt_tokens").AsU64();
            m_usage.outputTokens += u.Get("completion_tokens").AsU64();
            ++m_usage.requests;
        }
        const JsonValue& choice = doc.Get("choices")[0];
        const JsonValue& msg = choice.Get("message");
        const StringView finish = choice.Get("finish_reason").AsString();
        if (!msg.Get("content").AsString().Trim().Empty()) {
            AgentEvent e;
            e.text = msg.Get("content").AsString();
            Emit(move(e));
        }
        const JsonValue& calls = msg.Get("tool_calls");
        JsonValue assistant = JsonValue::MakeObject();
        assistant.Set("role", "assistant");
        assistant.Set("content", msg.Get("content").IsString() ? msg.Get("content") : JsonValue());
        if (calls.IsArray() && calls.Size() && finish != "length")
            assistant.Set("tool_calls", calls);
        m_messages.PushBack(move(assistant));
        if (finish == "length") {
            Emit(ErrorEvent("The answer was cut off (output limit). Ask to continue, or raise Max Tokens in Settings > AI."));
            return false;
        }
        if (!calls.IsArray() || calls.Size() == 0)
            return true;

        Array<JsonValue> images;
        for (const JsonValue& call : calls.Elements()) {
            const StringView name = call.Get("function").Get("name").AsString();
            JsonValue args;
            const StringView argText = call.Get("function").Get("arguments").AsString();
            ToolResult tr;
            if (!argText.Trim().Empty() && (!ParseJson(argText, args) || !args.IsObject())) {
                tr = ToolResult::Error("The arguments are not a JSON object.");
            } else if (m_cancel.load()) {
                tr = ToolResult::Error("Cancelled by the user.");
            } else {
                if (!args.IsObject())
                    args = JsonValue::MakeObject();
                AgentEvent start;
                start.type = AgentEventType::ToolStart;
                start.tool = name;
                start.text = SummarizeArguments(args);
                Emit(move(start));
                tr = m_host.Call(name, args);
                AgentEvent end;
                end.type = AgentEventType::ToolEnd;
                end.tool = name;
                end.isError = tr.isError;
                end.text = tr.text.View().Left(Utf8TruncateBytes(tr.text, 400));
                Emit(move(end));
            }
            JsonValue m = JsonValue::MakeObject();
            m.Set("role", "tool");
            m.Set("tool_call_id", call.Get("id"));
            m.Set("content", tr.isError ? String::Format("Error: {}", tr.text) : (tr.text.Empty() ? String("(no output)") : tr.text));
            m_messages.PushBack(move(m));
            if (!tr.pngBase64.Empty()) {
                AgentEvent img;
                img.type = AgentEventType::Image;
                img.tool = name;
                img.pngBase64 = tr.pngBase64;
                Emit(move(img));
                // Tool messages carry text only: the image follows as a user message.
                JsonValue parts = JsonValue::MakeArray();
                JsonValue text = JsonValue::MakeObject();
                text.Set("type", "text");
                text.Set("text", String::Format("Image returned by {}:", name));
                parts.PushBack(move(text));
                JsonValue image = JsonValue::MakeObject();
                image.Set("type", "image_url");
                image["image_url"].Set("url", String::Format("data:image/png;base64,{}", tr.pngBase64));
                parts.PushBack(move(image));
                JsonValue user = JsonValue::MakeObject();
                user.Set("role", "user");
                user.Set("content", move(parts));
                images.PushBack(move(user));
            }
        }
        for (JsonValue& m : images)
            m_messages.PushBack(move(m));
    }
    Emit(ErrorEvent(String::Format("Stopped after {} tool rounds. Send a message to continue.", config.maxToolRounds)));
    return false;
}

} // namespace zx::ai
