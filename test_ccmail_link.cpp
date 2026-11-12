// Build with a C++17 compiler. These are the NFT frames on a stock Simon
// RCONNECT/Router session. The plaintext mail commands exercise a different
// protocol and cannot detect a broken native handshake.
#include "ccmail_link.h"
#include <cassert>

struct MailProbe {
    unsigned requests = 0;
    void reset() { requests = 0; }
    std::vector<std::string> receive(std::string_view data) {
        ++requests;
        return data == "\x80" ? std::vector<std::string>{"\x81"} : std::vector<std::string>{};
    }
};

int main() {
    ccmail::Link link;
    MailProbe mail;
    const std::string hello = "\x02\x04\x07\xff\x68\xc4\xaa\x55\x11\xba\x64";
    std::string response;
    for (char byte : hello) response += link.receive({&byte, 1}, mail);
    assert(response == std::string("\x02\x01\x40\x00\x09\x9f\x02\x01\x80\x0f\xee\x24", 12));

    const auto identify = ccmail::frame(8, "\x80");
    const auto ack = ccmail::frame(0x41, std::string(1, 0));
    const auto challenge = ccmail::frame(9, "\x81");
    assert(link.receive(identify, mail) == ack + challenge);
    assert(link.receive(identify, mail) == ack && mail.requests == 1);
    assert(link.receive(ccmail::frame(0x81, "\x04"), mail) == challenge);

    const auto data = ccmail::frame(0x11, "two");
    auto corrupt = data;
    corrupt.back() ^= 1;
    assert(link.receive(corrupt, mail) == ccmail::frame(0x81, "\x04"));
    assert(link.receive(data, mail) == ccmail::frame(0x42, std::string(1, 0)));
    assert(mail.requests == 2);

    // The 8250 consumes these serially. A long body must wait for an ACK
    // before each new frame and survive the three-bit sequence rollover.
    const std::string content(1024, 'x');
    std::string received;
    for (auto wire = link.send({content}); !wire.empty();) {
        unsigned length = std::uint8_t(wire[1]);
        received.push_back(wire[3]);
        if (length > 1) received.append(wire, 6, length - 1);
        unsigned sequence = (wire[2] >> 3) & 7;
        wire = link.receive(ccmail::frame(0x40 | sequence, std::string(1, 0)), mail);
    }
    assert(received == content);
}
