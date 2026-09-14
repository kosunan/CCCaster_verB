// ============================================================================
// NetplayOverlay.cpp — ネット対戦UI オーバーレイ描画実装
// ============================================================================
//
// 【設計思想】
//   DLLインジェクションによりゲームプロセス(MBAA)内で動作する ImGui ベースの
//   オーバーレイUI。DxHook の D3D9 Present コールバックから毎フレーム
//   Render() が呼び出され、キャラクターセレクト画面 (GameMode==20) でのみ描画。
//
// 【3層分離】
//   core/overlay/OverlayRenderer : スタイル・描画ユーティリティ基盤
//   domain/ui/NetplayOverlay     : 表示ロジック (本ファイル)
//   domain/ui/ControllerMapper   : コントローラーマッピング業務ロジック
//
// 【表示優先度】（排他的に1つだけ描画される）
//   1. ControllerMapper::Draw()   : コントローラー設定画面 (F4トグル)
//   2. DrawDelaySetting()         : Delay値変更時の一時大表示 (0.8秒間)
//   3. DrawRollbackSetting()      : Rollback値変更時の一時大表示 (0.8秒間)
//   4. DrawConstantGuide()        : 常時ステータスバー (DLY/RB/Ping/Jitter)
//
// 【処理フロー概要】
//   InputHook.cpp →(Ctrl+0-9)→ OnDelayInput()    →タイマー設定→ DrawDelaySetting()
//   InputHook.cpp →(Alt+0-9) → OnRollbackInput() →タイマー設定→ DrawRollbackSetting()
//   InputHook.cpp →(F4)      → OnMappingInput()  →フラグ切替 → ControllerMapper::Draw()
//   DxHook.cpp    →(毎フレーム)→ Render()         →GameMode判定→ 上記いずれかを描画
// ============================================================================

#include "core_dll/ui/NetplayOverlay.hpp"
#include "core_dll/ui/ControllerMapper.hpp"
#include "core_dll/ui/OverlayRenderer.hpp"
#include <imgui.h>

/// DLL側の共通ログ関数（dllmain.cpp で定義）
void HookLog(const char *msg);

using namespace cccaster::domain::ui;
using cccaster::overlay::OverlayRenderer;

// ============================================================================
// 静的メンバー変数の定義
// ============================================================================

uint64_t NetplayOverlay::delayDisplayTimeEndTime = 0;
uint64_t NetplayOverlay::rollbackDisplayTimeEndTime = 0;
bool NetplayOverlay::showMappingWindow = false;
int NetplayOverlay::currentDelay = 0;
int NetplayOverlay::currentRollback = 0;
float NetplayOverlay::pingHistory[METRICS_HISTORY_SIZE] = {0};
float NetplayOverlay::jitterHistory[METRICS_HISTORY_SIZE] = {0};
int NetplayOverlay::historyIndex = 0;
float NetplayOverlay::worstPingCache = 0.0f;
float NetplayOverlay::worstJitterCache = 0.0f;

// ============================================================================
// パブリックAPI — 入力イベントハンドラ
// ============================================================================

void NetplayOverlay::OnDelayInput(int num) {
    currentDelay = num;
    delayDisplayTimeEndTime = OverlayRenderer::GetTimeMs() + OVERLAY_DISPLAY_DURATION_MS;
}

void NetplayOverlay::OnRollbackInput(int num) {
    currentRollback = num;
    rollbackDisplayTimeEndTime = OverlayRenderer::GetTimeMs() + OVERLAY_DISPLAY_DURATION_MS;
}

void NetplayOverlay::OnMappingInput() {
    showMappingWindow = !showMappingWindow;
    if (!showMappingWindow) {
        // バインド途中のステートもリセットして安全に閉じる
        ControllerMapper::ResetBindingState();
        ControllerMapper::SaveDeviceAllocations();
    }
}

void NetplayOverlay::UpdateNetworkMetrics(float pingMs, float jitterMs) {
    pingHistory[historyIndex] = pingMs;
    jitterHistory[historyIndex] = jitterMs;
    historyIndex = (historyIndex + 1) % METRICS_HISTORY_SIZE;

    float maxPing = 0.0f;
    float maxJitter = 0.0f;
    for (int i = 0; i < METRICS_HISTORY_SIZE; ++i) {
        if (pingHistory[i] > maxPing)
            maxPing = pingHistory[i];
        if (jitterHistory[i] > maxJitter)
            maxJitter = jitterHistory[i];
    }
    worstPingCache = maxPing;
    worstJitterCache = maxJitter;
}

