#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <string_view>

namespace ccmail {

// RIMPORT writes text items in blocks of up to 512 bytes. Within each
// block it sends the columns of a 17-column grid in this order.
inline std::string decode_text_item(std::string_view encoded) {
    constexpr std::array<unsigned, 17> columns = {9, 4, 5, 13, 12, 11, 1, 7, 10, 3, 2, 16, 8, 15, 0, 14, 6};
    std::string text(encoded.size(), '\0');
    std::size_t source = 0;
    for (std::size_t start = 0; start < encoded.size(); start += 512) {
        const auto end = std::min(start + 512, encoded.size());
        for (unsigned column : columns) {
            for (std::size_t i = start + column; i < end; i += columns.size())
                text[i] = encoded[source++];
        }
    }
    return text;
}

} // namespace ccmail
