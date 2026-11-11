// Small helpers: text conversion, encoding, hashing, DPAPI, formatting.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mango {

std::wstring Widen(std::string_view s);
std::string Narrow(std::wstring_view s);

// UrlEncode percent-encodes everything but RFC 3986's unreserved characters.
std::string UrlEncode(std::string_view s);
// UrlDecode undoes UrlEncode (and turns + into a space, as forms do).
std::string UrlDecode(std::string_view s);
// QueryValue finds name's value in a query string such as "a=1&b=2".
std::string QueryValue(std::string_view query, std::string_view name);

std::string Base64(std::string_view bytes, bool url = false);  // url: base64url without padding
std::string Unbase64(std::string_view text);

std::string Sha256(std::string_view data);  // raw 32 bytes
std::string RandomBytes(size_t n);

// Protect and Unprotect wrap DPAPI (this Windows user only), as base64.
std::string Protect(const std::string& plain);
std::string Unprotect(const std::string& sealed);

// Duration gives m:ss, or h:mm:ss from an hour.
std::string Duration(int64_t ms);
// LongDuration gives "1 hr 12 min" or "42 min".
std::string LongDuration(int64_t ms);

// NowMs is the wall clock in Unix milliseconds.
int64_t NowMs();

// Lower folds ASCII letters; enough for matching.
std::string Lower(std::string s);

}  // namespace mango