// ============================================================================
// メイン描画エントリポイント
// ============================================================================

/// @brief 呼び出し元がGameMode判定を行い、isCharaSelect==true の時のみ描画する。
/// @details ゲームメモリへの直接アクセスは行わない（レイヤー分離原則）。
void NetplayOverlay::Render(bool isCharaSelect) {
    if (!isCharaSelect)
        return;

    uint64_t now = OverlayRenderer::GetTimeMs();

    if (showMappingWindow) {
        ControllerMapper::Draw();
    } else if (now < delayDisplayTimeEndTime) {
        DrawDelaySetting();
    } else if (now < rollbackDisplayTimeEndTime) {
        DrawRollbackSetting();
    } else {
        DrawConstantGuide();
    }
}

// ============================================================================
// 描画サブルーチン
// ============================================================================

void NetplayOverlay::DrawConstantGuide() {
    float width = ImGui::GetIO().DisplaySize.x;
    ImGui::SetNextWindowPos(ImVec2(width * 0.5f, -2.0f), ImGuiCond_Always, ImVec2(0.5f, 0.0f));

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                             ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove;

    OverlayRenderer::PushModernStyle();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(15.0f, 4.0f));

    if (ImGui::Begin("OverlayGuide", nullptr, flags)) {
        ImVec4 baseColor = ImVec4(0.8f, 0.8f, 0.8f, 1.0f);

        ImGui::TextColored(baseColor, "DLY:");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "%d", currentDelay);
        ImGui::SameLine();
        ImGui::TextColored(baseColor, "[Ctrl]+0-9| RB:");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "%d", currentRollback);
        ImGui::SameLine();
        ImGui::TextColored(baseColor, "[Alt]+0-9| PING: %.0f  |  JTR: %.0f  |  [F4] Controller Setup",
                           worstPingCache, worstJitterCache);
    }
    ImGui::End();

    ImGui::PopStyleVar();
    OverlayRenderer::PopModernStyle();
}

void NetplayOverlay::DrawDelaySetting() {
    float width = ImGui::GetIO().DisplaySize.x;
    ImGui::SetNextWindowPos(ImVec2(width * 0.5f, 50.0f), ImGuiCond_Always, ImVec2(0.5f, 0.0f));

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove;

    OverlayRenderer::PushModernStyle();
    if (ImGui::Begin("OverlayDelay", nullptr, flags)) {
        if (ImGui::GetIO().Fonts->Fonts.Size > 1)
            ImGui::PushFont(ImGui::GetIO().Fonts->Fonts[1]);
        else
            ImGui::SetWindowFontScale(1.5f);

        ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "Input Delay: %d", currentDelay);

        if (ImGui::GetIO().Fonts->Fonts.Size > 1)
            ImGui::PopFont();
        else
            ImGui::SetWindowFontScale(1.0f);
    }
    ImGui::End();
    OverlayRenderer::PopModernStyle();
}

void NetplayOverlay::DrawRollbackSetting() {
    float width = ImGui::GetIO().DisplaySize.x;
    ImGui::SetNextWindowPos(ImVec2(width * 0.5f, 50.0f), ImGuiCond_Always, ImVec2(0.5f, 0.0f));

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove;

    OverlayRenderer::PushModernStyle();
    if (ImGui::Begin("OverlayRollback", nullptr, flags)) {
        if (ImGui::GetIO().Fonts->Fonts.Size > 1)
            ImGui::PushFont(ImGui::GetIO().Fonts->Fonts[1]);
        else
            ImGui::SetWindowFontScale(1.5f);

        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "Max Rollback: %d", currentRollback);

        if (ImGui::GetIO().Fonts->Fonts.Size > 1)
            ImGui::PopFont();
        else
            ImGui::SetWindowFontScale(1.0f);
    }
    ImGui::End();
    OverlayRenderer::PopModernStyle();
}
