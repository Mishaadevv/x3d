// HTTP client (WinHTTP), localhost HTTP server (Winsock), DPAPI secrets, system RNG.

#include "Core/Log/Log.h"
#include "Core/String/Base64.h"
#include "Core/String/Parse.h"
#include "Core/String/Utf.h"
#include "Core/Threading/Mutex.h"
#include "Core/Threading/Thread.h"
#include "Platform/Net.h"

#include "Win32/Win32Include.h"

#include <WinSock2.h>
#include <WS2tcpip.h>
#include <bcrypt.h>
#include <dpapi.h>
#include <winhttp.h>

ZX_DEFINE_LOG_CATEGORY_STATIC(LogNet, Info);

namespace zx::platform {

// --- HTTP client ----------------------------------------------------------------------------------------------------

namespace {

String WinHttpErrorText(DWORD code)
{
    switch (code) {
    case ERROR_WINHTTP_TIMEOUT: return "timed out";
    case ERROR_WINHTTP_NAME_NOT_RESOLVED: return "host name not resolved (no internet?)";
    case ERROR_WINHTTP_CANNOT_CONNECT: return "cannot connect";
    case ERROR_WINHTTP_CONNECTION_ERROR: return "connection reset";
    case ERROR_WINHTTP_SECURE_FAILURE: return "TLS failure";
    case ERROR_WINHTTP_OPERATION_CANCELLED: return "cancelled";
    case ERROR_WINHTTP_INVALID_URL:
    case ERROR_WINHTTP_UNRECOGNIZED_SCHEME: return "invalid URL";
    default: return String::Format("WinHTTP error {}", u32(code));
    }
}

struct InternetHandle
{
    HINTERNET h = nullptr;
    ~InternetHandle()
    {
        if (h)
            ::WinHttpCloseHandle(h);
    }
};

} // namespace

HttpResponse HttpRequest(const HttpRequestDesc& desc, const std::atomic<bool>* cancel)
{
    HttpResponse res;
    Array<wchar_t> url = Utf8ToUtf16(desc.url);
    URL_COMPONENTS parts = {};
    parts.dwStructSize = sizeof(parts);
    wchar_t host[256] = {};
    wchar_t path[2048] = {};
    parts.lpszHostName = host;
    parts.dwHostNameLength = DWORD(ZX_ARRAY_COUNT(host));
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = DWORD(ZX_ARRAY_COUNT(path));
    wchar_t extra[2048] = {};
    parts.lpszExtraInfo = extra;
    parts.dwExtraInfoLength = DWORD(ZX_ARRAY_COUNT(extra));
    if (!::WinHttpCrackUrl(url.Data(), 0, 0, &parts)) {
        res.error = "invalid URL";
        return res;
    }
    const bool https = parts.nScheme == INTERNET_SCHEME_HTTPS;

    InternetHandle session, connection;
    session.h = ::WinHttpOpen(L"ZeqouX3D", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.h) {
        res.error = WinHttpErrorText(::GetLastError());
        return res;
    }
    const int timeout = int(desc.timeoutMs);
    ::WinHttpSetTimeouts(session.h, 15000, 15000, 60000, timeout);
    connection.h = ::WinHttpConnect(session.h, host, parts.nPort, 0);
    if (!connection.h) {
        res.error = WinHttpErrorText(::GetLastError());
        return res;
    }
    String fullPath = Utf16ToUtf8(path);
    fullPath += Utf16ToUtf8(extra);
    Array<wchar_t> method = Utf8ToUtf16(desc.method);
    Array<wchar_t> object = Utf8ToUtf16(fullPath);
    HINTERNET request = ::WinHttpOpenRequest(connection.h, method.Data(), object.Data(), nullptr, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES, https ? WINHTTP_FLAG_SECURE : 0);
    if (!request) {
        res.error = WinHttpErrorText(::GetLastError());
        return res;
    }

    // Cancellation: closing the request handle from another thread aborts a pending synchronous call.
    Mutex closeMutex;
    bool finished = false;
    bool closedByWatcher = false;
    Event stopWatcher(true);
    Thread watcher;
    if (cancel) {
        watcher.Start("HttpCancel", [&] {
            while (!stopWatcher.Wait(50)) {
                if (cancel->load(std::memory_order_relaxed)) {
                    ScopedLock lock(closeMutex);
                    if (!finished && !closedByWatcher) {
                        ::WinHttpCloseHandle(request);
                        closedByWatcher = true;
                    }
                    return;
                }
            }
        });
    }

    String headers;
    for (const HttpHeader& h : desc.headers)
        headers.Appendf("{}: {}\r\n", h.name, h.value);
    Array<wchar_t> headersW = Utf8ToUtf16(headers);
    bool ok = ::WinHttpSendRequest(request, headers.Empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headersW.Data(),
                                   headers.Empty() ? 0 : DWORD(-1), (void*)desc.body.Data(), DWORD(desc.body.Size()),
                                   DWORD(desc.body.Size()), 0) &&
              ::WinHttpReceiveResponse(request, nullptr);
    if (ok) {
        DWORD status = 0, size = sizeof(status);
        ::WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                              &status, &size, WINHTTP_NO_HEADER_INDEX);
        res.status = status;
        char buffer[16384];
        for (;;) {
            DWORD read = 0;
            if (!::WinHttpReadData(request, buffer, sizeof(buffer), &read)) {
                ok = false;
                break;
            }
            if (read == 0)
                break;
            res.body.Append(StringView(buffer, read));
        }
    }
    const DWORD error = ok ? 0 : ::GetLastError();

