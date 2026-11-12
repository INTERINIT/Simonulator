// Original Simon MAIL.EXE dials RCONNECT, which logs into a cc:Mail remote
// mailbox. The application stream is carried inside sequenced NFT packets.
#pragma once

#include <string>
#include <string_view>
#include <optional>
#include <utility>
#include <vector>
#include "ccmail_message.h"

namespace ccmail {

class Session {
public:
    static constexpr std::string_view office = postoffice::office_name;

    void reset() { *this = Session{}; }

    auto take_login() { return std::exchange(credentials, std::nullopt); }

    std::vector<std::string> login_result(bool ok) {
        if (phase != Phase::Authenticating) return {};
        phase = ok ? Phase::ConfirmOffice : Phase::Rejected;
        if (ok) return {std::string(1, char(Command::Accepted)), office_identity()};
        return {std::string(1, char(Command::PasswordRejected))};
    }

    std::vector<std::string> receive(std::string_view data) {
        pending.append(data);
        std::vector<std::string> reply;
        while (!pending.empty()) {
            const auto first = static_cast<unsigned char>(pending.front());
            if (phase == Phase::UploadHeader) {
                if (pending.size() < 2) break;
                const auto size = word(pending, 0);
                if (pending.size() < 2u + size) break;
                const bool valid = message.read_header(pending.substr(2, size), user, reply);
                pending.erase(0, 2u + size);
                if (!valid || message.recipients.empty()) {
                    reply.emplace_back(1, char(Command::Undeliverable));
                    phase = Phase::Transfer;
                    continue;
                }
                reply.emplace_back(1, char(Command::RequestContent));
                phase = Phase::UploadContent;
            }
            if (phase == Phase::UploadContent) {
                if (pending.size() < message.content_size) break;
                message.content = pending.substr(0, message.content_size);
                pending.erase(0, message.content_size);
                reply.emplace_back(1, char(message.deliver(user) ? Command::Stored : Command::Undeliverable));
                phase = Phase::Transfer;
                continue;
            }
            if (phase == Phase::User || phase == Phase::Password) {
                if (pending.size() <= first) break;
                auto field = pending.substr(1, first);
                pending.erase(0, 1 + first);
                const auto end = field.find('\0');
                if (end == field.npos) { phase = Phase::Closed; continue; }
                field.resize(end);
                if (phase == Phase::User) {
                    user = field;
                    if (postoffice::exists(user)) {
                        reply.emplace_back(1, char(Command::RequestPassword));
                        phase = Phase::Password;
                    } else {
                        reply.push_back(office_identity(Command::NotFound));
                        phase = Phase::Rejected;
                    }
                } else {
                    credentials.emplace(user, std::move(field));
                    phase = Phase::Authenticating;
                }
                continue;
            }
            pending.erase(0, 1);
            switch (phase) {
            case Phase::Hello:
                if (first == Command::StartSession) {
                    reply.emplace_back(1, char(Command::RequestMailbox));
                    phase = Phase::User;
                }
                break;
            case Phase::ConfirmOffice:
                phase = first == Command::Accepted ? Phase::Transfer : Phase::Rejected;
                break;
            case Phase::Transfer:
                if (first == Command::OfferMessage) {
                    message = {};
                    reply.emplace_back(1, char(Command::RequestHeader));
                    phase = Phase::UploadHeader;
                } else if (first == Command::EndBatch) offer_next(reply);
                break;
            case Phase::Offer:
                if (first == Command::RequestHeader) {
                    const auto size = message.header.size();
                    std::string record{char(size), char(size >> 8)};
                    record += message.header;
                    reply.push_back(std::move(record));
                    phase = Phase::Download;
                }
                break;
            case Phase::Download:
                // Simon replies to each recipient, then requests the data
                // with 86. Only 87 means it committed the letter to disk.
                if (first == Command::RequestContent && !message.content.empty()) reply.push_back(message.content);
                if (first == Command::Stored) {
                    if (message.collected()) offer_next(reply);
                    else { reply.emplace_back(1, char(Command::EndBatch)); phase = Phase::Closed; }
                }
                if (first == Command::Undeliverable) { reply.emplace_back(1, char(Command::EndBatch)); phase = Phase::Closed; }
                break;
            case Phase::Rejected:
                if (first == Command::EndBatch) {
                    reply.emplace_back(1, char(Command::EndBatch));
                    phase = Phase::Closed;
                }
                break;
            default:
                break;
            }
        }
        return reply;
    }

private:
    enum class Phase { Hello, User, Password, Authenticating, ConfirmOffice, Transfer,
        UploadHeader, UploadContent, Offer, Download, Rejected, Closed };
    Phase phase = Phase::Hello;
    std::string pending;
    std::string user;
    std::optional<std::pair<std::string, std::string>> credentials;
    Message message;

    void offer_next(std::vector<std::string> &reply) {
        message = {};
        if (message.next_for(user)) {
            reply.emplace_back(1, char(Command::OfferMessage));
            phase = Phase::Offer;
        } else {
            reply.emplace_back(1, char(Command::EndBatch));
            phase = Phase::Closed;
        }
    }

    static std::string office_identity(int status = 0) {
        std::string record;
        if (status) record.push_back(char(status));
        record.push_back(char(office.size() + 1));
        record.append(office);
        record.push_back('\0');
        return record;
    }
};

}
