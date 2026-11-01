# ZeqouX3D — changelog (English)

The full history, including older releases, is in [CHANGELOG.md](CHANGELOG.md) (Russian). This file covers the
latest releases in English. Version scheme up to 1.0: **0.<last closed milestone>.<release within it>**.

## 0.17.9 — Zeqou AI: 22 providers, model picker in chat, context window, new panel (2026-11-01)
- **Providers**: 22 instead of three — Anthropic, OpenAI, Google Gemini, xAI Grok, DeepSeek, Mistral, OpenRouter, Groq,
  Cerebras, Together, Fireworks, Perplexity, Moonshot (Kimi), Z.ai (GLM), Alibaba Qwen, NVIDIA NIM, GitHub Models,
  Hugging Face, Ollama, LM Studio, llama.cpp / vLLM and Custom (any OpenAI-compatible server). Each has its own key,
  model and URL; a check mark in the list means the provider is set up. Settings from 0.17.8 (`AI.json`) are read.
- **Model picker in the chat**: the model name under the message field opens a list — providers on the left, models on
  the right, with search, the context size of each model, a refresh button, and any model id typed by hand. The list
  comes from the provider's API (`/models`: OpenAI, Anthropic, OpenRouter, Groq, Mistral, Gemini, Ollama, GitHub Models
  and others). Embedding, speech and image models are filtered out.
- **Context window**: detected automatically — from the provider's answer (`context_length`, `context_window`,
  `max_context_length`, `inputTokenLimit` and similar), otherwise from the model family (a table), otherwise 128k. In
  Settings > AI you can set it by hand for a model (slider and presets from 8k to 1M; the maximum is **1 million
  tokens**). When a conversation does not fit, the oldest turns are dropped and a note appears in the chat; the latest
  turn and each tool call with its result are kept together.
- **Context ring** to the right of the message box: how full the window is (accent, then yellow, then red), a percentage
  from 50%, a tooltip with where the value came from; a click opens Settings > AI.
- **A new panel** in the editor's own style: a flat card in the theme's colours instead of the glass card with a glow.
  Assistant replies are real Markdown (headings, lists, quotes, bold, italic, `code`, links, code blocks with Copy and
  wrapping). Tool calls are compact rows with status, duration and an expandable result; there is a "Thinking" line
  while the model works; errors and notices are shown inline; a "jump to latest" button appears when you scroll up. The
  panel's left edge can be dragged to change its width. The opening animation has no spring.
- **Message box**: several lines (Enter sends, Shift+Enter adds a line), grows up to 8 lines. Esc does not erase the
  draft; Esc stops the assistant while it works, when the message box has focus.
- UI: `InputTextFlags::SubmitOnEnter`, `InputTextMultiline` with a hint and focus, `OpenPopupAnchored`,
  `GetScrollY` / `GetScrollMaxY`, `GetDisplaySize`.
- Fixed: the node lists of the Material, Behavior Tree and ZGraph editors stay on screen (they scroll) and take typing
  as soon as they open.
- Tests: `Models.*` (parsing of model lists in OpenAI, Anthropic, OpenRouter, Gemini, Ollama, llama.cpp and other
  formats, the known-context table, list requests), `Agent.ContextTrimming`, `UI.MultilineSubmitOnEnter`,
  `UI.PopupSearchFieldAcceptsTyping`. Full `ZXTests` run: 473 tests passed. Live check: the editor against an
  OpenAI-compatible test server — the model list is requested when the panel opens, a tool call and a reply go through.
- Settings from 0.17.8 (`AI.json`) are read: keys, model, URL, effort, Max Tokens and MCP settings. `openai-compatible`
  becomes the "Custom" provider.

## 0.17.8 — Zeqou AI: assistant and MCP server (2026-11-01)
- **Zeqou AI**, an AI assistant in the editor. The button in the toolbar (to the left of LITE / FULL) or **Ctrl+Space**
  opens the panel. Chat bubbles, tool-call chips with their state, a typing indicator, prompt suggestions, a request and
  token counter, buttons for a new chat, connecting an agent, settings, and Esc to close.
  - **Your own API key**: Anthropic (Claude Opus 5.5 by default; effort low to max, a fallback model when a request is
    declined, conversation caching), OpenAI, or any OpenAI-compatible server (Ollama, LM Studio, OpenRouter and others).
    The key is encrypted for your Windows account (DPAPI) and stored in `%LOCALAPPDATA%/ZeqouX3D/AI.json`, never in a
    project.
  - The assistant builds things itself: it creates and changes objects, writes scripts, starts Play, looks at the
    viewport through a screenshot and reads the log. Every scene change is a separate Undo step, "AI: …".
- **Editor MCP server** (Model Context Protocol, Streamable HTTP, JSON responses). Any agent (Claude Code, Cursor,
  VS Code / Copilot, Claude Desktop through `mcp-remote`) can connect to the editor and make a game from a text request.
  It is turned on in Settings > AI or on the panel's connect page, which also has ready commands and configs with a Copy
  button. It listens on 127.0.0.1 only, requires a token (`Authorization: Bearer`) and rejects foreign Origins.
- 17 tools shared by the assistant and MCP: `get_editor_state`, `get_docs`, `list_entities`, `get_entity`,
  `create_entities` (cube, sphere, cylinder and plane shapes with a collider; position, rotation in degrees, scale,
  parent, components), `modify_entity`, `delete_entities`, `select_entities`, `run_console_command`, `get_log`,
  `list_files` / `read_file` / `write_file` (text files inside the project only), `screenshot`, `play_control`,
  `save_scene`, `undo`. Calls run on the main thread between frames.
- **Settings > AI**: provider, key, model, server, effort, Max Tokens, MCP port and token (New Token).
- Console: `ai.open [0|1] [chat|connect]`, `ai.send <text>`, `ai.mcp <0|1>`.
- Platform: an HTTP(S) client on WinHTTP (with cancellation), a local HTTP server on Winsock, DPAPI secrets, the system
  random generator.
- Tests: `Mcp.*` (protocol, authorization, Origin, batches, a real request over localhost), `Agent.*` (the tool loop of
  Claude and OpenAI on recorded responses, cut-off and declined responses, a retry without unsupported parameters, key
  errors), `Net.SecretsRoundTrip`. Manual check: an external MCP client went through all 17 tools in the live editor; the
  built-in agent completed a full cycle against an OpenAI-compatible server.
