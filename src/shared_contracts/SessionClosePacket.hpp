#pragma once
#include "shared_contracts/NetplaySettings.hpp"
#include <vector>
#include <cstdint>
#include <array>
#include <algorithm>
#include <cstring>
#include <cstdlib>
#ifdef _WIN32
#include <windows.h>
#endif
#include "shared_contracts/SessionExitReason.hpp"
#include "shared_contracts/StartupNegotiation.hpp"

namespace cccaster::public_api {
// 通信版10の終了通知。通常の同期パケットとは独立し、終了後のランチャーからも送れる。
inline constexpr auto SessionCloseBytes = [] {
    std::array<uint8_t, 20> packet{};
    packet[0] = 'C'; packet[1] = 'C'; packet[2] = '1'; packet[3] = '0';
    packet[5] = 0x31;
    packet[6] = NetplaySettings::WireVersion;
    return packet;
}();
inline std::vector<uint8_t> BuildSessionClosePacket() {
    return {SessionCloseBytes.begin(), SessionCloseBytes.end()};
}
inline bool IsSessionClosePacket(const std::vector<uint8_t> &packet) {
    return packet.size() == SessionCloseBytes.size() &&
           std::equal(packet.begin(), packet.end(), SessionCloseBytes.begin());
}

// 理由付き通知とACK。交渉時nonceを両方向で照合し別セッションの残存通知を拒否。
struct SessionCloseMessage {
    SessionExitReason reason = SessionExitReason::Unknown;
    bool ack = false;
    uint64_t sender = 0, receiver = 0;
};
inline uint64_t SessionNonce(const char *name) {
#ifdef _WIN32
    char stored[32]{};
    const DWORD length = GetEnvironmentVariableA(name, stored, sizeof(stored));
    const char *value = length && length < sizeof(stored) ? stored : nullptr;
#else
    const char *value = std::getenv(name);
#endif
    if (!value || !*value) return 0;
    for (auto p = value; *p; ++p) if (*p < '0' || *p > '9') return 0;
    return std::strtoull(value, nullptr, 10);
}
inline std::vector<uint8_t> EncodeSessionClose(const SessionCloseMessage &message) {
    auto data = BuildSessionClosePacket();
    data.resize(36);
    data[4] = static_cast<uint8_t>(message.reason);
    data[5] = message.ack ? 0x32 : 0x31;
    data[7] = 1; // 終了制御専用の拡張。ゲーム同期の拡張番号とは独立。
    std::memcpy(data.data() + 20, &message.sender, 8);
    std::memcpy(data.data() + 28, &message.receiver, 8);
    return data;
}
inline bool DecodeSessionClose(const std::vector<uint8_t> &data, uint64_t local, uint64_t peer,
                               SessionCloseMessage &message) {
    if (data.size() != 36 || !local || !peer || !ValidExitReason(data[4])) return false;
    if (data[5] != 0x31 && data[5] != 0x32) return false;
    auto header = SessionCloseBytes;
    header[4] = data[4]; header[5] = data[5]; header[7] = 1;
    if (!std::equal(header.begin(), header.end(), data.begin())) return false;
    std::memcpy(&message.sender, data.data() + 20, 8);
    std::memcpy(&message.receiver, data.data() + 28, 8);
    if (message.sender != peer || message.receiver != local) return false;
    message.reason = static_cast<SessionExitReason>(data[4]);
    message.ack = data[5] == 0x32;
    return true;
}
}
