// Each operation owns its SQLite connection. Startup
// creates the schema once, before either thread starts handling requests.
#pragma once
#include <filesystem>
#include <sqlite3.h>
#include <string>

namespace switch_store {
inline std::string database_path;

class Database {
public:
    sqlite3 *db = nullptr;
    bool ready;

    explicit Database(bool create = false) {
        const int flags = SQLITE_OPEN_READWRITE | (create ? SQLITE_OPEN_CREATE : 0);
        ready = sqlite3_open_v2(database_path.c_str(), &db, flags, nullptr) == SQLITE_OK;
        if (ready)
            sqlite3_busy_timeout(db, 3000);
    }
    Database(const Database &) = delete;
    Database &operator=(const Database &) = delete;
    ~Database() { sqlite3_close(db); }
};

struct Query {
    sqlite3_stmt *s = nullptr;
    Query(Database &database, const char *sql) {
        if (database.ready)
            sqlite3_prepare_v2(database.db, sql, -1, &s, nullptr);
    }
    Query(const Query &) = delete;
    Query &operator=(const Query &) = delete;
    ~Query() { sqlite3_finalize(s); }
    void text(int parameter, const std::string &value) {
        sqlite3_bind_text(s, parameter, value.data(), int(value.size()), SQLITE_TRANSIENT);
    }
    std::string col(int column) const {
        const auto *value = sqlite3_column_text(s, column);
        return value ? reinterpret_cast<const char *>(value) : "";
    }
};

struct Listener {
    std::string address = "127.0.0.1";
    int port = 5555;
};

inline std::string initialize(const std::filesystem::path &directory, Listener &listener) {
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) return error.message();
    const auto path = (directory / L"simon-postoffice.sqlite3").u8string();
    database_path.assign(path.begin(), path.end());
    Database database(true);
    if (!database.ready) return sqlite3_errmsg(database.db);
    constexpr const char *schema = R"sql(
        BEGIN IMMEDIATE;
        CREATE TABLE IF NOT EXISTS users (
            name TEXT COLLATE NOCASE PRIMARY KEY, salt BLOB NOT NULL, verifier BLOB NOT NULL);
        CREATE TABLE IF NOT EXISTS letters (
            id INTEGER PRIMARY KEY, recipient TEXT COLLATE NOCASE NOT NULL, sender TEXT NOT NULL,
            subject TEXT NOT NULL, body TEXT NOT NULL);
        CREATE INDEX IF NOT EXISTS mailbox ON letters(recipient,id);
        CREATE TABLE IF NOT EXISTS ccmail_envelopes (
            letter_id INTEGER PRIMARY KEY REFERENCES letters(id),
            header BLOB NOT NULL, content BLOB NOT NULL, collected INTEGER NOT NULL DEFAULT 0);
        CREATE TABLE IF NOT EXISTS profiles (
            number INTEGER PRIMARY KEY, registration TEXT NOT NULL, signal INTEGER NOT NULL,
            operator_name TEXT NOT NULL);
        CREATE TABLE IF NOT EXISTS listener (
            id INTEGER PRIMARY KEY CHECK(id=1), address TEXT NOT NULL,
            port INTEGER NOT NULL CHECK(port BETWEEN 1 AND 65535));
        INSERT OR IGNORE INTO listener VALUES(1,'127.0.0.1',5555);
        CREATE TABLE IF NOT EXISTS preferences (name TEXT PRIMARY KEY, value TEXT NOT NULL);
        COMMIT;
    )sql";
    if (sqlite3_exec(database.db, schema, nullptr, nullptr, nullptr) != SQLITE_OK) return sqlite3_errmsg(database.db);
    Query query(database, "SELECT address,port FROM listener WHERE id=1");
    if (!query.s || sqlite3_step(query.s) != SQLITE_ROW) return sqlite3_errmsg(database.db);
    listener.address = query.col(0);
    listener.port = sqlite3_column_int(query.s, 1);
    return {};
}

inline bool save_listener(const Listener &listener) {
    Database database;
    Query query(database, "UPDATE listener SET address=?,port=? WHERE id=1");
    if (!query.s) return false;
    query.text(1, listener.address);
    sqlite3_bind_int(query.s, 2, listener.port);
    return sqlite3_step(query.s) == SQLITE_DONE;
}

// An unset language follows Windows until the operator chooses one.
inline bool chinese_interface(bool system_default) {
    Database database;
    Query query(database, "SELECT value FROM preferences WHERE name='language'");
    if (!query.s || sqlite3_step(query.s) != SQLITE_ROW) return system_default;
    return query.col(0) == "zh-CN";
}

inline bool save_language(bool chinese) {
    Database database;
    Query query(database, "INSERT OR REPLACE INTO preferences VALUES('language',?)");
    if (!query.s) return false;
    query.text(1, chinese ? "zh-CN" : "en");
    return sqlite3_step(query.s) == SQLITE_DONE;
}

struct Profile {
    std::string registration = "HOME1";
    int signal = 6;
    std::string operator_name = "Simon AMPS";
};

inline bool save_profile(int number, const Profile &profile) {
    Database database;
    Query query(database, "INSERT OR REPLACE INTO profiles(number,registration,signal,operator_name) VALUES(?,?,?,?)");
    if (!query.s) return false;
    sqlite3_bind_int(query.s, 1, number);
    query.text(2, profile.registration);
    sqlite3_bind_int(query.s, 3, profile.signal);
    query.text(4, profile.operator_name);
    return sqlite3_step(query.s) == SQLITE_DONE;
}

inline bool load_profile(int number, Profile &profile) {
    Database database;
    Query query(database, "SELECT registration,signal,operator_name FROM profiles WHERE number=?");
    if (!query.s) return false;
    sqlite3_bind_int(query.s, 1, number);
    const int result = sqlite3_step(query.s);
    if (result == SQLITE_DONE) return true; // A new number starts on the default network.
    if (result != SQLITE_ROW) return false;
    profile = {query.col(0), sqlite3_column_int(query.s, 1), query.col(2)};
    return true;
}
} // namespace switch_store
