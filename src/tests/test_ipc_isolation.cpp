#include "tests/test_support.hpp"
#include "shared_contracts/IpcData.hpp"
#include <string>
using namespace cccaster::public_api;
int main(int argc, char **argv) {
    SharedState state{};
    state.magicVersion = IPC_VERSION_MAGIC;
    state.isHost = argc == 1;
    state.localPort = state.isHost ? 18100 : 18101;
    if (argc > 1 && std::string(argv[1]) == "reader") {
        SharedState read{};
        return IpcManager::OpenAndRead(read) && read.localPort == 18100 ? 0 : 1;
    }
    HANDLE mapping = IpcManager::CreateAndWrite(state);
    if (!mapping)
        return 2;
    if (argc > 1) {
        SharedState read{};
        const bool ok = IpcManager::OpenAndRead(read) && read.localPort == 18101;
        CloseHandle(mapping);
        return ok ? 0 : 3;
    }
    char exe[MAX_PATH]{};
    GetModuleFileNameA(nullptr, exe, sizeof(exe));
    CC_CASE("子ゲームは継承IPCを読むが別ランチャーは自分の領域を生成する");
    for (const char *mode : {"reader", "creator"}) {
        std::string cmd = std::string("\"") + exe + "\" " + mode;
        STARTUPINFOA si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        bool created = CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                                      nullptr, &si, &pi);
        CC_CHECK(created);
        if (created) {
            auto result = WaitForSingleObject(pi.hProcess, 5000);
            CC_CHECK_EQ(result, WAIT_OBJECT_0);
            if (result != WAIT_OBJECT_0)
                TerminateProcess(pi.hProcess, 4);
            DWORD exit = 99;
            GetExitCodeProcess(pi.hProcess, &exit);
            CC_CHECK_EQ(exit, 0);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
        SharedState read{};
        CC_CHECK(IpcManager::OpenAndRead(read));
        CC_CHECK_EQ(read.localPort, 18100);
        CC_CHECK(read.isHost);
    }
    CC_CASE("終了理由をDLL消失後も保持し最初の操作を維持する");
    CC_CHECK(RequestLocalGameExit(SessionExitReason::Escape));
    CC_CHECK(RequestLocalGameExit(SessionExitReason::CloseButton));
    SharedState exitState{};
    CC_CHECK(IpcManager::OpenAndRead(exitState));
    CC_CHECK(exitState.gameShutdownRequest);
    CC_CHECK_EQ(exitState.localExitReason, static_cast<uint32_t>(SessionExitReason::Escape));
    IpcManager::UpdateOrReadState([](SharedState &s) {
        s.gameShutdownRequest = false;
        s.localExitReason = 0;
        s.targetGameMode = static_cast<uint32_t>(IpcGameMode::Training);
    });
    CC_CHECK(!RequestLocalGameExit(SessionExitReason::Escape));
    CC_CHECK(IpcManager::OpenAndRead(exitState));
    CC_CHECK(!exitState.gameShutdownRequest);
    CloseHandle(mapping);
    return cccaster::test::Summarize("ipc_isolation");
}
