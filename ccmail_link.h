// Stock Simon RCONNECT uses Lotus NFT over the modem's byte stream.
// Sequence numbers and checksums below are independent of AMPS framing.
#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

namespace ccmail {

inline std::uint16_t checksum(std::string_view data) {
    std::uint16_t crc = 0xffff;
    for (unsigned char byte : data) {
        crc ^= std::uint16_t(byte) << 8;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
    }
    return ~crc;
}

inline void append_checksum(std::string &wire, std::string_view data) {
    const auto crc = checksum(data);
    wire.push_back(char(crc >> 8));
    wire.push_back(char(crc));
}

inline std::string frame(unsigned control, std::string_view payload) {
    std::string out{char(2), char(payload.size()), char(control), payload.front()};
    append_checksum(out, std::string_view(out).substr(1));
    if (payload.size() > 1) {
        out.append(payload.substr(1));
        append_checksum(out, payload.substr(1));
    }
    return out;
}

// Simon negotiates the one-byte-length NFT format. The first payload byte
// has its own protection together with length and type. Remaining bytes, if
// present, have a separate CRC. Both checksums are sent high byte first.
class Link {
public:
    std::string send(const std::vector<std::string> &packets) {
        // The original client has four 128-byte serial slots. Keeping one
        // frame in flight also allows the modem to drain at its real bit rate.
        for (const auto &payload : packets)
            for (std::size_t offset = 0; offset < payload.size(); offset += 112)
                queued.push_back(payload.substr(offset, 112));
        return transmit();
    }

    template <typename Application>
    std::string receive(std::string_view bytes, Application &application) {
        input.append(bytes);
        std::string reply;
        while (!input.empty()) {
            if (input.front() != 2) {
                input.erase(0, input.find(char(2)));
                continue;
            }
            if (input.size() < 6) break;
            const auto size = std::uint8_t(input[1]);
            if (!size || !valid(1, 3)) {
                input.erase(0, 1);
                reply += control_frame(0x80 | received, 4);
                continue;
            }
            const std::size_t total = size == 1 ? 6 : size + 7;
            if (input.size() < total) break;
            if (size == 1 || valid(6, size - 1)) {
                std::string payload(1, input[3]);
                if (size > 1) payload.append(input, 6, size - 1);
                reply += accept(std::uint8_t(input[2]), payload, application);
            } else {
                reply += control_frame(0x80 | received, 4);
            }
            input.erase(0, total);
        }
        return reply;
    }

private:
    std::string input;
    std::string last_data;
    std::deque<std::string> queued;
    unsigned received = 7;
    unsigned sent = 0;

    std::string transmit() {
        if (!last_data.empty() || queued.empty()) return {};
        sent = (sent + 1) & 7;
        last_data = frame((sent << 3) | received, queued.front());
        queued.pop_front();
        return last_data;
    }

    bool valid(std::size_t start, std::size_t count) const {
        const auto crc = checksum(std::string_view(input).substr(start, count));
        return std::uint8_t(input[start + count]) == (crc >> 8)
            && std::uint8_t(input[start + count + 1]) == (crc & 255);
    }

    static std::string control_frame(unsigned flags, char value) {
        return frame(flags, std::string_view(&value, 1));
    }

    template <typename Application>
    std::string accept(unsigned flags, const std::string &payload, Application &application) {
        if ((flags >> 6) == 2 && payload == "\x04") return last_data;
        if ((flags >> 6) == 1) {
            if ((flags & 7) == sent) last_data.clear();
            return transmit();
        }
        if ((flags >> 6) != 0) return {};
        const auto seq = (flags >> 3) & 7;
        if (seq == 0 && payload == "\xff\xaa\x55\x11") {
            received = sent = 0;
            last_data.clear();
            queued.clear();
            application.reset();
            return control_frame(0x40, 0) + control_frame(0x80, 0x0f);
        }
        if (seq == received) return control_frame(0x40 | received, 0);
        if (seq != ((received + 1) & 7)) return control_frame(0x80 | received, 4);
        received = seq;
        if ((flags & 7) == sent) last_data.clear();
        std::string reply = control_frame(0x40 | received, 0);
        reply += send(application.receive(payload));
        return reply;
    }
};

}
