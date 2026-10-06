// Simon AMPS Laboratory Switch
//
// A native Win32 control panel and localhost cellular switch for the IBM
// Simon MAME driver.  The implementation deliberately separates an AMPS-like
// RF control plane (registration, paging, voice-channel assignment, SAT/ST)
// from the analogue bearer (8 kHz unsigned PCM).  It is not an RF transmitter,
// but the state transitions exposed to the emulated Mitsubishi deck follow the
// same order as a cellular call.

#define UNICODE
#define _UNICODE
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <commctrl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

namespace {

constexpr int kFirstNumber = 1001;
constexpr int kDefaultPort = 5555;
constexpr int kControlChannel = 334;
constexpr int kVoiceFirst = 354;
constexpr int kVoiceLast = 666;
constexpr int kMailNumber = 5551001;
constexpr int kFaxNumber = 5551002;
constexpr int kDataNumber = 5551000;
constexpr int kPagerNumber = 5552000;
constexpr double kPi = 3.14159265358979323846;

constexpr UINT WM_SWITCH_LOG = WM_APP + 1;
constexpr UINT WM_SWITCH_REFRESH = WM_APP + 2;

enum ControlId : int {
    IDC_UNITS = 100,
    IDC_LOG,
    IDC_FROM,
    IDC_TO,
    IDC_CALL,
    IDC_CONNECT,
    IDC_HANG,
    IDC_PAGE_TO,
    IDC_PAGE_TEXT,
    IDC_PAGE,
    IDC_REGISTRATION,
    IDC_SIGNAL,
    IDC_OPERATOR,
    IDC_APPLY,
    IDC_CLEAR,
};

enum class UnitState { Registered, Originating, Paging, Ringing, Connected, BusyTone, Data };
enum class CallStage { Page, Assign, Alert, Ringing, Connected };
enum class Service { None, Echo, Mail, Fax, Pager, Emergency };

const char *state_name(UnitState state) {
    switch (state) {
    case UnitState::Registered: return "Registered";
    case UnitState::Originating: return "Originating";
    case UnitState::Paging: return "Paging";
    case UnitState::Ringing: return "Ringing";
    case UnitState::Connected: return "Connected";
    case UnitState::BusyTone: return "Busy tone";
    case UnitState::Data: return "Data";
    }
    return "?";
}

std::wstring widen(const std::string &value) {
    if (value.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, 0, value.data(), int(value.size()), nullptr, 0);
    std::wstring result(count, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), int(value.size()), result.data(), count);
    return result;
}

