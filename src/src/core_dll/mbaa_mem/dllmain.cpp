#include "core_dll/mbaa_mem/StartupSounds.hpp"
#include "core_dll/mbaa_mem/StartupNativeInput.hpp"
#include "core_dll/hook/DirectInputHook.hpp"
#include "shared_contracts/ConfigPath.hpp"
#include "ProductVersion.hpp"
#include "BuildIdentity.hpp"
#include "shared_contracts/BootDiagnostics.hpp"
#include "shared_contracts/ProcessMemory.hpp"
#include "shared_contracts/NetplaySettings.hpp"
// ============================================================================
// dllmain.cpp — DLLエントリーポイント（最小構成）
//
// 責務:
//   1. DllMain: DLL_PROCESS_ATTACH/DETACH のOS橋渡し
//   2. InitThread: IPC読み取り → MatchContext構築 → フック初期化 → SceneRunner起動
//
// ★ モード分岐は行わない — MatchContext.appMode を持ち回り、
//   SceneRunner が画面状態を読み取りながらハンドリングする
// ============================================================================

#undef _mm_getcsr
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include "core_dll/common/LogSink.hpp"
#include "core_dll/common/DataPaths.hpp"
#include "core_dll/ui/ScoreBroadcast.hpp"
#include "cli_launcher/ConfigManager.hpp"
#include "core_dll/network/NetplayManager.hpp"
#include "core_dll/mbaa_mem/NativeFrameWait.hpp"
#include "core_dll/mbaa_mem/MbaaPatcher.hpp"
#include "core_dll/mbaa_mem/RealGameMemory.hpp"
#include "core_dll/hook/DxHook.hpp"
#include "core_dll/engine/SceneRunner.hpp"
#include "core_dll/engine/MatchContext.hpp"
#include "core_dll/engine/GameFrameOrchestrator.hpp"
#include "shared_contracts/IpcData.hpp"
#include "shared_contracts/PlayerName.hpp"
#include "core_dll/common/StartupTrace.hpp"
#include "core_dll/mbaa_mem/StartupPatch.hpp"
#include "core_dll/mbaa_mem/StartupProfile.hpp"
#include "core_dll/mbaa_mem/StartupAssets.hpp"
#include "core_dll/mbaa_mem/StartupFileRead.hpp"
#include "core_dll/mbaa_mem/StartupDirectEntry.hpp"
#include "core_dll/mbaa_mem/StartupSystemInfo.hpp"
#include "core_dll/mbaa_mem/GameBuildGuard.hpp"

// DLL モジュールハンドル（DllMain で最初に設定）
// HookLog がモジュールパス基準でログファイルを開くために使用する。
static HMODULE g_hModule = nullptr;