    {
        ScopedLock lock(closeMutex);
        finished = true;
    }
    if (watcher.IsJoinable()) {
        stopWatcher.Signal();
        watcher.Join();
    }
    if (!closedByWatcher)
        ::WinHttpCloseHandle(request);
    if (!ok) {
        res.status = 0;
        res.error = closedByWatcher ? String("cancelled") : WinHttpErrorText(error);
    }
    return res;
}

// --- Local HTTP server ----------------------------------------------------------------------------------------------

StringView HttpServerRequest::Header(StringView name) const
{
    for (const HttpHeader& h : headers)
        if (h.name.EqualsIgnoreCase(name))
            return h.value;
    return {};
}

namespace {

constexpr usize MaxHeaderBytes = 64 * 1024;
constexpr usize MaxBodyBytes = 32 * 1024 * 1024;

const char* StatusText(u32 status)
{
    switch (status) {
    case 200: return "OK";
    case 202: return "Accepted";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 413: return "Payload Too Large";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    default: return "Status";
    }
}

bool SendAll(SOCKET s, StringView data)
{
    usize sent = 0;
    while (sent < data.Size()) {
        const int n = ::send(s, data.Data() + sent, int(Min<usize>(data.Size() - sent, 1 << 20)), 0);
        if (n <= 0)
            return false;
        sent += usize(n);
    }
    return true;
}

/// Reads one request. Returns 0 on success, otherwise the HTTP status to answer with (or 1 = drop silently).
u32 ReadRequest(SOCKET s, HttpServerRequest& req)
{
    String data;
    char buffer[8192];
    usize headerEnd = StringView::NPos;
    while (headerEnd == StringView::NPos) {
        const int n = ::recv(s, buffer, sizeof(buffer), 0);
        if (n <= 0)
            return 1;
        data.Append(StringView(buffer, usize(n)));
        headerEnd = data.Find("\r\n\r\n");
        if (headerEnd == StringView::NPos && data.Size() > MaxHeaderBytes)
            return 413;
    }
    StringView head = data.View().Left(headerEnd);
    usize lineEnd = head.Find("\r\n");
    StringView requestLine = lineEnd == StringView::NPos ? head : head.Left(lineEnd);
    const usize sp1 = requestLine.Find(' ');
    const usize sp2 = sp1 == StringView::NPos ? sp1 : requestLine.Find(' ', sp1 + 1);
    if (sp1 == StringView::NPos || sp2 == StringView::NPos)
        return 400;
    req.method = requestLine.Left(sp1);
    StringView target = requestLine.SubStr(sp1 + 1, sp2 - sp1 - 1);
    const usize query = target.Find('?');
    req.path = query == StringView::NPos ? target : target.Left(query);

    usize contentLength = 0;
    StringView rest = lineEnd == StringView::NPos ? StringView() : head.DropLeft(lineEnd + 2);
    while (!rest.Empty()) {
        const usize e = rest.Find("\r\n");
        StringView line = e == StringView::NPos ? rest : rest.Left(e);
        rest = e == StringView::NPos ? StringView() : rest.DropLeft(e + 2);
        const usize colon = line.Find(':');
        if (colon == StringView::NPos)
            continue;
        HttpHeader h{String(line.Left(colon).Trim()), String(line.DropLeft(colon + 1).Trim())};
        if (h.name.EqualsIgnoreCase("Content-Length")) {
            Optional<u64> len = ParseUInt(h.value);
            if (!len)
                return 400;
            if (*len > MaxBodyBytes)
                return 413;
            contentLength = usize(*len);
        } else if (h.name.EqualsIgnoreCase("Transfer-Encoding")) {
            return 400; // chunked requests are not supported (MCP clients send Content-Length)
        }
        req.headers.PushBack(move(h));
    }
    req.body = data.View().DropLeft(headerEnd + 4);
    while (req.body.Size() < contentLength) {
        const int n = ::recv(s, buffer, int(Min<usize>(sizeof(buffer), contentLength - req.body.Size())), 0);
        if (n <= 0)
            return 1;
        req.body.Append(StringView(buffer, usize(n)));
    }
    if (req.body.Size() > contentLength)
        req.body.Resize(contentLength);
    return 0;
}

} // namespace

