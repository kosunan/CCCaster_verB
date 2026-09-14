// ============================================================================
// NetplayManager.cpp — ネット対戦の通信管理（実装）
//
// 旧 GameHooks.cpp から UDP ソケット管理部分のみを抽出・移設。
// ============================================================================

#include "core_dll/network/NetplayManager.hpp"
#include "shared_contracts/SessionClosePacket.hpp"
#include "shared_contracts/IpcData.hpp"
#include "core_dll/network/NetworkSimulator.hpp"
#include "core_dll/common/Platform.hpp"
#include <cstdlib>
#include <cstdio>
#include "core_dll/network/PacketRouter.hpp"
#include "core_dll/common/DebugLog.hpp"
#include "core_dll/common/StartupTrace.hpp"
#include "shared_contracts/StartupNegotiation.hpp"
#include "core_dll/sync/NetplaySession.hpp"
#include <cerrno>

namespace cccaster::netplay {

// ============================================================================
// Initialize — UDP ソケット生成・バインド・受信コールバック登録
// ============================================================================
void NetplayManager::Initialize(bool isNetplay, bool isHost, uint16_t localPort, uint16_t targetPort,
                                const std::string &targetIp) {
    _isNetplay = isNetplay;
    _isHost = isHost;
    _localPort = localPort;
    _targetPort = targetPort;
    _targetIp = targetIp;

    if (!_isNetplay) {
        cccaster::domain::session::DebugLog(
            "[NetplayManager] 非ネットプレイモード。UDPソケットは作成しない。");
        return;
    }

    cccaster::domain::session::DebugLog("[NetplayManager] 通信対戦モードが有効です。");
    cccaster::domain::session::DebugLog("[NetplayManager]  - ロール        : %s",
                                        (_isHost ? "通信ホスト (サーバー)" : "通信クライアント"));
    cccaster::domain::session::DebugLog("[NetplayManager]  - 相手IPアドレス: %s", _targetIp.c_str());
    cccaster::domain::session::DebugLog("[NetplayManager]  - 相手ポート    : %u", _targetPort);
    cccaster::domain::session::DebugLog("[NetplayManager]  - 自バインドPort: %u", _localPort);

    // テスト用設定をDLL/harness自身で有効化する。対戦UDP受信にも作用する。
    if (const char *setting = std::getenv("CCCASTER_TEST_NETWORK")) {
        unsigned minMs = 0, maxMs = 0, loss = 0;
        if (std::sscanf(setting, "%u,%u,%u", &minMs, &maxMs, &loss) == 3 && minMs <= maxMs && maxMs <= 1000 &&
            loss <= 100) {
            cccaster::network::NetworkSimulator::Instance().SetClock(cccaster::platform::RealMonotonicUs);
            // 両端で有効にしても、各パケットに遅延・損失を1回だけ適用する。
            cccaster::network::NetworkSimulator::Instance().Enable(minMs, maxMs, loss, true);
            cccaster::domain::session::DebugLog("[NetworkTest] receive delay=%u..%u ms loss=%u%%", minMs,
                                                maxMs, loss);
        }
    }
    try {
        auto readNonce = [](const char *name) -> uint64_t {
            const char *value = std::getenv(name);
            if (!value || !*value) return 0;
            for (const char *p = value; *p; ++p) if (*p < '0' || *p > '9') return 0;
            char *end = nullptr;
            errno = 0;
            const auto nonce = std::strtoull(value, &end, 10);
            return errno || !end || *end ? 0 : nonce;
        };
        const auto localNonce = readNonce(cccaster::public_api::startup::LocalNonceEnv);
        const auto peerNonce = readNonce(cccaster::public_api::startup::PeerNonceEnv);
        const auto began = cccaster::platform::RealMonotonicUs();
        _udpSocket = std::make_unique<cccaster::network::UdpSocket>(_localPort, targetIp.find(':') != std::string::npos);
        if (!_udpSocket->ConfigurePeer(targetIp, targetPort))
            throw std::runtime_error("UDP peer configuration failed");
        auto *socket = _udpSocket.get();
        _udpSocket->OnReceive([socket, localNonce, peerNonce, began, targetIp, targetPort, reportedReply = false]
                             (const std::vector<uint8_t> &data, const std::string &ip, uint16_t port) mutable {
            namespace startup = cccaster::public_api::startup;
            if (ip == targetIp && port == targetPort && cccaster::public_api::IsSessionClosePacket(data)) {
                cccaster::public_api::IpcManager::UpdateOrReadState([](cccaster::public_api::SharedState &s) {
                    s.peerExitReason = 0;
                    s.lastErrorCode = static_cast<uint32_t>(cccaster::public_api::SessionErrorType::PeerClosed);
                });
                return;
            }
            if (data.size() == 36 && ip == targetIp && port == targetPort) {
                cccaster::public_api::SessionCloseMessage close;
                if (cccaster::public_api::DecodeSessionClose(data, localNonce, peerNonce, close)) {
                    if (!close.ack) {
                        // DLLは受信内容をランチャーへ渡すだけ。ACKはゲーム終了後にランチャーが送る。
                        cccaster::public_api::IpcManager::UpdateOrReadState([&](cccaster::public_api::SharedState &s) {
                            s.peerExitReason = static_cast<uint32_t>(close.reason);
                            s.lastErrorCode = static_cast<uint32_t>(cccaster::public_api::SessionErrorType::PeerClosed);
                        });
                    }
                    return;
                }
            }
            // 最後の確認が喪失しても、移管先で同じnonceの交渉だけを救済する。
            // ゲーム同期済み、30秒経過後、別peer/別世代には応答しない。
            if (data.size() == 46 && ip == targetIp && port == targetPort && localNonce && peerNonce) {
                startup::Probe probe;
                if (startup::Decode(data, probe) && startup::CanReply(
                        probe, localNonce, peerNonce, cccaster::platform::RealMonotonicUs() - began,
                        cccaster::core::netplay::NetplaySession::GetState().isSynced.load(std::memory_order_acquire))) {
                    startup::Probe reply;
                    reply.extended = true;
                    reply.sequence = probe.sequence;
                    // 0は移管先からの最終応答。DLL同士が再応答し続けないよう区別する。
                    reply.timestamp = 0;
                    reply.echoedTime = probe.timestamp;
                    reply.nonce = localNonce;
                    reply.echoNonce = peerNonce;
                    socket->Send(ip, port, startup::Encode(reply));
                    if (!reportedReply) {
                        reportedReply = true;
                        cccaster::diagnostics::startup::Mark("negotiation_dll_reply");
                    }
                    cccaster::domain::session::DebugLog("[StartupHandoff] replied to negotiated peer probe");
                    return;
                }
            }
            cccaster::core::network::PacketRouter::OnPacket(data, ip, port);
        });
        cccaster::domain::session::DebugLog(
            "[NetplayManager] UdpSocket をポート %u でバインド成功。受信ループ稼働。", _localPort);
    } catch (const std::exception &e) {
        cccaster::domain::session::DebugLog("[NetplayManager] UdpSocket のバインドに失敗: %s", e.what());
    }
}

// ============================================================================
// Shutdown — ソケット破棄
// ============================================================================
void NetplayManager::Shutdown() {
    _udpSocket.reset();
    cccaster::domain::session::DebugLog("[NetplayManager] シャットダウン完了。");
}

} // namespace cccaster::netplay
