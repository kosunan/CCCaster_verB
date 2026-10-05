#include "p2p/Watch.hpp"
#include "p2p/Ntfy.hpp"
#include "p2p/Matching.hpp"
#include "shared_contracts/SpectatorHello.hpp"
#include <asio.hpp>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

namespace cccaster::p2p {
namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
// 一度の確認でIPv4/IPv6/LANを並行して試す。対戦用UDPには触らない。
Candidate Probe(const std::vector<Candidate> &endpoints, const std::function<bool()> &cancelled) {
    asio::io_context io;
    struct Attempt {
        asio::ip::tcp::socket socket;
        std::array<uint32_t, 4> reply{};
        bool ready = false;
        explicit Attempt(asio::io_context &io) : socket(io) {}
    };
    std::vector<std::unique_ptr<Attempt>> attempts;
    for (const auto &endpoint : endpoints) {
        auto attempt = std::make_unique<Attempt>(io);
        auto *p = attempt.get();
        attempts.push_back(std::move(attempt));
        asio::error_code error;
        const auto address = asio::ip::make_address(endpoint.ip, error);
        if (error)
            continue;
        p->socket.async_connect({address, endpoint.port}, [p](asio::error_code e) {
            if (e)
                return;
            asio::async_write(p->socket, asio::buffer(spectator::Hello), [p](asio::error_code e, size_t) {
                if (e)
                    return;
                asio::async_read(p->socket, asio::buffer(p->reply), [p](asio::error_code e, size_t) {
                    p->ready = !e && p->reply == spectator::Hello;
                });
            });
        });
    }
    const auto deadline = Clock::now() + 2s;
    while (!cancelled() && Clock::now() < deadline) {
        io.poll();
        for (size_t i = 0; i < attempts.size(); ++i)
            if (attempts[i]->ready)
                return endpoints[i];
        if (io.stopped())
            break;
        std::this_thread::sleep_for(10ms);
    }
    return {};
}
} // namespace
Candidate WaitForSpectator(WatchOptions options) {
    auto report = [&](const std::string &state) {
        if (options.report)
            options.report("[WATCH_STATUS] " + state);
    };
    auto cancelled = [&] { return options.cancelled && options.cancelled(); };
    auto code = NormalizeCode(options.code);
    if (code.empty()) {
        report("invalid_code");
        return {};
    }
    report("checking");
    Keys keys(code);
    Ntfy ntfy(options.server);
    // 新しい個人コードは登録を再利用する。観戦操作時の個別応答で一組の配信元へ進む。
    const auto registered = ntfy.Poll(keys.status);
    for (const auto& body : registered.messages) {
        std::string plain;
        if (!keys.Open(keys.status, body, plain)) continue;
        const auto record = Json::parse(plain, nullptr, false);
        if (!matching::Registration(record)) continue;
        const auto matchCode = matching::Watch(code, options.server, record, cancelled, options.report);
        if (matchCode.empty() || matchCode == code) return {};
        options.code = matchCode;
        return WaitForSpectator(std::move(options));
    }
    struct Inbox {
        std::mutex mutex;
        std::deque<std::string> messages, events;
    };
    auto inbox = std::make_shared<Inbox>();
    Subscription subscription(
        ntfy, keys.status,
        [inbox](const std::string &text) {
            std::lock_guard lock(inbox->mutex);
            if (inbox->messages.size() < 128)
                inbox->messages.push_back(text);
        },
        [inbox](const std::string &text) {
            std::lock_guard lock(inbox->mutex);
            if (inbox->events.size() < 8)
                inbox->events.push_back(text);
        },
        "latest");
    SpectatorStatus current;
    bool have = false;
    auto lookupDeadline = Clock::now() + 20s;
    auto connectDeadline = Clock::time_point::max(), nextProbe = Clock::time_point::min();
    auto readUpdates = [&]() {
        std::deque<std::string> messages, events;
        {
            std::lock_guard lock(inbox->mutex);
            messages.swap(inbox->messages);
            events.swap(inbox->events);
        }
        for (const auto &event : events)
            report(event);
        for (const auto &body : messages) {
            SpectatorStatus status;
            if (!ReadSpectatorStatus(keys, body, status))
                continue;
            if (have && status.room != current.room) {
                report("closed");
                return false;
            }
            if (have && status.revision <= current.revision)
                continue;
            const bool changed = !have || current.state != status.state;
            current = std::move(status);
            have = true;
            if (current.state == "closed") {
                report("closed");
                return false;
            }
            if (!current.allowed) {
                report("disabled");
                return false;
            }
            if (changed) {
                report(current.state == "waiting" ? "standby" : "connecting");
                connectDeadline = current.state == "busy" ? Clock::now() + 45s : Clock::time_point::max();
            }
        }
        return true;
    };
    while (!cancelled()) {
        if (!readUpdates())
            return {};
        if (!have && Clock::now() >= lookupDeadline) {
            report("unavailable");
            return {};
        }
        if (have && current.timestamp < Now() - 3600) {
            report("expired");
            return {};
        }
        if (have && current.state == "busy") {
            if (Clock::now() >= connectDeadline) {
                report("unreachable");
                return {};
            }
            if (Clock::now() >= nextProbe) {
                auto endpoint = Probe(current.candidates, cancelled);
                if (!readUpdates())
                    return {};
                if (!endpoint.ip.empty() && current.state == "busy" && !cancelled()) {
                    report("ready");
                    return endpoint;
                }
                nextProbe = Clock::now() + 250ms;
            }
        }
        std::this_thread::sleep_for(20ms);
    }
    report("cancelled");
    return {};
}
} // namespace cccaster::p2p
