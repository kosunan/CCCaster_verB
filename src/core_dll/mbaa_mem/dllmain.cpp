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
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include "core_dll/common/LogSink.hpp"
#include "core_dll/common/DataPaths.hpp"
#include "core_dll/ui/ScoreBroadcast.hpp"
#include "cli_launcher/ConfigManager.hpp"
#include "core_dll/network/NetplayManager.hpp"
#include "core_dll/hook/TimeHooks.hpp"
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
#include "core_dll/mbaa_mem/StartupSystemInfo.hpp"
#include "core_dll/mbaa_mem/GameBuildGuard.hpp"

// DLL モジュールハンドル（DllMain で最初に設定）
// HookLog がモジュールパス基準でログファイルを開くために使用する。
static HMODULE g_hModule = nullptr;

// ============================================================================
// ApplyMultiInstanceBypass — MBAA 多重起動防止バイパス
//
// MBAA.exe は起動時に以下の2つのAPIで多重起動を検出する:
//   1. FindWindowA/W — 既存ウィンドウの検出
//   2. CreateMutexA  — 排他Mutexの存在チェック (ERROR_ALREADY_EXISTS)
//
// これらのAPI関数本体を直接パッチ（インラインフック）して無効化する。
// DllMain(DLL_PROCESS_ATTACH) から呼ばれるため、ゲーム本体の初期化より先に適用される。
// ============================================================================
static void PatchFunctionBytes(void *target, const BYTE *patch, size_t size) {
    DWORD oldProtect;
    VirtualProtect(target, size, PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy(target, patch, size);
    VirtualProtect(target, size, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), target, size);
}

static void ApplyMultiInstanceBypass() {
    // FindWindowA: xor eax, eax; ret 8 → 常に NULL を返す
    FARPROC pFW = GetProcAddress(GetModuleHandleA("user32.dll"), "FindWindowA");
    if (pFW) {
        BYTE patch[] = {0x31, 0xC0, 0xC2, 0x08, 0x00};
        PatchFunctionBytes((void *)pFW, patch, sizeof(patch));
    }

    // FindWindowW: xor eax, eax; ret 8 → 常に NULL を返す
    FARPROC pFWW = GetProcAddress(GetModuleHandleA("user32.dll"), "FindWindowW");
    if (pFWW) {
        BYTE patch[] = {0x31, 0xC0, 0xC2, 0x08, 0x00};
        PatchFunctionBytes((void *)pFWW, patch, sizeof(patch));
    }

    // CreateMutexA: SetLastError(0) + mov eax, 0x1337 + ret 12
    //   → ERROR_ALREADY_EXISTS を回避し、フェイクハンドルを返す
    FARPROC pCM = GetProcAddress(GetModuleHandleA("kernel32.dll"), "CreateMutexA");
    FARPROC pSLE = GetProcAddress(GetModuleHandleA("kernel32.dll"), "SetLastError");
    if (pCM && pSLE) {
        BYTE patch[16];
        patch[0] = 0x6A;
        patch[1] = 0x00;                                           // push 0
        patch[2] = 0xE8;                                           // call rel32
        INT32 rel = (INT32)((BYTE *)pSLE - ((BYTE *)pCM + 2 + 5)); // relative offset
        memcpy(&patch[3], &rel, 4);
        patch[7] = 0xB8; // mov eax, imm32
        patch[8] = 0x37;
        patch[9] = 0x13;
        patch[10] = 0x00;
        patch[11] = 0x00; // 0x1337
        patch[12] = 0xC2;
        patch[13] = 0x0C;
        patch[14] = 0x00; // ret 12
        PatchFunctionBytes((void *)pCM, patch, 15);
    }
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
        char dllPath[MAX_PATH] = {};
        if (g_hModule) {
            GetModuleFileNameA(g_hModule, dllPath, MAX_PATH);
            // ファイル名部分を切り落としてディレクトリパスを得る
            char *lastSlash = strrchr(dllPath, '\\');
            if (lastSlash)
                *(lastSlash + 1) = '\0';
        }
        // ログと設定ファイルで基準を分けると必ず食い違うので、同じ場所に寄せる
        cccaster::core::paths::SetDataRoot(dllPath);
        cccaster::core::log::SetLogPath(std::string(dllPath) + "cccaster_hook_log.txt");
        return true;
    }();
    (void)pathInitialized;

    cccaster::core::log::WriteLine(msg);
}

