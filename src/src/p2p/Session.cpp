#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include "p2p/Session.hpp"
#include "p2p/Ntfy.hpp"
#include <asio.hpp>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <map>

namespace cccaster::p2p {
namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
struct Datagram {
    Bytes bytes;
    std::string ip;
    uint16_t port;
    bool v6;
};
struct Inbox {
    std::mutex mutex;
    std::deque<Datagram> udp;
    std::deque<std::string> messages, lan;
    void Push(Datagram packet) {
        std::lock_guard lock(mutex);
        if (udp.size() < 256)
            udp.push_back(std::move(packet));
    }
    void Push(std::string text, bool local = false) {
        std::lock_guard lock(mutex);
        auto &q = local ? lan : messages;
        if (q.size() < 32)
            q.push_back(std::move(text));
    }
    template <class T> std::deque<T> Take(std::deque<T> &q) {
        std::lock_guard lock(mutex);
        std::deque<T> out;
        out.swap(q);
        return out;
    }
};
class Session : public std::enable_shared_from_this<Session> {
    Options options;
    Ntfy ntfy;
    std::shared_ptr<Inbox> inbox = std::make_shared<Inbox>();
    std::shared_ptr<network::UdpSocket> v4, v6;
    std::unique_ptr<Keys> keys;
    std::unique_ptr<Subscription> subscription;
    std::vector<Candidate> local;
    std::string code, sid, manualSid;
    const std::string room = Hex(Random(8));
    uint64_t statusRevision = 0;
    Admission admission;
    bool online = false, published = false;
    std::atomic<bool> stopped{false};
    std::mutex logMutex;
    std::thread busyThread, discoveryThread;
    SOCKET discovery = INVALID_SOCKET;
    Clock::time_point lastStatus = Clock::now();
    Clock::time_point nextPublish = Clock::time_point::min();
    void Log(const std::string &text) {
        std::lock_guard lock(logMutex);
        if (options.report)
            options.report(text);
    }
    bool Cancel() { return stopped || (options.cancelled && options.cancelled()); }
    void Sleep() { std::this_thread::sleep_for(20ms); }
    Json Message(const char *type) { return {{"v", 1}, {"type", type}, {"ts", Now()}}; }
    bool Post(const std::string &topic, const Json &message) {
        if (!online || Clock::now() < nextPublish)
            return false;
        auto result = ntfy.Post(topic, keys->Seal(topic, message.dump()));
        if (result.status == 429) {
            nextPublish = Clock::now() + std::chrono::seconds(std::max(60U, result.retryAfter));
            Log("[P2P_STATUS] rate_limited");
        }
        if (result.status != 200)
            Log("[P2P_HTTP] publish=" + std::to_string(result.status));
        return result.status == 200;
    }
    bool Status(const char *state) {
        auto j = Message("status");
        j["state"] = state;
        j["room"] = room;
        j["revision"] = ++statusRevision;
        j["spectators"] = {{"allowed", options.allowSpectators}, {"candidates", Json::array()}};
        if (options.allowSpectators && std::string(state) == "busy") {
            auto endpoints = local;
            // STUNで分かるのはUDPの外側ポート。観戦TCPの転送先とは混同しない。
            for (auto &endpoint : endpoints)
                endpoint.port = v4 ? v4->GetPort() : v6->GetPort();
            j["spectators"]["candidates"] = Candidates(endpoints);
        }
        const auto sent = Post(keys->status, j);
        if (online)
            Log(sent ? "[P2P_SERVICE] online" : "[P2P_SERVICE] unavailable");
        lastStatus = Clock::now();
        return sent;
    }
    void Subscribe(const std::string &topic) {
        subscription = std::make_unique<Subscription>(
            ntfy, topic, [box = inbox](const auto &text) { box->Push(text); },
            [this](const auto &text) { Log("[P2P_STATUS] " + text); });
    }
    std::vector<Candidate> Interfaces() {
        std::vector<Candidate> out;
        ULONG bytes = 16384;
        Bytes storage(bytes);
        auto adapters = (IP_ADAPTER_ADDRESSES *)storage.data();
        auto result = GetAdaptersAddresses(
            AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr,
            adapters, &bytes);
        if (result == ERROR_BUFFER_OVERFLOW) {
            storage.resize(bytes);
            adapters = (IP_ADAPTER_ADDRESSES *)storage.data();
            result = GetAdaptersAddresses(
                AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                nullptr, adapters, &bytes);
        }
        if (result != NO_ERROR)
            return out;
        for (auto a = adapters; a; a = a->Next)
            if (a->OperStatus == IfOperStatusUp && a->IfType != IF_TYPE_SOFTWARE_LOOPBACK)
                for (auto u = a->FirstUnicastAddress; u; u = u->Next) {
                    if (u->DadState != IpDadStatePreferred)
                        continue;
                    auto sa = u->Address.lpSockaddr;
                    bool six = sa->sa_family == AF_INET6;
                    if ((six && !v6) || (!six && !v4) ||
                        (sa->sa_family != AF_INET && sa->sa_family != AF_INET6))
                        continue;
                    char ip[64]{};
                    InetNtopA(six ? AF_INET6 : AF_INET,
                              six ? (void *)&((sockaddr_in6 *)sa)->sin6_addr
                                  : (void *)&((sockaddr_in *)sa)->sin_addr,
                              ip, sizeof(ip));
                    std::vector<Candidate> valid;
                    auto j = Json::array({{{"t", six ? "v6" : "v4lan"},
                                           {"a", ip},
                                           {"p", six ? v6->GetPort() : v4->GetPort()}}});
                    if (ReadCandidates(j, valid))
                        out.push_back(valid.front());
                }
        // OSが外向き通信で選ぶIPv6を先頭へ。プローブ用socketからパケットは送らない。
        SOCKET probe = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
        if (probe != INVALID_SOCKET) {
            sockaddr_in6 remote{};
            remote.sin6_family = AF_INET6;
            remote.sin6_port = htons(443);
            InetPtonA(AF_INET6, "2606:4700:4700::1111", &remote.sin6_addr);
            if (connect(probe, (sockaddr *)&remote, sizeof(remote)) == 0) {
                sockaddr_in6 source{};
                int size = sizeof(source);
                if (getsockname(probe, (sockaddr *)&source, &size) == 0) {
                    char ip[64]{};
                    InetNtopA(AF_INET6, &source.sin6_addr, ip, sizeof(ip));
                    std::stable_sort(out.begin(), out.end(), [&](const auto &a, const auto &b) {
                        return (a.ip == ip) > (b.ip == ip);
                    });
                }
            }
            closesocket(probe);
        }
        // LAN用に最低1枠を確保。
        std::vector<Candidate> bounded;
        for (const char *type : {"v4lan", "v6"})
            for (auto &c : out)
                if (c.type == type && bounded.size() < 6)
                    bounded.push_back(c);
        return bounded;
    }
    void Gather() {
        local = Interfaces();
        if (!v4 || options.offline) {
            Log("[P2P_CANDIDATES] " + Candidates(local).dump());
            return;
        }
        asio::io_context io;
        asio::ip::udp::resolver resolver(io);
        std::vector<Candidate> mapped;
        for (const auto &host : options.stunServers) {
            if (Cancel())
                break;
            asio::error_code error;
            auto targets = resolver.resolve(asio::ip::udp::v4(), host, "19302", error);
            if (error || targets.empty())
                continue;
            const auto ep = targets.begin()->endpoint();
            const auto tx = Random(12), request = StunRequest(tx);
            auto deadline = Clock::now() + 1200ms, next = Clock::time_point::min();
            bool found = false;
            while (!Cancel() && Clock::now() < deadline && !found) {
                if (Clock::now() >= next) {
                    v4->Send(ep.address().to_string(), ep.port(), request);
                    next = Clock::now() + 350ms;
                }
                for (const auto &p : inbox->Take(inbox->udp)) {
                    Candidate c;
                    if (!p.v6 && p.ip == ep.address().to_string() && p.port == ep.port() &&
                        StunResponse(p.bytes, tx, c)) {
                        mapped.push_back(c);
                        found = true;
                        break;
                    }
                }
                Sleep();
            }
        }
        if (mapped.size() > 1 && (mapped[0].ip != mapped[1].ip || mapped[0].port != mapped[1].port))
            Log("[P2P_STATUS] symmetric_nat_possible");
        for (auto &c : mapped)
            if (local.size() < 8 && std::none_of(local.begin(), local.end(), [&](const auto &other) {
                    return c.ip == other.ip && c.port == other.port;
                }))
                local.push_back(c);
        Log("[P2P_CANDIDATES] " + Candidates(local).dump());
    }
    void StartDiscovery() {
        if (!options.lanDiscovery)
            return;
        discovery = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (discovery == INVALID_SOCKET)
            return;
        BOOL yes = TRUE;
        setsockopt(discovery, SOL_SOCKET, SO_BROADCAST, (char *)&yes, sizeof(yes));
        if (options.host)
            setsockopt(discovery, SOL_SOCKET, SO_REUSEADDR, (char *)&yes, sizeof(yes));
        sockaddr_in bindTo{};
        bindTo.sin_family = AF_INET;
        bindTo.sin_port = htons(options.host ? 37500 : 0);
        if (bind(discovery, (sockaddr *)&bindTo, sizeof(bindTo)) != 0) {
            closesocket(discovery);
            discovery = INVALID_SOCKET;
            return;
        }
        u_long nonblock = 1;
        ioctlsocket(discovery, FIONBIO, &nonblock);
        discoveryThread = std::thread([this] {
            while (!stopped) {
                char buffer[4097];
                sockaddr_in from{};
                int size = sizeof(from);
                auto n = recvfrom(discovery, buffer, 4096, 0, (sockaddr *)&from, &size);
                if (n > 0) {
                    // 復号前の処理量を制限。ブロードキャストへの回答も通常の受付制限を通す。
                    char ip[64]{};
                    InetNtopA(AF_INET, &from.sin_addr, ip, sizeof(ip));
                    Json j = {{"body", std::string(buffer, n)}, {"ip", ip}, {"port", ntohs(from.sin_port)}};
                    inbox->Push(j.dump(), true);
                } else
                    std::this_thread::sleep_for(20ms);
            }
        });
    }
    void LocalSend(const std::string &ip, uint16_t port, const std::string &topic, const Json &j) {
        if (discovery == INVALID_SOCKET)
            return;
        auto body = keys->Seal(topic, j.dump());
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_port = htons(port);
        if (InetPtonA(AF_INET, ip.c_str(), &to.sin_addr) == 1)
            sendto(discovery, body.data(), int(body.size()), 0, (sockaddr *)&to, sizeof(to));
    }
    void Answer(const std::string &session, bool accept, const std::string &ip = {}, uint16_t port = 0) {
        auto j = Message("answer");
        j["session"] = session;
        j["accept"] = accept;
        if (accept)
            j["candidates"] = Candidates(local);
        else
            j["reason"] = "busy";
        auto topic = keys->Answer(session);
        if (ip.empty())
            Post(topic, j);
        else
            LocalSend(ip, port, topic, j);
    }
    bool Request(const std::string &body, std::vector<Candidate> &remote, const std::string &ip = {},
                 uint16_t port = 0, bool busy = false) {
        Json j;
        if (!ReadMessage(*keys, keys->request, body, j) || j["type"] != "request" ||
            !ReadCandidates(j.value("candidates", Json{}), remote))
            return false;
        auto session = j["session"].get<std::string>();
        if (!admission.Admit(session, Now()))
            return false;
        if (busy) {
            Answer(session, false, ip, port);
            return false;
        }
        Log(ip.empty() ? "[P2P_EXCHANGE] ntfy" : "[P2P_EXCHANGE] lan");
        sid = session;
        Log("[INCOMING_REQUEST] " + sid);
        Gather();
        Answer(sid, true, ip, port);
        return true;
    }
    bool Manual(std::vector<Candidate> &remote) {
        if (!options.manualPeer)
            return false;
        auto text = options.manualPeer();
        if (text.empty())
            return false;
        std::string otherCode, otherSession;
        bool host = false;
        if (!ReadDirectCode(text, otherCode, otherSession, host, remote) || otherCode != code || host ||
            otherSession != manualSid) {
            Log("[P2P_STATUS] invalid_manual_code");
            return false;
        }
        if (!admission.Admit(otherSession, Now()))
            return false;
        Log("[P2P_EXCHANGE] manual");
        sid = otherSession;
        Log("[INCOMING_REQUEST] " + sid);
        return true;
    }
    Result Punch(const std::vector<Candidate> &remote) {
        const bool can4 =
            v4 && std::any_of(remote.begin(), remote.end(), [](const auto &c) { return !c.V6(); });
        const bool can6 = v6 &&
                          std::any_of(remote.begin(), remote.end(), [](const auto &c) { return c.V6(); }) &&
                          std::any_of(local.begin(), local.end(), [](const auto &c) { return c.V6(); });
        Log("[P2P_STATUS] punching");
        Log("[IP_RESULT] IPv4 " + std::string(can4 ? "checking_punch" : "unavailable"));
        Log("[IP_RESULT] IPv6 " + std::string(can6 ? "checking_punch" : "unavailable"));
        const auto rawSid = Unhex(sid);
        Control control;
        control.host = options.host;
        std::copy(rawSid.begin(), rawSid.end(), control.session.begin());
        uint64_t sequence = uint64_t(GetTickCount64()) * 1024;
        auto send = [&](ControlType type, const std::string &ip, uint16_t port, bool six,
                        const std::string &route = {}) {
            auto socket = six ? v6 : v4;
            if (!socket)
                return;
            control.type = type;
            control.sequence = ++sequence;
            control.route = route;
            socket->Send(ip, port, EncodeControl(keys->punch, control));
        };
        struct Path {
            std::string ip;
            uint16_t port;
            bool six;
            int rank;
            std::string observedLocal;
        };
        std::map<std::string, Path> paths;
        auto deadline = Clock::now() + 10s, first = Clock::time_point::max(), next = Clock::time_point::min();
        Path selected{};
        bool fixed = false;
        auto routeKey = [](const std::string &ip, uint16_t port) { return ip + "/" + std::to_string(port); };
        auto result = [&](const Path &p) {
            Log(std::string("[IP_RESULT] IPv4 ") +
                (!can4 ? "unavailable"
                 : p.six
                     ? (std::any_of(paths.begin(), paths.end(), [](const auto &e) { return !e.second.six; })
                            ? "ok_punch"
                            : "failed")
                     : "selected_punch"));
            Log(std::string("[IP_RESULT] IPv6 ") +
                (!can6 ? "unavailable"
                 : !p.six
                     ? (std::any_of(paths.begin(), paths.end(), [](const auto &e) { return e.second.six; })
                            ? "ok_punch"
                            : "failed")
                     : "selected_punch"));
            auto socket = p.six ? v6 : v4;
            socket->EnableP2p(keys->punch, control.session, p.ip, p.port, options.host);
            Log("[P2P_STATUS] connected");
            return Result{shared_from_this(), socket, p.ip, p.port, p.six, keys->punch, control.session};
        };
        while (!Cancel() && Clock::now() < deadline) {
            auto now = Clock::now();
            const auto selection = selected.observedLocal + ">" + routeKey(selected.ip, selected.port);
            if (now >= next) {
                if (fixed)
                    send(ControlType::Select, selected.ip, selected.port, selected.six, selection);
                else
                    for (const auto &c : remote)
                        if (c.V6() ? can6 : can4)
                            send(ControlType::Punch, c.ip, c.port, c.V6());
                next = now + 150ms;
            }
            for (const auto &p : inbox->Take(inbox->udp)) {
                Control incoming;
                if (!DecodeControl(keys->punch, p.bytes, incoming) || incoming.session != control.session ||
                    incoming.host == options.host)
                    continue;
                if (incoming.type == ControlType::Punch)
                    send(ControlType::Ack, p.ip, p.port, p.v6, routeKey(p.ip, p.port));
                if (incoming.type == ControlType::Ack && !incoming.route.empty()) {
                    auto id = routeKey(p.ip, p.port);
                    int rank = p.v6 ? 1 : 2;
                    for (const auto &c : remote)
                        if (c.type == "v4lan" && c.ip == p.ip)
                            rank = 0;
                    if (rank && options.preference)
                        rank = (options.preference == 1 ? !p.v6 : p.v6) ? 1 : 2;
                    paths[id] = {p.ip, p.port, p.v6, rank, incoming.route};
                    if (first == Clock::time_point::max())
                        first = now;
                }
                if (!options.host && incoming.type == ControlType::Select &&
                    incoming.route.rfind(routeKey(p.ip, p.port) + ">", 0) == 0) {
                    // SELECTを受信した経路へのACKで双方の到達を確認。確定後はDLLも再送SELECTへ応答する。
                    send(ControlType::Selected, p.ip, p.port, p.v6, incoming.route);
                    return result({p.ip, p.port, p.v6, 0, {}});
                }
                if (options.host && fixed && incoming.type == ControlType::Selected && p.ip == selected.ip &&
                    p.port == selected.port && p.v6 == selected.six && incoming.route == selection)
                    return result(selected);
            }
            if (options.host && !fixed && first != Clock::time_point::max() && now >= first + 1s) {
                selected = std::min_element(paths.begin(), paths.end(), [](const auto &a, const auto &b) {
                               return a.second.rank < b.second.rank;
                           })->second;
                fixed = true;
                next = Clock::time_point::min();
            }
            // 接続中の追加要求にもbusyを返す。
            if (options.host) {
                for (const auto &text : inbox->Take(inbox->messages)) {
                    std::vector<Candidate> unused;
                    Request(text, unused, {}, 0, true);
                }
                for (const auto &text : inbox->Take(inbox->lan)) {
                    const auto j = Json::parse(text);
                    std::vector<Candidate> unused;
                    Request(j["body"], unused, j["ip"], j["port"], true);
                }
            }
            Sleep();
        }
        Log(std::string("[IP_RESULT] IPv4 ") + (can4 ? "failed" : "unavailable"));
        Log(std::string("[IP_RESULT] IPv6 ") + (can6 ? "failed" : "unavailable"));
        Log("[P2P_STATUS] punch_timeout");
        return {};
    }
    void Busy() {
        Status("busy");
        busyThread = std::thread([this] {
            while (!stopped) {
                try {
                    for (const auto &text : inbox->Take(inbox->messages)) {
                        std::vector<Candidate> ignored;
                        Request(text, ignored, {}, 0, true);
                    }
                    for (const auto &text : inbox->Take(inbox->lan)) {
                        auto j = Json::parse(text);
                        std::vector<Candidate> ignored;
                        Request(j["body"], ignored, j["ip"], j["port"], true);
                    }
                    if (Clock::now() - lastStatus >= 30min)
                        Status("busy");
                } catch (const std::exception &) {
                }
                std::this_thread::sleep_for(100ms);
            }
        });
    }

