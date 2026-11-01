#pragma once

// Networking and secrets on top of the OS (no third-party code):
//   - HttpRequest: blocking HTTP(S) client (WinHTTP); call it from a worker thread.
//   - LocalHttpServer: a small HTTP/1.1 server bound to 127.0.0.1 only (Winsock), one thread per connection; the
//     handler runs on those threads.
//   - ProtectSecret / UnprotectSecret: per-user encryption of small secrets such as API keys (DPAPI).
//
//   HttpRequestDesc req;
//   req.url = "https://api.example.com/v1/items";
//   req.headers.PushBack({"Content-Type", "application/json"});
//   req.body = "{}";
//   HttpResponse res = platform::HttpRequest(req);   // res.status, res.body, res.error

#include "Core/Containers/Array.h"
#include "Core/Containers/Function.h"
#include "Core/String/String.h"

#include <atomic>

namespace zx::platform {

struct HttpHeader
{
    String name;
    String value;
};

struct HttpRequestDesc
{
    String method = "POST";
    String url; // http:// or https://
    Array<HttpHeader> headers;
    String body;
    u32 timeoutMs = 600000; // long model turns
};

struct HttpResponse
{
    u32 status = 0;  // 0 = no response (see error)
    String body;
    String error;    // transport error (DNS, TLS, timeout, cancelled)
};

/// Blocking request. `cancel` (optional) is polled while waiting for data; setting it aborts the request.
HttpResponse HttpRequest(const HttpRequestDesc& request, const std::atomic<bool>* cancel = nullptr);

struct HttpServerRequest
{
    String method;
    String path; // without the query string
    Array<HttpHeader> headers;
    String body;

    StringView Header(StringView name) const; // case-insensitive; empty if absent
};

struct HttpServerResponse
{
    u32 status = 200;
    String contentType = "application/json";
    Array<HttpHeader> headers;
    String body;
};

class LocalHttpServer
{
public:
    using Handler = Function<HttpServerResponse(const HttpServerRequest&)>;

    LocalHttpServer();
    ~LocalHttpServer();
    ZX_NON_COPYABLE(LocalHttpServer)

    /// Listens on 127.0.0.1:port. Returns false (with `error`) if the port is taken or sockets fail.
    bool Start(u16 port, Handler handler, String* error = nullptr);
    void Stop();
    bool IsRunning() const;
    u16 Port() const;
    /// Requests served since Start.
    u64 RequestCount() const;

private:
    struct Impl;
    Impl* m_impl;
};

/// Encrypts for the current Windows user (DPAPI) and returns base64; empty on failure.
String ProtectSecret(StringView plain);
/// Inverse of ProtectSecret; false if the data was not produced by this user.
bool UnprotectSecret(StringView protectedBase64, String& plain);

/// Cryptographically random bytes as lowercase hex (2 characters per byte).
String RandomHex(u32 bytes);

} // namespace zx::platform
