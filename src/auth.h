// Spotify sign-in: authorisation code with PKCE, through the browser and a
// one-shot listener on 127.0.0.1, so no client secret is needed.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include "net.h"

namespace mango {

constexpr int kRedirectPort = 46075;
constexpr const char* kRedirectUri = "http://127.0.0.1:46075/callback";
constexpr const char* kScopes =
    "user-library-read user-library-modify playlist-read-private playlist-read-collaborative "
    "user-read-playback-state user-modify-playback-state user-read-private";

struct Tokens {
    std::string access;
    std::string refresh;
    int64_t expiresAt = 0;  // Unix ms
};

// AuthorizeUrl builds the page the browser opens.
std::string AuthorizeUrl(const std::string& clientId, const std::string& challenge, const std::string& state);
// Challenge is the S256 code challenge for a verifier.
std::string Challenge(const std::string& verifier);

struct Callback {
    std::string code, state, error;
    bool found = false;  // the request was for /callback
};
// ParseCallback reads the browser's request line, e.g.
// "GET /callback?code=abc&state=xyz HTTP/1.1".
Callback ParseCallback(const std::string& request);

// ParseTokens reads the token endpoint's JSON; refresh is kept if the
// response has none (Spotify doesn't always rotate it).
bool ParseTokens(const std::string& body, Tokens& t, std::string& error);

// SignIn opens the browser and waits (until cancel) for the redirect.
bool SignIn(Http& http, const std::string& clientId, Tokens& out, std::string& error, const std::atomic<bool>& cancel);
// Refresh swaps the refresh token for a new access token.
bool Refresh(Http& http, const std::string& clientId, Tokens& t, std::string& error);

}  // namespace mango
