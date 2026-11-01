#pragma once

// Zeqou AI: the assistant button in the toolbar, its chat panel, Settings > AI, and the editor tools an AI can use -
// both the built-in agent (the user's own API key) and any external agent connected over MCP.
//
//   EditorAITools.cpp  the tools (scene, files, console, screenshots) run on the main thread
//   EditorAIPanel.cpp  the toolbar button, the panel, the settings page, providers and models
//
// Settings live per user in %LOCALAPPDATA%/ZeqouX3D/AI.json; API keys are encrypted for the Windows user (DPAPI) and
// never written into a project.

#include "Engine/AIAssistant.h"
#include "Core/Containers/HashMap.h"
#include "Core/Math/Geometry.h"
#include "UI/Font.h"

namespace zx::editor {

class Editor;

/// A model provider the assistant can talk to (Anthropic natively, the rest through the OpenAI Chat Completions format).
struct AIProviderInfo
{
    const char* id;           // stable key in AI.json
    const char* name;
    ai::Provider wire;
    const char* baseUrl;
    const char* defaultModel; // empty: pick one from the list
    const char* keyUrl;       // where to get a key (empty: none needed)
    const char* keyHint;      // placeholder of the key field
    bool needsKey;
    bool local;               // runs on this computer (Ollama, LM Studio, ...): the URL is editable
    const char* modelsUrl;    // model list URL if not <baseUrl>/models (empty: the default)
};
Span<const AIProviderInfo> AIProviders();
inline constexpr u32 MaxAIProviders = 32;
/// Index of a provider id (U32Max if unknown).
u32 FindAIProvider(StringView id);

struct AISettings
{
    u32 provider = 0;                // index into AIProviders()
    String apiKeys[MaxAIProviders];  // decrypted (memory only)
    String models[MaxAIProviders];   // chosen model per provider (empty = its default)
    String baseUrls[MaxAIProviders]; // URL overrides (local servers, Custom)
    String effort = "high";          // Claude: low | medium | high | xhigh | max
    i32 maxTokens = 16000;
    HashMap<String, u32> contextOverrides; // "provider/model" -> context window set by hand (tokens, 1k .. 1M)
    bool serverFallbacks = true;     // Claude: a declined request is re-run on Anthropic's recommended fallback model
    f32 panelWidth = 440.0f;         // logical px, resizable by dragging the panel's left edge
    bool mcpEnabled = false;
    i32 mcpPort = 47800;
    String mcpToken;                 // random, generated on first use
};

void LoadAISettings(AISettings& s);
void SaveAISettings(const AISettings& s);

struct AIChatItem
{
    enum class Kind : u8
    {
        User,
        Assistant,
        Tool,
        Error,
        Notice,
    };
    Kind kind = Kind::Assistant;
    String text;   // message (Markdown for the assistant) / tool argument summary / error
    String tool;   // Tool: name
    String result; // Tool: the result (first lines)
    bool running = false;
    bool isError = false;
    bool expanded = false; // Tool: details shown
    f32 age = 0.0f;        // seconds since it appeared (entrance fade)
    f64 started = 0.0;     // Tool: when it started (duration)
    f32 duration = 0.0f;
    f32 layoutWidth = 0.0f, layoutHeight = 0.0f; // last layout (off-screen items are skipped)
};

/// Main-thread tool execution for worker threads (MCP connections, the agent).
class EditorToolHost final : public ai::ToolHost
{
public:
    explicit EditorToolHost(Editor& ed);
    ~EditorToolHost() override;
    Span<const ai::ToolDef> Tools() const override { return m_tools.View(); }
    ai::ToolResult Call(StringView name, const JsonValue& arguments) override;

    /// Runs queued calls (main thread, every frame).
    void Pump();
    /// Fails every waiting call and refuses new ones (before shutdown).
    void Shutdown();

    struct Job;

private:
    void Execute(Job& job);

