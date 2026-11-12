// Physical modem endpoint for SimonWeber's documented serial HTTP protocol.
// This server never dials out. --answer explicitly enables Hayes auto-answer.
#ifndef UNICODE
#define UNICODE
#endif
#define _UNICODE
#include "weber_gateway.h"

#include <atomic>
#include <cstdio>
#include <iostream>

namespace {
std::atomic_bool running{true};
BOOL WINAPI stop_console(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT || event == CTRL_CLOSE_EVENT) {
        running = false;
        return TRUE;
    }
    return FALSE;
}

// Transport-independent session state also supports --self-test on hosts
// without a modem or a virtual COM pair. A carrier edge invalidates the ID,
// so delayed worker completions can never cross into the next caller's call.
class SerialSession {
public:
    std::uint64_t generation = 1;
    bool online = false;
    bool pending = false;
    std::string input;
    std::string output;
    std::size_t position = 0;

    void carrier(bool present) {
        if (present == online) return;
        online = present;
        ++generation;
        input.clear();
        output.clear();
        position = 0;
        pending = false;
    }
    std::string receive(const char *data, std::size_t size) {
        if (!online || pending || !output.empty()) return {};
        input.append(data, size);
        if (input.size() > simon_weber::kRequestLimit) {
            output = simon_weber::framing_error(413);
            input.clear();
            return {};
        }
        // A Hayes CONNECT banner can be split across any read boundary and
        // DCD can rise before its trailing CRLF. Discard only that preamble,
        // retaining every byte of the subsequent HTTP request.
        while (!input.empty() && (input.front() == '\r' || input.front() == '\n')) input.erase(0, 1);
        if (input.starts_with("CONNECT")) {
            const auto newline = input.find('\n');
            if (newline == std::string::npos) return {};
            input.erase(0, newline + 1);
            while (!input.empty() && (input.front() == '\r' || input.front() == '\n')) input.erase(0, 1);
        }
        const auto frame = simon_weber::inspect_request(input);
        if (frame.state == simon_weber::FrameState::Error) {
            output = simon_weber::framing_error(frame.error);
            input.clear();
            return {};
        }
        if (frame.state != simon_weber::FrameState::Complete) return {};
        std::string request = std::move(input);
        input.clear();
        pending = true;
        return request;
    }
    void complete(simon_weber::Completion reply) {
        if (!online || !pending || reply.id != generation) return;
        pending = false;
        output = std::move(reply.response);
        position = 0;
        ++generation;
    }
};