std::string narrow(const std::wstring &value) {
    if (value.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, 0, value.data(), int(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(count, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), int(value.size()), result.data(), count, nullptr, nullptr);
    return result;
}

std::string trim(std::string value) {
    while (!value.empty() && (value.back() == '\r' || value.back() == '\n' || value.back() == ' ' || value.back() == '\t')) value.pop_back();
    std::size_t first = 0;
    while (first < value.size() && (value[first] == ' ' || value[first] == '\t')) ++first;
    return value.substr(first);
}

struct DtmfDetector {
    std::vector<double> samples;
    char candidate = 0;
    int stable = 0;

    static double power(const std::vector<double> &input, double frequency) {
        const double omega = 2.0 * kPi * frequency / 8000.0;
        const double coefficient = 2.0 * std::cos(omega);
        double q0 = 0.0, q1 = 0.0, q2 = 0.0;
        for (double sample : input) {
            q0 = coefficient * q1 - q2 + sample;
            q2 = q1;
            q1 = q0;
        }
        return q1 * q1 + q2 * q2 - coefficient * q1 * q2;
    }

    std::optional<char> push(const std::vector<std::uint8_t> &pcm) {
        for (std::uint8_t byte : pcm) {
            samples.push_back((int(byte) - 128) / 128.0);
            if (samples.size() < 205) continue;
            static constexpr std::array<double, 4> lows{697, 770, 852, 941};
            static constexpr std::array<double, 3> highs{1209, 1336, 1477};
            static constexpr char keys[4][3]{{'1','2','3'}, {'4','5','6'}, {'7','8','9'}, {'*','0','#'}};
            std::array<double, 4> lp{};
            std::array<double, 3> hp{};
            for (std::size_t i = 0; i < lp.size(); ++i) lp[i] = power(samples, lows[i]);
            for (std::size_t i = 0; i < hp.size(); ++i) hp[i] = power(samples, highs[i]);
            const auto li = std::size_t(std::max_element(lp.begin(), lp.end()) - lp.begin());
            const auto hi = std::size_t(std::max_element(hp.begin(), hp.end()) - hp.begin());
            double low_second = 0.0, high_second = 0.0;
            for (std::size_t i = 0; i < lp.size(); ++i) if (i != li) low_second = std::max(low_second, lp[i]);
            for (std::size_t i = 0; i < hp.size(); ++i) if (i != hi) high_second = std::max(high_second, hp[i]);
            const double energy = lp[li] + hp[hi];
            const bool dominant = lp[li] > low_second * 2.5 && hp[hi] > high_second * 2.5;
            const char found = energy > 18.0 && dominant ? keys[li][hi] : 0;
            samples.clear();
            if (found && found == candidate) ++stable;
            else { candidate = found; stable = found ? 1 : 0; }
            if (stable >= 2) {
                stable = 0;
                candidate = 0;
                return found;
            }
        }
        return std::nullopt;
    }
};

struct MailMessage {
    std::string from;
    std::string to;
    std::string subject;
    std::string body;
    std::string date;
};

struct Client {
    SOCKET socket = INVALID_SOCKET;
    int number = 0;
    std::string input;
    std::string output;
    std::size_t output_position = 0;
    bool transport_failed = false;
    int peer = 0;
    UnitState state = UnitState::Registered;
    std::string registration = "HOME1";
    int signal = 6;
    int sid = 1;
    int channel = kControlChannel;
    int sat = 6000;
    std::string operator_name = "Simon AMPS";
    Clock::time_point last_sat = Clock::now();
    std::size_t tone_sample = 0;
    Service service = Service::None;
    std::string service_input;
    std::string mail_user;
    bool mail_message_mode = false;
    bool mail_native_mode = false;
    std::string mail_message;
    std::string pager_target;
    std::string pager_message;
    DtmfDetector dtmf;
    bool rf_ready = false;
    std::size_t mail_trace_bytes = 0;
};

struct Call {
    int caller = 0;
    int called = 0;
    int channel = 0;
    int sat = 6000;
    CallStage stage = CallStage::Page;
    Clock::time_point due{};
    bool network_page = false;
    std::string page_digits;
    std::size_t page_position = 0;
    std::size_t page_samples = 0;
};

enum class ActionType { Call, Connect, Hang, Page, Profile };
struct Action {
    ActionType type{};
    int from = 0;
    int to = 0;
    int signal = 6;
    std::string text;
    std::string registration;
    std::string operator_name;
};

struct UnitSnapshot {
    int number = 0;
    std::string state;
    int peer = 0;
    int channel = 0;
    int sat = 0;
    std::string registration;
    int signal = 0;
    int sid = 0;
    std::string operator_name;
};

class SwitchServer {
public:
    explicit SwitchServer(HWND notify) : m_notify(notify) {}
    ~SwitchServer() { stop(); }

    bool start(int port) {
        if (m_running.exchange(true)) return true;
        m_port = port;
        m_thread = std::thread([this] { run(); });
        return true;
    }

    void stop() {
        if (!m_running.exchange(false)) return;
        if (m_thread.joinable()) m_thread.join();
    }

    void action(Action action) {
        std::lock_guard lock(m_action_mutex);
        m_actions.push_back(std::move(action));
    }

    std::vector<UnitSnapshot> snapshots() const {
        std::lock_guard lock(m_snapshot_mutex);
        return m_snapshots;
    }

private:
    HWND m_notify = nullptr;
    int m_port = kDefaultPort;
    std::atomic_bool m_running{false};
    std::thread m_thread;
    SOCKET m_listener = INVALID_SOCKET;
    int m_next_number = kFirstNumber;
    int m_next_voice = kVoiceFirst;
    int m_last_read_number = 0;
    std::map<int, std::unique_ptr<Client>> m_clients;
    std::vector<Call> m_calls;
    std::unordered_map<std::string, std::vector<MailMessage>> m_mailboxes;
    mutable std::mutex m_snapshot_mutex;
    std::vector<UnitSnapshot> m_snapshots;
    std::mutex m_action_mutex;
    std::deque<Action> m_actions;

    void log(std::string text) {
        SYSTEMTIME st{};
        GetLocalTime(&st);
        char prefix[32];
        std::snprintf(prefix, sizeof(prefix), "%02u:%02u:%02u.%03u  ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        std::ofstream trace("amps-switch.log", std::ios::app | std::ios::binary);
        trace << prefix << text << "\r\n";
        if (m_notify) {
            auto *message = new std::wstring(widen(std::string(prefix) + text + "\r\n"));
            if (!PostMessageW(m_notify, WM_SWITCH_LOG, 0, reinterpret_cast<LPARAM>(message))) delete message;
        }
    }

    Client *find(int number) {
        auto it = m_clients.find(number);
        return it == m_clients.end() ? nullptr : it->second.get();
    }

    static bool send_line(Client &client, const std::string &line) {
        // Winsock can accept only part of a line, or apply backpressure.  Keep
        // the unsent suffix intact; losing it corrupts both PCM and RF orders.
        if (client.transport_failed) return false;
        if (client.output.size() - client.output_position + line.size() + 1 > 1024 * 1024) {
            client.transport_failed = true;
            return false;
        }
        client.output.append(line);
        client.output.push_back('\n');
        return true;
    }

    static bool flush_client(Client &client) {
        std::size_t budget = 64 * 1024;
        while (client.output_position < client.output.size() && budget) {
            const int count = int(std::min(budget, client.output.size() - client.output_position));
            const int sent = send(client.socket, client.output.data() + client.output_position, count, 0);
            if (sent == SOCKET_ERROR) {
                const int error = WSAGetLastError();
                if (error == WSAEWOULDBLOCK) break;
                return false;
            }
            if (!sent) return false;
            client.output_position += std::size_t(sent);
            budget -= std::size_t(sent);
        }
        if (client.output_position) {
            client.output.erase(0, client.output_position);
            client.output_position = 0;
        }
        return true;
    }

    void send_channel(Client &client) {
        send_line(client, "D " + std::to_string(client.sid) + " " + std::to_string(client.channel) + " " + std::to_string(client.sat));
    }

    void send_profile(Client &client) {
        send_line(client, "V " + std::to_string(client.number));
        send_channel(client);
        send_line(client, "S " + client.registration + " " + std::to_string(client.signal) + " " + client.operator_name);
    }

    void send_modem(Client &client, const std::string &data) {
        static constexpr char hex[] = "0123456789ABCDEF";
        for (std::size_t offset = 0; offset < data.size(); offset += 32) {
            std::string line = "M X ";
            const std::size_t count = std::min<std::size_t>(32, data.size() - offset);
            for (std::size_t i = 0; i < count; ++i) {
                const auto value = std::uint8_t(data[offset + i]);
                line.push_back(hex[value >> 4]);
                line.push_back(hex[value & 15]);
            }
            send_line(client, line);
        }
    }

    int allocate_channel() {
        for (int attempts = 0; attempts <= kVoiceLast - kVoiceFirst; ++attempts) {
            const int candidate = m_next_voice++;
            if (m_next_voice > kVoiceLast) m_next_voice = kVoiceFirst;
            const bool used = std::any_of(m_calls.begin(), m_calls.end(), [candidate](const Call &call) { return call.channel == candidate; });
            if (!used) return candidate;
        }
        return 0;
    }

    void begin_call(Client &source, int target_number) {
        Client *target = find(target_number);
        if (!target || source.state != UnitState::Registered || target->state != UnitState::Registered) {
            send_line(source, "B");
            source.state = UnitState::BusyTone;
            source.tone_sample = 0;
            log("Busy/no idle mobile: " + std::to_string(source.number) + " -> " + std::to_string(target_number));
            refresh();
            return;
        }
        const int channel = allocate_channel();
        if (!channel) {
            // I 83 is Simon's native system-busy order, distinct from subscriber busy audio.
            send_line(source, "I 83");
            log("No free AMPS voice channel for " + std::to_string(source.number));
            return;
        }
        const int sat = std::array<int,3>{5970,6000,6030}[channel % 3];
        source.peer = target_number;
        target->peer = source.number;
        source.state = UnitState::Originating;
        target->state = UnitState::Paging;
        m_calls.push_back(Call{source.number, target_number, channel, sat, CallStage::Page, Clock::now() + 120ms});
        log("RECC origination: " + std::to_string(source.number) + " requests " + std::to_string(target_number));
        refresh();
    }

    void start_service(Client &source, Service service, int number) {
        source.service = service;
        source.state = UnitState::Data;
        source.peer = number;
        source.service_input.clear();
        if (service == Service::Echo || service == Service::Mail || service == Service::Fax) {
            send_line(source, "M C 2400");
            if (service == Service::Mail) {
                source.mail_user = std::to_string(source.number);
            }
            log("2400 bit/s data call: " + std::to_string(source.number) + " -> " + std::to_string(number));
        } else if (service == Service::Pager) {
            source.state = UnitState::Connected;
            source.channel = allocate_channel();
            source.sat = std::array<int,3>{5970,6000,6030}[source.channel % 3];
            send_line(source, "D " + std::to_string(source.sid) + " " + std::to_string(source.channel) + " " + std::to_string(source.sat));
            send_line(source, "C");
            source.pager_target.clear();
            source.pager_message.clear();
            log("Pager IVR answered " + std::to_string(source.number) + "; enter destination#, message# using DTMF");
        }
        refresh();
    }

    void start_emergency_call(Client &source) {
        if (source.state != UnitState::Registered) {
            send_line(source, "B");
            source.state = UnitState::BusyTone;
            source.tone_sample = 0;
            log("Emergency call rejected: mobile " + std::to_string(source.number) + " is already in use");
            refresh();
            return;
        }

        const int channel = allocate_channel();
        if (!channel) {
            // Even an emergency call still requires an available AMPS forward/
            // reverse voice-channel pair.  Report genuine system congestion.
            send_line(source, "I 83");
            log("Emergency call failed: no free AMPS voice channel for " + std::to_string(source.number));
            return;
        }

        const int sat = std::array<int,3>{5970,6000,6030}[channel % 3];
        source.peer = 911;
        source.state = UnitState::Connected;
        source.service = Service::Emergency;
        source.channel = channel;
        source.sat = sat;
        source.last_sat = Clock::now();

        // A PSAP is a network endpoint, not another registered mobile.  Retain
        // a connected call record so its voice channel cannot be allocated to
        // another call, then complete the normal FVC assignment seen by Simon.
        Call emergency;
        emergency.caller = source.number;
        emergency.channel = channel;
        emergency.sat = sat;
        emergency.stage = CallStage::Connected;
        m_calls.push_back(std::move(emergency));
        send_channel(source);
        send_line(source, "C");
        log("Emergency 911 call connected: mobile " + std::to_string(source.number) +
            ", channel " + std::to_string(channel) + ", SAT " + std::to_string(sat));
        refresh();
    }

    void originate(Client &source, const std::string &digits) {
        int target = 0;
        try { target = std::stoi(digits); } catch (...) { }
        const auto dialled = [&digits](int service) {
            const std::string suffix = std::to_string(service);
            // Real installations commonly required an access/prefix digit.
            // Match the configured service number at the end of the dial string
            // while retaining the complete digits in the operator log.
            return digits == suffix || (digits.size() > suffix.size() && digits.ends_with(suffix));
        };
        if (digits == "911") start_emergency_call(source);
        else if (dialled(kDataNumber)) start_service(source, Service::Echo, kDataNumber);
        else if (dialled(kMailNumber)) start_service(source, Service::Mail, kMailNumber);
        else if (dialled(kFaxNumber)) start_service(source, Service::Fax, kFaxNumber);
        else if (dialled(kPagerNumber)) start_service(source, Service::Pager, kPagerNumber);
        else begin_call(source, target);
    }

    void release(int number, bool notify) {
        Client *client = find(number);
        if (!client) return;
        const int peer_number = client->peer;
        if (notify) send_line(*client, "H");
        client->peer = 0;
        client->state = UnitState::Registered;
        client->channel = kControlChannel;
        client->sat = 6000;
        client->service = Service::None;
        client->tone_sample = 0;
        if (Client *peer = find(peer_number); peer && peer->peer == number) {
            send_line(*peer, "H");
            peer->peer = 0;
            peer->state = UnitState::Registered;
            peer->channel = kControlChannel;
            peer->sat = 6000;
            peer->service = Service::None;
            send_channel(*peer);
        }
        // Releasing a voice/pager bearer returns to the control channel; it
        // does not re-register the mobile.  Re-sending S here used to show an
        // operator popup and provoke another host RF-status report per page.
        send_channel(*client);
        std::erase_if(m_calls, [number](const Call &call) { return call.caller == number || call.called == number; });
        log("Release/ST: " + std::to_string(number) + (peer_number ? " <-> " + std::to_string(peer_number) : ""));
        refresh();
    }

    void answer(Client &source) {
        auto it = std::find_if(m_calls.begin(), m_calls.end(), [&source](const Call &call) { return call.called == source.number; });
        if (it == m_calls.end()) return;
        Client *caller = find(it->caller);
        send_line(source, "C");
        if (caller) send_line(*caller, "C");
        source.state = UnitState::Connected;
        source.channel = it->channel;
        source.sat = it->sat;
        source.last_sat = Clock::now();
        if (caller) {
            caller->state = UnitState::Connected;
            caller->channel = it->channel;
            caller->sat = it->sat;
            caller->last_sat = source.last_sat;
        }
        it->stage = CallStage::Connected;
        it->due = Clock::now() + (it->network_page ? 1200ms : 0ms);
		if (it->network_page)
			log("Pager IVR voice path connected to " + std::to_string(source.number) +
				"; sending digits to the RF deck's silent DTMF path");
        else
            log("FVC conversation: " + std::to_string(caller->number) + " <-> " + std::to_string(source.number) +
                ", channel " + std::to_string(it->channel) + ", SAT " + std::to_string(it->sat));
        refresh();
    }

    static std::vector<std::uint8_t> decode_hex(const std::string &hex) {
        auto nibble = [](char ch) -> int {
            if (ch >= '0' && ch <= '9') return ch - '0';
            if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
            if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
            return -1;
        };
        std::vector<std::uint8_t> result;
        for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
            const int a = nibble(hex[i]), b = nibble(hex[i + 1]);
            if (a < 0 || b < 0) break;
            result.push_back(std::uint8_t((a << 4) | b));
        }
        return result;
    }

    static std::string encode_hex(std::string_view bytes) {
        static constexpr char digits[] = "0123456789ABCDEF";
        std::string result;
        result.reserve(bytes.size() * 2);
        for (unsigned char byte : bytes) {
            result.push_back(digits[byte >> 4]);
            result.push_back(digits[byte & 15]);
        }
        return result;
    }

    void deliver_page(int from, int to, const std::string &text) {
        Client *target = find(to);
        if (!target || target->state != UnitState::Registered) {
            log("Pager destination unavailable: " + std::to_string(to));
            return;
        }
        std::string digits;
        std::copy_if(text.begin(), text.end(), std::back_inserter(digits), [](unsigned char ch) { return std::isdigit(ch); });
        if (digits.empty()) {
            log("Pager message rejected: enter one or more content digits");
            return;
        }
        const int channel = allocate_channel();
        if (!channel) {
            log("Pager delivery failed: no free AMPS voice channel");
            return;
        }
        const int sat = std::array<int,3>{5970,6000,6030}[channel % 3];
        target->peer = kPagerNumber;
        target->state = UnitState::Paging;
        // The network is the calling party.  Once Simon's Accept Pages mode
        // answers, the server sends #<content># as genuine in-band DTMF; the
        // handset's voice modem, rather than a private host popup, records it.
        Call page;
        page.called = to;
        page.channel = channel;
        page.sat = sat;
        page.stage = CallStage::Page;
        page.due = Clock::now() + 120ms;
        page.network_page = true;
        page.page_digits = "#" + digits + "#";
        m_calls.push_back(std::move(page));
        log("Network pager request -> " + std::to_string(to) + ", content digits: " + digits +
            (from ? " (submitted by " + std::to_string(from) + ")" : ""));
        refresh();
    }

    void pager_digit(Client &client, char digit) {
        std::string *field = client.pager_target.empty() || client.pager_message.empty() && client.service_input != "message"
            ? &client.pager_target : &client.pager_message;
        if (digit != '#') {
            field->push_back(digit);
            return;
        }
        if (field == &client.pager_target) {
            client.service_input = "message";
            log("Pager IVR destination accepted: " + client.pager_target);
            return;
        }
        int target = 0;
        try { target = std::stoi(client.pager_target); } catch (...) { }
        deliver_page(client.number, target, client.pager_message);
        client.pager_target.clear();
        client.pager_message.clear();
        client.service_input.clear();
    }

    void receive_voice(Client &source, const std::string &hex) {
        if (source.state != UnitState::Connected) return;
        if (Client *peer = find(source.peer); peer && peer->state == UnitState::Connected)
            send_line(*peer, "Y " + hex);
        const auto pcm = decode_hex(hex);
        if (const auto digit = source.dtmf.push(pcm)) {
            log("In-band DTMF from " + std::to_string(source.number) + ": " + std::string(1, *digit));
            if (Client *peer = find(source.peer))
                send_line(*peer, "E DTMF " + std::to_string(source.number) + " " + std::string(1, *digit));
            if (source.service == Service::Pager) pager_digit(source, *digit);
        }
    }

    void finish_mail(Client &client) {
        MailMessage message;
        message.from = client.mail_user.empty() ? std::to_string(client.number) : client.mail_user;
        std::istringstream stream(client.mail_message);
        std::string line;
        bool body = false;
        while (std::getline(stream, line)) {
            line = trim(line);
            if (!body && line.empty()) { body = true; continue; }
            if (!body && line.rfind("TO:", 0) == 0) message.to = trim(line.substr(3));
            else if (!body && line.rfind("FROM:", 0) == 0) message.from = trim(line.substr(5));
            else if (!body && line.rfind("SUBJECT:", 0) == 0) message.subject = trim(line.substr(8));
            else { message.body += line; message.body += "\r\n"; }
        }
        if (message.to.empty()) message.to = std::to_string(client.number);
        m_mailboxes[message.to].push_back(message);
        send_modem(client, "+OK QUEUED " + std::to_string(m_mailboxes[message.to].size()) + "\r\nMAIL> ");
        log("Mail relayed: " + message.from + " -> " + message.to + " [" + message.subject + "]");
        client.mail_message.clear();
        client.mail_message_mode = false;
    }

    void mail_command(Client &client, std::string line) {
        line = trim(std::move(line));
        if (client.mail_message_mode) {
            if (line == ".") finish_mail(client);
            else { client.mail_message += line; client.mail_message += "\r\n"; }
            return;
        }
        std::string upper = line;
        std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) { return char(std::toupper(c)); });
        if (upper.rfind("USER ", 0) == 0) {
            client.mail_user = trim(line.substr(5));
            send_modem(client, "+OK PASSWORD\r\nPASS ");
        } else if (upper.rfind("PASS ", 0) == 0) {
            send_modem(client, "+OK SIMON MAILBOX\r\nMAIL> ");
        } else if (upper == "SEND" || upper == "DATA") {
            client.mail_message_mode = true;
            client.mail_message.clear();
            send_modem(client, "+OK END WITH <CRLF>.<CRLF>\r\n");
        } else if (upper == "LIST") {
            auto &box = m_mailboxes[client.mail_user];
            std::string response = "+OK " + std::to_string(box.size()) + "\r\n";
            for (std::size_t i = 0; i < box.size(); ++i)
                response += std::to_string(i + 1) + " " + box[i].from + " " + box[i].subject + "\r\n";
            send_modem(client, response + ".\r\nMAIL> ");
        } else if (upper.rfind("RETR ", 0) == 0) {
            int index = 0;
            try { index = std::stoi(upper.substr(5)); } catch (...) { }
            auto &box = m_mailboxes[client.mail_user];
            if (index < 1 || std::size_t(index) > box.size()) send_modem(client, "-ERR NO MESSAGE\r\nMAIL> ");
            else {
                const auto &mail = box[std::size_t(index - 1)];
                send_modem(client, "+OK\r\nFrom: " + mail.from + "\r\nTo: " + mail.to + "\r\nSubject: " + mail.subject + "\r\n\r\n" + mail.body + ".\r\nMAIL> ");
            }
        } else if (upper == "QUIT") {
            send_modem(client, "+OK BYE\r\n");
            send_line(client, "M H");
            client.service = Service::None;
            client.state = UnitState::Registered;
        } else if (!line.empty()) {
            send_modem(client, "-ERR COMMAND\r\nMAIL> ");
        }
    }

    void receive_modem(Client &client, const std::string &hex) {
        const auto bytes = decode_hex(hex);
        if (client.service == Service::Echo) {
            send_line(client, "M X " + hex);
            return;
        }
        if (client.service == Service::Fax) {
            // The Simon Cirrus modem performs T.30; retain the octets so a
            // future image decoder can consume them without changing transport.
            log("Fax bearer: " + std::to_string(client.number) + " sent " + std::to_string(bytes.size()) + " octets");
            send_modem(client, "OK\r\n");
            return;
        }
        if (client.service != Service::Mail) return;
        if (client.mail_trace_bytes < 512) {
            const std::size_t count = std::min<std::size_t>(bytes.size(), 512 - client.mail_trace_bytes);
            log("cc:Mail RX " + std::to_string(client.number) + ": " + hex.substr(0, count * 2));
            client.mail_trace_bytes += count;
        }
        for (std::uint8_t byte : bytes) {
            client.service_input.push_back(char(byte));
            if (!client.mail_native_mode && byte == '\n') {
                const std::string line = client.service_input;
                client.service_input.clear();
                mail_command(client, line);
            }
        }

        // Stock Simon RCONNECT/NFT begins with an 11-octet binary hello.
        // Keep the legacy text test protocol available, but recognize the
        // native framing before treating its bytes as line-oriented commands.
        // Echoing this discovery frame is an intentional protocol probe; the
        // following frame tells us which fields the cc:Mail peer validates.
        while (client.service_input.size() >= 2) {
            const auto lead = std::uint8_t(client.service_input[0]);
            const auto size_code = std::uint8_t(client.service_input[1]);
            if (lead != 0x02) {
                client.service_input.erase(0, 1);
                continue;
            }
            if (size_code == 0x01) {
                if (client.service_input.size() < 6) break;
                const std::string acknowledgement = client.service_input.substr(0, 6);
                log("cc:Mail NFT acknowledgement: " + encode_hex(acknowledgement));
                client.service_input.erase(0, 6);
                continue;
            }
            if (size_code == 0x04) {
                if (client.service_input.size() < 11) break;
                client.mail_native_mode = true;
                const std::string frame = client.service_input.substr(0, 11);
                client.service_input.erase(0, 11);
                send_modem(client, frame);
                log("cc:Mail NFT discovery frame echoed: " + encode_hex(frame));
                continue;
            }
            break;
        }
    }

    void route_line(Client &client, const std::string &line) {
        if (line == "N") { client.rf_ready = true; send_profile(client); log("RF deck ready: " + std::to_string(client.number)); return; }
        if (line == "L") {
            for (const auto &[number, unit] : m_clients)
                send_line(client, "E UNIT " + std::to_string(number) + " " + (unit->rf_ready ? "READY" : "ATTACHED"));
            return;
        }
        if (line.rfind("T ", 0) == 0) {
            log("[" + std::to_string(client.number) + "] RF " + line);
            return;
        }
        if (line.rfind("P ", 0) == 0) return;
        if (line.rfind("O ", 0) == 0) { originate(client, trim(line.substr(2))); return; }
        if (line == "A") { answer(client); return; }
        if (line == "H") { release(client.number, false); return; }
        if (line.rfind("Y ", 0) == 0) { receive_voice(client, trim(line.substr(2))); return; }
        if (line.rfind("Q ", 0) == 0) {
            const std::string request = trim(line.substr(2));
            const auto split = request.find(' ');
            int target = 0;
            try { target = std::stoi(request.substr(0, split)); } catch (...) { }
            if (target) deliver_page(client.number, target, split == std::string::npos ? "" : request.substr(split + 1));
            return;
        }
        if (line.rfind("Z ", 0) == 0) {
            int sat = 0;
            try { sat = std::stoi(trim(line.substr(2))); } catch (...) { }
            if (sat == client.sat) client.last_sat = Clock::now();
            return;
        }
        if (line.rfind("M D ", 0) == 0) {
            const std::string digits = trim(line.substr(4));
            int number = 0;
            try { number = std::stoi(digits); } catch (...) { }
            const auto dialled = [&digits](int service) {
                const std::string suffix = std::to_string(service);
                return digits == suffix || (digits.size() > suffix.size() && digits.ends_with(suffix));
            };
            if (dialled(kDataNumber)) start_service(client, Service::Echo, kDataNumber);
            else if (dialled(kMailNumber)) start_service(client, Service::Mail, kMailNumber);
            else if (dialled(kFaxNumber)) start_service(client, Service::Fax, kFaxNumber);
            else send_line(client, "M N");
            return;
        }
        if (line.rfind("M X ", 0) == 0) { receive_modem(client, trim(line.substr(4))); return; }
        if (line == "M H") { send_line(client, "M H"); client.service = Service::None; client.state = UnitState::Registered; refresh(); return; }
        log("[" + std::to_string(client.number) + "] " + line);
    }

    void process_calls() {
        const auto now = Clock::now();
        for (auto &call : m_calls) {
            if (call.stage == CallStage::Connected || call.stage == CallStage::Ringing || now < call.due) continue;
            Client *caller = find(call.caller);
            Client *called = find(call.called);
            if ((!caller && !call.network_page) || !called) continue;
            if (call.stage == CallStage::Page) {
                log("FOCC page order -> " + std::to_string(called->number) + "; RECC page response received");
                call.stage = CallStage::Assign;
                call.due = now + 180ms;
            } else if (call.stage == CallStage::Assign) {
                called->channel = call.channel;
                called->sat = call.sat;
                if (caller) {
                    caller->channel = call.channel;
                    caller->sat = call.sat;
                    send_line(*caller, "D " + std::to_string(caller->sid) + " " + std::to_string(call.channel) + " " + std::to_string(call.sat));
                }
                send_line(*called, "D " + std::to_string(called->sid) + " " + std::to_string(call.channel) + " " + std::to_string(call.sat));
                log("FVC assignment: channel " + std::to_string(call.channel) + ", SAT " + std::to_string(call.sat));
                call.stage = CallStage::Alert;
                call.due = now + 80ms;
            } else if (call.stage == CallStage::Alert) {
                // R means that page response, channel assignment and SAT
                // acquisition are complete.  The deck latches A7 before 6A.
				const int calling_number = call.network_page ? kPagerNumber : caller->number;
				// P distinguishes unattended numeric paging from an ordinary
				// incoming voice call before the RF deck raises any host UI flags.
				send_line(*called, "R " + std::to_string(calling_number) + (call.network_page ? " P" : ""));
                called->state = UnitState::Ringing;
                if (caller) {
                    caller->state = UnitState::Ringing;
                    caller->tone_sample = 0;
                }
                call.stage = CallStage::Ringing;
                log("Alert order: " + std::to_string(calling_number) + " -> " + std::to_string(called->number));
                refresh();
            }
        }
    }

    void send_tone(Client &client, double f1, double f2, int on_samples, int period_samples) {
        static constexpr char hex[] = "0123456789ABCDEF";
        // 160 samples per 20 ms server tick, framed in the driver's native 16-sample packets.
        for (int packet = 0; packet < 10; ++packet) {
            std::string line = "Y ";
            for (int i = 0; i < 16; ++i) {
                const auto n = client.tone_sample++;
                const bool on = int(n % std::size_t(period_samples)) < on_samples;
                int value = 128;
                if (on) value += int(28.0 * std::sin(2.0 * kPi * f1 * n / 8000.0) + 28.0 * std::sin(2.0 * kPi * f2 * n / 8000.0));
                value = std::clamp(value, 0, 255);
                line.push_back(hex[value >> 4]);
                line.push_back(hex[value & 15]);
            }
            send_line(client, line);
        }
    }

    void send_page_digit(Client &client, Call &call) {
        static constexpr char hex[] = "0123456789ABCDEF";
        const char digit = call.page_digits[call.page_position];
        double low = 0.0, high = 0.0;
        switch (digit) {
        case '1': low = 697; high = 1209; break; case '2': low = 697; high = 1336; break; case '3': low = 697; high = 1477; break;
        case '4': low = 770; high = 1209; break; case '5': low = 770; high = 1336; break; case '6': low = 770; high = 1477; break;
        case '7': low = 852; high = 1209; break; case '8': low = 852; high = 1336; break; case '9': low = 852; high = 1477; break;
        case '*': low = 941; high = 1209; break; case '0': low = 941; high = 1336; break; case '#': low = 941; high = 1477; break;
        default: break;
        }
        // 120 ms tone followed by 80 ms silence.  It is transported as the
        // same 8 kHz unsigned PCM Y bearer used by calls between two Simons.
        for (int packet = 0; packet < 10; ++packet) {
            std::string line = "Y ";
            for (int index = 0; index < 16; ++index) {
                const std::size_t sample = call.page_samples++;
                int value = 128;
                if (sample < 960 && low && high)
                    value += int(28.0 * std::sin(2.0 * kPi * low * sample / 8000.0) +
                                 28.0 * std::sin(2.0 * kPi * high * sample / 8000.0));
                value = std::clamp(value, 0, 255);
                line.push_back(hex[value >> 4]);
                line.push_back(hex[value & 15]);
            }
            send_line(client, line);
        }
        if (call.page_samples >= 1600) {
            call.page_samples = 0;
            ++call.page_position;
            if (call.page_position == call.page_digits.size())
                call.due = Clock::now() + 800ms;
        }
    }

    void service_audio() {
        const auto now = Clock::now();
        std::vector<int> completed_pages;
        for (auto &[number, holder] : m_clients) {
            Client &client = *holder;
            if (client.state == UnitState::BusyTone) send_tone(client, 480, 620, 4000, 8000);
            else if (client.state == UnitState::Ringing) {
                const auto it = std::find_if(m_calls.begin(), m_calls.end(), [number](const Call &call) { return call.caller == number; });
                if (it != m_calls.end()) send_tone(client, 440, 480, 16000, 48000);
            }
        }
        for (Call &call : m_calls) {
            if (!call.network_page || call.stage != CallStage::Connected || now < call.due) continue;
            Client *called = find(call.called);
            if (!called) continue;
            if (call.page_position < call.page_digits.size())
                send_page_digit(*called, call);
            else
                completed_pages.push_back(call.called);
        }
        for (int number : completed_pages) {
            log("Pager digits accepted by Simon " + std::to_string(number));
            release(number, true);
        }
    }

    void supervise_sat() {
        const auto now = Clock::now();
        std::vector<int> lost;
        for (const auto &[number, client] : m_clients)
            if (client->state == UnitState::Connected && client->service == Service::None && now - client->last_sat > 5s)
                lost.push_back(number);
        for (int number : lost) {
            log("SAT lost for " + std::to_string(number) + "; releasing voice channel");
            release(number, true);
        }
    }

    void process_actions() {
        std::deque<Action> actions;
        {
            std::lock_guard lock(m_action_mutex);
            actions.swap(m_actions);
        }
        for (const Action &action : actions) {
            if (action.type == ActionType::Call) { if (Client *client = find(action.from)) originate(*client, std::to_string(action.to)); }
            else if (action.type == ActionType::Connect) { if (Client *client = find(action.to)) answer(*client); }
            else if (action.type == ActionType::Hang) release(action.from, true);
            else if (action.type == ActionType::Page) deliver_page(action.from, action.to, action.text);
            else if (action.type == ActionType::Profile) {
                if (Client *client = find(action.from)) {
                    client->registration = action.registration;
                    client->signal = std::clamp(action.signal, 0, 6);
                    client->operator_name = action.operator_name.empty() ? "Simon AMPS" : action.operator_name;
                    send_profile(*client);
                    log("Profile updated for " + std::to_string(client->number));
                    refresh();
                }
            }
        }
    }

    void refresh() {
        std::vector<UnitSnapshot> next;
        for (const auto &[number, client] : m_clients)
            next.push_back(UnitSnapshot{number, state_name(client->state), client->peer, client->channel, client->sat,
                client->registration, client->signal, client->sid, client->operator_name});
        {
            std::lock_guard lock(m_snapshot_mutex);
            m_snapshots = std::move(next);
        }
        if (m_notify) PostMessageW(m_notify, WM_SWITCH_REFRESH, 0, 0);
    }

    void disconnect(int number) {
        Client *client = find(number);
        if (!client) return;
        const int peer = client->peer;
        closesocket(client->socket);
        m_clients.erase(number);
        if (Client *other = find(peer)) {
            send_line(*other, "H");
            other->peer = 0;
            other->state = UnitState::Registered;
            other->channel = kControlChannel;
            other->sat = 6000;
            other->service = Service::None;
            send_channel(*other);
        }
        std::erase_if(m_calls, [number](const Call &call) { return call.caller == number || call.called == number; });
        log("Simon disconnected: " + std::to_string(number));
        refresh();
    }

    void accept_clients() {
        for (;;) {
            SOCKET socket = accept(m_listener, nullptr, nullptr);
            if (socket == INVALID_SOCKET) break;
            u_long nonblocking = 1;
            ioctlsocket(socket, FIONBIO, &nonblocking);
            BOOL no_delay = TRUE;
            setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&no_delay), sizeof(no_delay));
            auto client = std::make_unique<Client>();
            client->socket = socket;
            client->number = m_next_number++;
            const int number = client->number;
            m_clients[number] = std::move(client);
            send_profile(*m_clients[number]);
            log("Simon connected; MIN/number " + std::to_string(number) + ", control channel " + std::to_string(kControlChannel));
            refresh();
        }
    }

    void read_clients() {
        std::vector<int> closed;
        std::array<char, 4096> buffer{};
        std::vector<int> order;
        for (const auto &[number, client] : m_clients) order.push_back(number);
        // Rotate the first socket and bound each turn.  A continuous PCM
        // stream from a low-number mobile must not starve later mobiles' A/H
        // orders, and number assignment must not change call responsiveness.
        const auto first = std::upper_bound(order.begin(), order.end(), m_last_read_number);
        std::rotate(order.begin(), first, order.end());
        for (int number : order) {
            Client *client = find(number);
            for (int packet = 0; packet < 4; ++packet) {
                const int count = recv(client->socket, buffer.data(), int(buffer.size()), 0);
                if (count > 0) client->input.append(buffer.data(), std::size_t(count));
                else if (count == 0) { closed.push_back(number); break; }
                else {
                    const int error = WSAGetLastError();
                    if (error != WSAEWOULDBLOCK) closed.push_back(number);
                    break;
                }
            }
            for (int line_count = 0; line_count < 256; ++line_count) {
                const auto newline = client->input.find('\n');
                if (newline == std::string::npos) break;
                const std::string line = trim(client->input.substr(0, newline));
                client->input.erase(0, newline + 1);
                if (!line.empty()) route_line(*client, line);
            }
            if (client->input.size() > 1024 * 1024) closed.push_back(number);
        }
        if (!order.empty()) m_last_read_number = order.front();
        std::sort(closed.begin(), closed.end());
        closed.erase(std::unique(closed.begin(), closed.end()), closed.end());
        for (int number : closed) disconnect(number);
    }

    void flush_clients() {
        std::vector<int> closed;
        for (auto &[number, client] : m_clients)
            if (client->transport_failed || !flush_client(*client)) closed.push_back(number);
        for (int number : closed) disconnect(number);
    }

    void wait_for_io(Clock::time_point audio_due) {
        fd_set readable, writable;
        FD_ZERO(&readable);
        FD_ZERO(&writable);
        FD_SET(m_listener, &readable);
        for (const auto &[number, client] : m_clients) {
            // Complete buffered lines need another bounded processing turn;
            // they will not cause a new socket-readability notification.
            if (client->input.find('\n') != std::string::npos) return;
            FD_SET(client->socket, &readable);
            if (client->output_position < client->output.size()) FD_SET(client->socket, &writable);
        }
        // GUI actions are queued from another thread, so poll them at least
        // every 5 ms even when no network socket is ready.
        const auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(audio_due - Clock::now());
        const auto delay = std::clamp(remaining, 0us, 5000us);
        timeval timeout{0, long(delay.count())};
        select(0, &readable, &writable, nullptr, &timeout);
    }

    void run() {
        { std::ofstream clear("amps-switch.log", std::ios::trunc | std::ios::binary); }
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) { log("WSAStartup failed"); m_running = false; return; }
        m_listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(std::uint16_t(m_port));
        BOOL reuse = TRUE;
        setsockopt(m_listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&reuse), sizeof(reuse));
        if (bind(m_listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == SOCKET_ERROR || listen(m_listener, 8) == SOCKET_ERROR) {
            log("Cannot listen on 127.0.0.1:" + std::to_string(m_port) + " (is another switch running?)");
            closesocket(m_listener);
            m_listener = INVALID_SOCKET;
            WSACleanup();
            m_running = false;
            return;
        }
        u_long nonblocking = 1;
        ioctlsocket(m_listener, FIONBIO, &nonblocking);
        log("AMPS switch listening on 127.0.0.1:" + std::to_string(m_port));
        log("Services: data " + std::to_string(kDataNumber) + ", mail " + std::to_string(kMailNumber) +
            ", fax " + std::to_string(kFaxNumber) + ", pager " + std::to_string(kPagerNumber));
        auto audio_due = Clock::now() + 20ms;
        while (m_running) {
            accept_clients();
            read_clients();
            process_actions();
            process_calls();
            // An absolute sample clock maintains 8 kHz despite processing
            // time and Windows timer granularity.  sleep_for(20ms) after each
            // 160-sample batch accumulated a deficit on every iteration.
            int audio_batches = 0;
            while (Clock::now() >= audio_due && audio_batches < 5) {
                service_audio();
                audio_due += 20ms;
                ++audio_batches;
            }
            // Do not replay seconds of stale sound after a debugger/host pause.
            if (Clock::now() >= audio_due + 100ms) audio_due = Clock::now() + 20ms;
            supervise_sat();
            flush_clients();
            wait_for_io(audio_due);
        }
        for (auto &[number, client] : m_clients) closesocket(client->socket);
        m_clients.clear();
        if (m_listener != INVALID_SOCKET) closesocket(m_listener);
        m_listener = INVALID_SOCKET;
        WSACleanup();
    }
};

