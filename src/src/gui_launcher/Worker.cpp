#include "AppContext.hpp"
#include "cli_launcher/GuiSession.hpp"
#include "cli_launcher/ConfigManager.hpp"
#include "cli_launcher/controller/MainController.hpp"
#include "shared_contracts/NetplaySettings.hpp"
#include "shared_contracts/TrainingStandby.hpp"
#include "shared_contracts/SessionDiagnostics.hpp"
#include <shellapi.h>
#include <iostream>
namespace cccaster::gui {
using namespace main_app;
int RunWorker(int argc, wchar_t** argv) {
    if (argv && (argc == 6 || argc == 7 || argc == 9) &&
        (wcscmp(argv[1], L"--worker") == 0 || wcscmp(argv[1], L"--gui-worker") == 0)) {
        main_app::gui::cancelEvent = OpenEventW(SYNCHRONIZE, FALSE, argv[2]);
        if (!main_app::gui::cancelEvent) { LocalFree(argv); return 1; }
        if (!_wfreopen(argv[3], L"wb", stdout)) { CloseHandle(main_app::gui::cancelEvent); LocalFree(argv); return 1; }
        std::cout << std::unitbuf;
        // ランチャーの版照合・起動診断はstderrへ出る。同じログへ集約する。
        std::cerr.rdbuf(std::cout.rdbuf());
        const bool replay = wcscmp(argv[4], L"replay") == 0;
        const bool training = wcscmp(argv[4], L"training") == 0;
        const bool offline = wcscmp(argv[4], L"offline") == 0;
        SetEnvironmentVariableW(training_standby::Environment, training && argc == 7 ? argv[6] : nullptr);
        const bool spectator = wcscmp(argv[4], L"spectate") == 0;
        const bool host = training || replay || offline || wcscmp(argv[4], L"host") == 0;
        const std::wstring value = argv[5];
        const auto manualPath=std::wstring(argv[3])+L".peer";
        SetEnvironmentVariableW(L"CCCASTER_P2P_PEER_FILE",manualPath.c_str());
        const auto guestPort= !host&&!spectator&&argc>=7 ? static_cast<uint16_t>(std::stoi(argv[6])) : 7500;
        if(argc>=7 && host && !training && !replay && !offline) {
            if(wcscmp(argv[6],L"1")==0) main_app::gui::hostPreference=1;
            else if(wcscmp(argv[6],L"2")==0) main_app::gui::hostPreference=2;
        }
        if (argc == 9 && wcscmp(argv[7], L"matched") == 0) {
            main_app::gui::matchedSession = true;
            ConfigManager::SetInt("Connection", "AllowSpectators", wcscmp(argv[8], L"1") == 0 ? 1 : 0);
            ConfigManager::SetInt("Netplay", "DefaultDelay", 2);
            ConfigManager::SetInt("Netplay", "MaxRollback", cccaster::public_api::NetplaySettings::DefaultRollback);
        }
        LocalFree(argv);
        int result = 0;
        try {
            controller::MainController app(true, false, host, "", host ? static_cast<uint16_t>(std::stoi(value)) : guestPort,
                                           host ? "" : std::string(value.begin(), value.end()), true,
                                           offline ? cccaster::public_api::IpcGameMode::LocalVersus
                                           : replay ? cccaster::public_api::IpcGameMode::Replay
                                           : spectator ? cccaster::public_api::IpcGameMode::Spectator
                                           : training ? cccaster::public_api::IpcGameMode::Training
                                                    : cccaster::public_api::IpcGameMode::Versus);
            app.Run();
        } catch (const std::exception& error) {
            session_diagnostics::Write(std::cout, session_diagnostics::Code::Exception, "worker", error.what()); result = 1;
        }
        CloseHandle(main_app::gui::cancelEvent); return result;
    }
    if (argv) LocalFree(argv);
    return -1;
}
}
