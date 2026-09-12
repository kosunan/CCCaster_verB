#include "launcher/GameLauncher.hpp"
#include "shared_contracts/StartupTrace.hpp"
#include "shared_contracts/GameBuild.hpp"
#include "launcher/RemoteGameImage.hpp"

#include <iostream>
#include <chrono>
#include <cstdint>
#include <thread>
#include <cstring>
#include <string>
#include <fstream>
#include <vector>
#include <cstdlib>

namespace cccaster::main_app {

namespace {
cccaster::game_build::Edition InspectGameFile(const std::string &path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    const auto size = file.tellg();
    if (!file || size <= 0 || size > 64 * 1024 * 1024) return cccaster::game_build::Edition::Unknown;
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char *>(bytes.data()), bytes.size()))
        return cccaster::game_build::Edition::Unknown;
    return cccaster::game_build::IdentifyFile(bytes);
}
}

GameLauncher::GameLauncher() : _originalEntryPoint(0), _originalEntryPointCode(0) {
    memset(&_pi, 0, sizeof(_pi));
}

GameLauncher::~GameLauncher() {
    _spikeDebugger.Stop();
    if (_pi.hProcess) {
        CloseHandle(_pi.hProcess);
    }
    if (_pi.hThread) {
        CloseHandle(_pi.hThread);
    }
}

/**
 * @brief 指定したメモリアドレスに対してバッファのデータを書き込むヘルパー。
 *        メモリの保護属性を一時的に PAGE_EXECUTE_READWRITE に変更して強引に書き換えます。
 */
void GameLauncher::WriteMemory(uintptr_t addr, const void *buffer, size_t size) {
    if (!_pi.hProcess)
        return;
    DWORD oldP;
    VirtualProtectEx(_pi.hProcess, reinterpret_cast<LPVOID>(addr), size, PAGE_EXECUTE_READWRITE, &oldP);
    WriteProcessMemory(_pi.hProcess, reinterpret_cast<LPVOID>(addr), buffer, size, nullptr);
    VirtualProtectEx(_pi.hProcess, reinterpret_cast<LPVOID>(addr), size, oldP, &oldP);
}

/**
 * @brief 指定したメモリアドレスからバッファにデータを読み込むヘルパー。
 */
void GameLauncher::ReadMemory(uintptr_t addr, void *buffer, size_t size) {
    if (!_pi.hProcess)
        return;
    ReadProcessMemory(_pi.hProcess, reinterpret_cast<LPCVOID>(addr), buffer, size, nullptr);
}

/**
 * @brief ゲーム(MBAA.exe)を**サスペンド状態(停止状態)**で起動します。
 *        これにより、ゲームプロセスが初期化処理（OSからのDLLロード等）を済ませた段階で
 *        ユーザースレッドの実行開始直前で停止します。この隙を利用して、DLLインジェクトや
 *        メモリ改ざんの準備（無限ループ化パッチ等）を仕掛けることができます。
 */
bool GameLauncher::LaunchSuspended(const std::string &exePath) {
    STARTUPINFOA si = {sizeof(si)};
    char debugMode[2]{};
    // CLIのSetEnvironmentVariableはCRTのgetenvキャッシュを更新しない。
    const bool debugSpikes = GetEnvironmentVariableA("CCCASTER_DEBUG_SPIKES", debugMode, sizeof(debugMode)) != 0;
    // 子DLLも同じ実QPC窓を採取する。通常起動の環境は変更しない。
    if (debugSpikes) SetEnvironmentVariableA("CCCASTER_SPIN_PROBE", "1");

    // 実行ファイルのパスからディレクトリ部分を抽出し、作業ディレクトリ(カレントディレクトリ)として設定
    std::string workDir = exePath.substr(0, exePath.find_last_of("\\/"));

    // CREATE_SUSPENDED フラグをつけてプロセス生成
    std::string command = "\"" + exePath + "\"";
    if (!CreateProcessA(exePath.c_str(), command.data(), NULL, NULL, FALSE, CREATE_SUSPENDED, NULL,
                        workDir.c_str(), &si, &_pi)) {
        return false;
    }
    return true;
}

bool GameLauncher::StartSpikeDebugIfRequested() {
    char mode[2]{};
    if (!GetEnvironmentVariableA("CCCASTER_DEBUG_SPIKES", mode, sizeof(mode))) return true;
    return _spikeDebugger.Start(_pi.dwProcessId, _pi.dwThreadId);
}

/**
 * @brief DLLの安全なインジェクションを保証するため、一時的なエントリポイントロックを施します。
 *        ゲーム自体の改変パッチ（非アクティブ無効や描画スキップ等）はここでは「行いません」。
 *        あくまで「安全にインジェクトするまでの足止め」のみが責務です。
 */
