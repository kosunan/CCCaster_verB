#pragma once
#include <cstdint>
#include <cstring>
#include <vector>

namespace cccaster::public_api::startup {
inline constexpr const char *LocalNonceEnv = "CCCASTER_STARTUP_LOCAL_NONCE";
inline constexpr const char *PeerNonceEnv = "CCCASTER_STARTUP_PEER_NONCE";
inline constexpr uint32_t Magic = 0x314e5343; // CSN1、CC10同期形式とは別
struct Probe {
    uint16_t sequence = 0;
    uint64_t timestamp = 0, echoedTime = 0, processingDelay = 0;
    uint64_t nonce = 0, echoNonce = 0;
    bool extended = false;
};
inline bool Decode(const std::vector<uint8_t> &data, Probe &out) {
    out = {};
    if (data.size() != 26 && data.size() != 46) return false;
    if (data.size() == 46) {
        uint32_t magic = 0;
        std::memcpy(&magic, data.data() + 26, 4);
        if (magic != Magic) return false;
        std::memcpy(&out.nonce, data.data() + 30, 8);
        std::memcpy(&out.echoNonce, data.data() + 38, 8);
        if (!out.nonce) return false;
        out.extended = true;
    }
    std::memcpy(&out.sequence, data.data(), 2);
    std::memcpy(&out.timestamp, data.data() + 2, 8);
    std::memcpy(&out.echoedTime, data.data() + 10, 8);
    std::memcpy(&out.processingDelay, data.data() + 18, 8);
    return true;
}
inline std::vector<uint8_t> Encode(const Probe &probe) {
    std::vector<uint8_t> data(probe.extended ? 46 : 26, 0);
    std::memcpy(data.data(), &probe.sequence, 2);
    std::memcpy(data.data() + 2, &probe.timestamp, 8);
    std::memcpy(data.data() + 10, &probe.echoedTime, 8);
    std::memcpy(data.data() + 18, &probe.processingDelay, 8);
    if (probe.extended) {
        std::memcpy(data.data() + 26, &Magic, 4);
        std::memcpy(data.data() + 30, &probe.nonce, 8);
        std::memcpy(data.data() + 38, &probe.echoNonce, 8);
    }
    return data;
}
// 呼出側でpeer endpointを照合し、mutexで保護する。
struct Agreement {
    uint64_t localNonce = 0, peerNonce = 0;
    bool echoed = false, replySent = false;
    bool Receive(const Probe &probe) {
        if (!probe.extended) return true;
        if (!probe.nonce || probe.nonce == localNonce || (peerNonce && peerNonce != probe.nonce) ||
            (probe.echoNonce && probe.echoNonce != localNonce)) return false;
        peerNonce = probe.nonce;
        echoed = echoed || probe.echoNonce == localNonce;
        return true;
    }
    void Sent(uint64_t echo) { if (peerNonce && echo == peerNonce) replySent = true; }
    bool Ready() const { return localNonce && peerNonce && echoed && replySent; }
};
inline bool CanReply(const Probe &probe, uint64_t local, uint64_t peer, int64_t elapsedUs,
                     bool synced) {
    return !synced && elapsedUs >= 0 && elapsedUs < 30000000 && local && peer &&
           probe.extended && probe.timestamp != 0 && probe.nonce == peer && probe.echoNonce == local;
}
}
