#pragma once
#include "cli_launcher/network_wrapper/RouteSelection.hpp"
#include "cli_launcher/network_wrapper/LegacyRelay.hpp"
#include "core_dll/network/UdpSocket.hpp"

namespace cccaster::main_app::network_wrapper {
struct RouteRequest {
    bool host = false, headless = true;
    uint16_t port = 0;
    uint32_t token = 0;
    route::Preference preference = route::Preference::Auto;
    std::vector<std::string> addresses;
    std::string localIpv6;
    std::vector<LegacyRelay::Server> relays;
    // 実ソケット試験でも製品と同じ状態機械を使う。
    std::function<bool()> cancelled;
};
struct SelectedRoute {
    std::unique_ptr<cccaster::network::UdpSocket> socket;
    std::string ip;
    uint16_t port = 0;
    bool ipv6 = false, punched = false;
    uint64_t nonce = 0;
};
SelectedRoute SelectRoute(const RouteRequest& request);
}
