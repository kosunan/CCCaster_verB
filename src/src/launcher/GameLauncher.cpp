#include "launcher/GameLauncher.hpp"
#include "shared_contracts/StartupTrace.hpp"
#include "shared_contracts/GameBuild.hpp"
#include "shared_contracts/GameCompatibility.hpp"
#include "launcher/RemoteGameImage.hpp"
#include "launcher/GameFileHash.hpp"
#include "launcher/HookDllImage.hpp"
#include "shared_contracts/ProcessMemory.hpp"
#include "BuildIdentity.hpp"
#include <tlhelp32.h>

#include <iostream>
#include <chrono>
#include <cstdint>
#include <thread>
#include <cstring>
#include <string>
#include <fstream>
#include <vector>
#include <cstdlib>
#include <filesystem>
#include <iomanip>

namespace cccaster::main_app {

namespace {
struct GameFileLock {
    HANDLE handle = INVALID_HANDLE_VALUE;
    ~GameFileLock() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
};
bool InspectGameFile(const std::filesystem::path &path, GameFileLock &file, boot::Error &failure, DWORD &systemError) {
    failure = boot::Error::GameFile;
    systemError = 0;
    using namespace cccaster::game_build;
    const auto display = path.u8string();
    std::cerr << "[GameBuild] File: " << reinterpret_cast<const char *>(display.c_str()) << "\n";
    std::error_code error;
    const auto status = std::filesystem::status(path, error);
    if (status.type() == std::filesystem::file_type::not_found) {
        failure = boot::Error::GameMissing;
        systemError = ERROR_FILE_NOT_FOUND;
        std::cerr << "[GameBuild] MBAA.exe was not found at the path above.\n"
                     "[GameBuild] Place cccaster_B directly inside the game folder, next to MBAA.exe.\n";
        return false;
    }
    if (error || !std::filesystem::is_regular_file(status)) {
        systemError = error ? DWORD(error.value()) : ERROR_DIRECTORY;
        std::cerr << "[GameBuild] Cannot access MBAA.exe as a regular file (system error="
                  << error.value() << "). Check the path and file permissions.\n";
        return false;
    }
    // 検査からCreateProcess完了まで書込み・削除共有を許可せず、検査後の差替えを防ぐ。
    file.handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file.handle == INVALID_HANDLE_VALUE) {
        systemError = GetLastError();
        std::cerr << "[GameBuild] Cannot open MBAA.exe for reading (system error=" << systemError
                  << "). Check file permissions or file locks.\n";
        return false;
    }
    LARGE_INTEGER fileSize{};
    if (!GetFileSizeEx(file.handle, &fileSize)) {
        systemError = GetLastError();
        std::cerr << "[GameBuild] Cannot read executable size (system error=" << systemError << ").\n";
        return false;
    }
    const auto size = fileSize.QuadPart;
    if (size <= 0 || size > 64 * 1024 * 1024) {
        failure = boot::Error::GameFormat;
        std::cerr << "[GameBuild] Invalid executable size: " << size << " bytes (expected 1..67108864).\n";
        return false;
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    DWORD read = 0;
    const bool readOk = ReadFile(file.handle, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr);
    if (!readOk || read != bytes.size()) {
        systemError = readOk ? ERROR_HANDLE_EOF : GetLastError();
        std::cerr << "[GameBuild] Failed to read the complete executable. Check file access and retry.\n";
        return false;
    }
    std::string sha256;
    if (FileSha256(bytes, sha256)) std::cerr << "[GameBuild] SHA-256: " << sha256 << "\n";
    else std::cerr << "[GameBuild] SHA-256 unavailable (diagnostic only).\n";
    const auto compatible = game_compat::Inspect(bytes);
    if (!compatible) {
        failure = compatible.issue == game_compat::Issue::Image ? boot::Error::GameFormat : boot::Error::GameMismatch;
        std::cerr << "[GameBuild] incompatible reason=" << game_compat::Name(compatible.issue)
                  << " site=" << compatible.name << " address=" << std::hex << compatible.address << std::dec << "\n"
                  << "[GameBuild] The required input/state layout does not match this caster.\n";
        return false;
    }
    const auto known = InspectFile(bytes).edition;
    const bool exact = SupportsRuntime(known) && sha256 == ExpectedFileSha256(known);
    std::cerr << "[GameBuild] compatibility=accepted profile=carnival140 known_exact=" << exact << "\n";
    if (exact) std::cerr << "[GameBuild] " << Name(known) << "\n";
    else std::cerr << "[GameBuild] EXE variant accepted by required input/state signatures.\n";
    return true;
}
}

namespace {
struct Handle {
    HANDLE value = nullptr;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
struct SharedBootStatus {
    Handle mapping;
    boot::Status *value = nullptr;
    SharedBootStatus(DWORD pid) {
        mapping.value = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(boot::Status), nullptr);
        if (!mapping.value) return;
        value = static_cast<boot::Status *>(MapViewOfFile(mapping.value, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(boot::Status)));
        if (value) {
            new (value) boot::Status{};
            value->processId = pid;
            std::memcpy(value->expectedBuild, CCCASTER_BUILD_ID, sizeof(value->expectedBuild));
        }
    }
    ~SharedBootStatus() { if (value) UnmapViewOfFile(value); }
};
std::filesystem::path ExecutablePath() {
    wchar_t path[32768]{};
    const auto length = GetModuleFileNameW(nullptr, path, std::size(path));
    return length && length < std::size(path) ? std::filesystem::path(path) : std::filesystem::path{};
}
// ASLRとforwarded exportを考慮し、実際に関数を持つシステムDLLの対象側基底を使う。
LPTHREAD_START_ROUTINE RemoteLoadLibrary(DWORD pid) {
    const auto local = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    HMODULE owner = nullptr;
    if (!local || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(local), &owner)) return nullptr;
    wchar_t path[32768]{};
    if (!GetModuleFileNameW(owner, path, std::size(path))) return nullptr;
    const auto name = std::filesystem::path(path).filename().wstring();
    Handle snapshot;
    for (int i = 0; i < 8; ++i) {
        snapshot.value = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (snapshot.value != INVALID_HANDLE_VALUE || GetLastError() != ERROR_BAD_LENGTH) break;
    }
    if (snapshot.value == INVALID_HANDLE_VALUE) return nullptr;
    MODULEENTRY32W entry{}; entry.dwSize = sizeof(entry);
    if (Module32FirstW(snapshot.value, &entry)) do {
        if (_wcsicmp(entry.szModule, name.c_str())) continue;
        const auto offset = reinterpret_cast<uintptr_t>(local)-reinterpret_cast<uintptr_t>(owner);
        if (offset >= entry.modBaseSize) break;
        return reinterpret_cast<LPTHREAD_START_ROUTINE>(reinterpret_cast<uintptr_t>(entry.modBaseAddr)+offset);
    } while (Module32NextW(snapshot.value, &entry));
    SetLastError(ERROR_PROC_NOT_FOUND);
    return nullptr;
}
}

