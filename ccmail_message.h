// cc:Mail envelope storage. Keep original item records and attachment bytes:
// the Simon reader uses them to reconstruct its own native mail folders.
#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include "ccmail_text.h"
#include "mail_store.h"

namespace ccmail {

enum Command : std::uint8_t {
    StartSession = 0x80, RequestMailbox = 0x81, Accepted = 0x82, NotFound = 0x83,
    OfferMessage = 0x84, RequestHeader = 0x85, RequestContent = 0x86, Stored = 0x87,
    EndBatch = 0x88, Undeliverable = 0x89, RequestPassword = 0x8b, PasswordRejected = 0x8c,
};

inline std::uint16_t word(std::string_view data, std::size_t offset) {
    return std::uint8_t(data[offset]) | (std::uint16_t(std::uint8_t(data[offset + 1])) << 8);
}
inline std::uint32_t dword(std::string_view data, std::size_t offset) {
    return word(data, offset) | (std::uint32_t(word(data, offset + 2)) << 16);
}

struct Message {
    sqlite3_int64 id = 0;
    std::string header, content, subject;
    std::vector<std::string> recipients;
    std::size_t content_size = 0;
    unsigned addresses = 0;

    bool read_header(std::string bytes, const std::string &sender, std::vector<std::string> &replies) {
        header = std::move(bytes);
        // RCONNECT reads peer headers straight into an 8192-byte stack buffer.
        if (header.size() < 32 || header.size() > 8192) return false;
        const auto end = dword(header, 4);
        const auto start = word(header, 8);
        if (start < 32 || start > header.size() || end < start || end - start > 1024 * 1024) return false;
        content_size = end - start;
        if (dword(header, 0) != header.size() + content_size) return false;
        bool author = false;
        for (std::size_t i = start; i < header.size();) {
            if (i + 2 > header.size()) return false;
            const auto type = std::uint8_t(header[i++]);
            const auto size = std::uint8_t(header[i++]);
            if (i + size > header.size()) return false;
            const auto terminator = header.find('\0', i);
            const auto value = header.substr(i, std::min<std::size_t>(terminator - i, size));
            if (type == 1) author = sqlite3_stricmp(value.c_str(), sender.c_str()) == 0;
            if (type == 5) subject = value;
            if (type == 3) {
                ++addresses;
                const bool found = postoffice::exists(value);
                replies.emplace_back(1, char(found ? Command::Accepted : Command::NotFound));
                if (found && std::find(recipients.begin(), recipients.end(), value) == recipients.end())
                    recipients.push_back(value);
            }
            i += size;
        }
        return author && addresses != 0;
    }

    bool deliver(const std::string &sender) const {
        switch_store::Database db;
        if (!db.ready || sqlite3_exec(db.db, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) != SQLITE_OK) return false;
        switch_store::Query letter(db, "INSERT INTO letters(recipient,sender,subject,body) VALUES(?,?,?,?)");
        switch_store::Query wire(db, "INSERT INTO ccmail_envelopes(letter_id,header,content) VALUES(?,?,?)");
        if (!letter.s || !wire.s) return false;
        const auto body = body_text();
        for (const auto &recipient : recipients) {
            sqlite3_reset(letter.s);
            letter.text(1, recipient);
            letter.text(2, sender);
            letter.text(3, subject);
            letter.text(4, body);
            if (sqlite3_step(letter.s) != SQLITE_DONE) return false;
            sqlite3_reset(wire.s);
            sqlite3_bind_int64(wire.s, 1, sqlite3_last_insert_rowid(db.db));
            sqlite3_bind_blob(wire.s, 2, header.data(), int(header.size()), SQLITE_TRANSIENT);
            sqlite3_bind_blob(wire.s, 3, content.data(), int(content.size()), SQLITE_TRANSIENT);
            if (sqlite3_step(wire.s) != SQLITE_DONE) return false;
        }
        return sqlite3_exec(db.db, "COMMIT", nullptr, nullptr, nullptr) == SQLITE_OK;
    }

    std::string body_text() const {
        std::string result;
        const std::size_t start = word(header, 8);
        for (std::size_t item = 32; item + 2 <= start;) {
            const auto size = std::uint8_t(header[item + 1]);
            if (item + 2 + size > start) break;
            // Item type 6 describes a segment. Kind 1 is the message text.
            // The segment offsets include the descriptor area preceding the
            // content bytes. Other kinds, including attachments, remain in
            // the original envelope for the receiving Simon.
            if (header[item] == 6 && size == 18 && word(header, item + 8) == 1) {
                const auto count = dword(header, item + 12);
                const auto offset = dword(header, item + 16);
                if (offset >= start && offset - start <= content.size()
                        && count <= content.size() - (offset - start))
                    result += decode_text_item(std::string_view(content).substr(offset - start, count));
            }
            item += 2u + size;
        }
        return result;
    }

    bool next_for(const std::string &user) {
        switch_store::Database db;
        switch_store::Query next(db, "SELECT e.letter_id,e.header,e.content FROM ccmail_envelopes e "
            "JOIN letters l ON l.id=e.letter_id WHERE l.recipient=? AND e.collected=0 ORDER BY l.id LIMIT 1");
        if (!next.s) return false;
        next.text(1, user);
        if (sqlite3_step(next.s) != SQLITE_ROW) return false;
        id = sqlite3_column_int64(next.s, 0);
        auto blob = [&](int col) {
            const auto size = sqlite3_column_bytes(next.s, col);
            return size ? std::string(static_cast<const char *>(sqlite3_column_blob(next.s, col)), size) : std::string();
        };
        header = blob(1);
        content = blob(2);
        return true;
    }

    bool collected() const {
        switch_store::Database db;
        switch_store::Query update(db, "UPDATE ccmail_envelopes SET collected=1 WHERE letter_id=?");
        if (!update.s) return false;
        sqlite3_bind_int64(update.s, 1, id);
        return sqlite3_step(update.s) == SQLITE_DONE;
    }
};

}
