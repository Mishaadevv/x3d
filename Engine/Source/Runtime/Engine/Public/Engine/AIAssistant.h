#pragma once

// AI assistance for the editor, independent of the editor itself:
//   - ToolHost: the engine operations an AI may call (the editor implements it).
//   - McpServer: exposes a ToolHost over the Model Context Protocol (Streamable HTTP, JSON responses) on 127.0.0.1,
//     so any MCP client (Claude Code, Cursor, VS Code, Claude Desktop, ...) can drive the editor.
//   - Agent: the built-in assistant. Talks to a model provider with the user's own API key (Anthropic Messages API,
//     OpenAI or an OpenAI-compatible Chat Completions endpoint) and runs the tool loop on a worker thread.
//
//   ai::McpServer mcp;
//   mcp.Start(47800, {.token = token, .instructions = text}, host);   // POST http://127.0.0.1:47800/mcp
//
//   ai::Agent agent(host);
//   agent.Send(config, systemPrompt, "Build a small arena with three enemies");
//   for (const ai::AgentEvent& e : agent.TakeEvents()) ...                 // every frame, on the UI thread

#include "Core/Containers/Array.h"
#include "Core/Containers/Function.h"
#include "Core/Serialization/Json.h"
#include "Core/Threading/Mutex.h"
#include "Core/Threading/Thread.h"
#include "Platform/Net.h"

namespace zx::ai {

struct ToolDef
{
    String name;        // snake_case
    String title;       // human-readable
    String description; // what it does, when to use it (the model reads this)
    JsonValue inputSchema; // JSON Schema of the arguments (type: object)
    bool readOnly = false;
};

struct ToolResult
{
    String text;
    String pngBase64; // optional image (screenshots)
    bool isError = false;

    static ToolResult Ok(StringView text) { return ToolResult{String(text), {}, false}; }
    static ToolResult Error(StringView text) { return ToolResult{String(text), {}, true}; }
};

/// Implemented by the application. Call() may be invoked from any thread (MCP connections, the agent's worker).
class ToolHost
{
public:
    virtual ~ToolHost() = default;
    virtual Span<const ToolDef> Tools() const = 0;
    virtual ToolResult Call(StringView name, const JsonValue& arguments) = 0;
};

// --- MCP server ---------------------------------------------------------------------------------------------------

/// Newest protocol revision implemented; older client revisions in McpSupportedVersions are answered with theirs.
inline constexpr StringView McpProtocolVersion = "2025-11-25";

struct McpOptions
{
    String token;        // required "Authorization: Bearer <token>" (empty = no auth; tests only)
    String instructions; // sent in the initialize result (how to use the tools)
    String serverName = "zeqoux3d";
    String serverTitle = "ZeqouX3D Editor";
    String serverVersion;
};

class McpServer
{
public:
    McpServer() = default;
    ~McpServer() { Stop(); }
    ZX_NON_COPYABLE(McpServer)

    bool Start(u16 port, McpOptions options, ToolHost& host, String* error = nullptr);
    void Stop();
    bool IsRunning() const { return m_http.IsRunning(); }
    u16 Port() const { return m_http.Port(); }
    u64 RequestCount() const { return m_http.RequestCount(); }
    /// Name of the last client that initialized ("claude-code 2.1", ...); empty if none yet.
    String LastClient() const;
    u64 ToolCallCount() const { return m_toolCalls.load(); }