// ============================================================================
// ApplyMultiInstanceBypass — ゲームの多重起動判定呼出しだけを成功へ置換。
// Windows API本体を変更すると、D3Dや外部DLLまで偽のMutexを受け取る。
// ValidateLoadedRuntime成功後、まだ停止中のゲーム入口でのみ適用する。
// ============================================================================
static cccaster::patch::Result ApplyMultiInstanceBypass() {
    constexpr uint8_t caller[] = {0xE8,0x18,0x02,0,0,0x85,0xC0,0x75,0x06,
        0x8B,0xE5,0x5D,0xC2,0x10,0};
    // CreateMutexA/GetLastError/FindWindowA/ReleaseMutexだけの判定関数。
    // 従来版・コミュニティ版の両実体で、この80バイトと呼出元を照合済み。
    constexpr uint8_t check[] = {
        0x56,0x68,0xC4,0x51,0x53,0,0x6A,1,0x6A,0,0xFF,0x15,0x38,0xB0,0x51,0,
        0x8B,0xF0,0xFF,0x15,0x30,0xB0,0x51,0,0x3D,0xB7,0,0,0,0x75,0x23,0x6A,0,
        0x68,0xC4,0x51,0x53,0,0xFF,0x15,0xC4,0xB2,0x51,0,0x85,0xC0,0x74,0x0E,
        0x50,0xFF,0x15,0x30,0xB2,0x51,0,0x50,0xFF,0x15,0x98,0xB2,0x51,0,
        0x33,0xC0,0x5E,0xC3,0x56,0xFF,0x15,0x34,0xB0,0x51,0,0xB8,1,0,0,0,0x5E,0xC3};
    constexpr uintptr_t base = 0x400000, site = 0x40D253, target = 0x40D470;
    if (reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) != base ||
        !cccaster::game_build::ReadableLoadedRange(base, site, sizeof(caller)) ||
        !cccaster::game_build::ReadableLoadedRange(base, target, sizeof(check)) ||
        std::memcmp(reinterpret_cast<void *>(site), caller, sizeof(caller)) ||
        std::memcmp(reinterpret_cast<void *>(target), check, sizeof(check)))
        return {cccaster::patch::Error::Mismatch, 0, site, "multi_instance"};
    // call判定関数（引数なし）→mov eax,1。続くtest/jneは元のまま。
    constexpr uint8_t patch[] = {0xB8,1,0,0,0};
    const cccaster::patch::Spec spec{"multi_instance", site, caller, patch};
    return cccaster::patch::Apply(std::span(&spec, 1));
}

// ============================================================================
// HookLog — DLL 全体のログ入口
//
// 実体は core_dll/common/LogSink（開きっぱなし + 排他 + バッファリング）。
// ここは「Win32 でしか解けない出力先パス」を LogSink に渡す薄い橋渡しだけを
// 持つ。1 行ごとの fopen/fclose はやめたので、毎フレーム呼ばれても
// ゲームスレッドは I/O の完了を待たない。
//
// 呼び出し側のシグネチャは従来どおり。DebugLog(...) 経由の既存呼び出しは
// 一切書き換えていない。
// ============================================================================
void HookLog(const char *msg) {
    // DLL 自身のパスを基準にログファイルパスを構築する。
    // カレントディレクトリに依存せず、常に DLL と同じフォルダ
    // （_TEST_MBAACC\cccaster_B\）に cccaster_hook_log.txt を出力する。
    // 解決は初回 1 回だけ（従来は毎行 GetModuleFileNameA を呼んでいた）。
    static const bool pathInitialized = [] {
        wchar_t dllPath[32768]{};
        const DWORD length = GetModuleFileNameW(g_hModule, dllPath, std::size(dllPath));
        if (!length || length >= std::size(dllPath)) throw std::runtime_error("DLL path unavailable");
        const auto root = std::filesystem::path(dllPath).parent_path().u8string();
        const std::string utf8Root(reinterpret_cast<const char *>(root.c_str()), root.size());
        cccaster::core::paths::SetDataRoot(utf8Root);
        cccaster::core::log::SetLogPath(utf8Root + "/cccaster_hook_log.txt");
        return true;
    }();
    (void)pathInitialized;

    cccaster::core::log::WriteLine(msg);
}

