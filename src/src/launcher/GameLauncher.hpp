#pragma once
#include <filesystem>
#include <windows.h>
#include <cstdint>
#include <functional>
#include "launcher/SpikeDebugger.hpp"
#include "shared_contracts/BootDiagnostics.hpp"
#include "shared_contracts/CheckedPatch.hpp"
namespace cccaster::main_app {
class GameLauncher {
public:
    GameLauncher();
    ~GameLauncher();
    bool BootAndMonitor(const std::filesystem::path &exePath,
                        const std::function<bool(uint32_t)>& prepareChild = {});
    bool StartSpikeDebugIfRequested();
    HANDLE GetProcessHandle() const { return _pi.hProcess; }
    const boot::Status &StartupStatus() const { return _diagnostic; }
private:
    PROCESS_INFORMATION _pi{};
    SpikeDebugger _spikeDebugger;
    DWORD _originalEntryPoint = 0;
    WORD _originalEntryPointCode = 0;
    boot::Status _diagnostic{};
    bool LaunchSuspended(const std::filesystem::path &exePath);
    bool ApplyInitialPatches();
    bool MonitorBootSequence(const std::filesystem::path &dllPath, uint32_t initializeRva);
    bool Fail(boot::Error error, DWORD systemError = 0);
    bool PatchFailure(const patch::Result &result);
    void ReportFailure() const;
};
}
