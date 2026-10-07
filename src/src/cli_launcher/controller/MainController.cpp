#include "ProductVersion.hpp"
#include "cli_launcher/network_wrapper/SpectatorEndpoint.hpp" // ASIO/Winsock2はwindows.hより先。
#include "shared_contracts/NetplaySettings.hpp"
#include "cli_launcher/controller/MainController.hpp"
#include "cli_launcher/controller/SessionCloseMonitor.hpp"
#include "cli_launcher/ui/ConsoleRenderer.hpp"
#include "cli_launcher/network_wrapper/SessionNegotiator.hpp"
#include "cli_launcher/network_wrapper/ConnectionHash.hpp"
#include "cli_launcher/ConfigManager.hpp"
#include "cli_launcher/GuiSession.hpp"
#include "launcher/GameLauncher.hpp"
#include "launcher/RequestNotification.hpp"
#include "shared_contracts/IpcData.hpp"
#include "shared_contracts/PlayerName.hpp"
#include "shared_contracts/SessionClosePacket.hpp"
#include "shared_contracts/SessionDiagnostics.hpp"
#include "core_dll/network/UdpSocket.hpp"
#include "p2p/Session.hpp"
#include "p2p/Watch.hpp"
#include "shared_contracts/NativePath.hpp"

#include <iostream>
#include <conio.h>
#include <windows.h>
#include <tlhelp32.h>
#include <chrono>
#include <stdexcept>
#include <filesystem>
#include <fstream>

