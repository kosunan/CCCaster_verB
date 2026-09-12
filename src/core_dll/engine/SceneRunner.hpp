#pragma once
// ============================================================================
// SceneRunner — ゲームスレッド統合型フレームディスパッチャ
//
// 【設計】
//   Init()  : InitThread から1回だけ呼ばれ、状態変数を初期化する。
//   Step()  : ゲームスレッドから毎フレーム呼ばれ、1フレーム分の処理を実行する。
//             呼び出し元は GameFrameOrchestrator::Register() を参照。
//   IsReady(): Init() 完了後に true を返す。Present から Step() を呼ぶ前に確認。
//
// 【旧設計との違い】
//   旧: Run() が CreateThread で別スレッドの while ループとして動作
//        → ゲームメモリの読み書きがスレッド競合
//   新: Step() がゲームスレッドの Present コールバック内で動作
//        → ゲームと同一スレッドで安全にメモリアクセス
// ============================================================================

#include "core_dll/engine/MatchContext.hpp"
#include "core_dll/engine/SessionScore.hpp"
#include "core_dll/mbaa_mem/TrainingMetrics.hpp"
#include "core_dll/engine/TrainingState.hpp"
#include <cstdint>
#include <array>

namespace cccaster::domain::session {

class SceneRunner {
  public:
    /// @brief 初期化（InitThread から1回だけ呼ぶ）
    static void Init(MatchContext &ctx);

    /// @brief 1フレーム分の処理。未着入力には期限付きで待機する。
    static void Step();
    static void FlushCadence();

    /// @brief Init() 完了済みか
    static bool IsReady();
    static bool IsReplaying();
    /// IPCで選ばれた用途。準備前は無効値。ゲーム内画面番号とは区別する。
    static uint8_t AppMode();
    static SessionScoreSnapshot Score(); // ゲームスレッドの描画時に参照
    struct PlayerNamesSnapshot {
        std::array<char, cccaster::public_api::PlayerNameSize> p1{};
        std::array<char, cccaster::public_api::PlayerNameSize> p2{};
    };
    static PlayerNamesSnapshot PlayerNames();
    static FrameAdvantageResult FrameAdvantage();
    static TrainingStateEvent TrainingStateNotice();
    static bool HasTrainingState();
    struct SpectatorInfo { uint32_t frame, latest, viewers, state; bool catching; };
    static SpectatorInfo SpectatorStatus();
};

} // namespace cccaster::domain::session
