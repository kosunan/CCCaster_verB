// ============================================================================
// GameMonitor.cpp — ゲーム状態の監視・判定ファサード（実装）
//
// 【処理概要】
//   MbaaAddresses.hpp で定義された MBAA 固有のメモリアドレスから
//   ゲームモードとイントロ状態を読み取り、GamePhase enum に変換する。
//
// 【Domain 層】
//   このファイルは MbaaAddresses.hpp に完全依存しており、
//   他のゲームでは使用できない。将来 src/domain/ に移動予定。
//
// 設計書: docs/design/memory_and_hook_design.md
// ============================================================================

#include "core_dll/mbaa_mem/GamePhaseDetector.hpp"
#include "core_dll/mbaa_mem/MbaaAddresses.hpp"
#include "core_dll/mbaa_mem/IGameMemory.hpp"

namespace cccaster::game_interface {

// ================================================================
// GetRawGameMode — ゲームモードID の読み取り
// ================================================================
uint32_t PhaseMonitor::GetRawGameMode() {
    return GameMem().GameMode();
}

// ================================================================
// GetCurrentPhase — 生モードID → GamePhase 変換
// ================================================================
// 30以上ある生モードIDを6つのフェーズに集約する。
// switch-case で直書き。パフォーマンスクリティカルではない
// （毎フレーム1回、ns単位の処理時間）。
GamePhase PhaseMonitor::GetCurrentPhase() {
    uint32_t rawMode = GetRawGameMode();

    switch (rawMode) {
    case CC_GAME_MODE_TITLE:   // 2: タイトル画面
    case CC_GAME_MODE_STARTUP: // 65535: 起動直後
        return GamePhase::MainMenu;

    case CC_GAME_MODE_CHARA_SELECT: // 20: キャラセレ
        return GamePhase::CharaSelect;

    case CC_GAME_MODE_LOADING:      // 8: ローディング
    case CC_GAME_MODE_LOADING_DEMO: // 13: デモ用ローディング
        return GamePhase::Loading;

    case CC_GAME_MODE_IN_GAME: // 1: 対戦中
    case CC_GAME_MODE_REPLAY:  // 26: リプレイ再生
        return GamePhase::InGame;

    case CC_GAME_MODE_RETRY: // 5: リマッチ画面
        return GamePhase::Rematch;

    default:
        return GamePhase::Unknown;
    }
}

// ================================================================
// 画面判定ヘルパー関数群
// ================================================================
// GetCurrentPhase() の薄いラッパー。可読性のために個別関数として提供。

bool PhaseMonitor::IsInMainMenu() {
    return GetCurrentPhase() == GamePhase::MainMenu;
}

bool PhaseMonitor::IsInCharaSelect() {
    return GetCurrentPhase() == GamePhase::CharaSelect;
}

bool PhaseMonitor::IsLoading() {
    return GetCurrentPhase() == GamePhase::Loading;
}

bool PhaseMonitor::IsInGame() {
    return GetCurrentPhase() == GamePhase::InGame;
}

bool PhaseMonitor::IsInRematch() {
    return GetCurrentPhase() == GamePhase::Rematch;
}

// ================================================================
// GetIntroState — イントロ状態の読み取り
// ================================================================
// @return 0: 対戦進行中（ロールバック有効）
//         1: 準備中（キャラ出現前）
//         2: イントロ演出中（カメラワーク + キャラ名表示）
uint8_t PhaseMonitor::GetIntroState() {
    return GameMem().IntroState();
}

// ================================================================
// IsRoundActive — ラウンド進行中の複合判定
// ================================================================
// InGame + IntroState == 0 の組み合わせで判定。
// 用途: ロールバック方式の切り替え
//   - IsRoundActive() == true  → ロールバック有効
//   - IsRoundActive() == false → ディレイのみ（ロールバック無効）
bool PhaseMonitor::IsRoundActive() {
    return IsInGame() && (GetIntroState() == 0);
}

} // namespace cccaster::game_interface