namespace cccaster::main_app::controller {
namespace diagnostic = cccaster::session_diagnostics;

namespace {
std::string ReadGamePlayerName(const std::filesystem::path &gameDirectory) {
    std::ifstream input(gameDirectory / "System" / "NetConnect.dat", std::ios::binary);
    std::string line;
    while (std::getline(input, line)) {
        if (line.rfind("UserName", 0) != 0)
            continue;
        const auto opening = line.find('"');
        const auto closing = opening == std::string::npos ? std::string::npos : line.find('"', opening + 1);
        if (closing != std::string::npos)
            return line.substr(opening + 1, closing - opening - 1);
        break;
    }
    return {};
}
} // namespace

MainController::MainController(bool isHeadless, bool isIpv6, bool isHost, const std::string &targetIp,
                               uint16_t port, const std::string &connectionHash, bool guiSession,
                               cccaster::public_api::IpcGameMode gameMode)
    : _currentState(AppState::MainMenu), _isHeadless(isHeadless), _isIpv6(isIpv6), _isHost(isHost),
      _targetIp(targetIp), _port(port), _connectionHash(connectionHash) {
    _guiSession = guiSession;
    _targetGameMode = gameMode;
    _allowSpectators = ConfigManager::GetInt("Connection", "AllowSpectators", 1) != 0 &&
        !std::getenv("CCCASTER_SPECTATE_OFF");
    ui::ConsoleRenderer::EnableVirtualTerminalProcessing();

    if (_isHeadless) {
        _currentState = cccaster::public_api::IsLocalGameMode(gameMode) ? AppState::GameRunning
            : gameMode == cccaster::public_api::IpcGameMode::Spectator ? AppState::Spectating_WaitingForHost
            : AppState::NetplayConnection;
    }
}

bool MainController::CheckGameExecutable() {
    return GetFileAttributesA("..\\MBAA.exe") != INVALID_FILE_ATTRIBUTES;
}

void MainController::ShowGameNotFoundError() {
    ui::ConsoleRenderer::ClearScreen();
    ui::ConsoleRenderer::PrintHeader();
    std::cout << "\n  \x1b[31m[ ERROR ]\x1b[0m \"MBAA.exe\" not found.\n\n"
              << "  Please ensure this tool is placed in the \"cccaster_B\" folder\n"
              << "  inside your MBAA game directory.\n\n"
              << "  (Press any key to return to Main Menu)\n";
    _getch();
}

void MainController::LaunchAndMonitorGame() {
    struct ReleaseConnection {
        std::shared_ptr<cccaster::p2p::Result>& value;
        ~ReleaseConnection() { value.reset(); }
    } releaseConnection{_p2p};
    if (_guiSession && gui::Cancelled()) {
        diagnostic::Write(std::cout, diagnostic::Code::Cancelled, "launching"); return;
    }
    std::cout << "[Release] " CCCASTER_PRODUCT_TITLE "\n" << std::flush;
    // Trainingの開始席はP1。メニューから来た場合や直前の接続役割に依存させない。
    // 起動ナビをP2へ送るとゲームがP2側でTrainingへ入り、P1設定では操作できなくなる。
    if (cccaster::public_api::IsLocalGameMode(_targetGameMode))
        _isHost = true;
    std::cout << "  \x1b[1;36m[ INFO ]\x1b[0m Launching ..\\MBAA.exe via Launcher\\GameLauncher...\n\n"
              << std::flush;

    // EXE自身のディレクトリを基準にMBAA.exeとプレイヤー設定を解決（CWD非依存）
    wchar_t myExePath[32768]{};
    const DWORD pathLength = GetModuleFileNameW(nullptr, myExePath, std::size(myExePath));
    if (!pathLength || pathLength >= std::size(myExePath)) {
        std::cerr << "[BOOT_ERROR] code=windows stage=preflight win32=" << GetLastError() << "\n";
        return;
    }
    const auto exeDir = std::filesystem::path(myExePath).parent_path();
    const auto gameDirectory = exeDir.parent_path();

    // ---- Write IPC Shared Memory for DLL ----
    cccaster::public_api::SharedState state{};
    state.magicVersion = cccaster::public_api::IPC_VERSION_MAGIC;
    state.targetGameMode = static_cast<uint32_t>(_targetGameMode);
    state.isHost = _isHost;
    state.isIpv6 = _isIpv6;
    state.port = _port;
    state.headlessMode = _isHeadless && !_guiSession;
    if (!_targetIp.empty() && _targetIp.length() < sizeof(state.targetIp)) {
        std::strcpy(state.targetIp, _targetIp.c_str());
    }
    // Negotiation完了後のpeer情報をIPCに書き込み
    if (!_peerIp.empty() && _peerIp.length() < sizeof(state.peerIp)) {
        std::strcpy(state.peerIp, _peerIp.c_str());
    }
    state.peerPort = _peerPort;
    state.localPort = _localPort;

    // DはINIを尊重。Rは利用者指定で当面7固定とし、INIの旧値は保全する。
    const bool replay = _targetGameMode == cccaster::public_api::IpcGameMode::Replay;
    const bool localVersus = _targetGameMode == cccaster::public_api::IpcGameMode::LocalVersus;
    const bool offline = cccaster::public_api::IsLocalGameMode(_targetGameMode);
    const int delay = localVersus ? 0 : offline ? cccaster::public_api::NetplaySettings::DefaultDelay :
        ConfigManager::GetInt("Netplay", "DefaultDelay", cccaster::public_api::NetplaySettings::DefaultDelay);
    const int rollback = cccaster::public_api::NetplaySettings::DefaultRollback;
    if (!cccaster::public_api::NetplaySettings::IsValid(delay, rollback))
        throw std::runtime_error("入力ディレイは0〜8で指定してください。ロールバックはR7固定です。");
    state.delayFrames = static_cast<uint8_t>(delay);
    state.maxRollbackFrames = static_cast<uint8_t>(rollback);
    auto configuredName = ConfigManager::GetString("Player", "Name", "");
    char normalizedName[cccaster::public_api::PlayerNameSize]{};
    cccaster::public_api::NormalizePlayerName(normalizedName, configuredName.c_str(), "");
    configuredName = normalizedName;
    if (configuredName.empty()) configuredName = ReadGamePlayerName(gameDirectory);
    cccaster::public_api::NormalizePlayerName(state.playerName, configuredName.c_str(),
                                               _isHost ? "PLAYER 1" : "PLAYER 2");
    std::cout << "[Player] name=" << state.playerName << '\n';

    // Keep handle alive until game ends or Controller exits
    HANDLE hIpc = cccaster::public_api::IpcManager::CreateAndWrite(state);
    if (!hIpc) {
        std::cerr << "[BOOT_ERROR] code=ipc stage=ipc win32=" << GetLastError() << "\n";
        return;
    }

    const auto absPath = (gameDirectory / L"MBAA.exe").lexically_normal();

    // ---- Boot Game and Inject DLL ----
    cccaster::main_app::GameLauncher monitor;
    SessionCloseMonitor closeMonitor;
    SetEnvironmentVariableA("CCCASTER_SPECTATE_OFF", _allowSpectators ? nullptr : "1");

    if (!monitor.BootAndMonitor(absPath, [this](uint32_t pid) {
        if (!_p2p || !_p2p->socket) return true;
        auto protocol = _p2p->socket->DuplicateForProcess(pid);
        if (protocol.empty() || protocol.size() > sizeof(cccaster::public_api::SharedState::udpProtocol)) return false;
        return cccaster::public_api::IpcManager::UpdateOrReadState([&](cccaster::public_api::SharedState& ipc) {
            ipc.udpProtocolSize = uint32_t(protocol.size());
            std::copy(protocol.begin(),protocol.end(),ipc.udpProtocol);
            std::copy(_p2p->mac.begin(),_p2p->mac.end(),ipc.p2pMac);
            std::copy(_p2p->session.begin(),_p2p->session.end(),ipc.p2pSession);
        });
    })) {
        std::cout << "  \x1b[31m[ ERROR ]\x1b[0m Fast boot execution failed.\n";
        if (!_isHeadless) {
            std::cout << "  (Press any key to return to Main Menu)\n";
            _getch();
        }
    } else {
        std::cout << "  \x1b[32m[ SUCCESS ]\x1b[0m Native control restored. You may now play.\n";

        // トレーニングはネット同期を行わない。DLL初期化を確認して終了まで監視する。
        // DLLがポートをバインドして NetplaySession で同期を完了するのを待つ。
        // 計測開始は Launcher の起動時点ではなく、この待ちループの開始時点とする。
        std::cout << (offline ? "  [ OFFLINE ] Waiting for DLL initialization...\n"
                               : "  \x1b[1;36m[ SYNC ]\x1b[0m Waiting for DLL sync completion (15s timeout)...\n")
                  << std::flush;
        auto syncStart = std::chrono::steady_clock::now();
        bool syncOk = false;
        bool initialTimedOut = false;
        HANDLE hProcess = monitor.GetProcessHandle();

        while (true) {
            auto elapsed =
                std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - syncStart)
                    .count();

            if (closeMonitor.Poll(hProcess)) break;
            cccaster::public_api::SharedState readState{};
            if (cccaster::public_api::IpcManager::OpenAndRead(readState)) {
                if (offline ? readState.dllInitialized : readState.syncCompleted) {
                    syncOk = true;
                    break;
                }
            }

            // ゲームプロセスが異常終了またはDLLが自己終了(ExitGame)した場合
            if (hProcess && WaitForSingleObject(hProcess, 0) == WAIT_OBJECT_0) {
                std::cout << "  \x1b[33m[ INFO ]\x1b[0m Game process exited unexpectedly during sync.\n"
                          << std::flush;
                break;
            }

            if (elapsed >= 15) {
                initialTimedOut = true;
                break;
            }

            Sleep(200); // 200ms間隔でポーリング
        }

        if (syncOk) {
            // DirectDrawの起動初期化をデバッガで中断しない。自分のゲームへ同期後に接続。
            if (!monitor.StartSpikeDebugIfRequested())
                std::cerr << "[SpikeDebug] 診断開始に失敗しました。採取なしでゲームを継続します。\n";
            std::cout << (replay ? "  [ REPLAY READY ] Offline initialization completed.\n"
                                 : localVersus ? "  [ OFFLINE READY ] Local versus initialization completed.\n"
                                 : offline ? "  [ TRAINING READY ] Offline initialization completed.\n"
                                   : "  \x1b[32m[ SYNC OK ]\x1b[0m DLL synchronization completed successfully!\n")
                      << std::flush;

            // ===== ゲームプロセスの終了を監視 (UIブロッキング) =====
            std::cout << "  \x1b[1;36m[ IN GAME ]\x1b[0m Playing match. Waiting for game process to exit...\n"
                      << std::flush;
            if (hProcess) {
                // ゲーム終了まで待機
                closeMonitor.Wait(hProcess);
            }
            std::cout << "  \x1b[1;36m[ INFO ]\x1b[0m Game process exited.\n" << std::flush;
        } else {
            // 実際に初期化の期限へ到達した場合だけ、時間切れとして終了する。
            if (initialTimedOut && hProcess && WaitForSingleObject(hProcess, 0) != WAIT_OBJECT_0) {
                std::cout
                    << (offline ? "  [ INIT TIMEOUT ] DLL initialization did not complete within 15 seconds.\n"
                                 : "  \x1b[31m[ SYNC TIMEOUT ]\x1b[0m DLL sync did not complete within 15 seconds.\n")
                    << std::flush;
                std::cout << "  \x1b[31m[ ABORT ]\x1b[0m Terminating game process...\n" << std::flush;
                TerminateProcess(hProcess, 1);
                WaitForSingleObject(hProcess, 1000);
            }
        }

        closeMonitor.Finish(hProcess, _p2p ? _p2p->socket : nullptr);

        // DLLの終了処理はloader lock下。終了を確認した監視元が自分の配信ファイルだけ無効化する。
        if (!offline && hProcess && WaitForSingleObject(hProcess, 0) == WAIT_OBJECT_0) {
            const auto base = std::filesystem::path(exeDir) / "broadcast" /
                ("cccaster-score-" + std::to_string(GetProcessId(hProcess)));
            for (const auto *suffix : {".json", ".txt"}) {
                auto path = base; path += suffix;
                if (!std::filesystem::exists(path)) continue;
                auto temporary = path; temporary += ".tmp";
                std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
                if (suffix[1] == 'j') output << "{\"schema_version\":1,\"active\":false}\n";
                output.close();
                if (output) MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
            }
        }
        // 終了通知の有無と成否は別。DLLが記録した原因を構造化してGUIへ渡す。
        DWORD gameExit = 0;
        if (hProcess) GetExitCodeProcess(hProcess, &gameExit);
        const auto stage = syncOk ? "running" : offline ? "initializing" : "synchronizing";
        cccaster::public_api::SharedState finalState{};
        if (cccaster::public_api::IpcManager::OpenAndRead(finalState)) {
            auto err = static_cast<cccaster::public_api::SessionErrorType>(finalState.lastErrorCode);
            finalState.lastErrorReason[sizeof(finalState.lastErrorReason)-1] = 0;
            using Code = diagnostic::Code;
            Code outcome = Code::Completed;
            uint32_t reason = 0;
            switch (err) {
            case cccaster::public_api::SessionErrorType::PeerClosed:
                outcome = Code::PeerExit; reason = finalState.peerExitReason; break;
            case cccaster::public_api::SessionErrorType::AbortedByUser:
                outcome = Code::UserExit; reason = finalState.localExitReason; break;
            case cccaster::public_api::SessionErrorType::SyncTimeout:
                outcome = Code::StateFailure; break;
            case cccaster::public_api::SessionErrorType::PeerDisconnected:
                outcome = Code::Disconnected; break;
            case cccaster::public_api::SessionErrorType::None:
                if (finalState.localExitReason) {
                    outcome = Code::UserExit; reason = finalState.localExitReason;
                } else if (initialTimedOut) outcome = Code::InitTimeout;
                else if (!syncOk || gameExit != 0) outcome = Code::UnexpectedExit;
                break;
            default: outcome = Code::StateFailure; break;
            }
            diagnostic::Write(std::cout, outcome, stage, finalState.lastErrorReason, reason, gameExit);
            if (err == cccaster::public_api::SessionErrorType::PeerClosed) {
                // 相手の操作理由は既に表示済み。直後の空のエラー画面でログを隠さない。
                if (!_isHeadless) {
                    std::cout << "  Press any key to return to Main Menu...\n";
                    _getch();
                }
            } else if (err == cccaster::public_api::SessionErrorType::AbortedByUser) {
                std::cout << "  [ User Aborted ] The game was ended by a local operation.\n" << std::flush;
            } else if (err != cccaster::public_api::SessionErrorType::None) {
                ui::ConsoleRenderer::ClearScreen();
                ui::ConsoleRenderer::PrintHeader();
                std::cout << "\n  \x1b[1;31m====== SESSION TERMINATED WITH ERROR ======\x1b[0m\n\n";
                switch (err) {
                case cccaster::public_api::SessionErrorType::PeerClosed:
                    // 詳細な理由はSessionCloseMonitorがGUIログへ出力済み。
                    break;
                case cccaster::public_api::SessionErrorType::SyncTimeout:
                    std::cout
                        << "  [ State Failure ] Game state processing or synchronization failed.\n";
                    break;
                case cccaster::public_api::SessionErrorType::PeerDisconnected:
                    std::cout << "  [ Peer Disconnected ] Connection to the other player was lost.\n";
                    break;
                case cccaster::public_api::SessionErrorType::AbortedByUser:
                    std::cout
                        << "  [\x1b[33m User Aborted \x1b[0m] The match was aborted locally (F12 pressed).\n";
                    break;
                default:
                    std::cout << "  [\x1b[31m Unknown Error \x1b[0m] Code: " << finalState.lastErrorCode
                              << "\n";
                    break;
                }
                std::cout << "\n  ===========================================\n\n";
                if (!_isHeadless) {
                    std::cout << "  Press any key to return to Main Menu...\n";
                    _getch();
                }
            } else if (outcome == Code::Completed) {
                if (!_isHeadless)
                    std::cout << "  \x1b[32m[ OK ]\x1b[0m Match finished normally.\n";
            }
        } else diagnostic::Write(std::cout, diagnostic::Code::IpcFailure, stage, "Could not read final IPC state", 0, gameExit);
    } // <-- Missing brace closed

