#include "auth.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <shellapi.h>

#include "nlohmann/json.hpp"
#include "util.h"

namespace mango {

std::string Challenge(const std::string& verifier) { return Base64(Sha256(verifier), true); }

std::string AuthorizeUrl(const std::string& clientId, const std::string& challenge, const std::string& state) {
    return "https://accounts.spotify.com/authorize?response_type=code&client_id=" + UrlEncode(clientId) +
           "&scope=" + UrlEncode(kScopes) + "&redirect_uri=" + UrlEncode(kRedirectUri) +
           "&code_challenge_method=S256&code_challenge=" + challenge + "&state=" + UrlEncode(state);
}

Callback ParseCallback(const std::string& request) {
    Callback c;
    size_t sp = request.find(' ');
    if (sp == std::string::npos) return c;
    size_t end = request.find(' ', sp + 1);
    std::string target = request.substr(sp + 1, end == std::string::npos ? std::string::npos : end - sp - 1);
    size_t q = target.find('?');
    std::string path = target.substr(0, q);
    if (path != "/callback") return c;
    c.found = true;
    std::string query = q == std::string::npos ? "" : target.substr(q + 1);
    c.code = QueryValue(query, "code");
    c.state = QueryValue(query, "state");
    c.error = QueryValue(query, "error");
    return c;
}

bool ParseTokens(const std::string& body, Tokens& t, std::string& error) {
    try {
        auto j = nlohmann::json::parse(body);
        if (j.contains("error")) {
            error = j.value("error_description", j.value("error", std::string("sign-in refused")));
            return false;
        }
        t.access = j.value("access_token", "");
        if (j.contains("refresh_token")) t.refresh = j["refresh_token"].get<std::string>();
        t.expiresAt = NowMs() + int64_t(j.value("expires_in", 3600)) * 1000;
        if (t.access.empty()) {
            error = "no access token";
            return false;
        }
        return true;
    } catch (...) {
        error = "unreadable token response";
        return false;
    }
}

namespace {

bool tokenRequest(Http& http, const std::string& form, Tokens& t, std::string& error) {
    auto r = http.Request("POST", "https://accounts.spotify.com/api/token",
                          {{"Content-Type", "application/x-www-form-urlencoded"}}, form);
    if (r.status == 0) {
        error = r.error;
        return false;
    }
    return ParseTokens(r.body, t, error);
}

void reply(SOCKET s, const char* status, const std::string& message) {
    std::string html =
        "<!doctype html><meta charset=utf-8><title>Mango</title>"
        "<body style=\"font:16px 'Segoe UI',sans-serif;background:#15171C;color:#E6E8EC;display:grid;place-items:center;height:90vh\">"
        "<p>" + message + "</p>";
    std::string resp = std::string("HTTP/1.1 ") + status +
                       "\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\nContent-Length: " +
                       std::to_string(html.size()) + "\r\n\r\n" + html;
    send(s, resp.data(), int(resp.size()), 0);
}

}  // namespace

bool SignIn(Http& http, const std::string& clientId, Tokens& out, std::string& error, const std::atomic<bool>& cancel) {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(kRedirectPort);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (listener == INVALID_SOCKET || bind(listener, (sockaddr*)&addr, sizeof(addr)) != 0 || listen(listener, 4) != 0) {
        error = "port " + std::to_string(kRedirectPort) + " is in use";
        if (listener != INVALID_SOCKET) closesocket(listener);
        WSACleanup();
        return false;
    }
    std::string verifier = Base64(RandomBytes(48), true);
    std::string state = Base64(RandomBytes(12), true);
    std::wstring url = Widen(AuthorizeUrl(clientId, Challenge(verifier), state));
    ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);

    bool ok = false;
    error = "cancelled";
    while (!cancel) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(listener, &fds);
        timeval tv{0, 250000};
        if (select(0, &fds, nullptr, nullptr, &tv) <= 0) continue;
        SOCKET s = accept(listener, nullptr, nullptr);
        if (s == INVALID_SOCKET) continue;
        DWORD timeout = 3000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
        std::string req;
        char buf[4096];
        while (req.find("\r\n") == std::string::npos && req.size() < 16384) {
            int n = recv(s, buf, sizeof(buf), 0);
            if (n <= 0) break;
            req.append(buf, n);
        }
        Callback c = ParseCallback(req);
        if (!c.found) {
            reply(s, "404 Not Found", "Nothing here.");
            closesocket(s);
            continue;
        }
        if (!c.error.empty() || c.state != state || c.code.empty()) {
            error = c.error.empty() ? "sign-in didn't match; try again" : "Spotify said: " + c.error;
            reply(s, "400 Bad Request", "Mango couldn't sign in (" + error + "). You can close this tab.");
            closesocket(s);
            break;
        }
        std::string form = "grant_type=authorization_code&code=" + UrlEncode(c.code) +
                           "&redirect_uri=" + UrlEncode(kRedirectUri) + "&client_id=" + UrlEncode(clientId) +
                           "&code_verifier=" + verifier;
        ok = tokenRequest(http, form, out, error);
        reply(s, ok ? "200 OK" : "500 Internal Server Error",
              ok ? "Mango is signed in. You can close this tab." : "Mango couldn't sign in: " + error);
        closesocket(s);
        break;
    }
    closesocket(listener);
    WSACleanup();
    return ok;
}

bool Refresh(Http& http, const std::string& clientId, Tokens& t, std::string& error) {
    if (t.refresh.empty()) {
        error = "not signed in";
        return false;
    }
    std::string form = "grant_type=refresh_token&refresh_token=" + UrlEncode(t.refresh) + "&client_id=" + UrlEncode(clientId);
    return tokenRequest(http, form, t, error);
}

}  // namespace mango
