#include "net.h"

#include "util.h"

namespace mango {

namespace {

std::string winError(DWORD code) {
    switch (code) {
    case ERROR_WINHTTP_TIMEOUT: return "timed out";
    case ERROR_WINHTTP_NAME_NOT_RESOLVED: return "no internet (name not found)";
    case ERROR_WINHTTP_CANNOT_CONNECT: return "couldn't connect";
    case ERROR_WINHTTP_CONNECTION_ERROR: return "connection lost";
    case ERROR_WINHTTP_SECURE_FAILURE: return "secure connection failed";
    }
    return "network error " + std::to_string(code);
}

DWORD statusOf(HINTERNET req) {
    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &size, WINHTTP_NO_HEADER_INDEX);
    return status;
}

int retryAfterOf(HINTERNET req) {
    DWORD v = 0, size = sizeof(v);
    if (WinHttpQueryHeaders(req, WINHTTP_QUERY_RETRY_AFTER | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &v, &size, WINHTTP_NO_HEADER_INDEX))
        return int(v);
    return 0;
}

}  // namespace

Http::Http() {
    session_ = WinHttpOpen(L"mango/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                           WINHTTP_NO_PROXY_BYPASS, 0);
    if (session_) {
        WinHttpSetTimeouts(session_, 10000, 10000, 15000, 30000);
        DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
        WinHttpSetOption(session_, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
        DWORD decompress = WINHTTP_DECOMPRESSION_FLAG_ALL;
        WinHttpSetOption(session_, WINHTTP_OPTION_DECOMPRESSION, &decompress, sizeof(decompress));
    }
}

Http::~Http() {
    if (session_) WinHttpCloseHandle(session_);
}

HttpResult Http::Request(const std::string& method, const std::string& url, const Headers& headers,
                         const std::string& body, size_t maxBytes) {
    HttpResult r;
    if (!session_) {
        r.error = "WinHTTP unavailable";
        return r;
    }
    std::wstring wurl = Widen(url);
    URL_COMPONENTS uc{sizeof(uc)};
    wchar_t host[256], path[4096];
    uc.lpszHostName = host;
    uc.dwHostNameLength = ARRAYSIZE(host);
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = ARRAYSIZE(path);
    uc.dwExtraInfoLength = 1;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) {
        r.error = "bad URL";
        return r;
    }
    std::wstring target(uc.lpszUrlPath, uc.dwUrlPathLength);
    if (uc.lpszExtraInfo) target.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    if (target.empty()) target = L"/";

    HINTERNET conn = WinHttpConnect(session_, host, uc.nPort, 0);
    if (!conn) {
        r.error = winError(GetLastError());
        return r;
    }
    DWORD flags = uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET req = WinHttpOpenRequest(conn, Widen(method).c_str(), target.c_str(), nullptr, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!req) {
        r.error = winError(GetLastError());
        WinHttpCloseHandle(conn);
        return r;
    }
    std::wstring h;
    for (const auto& [k, v] : headers) h += Widen(k) + L": " + Widen(v) + L"\r\n";
    LPVOID data = body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data());
    if (!WinHttpSendRequest(req, h.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : h.c_str(), DWORD(-1), data,
                            DWORD(body.size()), DWORD(body.size()), 0) ||
        !WinHttpReceiveResponse(req, nullptr)) {
        r.error = winError(GetLastError());
    } else {
        r.status = statusOf(req);
        r.retryAfter = retryAfterOf(req);
        char buf[16384];
        for (;;) {
            DWORD n = 0;
            if (!WinHttpReadData(req, buf, sizeof(buf), &n)) {
                r.error = winError(GetLastError());
                break;
            }
            if (n == 0) break;
            if (r.body.size() + n > maxBytes) {
                r.error = "response too large";
                break;
            }
            r.body.append(buf, n);
        }
        if (r.error.empty() && !r.ok()) r.error = "HTTP " + std::to_string(r.status);
    }
    WinHttpCloseHandle(req);
    WinHttpCloseHandle(conn);
    return r;
}

WebSocket::~WebSocket() { Close(); }

bool WebSocket::Connect(const std::string& host, int port, const std::string& path, std::string& error) {
    Close();
    HINTERNET session = WinHttpOpen(L"mango/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        error = "WinHTTP unavailable";
        return false;
    }
    WinHttpSetTimeouts(session, 3000, 3000, 3000, 0);  // receives wait for as long as it takes
    HINTERNET conn = WinHttpConnect(session, Widen(host).c_str(), INTERNET_PORT(port), 0);
    HINTERNET req = conn ? WinHttpOpenRequest(conn, L"GET", Widen(path).c_str(), nullptr, WINHTTP_NO_REFERER,
                                              WINHTTP_DEFAULT_ACCEPT_TYPES, 0)
                         : nullptr;
    HINTERNET ws = nullptr;
    if (req && WinHttpSetOption(req, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0) &&
        WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(req, nullptr)) {
        ws = WinHttpWebSocketCompleteUpgrade(req, 0);
    }
    if (!ws) error = winError(GetLastError());
    if (req) WinHttpCloseHandle(req);
    if (!ws) {
        if (conn) WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        return false;
    }
    std::lock_guard<std::mutex> lock(handleMu_);
    session_ = session;
    connect_ = conn;
    socket_ = ws;
    return true;
}

bool WebSocket::Send(const std::string& text) {
    std::lock_guard<std::mutex> lock(sendMu_);
    HINTERNET ws;
    {
        std::lock_guard<std::mutex> l(handleMu_);
        ws = socket_;
    }
    if (!ws) return false;
    return WinHttpWebSocketSend(ws, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, (PVOID)text.data(),
                                DWORD(text.size())) == NO_ERROR;
}

bool WebSocket::Receive(std::string& message) {
    message.clear();
    HINTERNET ws;
    {
        std::lock_guard<std::mutex> l(handleMu_);
        ws = socket_;
    }
    if (!ws) return false;
    char buf[16384];
    for (;;) {
        DWORD n = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
        if (WinHttpWebSocketReceive(ws, buf, sizeof(buf), &n, &type) != NO_ERROR) return false;
        if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) return false;
        message.append(buf, n);
        if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE || type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE)
            return true;
        if (message.size() > (32 << 20)) return false;
    }
}

void WebSocket::Close() {
    std::lock_guard<std::mutex> l(handleMu_);
    if (socket_) {
        WinHttpWebSocketClose(socket_, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
        WinHttpCloseHandle(socket_);
    }
    if (connect_) WinHttpCloseHandle(connect_);
    if (session_) WinHttpCloseHandle(session_);
    socket_ = connect_ = session_ = nullptr;
}

}  // namespace mango
