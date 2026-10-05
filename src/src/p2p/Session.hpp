#pragma once
#include "p2p/Protocol.hpp"
#include "core_dll/network/UdpSocket.hpp"
#include <functional>
#include <memory>
namespace cccaster::p2p {
struct Options {
    bool host = false, offline = false;
    bool lanDiscovery = true;
    bool allowSpectators = true;
    uint16_t port = 7500;
    int preference = 0;
    std::string code, server = "https://ntfy.sh";
    std::vector<std::string> stunServers{"stun.l.google.com", "stun1.l.google.com"};
    std::function<bool()> cancelled;
    std::function<void(const std::string &)> report;
    std::function<std::string()> manualPeer;
};
struct Result {
    std::shared_ptr<void> lifetime;
    std::shared_ptr<network::UdpSocket> socket;
    std::string ip;
    uint16_t port = 0;
    bool ipv6 = false;
    Key mac{};
    std::array<uint8_t, 8> session{};
};
Result Connect(Options options);
} // namespace cccaster::p2p
