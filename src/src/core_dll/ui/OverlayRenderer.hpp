// ============================================================================
// OverlayRenderer.hpp — ImGui描画基盤ユーティリティ
// ============================================================================
//
// 【設計思想】
//   DLLインジェクション環境で動作するImGuiオーバーレイの
//   汎用描画インフラストラクチャ。スタイル管理・テキスト描画ヘルパー・
//   時間ユーティリティなど、特定のUI画面に依存しない共通機能を提供。
//
// 【再利用性】
//   NetplayOverlay, ControllerMapper, 将来のスペクテーターUI等、
//   任意のオーバーレイモジュールから利用可能な汎用基盤。
//
// 【依存関係】
//   - ImGui (v1.90.4-docking)
// ============================================================================
#pragma once
#include <cstdint>
#include <imgui.h>

namespace cccaster::overlay {

/// @brief ImGuiオーバーレイ描画の汎用ユーティリティ。
/// @details 全メソッド static。スタイル管理とテキスト描画ヘルパーを提供。
class OverlayRenderer {
  public:
    /// @brief モダンテーマスタイルをImGuiスタックにプッシュ。
    /// @details WindowRounding=8, FrameRounding=4, ダークブルー背景。
    ///   PopModernStyle() と対で使用すること（Push 3 + Color 2）。
    static void PushModernStyle();

    /// @brief PushModernStyle() で積んだスタイルをポップ。
    static void PopModernStyle();

    /// @brief テーブルカラム幅に収まるようテキストを縮小描画する。
    /// @param text 表示テキスト
    /// @param color テキスト色
    /// @param rightAlign true で右寄せ（P2側用）
    static void DrawFittedText(const char *text, ImVec4 color, bool rightAlign);

    /// @brief 現在時刻をミリ秒単位で取得する。
    /// @return steady_clock epoch からの経過ミリ秒
    static uint64_t GetTimeMs();
};

} // namespace cccaster::overlay
