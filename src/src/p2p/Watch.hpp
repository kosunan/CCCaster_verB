#pragma once
#include "p2p/Protocol.hpp"
#include <functional>
namespace cccaster::p2p {
struct WatchOptions {
    std::string code, server = "https://ntfy.sh";
    std::function<bool()> cancelled;
    std::function<void(const std::string &)> report;
};
// 状態topicの購読だけで待つ。対戦のrequest・answer・UDP受付枠を消費しない。
Candidate WaitForSpectator(WatchOptions options);
} // namespace cccaster::p2p
