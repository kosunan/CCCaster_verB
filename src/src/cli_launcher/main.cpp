#include "shared_contracts/NativePath.hpp"
#include "cli_launcher/controller/MainController.hpp"
#include "cli_launcher/ConfigManager.hpp"
#include "core_dll/network/NetworkSimulator.hpp"
#include <iostream>
#include <fstream>
#include <filesystem>
#include "shared_contracts/ConfigPath.hpp"
#include "gui_launcher/AppContext.hpp"
#include <shellapi.h>

#ifdef _WIN32
#include <windows.h>
#include "shared_contracts/ResourceIds.h"
#endif

#include <string>
#include <cstdint>

int main(int argc, char *argv[]) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    if (const HWND console = GetConsoleWindow()) {
        if (const auto icon = LoadIconW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(CCCASTER_APP_ICON))) {
            SendMessageW(console,WM_SETICON,ICON_BIG,reinterpret_cast<LPARAM>(icon));
            SendMessageW(console,WM_SETICON,ICON_SMALL,reinterpret_cast<LPARAM>(icon));
        }
    }
#endif

    // ゲーム起動前にINI設定を読み込む（Rollback設定その他の自動引渡しに使用）
    // ファイルが存在しない場合（初回起動・テスト環境）は警告のみ表示して継続。
    // Dの既定は2。RはINIの旧値によらず、MainControllerで当面7固定にする。
    {
        // DLLと同じ設定を読む。起動時のカレントディレクトリに依存させない。
        std::filesystem::path configDir = std::filesystem::absolute(argv[0]).parent_path();
#ifdef _WIN32
        wchar_t executablePath[32768]{};
        const DWORD length = GetModuleFileNameW(nullptr, executablePath, std::size(executablePath));
        if (length > 0 && length < std::size(executablePath))
            configDir = std::filesystem::path(executablePath).parent_path();
#endif
        const std::string kConfigPath = cccaster::PathUtf8(cccaster::ConfigPath(configDir));
        std::ifstream testFile(cccaster::Utf8Path(kConfigPath));
        if (testFile.is_open()) {
            testFile.close();
            cccaster::main_app::ConfigManager::Load(kConfigPath);
        } else {
            std::cout << "  \x1b[33m[ WARN ]\x1b[0m cccaster.ini "
                         "が見つかりません。デフォルト値で起動します。\n";
        }
    }

    // GUIの起動・取消・ログ契約を共通ワーカーで処理する。通常CUIの引数は維持する。
    if (argc > 1 && std::string(argv[1]) == "--gui-worker") {
        int count = 0;
        auto arguments = CommandLineToArgvW(GetCommandLineW(), &count);
        const auto result = cccaster::gui::RunWorker(count, arguments);
        return result < 0 ? 2 : result;
    }
    bool isHeadless = false;
    bool trainingMode = false;
    bool localVersusMode = false;
    bool replayMode = false;
    bool spectatorMode = false;
    bool isIpv6 = false;
    bool isHost = false;
    std::string targetIp = "";
    uint16_t port = 0;
    std::string connectionHash = "";

    // ネットワークシミュレーション用
    uint32_t simDelayMin = 0, simDelayMax = 0;
    uint32_t simLossPercent = 0;
    bool simEnabled = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "host") { isHost=true; isHeadless=true;
        } else if (arg == "join" && i+1<argc) { connectionHash=argv[++i]; isHeadless=true;
        } else if (arg == "spectate" && i+1<argc) { connectionHash=argv[++i]; spectatorMode=true; isHeadless=true;
        } else if (arg == "--no-spectators") { cccaster::main_app::ConfigManager::SetInt("Connection","AllowSpectators",0);
        } else if (arg == "--allow-spectators") { cccaster::main_app::ConfigManager::SetInt("Connection","AllowSpectators",1);
        } else if (arg == "--legacy-host") { isHost=true; SetEnvironmentVariableA("CCCASTER_LEGACY_HOST","1");
        } else if (arg == "--offline-network") { SetEnvironmentVariableA("CCCASTER_P2P_OFFLINE","1");
        } else if (arg == "--ntfy-server" && i+1<argc) { SetEnvironmentVariableA("CCCASTER_NTFY_SERVER",argv[++i]);
        } else if (arg == "--peer-code-file" && i+1<argc) { SetEnvironmentVariableA("CCCASTER_P2P_PEER_FILE",argv[++i]);
        } else if (arg == "--debug-spikes") {
            SetEnvironmentVariableA("CCCASTER_DEBUG_SPIKES", "1");
        } else if (arg == "--headless") {
            isHeadless = true;
        } else if (arg == "--replay") {
            replayMode = true;
            isHeadless = true;
        } else if (arg == "--offline") {
            localVersusMode = true;
            isHeadless = true;
        } else if (arg == "--training") {
            trainingMode = true;
            isHeadless = true;
        } else if (arg == "--spectate") {
            spectatorMode = true;
            isHeadless = true;
        } else if (arg == "--ipv6") {
            isIpv6 = true;
        } else if (arg == "--host") {
            isHost = true;
        } else if (arg == "--ip" && i + 1 < argc) {
            targetIp = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            const std::string text=argv[++i];
            if(text.empty()||text.size()>5||text.find_first_not_of("0123456789")!=std::string::npos||std::stoul(text)<1||std::stoul(text)>65535) {
                std::cerr << "ポート番号は1〜65535で指定してください。\n"; return 2;
            }
            port = static_cast<uint16_t>(std::stoul(text));
        } else if (arg == "--hash" && i + 1 < argc) {
            connectionHash = argv[++i];
        } else if (arg == "--sim-delay" && i + 1 < argc) {
            // 形式: "min,max" (例: "50,90")
            std::string val = argv[++i];
            auto comma = val.find(',');
            if (comma != std::string::npos) {
                simDelayMin = static_cast<uint32_t>(std::stoi(val.substr(0, comma)));
                simDelayMax = static_cast<uint32_t>(std::stoi(val.substr(comma + 1)));
            } else {
                // カンマ無しの場合は固定遅延
                simDelayMin = simDelayMax = static_cast<uint32_t>(std::stoi(val));
            }
            simEnabled = true;
        } else if (arg == "--sim-loss" && i + 1 < argc) {
            simLossPercent = static_cast<uint32_t>(std::stoi(argv[++i]));
            simEnabled = true;
        }
    }

    // ネットワークシミュレーションの有効化
    if (simEnabled) {
        cccaster::network::NetworkSimulator::Instance().Enable(simDelayMin, simDelayMax, simLossPercent);
        std::cout << "  \x1b[35m[ SIM ]\x1b[0m ネットワークシミュレーション有効: "
                  << "遅延=" << simDelayMin << "~" << simDelayMax << "ms, "
                  << "パケットロス=" << simLossPercent << "%\n";
    }

    cccaster::main_app::controller::MainController appController(isHeadless, isIpv6, trainingMode || localVersusMode || replayMode || isHost, targetIp, port,
        connectionHash, false, localVersusMode ? cccaster::public_api::IpcGameMode::LocalVersus
                             : replayMode ? cccaster::public_api::IpcGameMode::Replay
                             : spectatorMode ? cccaster::public_api::IpcGameMode::Spectator
                             : trainingMode ? cccaster::public_api::IpcGameMode::Training
                                            : cccaster::public_api::IpcGameMode::Versus);
    appController.Run();

    return 0;
}