// ============================================================================
// InitThread — DLL初期化スレッド
//
// DllMain(DLL_PROCESS_ATTACH) から CreateThread で起動される
// DllMain 内ではブロッキング禁止のため、全初期化をここで行う
// ============================================================================
DWORD WINAPI InitThread(LPVOID lpParam) {
    (void)lpParam;

    // ログ書き出しスレッドはここで起動する。
    // DllMain の中で起こすとローダーロックを踏むため、必ず DllMain の外で。
    // これ以降、HookLog は積むだけで返る（I/O 完了を待たない）。
    cccaster::core::log::StartWriter();
    cccaster::diagnostics::startup::Mark("dll_init");

    HookLog("=====================================");
    HookLog("[InitThread] Starting hook initialization...");

    // ── 設定ファイルの読み込み ──────────────────────────────
    // ConfigManager はプロセスごとの共通Configへ委譲する。
    // ランチャー EXE が読んだ内容は、注入先のゲームプロセス（＝この DLL）には
    // 一切来ない。にもかかわらず DLL 側で Load を呼んでいなかったため、
    // GetString("Settings","P1Device") は常に "" を返し、
    // BuildPlayerInput() が joyId=-1 で無言の 0 を返し、
    // その 0 が毎フレームゲームメモリに書き込まれていた。
    // ＝**コントローラ設定が何であれ入力が一切効かない**状態だった。
    {
        const std::string iniPath = cccaster::core::paths::Resolve("cccaster_v10.ini");
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

    if (cccaster::public_api::IpcManager::OpenAndRead(state)) {
        HookLog("[InitThread] IPC Shared Memory Read SUCCESS.");

        // 起動モード
        ctx.appMode = static_cast<uint8_t>(state.targetGameMode);
        // 0=Versus, 1=Training, 2=Spectator

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
        HookLog("[InitThread] IPC Shared Memory Read FAILED. Using defaults.");
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
    cccaster::game_memory::MbaaPatcher::ApplyStartupPatches();

    // ================================================================
    // (3) Time API フック初期化
    // ================================================================
    HookLog("[InitThread] Initializing TimeHooks...");
    cccaster::core::hooks::TimeHooks::Initialize();
    if (!cccaster::core::hooks::TimeHooks::s_initialized) {
        HookLog("[InitThread] FAILED game timing imports unavailable");
        ExitProcess(1);
    }
    // MBAA本体のインポートだけを変更。DLL・音声側のSleep/時計は通常のまま。
    cccaster::core::hooks::TimeHooks::SetTimeMultiplier(1000);
    cccaster::core::hooks::TimeHooks::SetSleepBypass(true);

    // ================================================================
    // (4) ネットプレイ通信初期化（UDPソケット生成・受信開始）
    // ================================================================
    bool isNetplay = (ctx.appMode == 0); // Versus = netplay
    std::string peerIpStr(ctx.peerIp);

    HookLog("[InitThread] Initializing NetplayManager...");
    cccaster::netplay::NetplayManager::GetInstance().Initialize(isNetplay, ctx.isHost, ctx.localPort,
                                                                ctx.peerPort, peerIpStr);
    if (isNetplay) {
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
    if (cccaster::game_interface::DxHook::Initialize()) {
        HookLog("[InitThread] DxHook::Initialize() SUCCEEDED");
    } else {
        HookLog("[InitThread] DxHook::Initialize() FAILED");
        ExitProcess(1);
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
    cccaster::domain::session::SceneRunner::Init(ctx);

    // ================================================================
    // (7) DxHook コールバック登録
    //
    // ★ SceneRunner::Init() 完了後に登録する（R-01: 初期化順序）
    // ================================================================
    cccaster::domain::session::GameFrameOrchestrator::Register();
    HookLog("[InitThread] DxHook callbacks registered.");
    cccaster::public_api::IpcManager::UpdateOrReadState(
        [](cccaster::public_api::SharedState &s) { s.dllInitialized = true; });
    cccaster::diagnostics::startup::Mark("dll_ready");
    cccaster::game_memory::startup_system_info::Initialize(ctx.appMode);
    cccaster::game_memory::startup_assets::Initialize(ctx.appMode);
    cccaster::game_memory::startup_profile::Initialize();
    cccaster::diagnostics::startup::SignalReady();

    return 0;
}

// ============================================================================
// DllMain — Windowsが呼ぶDLLエントリーポイント
// ============================================================================
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    (void)lpReserved;

    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
        if (!cccaster::game_build::ValidateLoadedRuntime()) {
            OutputDebugStringA("[CCCaster] Unsupported or modified game image; no hooks applied.\n");
            return FALSE;
        }
        g_hModule = hModule; // ← ログパス解決のため最初に設定
        DisableThreadLibraryCalls(hModule);
        ApplyMultiInstanceBypass(); // ← 多重起動バイパス（ゲーム初期化より先に適用）
        HookLog("[DllMain] DLL_PROCESS_ATTACH (multi-instance bypass applied)");
        CreateThread(nullptr, 0, InitThread, hModule, 0, nullptr);
        break;

    case DLL_PROCESS_DETACH:
        if (!g_hModule) break; // PROCESS_ATTACH拒否時は未初期化の処理を呼ばない。
        HookLog("[DllMain] DLL_PROCESS_DETACH");
        // lpReserved が nullptr でない場合、プロセス終了(ExitProcess)によるデタッチであることを示す。
        // プロセス終了時は他スレッドがすでに停止しており、Shutdownで join 等を行うとデッドロックするためスキップする。
        if (lpReserved == nullptr) {
            cccaster::domain::session::GameFrameOrchestrator::Shutdown();
            cccaster::game_interface::DxHook::Shutdown();
            cccaster::netplay::NetplayManager::GetInstance().Shutdown();
            cccaster::core::hooks::TimeHooks::Shutdown();
        } else {
            HookLog("[DllMain] Process is terminating. Skipping Shutdown() to avoid deadlock.");
        }
        // ログは最後に閉じる（上の Shutdown 群のログを取りこぼさないため）。
        // プロセス終了時は他スレッドが排他を握ったまま消えている可能性がある
        // ので、ロック取得を諦める非ブロッキング版で呼ぶ。
        cccaster::core::log::Shutdown(/*blocking=*/lpReserved == nullptr);
        break;

    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
        break;
    }
    return TRUE;
}