    /// Without the network (tests): configures the server for Handle().
    void Configure(McpOptions options, ToolHost& host);
    /// One HTTP request to the endpoint (auth, origin, method checks, JSON-RPC dispatch).
    platform::HttpServerResponse Handle(const platform::HttpServerRequest& request);
    /// One JSON-RPC message; returns the response JSON or empty for notifications / responses.
    String HandleMessage(const JsonValue& message);

private:
    platform::LocalHttpServer m_http;
    McpOptions m_options;
    ToolHost* m_host = nullptr;
    mutable Mutex m_mutex;
    String m_lastClient;
    std::atomic<u64> m_toolCalls{0};
};

// --- Built-in agent -------------------------------------------------------------------------------------------------

enum class Provider : u8
{
    Anthropic,        // Claude: Messages API
    OpenAI,           // api.openai.com Chat Completions
    OpenAICompatible, // any Chat Completions endpoint (OpenRouter, LM Studio, Ollama, ...): baseUrl required
};

struct AgentConfig
{
    Provider provider = Provider::Anthropic;
    String apiKey;
    String model;            // empty = the provider's default
    String baseUrl;          // empty = the provider's default
    String effort = "high";  // Anthropic: low | medium | high | xhigh | max
    u32 maxTokens = 16000;
    bool serverFallbacks = true; // Anthropic: re-run a declined request on Anthropic's recommended fallback model
    u32 maxToolRounds = 60;      // tool calls per message before the agent stops
    u32 contextWindow = 0;       // tokens the model accepts; older turns are dropped to fit (0 = never trimmed)
};

StringView DefaultModel(Provider provider);
StringView DefaultBaseUrl(Provider provider);

// --- Models and context windows -----------------------------------------------------------------------------------

/// The largest context window the editor works with (tokens).
inline constexpr u32 MaxContextWindow = 1000000;

struct ModelInfo
{
    String id;
    String name;            // display name if the provider gives one
    u32 contextWindow = 0;  // tokens reported by the provider (0 = not reported), at most MaxContextWindow
};

/// GET request for a provider's model list: Anthropic <base>/v1/models, everything else <base>/models (OpenAI format).
platform::HttpRequestDesc BuildModelListRequest(Provider provider, StringView baseUrl, StringView apiKey);
/// Parses a model list in the formats providers use (OpenAI / Anthropic / OpenRouter / Groq / Mistral / Together /
/// Gemini / llama.cpp / vLLM / Ollama tags). Keeps chat models only, sorted by id.
bool ParseModelList(StringView body, Array<ModelInfo>& out);
/// Context window of a well-known model family from its id (0 = unknown), at most MaxContextWindow.
u32 KnownContextWindow(StringView model);
/// Rough token count of JSON content (about 4 characters per token, an image about 1.6k tokens).
u64 EstimateTokens(const JsonValue& value);
/// Drops the oldest turns (a turn starts at a user text message) until the estimate fits `budget` tokens; the latest
/// turn is always kept. Returns the number of messages dropped.
usize TrimHistory(JsonValue& messages, Provider provider, u64 budget);

enum class AgentEventType : u8
{
    Text,      // assistant text (Markdown)
    ToolStart, // a tool call begins (name, detail = a short summary of the arguments)
    ToolEnd,   // the call finished (isError)
    Image,     // a screenshot the model looked at (pngBase64)
    Error,     // request failed; the turn ended
    Done,      // the turn ended normally
    Notice,    // information for the user (older messages were dropped to fit the context window)
};

struct AgentEvent
{
    AgentEventType type = AgentEventType::Text;
    String text;
    String tool;
    String pngBase64;
    bool isError = false;
};

struct AgentUsage
{
    u64 inputTokens = 0, outputTokens = 0, cacheReadTokens = 0;
    u64 contextTokens = 0; // prompt size of the last request: how full the context window is
    u32 requests = 0;
};

class Agent
{
public:
    using Transport = Function<platform::HttpResponse(const platform::HttpRequestDesc&, const std::atomic<bool>*)>;

    explicit Agent(ToolHost& host);
    ~Agent();
    ZX_NON_COPYABLE(Agent)

    /// Replaces the network (tests).
    void SetTransport(Transport transport) { m_transport = move(transport); }

    /// Starts a turn on a worker thread. False if a turn is running or the config has no key.
    bool Send(const AgentConfig& config, StringView systemPrompt, StringView userText);
    void Cancel();
    bool IsBusy() const { return m_busy.load(); }
    /// Blocks until the running turn ends (tests, shutdown).
    void Wait();
    /// Forgets the conversation (not while busy).
    void Reset();
    /// Events since the last call, in order.
    Array<AgentEvent> TakeEvents();
    AgentUsage Usage() const;

private:
    void Run(AgentConfig config, String system);
    bool RunAnthropic(const AgentConfig& config, StringView system);
    bool RunOpenAI(const AgentConfig& config, StringView system);
    platform::HttpResponse Post(const platform::HttpRequestDesc& request);
    void Emit(AgentEvent event);
    /// Drops the oldest turns so the next request fits config.contextWindow (with room for the reply).
    void FitContext(const AgentConfig& config, StringView system, Span<const ToolDef> tools);

    ToolHost& m_host;
    Transport m_transport;
    Thread m_thread;
    std::atomic<bool> m_busy{false};
    std::atomic<bool> m_cancel{false};
    mutable Mutex m_mutex; // events, usage
    Array<AgentEvent> m_events;
    AgentUsage m_usage;
    Provider m_historyProvider = Provider::Anthropic;
    JsonValue m_messages = JsonValue::MakeArray(); // the provider's own message format
};

// Wire format helpers (exposed for tests).
JsonValue BuildAnthropicRequest(const AgentConfig& config, StringView system, Span<const ToolDef> tools, const JsonValue& messages);
JsonValue BuildOpenAIRequest(const AgentConfig& config, StringView system, Span<const ToolDef> tools, const JsonValue& messages);
/// The assistant content to keep in the history: after a server-side fallback, blocks of the declined attempt that
/// precede the last "fallback" block (thinking, tool_use) are dropped.
JsonValue AnthropicHistoryContent(const JsonValue& content);
/// A short one-line summary of tool arguments for the UI ("name: Cube, position: [0, 1, 0]").
String SummarizeArguments(const JsonValue& arguments, usize maxLength = 90);
/// The error message of an API error response body (or the body itself).
String ApiErrorMessage(u32 status, StringView body);

} // namespace zx::ai