bool GameLauncher::ApplyInitialPatches() {
    // -------------------------------------------------------------------------
    // 0. エントリポイントのロック処理 (無限ループパッチ)
    // -------------------------------------------------------------------------
    // サスペンド解除後、ゲームの初期化スレッドが本来のコードを開始する前に、
    // エントリポイント(開始地点)の命令を一時的に `EB FE` (JMP $：無限ループ) に書き換えます。
    // この間、元の2バイト命令は `_originalEntryPointCode` に保持しておきます。
    cccaster::game_build::LoadedImage image;
    cccaster::game_build::PeIdentity identity;
    if (!cccaster::game_build::ReadSuspendedImage(_pi.hProcess, _pi.hThread, image, identity)) {
        std::cerr << "[GameBuild] Failed to resolve the loaded image.\n";
        return false;
    }

    // エントリポイントのアドレスを計算して元の2バイトを保存
    _originalEntryPoint = static_cast<DWORD>(image.Resolve(identity.entryRva, 2));
    SIZE_T transferred = 0;
    if (!ReadProcessMemory(_pi.hProcess, reinterpret_cast<void *>(uintptr_t(_originalEntryPoint)),
                           &_originalEntryPointCode, 2, &transferred) || transferred != 2) return false;
    std::cerr << "[GameBuild] base=" << std::hex << image.base << " entry=" << _originalEntryPoint
              << std::dec << "\n";

    // 無限ループにするための機械語 (EB FE)
    const WORD lock_code = 0xfeeb;
    WriteMemory(_originalEntryPoint, &lock_code, 2);
    WORD installed = 0;
    if (!ReadProcessMemory(_pi.hProcess, reinterpret_cast<void *>(uintptr_t(_originalEntryPoint)),
                           &installed, 2, &transferred) || transferred != 2 || installed != lock_code ||
        !FlushInstructionCache(_pi.hProcess, reinterpret_cast<void *>(uintptr_t(_originalEntryPoint)), 2))
        return false;

    // スレッドを再開(Resume)させますが、上記のパッチにより先頭で足踏みし続けます。
    // EIPレジスタ(プログラムカウンタ)がエントリポイントに到達するまで待ちます。
    CONTEXT ct{};
    ct.ContextFlags = CONTEXT_CONTROL;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    do {
        if (std::chrono::steady_clock::now() >= deadline ||
            WaitForSingleObject(_pi.hProcess, 0) != WAIT_TIMEOUT) return false;
        if (ResumeThread(_pi.hThread) == DWORD(-1)) return false;
        Sleep(1); // 高速化: 10ms → 1ms (エントリポイント同期は最小待機で十分)
        if (SuspendThread(_pi.hThread) == DWORD(-1)) return false;

        if (!GetThreadContext(_pi.hThread, &ct)) {
            std::cerr << "[FastBoot] Failed to get thread context.\n";
            return false;
        }
    } while (ct.Eip != _originalEntryPoint);
    return true;
}

/**
 * @brief ロック状態のゲームプロセスに対してコアDLLを注入し、その後ロックを解除して処理を委譲します。
 *        ゲームの起動シーケンスの監視や、メモリ監視はすべてDLL内（GameHooks）で行われます。
 */
