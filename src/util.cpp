#include "util.h"

#include <windows.h>
#include <bcrypt.h>
#include <dpapi.h>

#include <cstdio>

namespace mango {

std::wstring Widen(std::string_view s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

std::string Narrow(std::wstring_view w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::string UrlEncode(std::string_view s) {
    static const char* hexd = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~') {
            out += char(c);
        } else {
            out += '%';
            out += hexd[c >> 4];
            out += hexd[c & 15];
        }
    }
    return out;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string UrlDecode(std::string_view s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '+') {
            out += ' ';
        } else if (s[i] == '%' && i + 2 < s.size() && hexval(s[i + 1]) >= 0 && hexval(s[i + 2]) >= 0) {
            out += char(hexval(s[i + 1]) * 16 + hexval(s[i + 2]));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

std::string QueryValue(std::string_view query, std::string_view name) {
    size_t i = 0;
    while (i <= query.size()) {
        size_t amp = query.find('&', i);
        if (amp == std::string_view::npos) amp = query.size();
        std::string_view part = query.substr(i, amp - i);
        size_t eq = part.find('=');
        std::string_view key = part.substr(0, eq);
        if (key == name) return eq == std::string_view::npos ? std::string() : UrlDecode(part.substr(eq + 1));
        i = amp + 1;
    }
    return {};
}

std::string Base64(std::string_view bytes, bool url) {
    const char* table = url ? "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"
                            : "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    for (; i + 2 < bytes.size(); i += 3) {
        unsigned v = (unsigned char)bytes[i] << 16 | (unsigned char)bytes[i + 1] << 8 | (unsigned char)bytes[i + 2];
        out += table[v >> 18];
        out += table[(v >> 12) & 63];
        out += table[(v >> 6) & 63];
        out += table[v & 63];
    }
    if (i < bytes.size()) {
        unsigned v = (unsigned char)bytes[i] << 16;
        if (i + 1 < bytes.size()) v |= (unsigned char)bytes[i + 1] << 8;
        out += table[v >> 18];
        out += table[(v >> 12) & 63];
        if (i + 1 < bytes.size()) out += table[(v >> 6) & 63];
        else if (!url) out += '=';
        if (!url) out += '=';
    }
    return out;
}

std::string Unbase64(std::string_view text) {
    std::string out;
    unsigned v = 0;
    int bits = 0;
    for (char c : text) {
        int d;
        if (c >= 'A' && c <= 'Z') d = c - 'A';
        else if (c >= 'a' && c <= 'z') d = c - 'a' + 26;
        else if (c >= '0' && c <= '9') d = c - '0' + 52;
        else if (c == '+' || c == '-') d = 62;
        else if (c == '/' || c == '_') d = 63;
        else continue;
        v = v << 6 | d;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += char((v >> bits) & 0xFF);
        }
    }
    return out;
}

std::string Sha256(std::string_view data) {
    std::string out(32, '\0');
    BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, (PUCHAR)data.data(), ULONG(data.size()), (PUCHAR)out.data(), 32);
    return out;
}

std::string RandomBytes(size_t n) {
    std::string out(n, '\0');
    BCryptGenRandom(nullptr, (PUCHAR)out.data(), ULONG(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return out;
}

std::string Protect(const std::string& plain) {
    if (plain.empty()) return {};
    DATA_BLOB in{DWORD(plain.size()), (BYTE*)plain.data()}, out{};
    if (!CryptProtectData(&in, L"mango", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return {};
    std::string s((char*)out.pbData, out.cbData);
    LocalFree(out.pbData);
    return Base64(s);
}

std::string Unprotect(const std::string& sealed) {
    if (sealed.empty()) return {};
    std::string raw = Unbase64(sealed);
    DATA_BLOB in{DWORD(raw.size()), (BYTE*)raw.data()}, out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return {};
    std::string s((char*)out.pbData, out.cbData);
    LocalFree(out.pbData);
    return s;
}

std::string Duration(int64_t ms) {
    if (ms < 0) ms = 0;
    int64_t s = ms / 1000;
    char buf[32];
    if (s >= 3600) std::snprintf(buf, sizeof(buf), "%d:%02d:%02d", int(s / 3600), int(s / 60 % 60), int(s % 60));
    else std::snprintf(buf, sizeof(buf), "%d:%02d", int(s / 60), int(s % 60));
    return buf;
}

std::string LongDuration(int64_t ms) {
    int64_t min = (ms + 30000) / 60000;
    char buf[48];
    if (min >= 60) std::snprintf(buf, sizeof(buf), "%d hr %d min", int(min / 60), int(min % 60));
    else std::snprintf(buf, sizeof(buf), "%d min", int(min));
    return buf;
}

int64_t NowMs() {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    ULARGE_INTEGER u{ft.dwLowDateTime, ft.dwHighDateTime};
    return int64_t(u.QuadPart / 10000) - 11644473600000LL;
}

std::string Lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

}  // namespace mango
