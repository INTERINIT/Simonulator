// SimonWeber dial-up HTTP gateway.
//
// The handset sends HTTP request/response bytes through its real COM2 modem
// interface. This endpoint is a serial web proxy, not a PPP implementation.
// HTTPS and DNS terminate here; the 2400 bit/s handset link is plaintext.
#pragma once

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace simon_weber {

constexpr std::size_t kRequestHeaderLimit = 2048;
constexpr std::size_t kPostBodyLimit = 1018;
constexpr std::size_t kRequestLimit = 2048;
constexpr std::size_t kDefaultBodyLimit = 8192;
constexpr std::size_t kMaximumBodyLimit = 32768;

inline std::string lower(std::string text) {
    for (char &ch : text) if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
    return text;
}

inline std::string clean(std::string text) {
    while (!text.empty() && (text.back() == '\r' || text.back() == ' ' || text.back() == '\t')) text.pop_back();
    const auto start = text.find_first_not_of(" \t");
    return start == std::string::npos ? std::string() : text.substr(start);
}

inline std::string response(unsigned status, std::string reason, const std::string &body,
        std::string content_type = "text/plain; charset=us-ascii", bool truncated = false,
        const std::string &final_url = {}) {
    // Content-Length is always authoritative. Keeping the serial bearer open
    // permits a second GET without redialing; no chunked encoding is exposed.
    return "HTTP/1.0 " + std::to_string(status) + " " + reason + "\r\n"
        "Content-Type: " + content_type + "\r\nContent-Length: " + std::to_string(body.size()) +
        "\r\nConnection: keep-alive\r\nX-Simon-Gateway: 1\r\n" +
        (truncated ? "X-Simon-Truncated: yes\r\n" : "") +
        (final_url.empty() ? "" : "X-Simon-Final-URL: " + final_url + "\r\n") + "\r\n" + body;
}

enum class FrameState { Incomplete, Complete, Error };
struct RequestFrame {
    FrameState state = FrameState::Incomplete;
    unsigned error = 0;
    std::string method, url, body;
    std::size_t response_limit = kDefaultBodyLimit;
};

inline bool decimal_value(const std::string &text, std::size_t &value) {
    if (text.empty()) return false;
    value = 0;
    for (unsigned char ch : text) {
        if (ch < '0' || ch > '9' || value > (SIZE_MAX - (ch - '0')) / 10) return false;
        value = value * 10 + (ch - '0');
    }
    return true;
}

// One shared, strict HTTP/1.x boundary checker for both modem bearers and
// the worker. A POST is not eligible for submission until EVERY declared
// body byte has arrived. Pipelining, ambiguous framing and chunking are not
// supported. Error responses contain no input bytes, URLs or form values.
inline RequestFrame inspect_request(const std::string &input) {
    RequestFrame result;
    auto fail = [&](unsigned status) {
        result.state = FrameState::Error;
        result.error = status;
        return result;
    };
    if (input.size() > kRequestLimit) return fail(413);
    if (input.find('\0') != std::string::npos) return fail(400);
    const auto end = input.find("\r\n\r\n");
    if (end == std::string::npos) {
        if (input.size() >= kRequestHeaderLimit) return fail(413);
        return result;
    }
    if (end + 4 > kRequestHeaderLimit) return fail(413);
    const auto first_end = input.find("\r\n");
    std::istringstream first(input.substr(0, first_end));
    std::string version, extra;
    first >> result.method >> result.url >> version >> extra;
    if (result.method != "GET" && result.method != "POST") return fail(405);
    if (!extra.empty() || result.url.empty() || result.url.size() > 1024 ||
            (version != "HTTP/1.0" && version != "HTTP/1.1")) return fail(400);
    if (std::any_of(input.begin(), input.begin() + first_end,
            [](unsigned char c) { return c < 32 || c > 126; })) return fail(400);
    std::size_t length = 0;
    bool have_length = false, have_type = false, have_limit = false;
    std::string content_type;
    for (std::size_t at = first_end + 2; at < end;) {
        const auto next = input.find("\r\n", at);
        if (next == std::string::npos || next > end) return fail(400);
        const std::string line = input.substr(at, next - at);
        const auto colon = line.find(':');
        if (!colon || colon == std::string::npos) return fail(400);
        const std::string name = lower(line.substr(0, colon));
        if (std::any_of(name.begin(), name.end(), [](unsigned char c) {
                return !((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-');
            })) return fail(400);
        if (std::any_of(line.begin() + colon + 1, line.end(), [](unsigned char c) {
                return (c < 32 && c != '\t') || c > 126;
            })) return fail(400);
        const std::string value = clean(line.substr(colon + 1));
        if (name == "transfer-encoding") return fail(400);
        if (name == "content-length") {
            if (have_length || !decimal_value(value, length)) return fail(400);
            have_length = true;
            if (length > kPostBodyLimit) return fail(413);
        } else if (name == "content-type") {
            if (have_type) return fail(400);
            have_type = true;
            content_type = lower(clean(value.substr(0, value.find(';'))));
        } else if (name == "x-simon-max-bytes") {
            std::size_t requested = 0;
            if (have_limit || !decimal_value(value, requested)) return fail(400);
            have_limit = true;
            result.response_limit = std::clamp<std::size_t>(requested, 512, kMaximumBodyLimit);
        }
        at = next + 2;
    }
    if (result.method == "GET" && length) return fail(400);
    if (result.method == "POST") {
        if (!have_length) return fail(411);
        if (content_type != "application/x-www-form-urlencoded") return fail(415);
    }
    const std::size_t total = end + 4 + length;
    if (total > kRequestLimit) return fail(413);
    if (input.size() < total) return result;
    if (input.size() != total) return fail(400);
    result.body = input.substr(end + 4, length);
    if (std::any_of(result.body.begin(), result.body.end(),
            [](unsigned char c) { return c < 32 || c > 126; })) return fail(400);
    result.state = FrameState::Complete;
    return result;
}

inline std::string framing_error(unsigned status) {
    const char *reason = status == 413 ? "Request Too Large" : status == 411 ? "Length Required" :
        status == 415 ? "Unsupported Media Type" : status == 405 ? "Method Not Allowed" : "Bad Request";
    return response(status, reason, "Malformed or unsupported HTTP request.\r\n");
}

struct InternetHandle {
    HINTERNET value = nullptr;
    explicit InternetHandle(HINTERNET handle) : value(handle) {}
    ~InternetHandle() { if (value) WinHttpCloseHandle(value); }
    InternetHandle(const InternetHandle &) = delete;
    InternetHandle &operator=(const InternetHandle &) = delete;
    operator HINTERNET() const { return value; }
};

// WinHTTP owns cookie parsing and its domain/path/Secure/expiry rules. A
// separate session handle per handset prevents one phone inheriting another
// phone's login. These cookies are memory-only; no browser profile is read.
struct WebSession {
    InternetHandle handle{WinHttpOpen(L"SimonWeber-Gateway/0.2", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
    WebSession() { if (handle) WinHttpSetTimeouts(handle, 4000, 4000, 4000, 6000); }
};

inline std::string final_url_for_guest(std::string url) {
    // Do not turn an HTML GET-login password/token into a displayed or saved
    // page address. Relative path resolution needs the final directory, not
    // a sensitive query. This header is navigation metadata, never a log.
    if (const auto fragment = url.find('#'); fragment != std::string::npos) url.resize(fragment);
    const auto query = url.find('?');
    if (query != std::string::npos) {
        const std::string fields = lower(url.substr(query + 1));
        std::size_t start = 0;
        while (start < fields.size()) {
            const auto end = fields.find('&', start);
            const std::string field = fields.substr(start, end - start);
            const std::string name = field.substr(0, field.find('='));
            // Percent-encoded names are uncommon for static links and can
            // disguise a credential name. Conservatively hide their query.
            if (name.find('%') != std::string::npos || name.find("pass") != std::string::npos ||
                    name.find("token") != std::string::npos || name.find("secret") != std::string::npos ||
                    name == "pwd" || name == "auth" || name == "authorization" ||
                    name == "session" || name == "sessionid" || name == "sid" || name == "key" ||
                    name == "account" || name == "username" || name == "email") {
                url.resize(query);
                break;
            }
            if (end == std::string::npos) break;
            start = end + 1;
        }
    }
    // Request URLs are bounded ASCII. Never allow a remote response URL to
    // inject another header. Its complete header line must fit in 255 bytes.
    if (url.size() > 230 || std::any_of(url.begin(), url.end(), [](unsigned char c) { return c < 33 || c > 126; }))
        return {};
    return url;
}

inline std::string query_final_url(HINTERNET request) {
    DWORD size = 0;
    WinHttpQueryOption(request, WINHTTP_OPTION_URL, nullptr, &size);
    if (!size || size > 8192) return {};
    std::vector<wchar_t> wide(size / sizeof(wchar_t) + 1, L'\0');
    if (!WinHttpQueryOption(request, WINHTTP_OPTION_URL, wide.data(), &size)) return {};
    std::string result;
    for (wchar_t ch : wide) {
        if (!ch) break;
        if (ch < 33 || ch > 126) return {};
        result.push_back(char(ch));
    }
    return final_url_for_guest(result);
}

inline std::string charset_parameter(const std::string &value) {
    const std::string text = lower(value);
    auto at = text.find("charset");
    if (at == std::string::npos) return {};
    at += 7;
    while (at < text.size() && (text[at] == ' ' || text[at] == '\t')) ++at;
    if (at == text.size() || text[at++] != '=') return {};
    while (at < text.size() && (text[at] == ' ' || text[at] == '\t' || text[at] == '\'' || text[at] == '"')) ++at;
    const auto end = text.find_first_of(" ;\t\r\n\"'>/", at);
    return text.substr(at, end - at);
}

inline bool normalize_text(std::string &body, std::string &type, std::size_t limit, bool &truncated) {
    const std::string media = lower(clean(type.substr(0, type.find(';'))));
    std::string charset = charset_parameter(type);
    if (body.starts_with("\xef\xbb\xbf")) { body.erase(0, 3); charset = "utf-8"; }
    if (charset.empty() && (media == "text/html" || media == "application/xhtml+xml")) {
        // Legacy GB2312 pages commonly specify only an HTML meta charset.
        // Inspect tags as data; scripts and other remote code are never run.
        const std::string head = lower(body.substr(0, 4096));
        std::size_t at = 0;
        while ((at = head.find("<meta", at)) != std::string::npos) {
            const auto end = head.find('>', at);
            if (end == std::string::npos) break;
            charset = charset_parameter(head.substr(at, end - at));
            if (!charset.empty()) break;
            at = end + 1;
        }
    }
    UINT codepage = 0;
    if (charset.empty() || charset == "utf-8" || charset == "utf8") codepage = CP_UTF8;
    else if (charset == "gb2312" || charset == "gbk" || charset == "cp936" || charset == "x-gbk") codepage = 936;
    else if (charset == "gb18030") codepage = 54936;
    else if (charset == "us-ascii" || charset == "ascii") codepage = CP_UTF8;
    if (codepage && !body.empty()) {
        int wide_size = 0;
        // A raw-byte read bound can land midway through a UTF-8 or GBK
        // character. Drop only an incomplete tail of a truncated page;
        // reject malformed full pages rather than silently mangling text.
        for (unsigned tail = 0; tail < 4; ++tail) {
            wide_size = MultiByteToWideChar(codepage, MB_ERR_INVALID_CHARS, body.data(), int(body.size()), nullptr, 0);
            if (wide_size || !truncated || body.empty()) break;
            body.pop_back();
        }
        if (!wide_size) return false;
        std::wstring wide(wide_size, L'\0');
        if (!MultiByteToWideChar(codepage, MB_ERR_INVALID_CHARS, body.data(), int(body.size()), wide.data(), wide_size)) return false;
        const int utf8_size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), wide_size, nullptr, 0, nullptr, nullptr);
        if (!utf8_size) return false;
        body.resize(utf8_size);
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), wide_size, body.data(), utf8_size, nullptr, nullptr);
    }
    if (body.size() > limit) {
        std::size_t end = limit;
        if (codepage) while (end && (static_cast<unsigned char>(body[end]) & 0xc0) == 0x80) --end;
        body.resize(end);
        truncated = true;
    }
    // A JSON error/login response is data, not HTML. Preserve its readable
    // text and HTTP status so the guest does not mistake it for a media error.
    const bool json = media == "application/json" || media.ends_with("+json");
    type = (json ? "text/plain" : media) + (codepage ? "; charset=utf-8" : charset.empty() ? "" : "; charset=" + charset);
    return true;
}

inline std::string query_header(HINTERNET request, DWORD name) {
    std::array<wchar_t, 1024> text{};
    DWORD size = DWORD(text.size() * sizeof(wchar_t));
    if (!WinHttpQueryHeaders(request, name, WINHTTP_HEADER_NAME_BY_INDEX, text.data(), &size, WINHTTP_NO_HEADER_INDEX)) return {};
    std::string result;
    for (wchar_t ch : text) {
        if (!ch) break;
        // Header values forwarded to the small guest parser are ASCII only.
        if (ch >= 32 && ch <= 126) result.push_back(char(ch));
    }
    return result;
}

struct HttpTarget {
    bool secure = false;
    INTERNET_PORT port = 0;
    std::wstring host;
    std::wstring path;
    std::string origin;
    std::string path_and_query;
};

inline bool parse_http_target(const std::string &url, HttpTarget &target) {
    if (url.empty() || url.size() > 1024 || std::any_of(url.begin(), url.end(),
            [](unsigned char c) { return c < 33 || c > 126 || c == '\\'; })) return false;
    const std::wstring wide(url.begin(), url.end());
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = DWORD(-1);
    parts.dwUserNameLength = parts.dwPasswordLength = DWORD(-1);
    if (!WinHttpCrackUrl(wide.c_str(), DWORD(wide.size()), 0, &parts) ||
            (parts.nScheme != INTERNET_SCHEME_HTTP && parts.nScheme != INTERNET_SCHEME_HTTPS) ||
            !parts.dwHostNameLength || parts.dwUserNameLength || parts.dwPasswordLength) return false;
    const auto authority = url.find("://");
    if (authority == std::string::npos) return false;
    const auto path_start = url.find_first_of("/?#", authority + 3);
    target.secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
    target.port = parts.nPort;
    target.host.assign(parts.lpszHostName, parts.dwHostNameLength);
    target.origin = url.substr(0, path_start);
    target.path.assign(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (target.path.empty()) target.path = L"/";
    if (parts.dwExtraInfoLength) target.path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    if (const auto fragment = target.path.find(L'#'); fragment != std::wstring::npos) target.path.resize(fragment);
    target.path_and_query.assign(target.path.begin(), target.path.end());
    return true;
}

inline bool same_http_origin(const std::string &one, const std::string &two) {
    HttpTarget a, b;
    return parse_http_target(one, a) && parse_http_target(two, b) && a.secure == b.secure && a.port == b.port &&
        lower(std::string(a.host.begin(), a.host.end())) == lower(std::string(b.host.begin(), b.host.end()));
}

inline std::string remove_dot_segments(std::string input) {
    // RFC 3986 section 5.2.4, operating on URI bytes, not percent-decoded
    // data. The server must still distinguish literal %2e and slash bytes.
    std::string output;
    auto remove_last = [&] {
        const auto slash = output.rfind('/');
        output.resize(slash == std::string::npos ? 0 : slash);
    };
    while (!input.empty()) {
        if (input.starts_with("../")) input.erase(0, 3);
        else if (input.starts_with("./")) input.erase(0, 2);
        else if (input.starts_with("/./")) input.erase(0, 2);
        else if (input == "/.") input = "/";
        else if (input.starts_with("/../")) { input.erase(0, 3); remove_last(); }
        else if (input == "/..") { input = "/"; remove_last(); }
        else if (input == "." || input == "..") input.clear();
        else {
            const auto slash = input.find('/', input.front() == '/' ? 1 : 0);
            output += input.substr(0, slash);
            if (slash == std::string::npos) input.clear();
            else input.erase(0, slash);
        }
    }
    return output;
}

inline bool redirect_target(const std::string &current_url, std::string location, std::string &next_url) {
    HttpTarget current, next;
    if (!parse_http_target(current_url, current) || location.empty() || location.size() > 1024 ||
            std::any_of(location.begin(), location.end(),
                [](unsigned char c) { return c < 33 || c > 126 || c == '\\'; })) return false;
    if (const auto fragment = location.find('#'); fragment != std::string::npos) location.resize(fragment);
    const auto colon = location.find(':');
    const auto first_path = location.find_first_of("/?");
    if (colon != std::string::npos && (first_path == std::string::npos || colon < first_path))
        next_url = location; // An explicit scheme is validated below, including userinfo rejection.
    else if (location.starts_with("//"))
        next_url = std::string(current.secure ? "https:" : "http:") + location;
    else {
        std::string path = current.path_and_query;
        const auto query = path.find('?');
        const std::string base_path = path.substr(0, query);
        if (location.empty()) next_url = current.origin + path;
        else if (location.front() == '?') next_url = current.origin + base_path + location;
        else {
            const auto new_query = location.find('?');
            std::string new_path = location.substr(0, new_query);
            if (new_path.front() != '/') new_path = base_path.substr(0, base_path.rfind('/') + 1) + new_path;
            next_url = current.origin + remove_dot_segments(new_path);
            if (new_query != std::string::npos) next_url += location.substr(new_query);
        }
    }
    if (!parse_http_target(next_url, next)) return false;
    if (current.secure && !next.secure) {
        // Some old applications construct an absolute http:// Location even
        // when accessed through their HTTPS front end. Never contact that
        // insecure URL: retry its NEW path/query on the SAME default TLS
        // origin. No original login query is copied into an unrelated path.
        // Cross-host targets or custom ports are not presumed equivalent.
        if (current.port != INTERNET_DEFAULT_HTTPS_PORT || next.port != INTERNET_DEFAULT_HTTP_PORT ||
                lower(std::string(current.host.begin(), current.host.end())) !=
                lower(std::string(next.host.begin(), next.host.end()))) return false;
        std::string host(next.host.begin(), next.host.end());
        if (host.find(':') != std::string::npos && !host.starts_with("[")) host = "[" + host + "]";
        next_url = "https://" + host + next.path_and_query;
        if (!parse_http_target(next_url, next)) return false;
    }
    return true;
}

inline std::string query_location(HINTERNET request) {
    // Unlike display headers, redirect targets must never be repaired by
    // deleting malformed/control bytes: reject the complete value instead.
    std::array<wchar_t, 1025> text{};
    DWORD size = DWORD(text.size() * sizeof(wchar_t));
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
            text.data(), &size, WINHTTP_NO_HEADER_INDEX)) return {};
    std::string result;
    for (wchar_t ch : text) {
        if (!ch) break;
        if (ch < 33 || ch > 126) return {};
        result.push_back(char(ch));
    }
    return result;
}