GameLauncher::GameLauncher() = default;
GameLauncher::~GameLauncher() {
    _spikeDebugger.Stop();
    if (_pi.hProcess) CloseHandle(_pi.hProcess);
    if (_pi.hThread) CloseHandle(_pi.hThread);
}
bool GameLauncher::Fail(boot::Error error, DWORD systemError) {
    _diagnostic.error = error;
    _diagnostic.win32 = systemError;
    return false;
}
void GameLauncher::ReportFailure() const {
    std::cerr << "[BOOT_ERROR] code=" << boot::Name(_diagnostic.error)
              << " stage=" << boot::Name(boot::Stage(_diagnostic.stage))
              << " win32=" << _diagnostic.win32 << " address=" << std::hex << _diagnostic.address << std::dec
              << " patch=" << _diagnostic.patchName << " patch_error=" << patch::Name(patch::Error(_diagnostic.patchError))
              << " rollback_failed=" << _diagnostic.rollbackFailed << "\n"
              << "[Boot] " << boot::Message(_diagnostic.error, false) << "\n" << std::flush;
}
bool GameLauncher::LaunchSuspended(const std::filesystem::path &exePath) {
    STARTUPINFOW si{}; si.cb = sizeof(si);
    char debugMode[2]{};
    if (GetEnvironmentVariableA("CCCASTER_DEBUG_SPIKES", debugMode, sizeof(debugMode)))
        SetEnvironmentVariableA("CCCASTER_SPIN_PROBE", "1");
    auto command = L"\"" + exePath.wstring() + L"\"";
    return CreateProcessW(exePath.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED,
                          nullptr, exePath.parent_path().c_str(), &si, &_pi) || Fail(boot::Error::Windows, GetLastError());
}
bool GameLauncher::StartSpikeDebugIfRequested() {
    char mode[2]{};
    if (!GetEnvironmentVariableA("CCCASTER_DEBUG_SPIKES", mode, sizeof(mode))) return true;
    return _spikeDebugger.Start(_pi.dwProcessId, _pi.dwThreadId);
}
bool GameLauncher::ApplyInitialPatches() {
    game_build::LoadedImage image;
    game_build::PeIdentity identity;
    if (!game_build::ReadSuspendedImage(_pi.hProcess, _pi.hThread, image, identity))
        return Fail(boot::Error::Windows, GetLastError());
    _originalEntryPoint = DWORD(image.Resolve(identity.entryRva, 2));
    patch::ProcessMemory memory(_pi.hProcess);
    if (!memory.Read(_originalEntryPoint, &_originalEntryPointCode, 2)) return Fail(boot::Error::Windows, memory.LastError());
    const auto expected = std::span(reinterpret_cast<const uint8_t *>(&_originalEntryPointCode),size_t(2));
    constexpr uint8_t locked[]{0xeb,0xfe};
    const patch::Spec spec{"entry_lock", _originalEntryPoint, expected, locked};
    const auto result = patch::Apply(memory, std::span(&spec,1));
    if (!result) return PatchFailure(result);
    CONTEXT context{}; context.ContextFlags = CONTEXT_CONTROL;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    do {
        if (std::chrono::steady_clock::now() >= deadline) return Fail(boot::Error::Timeout);
        if (WaitForSingleObject(_pi.hProcess,0) != WAIT_TIMEOUT) return Fail(boot::Error::ChildExited);
        if (ResumeThread(_pi.hThread) == DWORD(-1)) return Fail(boot::Error::Windows,GetLastError());
        Sleep(1);
        if (SuspendThread(_pi.hThread) == DWORD(-1) || !GetThreadContext(_pi.hThread,&context))
            return Fail(boot::Error::Windows,GetLastError());
    } while (context.Eip != _originalEntryPoint);
    return true;
}
bool GameLauncher::PatchFailure(const patch::Result &result) {
    _diagnostic.address = uint32_t(result.address);
    _diagnostic.patchError = uint32_t(result.error);
    _diagnostic.rollbackFailed = result.rollbackFailed;
    std::snprintf(_diagnostic.patchName, sizeof(_diagnostic.patchName), "%s", result.name);
    return Fail(boot::Error::Patch, result.systemError);
}
bool GameLauncher::MonitorBootSequence(const std::filesystem::path &dllPath, uint32_t initializeRva) {
    char gateName[96]{};
    diagnostics::startup::GateName(gateName,sizeof(gateName),_pi.dwProcessId);
    Handle gate{CreateEventA(nullptr,TRUE,FALSE,gateName)};
    if (!gate.value) return Fail(boot::Error::Windows,GetLastError());
    SharedBootStatus shared(_pi.dwProcessId);
    if (!shared.value) return Fail(boot::Error::Windows,GetLastError());
    _diagnostic.stage = int32_t(boot::Stage::DllLoad);
    const auto path = dllPath.wstring();
    const auto bytes = (path.size()+1)*sizeof(wchar_t);
    void *remote = VirtualAllocEx(_pi.hProcess,nullptr,bytes,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    if (!remote) return Fail(boot::Error::Windows,GetLastError());
    patch::ProcessMemory memory(_pi.hProcess);
    if (!memory.Write(reinterpret_cast<uintptr_t>(remote),path.c_str(),bytes))
        return Fail(boot::Error::Windows,memory.LastError());
    const auto load = RemoteLoadLibrary(_pi.dwProcessId);
    if (!load) return Fail(boot::Error::Windows,GetLastError());
    Handle loader{CreateRemoteThread(_pi.hProcess,nullptr,0,load,remote,0,nullptr)};
    if (!loader.value) return Fail(boot::Error::Windows,GetLastError());
    const HANDLE loading[]{loader.value,_pi.hProcess};
    const auto loaded = WaitForMultipleObjects(2,loading,FALSE,30000);
    if (loaded == WAIT_TIMEOUT) return Fail(boot::Error::Timeout);
    if (loaded == WAIT_OBJECT_0+1) return Fail(boot::Error::ChildExited);
    if (loaded != WAIT_OBJECT_0) return Fail(boot::Error::Windows,GetLastError());
    DWORD module = 0;
    if (!GetExitCodeThread(loader.value,&module)) return Fail(boot::Error::Windows,GetLastError());
    if (!module) return Fail(boot::Error::Windows,ERROR_DLL_INIT_FAILED);
    if (!VirtualFreeEx(_pi.hProcess,remote,0,MEM_RELEASE)) return Fail(boot::Error::Windows,GetLastError());
    HANDLE remoteMapping = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(),shared.mapping.value,_pi.hProcess,&remoteMapping,0,FALSE,DUPLICATE_SAME_ACCESS))
        return Fail(boot::Error::Windows,GetLastError());
    _diagnostic.stage = int32_t(boot::Stage::Bootstrap);
    if (uint64_t(module)+initializeRva > UINT32_MAX) return Fail(boot::Error::DllFormat);
    Handle initializer{CreateRemoteThread(_pi.hProcess,nullptr,0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(uintptr_t(module)+initializeRva),remoteMapping,0,nullptr)};
    if (!initializer.value) return Fail(boot::Error::Windows,GetLastError());
    const HANDLE initializing[]{initializer.value,_pi.hProcess};
    const auto completed = WaitForMultipleObjects(2,initializing,FALSE,30000);
    // 対象が終了しても共有メモリはランチャー側で保持し、最後の段階を診断できる。
    _diagnostic.stage = InterlockedCompareExchange(reinterpret_cast<volatile LONG *>(&shared.value->stage),0,0);
    if (completed == WAIT_TIMEOUT) return Fail(boot::Error::Timeout);
    if (completed == WAIT_OBJECT_0+1) return Fail(boot::Error::ChildExited);
    if (completed != WAIT_OBJECT_0) return Fail(boot::Error::Windows,GetLastError());
    DWORD success = 0;
    if (!GetExitCodeThread(initializer.value,&success)) return Fail(boot::Error::Windows,GetLastError());
    std::memcpy(&_diagnostic,shared.value,sizeof(_diagnostic));
    if (shared.value->error != boot::Error::None) return false;
    if (success != 1 || _diagnostic.stage != int32_t(boot::Stage::Ready) ||
        std::memcmp(shared.value->dllBuild,CCCASTER_BUILD_ID,sizeof(shared.value->dllBuild)) ||
        WaitForSingleObject(gate.value,0) != WAIT_OBJECT_0) return Fail(boot::Error::Contract);
    _diagnostic.stage = int32_t(boot::Stage::EntryRelease);
    constexpr uint8_t locked[]{0xeb,0xfe};
    const auto original = std::span(reinterpret_cast<const uint8_t *>(&_originalEntryPointCode),size_t(2));
    const patch::Spec release{"entry_release",_originalEntryPoint,locked,original};
    const auto result = patch::Apply(memory,std::span(&release,1));
    if (!result) return PatchFailure(result);
    // 1でなければ想定した停止状態ではない。成功と報告しない。
    if (ResumeThread(_pi.hThread) != 1) return Fail(boot::Error::Windows,ERROR_INVALID_STATE);
    _diagnostic.stage = int32_t(boot::Stage::Running);
    std::cerr << "[BOOT_READY] build=" CCCASTER_BUILD_ID " stage=running\n" << std::flush;
    if (diagnostics::startup::Enabled())
        std::cerr << "[Startup] event=entry_release qpcUs=" << diagnostics::startup::QpcUs() << "\n";
    return true;
}
bool GameLauncher::BootAndMonitor(const std::filesystem::path &exePath,
                                 const std::function<bool(uint32_t)>& prepareChild) {
    _diagnostic = boot::Status{};
    _diagnostic.stage = int32_t(boot::Stage::Preflight);
    bool success = false;
    try {
        success = [&] {
            const auto executable = ExecutablePath();
            if (executable.empty()) return Fail(boot::Error::Windows,GetLastError());
            const auto dllPath = executable.parent_path()/L"libcccaster_hook.dll";
            // 検査からロード・初期化完了までDLLの書込み・削除共有を禁止。
            GameFileLock dll;
            dll.handle = CreateFileW(dllPath.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
            if (dll.handle == INVALID_HANDLE_VALUE) return Fail(boot::Error::DllFile,GetLastError());
            LARGE_INTEGER length{};
            if (!GetFileSizeEx(dll.handle,&length) || length.QuadPart <= 0 || length.QuadPart > 64*1024*1024)
                return Fail(boot::Error::DllFormat);
            std::vector<uint8_t> bytes(size_t(length.QuadPart));
            DWORD read = 0;
            if (!ReadFile(dll.handle,bytes.data(),DWORD(bytes.size()),&read,nullptr) || read != bytes.size())
                return Fail(boot::Error::DllFile,GetLastError());
            boot::HookDllImage image;
            if (!image.Inspect(bytes)) return Fail(boot::Error::DllFormat);
            if (!boot::Compatible(image.descriptor,CCCASTER_BUILD_ID)) return Fail(boot::Error::BuildMismatch);
            std::cerr << "[Boot] build=" CCCASTER_BUILD_ID " abi=" << boot::Abi << "\n";
            _diagnostic.stage = int32_t(boot::Stage::GameValidation);
            GameFileLock gameFile;
            boot::Error gameFailure{};
            DWORD gameSystemError = 0;
            if (!InspectGameFile(exePath,gameFile,gameFailure,gameSystemError)) return Fail(gameFailure,gameSystemError);
            _diagnostic.stage = int32_t(boot::Stage::CreateProcess);
            if (!LaunchSuspended(exePath)) return false;
            if (prepareChild && !prepareChild(_pi.dwProcessId)) return Fail(boot::Error::Ipc);
            std::cout << "[FastBoot] LaunchSuspended OK (PID=" << _pi.dwProcessId << ")\n" << std::flush;
            _diagnostic.stage = int32_t(boot::Stage::EntryLock);
            if (!ApplyInitialPatches()) return false;
            return MonitorBootSequence(dllPath,image.initializeRva);
        }();
    } catch (...) { Fail(boot::Error::Exception); }
    if (!success) {
        // この起動で作成した子だけ。停止したままのゲームや部分適用を残さない。
        if (_pi.hProcess) { TerminateProcess(_pi.hProcess,ERROR_DLL_INIT_FAILED); WaitForSingleObject(_pi.hProcess,5000); }
        ReportFailure();
    }
    return success;
}
} // namespace cccaster::main_app
