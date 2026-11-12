#pragma once

#include <algorithm>
#include <array>
#include <bcrypt.h>
#include <cstring>
#include <map>
#include <sstream>
#include "switch_store.h"
#include "weber_gateway.h"

namespace postoffice {

using switch_store::Database;
using switch_store::Query;
using Salt = std::array<unsigned char, 16>;
using Key = std::array<unsigned char, 32>;
inline constexpr char office_name[] = "5551001";

inline bool valid_name(const std::string &name) {
    return !name.empty() && name.size() <= 32 && name.find_first_not_of(
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") == name.npos;
}

inline bool exists(const std::string &name) {
    Database db;
    Query lookup(db, "SELECT 1 FROM users WHERE name=?");
    if (!lookup.s) return false;
    lookup.text(1, name);
    return sqlite3_step(lookup.s) == SQLITE_ROW;
}

inline bool derive(const std::string &password, Salt &salt, Key &verifier) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr,
            BCRYPT_ALG_HANDLE_HMAC_FLAG) < 0) return false;
    auto bytes = reinterpret_cast<PUCHAR>(const_cast<char *>(password.data()));
    auto status = BCryptDeriveKeyPBKDF2(algorithm, bytes, ULONG(password.size()), salt.data(),
        salt.size(), 210000, verifier.data(), verifier.size(), 0);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return status >= 0;
}

inline bool create(const std::string &name, const std::string &password) {
    // MAIL.EXE cannot resolve a mailbox with the same name as its post office.
    if (name == office_name || !valid_name(name) || password.size() < 4 || password.size() > 64) return false;
    if (!std::all_of(password.begin(), password.end(), [](unsigned char c) {
        return c >= 33 && c <= 126;
    })) return false;

    Salt salt;
    Key verifier;
    if (BCryptGenRandom(nullptr, salt.data(), salt.size(), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0 ||
            !derive(password, salt, verifier)) return false;

    Database db;
    Query insert(db, "INSERT INTO users(name,salt,verifier) VALUES(?,?,?)");
    if (!insert.s) return false;
    insert.text(1, name);
    sqlite3_bind_blob(insert.s, 2, salt.data(), salt.size(), SQLITE_TRANSIENT);
    sqlite3_bind_blob(insert.s, 3, verifier.data(), verifier.size(), SQLITE_TRANSIENT);
    return sqlite3_step(insert.s) == SQLITE_DONE;
}

inline bool login(const std::string &name, const std::string &password) {
    if (!valid_name(name) || password.empty() || password.size() > 64) return false;
    Salt salt;
    Key expected;
    {
        // End the SQLite reader before the CPU-intensive derivation so it
        // does not hold up new mail or registrations in another connection.
        Database db;
        Query lookup(db, "SELECT salt,verifier FROM users WHERE name=?");
        if (!lookup.s) return false;
        lookup.text(1, name);
        if (sqlite3_step(lookup.s) != SQLITE_ROW ||
                sqlite3_column_bytes(lookup.s, 0) != int(salt.size()) ||
                sqlite3_column_bytes(lookup.s, 1) != int(expected.size())) return false;
        std::memcpy(salt.data(), sqlite3_column_blob(lookup.s, 0), salt.size());
        std::memcpy(expected.data(), sqlite3_column_blob(lookup.s, 1), expected.size());
    }
    Key actual;
    if (!derive(password, salt, actual)) return false;
    unsigned difference = 0;
    for (size_t i = 0; i < actual.size(); ++i) difference |= actual[i] ^ expected[i];
    return difference == 0;
}

inline bool deliver(const std::string &from, const std::string &to,
        const std::string &subject, const std::string &body) {
    Database db;
    Query insert(db, "INSERT INTO letters(recipient,sender,subject,body) "
        "SELECT name,?,?,? FROM users WHERE name=?");
    if (!insert.s) return false;
    insert.text(1, from);
    insert.text(2, subject);
    insert.text(3, body);
    insert.text(4, to);
    return sqlite3_step(insert.s) == SQLITE_DONE && sqlite3_changes(db.db) == 1;
}

inline std::string inbox(const std::string &user, int id) {
    Database db;
    Query messages(db, id
        ? "SELECT sender,recipient,subject,body FROM letters WHERE recipient=? AND id=?"
        : "SELECT id,sender,subject FROM letters WHERE recipient=? ORDER BY id DESC LIMIT 100");
    if (!messages.s) return "-ERR STORAGE";
    messages.text(1, user);
    if (id) {
        sqlite3_bind_int(messages.s, 2, id);
        if (sqlite3_step(messages.s) != SQLITE_ROW) return "-ERR NO MESSAGE";
        return "+OK\r\nFrom: " + messages.col(0) + "\r\nTo: " + messages.col(1) +
            "\r\nSubject: " + messages.col(2) + "\r\n\r\n" + messages.col(3) + ".";
    }
    std::string lines;
    int count = 0;
    while (sqlite3_step(messages.s) == SQLITE_ROW) {
        lines += messages.col(0) + " " + messages.col(1) + " " + messages.col(2) + "\r\n";
        ++count;
    }
    return "+OK " + std::to_string(count) + "\r\n" + lines + ".";
}

inline int hex_digit(char digit) {
    if (digit >= '0' && digit <= '9') return digit - '0';
    if (digit >= 'a' && digit <= 'f') return digit - 'a' + 10;
    if (digit >= 'A' && digit <= 'F') return digit - 'A' + 10;
    return -1;
}

inline std::string decode_field(const std::string &encoded) {
    std::string value;
    for (size_t i = 0; i < encoded.size(); ++i) {
        if (encoded[i] != '%') {
            value += encoded[i] == '+' ? ' ' : encoded[i];
            continue;
        }
        if (i + 2 >= encoded.size()) return {};
        const int high = hex_digit(encoded[i + 1]);
        const int low = hex_digit(encoded[i + 2]);
        if (high < 0 || low < 0) return {};
        value += char(high * 16 + low);
        i += 2;
    }
    return value;
}

inline std::string registration(const simon_weber::RequestFrame &frame) {
    if (frame.method == "GET") {
        return simon_weber::response(200, "OK",
            "<html><body><h1>Simon Post Office</h1><p>Name: letters, digits, - or _. Password: 4-64 "
            "printable non-space characters. Use a lab-only password. In Simon Mail settings use "
            "5551001 for the post office. That name is reserved; choose a different mailbox name.</p>"
            "<form action='http://simon.local/register' method='POST'>Name <input name='user'> "
            "Password <input type='password' name='password'><input type='submit' value='Register'>"
            "</form></body></html>", "text/html");
    }
    std::map<std::string, std::string> fields;
    std::istringstream body(frame.body);
    std::string pair;
    while (std::getline(body, pair, '&')) {
        const auto separator = pair.find('=');
        if (separator == pair.npos ||
                !fields.emplace(pair.substr(0, separator), decode_field(pair.substr(separator + 1))).second) {
            return simon_weber::framing_error(400);
        }
    }
    if (fields.size() == 2 && create(fields["user"], fields["password"])) {
        return simon_weber::response(200, "OK", "<html><body>Account created.</body></html>", "text/html");
    }
    return simon_weber::response(400, "Registration failed",
        "<html><body>Registration failed. Check name and password or choose another name. "
        "5551001 is reserved for the post office.</body></html>", "text/html");
}

} // namespace postoffice