inline std::string fetch(const std::string &wire_request, WebSession *persistent_session = nullptr) {
    const RequestFrame frame = inspect_request(wire_request);
    if (frame.state != FrameState::Complete) return framing_error(frame.error ? frame.error : 400);
    const std::string &url = frame.url;
    const std::size_t limit = frame.response_limit;
    const std::wstring wide_url(url.begin(), url.end());
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = DWORD(-1);
    parts.dwUserNameLength = parts.dwPasswordLength = DWORD(-1);
    if (!WinHttpCrackUrl(wide_url.c_str(), DWORD(wide_url.size()), 0, &parts) ||
            (parts.nScheme != INTERNET_SCHEME_HTTP && parts.nScheme != INTERNET_SCHEME_HTTPS) || !parts.dwHostNameLength)
        return response(400, "Bad Request", "Only absolute http:// or https:// URLs are supported.\r\n");
    if (parts.dwUserNameLength || parts.dwPasswordLength)
        return response(400, "Bad Request", "Credentials in URLs are not supported.\r\n");
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (path.empty()) path = L"/";
    path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    if (const auto fragment = path.find(L'#'); fragment != std::wstring::npos) path.resize(fragment);

    // Deterministic linked pages exercise the complete guest UART/browser
    // path even when the laboratory host has no Internet connection.
    if (lower(std::string(host.begin(), host.end())) == "simon.local") {
        if (frame.method != "GET") return framing_error(405);
        std::string body;
        unsigned status = 200;
        if (path == L"/" || path == L"/index.html")
            body = "<html><head><title>SimonWeber Home</title></head><body>"
                "<h1>SimonWeber</h1><p>Welcome to the Simon web gateway.</p>"
                "<p>This page arrived through the 2400 bit/s modem.</p>"
                "<p><a href=\"http://simon.local/about\">About this browser</a></p>"
                "<p><a href=\"http://simon.local/test\">Link test</a></p></body></html>";
        else if (path == L"/about")
            body = "<html><head><title>About SimonWeber</title></head><body>"
                "<h1>About SimonWeber</h1><p>NAV native controls and a guest-side HTML reader.</p>"
                "<p>Dial-up HTTP gateway; TLS terminates at the gateway.</p>"
                "<a href=\"http://simon.local/\">Home</a></body></html>";
        else if (path == L"/test")
            body = "<html><head><title>Link test</title></head><body><h1>Link works</h1>"
                "<p>1 + 2 = 3. Ampersand: &amp; less than: &lt;.</p>"
                "<a href=\"/\">Home</a></body></html>";
        else { status = 404; body = "<html><body><h1>Page not found</h1><a href=\"/\">Home</a></body></html>"; }
        const bool truncated = body.size() > limit;
        if (truncated) body.resize(limit);
        return response(status, status == 200 ? "OK" : "Not Found", body, "text/html; charset=us-ascii", truncated,
            final_url_for_guest(url));
    }

    // All potentially blocking DNS/TLS/HTTP work runs on Gateway's worker.
    // No certificate validation is bypassed. Only this handset's in-memory
    // cookie jar is used; no host/browser credentials or guest headers pass.
    std::unique_ptr<WebSession> temporary_session;
    if (!persistent_session) { temporary_session = std::make_unique<WebSession>(); persistent_session = temporary_session.get(); }
    if (!persistent_session->handle) return response(502, "Bad Gateway", "Cannot open HTTP session.\r\n");
    std::string request_url = url;
    std::string request_method = frame.method;
    std::string request_body = frame.body;
    const auto start = std::chrono::steady_clock::now();
    for (unsigned redirects = 0;; ++redirects) {
    if (std::chrono::steady_clock::now() - start > std::chrono::seconds(15))
        return response(504, "Gateway Timeout", "Page transfer exceeded the gateway deadline.\r\n");
    HttpTarget target;
    if (!parse_http_target(request_url, target))
        return response(502, "Bad Gateway", "Invalid web redirect target.\r\n");
    InternetHandle connection(WinHttpConnect(persistent_session->handle, target.host.c_str(), target.port, 0));
    if (!connection) return response(502, "Bad Gateway", "Cannot connect to web host.\r\n");
    const wchar_t *accept[] = {L"text/html", L"text/plain", L"application/xhtml+xml", L"application/json", nullptr};
    const std::wstring wide_method(request_method.begin(), request_method.end());
    InternetHandle request(WinHttpOpenRequest(connection, wide_method.c_str(), target.path.c_str(), nullptr, WINHTTP_NO_REFERER,
        accept, target.secure ? WINHTTP_FLAG_SECURE : 0));
    if (!request) return response(502, "Bad Gateway", "Cannot create web request.\r\n");
    DWORD auto_logon = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
    // All redirects are handled above the HTTP API so the complete chain
    // has one hop limit and HTTPS can never silently become plaintext.
    DWORD disable = WINHTTP_DISABLE_AUTHENTICATION | WINHTTP_DISABLE_REDIRECTS;
    // REDIRECT_POLICY is also NEVER: setting a permissive policy after
    // DISABLE_REDIRECTS would re-enable WinHTTP's internal redirect engine.
    DWORD redirect_policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    if (!WinHttpSetOption(request, WINHTTP_OPTION_AUTOLOGON_POLICY, &auto_logon, sizeof(auto_logon)) ||
            !WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE, &disable, sizeof(disable)) ||
            !WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redirect_policy, sizeof(redirect_policy)))
        return response(502, "Bad Gateway", "Cannot configure secure HTTP policy.\r\n");
    std::wstring headers = L"Accept-Encoding: identity\r\nAccept-Language: en-US,en;q=0.9\r\n";
    if (request_method == "POST") headers += L"Content-Type: application/x-www-form-urlencoded\r\n";
    // No retry on transport failures: the origin may already have applied
    // a POST before the carrier/response failed. The user must decide next.
    if (!WinHttpSendRequest(request, headers.c_str(), DWORD(-1),
            request_body.empty() ? WINHTTP_NO_REQUEST_DATA : request_body.data(),
            DWORD(request_body.size()), DWORD(request_body.size()), 0) ||
            !WinHttpReceiveResponse(request, nullptr))
        return response(502, "Bad Gateway", "Web request failed (WinHTTP " + std::to_string(GetLastError()) + ").\r\n");
    DWORD status = 502, size = sizeof(status);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
    if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
        std::string redirected_url;
        if (redirects >= 4)
            return response(508, "Loop Detected", "Web redirect limit exceeded.\r\n");
        if (!redirect_target(request_url, query_location(request), redirected_url))
            return response(502, "Bad Gateway", "Unsafe or invalid web redirect rejected.\r\n");
        if (request_method == "POST") {
            if (status == 307 || status == 308) {
                if (!same_http_origin(request_url, redirected_url))
                    return response(502, "Bad Gateway", "Cross-origin POST redirect rejected.\r\n");
            } else {
                // Browser-compatible Post/Redirect/Get: never forward form
                // bytes or Content-Type after a 301, 302 or 303 response.
                request_method = "GET";
                request_body.clear();
            }
        }
        request_url = std::move(redirected_url);
        continue;
    }
    std::string type = query_header(request, WINHTTP_QUERY_CONTENT_TYPE);
    if (type.empty()) type = "text/plain";
    const std::string type_lower = lower(clean(type.substr(0, type.find(';'))));
    if (!type_lower.starts_with("text/") && type_lower != "application/xhtml+xml" &&
            type_lower != "application/json" && !type_lower.ends_with("+json"))
        return response(415, "Unsupported Media Type", "This browser accepts text and HTML pages only.\r\n");
    const std::string encoding = lower(query_header(request, WINHTTP_QUERY_CONTENT_ENCODING));
    if (!encoding.empty() && encoding != "identity")
        return response(502, "Bad Gateway", "Server ignored the uncompressed-content request.\r\n");
    std::string body;
    std::array<char, 2048> buffer{};
    bool truncated = false;
    for (;;) {
        if (std::chrono::steady_clock::now() - start > std::chrono::seconds(15))
            return response(504, "Gateway Timeout", "Page transfer exceeded the gateway deadline.\r\n");
        DWORD received = 0;
        const DWORD count = DWORD(std::min(buffer.size(), limit + 1 - body.size()));
        if (!WinHttpReadData(request, buffer.data(), count, &received))
            return response(502, "Bad Gateway", "Web response ended unexpectedly.\r\n");
        if (!received) break;
        body.append(buffer.data(), received);
        if (body.size() > limit) { truncated = true; break; }
    }
    if (!normalize_text(body, type, limit, truncated))
        return response(502, "Bad Gateway", "Malformed page character encoding.\r\n");
    std::string reason = query_header(request, WINHTTP_QUERY_STATUS_TEXT);
    if (reason.empty()) reason = status == 200 ? "OK" : "Remote Response";
    return response(status, reason, body, type, truncated, query_final_url(request));
    }
}

