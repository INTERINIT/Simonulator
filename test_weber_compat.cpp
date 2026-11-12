// Local-fixture test harness: hex transport prevents command-line secrets
// and binary encodings from being altered by the console code page.
#include "weber_gateway.h"
#include <iostream>

static std::string unhex(const std::string &text) {
    std::string output;
    for (std::size_t i = 0; i + 1 < text.size(); i += 2)
        output.push_back(char(std::stoul(text.substr(i, 2), nullptr, 16)));
    return output;
}

static std::string hex(const std::string &text) {
    constexpr char digits[] = "0123456789abcdef";
    std::string output;
    for (unsigned char c : text) { output += digits[c >> 4]; output += digits[c & 15]; }
    return output;
}

int main() {
    simon_weber::Gateway gateway;
    std::uint64_t next = 1;
    std::string line;
    while (std::getline(std::cin, line)) {
        const auto split = line.find(' ');
        if (split == std::string::npos) return 2;
        const auto key = std::stoull(line.substr(0, split));
        if (line.substr(split + 1) == "forget") {
            gateway.forget(key);
            std::cout << "OK\n" << std::flush;
            continue;
        }
        const auto id = next++;
        if (!gateway.submit(id, unhex(line.substr(split + 1)), key)) return 3;
        bool done = false;
        while (!done) {
            for (const auto &reply : gateway.drain()) {
                if (reply.id != id) return 4;
                std::cout << hex(reply.response) << '\n' << std::flush;
                done = true;
            }
            if (!done) Sleep(2);
        }
    }
}
