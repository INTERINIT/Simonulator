// Shared request framing checks. No sockets, accounts or origin requests.
#include "weber_gateway.h"
#include <iostream>

int main() {
    using namespace simon_weber;
    const std::string header = "POST https://example.test/chat?room=1 HTTP/1.0\r\n"
        "Content-Type: application/x-www-form-urlencoded\r\nContent-Length: 7\r\n\r\n";
    const std::string post = header + "a=local";
    for (std::size_t i = 0; i < post.size(); ++i)
        if (inspect_request(post.substr(0, i)).state != FrameState::Incomplete) return 1;
    const auto parsed = inspect_request(post);
    if (parsed.state != FrameState::Complete || parsed.method != "POST" || parsed.body != "a=local" ||
            parsed.url != "https://example.test/chat?room=1") return 2;
    if (inspect_request(post + "GET http://example.test/ HTTP/1.0\r\n\r\n").error != 400) return 3;
    const std::string start = "POST https://example.test/ HTTP/1.0\r\nContent-Type: application/x-www-form-urlencoded\r\n";
    for (const char *length : {"-1", "+3", "1,1", "9x", "99999999999999999999999"})
        if (inspect_request(start + "Content-Length: " + length + "\r\n\r\n").error != 400) return 4;
    for (const char *length : {"1019", "65535"})
        if (inspect_request(start + "Content-Length: " + length + "\r\n\r\n").error != 413) return 5;
    if (inspect_request(start + "Content-Length: 1018\r\n\r\n" + std::string(1018, 'a')).state != FrameState::Complete) return 6;
    if (inspect_request(start + "Content-Length: 0\r\nContent-Length: 0\r\n\r\n").error != 400) return 7;
    if (inspect_request(start + "Content-Length: 0\r\nTransfer-Encoding: chunked\r\n\r\n").error != 400) return 8;
    if (inspect_request(start + "\r\n").error != 411) return 9;
    if (inspect_request("POST http://example.test/ HTTP/1.0\r\nContent-Length: 0\r\n\r\n").error != 415) return 10;
    if (inspect_request("GET http://example.test/ HTTP/1.0\r\nContent-Length: 1\r\n\r\na").error != 400) return 11;
    if (inspect_request("GET http://example.test/ HTTP/1.0\r\nBadHeader\r\n\r\n").error != 400) return 12;
    if (inspect_request(std::string(2048, 'a')).error != 413) return 13;
    std::cout << "PASS shared GET/POST boundaries, every fragmentation split, strict lengths, headers and body bounds\n";
}
