#pragma once
#include "shared_contracts/SpectatorHello.hpp"
#include <asio.hpp>
#include <chrono>
#include <thread>
#include <memory>

namespace cccaster::main_app::network_wrapper {
// コードに含まれるIPv4/IPv6/LANを起動前に確認する。LAN内で外側のIPv4を
// 固定選択して失敗しないよう、同じ観戦helloが返った到達可能な候補を使う。
// ゲームスレッド/対戦UDPは使わない。確認用接続は関数終了時に閉じる。
template<class Cancelled>
std::string FindSpectatorEndpoint(const std::array<std::string, 3> &ips, uint16_t port, Cancelled cancelled) {
    asio::io_context io;
    struct Candidate {
        asio::ip::tcp::socket socket;
        std::array<uint32_t, 4> reply{};
        bool ready = false;
        explicit Candidate(asio::io_context &io) : socket(io) {}
    };
    std::array<std::unique_ptr<Candidate>, 3> candidates;
    for (size_t i = 0; i < ips.size(); ++i) {
        if (ips[i].empty()) continue;
        asio::error_code error;
        auto address = asio::ip::make_address(ips[i], error);
        if (error) continue;
        candidates[i] = std::make_unique<Candidate>(io);
        auto *p = candidates[i].get();
        p->socket.async_connect({address, port}, [p](asio::error_code e) {
            if (e) return;
            asio::async_write(p->socket, asio::buffer(spectator::Hello), [p](asio::error_code e, size_t) {
                if (e) return;
                asio::async_read(p->socket, asio::buffer(p->reply), [p](asio::error_code e, size_t) {
                    p->ready = !e && p->reply == spectator::Hello;
                });
            });
        });
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!cancelled() && std::chrono::steady_clock::now() < deadline) {
        io.poll();
        for (size_t i = 0; i < candidates.size(); ++i)
            if (candidates[i] && candidates[i]->ready) return ips[i];
        if (io.stopped()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
    return {};
}
}
