#pragma once
// ============================================================================
// NetplayManager — ネット対戦の通信管理
//
// 【責務】
//   UDP ソケットのライフサイクル管理と送信関数の提供。
//   パケット受信時は PacketRouter にディスパッチする。
//
// 【公開 API】
//   - Initialize()     : UDP ソケット生成・バインド・受信コールバック登録
//   - Shutdown()        : ソケット破棄
//   - GetSendFunc()     : NetplaySession 向け送信ラムダ
//   - GetUdpSocket()    : NetplaySession 向けソケット直接アクセス
//   - IsHost()          : ホスト/クライアント判定
//
// 【adapter_netplay 層の意図】
//   ネット対戦という固有概念に必要な汎用基盤を提供する。
//   ゲーム固有のメモリパッチや FastBoot はここに含まない。
//
// 【旧 GameHooks からの変更点】
//   - NOP パッチ / キーボードクリア → MbaaPatcher に移動
//   - FastBoot ループ → FastBootRunner に移動
//   - ReadMemory / WriteMemory テンプレート → 削除（MemoryPatcher で代替）
//   - TimeHooks::Initialize() 呼び出し → dllmain.cpp に移動
// ============================================================================

#include <cstdint>
#include <string>
#include <memory>
#include <functional>
#include <vector>
#include "core_dll/network/UdpSocket.hpp"

namespace cccaster::netplay {

class NetplayManager {
  public:
    static NetplayManager &GetInstance() {
        static NetplayManager instance;
        return instance;
    }

    /// UDP ソケット生成・バインド・受信開始。
    void Initialize(bool isNetplay, bool isHost, uint16_t localPort, uint16_t targetPort,
                    const std::string &targetIp);

    /// ソケット破棄。
    void Shutdown();

    /// ホスト（P1/サーバー）かどうか。
    bool IsHost() const {
        return _isHost;
    }

    /// UDP ソケットへの直接アクセス。
    cccaster::network::UdpSocket *GetUdpSocket() const {
        return _udpSocket.get();
    }

    /// @brief SceneRunner::SendFunc 互換のラムダを返す。
    ///
    /// 内部で UdpSocket::Send(peerIp, peerPort, data) を呼ぶラムダを生成する。
    /// 非ネットプレイ（UdpSocket 未作成）の場合は nullptr を返す。
    std::function<void(const std::vector<uint8_t> &)> GetSendFunc() const {
        if (!_udpSocket)
            return nullptr;
        auto *sock = _udpSocket.get();
        auto ip = _targetIp;
        auto port = _targetPort;
        return [sock, ip, port](const std::vector<uint8_t> &data) { sock->Send(ip, port, data); };
    }

    /// 通信相手のIPアドレス。
    const std::string &GetTargetIp() const {
        return _targetIp;
    }

    /// 通信相手のポート番号。
    uint16_t GetTargetPort() const {
        return _targetPort;
    }

    /// 使用中のUDPポート番号。
    uint16_t GetPort() const {
        return _localPort;
    }

  private:
    NetplayManager() = default;
    ~NetplayManager() = default;

    bool _isNetplay = false;
    bool _isHost = false;
    uint16_t _localPort = 0;
    uint16_t _targetPort = 0;
    std::string _targetIp;
    std::unique_ptr<cccaster::network::UdpSocket> _udpSocket;
};

} // namespace cccaster::netplay
