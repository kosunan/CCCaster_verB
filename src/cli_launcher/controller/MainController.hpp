#pragma once

#include <string>
#include <functional>
#include <cstdint>

#include "shared_contracts/IpcData.hpp"

namespace cccaster::main_app::controller {

enum class AppState { MainMenu, NetplayConnection, Spectating_WaitingForHost, GameRunning, Exit };

struct MenuOption {
    std::string label;
    std::function<void()> onSelect;
};

class MainController {
  public:
    MainController(bool isHeadless = false, bool isIpv6 = false, bool isHost = false,
                   const std::string &targetIp = "", uint16_t port = 0,
                   const std::string &connectionHash = "", bool guiSession = false,
                   cccaster::public_api::IpcGameMode gameMode = cccaster::public_api::IpcGameMode::Versus);

    // アプリケーションのエントリー・メインループ
    void Run();

  private:
    AppState _currentState;
    bool _isHeadless = false;
    bool _guiSession = false;
    bool _isIpv6 = false;
    bool _isHost = false;
    std::string _targetIp;
    uint16_t _port = 0;
    std::string _connectionHash; // クライアント用: 入力されたハッシュ文字列

    // Negotiation結果 (FastBoot中の中継用)
    std::string _peerIp;
    uint16_t _peerPort = 0;
    uint16_t _localPort = 0;
    cccaster::public_api::IpcGameMode _targetGameMode = cccaster::public_api::IpcGameMode::Versus;

    // 各ステートのハンドラ
    void HandleMainMenu();
    void HandleNetplayConnection();
    void HandleSpectateConnect();

    // ゲーム起動の委譲 (ランチャー層呼び出し)
    void LaunchAndMonitorGame();

    // ユーティリティ
    bool CheckGameExecutable();
    void ShowGameNotFoundError();
};

} // namespace cccaster::main_app::controller