// ============================================================================
// InitThread — DLL初期化スレッド
//
// ランチャーがロード完了後、CCCasterInitializeを明示的に呼ぶ。
// ゲーム入口は初期化の成功確認まで停止したまま。
// ============================================================================
namespace boot = cccaster::boot;
static void Stage(boot::Status &report, boot::Stage stage) {
    InterlockedExchange(reinterpret_cast<volatile LONG *>(&report.stage), LONG(stage));
}
static DWORD Fail(boot::Status &report, boot::Error error, DWORD systemError = 0) {
    report.win32 = systemError;
    report.error = error;
    return 0;
}
static DWORD PatchFailed(boot::Status &report, const cccaster::patch::Result &result) {
    report.address = uint32_t(result.address);
    report.patchError = uint32_t(result.error);
    report.rollbackFailed = result.rollbackFailed;
    std::snprintf(report.patchName, sizeof(report.patchName), "%s", result.name);
    return Fail(report, boot::Error::Patch, result.systemError);
}
static DWORD InitializeCore(boot::Status &report) {

    // ログ書き出しスレッドはここで起動する。
    // DllMain の中で起こすとローダーロックを踏むため、必ず DllMain の外で。
    // これ以降、HookLog は積むだけで返る（I/O 完了を待たない）。
    cccaster::core::log::StartWriter();
    cccaster::diagnostics::startup::Mark("dll_init");

    HookLog("=====================================");
    HookLog("[InitThread] Starting hook initialization...");
    HookLog("[Release] " CCCASTER_PRODUCT_TITLE "");

    // ── 設定ファイルの読み込み ──────────────────────────────
    // ConfigManager はプロセスごとの共通Configへ委譲する。
    // ランチャー EXE が読んだ内容は、注入先のゲームプロセス（＝この DLL）には
    // 一切来ない。にもかかわらず DLL 側で Load を呼んでいなかったため、
    // GetString("Settings","P1Device") は常に "" を返し、
    // BuildPlayerInput() が joyId=-1 で無言の 0 を返し、
    // その 0 が毎フレームゲームメモリに書き込まれていた。
    // ＝**コントローラ設定が何であれ入力が一切効かない**状態だった。
    {
        const std::string iniPath = cccaster::ConfigPath(cccaster::core::paths::GetDataRoot()).string();
        cccaster::main_app::ConfigManager::Load(iniPath);
        HookLog(("[InitThread] Config loaded: " + iniPath).c_str());
    }

    // 新ランチャーではentry解放前に準備を完了する。旧ランチャーは従来待機を維持。
    if ((!cccaster::diagnostics::startup::HasGate() && !std::getenv("CCCASTER_STARTUP_NO_WAIT")) ||
        cccaster::diagnostics::startup::Baseline() ||
        !cccaster::game_memory::startup::MatchesMenuCode())
        Sleep(100);
    cccaster::diagnostics::startup::Mark("init_wait_end");

    // ================================================================
    // (1) IPC 共有メモリ読み取り → MatchContext に集約
    // ================================================================
    // ★ static: InitThread 終了後も Step() からアクセスするため永続化が必要
    static cccaster::domain::session::MatchContext ctx;
    cccaster::public_api::SharedState state;

    Stage(report, boot::Stage::Ipc);
    if (cccaster::public_api::IpcManager::OpenAndRead(state)) {
        if (state.targetGameMode > uint32_t(cccaster::public_api::IpcGameMode::LocalVersus) ||
            !cccaster::public_api::NetplaySettings::IsValid(state.delayFrames, state.maxRollbackFrames) ||
            !std::memchr(state.peerIp, 0, sizeof(state.peerIp)) ||
            !std::memchr(state.targetIp, 0, sizeof(state.targetIp)))
            return Fail(report, boot::Error::Settings);
        HookLog("[InitThread] IPC Shared Memory Read SUCCESS.");

        // 起動モード
        ctx.appMode = static_cast<uint8_t>(state.targetGameMode);
        // IpcGameMode: 0=通信対戦、5=同一PCのオフライン対戦。

        // ネットワーク情報
        ctx.isHost = state.isHost;
        cccaster::public_api::NormalizePlayerName(ctx.playerName, state.playerName,
                                                   ctx.isHost ? "PLAYER 1" : "PLAYER 2");

        // 同期パラメータ（IPC値を無条件反映）
        ctx.delay = static_cast<int16_t>(state.delayFrames);
        ctx.maxRollback = static_cast<int16_t>(state.maxRollbackFrames);

        // ネットワーク接続先情報（SceneRunner/NetplayManager が使用）
        ctx.peerPort = (state.peerPort != 0) ? state.peerPort : state.port;
        // HOST/CLIENT共にネゴシエーション時と同じポートを再利用。
        // HOST: 待機に使用した固定ポート (例: 7500)
        // CLIENT: ネゴ時にOS割当されたエフェメラルポート
        // これにより相手の peerPort と一致し、返信パケットが正しく到達する。
        ctx.localPort = state.localPort;

        // peerIp → ctx に保存（固定長配列なのでコピー）
        const char *ip = (state.peerIp[0] != '\0') ? state.peerIp : state.targetIp;
        strncpy(ctx.peerIp, ip, sizeof(ctx.peerIp) - 1);
        ctx.peerIp[sizeof(ctx.peerIp) - 1] = '\0';

        // ── 同一PC検出: ポート衝突を自動回避 ──
        // peerIp がローカルアドレスの場合:
        //   CLIENT: localPort を peerPort+1 にシフト (自バインド衝突回避)
        //   HOST:   peerPort を peerPort+1 にシフト (CLIENT の新ポートへ送信)
        {
            std::string peerStr(ctx.peerIp);
            bool isLocalPeer = (peerStr == "127.0.0.1" || peerStr == "::1" || peerStr == "localhost" ||
                                peerStr.rfind("192.168.", 0) == 0 || peerStr.rfind("10.", 0) == 0);
            if (isLocalPeer && ctx.localPort == ctx.peerPort) {
                if (!ctx.isHost) {
                    // CLIENT: 自分を +1 にずらす
                    ctx.localPort = ctx.peerPort + 1;
                    char shiftLog[128];
                    snprintf(shiftLog, sizeof(shiftLog), "[InitThread] Same-PC: CLIENT localPort %u -> %u",
                             ctx.peerPort, ctx.localPort);
                    HookLog(shiftLog);
                } else {
                    // HOST: 相手(CLIENT)が +1 にずれるので送信先もずらす
                    ctx.peerPort = ctx.peerPort + 1;
                    char shiftLog[128];
                    snprintf(shiftLog, sizeof(shiftLog), "[InitThread] Same-PC: HOST peerPort %u -> %u",
                             ctx.localPort, ctx.peerPort);
                    HookLog(shiftLog);
                }
            }
        }

        char log[256];
        snprintf(log, sizeof(log),
                 "[InitThread] Config -> mode=%u host=%d delay=%d maxRB=%d peer=%s:%u local=%u", ctx.appMode,
                 ctx.isHost, ctx.delay, ctx.maxRollback, ctx.peerIp, ctx.peerPort, ctx.localPort);
        HookLog(log);
    } else {
        return Fail(report, boot::Error::Ipc);
    }

    // ================================================================
    // (2) ゲームメモリ実装の設置
    //   これ以降 GameMem() が実アドレスを読み書きする。
    //   未設置のままだと全読み取りが 0 を返すため、他の初期化より先に行う。
    // ================================================================
    cccaster::game_interface::InstallRealGameMemory();
    HookLog("[InitThread] RealGameMemory installed.");

    // ================================================================
    // (3) MBAA 固有パッチ適用（NOP/キーボードクリア/非アクティブ判定無効化）
    // ================================================================
    HookLog("[InitThread] Applying MBAA startup patches...");
    Stage(report, boot::Stage::Patches);
    const auto instancePatch = ApplyMultiInstanceBypass();
    if (!instancePatch) return PatchFailed(report, instancePatch);
    const auto patches = cccaster::game_memory::MbaaPatcher::ApplyStartupPatches(ctx.appMode == 1);
    if (!patches) return PatchFailed(report, patches);

    // ================================================================
    // (3) 本体のフレーム待機を直接バイパス。時計APIと他用途のSleepは実時間のまま。
    // ================================================================
    HookLog("[InitThread] Configuring native frame wait bypass...");
    Stage(report, boot::Stage::Clock);
    // 起動比較時だけ本体の待機を残し、キャラ選択到達後にゲームスレッドで適用する。
    const bool accelerateStartup = !cccaster::diagnostics::startup::Baseline();
    if (accelerateStartup) {
        const auto frameWait = cccaster::game_memory::native_frame_wait::Enable();
        if (!frameWait) return PatchFailed(report, frameWait);
    }
    HookLog(accelerateStartup ? "[StartupPolicy] acceleration=on" :
        "[StartupPolicy] acceleration=off gameClock=1 nativeFrameWait=1 originalAssets=1");

    // ================================================================
    // (4) ネットプレイ通信初期化（UDPソケット生成・受信開始）
    // ================================================================
    bool isNetplay = (ctx.appMode == 0); // Versus = netplay
    std::string peerIpStr(ctx.peerIp);

    HookLog("[InitThread] Initializing NetplayManager...");
    Stage(report, boot::Stage::Network);
    cccaster::netplay::NetplayManager::GetInstance().Initialize(isNetplay, ctx.isHost, ctx.localPort,
                                                                ctx.peerPort, peerIpStr);
    if (isNetplay) {
        if (!cccaster::netplay::NetplayManager::GetInstance().GetUdpSocket())
            return Fail(report, boot::Error::Network);
        cccaster::public_api::IpcManager::UpdateOrReadState([](cccaster::public_api::SharedState &s) {
            auto &net = cccaster::netplay::NetplayManager::GetInstance();
            if (auto *socket = net.GetUdpSocket()) s.localPort = socket->GetPort();
            s.peerPort = net.GetTargetPort();
            std::snprintf(s.peerIp, sizeof(s.peerIp), "%s", net.GetTargetIp().c_str());
        });
    }

    // ================================================================
    // (5) DxHook 初期化
    // ================================================================
    HookLog("[InitThread] Initializing DxHook...");
    cccaster::diagnostics::startup::Mark("dx_begin");
    Stage(report, boot::Stage::Graphics);
    if (cccaster::game_interface::DxHook::Initialize()) {
        HookLog("[InitThread] DxHook::Initialize() SUCCEEDED");
    } else {
        HookLog("[InitThread] DxHook::Initialize() FAILED");
        return Fail(report, boot::Error::Graphics);
    }

    // ================================================================
    // (6) SceneRunner 初期化
    //
    // ★ Init() は状態変数を初期化するだけで即リターン。
    //   実際のフレーム処理 (Step()) は GameFrameOrchestrator の Present コールバックから
    //   ゲームスレッド上で呼ばれる。
    // ================================================================
    HookLog("[InitThread] Initializing SceneRunner...");
    cccaster::diagnostics::startup::Mark("dx_end");
    if (ctx.appMode == 0) {
        const bool broadcast = cccaster::domain::ui::score_broadcast::Initialize(
            cccaster::core::paths::Resolve("broadcast"));
        HookLog(broadcast ? "[Broadcast] score output initialized" : "[Broadcast] score output unavailable");
    }
    Stage(report, boot::Stage::Scene);
    cccaster::domain::session::SceneRunner::Init(ctx);

    // ================================================================
    // (7) DxHook コールバック登録
    //
    // ★ SceneRunner::Init() 完了後に登録する（R-01: 初期化順序）
    // ================================================================
    cccaster::domain::session::GameFrameOrchestrator::Register();
    HookLog("[InitThread] DxHook callbacks registered.");
    Stage(report, boot::Stage::Assets);
    // 観戦もキャラ選択まではVersusと同じ起動経路。アプリの観戦mode=2をそのまま
    // 渡すと既存最適化の対象外になり、システム情報収集・素材変換を毎回待ってしまう。
    // 観戦・標準REPも同じ起動資産を使う。一覧到達時に元の読込み経路へ復元する。
    const uint8_t startupMode = ctx.appMode == 2 || ctx.appMode == 3 || ctx.appMode == 5 ||
        (ctx.appMode == 4 && cccaster::game_memory::startup_direct_entry::ReplayEnabled())
        ? uint8_t(0) : ctx.appMode;
    cccaster::game_memory::startup_system_info::Initialize(startupMode);
    cccaster::game_memory::startup_assets::Initialize(startupMode);
    cccaster::game_memory::startup_file_read::Initialize(startupMode);
    cccaster::game_memory::startup_direct_entry::Initialize(ctx.appMode, ctx.isHost);
    cccaster::game_memory::startup_sounds::Initialize(startupMode);
    cccaster::game_memory::startup_native_input::Initialize();
    if (!std::getenv("CCCASTER_STARTUP_MINIMAL_BASELINE") && !cccaster::diagnostics::startup::Baseline())
        cccaster::game_interface::DirectInputHook::BeginInitialize();
    cccaster::game_memory::startup_profile::Initialize();
    if (!cccaster::public_api::IpcManager::UpdateOrReadState(
            [](cccaster::public_api::SharedState &s) { s.dllInitialized = true; }))
        return Fail(report, boot::Error::Ipc);
    if (!cccaster::diagnostics::startup::SignalReady())
        return Fail(report, boot::Error::Windows, GetLastError());
    cccaster::diagnostics::startup::Mark("dll_ready");
    HookLog("[InitThread] Initialization complete; startup gate signaled.");
    Stage(report, boot::Stage::Ready);
    return 1;
}

