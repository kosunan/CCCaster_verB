// ============================================================================
// OverlayRenderer.cpp — ImGui描画基盤ユーティリティ 実装
// ============================================================================
#include "core_dll/ui/OverlayRenderer.hpp"
#include <chrono>

using namespace cccaster::overlay;

// ============================================================================
// スタイルユーティリティ
// ============================================================================

/// @brief モダンテーマスタイルをImGuiスタックにプッシュ。
/// @details
///   - WindowRounding: 8px（角丸ウィンドウ）
///   - FrameRounding: 4px
///   - WindowPadding: 15x15px
///   - WindowBg: 深いダークブルーグレー (RGB: 0.08, 0.08, 0.12, α=0.95)
///   - Border: 青みがかったグレー (α=0.5)
void OverlayRenderer::PushModernStyle() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(15.0f, 15.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.08f, 0.08f, 0.12f, 0.95f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.3f, 0.4f, 0.6f, 0.5f));
}

/// @brief PushModernStyle() で積んだスタイルをポップ。StyleVar×3 + StyleColor×2。
void OverlayRenderer::PopModernStyle() {
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
}

// ============================================================================
// テキスト描画ヘルパー
// ============================================================================

/// @brief テーブルカラム幅に収まるようテキストを縮小描画する。
/// @details テキスト幅がカラム幅を超える場合、フォントスケールを縮小して収める。
void OverlayRenderer::DrawFittedText(const char *text, ImVec4 color, bool rightAlign) {
    float availWidth = ImGui::GetColumnWidth() - 10.0f;
    float textWidth = ImGui::CalcTextSize(text).x;
    float scale = 1.0f;
    if (textWidth > availWidth && availWidth > 10.0f) {
        scale = availWidth / textWidth;
        ImGui::SetWindowFontScale(scale);
    }
    if (rightAlign) {
        float scaledWidth = ImGui::CalcTextSize(text).x * scale;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetColumnWidth() - scaledWidth - 5.0f);
    }
    ImGui::TextColored(color, "%s", text);
    ImGui::SetWindowFontScale(1.0f);
}

// ============================================================================
// 時間ユーティリティ
// ============================================================================

/// @brief steady_clock から現在のミリ秒値を取得する。
uint64_t OverlayRenderer::GetTimeMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
