// AI assistance: the MCP endpoint (auth, origin, JSON-RPC methods, tool calls over a real localhost socket), the
// agent's tool loop against a scripted fake provider (Anthropic and OpenAI wire formats), DPAPI secrets.

#include "Core/Testing/Test.h"
#include "Engine/AIAssistant.h"
#include "Platform/Net.h"

using namespace zx;
using namespace zx::ai;

namespace {

class TestHost final : public ToolHost
{
public:
    TestHost()
    {
        ToolDef add;
        add.name = "add";
        add.description = "Adds a and b";
        ParseJson(R"({"type":"object","properties":{"a":{"type":"number"},"b":{"type":"number"}},"required":["a","b"]})", add.inputSchema);
        add.readOnly = true;
        m_tools.PushBack(move(add));
        ToolDef shot;
        shot.name = "screenshot";
        shot.description = "Returns an image";
        m_tools.PushBack(move(shot));
    }
    Span<const ToolDef> Tools() const override { return m_tools.View(); }
    ToolResult Call(StringView name, const JsonValue& args) override
    {
        ++calls;
        if (name == "add") {
            if (!args.Get("a").IsNumber() || !args.Get("b").IsNumber())
                return ToolResult::Error("a and b must be numbers");
            return ToolResult::Ok(String::Format("{}", args.Get("a").AsInt() + args.Get("b").AsInt()));
        }
        ToolResult r = ToolResult::Ok("viewport");
        r.pngBase64 = "iVBORw0KGgo=";
        return r;
    }
    u32 calls = 0;

private:
    Array<ToolDef> m_tools;
};

platform::HttpServerRequest Post(StringView body, StringView token = "secret")
{
    platform::HttpServerRequest r;
    r.method = "POST";
    r.path = "/mcp";
    r.body = body;
    if (!token.Empty())
        r.headers.PushBack({"Authorization", String::Format("Bearer {}", token)});
    return r;
}

JsonValue ParseBody(const platform::HttpServerResponse& r)
{
    JsonValue v;
    ParseJson(r.body, v);
    return v;
}

} // namespace

ZX_TEST(Mcp, InitializeListAndCall)
{
    TestHost host;
    McpServer mcp;
    mcp.Configure(McpOptions{.token = "secret", .instructions = "Use the tools.", .serverVersion = "1.2.3"}, host);

    JsonValue init = ParseBody(mcp.Handle(Post(
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"claude-code","version":"2.1"}}})")));
    ZX_EXPECT(init.Get("id").AsInt() == 1);
    ZX_EXPECT(init.Get("result").Get("protocolVersion").AsString() == "2025-06-18"); // the client's version is kept
    ZX_EXPECT(init.Get("result").Get("capabilities").Get("tools").IsObject());
    ZX_EXPECT(init.Get("result").Get("serverInfo").Get("version").AsString() == "1.2.3");
    ZX_EXPECT(init.Get("result").Get("instructions").AsString() == "Use the tools.");
    ZX_EXPECT(mcp.LastClient() == "claude-code 2.1");

    JsonValue unknownVersion = ParseBody(mcp.Handle(Post(R"({"jsonrpc":"2.0","id":2,"method":"initialize","params":{"protocolVersion":"1999-01-01"}})")));
    ZX_EXPECT(unknownVersion.Get("result").Get("protocolVersion").AsString() == McpProtocolVersion);

    // Notifications are accepted without a body.
    const platform::HttpServerResponse note = mcp.Handle(Post(R"({"jsonrpc":"2.0","method":"notifications/initialized"})"));
    ZX_EXPECT_EQ(note.status, 202u);
    ZX_EXPECT(note.body.Empty());

    JsonValue list = ParseBody(mcp.Handle(Post(R"({"jsonrpc":"2.0","id":"a","method":"tools/list"})")));
    ZX_EXPECT(list.Get("id").AsString() == "a");
    const JsonValue& tools = list.Get("result").Get("tools");
    ZX_REQUIRE(tools.Size() == 2);
    ZX_EXPECT(tools[0].Get("name").AsString() == "add");
    ZX_EXPECT(tools[0].Get("inputSchema").Get("required").Size() == 2);
    ZX_EXPECT(tools[0].Get("annotations").Get("readOnlyHint").AsBool());
    ZX_EXPECT(tools[1].Get("inputSchema").Get("type").AsString() == "object"); // filled in when the tool has none

    JsonValue call = ParseBody(mcp.Handle(Post(R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"add","arguments":{"a":2,"b":40}}})")));
    ZX_EXPECT(call.Get("result").Get("content")[0].Get("text").AsString() == "42");
    ZX_EXPECT(!call.Get("result").Get("isError").AsBool(true));

    JsonValue bad = ParseBody(mcp.Handle(Post(R"({"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"add","arguments":{"a":"x"}}})")));
    ZX_EXPECT(bad.Get("result").Get("isError").AsBool()); // tool errors are results, not protocol errors

    JsonValue image = ParseBody(mcp.Handle(Post(R"({"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"screenshot"}})")));
    ZX_EXPECT(image.Get("result").Get("content")[1].Get("type").AsString() == "image");
    ZX_EXPECT(image.Get("result").Get("content")[1].Get("mimeType").AsString() == "image/png");

    JsonValue unknownTool = ParseBody(mcp.Handle(Post(R"({"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"nope"}})")));
    ZX_EXPECT(unknownTool.Get("error").Get("code").AsInt() == -32602);
    JsonValue unknownMethod = ParseBody(mcp.Handle(Post(R"({"jsonrpc":"2.0","id":7,"method":"sampling/whatever"})")));
    ZX_EXPECT(unknownMethod.Get("error").Get("code").AsInt() == -32601);
    JsonValue ping = ParseBody(mcp.Handle(Post(R"({"jsonrpc":"2.0","id":8,"method":"ping"})")));
    ZX_EXPECT(ping.Get("result").IsObject());
    ZX_EXPECT_EQ(host.calls, 3u);
    ZX_EXPECT_EQ(mcp.ToolCallCount(), 3u);
}

