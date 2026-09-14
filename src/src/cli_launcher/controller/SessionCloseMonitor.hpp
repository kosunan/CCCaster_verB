#pragma once
#include "shared_contracts/IpcData.hpp"
#include "shared_contracts/SessionClosePacket.hpp"
#include "core_dll/network/UdpSocket.hpp"
#include <atomic>
#include <iostream>
#include <chrono>

namespace cccaster::main_app::controller {
// CLIとGUI workerの共通監視。ゲーム/DLLが消えた後も送信・再送・ACK受信を続ける。
class SessionCloseMonitor {
    bool localReported = false, peerReported = false;
    void Report(const cccaster::public_api::SharedState &s) {
        using namespace cccaster::public_api;
        if (s.gameShutdownRequest && !localReported) {
            std::cout << "[ LOCAL CLOSED ] reason=" << s.localExitReason << " 自分: "
                      << ExitReasonText(static_cast<SessionExitReason>(s.localExitReason)) << '\n' << std::flush;
            localReported = true;
        }
        if (s.lastErrorCode == static_cast<uint32_t>(SessionErrorType::PeerClosed) && !peerReported) {
            std::cout << "[ PEER CLOSED ] reason=" << s.peerExitReason << " 相手: "
                      << ExitReasonText(static_cast<SessionExitReason>(s.peerExitReason)) << '\n' << std::flush;
            peerReported = true;
        }
    }
  public:
    bool Poll(HANDLE game) {
        using namespace cccaster::public_api;
        if (!game || WaitForSingleObject(game, 0) == WAIT_OBJECT_0) return true;
        SharedState state{};
        if (IpcManager::OpenAndRead(state) && state.targetGameMode == static_cast<uint32_t>(IpcGameMode::Versus) &&
            (state.gameShutdownRequest || state.lastErrorCode == static_cast<uint32_t>(SessionErrorType::PeerClosed))) {
            Report(state);
            // 起動したゲームのハンドルだけを終了する。DLLの終了処理には依存しない。
            if (!TerminateProcess(game, 0) && WaitForSingleObject(game, 0) != WAIT_OBJECT_0)
                std::cout << "[ CLOSE FAILED ] ゲーム終了要求に失敗: " << GetLastError() << '\n' << std::flush;
            return WaitForSingleObject(game, 1000) == WAIT_OBJECT_0;
        }
        return false;
    }
    void Wait(HANDLE game) {
        while (!Poll(game)) WaitForSingleObject(game, 10);
    }
    void Finish(HANDLE game) {
        using namespace cccaster::public_api;
        if (!game || WaitForSingleObject(game, 0) != WAIT_OBJECT_0) return;
        SharedState state{};
        if (!IpcManager::OpenAndRead(state) || state.targetGameMode != static_cast<uint32_t>(IpcGameMode::Versus)) return;
        Report(state);
        const bool initiated = state.lastErrorCode != static_cast<uint32_t>(SessionErrorType::PeerClosed) || state.gameShutdownRequest;
        if (initiated && !localReported) {
            std::cout << "[ LOCAL CLOSED ] reason=0 自分: " << ExitReasonText(SessionExitReason::Unknown) << '\n' << std::flush;
            localReported = true;
        }
        if (!state.localPort || !state.peerPort || !state.peerIp[0]) {
            std::cout << "[ CLOSE FAILED ] 終了通知の接続先を取得できません。\n" << std::flush;
            return;
        }
        const auto local = SessionNonce(startup::LocalNonceEnv), peer = SessionNonce(startup::PeerNonceEnv);
        const auto reason = state.gameShutdownRequest ? static_cast<SessionExitReason>(state.localExitReason) : SessionExitReason::Unknown;
        std::atomic<bool> confirmed{false};
        std::atomic<int> remoteReason{peerReported ? static_cast<int>(state.peerExitReason) : -1};
        cccaster::network::UdpSocket socket(state.localPort, std::strchr(state.peerIp, ':') != nullptr);
        if (!socket.IsValid()) {
            std::cout << "[ CLOSE FAILED ] 終了通知ポートを確保できません。\n" << std::flush;
            return;
        }
        socket.OnReceive([&](const std::vector<uint8_t> &packet, const std::string &ip, uint16_t port) {
            if (ip != state.peerIp || port != state.peerPort) return;
            SessionCloseMessage message;
            if (!DecodeSessionClose(packet, local, peer, message)) return;
            if (message.ack) {
                if (message.reason == reason) confirmed = true;
            } else remoteReason = static_cast<int>(message.reason);
        });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
        int attempts = 0;
        while (std::chrono::steady_clock::now() < deadline) {
            const int remote = remoteReason.load();
            if (remote >= 0) {
                if (!peerReported) {
                    state.peerExitReason = remote;
                    state.lastErrorCode = static_cast<uint32_t>(SessionErrorType::PeerClosed);
                    Report(state);
                }
                socket.Send(state.peerIp, state.peerPort, EncodeSessionClose({static_cast<SessionExitReason>(remote), true, local, peer}));
            }
            if (initiated && !confirmed) {
                // 旧交渉の場合も一般終了を通知するが、受領確認成功とは扱わない。
                socket.Send(state.peerIp, state.peerPort, local && peer
                    ? EncodeSessionClose({reason, false, local, peer}) : BuildSessionClosePacket());
                ++attempts;
            }
            if (initiated && confirmed && remote < 0) break;
            Sleep(50);
        }
        if (initiated) std::cout << (confirmed ? "[ CLOSE ACK ] 相手ランチャーの受領を確認しました。送信回数="
                                               : "[ CLOSE UNCONFIRMED ] 終了通知の受領を確認できませんでした。送信回数=")
                                 << attempts << '\n' << std::flush;
    }
};
}