struct LocalHttpServer::Impl
{
    Handler handler;
    SOCKET listener = INVALID_SOCKET;
    Thread acceptThread;
    std::atomic<bool> running{false};
    std::atomic<u32> activeConnections{0};
    std::atomic<u64> requests{0};
    u16 port = 0;
    bool wsaStarted = false;

    void Serve(SOCKET client)
    {
        HttpServerRequest req;
        const u32 readStatus = ReadRequest(client, req);
        if (readStatus != 1) {
            HttpServerResponse res;
            if (readStatus != 0) {
                res.status = readStatus;
                res.contentType = "text/plain";
                res.body = StatusText(readStatus);
            } else {
                requests.fetch_add(1, std::memory_order_relaxed);
                res = handler(req);
            }
            String out = String::Format("HTTP/1.1 {} {}\r\nContent-Length: {}\r\nConnection: close\r\nCache-Control: no-store\r\n",
                                        res.status, StatusText(res.status), res.body.Size());
            if (!res.contentType.Empty())
                out.Appendf("Content-Type: {}\r\n", res.contentType);
            for (const HttpHeader& h : res.headers)
                out.Appendf("{}: {}\r\n", h.name, h.value);
            out += "\r\n";
            out += res.body;
            SendAll(client, out);
        }
        ::shutdown(client, SD_SEND);
        ::closesocket(client);
    }

    void AcceptLoop()
    {
        while (running.load()) {
            SOCKET client = ::accept(listener, nullptr, nullptr);
            if (client == INVALID_SOCKET) {
                if (!running.load())
                    break;
                Thread::SleepMs(10);
                continue;
            }
            const DWORD timeout = 15000;
            ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
            ::setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));
            activeConnections.fetch_add(1);
            Thread worker;
            const bool started = worker.Start("HttpConnection", [this, client] {
                Serve(client);
                activeConnections.fetch_sub(1);
            });
            if (started) {
                worker.Detach();
            } else {
                ::closesocket(client);
                activeConnections.fetch_sub(1);
            }
        }
    }
};

LocalHttpServer::LocalHttpServer() : m_impl(new Impl) {}

LocalHttpServer::~LocalHttpServer()
{
    Stop();
    if (m_impl->activeConnections.load() == 0)
        delete m_impl; // otherwise a stuck connection thread still uses it: leak instead of a use-after-free
}