    if (hIpc) {
        // We do not close the IPC handle yet if the game relies on it later,
        // but since DLL already read it inside BootAndMonitor's injection phase,
        // closing it here is safe if we don't plan to poll dllInitialized.
        // For polling, we would keep it as a class member.
        CloseHandle(hIpc);
    }

    // Fix 2: Clean up terminal state and application variables post-game
    ui::ConsoleRenderer::EnableVirtualTerminalProcessing();
    ui::ConsoleRenderer::ClearScreen();
    _isHost = false;
    _isIpv6 = false;
    _p2p.reset();
}

void MainController::Run() {
    while (_currentState != AppState::Exit) {
        try {
            switch (_currentState) {
            case AppState::MainMenu:
                HandleMainMenu();
                break;
            // NetplayHostOrClient は廃止（入力内容で自動判別）
            case AppState::NetplayConnection:
                HandleNetplayConnection();
                break;
            case AppState::Spectating_WaitingForHost:
                HandleSpectateConnect();
                break;
            case AppState::GameRunning:
                LaunchAndMonitorGame();
                _currentState = _isHeadless ? AppState::Exit : AppState::MainMenu;
                break;
            default:
                _currentState = AppState::Exit;
                break;
            }
        } catch (const std::exception &e) {
            diagnostic::Write(std::cout, diagnostic::Code::Exception, "launcher", e.what());
            ui::ConsoleRenderer::ClearScreen();
            std::cout << "\n  \x1b[31m[ FATAL ERROR ]\x1b[0m An unexpected error occurred:\n"
                      << "  " << e.what() << "\n\n"
                      << "  Returning to Main Menu. Press any key to continue...\n";
            if (!_guiSession) _getch();
            _currentState = _guiSession ? AppState::Exit : AppState::MainMenu;
        } catch (...) {
            diagnostic::Write(std::cout, diagnostic::Code::Exception, "launcher", "Unknown exception");
            ui::ConsoleRenderer::ClearScreen();
            std::cout << "\n  \x1b[31m[ FATAL ERROR ]\x1b[0m An unknown error occurred.\n\n"
                      << "  Returning to Main Menu. Press any key to continue...\n";
            if (!_guiSession) _getch();
            _currentState = _guiSession ? AppState::Exit : AppState::MainMenu;
        }
    }
}

