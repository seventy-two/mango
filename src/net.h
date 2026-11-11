// HTTP and WebSocket over WinHTTP.
#pragma once

#include <windows.h>
#include <winhttp.h>

#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace mango {

struct HttpResult {
    unsigned long status = 0;  // 0 if the request failed before a response
    std::string body;
    std::string error;
    int retryAfter = 0;  // seconds, from a 429's Retry-After
    bool ok() const { return status >= 200 && status < 300; }
};

using Headers = std::vector<std::pair<std::string, std::string>>;

// Http is a WinHTTP session; requests are synchronous and may run on
// several threads at once.
class Http {
public:
    Http();
    ~Http();
    Http(const Http&) = delete;
    Http& operator=(const Http&) = delete;

    HttpResult Request(const std::string& method, const std::string& url, const Headers& headers = {},
                       const std::string& body = {}, size_t maxBytes = 16 << 20);

private:
    HINTERNET session_ = nullptr;
};

// WebSocket is one client connection. Receive blocks; Send may be called
// from another thread meanwhile (WinHTTP allows one of each at once).
class WebSocket {
public:
    WebSocket() = default;
    ~WebSocket();
    WebSocket(const WebSocket&) = delete;
    WebSocket& operator=(const WebSocket&) = delete;

    bool Connect(const std::string& host, int port, const std::string& path, std::string& error);
    bool Send(const std::string& text);
    // Receive reads one whole text message; false when the connection ends.
    bool Receive(std::string& message);
    void Close();

private:
    std::mutex sendMu_;
    std::mutex handleMu_;
    HINTERNET session_ = nullptr, connect_ = nullptr, socket_ = nullptr;
};

}  // namespace mango