ZX_TEST(Mcp, RejectsBadRequests)
{
    TestHost host;
    McpServer mcp;
    mcp.Configure(McpOptions{.token = "secret"}, host);
    const StringView ping = R"({"jsonrpc":"2.0","id":1,"method":"ping"})";
    ZX_EXPECT_EQ(mcp.Handle(Post(ping, "")).status, 401u);
    ZX_EXPECT_EQ(mcp.Handle(Post(ping, "wrong")).status, 401u);
    ZX_EXPECT_EQ(mcp.Handle(Post(ping, "secre")).status, 401u);

    platform::HttpServerRequest foreign = Post(ping);
    foreign.headers.PushBack({"Origin", "https://evil.example.com"});
    ZX_EXPECT_EQ(mcp.Handle(foreign).status, 403u);
    platform::HttpServerRequest lookalike = Post(ping);
    lookalike.headers.PushBack({"Origin", "http://localhost.evil.example.com"});
    ZX_EXPECT_EQ(mcp.Handle(lookalike).status, 403u);
    platform::HttpServerRequest local = Post(ping);
    local.headers.PushBack({"Origin", "http://localhost:6274"});
    ZX_EXPECT_EQ(mcp.Handle(local).status, 200u);

    platform::HttpServerRequest get = Post(ping);
    get.method = "GET";
    ZX_EXPECT_EQ(mcp.Handle(get).status, 405u);
    platform::HttpServerRequest wrongPath = Post(ping);
    wrongPath.path = "/other";
    ZX_EXPECT_EQ(mcp.Handle(wrongPath).status, 404u);

    const platform::HttpServerResponse garbage = mcp.Handle(Post("{not json"));
    ZX_EXPECT_EQ(garbage.status, 400u);
    ZX_EXPECT(ParseBody(garbage).Get("error").Get("code").AsInt() == -32700);

    // Batches (older clients).
    JsonValue batch = ParseBody(mcp.Handle(Post(R"([{"jsonrpc":"2.0","id":1,"method":"ping"},{"jsonrpc":"2.0","method":"notifications/x"},{"jsonrpc":"2.0","id":2,"method":"ping"}])")));
    ZX_EXPECT_EQ(batch.Size(), 2u);
}