void MainController::HandleMainMenu() {
    std::vector<MenuOption> options = {{"Netplay              [Hash Connect]",
                                        [self = this]() {
                                            if (!self)
                                                return;
                                            if (!self->CheckGameExecutable()) {
                                                self->ShowGameNotFoundError();
                                                return;
                                            }
                                            self->_targetGameMode = cccaster::public_api::IpcGameMode::Versus;
                                            self->_currentState = AppState::NetplayConnection;
                                        }},
                                       {"Offline Versus       [Local 2 Players]",
                                        [self = this]() {
                                            if (!self->CheckGameExecutable()) {
                                                self->ShowGameNotFoundError();
                                                return;
                                            }
                                            self->_targetGameMode = cccaster::public_api::IpcGameMode::LocalVersus;
                                            self->_currentState = AppState::GameRunning;
                                        }},
                                       {"Training Mode        [Offline]",
                                        [self = this]() {
                                            if (!self)
                                                return;
                                            if (!self->CheckGameExecutable()) {
                                                self->ShowGameNotFoundError();
                                                return;
                                            }
                                            ui::ConsoleRenderer::ClearScreen();
                                            ui::ConsoleRenderer::PrintHeader();
                                            std::cout << "\n  Launching Offline Training Mode...\n";
                                            self->_targetGameMode =
                                                cccaster::public_api::IpcGameMode::Training;
                                            self->_currentState = AppState::GameRunning;
                                        }},
                                       {"Spectate",
                                        [self = this]() {
                                            if (!self)
                                                return;
                                            self->_currentState = AppState::Spectating_WaitingForHost;
                                        }},
                                       {"Exit", [self = this]() {
                                            if (!self)
                                                return;
                                            self->_currentState = AppState::Exit;
                                        }}};

    std::vector<std::string> labels;
    for (const auto &opt : options)
        labels.push_back(opt.label);

    int choice = ui::ConsoleRenderer::DrawMenuAndGetSelection("MAIN MENU", labels, false);
    if (choice >= 0 && choice < static_cast<int>(options.size())) {
        options[choice].onSelect();
    }
}