bool GameLauncher::MonitorBootSequence() {
    // =========================================================================
    // [計測] FastBoot タイミング計測用
    // =========================================================================
    auto bootClock = std::chrono::steady_clock::now();
    auto elapsedMs = [&]() -> long long {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                     bootClock)
            .count();
    };

    std::cerr << "[FastBoot] === Boot Timing Start ===\n";
    char gateName[96]{};
    cccaster::diagnostics::startup::GateName(gateName, sizeof(gateName), _pi.dwProcessId);
    struct StartupGate {
        HANDLE handle;
        ~StartupGate() { if (handle) CloseHandle(handle); }
    } gate{CreateEventA(nullptr, TRUE, FALSE, gateName)};
    if (!gate.handle) return false;

    // -------------------------------------------------------------------------
    // コアDLL(cccaster_hook.dll)の注入(インジェクション)処理
    // -------------------------------------------------------------------------
    char myExeDir[MAX_PATH];
    GetModuleFileNameA(NULL, myExeDir, MAX_PATH);
    std::string dllPathStr = myExeDir;
    dllPathStr = dllPathStr.substr(0, dllPathStr.find_last_of("\\/")) + "\\libcccaster_hook.dll";

    void *pLibRemote = VirtualAllocEx(_pi.hProcess, NULL, dllPathStr.size() + 1, MEM_COMMIT, PAGE_READWRITE);
    if (pLibRemote) {
        if (!WriteProcessMemory(_pi.hProcess, pLibRemote, (void *)dllPathStr.c_str(), dllPathStr.size() + 1, NULL)) {
            VirtualFreeEx(_pi.hProcess, pLibRemote, 0, MEM_RELEASE);
            return false;
        }
        HANDLE hThread = CreateRemoteThread(
            _pi.hProcess, NULL, 0,
            (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleA("Kernel32.dll"), "LoadLibraryA"),
            pLibRemote, 0, NULL);
        if (hThread) {
            const DWORD waited = WaitForSingleObject(hThread, 30000);
            DWORD loaded = 0;
            const bool success = waited == WAIT_OBJECT_0 && GetExitCodeThread(hThread, &loaded) && loaded != 0;
            CloseHandle(hThread);
            if (!success) return false;
        } else {
            VirtualFreeEx(_pi.hProcess, pLibRemote, 0, MEM_RELEASE);
            return false;
        }
        VirtualFreeEx(_pi.hProcess, pLibRemote, 0, MEM_RELEASE);
    } else return false;
    long long tsInject = elapsedMs();
    std::cerr << "[FastBoot] [" << tsInject << "ms] DLL Injection complete\n";
    if (!cccaster::diagnostics::startup::Baseline()) {
        const HANDLE handles[] = {gate.handle, _pi.hProcess};
        if (WaitForMultipleObjects(2, handles, FALSE, 30000) != WAIT_OBJECT_0) {
            std::cerr << "[FastBoot] DLL preparation failed or timed out.\n";
            return false;
        }
    }

    // -------------------------------------------------------------------------
    // エンジン起動の開始 (エントリポイント無限ループの解除と実行の完全移行)
    // -------------------------------------------------------------------------
    // 足止めに用いていたパッチを元に戻し、スレッドの実行を再開します。
    // ここから先、ゲーム本体のパッチ適用や状態管理は注入されたDLL(GameHooks等)が主導します。
    if (_originalEntryPoint != 0) {
        WriteMemory(_originalEntryPoint, &_originalEntryPointCode, 2);
        FlushInstructionCache(_pi.hProcess, (LPCVOID)_originalEntryPoint, 2);
    }
    ResumeThread(_pi.hThread);
    if (cccaster::diagnostics::startup::Enabled())
        std::cerr << "[Startup] event=entry_release qpcUs=" << cccaster::diagnostics::startup::QpcUs() << "\n";
    long long tsResume = elapsedMs();
    std::cerr << "[FastBoot] [" << tsResume << "ms] Entry point released, game running\n";

    // ランチャー側での FastBoot 監視はここまで（残りはコアDLL側へ移譲）
    return true;
}

/**
 * @brief GameLauncherにおける外部公開インターフェース (MainControllerから呼ばれる)
 *        一連のシーケンス(サスペンド起動 -> インジェクト準備 -> DLL注入 -> 実行再開)を完遂します。
 */
bool GameLauncher::BootAndMonitor(const std::string &exePath) {
    const auto edition = InspectGameFile(exePath);
    std::cerr << "[GameBuild] " << cccaster::game_build::Name(edition) << "\n";
    if (!cccaster::game_build::SupportsRuntime(edition)) {
        std::cerr << (edition == cccaster::game_build::Edition::Steam20170105
            ? "[GameBuild] Steam executable recognized; runtime port is not ready. No game was started.\n"
            : "[GameBuild] Unsupported or modified executable. No game was started.\n");
        return false;
    }
    if (cccaster::diagnostics::startup::Enabled())
        std::cerr << "[Startup] event=launch qpcUs=" << cccaster::diagnostics::startup::QpcUs() << "\n";
    std::cout << "  [FastBoot] LaunchSuspended: " << exePath << std::endl;
    if (!LaunchSuspended(exePath)) {
        std::cout << "  [FastBoot] LaunchSuspended FAILED (CreateProcessA error=" << GetLastError() << ")"
                  << std::endl;
        return false;
    }
    std::cout << "  [FastBoot] LaunchSuspended OK (PID=" << _pi.dwProcessId << ")" << std::endl;

    if (!ApplyInitialPatches()) {
        TerminateProcess(_pi.hProcess, 1); // この呼出しが生成した停止中の子だけ終了。
        return false;
    }
    std::cout << "  [FastBoot] ApplyInitialPatches OK" << std::endl;

    bool reachedTarget = MonitorBootSequence();
    if (!reachedTarget) TerminateProcess(_pi.hProcess, 1); // この呼出しで生成した子だけ終了。
    std::cout << "  [FastBoot] MonitorBootSequence result=" << reachedTarget << std::endl;

    return reachedTarget;
}

} // namespace cccaster::main_app
