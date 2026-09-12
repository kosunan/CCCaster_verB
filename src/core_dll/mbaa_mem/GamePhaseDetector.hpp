#pragma once
/**
 * @file GamePhaseDetector.hpp
 * @brief MBAACC ゲームフェーズ検出 — メモリ読み取りによるゲーム状態判定
 *
 * 【責務】
 *   MBAAのゲームモードID（CC_GAME_MODE_ADDR）を読み取り、
 *   ネットワーク同期で必要なフェーズ（CharaSelect, Loading, InGame 等）を判定する。
 *
 * 【設計上の位置づけ】
 *   mbaa_game 層に属する。MBAAのゲームモードID値に依存しており、
 *   fg_netplay 層の GamePhase enum 定義を MBAA 固有の検出実装で実現する。
 *
 * 【入出力】
 *   入力: MBAAプロセスメモリ（CC_GAME_MODE_ADDR, CC_INTRO_STATE_ADDR）
 *   出力: GamePhase enum, bool判定値
 */

#include <cstdint>
#include "core_dll/mbaa_mem/GamePhase.hpp"

namespace cccaster::game_interface {

/**
     * @brief MBAAのゲーム状態を監視・判定するユーティリティクラス
     *
     * @details
     *   メモリアドレスの直接読み取りを隠蔽し、安全な判定インターフェースを提供する。
     *   全メソッドは static で、状態は持たない（メモリ読み取りのみ）。
     *
     *   【使用メモリアドレス】
     *     - CC_GAME_MODE_ADDR: ゲームモードID (uint32_t)
     *     - CC_INTRO_STATE_ADDR: ラウンドイントロ状態 (uint8_t)
     *     - CC_ROUND_TIMER_ADDR: ラウンドタイマー (uint32_t)
     */
class PhaseMonitor {
  public:
    /**
         * @brief CC_GAME_MODE_ADDR から現在の生のゲームモードIDを取得
         * @return uint32_t ゲームモードID（0〜26程度の範囲）
         */
    static uint32_t GetRawGameMode();

    /**
         * @brief 現在のゲームモードを GamePhase enum に変換して返す
         * @return GamePhase 現在のフェーズ
         * @details
         *   GetRawGameMode() → switch で振り分け。
         *   未知のモードIDは GamePhase::Unknown を返す。
         */
    static GamePhase GetCurrentPhase();

    // --- 画面判定ヘルパー関数群 ---

    /// @return true: メインメニュー画面にいる
    static bool IsInMainMenu();

    /// @return true: キャラクター選択画面にいる
    static bool IsInCharaSelect();

    /// @return true: ロード中
    static bool IsLoading();

    /// @return true: 対戦中
    static bool IsInGame();

    /// @return true: リマッチ画面にいる
    static bool IsInRematch();

    // --- ラウンド状態判定ヘルパー ---

    /**
         * @brief イントロ状態を取得
         * @return uint8_t 2=イントロ演出中 / 1=pre-game / 0=対戦進行中
         * @note 数値が大きいほど手前。直感と逆なので注意。
         */
    static uint8_t GetIntroState();

    /**
         * @brief ラウンドがアクティブ（プレイ可能）状態かを判定
         * @return true: InGame かつ introState==0（イントロ演出を抜けている）
         * @details
         *   同期のディレイ/ロールバック方式の切り替え判定に使用。
         *   イントロ演出中は false を返す。
         */
    static bool IsRoundActive();
};

} // namespace cccaster::game_interface
