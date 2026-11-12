// One fax modem carrier burst, as seen by the receiving modem. The PC switch
// transports HDLC frames and image bytes. T.30 and T.4 run inside Simon.
#pragma once

#include <deque>
#include <string>

struct FaxBurst {
    std::string mode;
    std::deque<std::string> chunks;
    bool complete = false;
};

struct FaxLink {
    std::deque<FaxBurst> incoming;
    std::string receive_mode;
    std::size_t queued_digits = 0;
    bool receiving = false;
    bool requested_chunk = false;
};

inline bool valid_fax_mode(const std::string &mode) {
    // V.21 HDLC, V.27ter and V.29 image modes advertised by Simon's modem.
    return mode == "H3" || mode == "M24" || mode == "M48" ||
           mode == "M72" || mode == "M96";
}