void MainController::HandleNetplayConnection() {
    if (_guiSession && gui::Cancelled()) {
        _currentState = AppState::Exit;
        return;
    }
    ui::ConsoleRenderer::ClearScreen();
    ui::ConsoleRenderer::PrintHeader();

    std::cout << "\n  Hash-Based Network Play\n"
              << "  ======================================================\n\n";

    network_wrapper::SessionNegotiator negotiator;
    network_wrapper::NegotiationResult negoResult;
    auto connectP2p = [&](bool host, uint16_t port, const std::string& code) {
        cccaster::p2p::Options options;
        options.host = host; options.port = port; options.code = code;
        options.allowSpectators = _allowSpectators;
        options.preference = _guiSession ? gui::hostPreference : 0;
        options.server = ConfigManager::GetString("Connection", "NtfyServer", "https://ntfy.sh");
        if (const char* server = std::getenv("CCCASTER_NTFY_SERVER")) options.server = server;
        options.offline = std::getenv("CCCASTER_P2P_OFFLINE") != nullptr;
        options.cancelled = [] { return gui::Cancelled(); };
        options.report = [host, allowSpectators=_allowSpectators, guiSession=_guiSession](const std::string& line) {
            std::cout << line << '\n' << std::flush;
            if (host && !guiSession && line.rfind("[INCOMING_REQUEST] ", 0) == 0) {
                if (ConfigManager::GetInt("Notifications", "Sound", 1))
                    cccaster::notification::PlayIncomingSound();
                if (ConfigManager::GetInt("Notifications", "FlashTaskbar", 1))
                    cccaster::notification::FlashIncomingWindow(GetConsoleWindow(), true);
            }
            if(host && !gui::matchedSession && line.rfind("[P2P_CODE] ",0)==0) network_wrapper::SessionNegotiator{}.CopyToClipboard(line.substr(11));
            if(host && allowSpectators && line.rfind("[P2P_CODE] ",0)==0)
                std::cout << "[SPECTATOR CODE] " << line.substr(11) << '\n' << std::flush;
            if(host && !guiSession && line.rfind("[P2P_MANUAL] ",0)==0)
                std::cout << "[P2P] 手動交換時は相手の返信コードを貼り付け、Enterを押してください。\n" << std::flush;
            if(!host && !guiSession && line=="[P2P_STATUS] manual_reply")
                std::cout << "[P2P] 返信コードを募集側へ送り、双方で準備ができたらEnterで接続を開始してください。\n" << std::flush;
        };
        options.manualPeer = [last = std::string{}, typed = std::string{}]() mutable {
            const char* path = std::getenv("CCCASTER_P2P_PEER_FILE");
            if(!path) {
                while(_kbhit()) {int c=_getch();if(c==0||c==224){_getch();continue;}
                    if(c=='\r'||c=='\n'){auto result=std::move(typed);typed.clear();std::cout<<'\n';return result.empty()?std::string("start"):result;}
                    if(c==8){if(!typed.empty()){typed.pop_back();std::cout<<"\b \b";}}
                    else if(c>=32&&c<=126&&typed.size()<400){typed+=char(c);std::cout<<char(c)<<std::flush;}}
                return std::string{};
            }
            std::ifstream input(cccaster::Utf8Path(path)); std::string line;
            std::getline(input,line);
            if(line==last||line.size()>400) return std::string{};
            last=line; return line;
        };
        std::cout << "[P2P] P2P接続では対戦相手にIPアドレスが伝わります。\n" << std::flush;
        _p2p = std::make_shared<cccaster::p2p::Result>(cccaster::p2p::Connect(std::move(options)));
        if(!_p2p->socket) { _p2p.reset(); return network_wrapper::NegotiationResult{}; }
        // 終了通知の世代識別も同じセッションから役割別に導出する。
        auto nonce = [&](const char* role) {auto key=cccaster::p2p::Hkdf(_p2p->mac,cccaster::p2p::Hex(_p2p->session)+role);uint64_t n=0;for(int i=0;i<8;++i)n=(n<<8)|key[i];return std::to_string(n?n:1);};
        SetEnvironmentVariableA(cccaster::public_api::startup::LocalNonceEnv,nonce(host?"host":"guest").c_str());
        SetEnvironmentVariableA(cccaster::public_api::startup::PeerNonceEnv,nonce(host?"guest":"host").c_str());
        return network_wrapper::NegotiationResult{true,_p2p->ip,_p2p->port,_p2p->socket->GetPort(),_p2p->ipv6};
    };

    if (_isHeadless) {
        // === ヘッドレスモード: 既存CLI引数ベースの分岐をそのまま使用 ===
        if (_isHost) {
            uint16_t port = (_port > 0) ? _port : 7500;
            if (!std::getenv("CCCASTER_LEGACY_HOST")) {
                negoResult = connectP2p(true,port,{});
            } else {
            std::string hash = network_wrapper::SessionNegotiator::GenerateConnectionHash(port);
            std::cout << "  [HEADLESS HOST] Hash: " << hash << "\n";
            std::cout << "  [SPECTATOR CODE] " << hash << "\n"
                      << "  観戦者は「観戦」を選び、この接続コードをそのまま入力してください。\n";
            negoResult = negotiator.RunAutomaticHost(port, hash,
                static_cast<network_wrapper::route::Preference>(_guiSession ? gui::hostPreference : 0), true);
            }
        } else if (!_connectionHash.empty()) {
            negoResult = !cccaster::p2p::NormalizeCode(_connectionHash).empty() || _connectionHash.rfind("P1-",0)==0
                ? connectP2p(false,_port ? _port : 7500,_connectionHash) : negotiator.RunNegotiationFromHash(_connectionHash);
        } else if (!_targetIp.empty()) {
            network_wrapper::RouteRequest request; request.addresses={_targetIp}; request.port=_port;
            negoResult = negotiator.RunAutomatic(std::move(request));
        } else {
            std::cout << "  \x1b[31m[ HEADLESS ERROR ]\x1b[0m No --host, --hash, or --ip specified.\n";
            _currentState = AppState::Exit;
            return;
        }
    } else {
        // === インタラクティブモード: 統合入力 ===
        // ポート番号入力 → HOST自動判別 / ハッシュ貼り付け → CLIENT自動判別
        auto inputOpt = ui::ConsoleRenderer::GetTextInputWithCancel(
            "Enter Port (HOST) or Paste Hash (JOIN)  [Empty = HOST:7500]", "", true);

        // ESCキャンセル → メインメニューに戻る
        if (!inputOpt.has_value()) {
            _currentState = AppState::MainMenu;
            return;
        }

        std::string input = inputOpt.value();

        // === 入力値の自動判別 ===
        // (1) 空文字列 → デフォルトポート(7500)でHOSTモード
        // (2) 全文字が数字[0-9]で0〜65535の範囲 → 指定ポートでHOSTモード
        // (3) それ以外 → ハッシュ文字列としてCLIENTモード
        bool isPortInput = true;
        if (!input.empty()) {
            if (input.length() > 5) {
                isPortInput = false;
            } else {
                for (char c : input) {
                    if (c < '0' || c > '9') {
                        isPortInput = false;
                        break;
                    }
                }
                if (isPortInput) {
                    int val = std::stoi(input);
                    if (val < 0 || val > 65535) {
                        isPortInput = false;
                    }
                }
            }
        }

        if (isPortInput) {
            // === HOSTモード ===
            _isHost = true;
            uint16_t port = input.empty() ? 7500 : static_cast<uint16_t>(std::stoi(input));

            negoResult = connectP2p(true,port ? port : 7500,{});
        } else {
            // === CLIENTモード（ハッシュ接続） ===
            _isHost = false;
            negoResult = !cccaster::p2p::NormalizeCode(input).empty() || input.rfind("P1-",0)==0
                ? connectP2p(false,7500,input) : negotiator.RunNegotiationFromHash(input);
        }
    }

    if (negoResult.success) {
        _isIpv6 = negoResult.isIpv6;
        // peer情報を保持 (FastBoot中の中継用)
        _peerIp = negoResult.peerIp;
        _peerPort = negoResult.peerPort;
        _localPort = negoResult.localPort;
        if(_isHost) _port = _localPort;
        if (_isHeadless) {
            std::cout
                << "  \x1b[32m[ HEADLESS ]\x1b[0m Connection established successfully. Booting game...\n";
        }
        _currentState = AppState::GameRunning;
    } else {
        diagnostic::Write(std::cout, _guiSession && gui::Cancelled() ? diagnostic::Code::Cancelled
            : diagnostic::Code::ConnectionFailure, "connecting");
        if (_isHeadless) {
            std::cout << "  \x1b[31m[ HEADLESS ERROR ]\x1b[0m Connection failed.\n";
            _currentState = AppState::Exit;
        } else {
            _currentState = AppState::MainMenu;
        }
    }
}