int self_test() {
    SerialSession session;
    session.carrier(true);
    session.receive("\r\nCON", 5);
    session.receive("NECT 2400\r\n", 11);
    if (!session.receive("GET http://simon.local/ HTTP/1.0\r\n", 34).empty()) return 1;
    const std::string request = session.receive("\r\n", 2);
    if (request.empty() || !session.pending) return 2;
    const auto old = session.generation;
    session.carrier(false);
    session.carrier(true);
    session.complete({old, "STALE"});
    if (!session.output.empty()) return 3;
    const std::string next = "GET http://simon.local/test HTTP/1.0\r\n\r\n";
    session.receive(next.data(), next.size());
    session.complete({session.generation, simon_weber::fetch(next)});
    if (session.output.find("1 + 2 = 3") == std::string::npos) return 4;
    session.carrier(false);
    session.carrier(true);
    std::string excessive(2049, 'X');
    session.receive(excessive.data(), excessive.size());
    if (!session.output.starts_with("HTTP/1.0 413 ")) return 5;
    session.carrier(false);
    session.carrier(true);
    const std::string post = "POST https://example.test/chat?room=1 HTTP/1.0\r\n"
        "Content-Type: application/x-www-form-urlencoded\r\nContent-Length: 7\r\n\r\nx=local";
    for (std::size_t i = 0; i + 1 < post.size(); ++i) {
        if (!session.receive(post.data() + i, 1).empty() || session.pending || !session.output.empty()) return 6;
    }
    if (session.receive(post.data() + post.size() - 1, 1) != post || !session.pending) return 7;
    if (!session.receive(post.data(), post.size()).empty()) return 8; // no replay while in flight
    session.carrier(false);
    session.carrier(true);
    const std::string bad = "POST http://example.test/ HTTP/1.0\r\nContent-Length: 2\r\n"
        "Content-Length: 3\r\nContent-Type: application/x-www-form-urlencoded\r\n\r\na=x";
    session.receive(bad.data(), bad.size());
    if (!session.output.starts_with("HTTP/1.0 400 ") || session.pending) return 9;
    const std::pair<std::string, unsigned> rejected[] = {
        {"POST http://example.test/ HTTP/1.0\r\nContent-Type: application/x-www-form-urlencoded\r\n\r\n", 411},
        {post + "EXTRA", 400},
        {"POST http://example.test/ HTTP/1.0\r\nContent-Length: 1019\r\n\r\n", 413},
        {"POST http://example.test/ HTTP/1.0\r\nTransfer-Encoding: chunked\r\n\r\n", 400},
    };
    for (const auto &[wire, status] : rejected) {
        session.carrier(false);
        session.carrier(true);
        session.receive(wire.data(), wire.size());
        if (session.pending || !session.output.starts_with("HTTP/1.0 " + std::to_string(status) + " ")) return 10;
    }
    std::puts("PASS serial GET/POST framing, complete-body gating, no replay, carrier reset, stale-completion cancellation and bounds");
    return 0;
}

bool write_all(HANDLE serial, const std::string &text) {
    std::size_t sent = 0;
    while (sent < text.size() && running) {
        DWORD count = 0;
        if (!WriteFile(serial, text.data() + sent, DWORD(text.size() - sent), &count, nullptr) || !count) return false;
        sent += count;
    }
    return sent == text.size();
}

bool command(HANDLE serial, const std::string &text) {
    if (!write_all(serial, text + "\r")) return false;
    std::string reply;
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    while (running && std::chrono::steady_clock::now() < end) {
        char buffer[128];
        DWORD count = 0;
        if (!ReadFile(serial, buffer, sizeof(buffer), &count, nullptr)) return false;
        reply.append(buffer, count);
        if (reply.find("OK") != std::string::npos || reply.find("\r0\r") != std::string::npos) return true;
        if (reply.find("ERROR") != std::string::npos || reply.size() > 1024) return false;
        Sleep(10);
    }
    return false;
}
} // namespace

