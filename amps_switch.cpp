// Simon AMPS Laboratory Switch
//
// A native Win32 control panel and localhost cellular switch for the IBM
// Simon MAME driver.  The implementation deliberately separates an AMPS-like
// RF control plane (registration, paging, voice-channel assignment, SAT/ST)
// from the analogue bearer (8 kHz unsigned PCM).  It is not an RF transmitter,
// but the state transitions exposed to the emulated Mitsubishi deck follow the
// same order as a cellular call.

#ifndef UNICODE
#define UNICODE
#endif
#define _UNICODE
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

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
#include <format>
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

#include "weber_gateway.h"
#include "mail_store.h"
#include "mail_worker.h"
#include "ccmail_link.h"
#include "ccmail_session.h"
#include "switch_log.h"
#include "fax_link.h"

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

namespace {

constexpr int kFirstNumber = 1001;
constexpr int kLastNumber = 1999;
constexpr int kWirelineNumberOffset = 1000;
constexpr int kControlChannel = 334;
constexpr int kVoiceFirst = 354;
constexpr int kVoiceLast = 666;
constexpr int kMailNumber = 5551001;
constexpr int kDataNumber = 5551000;
constexpr int kWebNumber = 5551003;
constexpr int kPagerNumber = 5552000;
constexpr double kPi = 3.14159265358979323846;

constexpr UINT WM_SWITCH_LOG = WM_APP + 1;
constexpr UINT WM_SWITCH_REFRESH = WM_APP + 2;
constexpr UINT WM_MAIL_REGISTERED = WM_APP + 3;

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
    IDC_MAIL_NAME, IDC_MAIL_PASSWORD, IDC_REGISTER,
    IDC_BIND_ADDRESS, IDC_BIND_PORT, IDC_SAVE_LISTENER,
    IDC_FROM_LABEL, IDC_TO_LABEL, IDC_PAGE_TO_LABEL, IDC_DIGITS_LABEL, IDC_PROFILE_LABEL,
    IDC_MAIL_NAME_LABEL, IDC_PASSWORD_LABEL, IDC_ADDRESS_LABEL, IDC_PORT_LABEL,
    IDC_SIGNAL_LABEL, IDC_OPERATOR_LABEL,
    IDM_ENGLISH = 40001, IDM_CHINESE,
};

enum class UnitState { RadioOff, Registered, Originating, Paging, Ringing, Connected, BusyTone, Data };
enum class CallStage { Page, Assign, Alert, Ringing, Connected };
enum class Service { None, Echo, Mail, Pager, Emergency, Web };
enum class WirelineState { OnHook, DialTone, Collecting, Calling, Ringing, Connected, Ended };

struct UiLabel {
    const wchar_t *english;
    const wchar_t *chinese;
};

bool g_chinese = false;
const wchar_t *display(UiLabel label) { return g_chinese ? label.chinese : label.english; }

UiLabel state_name(UnitState state) {
    switch (state) {
    case UnitState::RadioOff: return {L"RF off", L"射频已关闭"};
    case UnitState::Registered: return {L"Registered", L"已入网"};
    case UnitState::Originating: return {L"Originating", L"正在发起呼叫"};
    case UnitState::Paging: return {L"Paging", L"正在寻呼"};
    case UnitState::Ringing: return {L"Ringing", L"正在响铃"};
    case UnitState::Connected: return {L"Connected", L"已接通"};
    case UnitState::BusyTone: return {L"Busy tone", L"忙音"};
    case UnitState::Data: return {L"Data", L"数据连接"};
    }
    return {L"?", L"?"};
}

UiLabel wireline_state_name(WirelineState state) {
    switch (state) {
    case WirelineState::OnHook: return {L"Wall jack on-hook", L"座机已挂机"};
    case WirelineState::DialTone: return {L"Wall jack dial tone", L"座机拨号音"};
    case WirelineState::Collecting: return {L"Wall jack dialling", L"座机正在拨号"};
    case WirelineState::Calling: return {L"Wall jack ringing", L"座机正在回铃"};
    case WirelineState::Ringing: return {L"Wall jack incoming", L"座机来电"};
    case WirelineState::Connected: return {L"Wall jack connected", L"座机已接通"};
    case WirelineState::Ended: return {L"Wall jack ended/busy", L"座机已结束／占线"};
    }
    return {L"?", L"?"};
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
    char reported = 0;
    int quiet = 0;

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
            if (!found) {
                if (++quiet >= 2) reported = 0;
                candidate = 0;
                stable = 0;
                continue;
            }
            quiet = 0;
            if (found == candidate) ++stable;
            else { candidate = found; stable = 1; }
            if (stable >= 2 && found != reported) {
                // A held key is one digit.  Two quiet detection windows rearm
                // the same digit; a stable different tone may start immediately.
                reported = found;
                return found;
            }
        }
        return std::nullopt;
    }
};

struct MailMessage {
    std::string to;
    std::string subject;
    std::string body;
};

struct Client : switch_store::Profile {
    SOCKET socket = INVALID_SOCKET;
    int number = 0;
    int wireline_number = 0;
    bool wireline_present = false;
    std::string input;
    std::string output;
    std::size_t output_position = 0;
    bool transport_failed = false;
    int peer = 0;
    int peer_address = 0;
    UnitState state = UnitState::RadioOff;
    int sid = 1;
    int channel = 0;
    int sat = 0;
    Clock::time_point last_sat = Clock::now();
    std::size_t tone_sample = 0;
    Service service = Service::None;
    std::string service_input;
    std::string mail_user;
    bool mail_authenticated = false;
    bool mail_message_mode = false;
    bool mail_native_mode = false;
    ccmail::Link mail_link;
    ccmail::Session mail_session;
    std::string mail_message;
    std::uint64_t login_request = 0;
    std::string after_login;
    std::string pager_target;
    std::string pager_message;
    DtmfDetector dtmf;
    bool rf_ready = false;
    // The TCP link also carries the independent RJ11/COM2 modem path.  A
    // connected socket, or a wireline data service, is not RF registration.
    bool service_rf_bearer = false;
    bool wireline_offhook = false;
    WirelineState wireline_state = WirelineState::OnHook;
    std::string wireline_digits;
    Clock::time_point wireline_digit_due{};
    DtmfDetector wireline_dtmf;
    FaxLink fax;
    std::uint64_t web_request = 0;
    std::string web_response;
    std::size_t web_position = 0;
    Clock::time_point web_due{};
    bool web_cellular = false;
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
    bool caller_wireline = false;
    bool called_wireline = false;
    int calling_number = 0;
    int called_number = 0;
    bool ring_high = false;
    Clock::time_point ring_due{};
};

enum class ActionType { Call, Connect, Hang, Page, Profile, LoginReply, WebReply };
struct Action {
    ActionType type{};
    int from = 0;
    int to = 0;
    int signal = 6;
    std::string text{};
    std::string registration{};
    std::string operator_name{};
    std::uint64_t request = 0;
    bool accepted = false;
};

struct UnitSnapshot {
    int number = 0;
    int wireline_number = 0;
    bool wireline_present = false;
    UiLabel state;
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

    void start(const switch_store::Listener &listener) {
        m_running = true;
        m_endpoint = listener;
        m_thread = std::thread([this] { run(); });
    }

    void stop() {
        m_running = false;
        if (m_thread.joinable()) m_thread.join();
    }

    void action(Action action) {
        std::lock_guard lock(m_action_mutex);
        m_actions.push_back(std::move(action));
    }

    bool register_mailbox(std::string name, std::string password) {
        return m_accounts.submit([window = m_notify, name = std::move(name), password = std::move(password)] {
            PostMessageW(window, WM_MAIL_REGISTERED, postoffice::create(name, password), 0);
        });
    }

    std::vector<UnitSnapshot> snapshots() const {
        std::lock_guard lock(m_snapshot_mutex);
        return m_snapshots;
    }

private:
    HWND m_notify = nullptr;
    switch_store::Listener m_endpoint;
    std::atomic_bool m_running{false};
    std::thread m_thread;
    SOCKET m_listener = INVALID_SOCKET;
    int m_next_number = kFirstNumber;
    int m_next_voice = kVoiceFirst;
    int m_last_read_number = 0;
    std::map<int, std::unique_ptr<Client>> m_clients;
    std::vector<Call> m_calls;

    mutable std::mutex m_snapshot_mutex;
    std::vector<UnitSnapshot> m_snapshots;
    std::mutex m_action_mutex;
    std::deque<Action> m_actions;
    simon_weber::Gateway m_web_gateway;
    std::uint64_t m_next_request = 1;
    postoffice::AccountWorker m_accounts;

