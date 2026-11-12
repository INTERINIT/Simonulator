// Pass an empty temporary directory. Reusing a name must never resurrect
// the previous subscriber's login, pending uploads or download receipt.
#include <windows.h>
#include "ccmail_session.h"
#include <cassert>

ccmail::Message envelope(const postoffice::Account &sender, const postoffice::Account &recipient) {
    std::string header(32, 0);
    auto add = [&](char type, const std::string &value) {
        header.push_back(type);
        header.push_back(char(value.size() + 1));
        header += value;
        header.push_back(0);
    };
    add(1, sender.name);
    add(3, recipient.name);
    header[0] = char(header.size());
    header[4] = header[8] = 32;
    ccmail::Message message;
    std::vector<std::string> replies;
    assert(message.read_header(header, sender.name, replies));
    assert(replies == std::vector<std::string>{std::string(1, char(ccmail::Command::Accepted))});
    return message;
}

int main(int argc, char **argv) {
    switch_store::Listener listener;
    assert(argc == 2 && switch_store::initialize(argv[1], listener).empty());
    assert(postoffice::create("first", "test-pass"));
    assert(postoffice::create("second", "other-pass"));
    const auto old = postoffice::find_account("first");
    const auto other = postoffice::find_account("second");
    auto outgoing = envelope(old, other), incoming = envelope(other, old);
    assert(outgoing.deliver(old) && incoming.deliver(other));
    ccmail::Message download;
    assert(download.next_for(old));
    assert(postoffice::remove_account("FIRST") == old);
    assert(!postoffice::remove_account("first"));
    assert(postoffice::create("first", "test-pass"));
    const auto fresh = postoffice::find_account("first");
    assert(fresh != old && postoffice::login(fresh, "test-pass") && !postoffice::login(old, "test-pass"));

    assert(!outgoing.deliver(old) && !incoming.deliver(other));
    assert(!postoffice::deliver(old, other, "revoked", ""));
    assert(!postoffice::deliver(other, old, "stale recipient", ""));
    ccmail::Message fetched;
    assert(fetched.next_for(other)); // copies previously sent to another user remain
    assert(!fetched.next_for(fresh)); // replacement mailbox starts empty
    assert(envelope(other, fresh).deliver(other) && fetched.next_for(fresh));
    assert(fetched.id == download.id); // SQLite reuses the deleted highest letter id
    assert(!download.collected(old) && !fetched.next_for(old));
    assert(postoffice::inbox(old, int(download.id)) == "-ERR NO MESSAGE");
    assert(fetched.collected(fresh));

    // A failure in the last DELETE must roll back the first two DELETEs.
    switch_store::Database db;
    assert(sqlite3_exec(db.db, "CREATE TRIGGER test_abort BEFORE DELETE ON users "
        "BEGIN SELECT RAISE(ABORT,'rollback'); END", nullptr, nullptr, nullptr) == SQLITE_OK);
    assert(!postoffice::remove_account("second"));
    assert(postoffice::find_account("second") == other && fetched.next_for(other));
}