    Editor& m_ed;
    Array<ai::ToolDef> m_tools;
    Mutex m_mutex;
    Array<Job*> m_queue;
    std::atomic<bool> m_closed{false};
};

/// The model list of one provider, fetched from its API on a worker thread.
struct AIModelList
{
    Array<ai::ModelInfo> models;
    String error;
    String fetchedFor; // provider id + url + key hash the list belongs to
    bool fetched = false;
    f64 attemptedAt = -100.0; // AIState::time of the last request (failed lists retry after a while)
};

struct AIState
{
    explicit AIState(Editor& ed);
    ~AIState();

    AISettings settings;
    EditorToolHost host;
    ai::Agent agent;
    ai::McpServer mcp;
    String mcpError;
    i32 startedPort = 0;

    // Panel.
    bool open = false;
    f32 openAnim = 0.0f; // 0..1 (critically damped spring: eases in, no overshoot)
    f32 openVel = 0.0f;  // spring velocity of openAnim
    f32 focusAnim = 0.0f; // 0..1: the composer border eases towards the accent while the field has the keyboard
    f32 ringAnim = 0.0f;  // the context ring eases towards its real fill
    f32 frameDt = 0.0f;   // seconds of the last frame (clamped), for the eased visuals
    f32 buttonHover = 0.0f;
    f64 time = 0.0;
    Array<AIChatItem> chat;
    String input;
    bool focusInput = false;
    bool inputActive = false;  // last frame: the composer has the keyboard
    bool popupOpen = false;    // last frame: the model picker is open
    f64 turnStarted = 0.0;     // time of the last message sent
    f32 resizeStartX = 0.0f, resizeStartW = 0.0f;
    bool stickToBottom = true; // the chat follows new messages until the user scrolls up
    f32 lastSetScroll = 0.0f;
    ui::FontHandle mono;       // code (Consolas, else Inter)
    bool monoLoaded = false;
    i32 page = 0;              // 0 chat, 1 connect (MCP instructions)
    String settingsFilter;     // Settings > AI model list filter
    Rect buttonRect;           // the toolbar button, last frame (physical px)
    String modelSearch;
    bool focusModelSearch = false;
    u32 pickerProvider = 0;    // provider shown in the model picker
    String copiedId;           // "Copy" feedback
    f64 copiedAt = -10.0;
    u64 lastContextTokens = 0; // size of the conversation for the context ring

    // Model lists per provider (worker thread fills `pending`, the main thread moves it in).
    AIModelList lists[MaxAIProviders];
    Mutex fetchMutex;
    Thread fetchThread;
    std::atomic<bool> fetching{false};
    std::atomic<bool> fetchCancel{false};
    u32 fetchProvider = 0;
    AIModelList pending;
    bool pendingReady = false;

    bool pickerRequest = false; // console ai.picker: open the model picker on the next frame (for autotests)
};

void InitAI(Editor& ed);
void ShutdownAI(Editor& ed);
/// Every frame before the panels draw: runs queued tool calls, starts / stops the MCP server, reads agent events.
void UpdateAI(Editor& ed);
/// The assistant button in the toolbar (right side).
void DrawAIOrb(Editor& ed, const Rect& r);
/// The assistant panel (over everything, under modal dialogs).
void DrawAIPanel(Editor& ed);
/// Settings > AI page.
void DrawAISettingsPage(Editor& ed);
/// Sends a chat message to the built-in assistant (as if typed in the panel).
void SendAIMessage(Editor& ed, StringView text);
/// The model in use (the choice, else the provider's default, else the first model of its list).
String AIModel(const AIState& ai);
/// The context window of the model in use: a manual value, else what the provider reports for it, else the model
/// family's known size, else 128k. At most 1M.
u32 AIContextWindow(const AIState& ai);
/// Where the context window comes from: "manual", "model", "provider", "known" or "default".
StringView AIContextSource(const AIState& ai);
/// Fetches a provider's model list in the background (force: even if already fetched; one request at a time).
void RefreshAIModels(AIState& ai, u32 provider, bool force = false);
/// Switches provider / model (saves the settings; a different wire format starts a new conversation).
void SelectAIModel(AIState& ai, u32 provider, StringView model);
/// The text an MCP client gets as its instructions (also shown in Settings > AI).
String McpInstructions();

} // namespace zx::editor