    void write_log(const std::string &english, const std::string &chinese) {
        SYSTEMTIME st{};
        GetLocalTime(&st);
        char prefix[32];
        std::snprintf(prefix, sizeof(prefix), "%02u:%02u:%02u.%03u  ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        std::ofstream trace("amps-switch.log", std::ios::app | std::ios::binary);
        trace << prefix << english << "\r\n";
        if (m_notify) {
            auto *message = new SwitchLogMessage{
                widen(prefix + english + "\r\n"), widen(prefix + chinese + "\r\n")};
            if (!PostMessageW(m_notify, WM_SWITCH_LOG, 0, reinterpret_cast<LPARAM>(message))) delete message;
        }
    }

    template <typename... Args>
    void log(std::string_view english, std::string_view chinese, const Args &...values) {
        const auto args = std::make_format_args(values...);
        write_log(std::vformat(english, args), std::vformat(chinese, args));
    }

    void log_protocol(const std::string &line) { write_log(line, line); }

    Client *find(int number) {
        auto it = m_clients.find(number);
        return it == m_clients.end() ? nullptr : it->second.get();
    }

    Client *find_address(int number, bool &wireline) {
        wireline = false;
        if (Client *client = find(number)) return client;
        for (const auto &[key, client] : m_clients) {
            if (client->wireline_number == number) {
                wireline = true;
                return client.get();
            }
        }
        return nullptr;
    }

    static bool wireline_active(const Client &client) {
        // Ring voltage is present while the receiver is still on-hook.  Do
        // not mistake an unanswered wall-jack call for a cellular call.
        return client.wireline_offhook || client.wireline_state == WirelineState::Ringing;
    }

    int allocate_number() {
        // Reserve disjoint mobile/line pools for the lifetime of this socket.
        // Reinserted accessories retain the same pair, not a global identity
        // across switch restarts or newly established transport sessions.
        for (int attempt = kFirstNumber; attempt <= kLastNumber; ++attempt) {
            const int number = m_next_number++;
            if (m_next_number > kLastNumber) m_next_number = kFirstNumber;
            if (!m_clients.contains(number)) return number;
        }
        return 0;
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
        if (!client.rf_ready) return;
        send_line(client, "D " + std::to_string(client.sid) + " " + std::to_string(client.channel) + " " + std::to_string(client.sat));
    }

    void send_profile(Client &client) {
        if (!client.rf_ready) return;
        send_channel(client);
        send_line(client, "S " + client.registration + " " + std::to_string(client.signal) + " " + client.operator_name);
    }

    void register_radio(Client &client) {
        // N is emitted when the emulated deck is actually powered on.  Do not
        // repeat the operator/status notification for an already-ready deck.
        if (client.rf_ready) return;
        client.rf_ready = true;
        if (client.state == UnitState::RadioOff) client.state = UnitState::Registered;
        client.channel = kControlChannel;
        client.sat = 6000;
        send_profile(client);
        log("RF registered: {}, control channel {}",
            "射频模块已入网：{}，控制信道 {}", client.number, kControlChannel);
        refresh();
    }

    void power_off_radio(Client &client) {
        if (!client.rf_ready) return;
        client.rf_ready = false;
        // Powering down the RF deck releases cellular voice/data, but must not
        // interrupt a separately connected wall-jack modem service.
        if (!wireline_active(client) && (client.service == Service::None || client.service_rf_bearer))
            release(client.number, false);
        client.channel = 0;
        client.sat = 0;
        if (client.service == Service::None && !wireline_active(client)) client.state = UnitState::RadioOff;
        log("RF powered off/unregistered: {}",
            "射频模块已关机并注销：{}", client.number);
        refresh();
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

    void begin_call(Client &source, int target_number, bool source_wireline = false) {
        if (source_wireline) {
            if (!source.wireline_present || !source.wireline_offhook ||
                (source.wireline_state != WirelineState::DialTone && source.wireline_state != WirelineState::Collecting)) return;
        } else if (!source.rf_ready || wireline_active(source) || source.state != UnitState::Registered) return;
        bool target_wireline = false;
        Client *target = find_address(target_number, target_wireline);
        const bool idle = target && target != &source && target->service == Service::None && !target->peer &&
            (target_wireline ? target->wireline_present && !wireline_active(*target) &&
                (target->state == UnitState::RadioOff || target->state == UnitState::Registered)
                : target->rf_ready && !wireline_active(*target) && target->state == UnitState::Registered);
        const int calling_number = source_wireline ? source.wireline_number : source.number;
        if (!idle) {
            send_line(source, source_wireline ? "M V B" : "B");
            source.state = UnitState::BusyTone;
            if (source_wireline) source.wireline_state = WirelineState::Ended;
            source.tone_sample = 0;
            log("Busy/unavailable destination: {} -> {}",
                "被叫忙或不可达：{} -> {}", calling_number, target_number);
            refresh();
            return;
        }
        // Only cellular legs need a forward/reverse RF channel and SAT.
        // A wall-jack-to-wall-jack conversation has neither radio endpoint.
        const bool needs_radio = !source_wireline || !target_wireline;
        const int channel = needs_radio ? allocate_channel() : 0;
        if (needs_radio && !channel) {
            send_line(source, source_wireline ? "M V B" : "I 83");
            if (source_wireline) {
                source.state = UnitState::BusyTone;
                source.wireline_state = WirelineState::Ended;
                source.tone_sample = 0;
            }
            log("No free AMPS voice channel for {}",
                "没有可供 {} 使用的 AMPS 话音信道", calling_number);
            return;
        }
        const int sat = needs_radio ? std::array<int, 3>{5970, 6000, 6030}[channel % 3] : 0;
        source.peer = target->number;
        source.peer_address = target_number;
        target->peer = source.number;
        target->peer_address = calling_number;
        source.state = UnitState::Originating;
        target->state = UnitState::Paging;
        if (source_wireline) source.wireline_state = WirelineState::Calling;
        if (target_wireline) target->wireline_state = WirelineState::Ringing;
        Call call;
        call.caller = source.number;
        call.called = target->number;
        call.channel = channel;
        call.sat = sat;
        call.due = Clock::now() + 120ms;
        call.caller_wireline = source_wireline;
        call.called_wireline = target_wireline;
        call.calling_number = calling_number;
        call.called_number = target_number;
        m_calls.push_back(std::move(call));
        log(source_wireline ? "Wall-jack origination: {} requests {}" : "RECC origination: {} requests {}",
            source_wireline ? "固定电话发起呼叫：{} 拨打 {}" : "反向控制信道呼叫请求：{} 拨打 {}",
            calling_number, target_number);
        refresh();
    }

    void set_wireline_present(Client &client, bool present) {
        if (present) {
            client.wireline_present = true;
            send_line(client, "M V N " + std::to_string(client.wireline_number));
        } else if (client.wireline_present) {
            if (wireline_active(client)) release(client.number, true);
            send_line(client, "M V R 0");
            client.wireline_present = false;
            client.wireline_offhook = false;
            client.wireline_state = WirelineState::OnHook;
            client.wireline_digits.clear();
            client.wireline_dtmf = DtmfDetector{};
        }
        log(present ? "Wall jack {} inserted" : "Wall jack {} removed",
            present ? "固定电话 {} 已接入电话线" : "固定电话 {} 已拔出电话线", client.wireline_number);
        refresh();
    }

    void set_wireline_hook(Client &client, bool offhook) {
        if (offhook == client.wireline_offhook && (offhook || client.wireline_state != WirelineState::Ringing)) return;
        if (offhook) {
            if (!client.wireline_present) {
                send_line(client, "M V B");
                log("Wall-jack off-hook rejected: no adapter at {}",
                    "固定电话摘机失败：{} 未连接电话线适配器", client.wireline_number);
                return;
            }
            if (client.wireline_state == WirelineState::Ringing && client.state == UnitState::Ringing) {
                client.wireline_offhook = true;
                client.wireline_dtmf = DtmfDetector{};
                answer(client, true);
                return;
            }
            // One handset audio/modem path cannot originate a wall-jack call
            // over an existing cellular conversation or online data session.
            if (client.service != Service::None ||
                (client.state != UnitState::RadioOff && client.state != UnitState::Registered)) {
                send_line(client, "M V B");
                log("Wall-jack off-hook rejected: line {} shares busy handset {}",
                    "固定电话摘机失败：线路 {} 共用的话机 {} 正忙", client.wireline_number, client.number);
                return;
            }
            client.wireline_offhook = true;
            client.wireline_state = WirelineState::DialTone;
            client.wireline_digits.clear();
            client.wireline_dtmf = DtmfDetector{};
            client.tone_sample = 0;
            log("Wall-jack off-hook: {}; waiting for in-band DTMF",
                "固定电话 {} 已摘机，等待带内 DTMF 拨号", client.wireline_number);
        } else {
            release(client.number, false);
            client.wireline_offhook = false;
            client.wireline_state = WirelineState::OnHook;
            client.wireline_digits.clear();
            client.wireline_dtmf = DtmfDetector{};
            client.state = client.rf_ready ? UnitState::Registered : UnitState::RadioOff;
            log("Wall-jack on-hook: {}",
                "固定电话 {} 已挂机", client.wireline_number);
        }
        refresh();
    }

    void begin_wireline_call(Client &source) {
        int target_number = 0;
        try {
            std::size_t count = 0;
            target_number = std::stoi(source.wireline_digits, &count);
            if (count != source.wireline_digits.size()) target_number = 0;
        } catch (...) { }
        begin_call(source, target_number, true);
    }

    void receive_wireline_voice(Client &source, const std::string &hex) {
        if (!source.wireline_offhook) return;
        if (source.wireline_state == WirelineState::Connected) {
            if (Client *peer = find(source.peer); peer && peer->state == UnitState::Connected)
                send_line(*peer, (peer->wireline_offhook ? "M V Y " : "Y ") + hex);
        } else if (source.wireline_state != WirelineState::DialTone && source.wireline_state != WirelineState::Collecting) {
            return;
        }
        if (const auto digit = source.wireline_dtmf.push(decode_hex(hex))) {
            if (source.wireline_state == WirelineState::Connected) {
                if (Client *peer = find(source.peer))
                    send_line(*peer, "E DTMF " + std::to_string(source.wireline_number) + " " + std::string(1, *digit));
                return;
            }
            if (source.wireline_digits.size() >= 32) return;
            source.wireline_digits.push_back(*digit);
            source.wireline_state = WirelineState::Collecting;
            source.wireline_digit_due = Clock::now() + 2s;
            log("Wall-jack DTMF digit from {}: {}",
                "固定电话 {} 的 DTMF 拨号：{}", source.wireline_number, *digit);
        }
    }

    void process_wireline_dialling() {
        const auto now = Clock::now();
        for (auto &[number, client] : m_clients)
            if (client->wireline_offhook && client->wireline_state == WirelineState::Collecting && now >= client->wireline_digit_due)
                begin_wireline_call(*client);
    }

    void start_service(Client &source, Service service, int number, bool cellular_web = false, bool rf_bearer = false) {
        if (cellular_web && source.state != UnitState::Registered) {
            send_line(source, "B");
            return;
        }
        const int web_channel = cellular_web ? allocate_channel() : 0;
        if (cellular_web && !web_channel) {
            send_line(source, "I 83");
            return;
        }
        source.service = service;
        source.service_rf_bearer = cellular_web || rf_bearer;
        source.state = UnitState::Data;
        source.peer = number;
        source.service_input.clear();
        source.login_request = 0;
        source.after_login.clear();
        source.web_request = 0;
        source.web_response.clear();
        source.web_position = 0;
        source.web_cellular = cellular_web;
        if (service == Service::Echo || service == Service::Mail || service == Service::Web) {
            if (cellular_web) {
                // INT15/A001 originates the RF bearer first. Only the later
                // bare ATD on COM2 may put the modem into online data mode.
                source.channel = web_channel;
                source.sat = std::array<int,3>{5970,6000,6030}[web_channel % 3];
                Call call;
                call.caller = source.number;
                call.channel = web_channel;
                call.sat = source.sat;
                call.stage = CallStage::Connected;
                m_calls.push_back(std::move(call));
                send_channel(source);
                send_line(source, "C");
            } else send_line(source, "M C 2400");
            if (service == Service::Mail) {
                source.mail_user.clear();
                source.mail_link = {};
                source.mail_authenticated = source.mail_message_mode = source.mail_native_mode = false;
                source.mail_message.clear();
            }
            log("2400 bit/s data call: {} -> {}",
                "2400 bit/s 数据呼叫：{} -> {}", source.number, number);
        } else if (service == Service::Pager) {
            source.state = UnitState::Connected;
            source.channel = allocate_channel();
            source.sat = std::array<int,3>{5970,6000,6030}[source.channel % 3];
            send_line(source, "D " + std::to_string(source.sid) + " " + std::to_string(source.channel) + " " + std::to_string(source.sat));
            send_line(source, "C");
            source.pager_target.clear();
            source.pager_message.clear();
            log("Pager IVR answered {}; enter destination#, message# using DTMF",
                "寻呼语音应答系统已接通 {}，请用 DTMF 输入目标号码和 #，再输入数字消息和 #", source.number);
        }
        refresh();
    }

    void start_emergency_call(Client &source) {
        if (!source.rf_ready) return;
        if (source.state != UnitState::Registered) {
            send_line(source, "B");
            source.state = UnitState::BusyTone;
            source.tone_sample = 0;
            log("Emergency call rejected: mobile {} is already in use",
                "紧急呼叫被拒绝：移动电话 {} 正在通话", source.number);
            refresh();
            return;
        }

        const int channel = allocate_channel();
        if (!channel) {
            // Even an emergency call still requires an available AMPS forward/
            // reverse voice-channel pair.  Report genuine system congestion.
            send_line(source, "I 83");
            log("Emergency call failed: no free AMPS voice channel for {}",
                "紧急呼叫失败：没有可供 {} 使用的 AMPS 话音信道", source.number);
            return;
        }

        const int sat = std::array<int,3>{5970,6000,6030}[channel % 3];
        source.peer = 911;
        source.state = UnitState::Connected;
        source.service = Service::Emergency;
        source.service_rf_bearer = true;
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
        log("Emergency 911 call connected: mobile {}, channel {}, SAT {}",
            "911 紧急呼叫已接通：移动电话 {}，信道 {}，SAT {}", source.number, channel, sat);
        refresh();
    }

    void originate(Client &source, const std::string &digits) {
        if (!source.rf_ready || wireline_active(source)) {
            log("Cellular origination ignored while RF is off: {}",
                "移动呼叫未发起：{} 的射频模块已关闭", source.number);
            return;
        }
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
        else if (dialled(kDataNumber)) start_service(source, Service::Echo, kDataNumber, false, true);
        else if (dialled(kMailNumber)) start_service(source, Service::Mail, kMailNumber, false, true);
        else if (dialled(kWebNumber)) start_service(source, Service::Web, kWebNumber, true);
        else if (dialled(kPagerNumber)) start_service(source, Service::Pager, kPagerNumber, false, true);
        else begin_call(source, target);
    }

    void release(int number, bool notify) {
        Client *client = find(number);
        if (!client) return;
        const int peer_number = client->peer;
        const bool client_wireline = wireline_active(*client);
        const int public_number = client_wireline ? client->wireline_number : client->number;
        const int public_peer = client->peer_address ? client->peer_address : peer_number;
        if (client_wireline) send_line(*client, "M V R 0");
        if (notify) send_line(*client, client_wireline ? "M V H" : "H");
        client->peer = 0;
        client->peer_address = 0;
        client->fax = {};
        client->state = client->rf_ready ? UnitState::Registered : UnitState::RadioOff;
        client->channel = client->rf_ready ? kControlChannel : 0;
        client->sat = client->rf_ready ? 6000 : 0;
        client->service = Service::None;
        client->service_rf_bearer = false;
        client->web_request = 0;
        client->web_response.clear();
        client->web_cellular = false;
        client->tone_sample = 0;
        client->dtmf = DtmfDetector{};
        if (client_wireline) client->wireline_state = client->wireline_offhook ? WirelineState::Ended : WirelineState::OnHook;
        if (Client *peer = find(peer_number); peer && peer->peer == number) {
            const bool peer_wireline = wireline_active(*peer);
            if (peer_wireline) send_line(*peer, "M V R 0");
            send_line(*peer, peer_wireline ? "M V H" : "H");
            peer->peer = 0;
            peer->peer_address = 0;
            peer->fax = {};
            peer->state = peer->rf_ready ? UnitState::Registered : UnitState::RadioOff;
            peer->channel = peer->rf_ready ? kControlChannel : 0;
            peer->sat = peer->rf_ready ? 6000 : 0;
            peer->service = Service::None;
            peer->service_rf_bearer = false;
            peer->dtmf = DtmfDetector{};
            if (peer_wireline) peer->wireline_state = peer->wireline_offhook ? WirelineState::Ended : WirelineState::OnHook;
            else send_channel(*peer);
        }
        // Releasing a voice/pager bearer returns to the control channel; it
        // does not re-register the mobile.  Re-sending S here used to show an
        // operator popup and provoke another host RF-status report per page.
        if (!client_wireline) send_channel(*client);
        std::erase_if(m_calls, [number](const Call &call) { return call.caller == number || call.called == number; });
        log("Release/ST: {}{}",
            "释放通话/ST：{}{}", public_number, public_peer ? " <-> " + std::to_string(public_peer) : "");
        refresh();
    }

    void answer(Client &source, bool source_wireline = false) {
        if (source_wireline ? !source.wireline_present || !source.wireline_offhook : !source.rf_ready) return;
        auto it = std::find_if(m_calls.begin(), m_calls.end(), [&source](const Call &call) { return call.called == source.number; });
        if (it == m_calls.end() || it->called_wireline != source_wireline || it->stage != CallStage::Ringing) return;
        Client *caller = find(it->caller);
        if (source_wireline) send_line(source, "M V R 0");
        send_line(source, source_wireline ? "M V C" : "C");
        if (caller) send_line(*caller, it->caller_wireline ? "M V C" : "C");
        source.state = UnitState::Connected;
        if (source_wireline) source.wireline_state = WirelineState::Connected;
        else {
            source.channel = it->channel;
            source.sat = it->sat;
            source.last_sat = Clock::now();
        }
        if (caller) {
            caller->state = UnitState::Connected;
            if (it->caller_wireline) caller->wireline_state = WirelineState::Connected;
            else {
                caller->channel = it->channel;
                caller->sat = it->sat;
                caller->last_sat = Clock::now();
            }
        }
        it->stage = CallStage::Connected;
        it->due = Clock::now() + (it->network_page ? 1200ms : 0ms);
        if (it->network_page) {
            log("Pager IVR voice path connected to {}; sending digits to the RF deck's silent DTMF path",
                "寻呼语音通道已接通 {}，正通过射频模块的静音 DTMF 通道发送数字", source.number);
        } else {
            const bool wireline = it->caller_wireline || it->called_wireline;
            log(wireline ? "Wall-jack conversation: {} <-> {}, channel {}, SAT {}"
                         : "FVC conversation: {} <-> {}, channel {}, SAT {}",
                wireline ? "固定电话通话：{} <-> {}，信道 {}，SAT {}"
                         : "前向话音信道通话：{} <-> {}，信道 {}，SAT {}",
                it->calling_number ? it->calling_number : caller->number,
                it->called_number ? it->called_number : source.number, it->channel, it->sat);
        }
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
        if (!target || !target->rf_ready || wireline_active(*target) || target->state != UnitState::Registered) {
            log("Pager destination unavailable: {}",
                "寻呼目标不可达：{}", to);
            return;
        }
        std::string digits;
        std::copy_if(text.begin(), text.end(), std::back_inserter(digits), [](unsigned char ch) { return std::isdigit(ch); });
        if (digits.empty()) {
            log("Pager message rejected: enter one or more content digits",
                "寻呼消息被拒绝：内容至少需要一位数字");
            return;
        }
        const int channel = allocate_channel();
        if (!channel) {
            log("Pager delivery failed: no free AMPS voice channel",
                "寻呼投递失败：没有空闲的 AMPS 话音信道");
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
        if (from)
            log("Network pager request -> {}, content digits: {} (submitted by {})",
                "网络寻呼请求 -> {}，数字内容：{}（提交方 {}）", to, digits, from);
        else
            log("Network pager request -> {}, content digits: {}", "网络寻呼请求 -> {}，数字内容：{}", to, digits);
        refresh();
    }

    void pager_digit(Client &client, char digit) {
        const bool collecting_target = client.pager_target.empty() ||
            (client.pager_message.empty() && client.service_input != "message");
        std::string &field = collecting_target ? client.pager_target : client.pager_message;
        if (digit != '#') {
            field.push_back(digit);
            return;
        }
        if (collecting_target) {
            client.service_input = "message";
            log("Pager IVR destination accepted: {}",
                "寻呼语音应答系统已接受目标号码 {}", client.pager_target);
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
            send_line(*peer, (peer->wireline_offhook ? "M V Y " : "Y ") + hex);
        const auto pcm = decode_hex(hex);
        if (const auto digit = source.dtmf.push(pcm)) {
            log("In-band DTMF from {}: {}",
                "来自 {} 的带内 DTMF：{}", source.number, *digit);
            if (Client *peer = find(source.peer))
                send_line(*peer, "E DTMF " + std::to_string(source.number) + " " + std::string(1, *digit));
            if (source.service == Service::Pager) pager_digit(source, *digit);
        }
    }

    void finish_mail(Client &client) {
        MailMessage message;
        std::istringstream stream(client.mail_message);
        std::string line;
        bool body = false;
        while (std::getline(stream, line)) {
            line = trim(line);
            if (!body && line.empty()) { body = true; continue; }
            if (!body && line.rfind("TO:", 0) == 0) message.to = trim(line.substr(3));
            else if (!body && line.rfind("FROM:", 0) == 0) continue; // use authenticated sender
            else if (!body && line.rfind("SUBJECT:", 0) == 0) message.subject = trim(line.substr(8));
            else { message.body += line; message.body += "\r\n"; }
        }
        const bool ok = postoffice::deliver(client.mail_user, message.to, message.subject.substr(0,128), message.body);
        send_modem(client, ok ? "+OK QUEUED\r\nMAIL> " : "-ERR NO DELIVERY\r\nMAIL> ");
        client.mail_message.clear();
        client.mail_message_mode = false;
    }

    void finish_mail_login(Client &client, bool accepted) {
        if (client.mail_native_mode) {
            auto wire = client.mail_link.send(client.mail_session.login_result(accepted));
            if (!wire.empty()) send_modem(client, wire);
            return;
        }
        client.mail_authenticated = accepted;
        send_modem(client, accepted ? "+OK SIMON MAILBOX\r\nMAIL> " : "-ERR LOGIN\r\n");
        // A modem write may contain PASS followed by SEND or LIST. Process
        // these in order once the password result is known.
        std::istringstream pending(std::exchange(client.after_login, {}));
        for (std::string line; client.service == Service::Mail && std::getline(pending, line); )
            mail_command(client, std::move(line));
    }

    void begin_mail_login(Client &client, std::string name, std::string password) {
        const auto id = m_next_request++;
        client.login_request = id;
        client.mail_authenticated = false;
        if (!m_accounts.submit([this, number = client.number, id, name = std::move(name),
                password = std::move(password)] {
            Action reply{ActionType::LoginReply};
            reply.from = number;
            reply.request = id;
            reply.accepted = postoffice::login(name, password);
            action(std::move(reply));
        })) {
            client.login_request = 0;
            finish_mail_login(client, false);
        }
    }

    void mail_command(Client &client, std::string line) {
        line = trim(std::move(line));
        if (client.login_request) {
            if (client.after_login.size() + line.size() + 1 <= 32768) {
                client.after_login += line;
                client.after_login += '\n';
            } else {
                client.login_request = 0;
                client.after_login.clear();
                send_modem(client, "-ERR COMMAND BUFFER FULL\r\n");
            }
            return;
        }
        if (client.mail_message_mode) {
            if (line == ".") finish_mail(client);
            else if (client.mail_message.size()+line.size()+2 <= 32768) { client.mail_message += line; client.mail_message += "\r\n"; }
            else { client.mail_message_mode=false; client.mail_message.clear(); send_modem(client,"-ERR MESSAGE TOO LARGE\r\n"); }
            return;
        }
        std::string upper = line;
        std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) { return char(std::toupper(c)); });
        if (upper.rfind("USER ", 0) == 0) {
            client.mail_authenticated = false;
            client.mail_user = trim(line.substr(5));
            send_modem(client, "+OK PASSWORD\r\nPASS ");
        } else if (upper.rfind("PASS ", 0) == 0) {
            begin_mail_login(client, client.mail_user, line.substr(5));
        } else if (!client.mail_authenticated && upper != "QUIT") {
            send_modem(client,"-ERR LOGIN REQUIRED\r\n");
        } else if (upper == "SEND" || upper == "DATA") {
            client.mail_message_mode = true;
            client.mail_message.clear();
            send_modem(client, "+OK END WITH <CRLF>.<CRLF>\r\n");
        } else if (upper == "LIST") {
            send_modem(client, postoffice::inbox(client.mail_user,0)+"\r\nMAIL> ");
        } else if (upper.rfind("RETR ",0)==0) {
            int id=0;
            try { id=std::stoi(line.substr(5)); } catch (...) { }
            send_modem(client,(id>0 ? postoffice::inbox(client.mail_user,id) : "-ERR NO MESSAGE")+std::string("\r\nMAIL> "));
        } else if (upper == "QUIT") {
            send_modem(client, "+OK BYE\r\n");
            send_line(client, "M H");
            client.service = Service::None;
            client.service_rf_bearer = false;
            client.state = client.rf_ready ? UnitState::Registered : UnitState::RadioOff;
            client.peer = 0;
        } else if (!line.empty()) {
            send_modem(client, "-ERR COMMAND\r\nMAIL> ");
        }
    }

    void receive_web(Client &client, const std::vector<std::uint8_t> &bytes) {
        // One bounded HTTP request at a time. The real guest consumes the
        // preceding Content-Length before it issues another navigation.
        if (client.web_request || !client.web_response.empty()) return;
        client.service_input.append(reinterpret_cast<const char *>(bytes.data()), bytes.size());
        auto frame = simon_weber::inspect_request(client.service_input);
        if (frame.state == simon_weber::FrameState::Error) {
            client.web_response = simon_weber::framing_error(frame.error);
            client.service_input.clear();
            client.web_position = 0;
            client.web_due = Clock::now();
            return;
        }
        if (frame.state != simon_weber::FrameState::Complete) return;
        if (simon_weber::lower(frame.url) == "http://simon.local/register") {
            client.service_input.clear();
            client.web_position = 0;
            client.web_due = Clock::now();
            if (frame.method == "GET") {
                client.web_response = postoffice::registration(frame);
                return;
            }
            const auto id = m_next_request++;
            client.web_request = id;
            if (!m_accounts.submit([this, number = client.number, id, frame = std::move(frame)] {
                Action reply{ActionType::WebReply};
                reply.from = number;
                reply.request = id;
                reply.text = postoffice::registration(frame);
                action(std::move(reply));
            })) {
                client.web_request = 0;
                client.web_response = simon_weber::response(503, "Post Office Busy", "Try registering again shortly.");
            }
            return;
        }
        const std::string request = client.service_input;
        client.service_input.clear();
        client.web_request = m_next_request++;
        if (!m_web_gateway.submit(client.web_request, request, std::uint64_t(client.number))) {
            client.web_request = 0;
            client.web_response = simon_weber::response(503, "Gateway Busy", "Try this page again shortly.\r\n");
            client.web_position = 0;
            client.web_due = Clock::now();
        }
        // HTML GET/POST forms may contain passwords/tokens. Never log URLs,
        // headers, or transport payloads for a web service request.
        log("SimonWeber HTTP request from {}: {} bytes",
            "来自 {} 的 SimonWeber HTTP 请求：{} 字节", client.number, request.size());
    }

    void service_web() {
        // Worker completions are transferred back to the socket-owning
        // thread. Unique request IDs prevent a late DNS/HTTP reply from being
        // delivered after hang-up, redial, or a handset disconnect.
        for (auto &completion : m_web_gateway.drain()) {
            for (auto &[number, client] : m_clients) {
                if (client->service != Service::Web || client->web_request != completion.id) continue;
                client->web_request = 0;
                client->web_response = std::move(completion.response);
                client->web_position = 0;
                client->web_due = Clock::now();
                log("SimonWeber response to {}: {} bytes",
                    "发往 {} 的 SimonWeber 响应：{} 字节", number, client->web_response.size());
                break;
            }
        }
        const auto now = Clock::now();
        for (auto &[number, client] : m_clients) {
            if (client->service != Service::Web || client->web_response.empty() || now < client->web_due) continue;
            const auto count = std::min<std::size_t>(32, client->web_response.size() - client->web_position);
            send_modem(*client, client->web_response.substr(client->web_position, count));
            client->web_position += count;
            // 2400 bit/s with an 8N1 UART transfers 240 octets/s. Never burst
            // the whole web page into the emulated modem's 1024-byte queue.
            client->web_due = now + std::chrono::microseconds((count * 1000000 + 239) / 240);
            if (client->web_position == client->web_response.size()) {
                client->web_response.clear();
                client->web_position = 0;
            }
        }
    }

    void receive_modem(Client &client, const std::string &hex) {
        const auto bytes = decode_hex(hex);
        if (client.service == Service::Web) {
            receive_web(client, bytes);
            return;
        }
        if (client.service == Service::Echo) {
            send_line(client, "M X " + hex);
            return;
        }
        if (client.service != Service::Mail) return;
        if (bytes.empty()) return;
        if (client.service_input.empty() && bytes.front() == 2) client.mail_native_mode = true;
        if (client.mail_native_mode) {
            const std::string_view fragment(reinterpret_cast<const char *>(bytes.data()), bytes.size());
            const auto reply = client.mail_link.receive(fragment, client.mail_session);
            if (!reply.empty()) send_modem(client, reply);
            if (auto login = client.mail_session.take_login())
                begin_mail_login(client, std::move(login->first), std::move(login->second));
            return;
        }
        for (auto byte : bytes) {
            client.service_input.push_back(char(byte));
            if (client.service_input.size() > 4096) { release(client.number, true); return; }
            if (byte == '\n') {
                mail_command(client, client.service_input);
                client.service_input.clear();
            }
        }
    }

    // Class 1 reports a carrier as soon as it starts. Waiting for the entire
    // page before CONNECT would make large faxes time out during T.30 phase C.
    void pump_fax(Client &unit) {
        FaxLink &link = unit.fax;
        if (link.receive_mode.empty() || link.incoming.empty())
            return;

        FaxBurst &burst = link.incoming.front();
        if (!link.receiving) {
            if (burst.mode != link.receive_mode) {
                link.receive_mode.clear();
                send_line(unit, "M F FAIL");
                return;
            }
            link.receiving = true;
            send_line(unit, "M F START");
        }
        if (!link.requested_chunk)
            return;

        if (!burst.chunks.empty()) {
            const auto &chunk = burst.chunks.front();
            send_line(unit, "M F X " + chunk);
            link.queued_digits -= chunk.size();
            burst.chunks.pop_front();
        } else if (burst.complete) {
            send_line(unit, "M F END");
            link.incoming.pop_front();
            link.receive_mode.clear();
            link.receiving = false;
        } else {
            return; // Keep the outstanding read until the sender supplies data.
        }
        link.requested_chunk = false;
    }

    void fax_control(Client &unit, const std::string &op) {
        Client *peer = find(unit.peer);
        if (!peer || peer->peer != unit.number || unit.state != UnitState::Connected ||
                peer->state != UnitState::Connected) {
            unit.fax = {};
            send_line(unit, "M F FAIL");
            return;
        }

        FaxLink &receiver = peer->fax;
        const bool sending = !receiver.incoming.empty() && !receiver.incoming.back().complete;
        if (op.starts_with("TX ") && valid_fax_mode(op.substr(3)) && !sending &&
                receiver.incoming.size() < 16) {
            receiver.incoming.push_back({op.substr(3), {}, false});
            send_line(unit, "M F READY");
            pump_fax(*peer);
            return;
        }
        if (op.starts_with("X ") && sending && op.size() > 2 && op.size() <= 130 &&
                op.size() % 2 == 0 && op.find_first_not_of("0123456789abcdefABCDEF", 2) == op.npos &&
                receiver.queued_digits + op.size() - 2 <= 1024 * 1024) {
            receiver.incoming.back().chunks.emplace_back(op.substr(2));
            receiver.queued_digits += op.size() - 2;
            pump_fax(*peer);
            return;
        }
        if ((op == "DONE" || op == "EOT") && sending) {
            receiver.incoming.back().complete = true;
            send_line(unit, "M F OK");
            pump_fax(*peer);
            return;
        }
        if (op.starts_with("RX ") && valid_fax_mode(op.substr(3)) && unit.fax.receive_mode.empty()) {
            unit.fax.receive_mode = op.substr(3);
            pump_fax(unit);
            return;
        }
        if (op == "NEXT" && unit.fax.receiving) {
            unit.fax.requested_chunk = true;
            pump_fax(unit);
            return;
        }

        // Lost carrier or aborted transaction: both modems leave data mode.
        unit.fax = {};
        peer->fax = {};
        send_line(*peer, "M F FAIL");
        if (op != "ABORT")
            send_line(unit, "M F FAIL");
    }

    void route_line(Client &client, const std::string &line) {
        if (line.starts_with("M F ")) { fax_control(client,line.substr(4)); return; }
        if (line == "N") { register_radio(client); return; }
        if (line == "F 0") { power_off_radio(client); return; }
        if (line == "L") {
            for (const auto &[number, unit] : m_clients)
                send_line(client, "E UNIT " + std::to_string(number) + " " + (unit->rf_ready ? "READY" : "ATTACHED"));
            return;
        }
        if (line.rfind("T ", 0) == 0) {
            if (client.rf_ready) log_protocol("[" + std::to_string(client.number) + "] RF " + line);
            return;
        }
        if (line.rfind("P ", 0) == 0) return;
        if (line.rfind("O ", 0) == 0) { originate(client, trim(line.substr(2))); return; }
        if (line == "A") { answer(client); return; }
        if (line == "H") { if (!wireline_active(client)) release(client.number, false); return; }
        if (line.rfind("Y ", 0) == 0) { if (client.rf_ready && !wireline_active(client)) receive_voice(client, trim(line.substr(2))); return; }
        if (line.rfind("Q ", 0) == 0) {
            if (!client.rf_ready) return;
            const std::string request = trim(line.substr(2));
            const auto split = request.find(' ');
            int target = 0;
            try { target = std::stoi(request.substr(0, split)); } catch (...) { }
            if (target) deliver_page(client.number, target, split == std::string::npos ? "" : request.substr(split + 1));
            return;
        }
        if (line.rfind("Z ", 0) == 0) {
            if (!client.rf_ready) return;
            int sat = 0;
            try { sat = std::stoi(trim(line.substr(2))); } catch (...) { }
            if (sat == client.sat) client.last_sat = Clock::now();
            return;
        }
        if (line == "M J 1") { set_wireline_present(client, true); return; }
        if (line == "M J 0") { set_wireline_present(client, false); return; }
        if (line == "M L 1") { set_wireline_hook(client, true); return; }
        if (line == "M L 0") { set_wireline_hook(client, false); return; }
        if (line.rfind("M V Y ", 0) == 0) { receive_wireline_voice(client, trim(line.substr(6))); return; }
        if (line.rfind("M D ", 0) == 0) {
            if (wireline_active(client)) { send_line(client, "M N"); return; }
            const std::string digits = trim(line.substr(4));
            const auto dialled = [&digits](int service) {
                const std::string suffix = std::to_string(service);
                return digits == suffix || (digits.size() > suffix.size() && digits.ends_with(suffix));
            };
            if (dialled(kDataNumber)) start_service(client, Service::Echo, kDataNumber);
            else if (dialled(kMailNumber)) start_service(client, Service::Mail, kMailNumber);
            else if (dialled(kWebNumber)) start_service(client, Service::Web, kWebNumber);
            else send_line(client, "M N");
            return;
        }
        if (line.rfind("M X ", 0) == 0) { receive_modem(client, trim(line.substr(4))); return; }
        if (line == "M H") {
            if (wireline_active(client)) { send_line(client, "M H"); return; }
            if (client.web_cellular) release(client.number, true);
            send_line(client, "M H");
            client.service = Service::None;
            client.service_rf_bearer = false;
            client.state = client.rf_ready ? UnitState::Registered : UnitState::RadioOff;
            client.peer = 0;
            client.web_request = 0;
            client.web_response.clear();
            client.service_input.clear();
            refresh();
            return;
        }
        log_protocol("[" + std::to_string(client.number) + "] " + line);
    }

    void process_calls() {
        const auto now = Clock::now();
        for (auto &call : m_calls) {
            if (call.stage == CallStage::Connected) continue;
            if (call.stage == CallStage::Ringing) {
                if (call.called_wireline && now >= call.ring_due) {
                    if (Client *called = find(call.called); called && called->wireline_present) {
                        call.ring_high = !call.ring_high;
                        send_line(*called, call.ring_high ? "M V R 1" : "M V R 0");
                        call.ring_due = now + (call.ring_high ? 2s : 4s);
                    }
                }
                continue;
            }
            if (now < call.due) continue;
            Client *caller = find(call.caller);
            Client *called = find(call.called);
            if ((!caller && !call.network_page) || !called) continue;
            if (call.stage == CallStage::Page) {
                if (call.called_wireline)
                    log("PSTN line seizure -> {}", "已占用固定电话线路 -> {}", called->wireline_number);
                else
                    log("FOCC page order -> {}; RECC page response received",
                        "前向控制信道已呼叫 {}，已收到反向控制信道应答", called->number);
                call.stage = CallStage::Assign;
                call.due = now + 180ms;
            } else if (call.stage == CallStage::Assign) {
                if (!call.called_wireline) {
                    called->channel = call.channel;
                    called->sat = call.sat;
                    send_channel(*called);
                }
                if (caller && !call.caller_wireline) {
                    caller->channel = call.channel;
                    caller->sat = call.sat;
                    send_line(*caller, "D " + std::to_string(caller->sid) + " " + std::to_string(call.channel) + " " + std::to_string(call.sat));
                }
                if (call.channel) log("FVC assignment: channel {}, SAT {}",
                    "已分配前向话音信道：信道 {}，SAT {}", call.channel, call.sat);
                call.stage = CallStage::Alert;
                call.due = now + 80ms;
            } else if (call.stage == CallStage::Alert) {
                // R means that page response, channel assignment and SAT
                // acquisition are complete.  The deck latches A7 before 6A.
				const int calling_number = call.network_page ? kPagerNumber : call.calling_number ? call.calling_number : caller->number;
				// P distinguishes unattended numeric paging from an ordinary
				// incoming voice call before the RF deck raises any host UI flags.
                if (call.called_wireline) {
                    // Actual ring voltage is separate from off-hook and from
                    // RF alerting.  The guest's modem UART observes this as RI.
                    send_line(*called, "M V R 1");
                    call.ring_high = true;
                    call.ring_due = now + 2s;
                } else send_line(*called, "R " + std::to_string(calling_number) + (call.network_page ? " P" : ""));
                called->state = UnitState::Ringing;
                if (caller) {
                    caller->state = UnitState::Ringing;
                    caller->tone_sample = 0;
                }
                call.stage = CallStage::Ringing;
                log("Alert order: {} -> {}",
                    "振铃指令：{} -> {}", calling_number, call.called_wireline ? called->wireline_number : called->number);
                refresh();
            }
        }
    }

    void send_tone(Client &client, double f1, double f2, int on_samples, int period_samples) {
        static constexpr char hex[] = "0123456789ABCDEF";
        // 160 samples per 20 ms server tick, framed in the driver's native 16-sample packets.
        for (int packet = 0; packet < 10; ++packet) {
            std::string line = client.wireline_offhook ? "M V Y " : "Y ";
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
            if (client.wireline_offhook && client.wireline_state == WirelineState::DialTone)
                send_tone(client, 350, 440, 8000, 8000);
            else if (client.state == UnitState::BusyTone) send_tone(client, 480, 620, 4000, 8000);
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
            log("Pager digits accepted by Simon {}",
                "Simon {} 已接收寻呼数字", number);
            release(number, true);
        }
    }

    void supervise_sat() {
        const auto now = Clock::now();
        std::vector<int> lost;
        for (const auto &[number, client] : m_clients)
            if (!client->wireline_offhook && client->state == UnitState::Connected && client->service == Service::None && now - client->last_sat > 5s)
                lost.push_back(number);
        for (int number : lost) {
            log("SAT lost for {}; releasing voice channel",
                "{} 的 SAT 监测音丢失，正在释放话音信道", number);
            release(number, true);
        }
    }

    void process_actions() {
        std::deque<Action> actions;
        {
            std::lock_guard lock(m_action_mutex);
            actions.swap(m_actions);
        }
        for (Action &action : actions) {
            // The worker never owns a Client. A hang-up and redial on the
            // same socket must not apply an earlier request to the new session.
            if (action.type == ActionType::LoginReply || action.type == ActionType::WebReply) {
                Client *client = find(action.from);
                if (!client) continue;
                if (action.type == ActionType::LoginReply && client->service == Service::Mail &&
                        client->login_request == action.request) {
                    client->login_request = 0;
                    finish_mail_login(*client, action.accepted);
                }
                if (action.type == ActionType::WebReply && client->service == Service::Web &&
                        client->web_request == action.request) {
                    client->web_request = 0;
                    client->web_response = std::move(action.text);
                    client->web_position = 0;
                    client->web_due = Clock::now();
                }
                continue;
            }
            if (action.type == ActionType::Call) {
                bool wireline = false;
                if (Client *client = find_address(action.from, wireline)) {
                    if (!wireline) originate(*client, std::to_string(action.to));
                    else if (client->wireline_offhook) begin_call(*client, action.to, true);
                    else log("Wall-jack Call requires the Simon handset to go off-hook first: {}",
                        "固定电话呼叫前，请先在 Simon {} 上摘机", action.from);
                }
            }
            else if (action.type == ActionType::Connect) {
                bool wireline = false;
                if (Client *client = find_address(action.to, wireline)) {
                    if (wireline) log("Answer wall-jack calls on the Simon handset; the GUI does not lift its receiver",
                        "请在 Simon 话机上摘机接听固定电话来电");
                    else answer(*client);
                }
            }
            else if (action.type == ActionType::Hang) {
                bool wireline = false;
                if (Client *client = find_address(action.from, wireline)) {
                    if (wireline == wireline_active(*client)) release(client->number, true);
                    else log("Hang ignored: the selected number is not the active bearer",
                        "挂机未执行：所选号码没有承载当前通话");
                }
            }
            else if (action.type == ActionType::Page) deliver_page(action.from, action.to, action.text);
            else if (action.type == ActionType::Profile) {
                if (action.from < kFirstNumber || action.from > kLastNumber) {
                    log("Select a mobile number in the range 1001-1999.",
                        "请选择 1001 至 1999 之间的移动电话号码");
                    continue;
                }
                switch_store::Profile profile{action.registration, std::clamp(action.signal, 0, 6),
                    action.operator_name.empty() ? "Simon AMPS" : action.operator_name};
                if (!switch_store::save_profile(action.from, profile)) {
                    log("Cannot save profile for {}",
                        "无法保存 {} 的网络配置", action.from);
                    continue;
                }
                if (Client *client = find(action.from)) {
                    static_cast<switch_store::Profile &>(*client) = profile;
                    if (client->rf_ready) send_profile(*client);
                    refresh();
                }
                log("Profile saved for {}",
                    "已保存 {} 的网络配置", action.from);
            }
        }
    }

    void refresh() {
        std::vector<UnitSnapshot> next;
        for (const auto &[number, client] : m_clients)
            next.push_back(UnitSnapshot{number, client->wireline_number, client->wireline_present,
                wireline_active(*client) ? wireline_state_name(client->wireline_state) :
                    client->state == UnitState::Data && !client->rf_ready ? UiLabel{L"Wireline data / RF off", L"座机数据连接／射频已关闭"} : state_name(client->state),
                client->peer_address ? client->peer_address : client->peer, client->channel, client->sat, client->rf_ready ? client->registration : "OFF",
                client->rf_ready ? client->signal : 0, client->rf_ready ? client->sid : 0,
                client->rf_ready ? client->operator_name : ""});
        {
            std::lock_guard lock(m_snapshot_mutex);
            m_snapshots = std::move(next);
        }
        if (m_notify) PostMessageW(m_notify, WM_SWITCH_REFRESH, 0, 0);
    }

    void disconnect(int number) {
        Client *client = find(number);
        if (!client) return;
        release(number, false);
        closesocket(client->socket);
        m_web_gateway.forget(std::uint64_t(number));
        m_clients.erase(number);
        log("Simon disconnected: {}",
            "Simon 已断开连接：{}", number);
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
            client->number = allocate_number();
            if (!client->number) {
                log("Transport rejected: all mobile 1001-1999 / wall-jack 2001-2999 number pairs are in use",
                    "连接被拒绝：所有移动号码（1001 至 1999）及对应固定号码（2001 至 2999）都已占用");
                closesocket(socket);
                continue;
            }
            client->wireline_number = client->number + kWirelineNumberOffset;
            const int number = client->number;
            if (!switch_store::load_profile(number, *client)) {
                log("Cannot read network profile for {}",
                    "无法读取 {} 的网络配置", number);
                closesocket(socket);
                continue;
            }
            m_clients[number] = std::move(client);
            // V is local virtual NAM configuration, not an over-the-air
            // registration.  Wait for N before sending any D/S RF profile.
            send_line(*m_clients[number], "V " + std::to_string(number));
            log("Simon transport attached; mobile {}, wall jack {}; RF off, wall jack unplugged",
                "Simon 已连接：移动号码 {}，固定号码 {}，射频模块已关闭，电话线尚未接入", number, m_clients[number]->wireline_number);
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
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            log("WSAStartup failed", "Windows 套接字初始化失败（WSAStartup）");
            m_running = false;
            return;
        }
        m_listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(std::uint16_t(m_endpoint.port));
        const std::string endpoint = m_endpoint.address + ":" + std::to_string(m_endpoint.port);
        if (inet_pton(AF_INET, m_endpoint.address.c_str(), &address.sin_addr) != 1 ||
            bind(m_listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == SOCKET_ERROR || listen(m_listener, 8) == SOCKET_ERROR) {
            log("Cannot listen on {} (check address and port)",
                "无法监听 {}，请检查地址和端口", endpoint);
            closesocket(m_listener);
            m_listener = INVALID_SOCKET;
            WSACleanup();
            m_running = false;
            return;
        }
        u_long nonblocking = 1;
        ioctlsocket(m_listener, FIONBIO, &nonblocking);
        log("AMPS switch listening on {}",
            "AMPS 交换机正在监听 {}", endpoint);
        log("Services: data {}, mail {}, web {}, pager {}",
            "服务号码：数据 {}，邮件 {}，网页 {}，寻呼 {}", kDataNumber, kMailNumber, kWebNumber, kPagerNumber);
        auto audio_due = Clock::now() + 20ms;
        while (m_running) {
            accept_clients();
            read_clients();
            process_actions();
            process_wireline_dialling();
            process_calls();
            service_web();
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
switch_store::Listener g_listener;
HFONT g_font = nullptr;
constexpr UiLabel kTitle{L"Simon AMPS Laboratory Switch", L"Simon AMPS 实验交换机"};
struct RegistrationOption { const char *code; const wchar_t *chinese; };
constexpr RegistrationOption kRegistrations[] = {
    {"HOME1", L"归属网 1"}, {"HOME2", L"归属网 2"}, {"HOME3", L"归属网 3"}, {"HOME4", L"归属网 4"},
    {"ROAM", L"漫游"}, {"ALTROAM", L"备用漫游"}, {"OFFLINE", L"离线"}
};

std::wstring registration_label(const std::string &code) {
    if (g_chinese)
        for (auto &option : kRegistrations)
            if (code == option.code) return option.chinese;
    return widen(code);
}

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

// Keep the most recent events bilingual so changing the menu also updates
// the visible history. The full English diagnostic trace stays on disk.
std::deque<SwitchLogMessage> g_log_history;

const std::wstring &log_text(const SwitchLogMessage &event) {
    return g_chinese ? event.chinese : event.english;
}

void append_log(SwitchLogMessage event) {
    if (g_log_history.size() == 512) {
        SendMessageW(g_log, EM_SETSEL, 0, log_text(g_log_history.front()).size());
        SendMessageW(g_log, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(L""));
        g_log_history.pop_front();
    }
    const int length = GetWindowTextLengthW(g_log);
    SendMessageW(g_log, EM_SETSEL, length, length);
    SendMessageW(g_log, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(log_text(event).c_str()));
    g_log_history.push_back(std::move(event));
}

void refresh_list() {
    if (!g_server || !g_units) return;
    const auto units = g_server->snapshots();
    ListView_DeleteAllItems(g_units);
    int row = 0;
    for (const auto &unit : units) {
        std::array<std::wstring, 11> values{
            std::to_wstring(unit.number), std::to_wstring(unit.wireline_number),
            unit.wireline_present ? display({L"Inserted", L"已插入"}) : display({L"Unplugged", L"未插入"}),
            display(unit.state), unit.peer ? std::to_wstring(unit.peer) : L"-",
            std::to_wstring(unit.channel), std::to_wstring(unit.sat), registration_label(unit.registration),
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

void update_interface() {
    SetWindowTextW(g_window, display(kTitle));
    std::wstring history;
    for (const auto &event : g_log_history) history += log_text(event);
    SetWindowTextW(g_log, history.c_str());
    SendMessageW(g_log, EM_SETSEL, history.size(), history.size());
    SendMessageW(g_log, EM_SCROLLCARET, 0, 0);
    CheckMenuRadioItem(GetSubMenu(GetMenu(g_window), 0), IDM_ENGLISH, IDM_CHINESE,
                       g_chinese ? IDM_CHINESE : IDM_ENGLISH, MF_BYCOMMAND);
    struct Caption { int id; UiLabel text; };
    constexpr Caption captions[] = {
        {IDC_FROM_LABEL, {L"Call from", L"主叫号码"}}, {IDC_TO_LABEL, {L"to", L"至"}},
        {IDC_CALL, {L"Call", L"呼叫"}}, {IDC_CONNECT, {L"Answer", L"接听"}}, {IDC_HANG, {L"Hang", L"挂断"}},
        {IDC_PAGE_TO_LABEL, {L"Page to", L"寻呼号码"}}, {IDC_DIGITS_LABEL, {L"Digits", L"数字"}},
        {IDC_PAGE, {L"Send network page", L"发送网络寻呼"}}, {IDC_PROFILE_LABEL, {L"Profile", L"网络"}},
        {IDC_APPLY, {L"Apply", L"应用"}}, {IDC_CLEAR, {L"Clear log", L"清空日志"}},
        {IDC_SIGNAL_LABEL, {L"RSSI", L"信号"}}, {IDC_OPERATOR_LABEL, {L"Operator", L"运营商"}},
        {IDC_MAIL_NAME_LABEL, {L"Mailbox name", L"邮箱账号"}}, {IDC_PASSWORD_LABEL, {L"Password", L"邮箱密码"}},
        {IDC_ADDRESS_LABEL, {L"Listen address", L"监听地址"}}, {IDC_PORT_LABEL, {L"Port", L"端口"}},
        {IDC_REGISTER, {L"Register mailbox", L"注册邮箱"}},
        {IDC_SAVE_LISTENER, {L"Save listener for next start", L"保存监听设置（重启生效）"}}
    };
    for (auto &caption : captions) SetDlgItemTextW(g_window, caption.id, display(caption.text));

    const auto selected = SendMessageW(g_registration, CB_GETCURSEL, 0, 0);
    SendMessageW(g_registration, CB_RESETCONTENT, 0, 0);
    for (auto &option : kRegistrations) {
        auto name = registration_label(option.code);
        SendMessageW(g_registration, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
    }
    SendMessageW(g_registration, CB_SETCURSEL, selected == CB_ERR ? 0 : selected, 0);

    constexpr std::array<std::pair<UiLabel, int>, 11> columns{{
        {{L"Mobile",L"手机号码"},70}, {{L"Wall Jack",L"座机号码"},75}, {{L"Jack",L"电话插孔"},75},
        {{L"State",L"状态"},145}, {{L"Peer",L"对端号码"},75}, {{L"Channel",L"信道"},70}, {{L"SAT",L"SAT"},60},
        {{L"Registration",L"入网状态"},90}, {{L"RSSI",L"RSSI"},55}, {{L"SID",L"SID"},55}, {{L"Operator",L"运营商"},115}
    }};
    const bool create = Header_GetItemCount(ListView_GetHeader(g_units)) == 0;
    for (int i = 0; i < int(columns.size()); ++i) {
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | (create ? LVCF_WIDTH : 0);
        column.pszText = const_cast<wchar_t *>(display(columns[i].first));
        column.cx = columns[i].second;
        if (create) ListView_InsertColumn(g_units, i, &column);
        else ListView_SetColumn(g_units, i, &column);
    }
    refresh_list();
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
    MoveWindow(GetDlgItem(g_window, IDC_PAGE), 350, controls_y + 32, 145, 24, TRUE);
    MoveWindow(g_profile_label, 10, controls_y + 67, 45, 20, TRUE);
    MoveWindow(g_registration, 55, controls_y + 64, 90, 200, TRUE);
    MoveWindow(GetDlgItem(g_window, IDC_SIGNAL_LABEL), 154, controls_y + 67, 35, 20, TRUE);
    MoveWindow(g_signal, 192, controls_y + 64, 40, 24, TRUE);
    MoveWindow(GetDlgItem(g_window, IDC_OPERATOR_LABEL), 242, controls_y + 67, 58, 20, TRUE);
    MoveWindow(g_operator, 302, controls_y + 64, 131, 24, TRUE);
    MoveWindow(GetDlgItem(g_window, IDC_APPLY), 443, controls_y + 64, 75, 24, TRUE);
    MoveWindow(GetDlgItem(g_window, IDC_CLEAR), width - 90, controls_y + 64, 80, 24, TRUE);
    MoveWindow(GetDlgItem(g_window, IDC_MAIL_NAME_LABEL), 10, controls_y + 99, 90, 20, TRUE);
    MoveWindow(GetDlgItem(g_window, IDC_MAIL_NAME), 104, controls_y + 96, 128, 24, TRUE);
    MoveWindow(GetDlgItem(g_window, IDC_PASSWORD_LABEL), 242, controls_y + 99, 58, 20, TRUE);
    MoveWindow(GetDlgItem(g_window, IDC_MAIL_PASSWORD), 302, controls_y + 96, 131, 24, TRUE);
    MoveWindow(GetDlgItem(g_window, IDC_REGISTER), 443, controls_y + 96, 135, 24, TRUE);
    MoveWindow(GetDlgItem(g_window, IDC_ADDRESS_LABEL), 10, controls_y + 131, 90, 20, TRUE);
    MoveWindow(GetDlgItem(g_window, IDC_BIND_ADDRESS), 104, controls_y + 128, 128, 24, TRUE);
    MoveWindow(GetDlgItem(g_window, IDC_PORT_LABEL), 242, controls_y + 131, 58, 20, TRUE);
    MoveWindow(GetDlgItem(g_window, IDC_BIND_PORT), 302, controls_y + 128, 75, 24, TRUE);
    MoveWindow(GetDlgItem(g_window, IDC_SAVE_LISTENER), 387, controls_y + 128, 200, 24, TRUE);
    MoveWindow(g_log,margin,controls_y+160,width-margin*2,std::max(80,height-controls_y-170),TRUE);
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_CREATE: {
        g_window = window;
        g_units = control(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS, 0, 0, 0, 0, IDC_UNITS);
        ListView_SetExtendedListViewStyle(g_units, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
        g_call_from_label = control(L"STATIC", L"", 0, 0, 0, 0, 0, IDC_FROM_LABEL);
        g_from = control(L"EDIT", L"1001", WS_BORDER | ES_NUMBER, 0, 0, 0, 0, IDC_FROM);
        g_call_to_label = control(L"STATIC", L"", 0, 0, 0, 0, 0, IDC_TO_LABEL);
        g_to = control(L"EDIT", L"1002", WS_BORDER | ES_NUMBER, 0, 0, 0, 0, IDC_TO);
        control(L"BUTTON", L"", BS_PUSHBUTTON, 0, 0, 0, 0, IDC_CALL);
        control(L"BUTTON", L"", BS_PUSHBUTTON, 0, 0, 0, 0, IDC_CONNECT);
        control(L"BUTTON", L"", BS_PUSHBUTTON, 0, 0, 0, 0, IDC_HANG);
        g_page_to_label = control(L"STATIC", L"", 0, 0, 0, 0, 0, IDC_PAGE_TO_LABEL);
        g_page_to = control(L"EDIT", L"1002", WS_BORDER | ES_NUMBER, 0, 0, 0, 0, IDC_PAGE_TO);
        g_page_digits_label = control(L"STATIC", L"", 0, 0, 0, 0, 0, IDC_DIGITS_LABEL);
        g_page_text = control(L"EDIT", L"123", WS_BORDER | ES_NUMBER, 0, 0, 0, 0, IDC_PAGE_TEXT);
        control(L"BUTTON", L"", BS_PUSHBUTTON, 0, 0, 0, 0, IDC_PAGE);
        g_profile_label = control(L"STATIC", L"", 0, 0, 0, 0, 0, IDC_PROFILE_LABEL);
        g_registration = control(L"COMBOBOX", L"", CBS_DROPDOWNLIST, 0, 0, 0, 0, IDC_REGISTRATION);
        g_signal = control(L"EDIT", L"6", WS_BORDER | ES_NUMBER, 0, 0, 0, 0, IDC_SIGNAL);
        g_operator = control(L"EDIT", L"Simon AMPS", WS_BORDER, 0, 0, 0, 0, IDC_OPERATOR);
        control(L"BUTTON", L"", BS_PUSHBUTTON, 0, 0, 0, 0, IDC_APPLY);
        control(L"BUTTON", L"", BS_PUSHBUTTON, 0, 0, 0, 0, IDC_CLEAR);
        auto name=control(L"EDIT",L"",WS_BORDER|ES_AUTOHSCROLL,0,0,0,0,IDC_MAIL_NAME);
        auto password=control(L"EDIT",L"",WS_BORDER|ES_AUTOHSCROLL|ES_PASSWORD,0,0,0,0,IDC_MAIL_PASSWORD);
        SendMessageW(name,EM_SETLIMITTEXT,32,0); SendMessageW(password,EM_SETLIMITTEXT,64,0);
        control(L"BUTTON",L"",BS_PUSHBUTTON,0,0,0,0,IDC_REGISTER);
        control(L"EDIT",widen(g_listener.address).c_str(),WS_BORDER,0,0,0,0,IDC_BIND_ADDRESS);
        control(L"EDIT",std::to_wstring(g_listener.port).c_str(),WS_BORDER|ES_NUMBER,0,0,0,0,IDC_BIND_PORT);
        control(L"BUTTON",L"",BS_PUSHBUTTON,0,0,0,0,IDC_SAVE_LISTENER);
        for (int label : {IDC_MAIL_NAME_LABEL, IDC_PASSWORD_LABEL, IDC_ADDRESS_LABEL, IDC_PORT_LABEL,
                          IDC_SIGNAL_LABEL, IDC_OPERATOR_LABEL})
            control(L"STATIC", L"", 0, 0, 0, 0, 0, label);
        g_log = control(L"EDIT", L"", WS_BORDER | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, 0, 0, 0, 0, IDC_LOG);
        SendMessageW(g_log, EM_SETLIMITTEXT, 0x7ffffffe, 0);
        NONCLIENTMETRICSW metrics{};
        metrics.cbSize = sizeof(metrics);
        if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0))
            g_font = CreateFontIndirectW(&metrics.lfMessageFont);
        if (g_font) EnumChildWindows(window, [](HWND child, LPARAM font) -> BOOL {
            SendMessageW(child, WM_SETFONT, font, TRUE);
            return TRUE;
        }, reinterpret_cast<LPARAM>(g_font));
        update_interface();
        g_server = std::make_unique<SwitchServer>(window);
        g_server->start(g_listener);
        return 0;
    }
    case WM_SIZE: layout(LOWORD(lparam), HIWORD(lparam)); return 0;
    case WM_COMMAND:
        if (LOWORD(wparam) == IDM_ENGLISH || LOWORD(wparam) == IDM_CHINESE) {
            g_chinese = LOWORD(wparam) == IDM_CHINESE;
            update_interface();
            if (!switch_store::save_language(g_chinese))
                MessageBoxW(window, display({L"Cannot save the language preference.", L"无法保存界面语言设置。"}),
                            display(kTitle), MB_OK | MB_ICONWARNING);
            return 0;
        }
        if (HIWORD(wparam) == BN_CLICKED && g_server) {
            const int from = int_text(g_from), to = int_text(g_to);
            switch (LOWORD(wparam)) {
            case IDC_CALL: g_server->action(Action{ActionType::Call, from, to}); break;
            case IDC_CONNECT: g_server->action(Action{ActionType::Connect, from, to}); break;
            case IDC_HANG: g_server->action(Action{ActionType::Hang, from}); break;
            case IDC_PAGE: g_server->action(Action{ActionType::Page, 0, int_text(g_page_to), 6, utf8_text(g_page_text)}); break;
            case IDC_APPLY: {
                const auto selected = SendMessageW(g_registration, CB_GETCURSEL, 0, 0);
                if (selected == CB_ERR) break;
                Action action{ActionType::Profile, from};
                action.signal = int_text(g_signal);
                action.registration = kRegistrations[selected].code;
                action.operator_name = utf8_text(g_operator);
                g_server->action(std::move(action));
                break;
            }
            case IDC_REGISTER: {
                HWND password = GetDlgItem(window, IDC_MAIL_PASSWORD);
                if (!g_server->register_mailbox(utf8_text(GetDlgItem(window, IDC_MAIL_NAME)), utf8_text(password)))
                    append_log({L"Post office busy. Try registering the mailbox again shortly.\r\n",
                                L"邮局正忙，请稍后重新注册邮箱。\r\n"});
                SetWindowTextW(password, L"");
                break;
            }
            case IDC_CLEAR:
                g_log_history.clear();
                SetWindowTextW(g_log, L"");
                break;
            case IDC_SAVE_LISTENER: {
                switch_store::Listener next{utf8_text(GetDlgItem(window, IDC_BIND_ADDRESS)), int_text(GetDlgItem(window, IDC_BIND_PORT))};
                IN_ADDR address{};
                if (inet_pton(AF_INET,next.address.c_str(),&address)!=1 || next.port<1 || next.port>65535) {
                    MessageBoxW(window,display({L"Enter a local IPv4 address and a port from 1 to 65535.",
                                                L"请输入本机 IPv4 地址和 1 至 65535 之间的端口号。"}),
                                display({L"Listener", L"监听设置"}),MB_OK);
                    break;
                }
                const bool saved = switch_store::save_listener(next);
                if (saved) append_log({L"Saved. Restart the switch to use this listener.\r\n",
                                       L"监听设置已保存，重启交换机后生效。\r\n"});
                else MessageBoxW(window,display({L"Cannot save listener settings.", L"无法保存监听设置。"}),
                                 display({L"Listener", L"监听设置"}),MB_OK);
                break;
            }
            }
        }
        return 0;
    case WM_SWITCH_LOG: {
        std::unique_ptr<SwitchLogMessage> text(reinterpret_cast<SwitchLogMessage *>(lparam));
        append_log(std::move(*text));
        return 0;
    }
    case WM_MAIL_REGISTERED:
        append_log(wparam
            ? SwitchLogMessage{L"Mailbox created. Use 5551001 as the post office in Simon Mail.\r\n",
                       L"邮箱已创建。在 Simon Mail 中，邮局名称和拨号号码均填 5551001。\r\n"}
            : SwitchLogMessage{L"Registration failed. Use a unique mailbox name and a 4-64 character non-space ASCII password. "
                       L"5551001 is reserved for the post office.\r\n",
                       L"注册失败。请使用尚未注册的邮箱名，密码由 4 至 64 个非空格 ASCII 字符组成。5551001 是邮局保留名称。\r\n"});
        return 0;
    case WM_SWITCH_REFRESH: refresh_list(); return 0;
    case WM_DESTROY:
        if (g_server) { g_server->stop(); g_server.reset(); }
        if (g_font) DeleteObject(g_font);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    int argc = 0;
    auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 1;
    wchar_t module[32768];
    DWORD length = GetModuleFileNameW(nullptr, module, DWORD(std::size(module)));
    if (!length || length >= std::size(module)) { LocalFree(argv); return 1; }
    auto directory = std::filesystem::path(module).parent_path();
    int port_override = 0;
    for (int i = 1; i < argc; ++i) {
        const std::wstring option = argv[i];
        if (option == L"--data-dir" && i + 1 < argc) directory = argv[++i];
        else if (option == L"--port" && i + 1 < argc) port_override = _wtoi(argv[++i]);
        else if (option.rfind(L"--port=",0)==0) port_override = _wtoi(option.c_str()+7);
        else { LocalFree(argv); return 2; }
    }
    LocalFree(argv);
    if (port_override < 0 || port_override > 65535) return 2;
    g_chinese = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_CHINESE;
    const auto error = switch_store::initialize(directory, g_listener);
    if (!error.empty()) {
        const auto prefix = display({L"Cannot initialize switch data in ", L"无法初始化以下目录中的交换机数据："});
        MessageBoxW(nullptr,(prefix + directory.wstring() + L"\n" + widen(error)).c_str(),
            display(kTitle),MB_OK|MB_ICONERROR);
        return 1;
    }
    if (port_override) g_listener.port = port_override;
    g_chinese = switch_store::chinese_interface(g_chinese);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES};
    InitCommonControlsEx(&controls);
    WNDCLASSEXW klass{};
    klass.cbSize = sizeof(klass);
    klass.lpfnWndProc = window_proc;
    klass.hInstance = instance;
    klass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    klass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    klass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    klass.lpszClassName = L"SimonAmpsSwitchWindow";
    RegisterClassExW(&klass);
    HMENU menu = CreateMenu();
    HMENU languages = CreatePopupMenu();
    AppendMenuW(languages, MF_STRING, IDM_ENGLISH, L"English");
    AppendMenuW(languages, MF_STRING, IDM_CHINESE, L"简体中文");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(languages), L"语言 / &Language");
    HWND window = CreateWindowExW(0, klass.lpszClassName, display(kTitle), WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 900, 690, nullptr, menu, instance, nullptr);
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
