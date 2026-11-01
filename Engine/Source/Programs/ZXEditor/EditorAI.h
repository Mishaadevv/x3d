#pragma once

// Zeqou AI: the assistant orb in the toolbar, its chat panel, Settings > AI, and the editor tools an AI can use -
// both the built-in agent (the user's own API key) and any external agent connected over MCP.
//
//   EditorAITools.cpp  the tools (scene, files, console, screenshots) run on the main thread
//   EditorAIPanel.cpp  the orb, the panel, the settings page
//
// Settings live per user in %LOCALAPPDATA%/ZeqouX3D/AI.json; API keys are encrypted for the Windows user (DPAPI) and
// never written into a project.

#include "Engine/AIAssistant.h"
#include "Core/Math/Geometry.h"

namespace zx::editor {

class Editor;

struct AISettings
{
    ai::Provider provider = ai::Provider::Anthropic;
    String model;                // empty = the provider's default
    String baseUrl;              // OpenAI-compatible server
    String effort = "high";      // Claude: low | medium | high | xhigh | max
    i32 maxTokens = 16000;
    bool serverFallbacks = true; // Claude: a declined request is re-run on Anthropic's recommended fallback model
    String apiKeys[3];           // per provider, decrypted (memory only)
    bool mcpEnabled = false;
    i32 mcpPort = 47800;
    String mcpToken;             // random, generated on first use
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
    };
    Kind kind = Kind::Assistant;
    String text;   // message / tool argument summary / error
    String tool;   // Tool: name
    String result; // Tool: first line of the result
    bool running = false;
    bool isError = false;
    f32 age = 0.0f; // seconds since it appeared (entrance animation)
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
    f32 openAnim = 0.0f, openVelocity = 0.0f; // spring 0..1
    f32 orbHover = 0.0f, orbPress = 0.0f;
    f64 time = 0.0;
    Array<AIChatItem> chat;
    String input;
    bool focusInput = false;
    bool scrollToBottom = false;
    f32 scroll = 0.0f, scrollTarget = 0.0f, contentHeight = 0.0f;
    i32 page = 0; // 0 chat, 1 connect (MCP instructions)
    Rect orbRect;  // last frame (physical px)
};

void InitAI(Editor& ed);
void ShutdownAI(Editor& ed);
/// Every frame before the panels draw: runs queued tool calls, starts / stops the MCP server, reads agent events.
void UpdateAI(Editor& ed);
/// The orb button in the toolbar (right side).
void DrawAIOrb(Editor& ed, const Rect& r);
/// The assistant panel (over everything, under modal dialogs).
void DrawAIPanel(Editor& ed);
/// Settings > AI page.
void DrawAISettingsPage(Editor& ed);
/// Sends a chat message to the built-in assistant (as if typed in the panel).
void SendAIMessage(Editor& ed, StringView text);
/// The text an MCP client gets as its instructions (also shown in Settings > AI).
String McpInstructions();

} // namespace zx::editor