void MainController::HandleSpectateConnect() {
    std::string code = _connectionHash;
    if (!_isHeadless) {
        std::cout << "募集側の6文字コードを入力してください。対戦開始前でも待機できます。\n";
        const auto input = ui::ConsoleRenderer::GetTextInputWithCancel("接続コード", "", true);
        if (!input) { _currentState = AppState::MainMenu; return; }
        code = *input;
    }
    if (!cccaster::p2p::NormalizeCode(code).empty()) {
        cccaster::p2p::WatchOptions options;
        options.code = code;
        options.server = ConfigManager::GetString("Connection", "NtfyServer", "https://ntfy.sh");
        if (const char* server = std::getenv("CCCASTER_NTFY_SERVER")) options.server = server;
        options.cancelled = [this] { return (_guiSession && gui::Cancelled()) || (!_guiSession && _kbhit() && _getch() == 27); };
        options.report = [](const std::string& line) { std::cout << line << '\n' << std::flush; };
        std::cout << "[ SPECTATE ] 対戦開始まで待機します。待機中はGUIのキャンセル、CLIではESCで終了できます。\n" << std::flush;
        try {
            const auto endpoint = cccaster::p2p::WaitForSpectator(std::move(options));
            _peerIp = endpoint.ip; _peerPort = endpoint.port;
        } catch (const std::exception& e) {
            std::cout << "[WATCH_STATUS] unavailable\n[ SPECTATE ] " << e.what() << '\n' << std::flush;
            _peerIp.clear(); _peerPort = 0;
        }
        if (_peerIp.empty()) {
            _currentState = _isHeadless ? AppState::Exit : AppState::MainMenu; return;
        }
    } else if (!code.empty()) {
        network_wrapper::ConnectionHash::DecodedAddress address;
        if (!network_wrapper::ConnectionHash::DecodeSpectator(code, address)) {
            std::cout << "[WATCH_STATUS] invalid_code\n" << std::flush;
            std::cout << "[ ERROR ] 接続コードが不正、または期限切れです。募集側のコードを確認してください。\n";
            _currentState = _isHeadless ? AppState::Exit : AppState::MainMenu; return;
        }
        std::cout << "[ SPECTATE ] Checking host connection...\n" << std::flush;
        _peerIp = network_wrapper::FindSpectatorEndpoint({address.ipv4, address.ipv6, address.localIpv4}, address.port,
            [this] { return _guiSession && gui::Cancelled(); });
        _peerPort = address.port;
        if (_peerIp.empty()) {
            std::cout << "[WATCH_STATUS] " << (_guiSession && gui::Cancelled() ? "cancelled" : "unreachable") << '\n' << std::flush;
            _currentState = _isHeadless ? AppState::Exit : AppState::MainMenu; return;
        }
    } else { _peerIp = _targetIp; _peerPort = _port; }
    if (_peerIp.empty() || !_peerPort) {
        std::cout << "[WATCH_STATUS] missing_endpoint\n" << std::flush;
        std::cout << "[ ERROR ] 観戦先IP・ポートまたは募集側の接続コードを指定してください。\n";
        _currentState = _isHeadless ? AppState::Exit : AppState::MainMenu; return;
    }
    _targetGameMode = cccaster::public_api::IpcGameMode::Spectator;
    _isHost = true; // 起動ナビはP1。対戦交渉・UDPソケットは作らない。
    _localPort = 0;
    std::cout << "[ SPECTATE ] Booting game: " << _peerIp << ':' << _peerPort << '\n';
    _currentState = AppState::GameRunning;
}

} // namespace cccaster::main_app::controller