ZX_TEST(Mcp, ServesOverLocalhost)
{
    TestHost host;
    McpServer mcp;
    String error;
    u16 port = 0;
    for (u16 p = 47911; p < 47931 && !port; ++p)
        if (mcp.Start(p, McpOptions{.token = "tok"}, host, &error))
            port = p;
    ZX_REQUIRE(port != 0);

    platform::HttpRequestDesc req;
    req.url = String::Format("http://127.0.0.1:{}/mcp", port);
    req.headers.PushBack({"Content-Type", "application/json"});
    req.headers.PushBack({"Accept", "application/json, text/event-stream"});
    req.headers.PushBack({"Authorization", "Bearer tok"});
    req.body = R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"add","arguments":{"a":20,"b":22}}})";
    req.timeoutMs = 10000;
    const platform::HttpResponse res = platform::HttpRequest(req);
    ZX_EXPECT_EQ(res.status, 200u);
    JsonValue doc;
    ZX_REQUIRE(ParseJson(res.body, doc));
    ZX_EXPECT(doc.Get("result").Get("content")[0].Get("text").AsString() == "42");

    req.headers.PopBack();
    ZX_EXPECT_EQ(platform::HttpRequest(req).status, 401u);
    ZX_EXPECT_GE(mcp.RequestCount(), 2u);

    // The port is exclusive while the server runs.
    platform::LocalHttpServer other;
    ZX_EXPECT(!other.Start(port, [](const platform::HttpServerRequest&) { return platform::HttpServerResponse{}; }));
    mcp.Stop();
    ZX_EXPECT(!mcp.IsRunning());
}

namespace {

/// Answers the agent's requests from a script and records them.
struct FakeProvider
{
    Array<String> responses;
    Array<JsonValue> requests;
    Array<Array<platform::HttpHeader>> headers;
    u32 status = 200;

    Agent::Transport Transport()
    {
        return [this](const platform::HttpRequestDesc& r, const std::atomic<bool>*) {
            JsonValue body;
            ParseJson(r.body, body);
            requests.PushBack(move(body));
            headers.PushBack(r.headers);
            platform::HttpResponse res;
            const usize i = requests.Size() - 1;
            res.status = status;
            res.body = i < responses.Size() ? responses[i] : String(R"j({"content":[{"type":"text","text":"(script ended)"}],"stop_reason":"end_turn"})j");
            return res;
        };
    }
};

StringView HeaderValue(Span<const platform::HttpHeader> headers, StringView name)
{
    for (const platform::HttpHeader& h : headers)
        if (h.name.EqualsIgnoreCase(name))
            return h.value;
    return {};
}

} // namespace

ZX_TEST(Agent, AnthropicToolLoop)
{
    TestHost host;
    FakeProvider fake;
    fake.responses.PushBack(R"({"content":[{"type":"thinking","thinking":"plan","signature":"sig1"},{"type":"text","text":"Adding."},
        {"type":"tool_use","id":"toolu_1","name":"add","input":{"a":1,"b":2}},{"type":"tool_use","id":"toolu_2","name":"screenshot","input":{}}],
        "stop_reason":"tool_use","usage":{"input_tokens":100,"output_tokens":20,"cache_read_input_tokens":5}})");
    fake.responses.PushBack(R"({"content":[{"type":"text","text":"The sum is 3."}],"stop_reason":"end_turn","usage":{"input_tokens":10,"output_tokens":5}})");
    Agent agent(host);
    agent.SetTransport(fake.Transport());
    AgentConfig config;
    config.apiKey = "sk-test";
    ZX_REQUIRE(agent.Send(config, "You are a test.", "What is 1 + 2?"));
    agent.Wait();
    ZX_EXPECT(!agent.IsBusy());

    ZX_REQUIRE(fake.requests.Size() == 2);
    const JsonValue& first = fake.requests[0];
    ZX_EXPECT(first.Get("model").AsString() == "claude-opus-5-5");
    ZX_EXPECT(first.Get("system")[0].Get("text").AsString() == "You are a test.");
    ZX_EXPECT(first.Get("tools")[0].Get("input_schema").Get("type").AsString() == "object");
    ZX_EXPECT(first.Get("output_config").Get("effort").AsString() == "high");
    ZX_EXPECT(first.Get("fallbacks").AsString() == "default");
    ZX_EXPECT(!first.Contains("thinking")); // adaptive thinking is the model's default
    ZX_EXPECT(!first.Contains("temperature"));
    ZX_EXPECT(HeaderValue(fake.headers[0].View(), "x-api-key") == "sk-test");
    ZX_EXPECT(HeaderValue(fake.headers[0].View(), "anthropic-version") == "2026-10-21");
    ZX_EXPECT(HeaderValue(fake.headers[0].View(), "anthropic-beta") == "server-side-fallback-2026-07-01");

    // Second request: the assistant turn unchanged (thinking kept), then all tool results in one user message.
    const JsonValue& msgs = fake.requests[1].Get("messages");
    ZX_REQUIRE(msgs.Size() == 3);
    ZX_EXPECT(msgs[1].Get("content")[0].Get("signature").AsString() == "sig1");
    const JsonValue& results = msgs[2].Get("content");
    ZX_REQUIRE(results.Size() == 2);
    ZX_EXPECT(results[0].Get("tool_use_id").AsString() == "toolu_1");
    ZX_EXPECT(results[0].Get("content")[0].Get("text").AsString() == "3");
    ZX_EXPECT(results[1].Get("content")[1].Get("type").AsString() == "image");
    ZX_EXPECT(results[1].Get("content")[1].Get("source").Get("media_type").AsString() == "image/png");

    const Array<AgentEvent> events = agent.TakeEvents();
    u32 texts = 0, starts = 0, ends = 0, images = 0, done = 0;
    for (const AgentEvent& e : events) {
        texts += e.type == AgentEventType::Text;
        starts += e.type == AgentEventType::ToolStart;
        ends += e.type == AgentEventType::ToolEnd;
        images += e.type == AgentEventType::Image;
        done += e.type == AgentEventType::Done && !e.isError;
    }
    ZX_EXPECT_EQ(texts, 2u);
    ZX_EXPECT_EQ(starts, 2u);
    ZX_EXPECT_EQ(ends, 2u);
    ZX_EXPECT_EQ(images, 1u);
    ZX_EXPECT_EQ(done, 1u);
    ZX_EXPECT_EQ(agent.Usage().inputTokens, 110u);
    ZX_EXPECT_EQ(agent.Usage().requests, 2u);

    // The next message continues the same conversation.
    ZX_REQUIRE(agent.Send(config, "You are a test.", "Thanks"));
    agent.Wait();
    ZX_EXPECT_EQ(fake.requests[2].Get("messages").Size(), 5u);
}