HWND g_window = nullptr;
HWND g_units = nullptr;
HWND g_log = nullptr;
HWND g_call_from_label = nullptr;
HWND g_call_to_label = nullptr;
HWND g_from = nullptr;
HWND g_to = nullptr;
HWND g_page_to_label = nullptr;
HWND g_page_digits_label = nullptr;
HWND g_page_to = nullptr;
HWND g_page_text = nullptr;
HWND g_profile_label = nullptr;
HWND g_registration = nullptr;
HWND g_signal = nullptr;
HWND g_operator = nullptr;
std::unique_ptr<SwitchServer> g_server;
int g_listen_port = kDefaultPort;

HWND control(const wchar_t *kind, const wchar_t *text, DWORD style, int x, int y, int w, int h, int id) {
    return CreateWindowExW(0, kind, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, g_window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
}

int int_text(HWND handle) {
    wchar_t buffer[64]{};
    GetWindowTextW(handle, buffer, 64);
    return _wtoi(buffer);
}

std::string utf8_text(HWND handle) {
    const int count = GetWindowTextLengthW(handle);
    std::wstring value(std::size_t(count), L'\0');
    GetWindowTextW(handle, value.data(), count + 1);
    return narrow(value);
}

void refresh_list() {
    if (!g_server || !g_units) return;
    const auto units = g_server->snapshots();
    ListView_DeleteAllItems(g_units);
    int row = 0;
    for (const auto &unit : units) {
        std::array<std::wstring, 9> values{
            std::to_wstring(unit.number), widen(unit.state), unit.peer ? std::to_wstring(unit.peer) : L"-",
            std::to_wstring(unit.channel), std::to_wstring(unit.sat), widen(unit.registration),
            std::to_wstring(unit.signal), std::to_wstring(unit.sid), widen(unit.operator_name)
        };
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = row;
        item.pszText = values[0].data();
        ListView_InsertItem(g_units, &item);
        for (int column = 1; column < int(values.size()); ++column) ListView_SetItemText(g_units, row, column, values[column].data());
        ++row;
    }
}

void add_columns() {
    const std::array<std::pair<const wchar_t *, int>, 9> columns{{
        {L"Number",75}, {L"State",95}, {L"Peer",75}, {L"Channel",70}, {L"SAT",60},
        {L"Registration",90}, {L"RSSI",55}, {L"SID",55}, {L"Operator",130}
    }};
    for (int i = 0; i < int(columns.size()); ++i) {
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH;
        column.pszText = const_cast<wchar_t *>(columns[i].first);
        column.cx = columns[i].second;
        ListView_InsertColumn(g_units, i, &column);
    }
}

void layout(int width, int height) {
    if (!g_units) return;
    const int margin = 10;
    const int list_height = std::max(140, height / 3);
    MoveWindow(g_units, margin, margin, width - margin * 2, list_height, TRUE);
    const int controls_y = margin + list_height + 8;
    MoveWindow(g_call_from_label, 10, controls_y + 3, 58, 20, TRUE);
    MoveWindow(g_from, 70, controls_y, 75, 24, TRUE);
    MoveWindow(g_call_to_label, 153, controls_y + 3, 22, 20, TRUE);
    MoveWindow(g_to, 177, controls_y, 83, 24, TRUE);
    MoveWindow(GetDlgItem(g_window, IDC_CALL), 270, controls_y, 75, 24, TRUE);
    MoveWindow(GetDlgItem(g_window, IDC_CONNECT), 350, controls_y, 75, 24, TRUE);
    MoveWindow(GetDlgItem(g_window, IDC_HANG), 430, controls_y, 75, 24, TRUE);
    MoveWindow(g_page_to_label, 10, controls_y + 35, 58, 20, TRUE);
    MoveWindow(g_page_to, 70, controls_y + 32, 75, 24, TRUE);
    MoveWindow(g_page_digits_label, 153, controls_y + 35, 48, 20, TRUE);
    MoveWindow(g_page_text, 203, controls_y + 32, 142, 24, TRUE);
    MoveWindow(GetDlgItem(g_window, IDC_PAGE), 350, controls_y + 32, 100, 24, TRUE);
    MoveWindow(g_profile_label, 10, controls_y + 67, 45, 20, TRUE);
    MoveWindow(g_registration, 55, controls_y + 64, 90, 200, TRUE);
    MoveWindow(g_signal, 150, controls_y + 64, 45, 24, TRUE);
    MoveWindow(g_operator, 200, controls_y + 64, 145, 24, TRUE);
    MoveWindow(GetDlgItem(g_window, IDC_APPLY), 350, controls_y + 64, 75, 24, TRUE);
    MoveWindow(GetDlgItem(g_window, IDC_CLEAR), width - 90, controls_y + 64, 80, 24, TRUE);
    MoveWindow(g_log, margin, controls_y + 96, width - margin * 2, std::max(80, height - controls_y - 106), TRUE);
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_CREATE: {
        g_window = window;
        g_units = control(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS, 0, 0, 0, 0, IDC_UNITS);
        ListView_SetExtendedListViewStyle(g_units, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
        add_columns();
        g_call_from_label = control(L"STATIC", L"Call from", 0, 0, 0, 0, 0, -1);
        g_from = control(L"EDIT", L"1001", WS_BORDER | ES_NUMBER, 0, 0, 0, 0, IDC_FROM);
        g_call_to_label = control(L"STATIC", L"to", 0, 0, 0, 0, 0, -1);
        g_to = control(L"EDIT", L"1002", WS_BORDER | ES_NUMBER, 0, 0, 0, 0, IDC_TO);
        control(L"BUTTON", L"Call", BS_PUSHBUTTON, 0, 0, 0, 0, IDC_CALL);
        control(L"BUTTON", L"Answer", BS_PUSHBUTTON, 0, 0, 0, 0, IDC_CONNECT);
        control(L"BUTTON", L"Hang", BS_PUSHBUTTON, 0, 0, 0, 0, IDC_HANG);
        g_page_to_label = control(L"STATIC", L"Page to", 0, 0, 0, 0, 0, -1);
        g_page_to = control(L"EDIT", L"1002", WS_BORDER | ES_NUMBER, 0, 0, 0, 0, IDC_PAGE_TO);
        g_page_digits_label = control(L"STATIC", L"Digits", 0, 0, 0, 0, 0, -1);
        g_page_text = control(L"EDIT", L"123", WS_BORDER | ES_NUMBER, 0, 0, 0, 0, IDC_PAGE_TEXT);
        control(L"BUTTON", L"Send network page", BS_PUSHBUTTON, 0, 0, 0, 0, IDC_PAGE);
        g_profile_label = control(L"STATIC", L"Profile", 0, 0, 0, 0, 0, -1);
        g_registration = control(L"COMBOBOX", L"", CBS_DROPDOWNLIST, 0, 0, 0, 0, IDC_REGISTRATION);
        for (const wchar_t *item : {L"HOME1",L"HOME2",L"HOME3",L"HOME4",L"ROAM",L"ALTROAM",L"OFFLINE"})
            SendMessageW(g_registration, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item));
        SendMessageW(g_registration, CB_SETCURSEL, 0, 0);
        g_signal = control(L"EDIT", L"6", WS_BORDER | ES_NUMBER, 0, 0, 0, 0, IDC_SIGNAL);
        g_operator = control(L"EDIT", L"Simon AMPS", WS_BORDER, 0, 0, 0, 0, IDC_OPERATOR);
        control(L"BUTTON", L"Apply", BS_PUSHBUTTON, 0, 0, 0, 0, IDC_APPLY);
        control(L"BUTTON", L"Clear log", BS_PUSHBUTTON, 0, 0, 0, 0, IDC_CLEAR);
        g_log = control(L"EDIT", L"", WS_BORDER | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, 0, 0, 0, 0, IDC_LOG);
        HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        EnumChildWindows(window, [](HWND child, LPARAM value) -> BOOL { SendMessageW(child, WM_SETFONT, value, TRUE); return TRUE; }, reinterpret_cast<LPARAM>(font));
        g_server = std::make_unique<SwitchServer>(window);
        g_server->start(g_listen_port);
        return 0;
    }
    case WM_SIZE: layout(LOWORD(lparam), HIWORD(lparam)); return 0;
    case WM_COMMAND:
        if (HIWORD(wparam) == BN_CLICKED && g_server) {
            const int from = int_text(g_from), to = int_text(g_to);
            switch (LOWORD(wparam)) {
            case IDC_CALL: g_server->action(Action{ActionType::Call, from, to}); break;
            case IDC_CONNECT: g_server->action(Action{ActionType::Connect, from, to}); break;
            case IDC_HANG: g_server->action(Action{ActionType::Hang, from}); break;
            case IDC_PAGE: g_server->action(Action{ActionType::Page, 0, int_text(g_page_to), 6, utf8_text(g_page_text)}); break;
            case IDC_APPLY: {
                wchar_t registration[32]{};
                GetWindowTextW(g_registration, registration, 32);
                Action action{ActionType::Profile, from};
                action.signal = int_text(g_signal);
                action.registration = narrow(registration);
                action.operator_name = utf8_text(g_operator);
                g_server->action(std::move(action));
                break;
            }
            case IDC_CLEAR: SetWindowTextW(g_log, L""); break;
            }
        }
        return 0;
    case WM_SWITCH_LOG: {
        std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring *>(lparam));
        const int length = GetWindowTextLengthW(g_log);
        SendMessageW(g_log, EM_SETSEL, length, length);
        SendMessageW(g_log, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(text->c_str()));
        return 0;
    }
    case WM_SWITCH_REFRESH: refresh_list(); return 0;
    case WM_DESTROY:
        if (g_server) { g_server->stop(); g_server.reset(); }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR command_line, int show) {
    // A non-default port is useful for unattended regression tests and avoids
    // colliding with streaming/serial tools on a developer workstation.
    if (command_line) {
        const std::wstring arguments(command_line);
        const auto marker = arguments.find(L"--port");
        if (marker != std::wstring::npos) {
            const wchar_t *number = arguments.c_str() + marker + 6;
            while (*number == L' ' || *number == L'=') ++number;
            const long parsed = std::wcstol(number, nullptr, 10);
            if (parsed >= 1024 && parsed <= 65535) g_listen_port = int(parsed);
        }
    }
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES};
    InitCommonControlsEx(&controls);
    WNDCLASSEXW klass{sizeof(klass)};
    klass.lpfnWndProc = window_proc;
    klass.hInstance = instance;
    klass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    klass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    klass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    klass.lpszClassName = L"SimonAmpsSwitchWindow";
    RegisterClassExW(&klass);
    HWND window = CreateWindowExW(0, klass.lpszClassName, L"Simon AMPS Laboratory Switch", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 900, 690, nullptr, nullptr, instance, nullptr);
    if (!window) return 1;
    ShowWindow(window, show);
    UpdateWindow(window);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return int(message.wParam);
}
