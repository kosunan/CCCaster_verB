#include "cli_launcher/controller/MainController.hpp"
#include "cli_launcher/ConfigManager.hpp"
#include "core_dll/network/NetworkSimulator.hpp"
#include <iostream>
#include <fstream>
#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#endif

#include <string>
#include <cstdint>

int main(int argc, char *argv[]) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif

    // ゲーム起動前にINI設定を読み込む（Rollback設定その他の自動引渡しに使用）
    // ファイルが存在しない場合（初回起動・テスト環境）は警告のみ表示して継続。
    // GetInt() のデフォルト引数（DefaultDelay=2, MaxRollback=4）がフォールバック値として機能する。
    {
        // DLLと同じ設定を読む。起動時のカレントディレクトリに依存させない。
        std::filesystem::path configDir = std::filesystem::absolute(argv[0]).parent_path();
#ifdef _WIN32
        char executablePath[32768]{};
        const DWORD length = GetModuleFileNameA(nullptr, executablePath, sizeof(executablePath));
        if (length > 0 && length < sizeof(executablePath))
            configDir = std::filesystem::path(executablePath).parent_path();
#endif
        const std::string kConfigPath = (configDir / "cccaster_v10.ini").string();
        std::ifstream testFile(kConfigPath);
        if (testFile.is_open()) {
            testFile.close();
            cccaster::main_app::ConfigManager::Load(kConfigPath);
        } else {
            std::cout << "  \x1b[33m[ WARN ]\x1b[0m cccaster_v10.ini "
                         "が見つかりません。デフォルト値で起動します。\n";
        }
    }

    bool isHeadless = false;
    bool trainingMode = false;
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
        if (arg == "--debug-spikes") {
            SetEnvironmentVariableA("CCCASTER_DEBUG_SPIKES", "1");
        } else if (arg == "--headless") {
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
            port = static_cast<uint16_t>(std::stoi(argv[++i]));
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

    cccaster::main_app::controller::MainController appController(isHeadless, isIpv6, trainingMode || isHost, targetIp, port,
        connectionHash, false, spectatorMode ? cccaster::public_api::IpcGameMode::Spectator
                             : trainingMode ? cccaster::public_api::IpcGameMode::Training
                                            : cccaster::public_api::IpcGameMode::Versus);
    appController.Run();

    return 0;
}