ZX_TEST(Agent, AnthropicStopReasonsAndRetries)
{
    TestHost host;
    {
        // A cut-off turn: its tool call never runs and is not kept.
        FakeProvider fake;
        fake.responses.PushBack(R"({"content":[{"type":"text","text":"Let me"},{"type":"tool_use","id":"t","name":"add","input":{"a":1}}],"stop_reason":"max_tokens"})");
        Agent agent(host);
        agent.SetTransport(fake.Transport());
        AgentConfig config;
        config.apiKey = "k";
        ZX_REQUIRE(agent.Send(config, "", "hi"));
        agent.Wait();
        ZX_EXPECT_EQ(host.calls, 0u);
        ZX_REQUIRE(agent.Send(config, "", "continue"));
        agent.Wait();
        const JsonValue& msgs = fake.requests[1].Get("messages");
        ZX_REQUIRE(msgs.Size() == 3);
        ZX_EXPECT(msgs[1].Get("content").Size() == 1); // only the text
    }
    {
        // A model that does not take the optional parameters: they are dropped and the request repeated.
        FakeProvider fake;
        fake.responses.PushBack(R"({"type":"error","error":{"type":"invalid_request_error","message":"fallbacks: not supported for this model"}})");
        fake.responses.PushBack(R"({"type":"error","error":{"type":"invalid_request_error","message":"output_config.effort: unsupported"}})");
        fake.responses.PushBack(R"({"content":[{"type":"text","text":"ok"}],"stop_reason":"end_turn"})");
        Agent agent(host);
        u32 n = 0;
        agent.SetTransport([&](const platform::HttpRequestDesc& r, const std::atomic<bool>* c) {
            fake.status = n++ < 2 ? 400 : 200;
            return fake.Transport()(r, c);
        });
        AgentConfig config;
        config.apiKey = "k";
        config.model = "claude-haiku-4-5";
        ZX_REQUIRE(agent.Send(config, "", "hi"));
        agent.Wait();
        ZX_REQUIRE(fake.requests.Size() == 3);
        ZX_EXPECT(!fake.requests[1].Contains("fallbacks"));
        ZX_EXPECT(!fake.requests[2].Contains("output_config"));
        ZX_EXPECT(fake.requests[2].Get("model").AsString() == "claude-haiku-4-5");
    }
    {
        // A bad key ends the turn with a readable error.
        FakeProvider fake;
        fake.status = 401;
        fake.responses.PushBack(R"({"type":"error","error":{"type":"authentication_error","message":"invalid x-api-key"}})");
        Agent agent(host);
        agent.SetTransport(fake.Transport());
        AgentConfig config;
        config.apiKey = "bad";
        ZX_REQUIRE(agent.Send(config, "", "hi"));
        agent.Wait();
        bool sawError = false;
        for (const AgentEvent& e : agent.TakeEvents())
            sawError |= e.type == AgentEventType::Error && e.text.Contains("invalid x-api-key") && e.text.Contains("Settings > AI");
        ZX_EXPECT(sawError);
    }
    // No key: nothing starts.
    Agent agent(host);
    ZX_EXPECT(!agent.Send(AgentConfig{}, "", "hi"));
}