bool LocalHttpServer::Start(u16 port, Handler handler, String* error)
{
    Stop();
    auto fail = [&](StringView what) {
        if (error)
            *error = String::Format("{} (error {})", what, ::WSAGetLastError());
        if (m_impl->listener != INVALID_SOCKET)
            ::closesocket(m_impl->listener);
        m_impl->listener = INVALID_SOCKET;
        return false;
    };
    if (!m_impl->wsaStarted) {
        WSADATA wsa;
        if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
            return fail("WSAStartup failed");
        m_impl->wsaStarted = true;
    }
    m_impl->listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (m_impl->listener == INVALID_SOCKET)
        return fail("socket() failed");
    BOOL exclusive = TRUE; // no other process may bind the same port and steal requests
    ::setsockopt(m_impl->listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&exclusive, sizeof(exclusive));
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = ::htons(port);
    addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    if (::bind(m_impl->listener, (sockaddr*)&addr, sizeof(addr)) != 0)
        return fail(String::Format("Port {} is in use", port));
    if (::listen(m_impl->listener, SOMAXCONN) != 0)
        return fail("listen() failed");
    m_impl->handler = move(handler);
    m_impl->port = port;
    m_impl->requests = 0;
    m_impl->running = true;
    if (!m_impl->acceptThread.Start("HttpServer", [impl = m_impl] { impl->AcceptLoop(); })) {
        m_impl->running = false;
        return fail("thread start failed");
    }
    ZX_LOG_INFO(LogNet, "HTTP server listening on 127.0.0.1:{}", port);
    return true;
}

void LocalHttpServer::Stop()
{
    if (m_impl->running.exchange(false)) {
        ::closesocket(m_impl->listener); // unblocks accept()
        m_impl->listener = INVALID_SOCKET;
        m_impl->acceptThread.Join();
        // Connection threads finish within the socket timeouts (handlers must not block on the caller of Stop).
        for (u32 i = 0; i < 2000 && m_impl->activeConnections.load() > 0; ++i)
            Thread::SleepMs(10);
        if (m_impl->activeConnections.load() > 0)
            ZX_LOG_WARN(LogNet, "HTTP server stopped with {} connection(s) still open", m_impl->activeConnections.load());
    }
    if (m_impl->wsaStarted && m_impl->activeConnections.load() == 0) {
        ::WSACleanup();
        m_impl->wsaStarted = false;
    }
}

bool LocalHttpServer::IsRunning() const { return m_impl->running.load(); }
u16 LocalHttpServer::Port() const { return m_impl->port; }
u64 LocalHttpServer::RequestCount() const { return m_impl->requests.load(); }

// --- Secrets --------------------------------------------------------------------------------------------------------

namespace {
constexpr char SecretEntropy[] = "ZeqouX3D.Secret.v1";
}

String ProtectSecret(StringView plain)
{
    DATA_BLOB in{DWORD(plain.Size()), (BYTE*)plain.Data()};
    DATA_BLOB entropy{DWORD(sizeof(SecretEntropy) - 1), (BYTE*)SecretEntropy};
    DATA_BLOB out = {};
    if (!::CryptProtectData(&in, L"ZeqouX3D", &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
        return {};
    String result = Base64Encode(ByteSpan(out.pbData, out.cbData));
    ::SecureZeroMemory(out.pbData, out.cbData);
    ::LocalFree(out.pbData);
    return result;
}

bool UnprotectSecret(StringView protectedBase64, String& plain)
{
    Array<u8> bytes;
    if (!Base64Decode(protectedBase64, bytes) || bytes.Empty())
        return false;
    DATA_BLOB in{DWORD(bytes.Size()), bytes.Data()};
    DATA_BLOB entropy{DWORD(sizeof(SecretEntropy) - 1), (BYTE*)SecretEntropy};
    DATA_BLOB out = {};
    if (!::CryptUnprotectData(&in, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
        return false;
    plain.Assign(StringView((const char*)out.pbData, out.cbData));
    ::SecureZeroMemory(out.pbData, out.cbData);
    ::LocalFree(out.pbData);
    return true;
}

String RandomHex(u32 bytes)
{
    Array<u8> raw;
    raw.Resize(bytes);
    if (bytes && !BCRYPT_SUCCESS(::BCryptGenRandom(nullptr, raw.Data(), bytes, BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
        return {};
    static constexpr char Digits[] = "0123456789abcdef";
    String out;
    out.Reserve(bytes * 2);
    for (u8 b : raw) {
        out += Digits[b >> 4];
        out += Digits[b & 15];
    }
    return out;
}

} // namespace zx::platform