int wmain(int argc, wchar_t **argv) {
    std::wstring port;
    DWORD baud = 2400;
    bool answer = false, direct = false;
    for (int i = 1; i < argc; ++i) {
        const std::wstring option = argv[i];
        if (option == L"--self-test") return self_test();
        if (option == L"--serial" && i + 1 < argc) port = argv[++i];
        else if (option == L"--baud" && i + 1 < argc) baud = std::wcstoul(argv[++i], nullptr, 10);
        else if (option == L"--answer") answer = true;
        else if (option == L"--direct") direct = true;
        else {
            std::fputs("Unknown/missing option. Use --serial COMx [--baud 2400] [--answer | --direct].\n", stderr);
            return 2;
        }
    }
    if (port.empty() || baud < 300 || baud > 115200 || (answer && direct)) {
        std::fputs("Usage: SimonWeber_Modem_Server --serial COMx [--baud 2400] [--answer | --direct]\n"
            "Default: wait for modem DCD. --answer: initialize Hayes auto-answer.\n"
            "--direct: already-connected raw serial stream, ignores DCD (test/null-modem only).\n", stderr);
        return 2;
    }
    if (!port.starts_with(L"\\\\.\\")) port = L"\\\\.\\" + port;
    HANDLE serial = CreateFileW(port.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (serial == INVALID_HANDLE_VALUE) {
        std::fprintf(stderr, "Cannot open serial port: Windows error %lu\n", GetLastError());
        return 1;
    }
    DCB state{};
    state.DCBlength = sizeof(state);
    if (!GetCommState(serial, &state)) { CloseHandle(serial); return 1; }
    state.BaudRate = baud;
    state.ByteSize = 8;
    state.Parity = NOPARITY;
    state.StopBits = ONESTOPBIT;
    state.fBinary = TRUE;
    state.fParity = FALSE;
    state.fOutxCtsFlow = direct ? FALSE : TRUE;
    state.fOutxDsrFlow = FALSE;
    state.fDtrControl = DTR_CONTROL_ENABLE;
    state.fDsrSensitivity = FALSE;
    state.fOutX = state.fInX = FALSE;
    state.fNull = FALSE;
    state.fErrorChar = FALSE;
    state.fAbortOnError = FALSE;
    state.fRtsControl = direct ? RTS_CONTROL_ENABLE : RTS_CONTROL_HANDSHAKE;
    COMMTIMEOUTS timeouts{MAXDWORD, 0, 0, 0, 200};
    if (!SetCommState(serial, &state) || !SetCommTimeouts(serial, &timeouts)) {
        std::fprintf(stderr, "Cannot configure serial port: Windows error %lu\n", GetLastError());
        CloseHandle(serial);
        return 1;
    }
    SetupComm(serial, 4096, 4096);
    PurgeComm(serial, PURGE_RXCLEAR | PURGE_TXCLEAR);
    SetConsoleCtrlHandler(stop_console, TRUE);
    if (answer && (!command(serial, "ATZ") || !command(serial, "ATE0V1Q0S0=1&D2&C1"))) {
        std::fputs("Modem auto-answer initialization failed; check modem commands and CTS wiring.\n", stderr);
        CloseHandle(serial);
        return 1;
    }
    std::printf("SimonWeber modem server ready at %lu baud, 8N1; mode %s. Ctrl+C to stop.\n",
        baud, direct ? "direct stream" : answer ? "Hayes auto-answer, RTS/CTS, DCD" : "wait for DCD, RTS/CTS");
    simon_weber::Gateway gateway;
    SerialSession session;
    auto due = std::chrono::steady_clock::now();
    while (running) {
        DWORD modem = 0;
        if (!GetCommModemStatus(serial, &modem)) break;
        const bool carrier = direct || (modem & MS_RLSD_ON);
        if (carrier != session.online) {
            session.carrier(carrier);
            PurgeComm(serial, carrier ? PURGE_TXCLEAR : PURGE_TXCLEAR | PURGE_RXCLEAR);
            due = std::chrono::steady_clock::now();
            std::puts(carrier ? "Carrier established." : "Carrier lost; pending response discarded.");
        }
        char buffer[128];
        DWORD count = 0;
        if (!ReadFile(serial, buffer, sizeof(buffer), &count, nullptr)) break;
        if (count) {
            const std::string request = session.receive(buffer, count);
            // One server instance belongs to one physical Simon. A browser
            // redials for each Refresh, so keep its cookie jar across DCD
            // transitions; generation still discards stale modem replies.
            if (!request.empty() && !gateway.submit(session.generation, request, 0))
                session.complete({session.generation, simon_weber::response(503, "Gateway Busy", "Try again shortly.\r\n")});
        }
        for (auto &reply : gateway.drain()) session.complete(std::move(reply));
        const auto now = std::chrono::steady_clock::now();
        if (session.online && !session.output.empty() && now >= due) {
            const auto bytes = std::min<std::size_t>(32, session.output.size() - session.position);
            DWORD written = 0;
            if (!WriteFile(serial, session.output.data() + session.position, DWORD(bytes), &written, nullptr)) break;
            session.position += written;
            due = now + std::chrono::microseconds((written * 10000000 + baud - 1) / baud);
            if (session.position == session.output.size()) { session.output.clear(); session.position = 0; }
        }
        Sleep(5);
    }
    EscapeCommFunction(serial, CLRDTR);
    CloseHandle(serial);
    return 0;
}