ZX_TEST(Agent, FallbackContentIsTrimmed)
{
    JsonValue content;
    ZX_REQUIRE(ParseJson(R"([{"type":"thinking","thinking":"a","signature":"s"},{"type":"text","text":"partial"},
        {"type":"tool_use","id":"x","name":"add","input":{}},{"type":"fallback","from":{"model":"m1"},"to":{"model":"m2"}},
        {"type":"thinking","thinking":"b","signature":"s2"},{"type":"text","text":"answer"}])", content));
    const JsonValue kept = AnthropicHistoryContent(content);
    ZX_REQUIRE(kept.Size() == 4);
    ZX_EXPECT(kept[0].Get("text").AsString() == "partial");
    ZX_EXPECT(kept[1].Get("type").AsString() == "fallback");
    ZX_EXPECT(kept[2].Get("signature").AsString() == "s2");
}

ZX_TEST(Agent, OpenAIToolLoop)
{
    TestHost host;
    FakeProvider fake;
    fake.responses.PushBack(R"({"choices":[{"message":{"role":"assistant","content":null,"tool_calls":[
        {"id":"call_1","type":"function","function":{"name":"add","arguments":"{\"a\":5,\"b\":6}"}},
        {"id":"call_2","type":"function","function":{"name":"screenshot","arguments":""}}]},"finish_reason":"tool_calls"}],
        "usage":{"prompt_tokens":50,"completion_tokens":7}})");
    fake.responses.PushBack(R"({"choices":[{"message":{"role":"assistant","content":"11"},"finish_reason":"stop"}]})");
    Agent agent(host);
    agent.SetTransport(fake.Transport());
    AgentConfig config;
    config.provider = Provider::OpenAICompatible;
    config.baseUrl = "http://localhost:1234/v1/";
    config.model = "local-model";
    ZX_REQUIRE(agent.Send(config, "sys", "5 + 6?"));
    agent.Wait();
    ZX_REQUIRE(fake.requests.Size() == 2);
    ZX_EXPECT(fake.requests[0].Get("messages")[0].Get("role").AsString() == "system");
    ZX_EXPECT(fake.requests[0].Get("tools")[0].Get("function").Get("name").AsString() == "add");
    ZX_EXPECT(fake.requests[0].Contains("max_tokens"));
    const JsonValue& msgs = fake.requests[1].Get("messages");
    // system, user, assistant (tool_calls), tool, tool, user (image)
    ZX_REQUIRE(msgs.Size() == 6);
    ZX_EXPECT(msgs[3].Get("tool_call_id").AsString() == "call_1");
    ZX_EXPECT(msgs[3].Get("content").AsString() == "11");
    ZX_EXPECT(msgs[5].Get("content")[1].Get("image_url").Get("url").AsString().StartsWith("data:image/png;base64,"));
    ZX_EXPECT_EQ(host.calls, 2u);
}

ZX_TEST(Agent, SummarizeArguments)
{
    JsonValue args;
    ZX_REQUIRE(ParseJson(R"({"name":"Cube","position":[0,1,2],"code":"line one\nline two"})", args));
    const String s = SummarizeArguments(args);
    ZX_EXPECT(s.StartsWith("name: Cube, position: [0,1,2], code: line one"));
    ZX_EXPECT(!s.Contains("line two"));
    ZX_EXPECT(ApiErrorMessage(404, R"({"error":{"message":"model not found"}})").Contains("model not found"));
}

ZX_TEST(Net, SecretsRoundTrip)
{
    const String protectedKey = platform::ProtectSecret("sk-ant-api03-example");
    ZX_REQUIRE(!protectedKey.Empty());
    ZX_EXPECT(!protectedKey.Contains("sk-ant"));
    String plain;
    ZX_EXPECT(platform::UnprotectSecret(protectedKey, plain));
    ZX_EXPECT(plain == "sk-ant-api03-example");
    ZX_EXPECT(!platform::UnprotectSecret("bm90IGRwYXBp", plain));
    const String a = platform::RandomHex(16), b = platform::RandomHex(16);
    ZX_EXPECT_EQ(a.Size(), 32u);
    ZX_EXPECT(a != b);
}