  public:
    explicit Session(Options o) : options(std::move(o)), ntfy(options.server) {}
    ~Session() {
        stopped = true;
        subscription.reset();
        if (busyThread.joinable())
            busyThread.join();
        if (discoveryThread.joinable())
            discoveryThread.join();
        if (discovery != INVALID_SOCKET)
            closesocket(discovery);
        if (published && keys)
            try {
                Status("closed");
            } catch (const std::exception &) {
            }
    }
    Result Run() {
        std::vector<Candidate> remote;
        bool manualGuest = false;
        if (options.host)
            code = NewCode();
        else {
            bool host = false;
            std::string session;
            if (ReadDirectCode(options.code, code, session, host, remote)) {
                if (!host)
                    return {};
                manualGuest = true;
                sid = session;
            } else
                code = NormalizeCode(options.code);
            if (code.empty()) {
                Log("[P2P_STATUS] invalid_code");
                return {};
            }
        }
        Log("[P2P_STATUS] preparing");
        auto bind = [&](bool six) {
            auto socket = std::make_shared<network::UdpSocket>(options.port, six, six, true);
            if (!socket->IsValid())
                return std::shared_ptr<network::UdpSocket>{};
            socket->OnReceive([box = inbox, six](const auto &data, const auto &ip, uint16_t port) {
                box->Push({data, ip, port, six});
            });
            return socket;
        };
        v4 = bind(false);
        v6 = bind(true);
        if (!v4 && !v6) {
            Log("[P2P_STATUS] bind_failed");
            return {};
        }
        keys = std::make_unique<Keys>(code);
        online = !options.offline && !manualGuest;
        if (options.host && online) {
            for (int attempt = 0; attempt < 8 && !Cancel(); ++attempt) {
                auto result = ntfy.Poll(keys->status);
                if (result.status != 200) {
                    online = false;
                    Log("[P2P_SERVICE] unavailable");
                    Log(result.status == 429 ? "[P2P_STATUS] rate_limited" : "[P2P_STATUS] offline_fallback");
                    break;
                }
                bool live = false;
                for (auto &body : result.messages) {
                    Json j;
                    if (ReadMessage(*keys, keys->status, body, j, false) && j["type"] == "status" &&
                        j.value("state", std::string{}) != "closed" && j["ts"].get<int64_t>() > Now() - 3600)
                        live = true;
                }
                if (!live)
                    break;
                if (attempt == 7) {
                    Log("[P2P_STATUS] code_collision");
                    return {};
                }
                code = NewCode();
                keys = std::make_unique<Keys>(code);
            }
        }
        Gather();
        if (options.offline)
            Log("[P2P_SERVICE] offline");
        manualSid = Hex(Random(8));
        if (options.host) {
            if (online) {
                Subscribe(keys->request);
                const auto deadline = Clock::now() + 8s;
                while (!Cancel() && !subscription->Ready() && Clock::now() < deadline)
                    Sleep();
                if (Cancel())
                    return {};
                published = Status("waiting");
                if (published) {
                    // ntfy.shのキャッシュは非同期バッチ（公称1秒）。コード表示前に一度だけ確認する。
                    const auto settled = Clock::now() + 2s;
                    while (!Cancel() && Clock::now() < settled)
                        Sleep();
                    const auto cached = ntfy.Poll(keys->status);
                    bool visible = false;
                    for (const auto &body : cached.messages) {
                        Json status;
                        if (ReadMessage(*keys, keys->status, body, status, false) &&
                            status["type"] == "status" && status["state"] == "waiting" &&
                            status["ts"].get<int64_t>() >= Now() - 60)
                            visible = true;
                    }
                    if (!visible)
                        Log("[P2P_SERVICE] unavailable");
                }
            }
            StartDiscovery();
            Log("[HEADLESS HOST] Hash: " + code);
            Log("[P2P_CODE] " + code);
            Log("[P2P_MANUAL] " + DirectCode(code, manualSid, true, local));
            Log("[P2P_STATUS] waiting");
            while (!Cancel()) {
                bool accepted = Manual(remote);
                for (const auto &text : inbox->Take(inbox->messages)) {
                    std::vector<Candidate> candidates;
                    if (Request(text, candidates, {}, 0, accepted)) {
                        remote = std::move(candidates);
                        accepted = true;
                    }
                }
                for (const auto &text : inbox->Take(inbox->lan)) {
                    const auto j = Json::parse(text);
                    std::vector<Candidate> candidates;
                    if (Request(j["body"], candidates, j["ip"], j["port"], accepted)) {
                        remote = std::move(candidates);
                        accepted = true;
                    }
                }
                if (accepted) {
                    auto result = Punch(remote);
                    if (result.socket) {
                        Busy();
                        return result;
                    }
                    Status("waiting");
                    manualSid = Hex(Random(8));
                    Log("[P2P_MANUAL] " + DirectCode(code, manualSid, true, local));
                    Log("[P2P_STATUS] waiting");
                }
                if (Clock::now() - lastStatus >= 30min)
                    Status("waiting");
                Sleep();
            }
        } else {
            if (online) {
                auto result = ntfy.Poll(keys->status);
                Log("[P2P_HTTP] status=" + std::to_string(result.status) +
                    " messages=" + std::to_string(result.messages.size()));
                bool waiting = false;
                for (auto &body : result.messages) {
                    Json j;
                    if (ReadMessage(*keys, keys->status, body, j, false) && j["type"] == "status" &&
                        j["ts"].get<int64_t>() >= Now() - 3600 && j["ts"].get<int64_t>() <= Now() + 60) {
                        auto state = j.value("state", std::string{});
                        if (state == "busy") {
                            Log("[P2P_STATUS] busy");
                            return {};
                        }
                        if (state == "closed") {
                            Log("[P2P_STATUS] closed");
                            return {};
                        }
                        waiting = state == "waiting";
                    }
                }
                if (!waiting) {
                    online = false;
                    Log(result.status == 429 ? "[P2P_STATUS] rate_limited" : "[P2P_STATUS] offline_fallback");
                }
            }
            if (manualGuest) {
                Log("[P2P_MANUAL] " + DirectCode(code, sid, false, local));
                Log("[P2P_STATUS] manual_reply");
                // 相手が貼り付けるまで待ち、認証済みPUNCHの到着後に10秒の接続試行を始める。
                while (!Cancel()) {
                    if (options.manualPeer && options.manualPeer() == "start")
                        return Punch(remote);
                    for (const auto &p : inbox->Take(inbox->udp)) {
                        Control c;
                        if (DecodeControl(keys->punch, p.bytes, c) && c.host && Hex(c.session) == sid) {
                            inbox->Push(p);
                            return Punch(remote);
                        }
                    }
                    Sleep();
                }
                return {};
            }
            sid = Hex(Random(8));
            auto answer = keys->Answer(sid);
            StartDiscovery();
            auto request = Message("request");
            request["session"] = sid;
            request["candidates"] = Candidates(local);
            if (online) {
                Subscribe(answer);
                auto deadline = Clock::now() + 8s;
                while (!Cancel() && !subscription->Ready() && Clock::now() < deadline)
                    Sleep();
                if (!subscription->Ready()) {
                    Log("[P2P_STATUS] ntfy_unavailable");
                    online = false;
                    subscription.reset();
                } else if (!Post(keys->request, request)) {
                    Log("[P2P_STATUS] ntfy_unavailable");
                    online = false;
                    subscription.reset();
                }
            }
            auto deadline = Clock::now() + 15s, next = Clock::time_point::min();
            while (!Cancel() && Clock::now() < deadline) {
                if (Clock::now() >= next) {
                    LocalSend("255.255.255.255", 37500, keys->request, request);
                    next = Clock::now() + 1s;
                }
                auto messages = inbox->Take(inbox->messages);
                for (const auto &text : inbox->Take(inbox->lan)) {
                    auto j = Json::parse(text);
                    messages.push_back(j["body"]);
                }
                for (const auto &text : messages) {
                    Json j;
                    if (!ReadMessage(*keys, answer, text, j) || j["type"] != "answer" || j["session"] != sid)
                        continue;
                    if (!j.value("accept", false)) {
                        Log("[P2P_STATUS] busy");
                        return {};
                    }
                    if (ReadCandidates(j.value("candidates", Json{}), remote)) {
                        subscription.reset();
                        return Punch(remote);
                    }
                }
                Sleep();
            }
            Log("[P2P_STATUS] answer_timeout");
        }
        return {};
    }
};
} // namespace
Result Connect(Options options) {
    auto session = std::make_shared<Session>(std::move(options));
    return session->Run();
}
} // namespace cccaster::p2p
