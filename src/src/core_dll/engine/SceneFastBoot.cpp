// ============================================================================
// SceneFastBoot.cpp — ゲームスレッド上の高速起動の実装
//
// 【旧 FastBootRunner からの移植】
//   - 裏スレッドの 1ms ポーリング → ゲームスレッドの毎フレーム呼び出し
//   - CC_SKIP_FRAMES 直書き → SpeedFlags::HighSpeedSkip
//   - MemoryPatcher 直書き → GC::WriteInput() + 直接メモリ操作
// ============================================================================

#include "core_dll/engine/SceneFastBoot.hpp"
#include "core_dll/engine/FrameControl.hpp"
#include "core_dll/mbaa_mem/MbaaAddresses.hpp"
#include "core_dll/mbaa_mem/IGameMemory.hpp"
#include "core_dll/mbaa_mem/MbaaInputDefs.hpp"
#include "core_dll/common/DebugLog.hpp"
#include "core_dll/common/StartupTrace.hpp"
#include "core_dll/mbaa_mem/StartupPatch.hpp"
#include "core_dll/mbaa_mem/StartupSystemInfo.hpp"
#include "core_dll/mbaa_mem/StartupAssets.hpp"
#include "core_dll/hook/TimeHooks.hpp"

#include <cstring>

namespace cccaster::domain::scene {

using GC = session::FrameControl;
using cccaster::game_interface::GameInput;
using session::DebugLog;
namespace Dir = cccaster::game_interface::Dir;

// ================================================================
// Static 変数
// ================================================================

/// FastBoot の目標ゲームモード
static cccaster::public_api::IpcGameMode s_targetMode = cccaster::public_api::IpcGameMode::Versus;

/// FastBoot 完了フラグ
static bool s_complete = false;

/// ダイレクトジャンプ命令適用済み
static bool s_forceGotoApplied = false;
static bool s_forceGotoAttempted = false;

/// メインメニューのナビゲーション進捗
static int s_menuNavCount = 0;

/// 入力トグル（1F おきに押す / 離す）
static bool s_toggle = false;

/// フレームカウンタ（デバッグログ用）
static uint32_t s_frameCount = 0;

// ================================================================
// Start — 初期化
// ================================================================
void SceneFastBoot::Start(cccaster::public_api::IpcGameMode targetMode) {
    s_targetMode = targetMode;
    s_complete = false;
    s_forceGotoApplied = false;
    s_forceGotoAttempted = false;
    s_menuNavCount = 0;
    s_toggle = false;
    s_frameCount = 0;
    DebugLog("[FastBoot] Started (targetMode=%d)", static_cast<int>(targetMode));
}

// ================================================================
// Reset
// ================================================================
void SceneFastBoot::Reset() {
    s_complete = false;
    s_forceGotoApplied = false;
    s_forceGotoAttempted = false;
    s_menuNavCount = 0;
    s_toggle = false;
    s_frameCount = 0;
}

// ================================================================
// IsComplete
// ================================================================
bool SceneFastBoot::IsComplete() {
    return s_complete;
}

// ================================================================
// ProcessFrame — 毎フレーム呼ばれるメインロジック
//
//   phase < CharaSelect のときに SceneRunner::Step() から呼ばれる。
//   戻り値: true = キャラセレ到達（FastBoot 完了）
// ================================================================
bool SceneFastBoot::ProcessFrame(bool isHost) {
    cccaster::game_memory::startup_system_info::Restore();
    cccaster::game_memory::startup_assets::Restore();
    if (s_complete)
        return true;

    s_frameCount++;

    // ゲームモード読取り
    uint32_t gameMode = cccaster::game_interface::GameMem().GameMode();
    if (cccaster::diagnostics::startup::Enabled()) {
        static uint32_t previousMode = UINT32_MAX;
        if (previousMode != gameMode) {
            previousMode = gameMode;
            DebugLog("[Startup] event=mode_%u qpcUs=%lld frame=%u", gameMode,
                cccaster::diagnostics::startup::QpcUs(), s_frameCount);
        }
    }

    // キャラセレ到達判定
    const bool replay = s_targetMode == cccaster::public_api::IpcGameMode::Replay;
    if (gameMode == CC_GAME_MODE_CHARA_SELECT || (replay && gameMode == CC_GAME_MODE_REPLAY)) {
        if (replay && !cccaster::game_memory::startup::SetReplayEntry(false)) ExitProcess(ERROR_WRITE_FAULT);
        cccaster::game_memory::startup_assets::Restore(true);
        if (!cccaster::game_memory::startup::SetBootFade(false))
            ExitProcess(ERROR_WRITE_FAULT);
        if (cccaster::diagnostics::startup::Enabled())
            DebugLog("[StartupFade] restored=1");
        s_complete = true;
        cccaster::diagnostics::startup::Mark("chara_detect");
        if (cccaster::diagnostics::startup::Enabled()) {
            DebugLog("[StartupMode] target=%u mode=%u kind=%u versus=%u frames=%u",
                static_cast<unsigned>(s_targetMode), gameMode,
                *reinterpret_cast<const uint32_t*>(0x562A74),
                *reinterpret_cast<const uint32_t*>(0x77BF2C), s_frameCount);
            const auto *keys = reinterpret_cast<const uint8_t*>(0x54D2C0);
            unsigned nonzero = 0;
            for (unsigned i=0; i<20; ++i) nonzero += keys[i] != 0;
            DebugLog("[StartupKeys] nonzero=%u", nonzero);
        }
        // 起動中だけ元のゲーム時計を使う。以後の60Hz待機・ロールバックは既存経路へ戻す。
        cccaster::core::hooks::TimeHooks::SetTimeMultiplier(1000);
        cccaster::core::hooks::TimeHooks::SetSleepBypass(true);
        DebugLog("[StartupPolicy] character selection reached; runtime pacing active");
        if (replay) DebugLog("[FastBoot] Replay reached (frame=%u).", s_frameCount);
        else DebugLog("[FastBoot] ★ CharaSelect reached! (frame=%u) Switching to NormalSpeed.", s_frameCount);
        GC::SetModeNormalSpeed();
        return true;
    }

    // 無効なゲームモード（起動初期）
    if (gameMode == 65535 || gameMode == 0) {
        return false;
    }

    // 30F ごとに進行ログ出力
    if (s_frameCount % 30 == 0) {
        DebugLog("[FastBoot] frame=%u gameMode=%u", s_frameCount, gameMode);
    }

    // ================================================================
    // イントロスキップ: gameState == 1 or 99 → 101 に書き換え
    // ================================================================
    uint32_t gameState = *CC_GAME_STATE_ADDR;
    if (!cccaster::diagnostics::startup::Baseline() &&
        (gameState == CC_GAME_STATE_CHARA_INTRO || gameState == CC_GAME_STATE_INTRO_DONE)) {
        *CC_GAME_STATE_ADDR = CC_GAME_STATE_INTRO_SKIP;
    }

    // SFX バッファゼロクリア（起動中の不快な SE 連打防止）
    std::memset(CC_SFX_ARRAY_ADDR, 0, CC_SFX_ARRAY_LEN);

    // ================================================================
    // メインメニュー (gameMode == 25):
    //   方向キー下 + 決定ボタンを交互に偽造してメニューを遷移
    // ================================================================
    if (gameMode == 25) {
        // ナビ回数: Versus=1, Training=5
        int targetNav = replay ? 7 : (s_targetMode == cccaster::public_api::IpcGameMode::Training) ? 5 : 1;
        if (replay && s_forceGotoApplied) targetNav = 0;
        if (s_forceGotoApplied && !cccaster::diagnostics::startup::Baseline() &&
            (s_targetMode == cccaster::public_api::IpcGameMode::Training ||
             s_targetMode == cccaster::public_api::IpcGameMode::Versus)) targetNav = 0;

        GameInput input{};

        if (s_menuNavCount < targetNav) {
            // 方向キー下を交互に入力
            if (s_toggle) {
                input.direction = Dir::Down;
            }
            if (!s_toggle) {
                s_menuNavCount++;
            }
        } else {
            // 決定ボタンを交互に入力
            if (s_toggle) {
                input.buttons = CC_BUTTON_CONFIRM;
            }
        }

        if (isHost) {
            GC::WriteInput(input, {});
        } else {
            GC::WriteInput({}, input);
        }

        if (s_toggle && s_frameCount % 10 == 0) {
            DebugLog("[FastBoot] MainMenu nav=%d/%d input=0x%08X", s_menuNavCount, targetNav, input.Pack());
        }

        s_toggle = !s_toggle;
        return false;
    }

    // ================================================================
    // 遷移画面 (gameMode == 2 or 3):
    //   ダイレクトジャンプ命令を書き込み
    // ================================================================
    // ランチャーで選んだモードへ入るための分岐は維持する。
    // 速度・描画・暗転の短縮とは別で、通常入力だけでは別項目へ遷移し得る。
    if (!s_forceGotoAttempted && (gameMode == 2 || gameMode == 3)) {
        s_forceGotoAttempted = true;
        uint8_t forcePatch[2] = {0xEB, 0x00};

        switch (s_targetMode) {
        case cccaster::public_api::IpcGameMode::Training:
            forcePatch[1] = 0x22;
            break;
        case cccaster::public_api::IpcGameMode::VersusCPU:
            forcePatch[1] = 0x5C;
            break;
        case cccaster::public_api::IpcGameMode::Versus:
            forcePatch[1] = 0x3F;
            break;
        case cccaster::public_api::IpcGameMode::Replay:
            forcePatch[1] = 0x22;
            break;
        default:
            forcePatch[1] = 0x3F;
            break;
        }

        // 照合に失敗した命令は上書きしない。成功した場合だけ方向入力を省略する。
        s_forceGotoApplied = replay ? cccaster::game_memory::startup::SetReplayEntry(true)
                                   : cccaster::game_memory::startup::Apply(forcePatch[1]);
        if (s_forceGotoApplied && !cccaster::diagnostics::startup::Baseline() &&
            (s_targetMode == cccaster::public_api::IpcGameMode::Training ||
             s_targetMode == cccaster::public_api::IpcGameMode::Versus)) {
            const bool fade = cccaster::game_memory::startup::SetBootFade(true);
            DebugLog("[StartupFade] applied=%u", fade ? 1 : 0);
        }
        DebugLog("[FastBoot] ForceGoto patch %s at gameMode=%u",
            s_forceGotoApplied ? "applied" : "rejected", gameMode);
        if (s_forceGotoApplied) return false;
    }

    // ================================================================
    // その他の画面 (タイトル画面等):
    //   決定ボタンを偽造して進める
    // ================================================================
    {
        GameInput input{};
        if (s_toggle)
            input.buttons = CC_BUTTON_CONFIRM;

        if (isHost) {
            GC::WriteInput(input, {});
        } else {
            GC::WriteInput({}, input);
        }

        s_toggle = !s_toggle;
    }

    return false;
}

} // namespace cccaster::domain::scene