extern "C" __declspec(dllexport) const boot::Descriptor CCCasterStartupInfo = {
    boot::Magic, boot::Abi, sizeof(boot::Descriptor), CCCASTER_BUILD_ID};
static LONG initializationStarted = 0;
static bool loggingStarted = false;

// このDLLはゲームの生存期間に固定する。実行中フックを残すFreeLibraryは許可しない。
extern "C" __declspec(dllexport) DWORD WINAPI CCCasterInitialize(LPVOID parameter) {
    const HANDLE mapping = static_cast<HANDLE>(parameter);
    auto *report = static_cast<boot::Status *>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(boot::Status)));
    CloseHandle(mapping);
    if (!report) return 0;
    DWORD result = 0;
    if (report->magic != boot::Magic || report->abi != boot::Abi || report->size != sizeof(*report) ||
        report->processId != GetCurrentProcessId() ||
        std::memcmp(report->expectedBuild, CCCASTER_BUILD_ID, sizeof(report->expectedBuild))) {
        Fail(*report, boot::Error::Contract);
    } else if (InterlockedCompareExchange(&initializationStarted, 1, 0) != 0) {
        Fail(*report, boot::Error::Contract);
    } else {
        std::memcpy(report->dllBuild, CCCASTER_BUILD_ID, sizeof(report->dllBuild));
        Stage(*report, boot::Stage::RuntimeValidation);
        const auto compatible = cccaster::game_build::ValidateLoadedRuntime();
        if (!compatible) {
            report->address = compatible.address;
            std::snprintf(report->patchName,sizeof(report->patchName),"%s",compatible.name);
            Fail(*report, boot::Error::GameMismatch);
        } else {
            HMODULE pinned = nullptr;
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                    reinterpret_cast<LPCWSTR>(&CCCasterInitialize), &pinned)) {
                Fail(*report, boot::Error::Windows, GetLastError());
            } else {
                try { loggingStarted = true; result = InitializeCore(*report); }
                catch (...) { Fail(*report, boot::Error::Exception); }
            }
        }
    }
    UnmapViewOfFile(report);
    return result;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hModule = module;
        DisableThreadLibraryCalls(module);
    } else if (reason == DLL_PROCESS_DETACH && reserved && loggingStarted) {
        // 他スレッドは停止済み。待機・フック解除・スレッドjoinをloader lock内でしない。
        cccaster::core::log::Shutdown(false);
    }
    return TRUE;
}