struct Completion {
    std::uint64_t id = 0;
    std::string response;
};

class Gateway {
public:
    Gateway() : m_worker([this] { work(); }) {}
    ~Gateway() {
        { std::lock_guard lock(m_mutex); m_stopping = true; m_jobs.clear(); }
        m_ready.notify_one();
        m_worker.join();
    }
    bool submit(std::uint64_t id, std::string request, std::uint64_t session_key = 0) {
        std::lock_guard lock(m_mutex);
        if (m_jobs.size() >= 8 || m_stopping) return false;
        m_jobs.push_back({id, std::move(request), session_key, false});
        m_ready.notify_one();
        return true;
    }
    void forget(std::uint64_t session_key) {
        // Queue behind existing requests, including an in-flight request,
        // so a late completion cannot re-create a disconnected client's jar.
        std::lock_guard lock(m_mutex);
        if (m_stopping) return;
        m_jobs.push_back({0, {}, session_key, true});
        m_ready.notify_one();
    }
    std::vector<Completion> drain() {
        std::lock_guard lock(m_mutex);
        std::vector<Completion> result;
        result.swap(m_completed);
        return result;
    }
private:
    struct Job { std::uint64_t id; std::string request; std::uint64_t session_key; bool forget; };
    struct SessionEntry { std::unique_ptr<WebSession> session; std::uint64_t used; };
    std::map<std::uint64_t, SessionEntry> m_sessions; // Worker thread only.
    std::uint64_t m_session_clock = 0;
    std::mutex m_mutex;
    std::condition_variable m_ready;
    std::deque<Job> m_jobs;
    std::vector<Completion> m_completed;
    bool m_stopping = false;
    std::thread m_worker;
    void work() {
        for (;;) {
            Job job;
            {
                std::unique_lock lock(m_mutex);
                m_ready.wait(lock, [this] { return m_stopping || !m_jobs.empty(); });
                if (m_stopping) return;
                job = std::move(m_jobs.front());
                m_jobs.pop_front();
            }
            if (job.forget) { m_sessions.erase(job.session_key); continue; }
            std::string reply;
            try {
                if (!m_sessions.contains(job.session_key)) {
                    // Bound anonymous client churn without sharing sessions.
                    if (m_sessions.size() >= 64) {
                        const auto oldest = std::min_element(m_sessions.begin(), m_sessions.end(),
                            [](const auto &a, const auto &b) { return a.second.used < b.second.used; });
                        m_sessions.erase(oldest);
                    }
                    m_sessions.emplace(job.session_key, SessionEntry{std::make_unique<WebSession>(), 0});
                }
                auto &session = m_sessions.at(job.session_key);
                session.used = ++m_session_clock;
                reply = fetch(job.request, session.session.get());
            }
            catch (...) { reply = response(500, "Gateway Error", "Unable to process this page.\r\n"); }
            std::lock_guard lock(m_mutex);
            m_completed.push_back({job.id, std::move(reply)});
        }
    }
};

} // namespace simon_weber
